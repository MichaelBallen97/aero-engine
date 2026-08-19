// tests/editor/asset_drag_test.cpp -- task 3.1.5, Step 7: the asset drag payload's decode and the
// whole drop routing matrix (DR1-DR18). A TU of aero_editor_shell_test, which supplies main() from
// shell_test.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED (the material_edit_test.cpp / asset_view_test.cpp precedent): asset_drag.hpp depends on
// aero/core/{guid,math}.hpp, aero/editor/asset_view.hpp and aero/scene/entity.hpp, none of them gated
// -- aero::scene is a PUBLIC, UNGATED dependency of aero_editor_core. Every case here must therefore
// be PRESENT and PASSING in all three build configurations. Tier-0: no GPU, no window, no ImGui
// context, no entropy source. DR18 reads editor/src/*.cpp source TEXT through AERO_EDITOR_SRC_DIR --
// a path, not a flag, so a missing fixture is a REQUIRE failure and never a silent skip.
//
// <ostream> is included PREVENTIVELY (.claude/rules/ci-portability.md): MS STL defines
// operator<<(std::ostream&, std::string_view) inline in <string_view> against a basic_ostream only
// <iosfwd> has declared, so a CHECK that stringifies a string_view fails the Windows lane alone.
//
// Enum CHECKs use the DOUBLE-PAREN posture -- CHECK((a == b)) -- which stops doctest's expression
// decomposition entirely. No toString overload is added anywhere.
#include <aero/core/guid.hpp>
#include <aero/editor/asset_drag.hpp>
#include <aero/editor/asset_view.hpp>
#include <aero/scene/entity.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

using engine::Guid;
using engine::editor::ASSET_PAYLOAD_TYPE;
using engine::editor::AssetDragPayload;
using engine::editor::AssetKind;
using engine::editor::assetKindIsDraggable;
using engine::editor::classifyAssetDrop;
using engine::editor::decodeAssetDragPayload;
using engine::editor::DropAction;
using engine::editor::dropActionLabel;
using engine::editor::DropSurface;
using engine::editor::dropSurfaceLabel;
using engine::editor::HierarchyAssetDrop;
using engine::editor::MaterialSlotTextureDrop;
using engine::editor::ViewportAssetDrop;

namespace {

constexpr Guid SAMPLE_GUID{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};

// The five AssetKinds a payload can legally carry plus the four it cannot -- ALL SEVEN, spelled out,
// so a future inserted kind is a compile-visible edit here rather than a silently unrouted row.
constexpr std::array<AssetKind, 7> ALL_KINDS{AssetKind::Folder, AssetKind::Texture, AssetKind::Model,
                                             AssetKind::Audio,  AssetKind::Text,    AssetKind::Material,
                                             AssetKind::Unknown};
constexpr std::array<DropSurface, 4> ALL_SURFACES{DropSurface::HierarchyRow, DropSurface::HierarchyVoid,
                                                  DropSurface::Viewport, DropSurface::MaterialSlot};

// The whole file, or "" when it could not be read. Binary mode: the pins below are line- and
// token-oriented and must not depend on a text-mode CRLF translation the Windows lane would apply.
[[nodiscard]] std::string readWholeFile(const std::filesystem::path& path) {
    const std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        return {};
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Drop everything from a `//` to the end of its line -- the SAME preprocessing the boundary-guard
// scripts do, and for the identical reason: PROSE naming a real identifier ("this header names no
// ImGui type") must not read as a use. Deliberately not string-literal aware; no file this pin scans
// contains a `//` inside a literal, and a guard that tries to parse C++ is a guard that gets relaxed.
[[nodiscard]] std::string stripLineComments(std::string_view source) {
    std::string out;
    out.reserve(source.size());
    std::size_t position = 0;
    while (position < source.size()) {
        const std::size_t lineEnd = std::min(source.find('\n', position), source.size());
        const std::string_view line = source.substr(position, lineEnd - position);
        out.append(line.substr(0, std::min(line.find("//"), line.size())));
        out.push_back('\n');
        position = lineEnd + 1;
    }
    return out;
}

// True iff `line` contains `->Data` as a WHOLE token -- i.e. not `->DataSize`, which is a legitimate
// SIZE read and is on hierarchy_panel.cpp's pre-existing AERO_ENTITY peek today. A plain substring
// test (and the POSIX-ERE `\b` form, which degrades to a literal on BSD/macOS) reports that line as a
// violation, which is a pin that cries wolf on a correct tree.
[[nodiscard]] bool readsPayloadDataMember(std::string_view line) noexcept {
    constexpr std::string_view TOKEN = "->Data";
    std::size_t at = line.find(TOKEN);
    while (at != std::string_view::npos) {
        const std::size_t after = at + TOKEN.size();
        const bool bounded =
            after >= line.size() || (std::isalnum(static_cast<unsigned char>(line[after])) == 0 && line[after] != '_');
        if (bounded) {
            return true;
        }
        at = line.find(TOKEN, at + 1);
    }
    return false;
}

// The two shapes a `->Data` line is ALLOWED to have: hand the pointer straight to the decoder, or the
// one legacy memcpy (hierarchy_panel.cpp's pre-existing AERO_ENTITY peek). Anything else is a read --
// which is exactly a cast.
[[nodiscard]] bool payloadDataLineIsPermitted(std::string_view line) noexcept {
    return line.find("decodeAssetDragPayload") != std::string_view::npos ||
           line.find("std::memcpy") != std::string_view::npos;
}

}  // namespace

TEST_CASE("asset_drag: a payload round-trips through a raw byte buffer (DR1)") {
    AssetDragPayload sent{};
    sent.guid = SAMPLE_GUID;
    sent.kind = static_cast<std::uint8_t>(AssetKind::Model);

    // Exactly what SetDragDropPayload does: memcpy all 24 bytes into an alignas(1) buffer. The odd
    // offset is deliberate -- ImGui's heap buffer carries no alignment guarantee beyond char, and the
    // decode must survive it (the memcpy-never-a-cast rule; UBSan runs on the Debug lanes).
    alignas(1) std::array<unsigned char, sizeof(AssetDragPayload) + 1> raw{};
    std::memcpy(raw.data() + 1, &sent, sizeof(sent));

    const std::optional<AssetDragPayload> got =
        decodeAssetDragPayload(raw.data() + 1, static_cast<int>(sizeof(AssetDragPayload)));
    REQUIRE(got.has_value());
    CHECK(got->guid == SAMPLE_GUID);
    CHECK(got->kind == static_cast<std::uint8_t>(AssetKind::Model));
}

TEST_CASE("asset_drag: every malformed buffer decodes to nullopt (DR2-DR6)") {
    AssetDragPayload sent{};
    sent.guid = SAMPLE_GUID;
    sent.kind = static_cast<std::uint8_t>(AssetKind::Texture);
    std::array<unsigned char, sizeof(AssetDragPayload)> raw{};
    std::memcpy(raw.data(), &sent, sizeof(sent));

    SUBCASE("null data (DR2)") {
        CHECK_FALSE(decodeAssetDragPayload(nullptr, static_cast<int>(sizeof(AssetDragPayload))).has_value());
    }
    SUBCASE("one byte SHORT (DR3)") {
        CHECK_FALSE(decodeAssetDragPayload(raw.data(), static_cast<int>(sizeof(AssetDragPayload)) - 1).has_value());
    }
    SUBCASE("one byte LONG (DR4)") {
        CHECK_FALSE(decodeAssetDragPayload(raw.data(), static_cast<int>(sizeof(AssetDragPayload)) + 1).has_value());
    }
    SUBCASE("size 0, and a negative size (DR5)") {
        CHECK_FALSE(decodeAssetDragPayload(raw.data(), 0).has_value());
        CHECK_FALSE(decodeAssetDragPayload(raw.data(), -1).has_value());
    }
    SUBCASE("an all-zero buffer is a NIL guid, which is a corrupt payload (DR6)") {
        const std::array<unsigned char, sizeof(AssetDragPayload)> zeros{};
        CHECK_FALSE(decodeAssetDragPayload(zeros.data(), static_cast<int>(sizeof(AssetDragPayload))).has_value());
    }
    SUBCASE("a nil guid with a NON-zero kind byte is still refused (DR6b)") {
        AssetDragPayload nil{};
        nil.kind = static_cast<std::uint8_t>(AssetKind::Model);
        std::array<unsigned char, sizeof(AssetDragPayload)> nilRaw{};
        std::memcpy(nilRaw.data(), &nil, sizeof(nil));
        CHECK_FALSE(decodeAssetDragPayload(nilRaw.data(), static_cast<int>(sizeof(AssetDragPayload))).has_value());
    }
}

// THE 28-ROW TABLE. Written as data, not as 28 hand-written checks: a missing row is visible as a
// SHORTER table, and S28 (Texture reaching the Viewport) reddens exactly one row of it.
TEST_CASE("asset_drag: classifyAssetDrop's whole 28-row matrix (DR7)") {
    struct Row {
        AssetKind kind;
        DropSurface surface;
        bool hasMeshRenderer;
        DropAction expected;
    };
    constexpr std::array<Row, 28> TABLE{{
        // Model -- instantiates on all three scene surfaces, refused on a material slot.
        {AssetKind::Model, DropSurface::HierarchyRow, false, DropAction::InstantiateModel},
        {AssetKind::Model, DropSurface::HierarchyVoid, false, DropAction::InstantiateModel},
        {AssetKind::Model, DropSurface::Viewport, false, DropAction::InstantiateModel},
        {AssetKind::Model, DropSurface::MaterialSlot, false, DropAction::None},
        // Material -- assigns ONLY where a MeshRenderer exists to assign onto.
        {AssetKind::Material, DropSurface::HierarchyRow, true, DropAction::AssignMaterial},
        {AssetKind::Material, DropSurface::HierarchyRow, false, DropAction::None},
        {AssetKind::Material, DropSurface::HierarchyVoid, false, DropAction::None},
        {AssetKind::Material, DropSurface::Viewport, true, DropAction::AssignMaterial},
        {AssetKind::Material, DropSurface::Viewport, false, DropAction::None},
        {AssetKind::Material, DropSurface::MaterialSlot, false, DropAction::None},
        // Texture -- the material slot and NOWHERE else (S28's row is the Viewport one).
        {AssetKind::Texture, DropSurface::HierarchyRow, false, DropAction::None},
        {AssetKind::Texture, DropSurface::HierarchyVoid, false, DropAction::None},
        {AssetKind::Texture, DropSurface::Viewport, false, DropAction::None},
        {AssetKind::Texture, DropSurface::MaterialSlot, false, DropAction::BindTextureSlot},
        // The four non-draggable kinds, every surface: None, sixteen times.
        {AssetKind::Folder, DropSurface::HierarchyRow, false, DropAction::None},
        {AssetKind::Folder, DropSurface::HierarchyVoid, false, DropAction::None},
        {AssetKind::Folder, DropSurface::Viewport, false, DropAction::None},
        {AssetKind::Folder, DropSurface::MaterialSlot, false, DropAction::None},
        {AssetKind::Audio, DropSurface::HierarchyRow, false, DropAction::None},
        {AssetKind::Audio, DropSurface::HierarchyVoid, false, DropAction::None},
        {AssetKind::Audio, DropSurface::Viewport, false, DropAction::None},
        {AssetKind::Audio, DropSurface::MaterialSlot, false, DropAction::None},
        {AssetKind::Text, DropSurface::HierarchyRow, false, DropAction::None},
        {AssetKind::Text, DropSurface::HierarchyVoid, false, DropAction::None},
        {AssetKind::Text, DropSurface::Viewport, false, DropAction::None},
        {AssetKind::Text, DropSurface::MaterialSlot, false, DropAction::None},
        {AssetKind::Unknown, DropSurface::HierarchyRow, false, DropAction::None},
        {AssetKind::Unknown, DropSurface::MaterialSlot, false, DropAction::None},
    }};

    // The table's own shape is asserted first: a row silently deleted during an edit would otherwise
    // make every later assertion pass over a shorter table (the project_settings shape lesson).
    REQUIRE(TABLE.size() == 28);
    std::size_t modelRows = 0;
    std::size_t assignRows = 0;
    std::size_t bindRows = 0;
    for (const Row& row : TABLE) {
        CAPTURE(dropSurfaceLabel(row.surface));
        CAPTURE(dropActionLabel(row.expected));
        const DropAction got = classifyAssetDrop(row.kind, row.surface, row.hasMeshRenderer);
        CHECK((got == row.expected));
        modelRows += (row.expected == DropAction::InstantiateModel) ? 1U : 0U;
        assignRows += (row.expected == DropAction::AssignMaterial) ? 1U : 0U;
        bindRows += (row.expected == DropAction::BindTextureSlot) ? 1U : 0U;
    }
    CHECK(modelRows == 3);
    CHECK(assignRows == 2);
    CHECK(bindRows == 1);
}

TEST_CASE("asset_drag: classifyAssetDrop is TOTAL over kind x surface x flag (DR8)") {
    // Every one of the 56 combinations answers something; nothing is left to a default: arm, and the
    // function never depends on an out-of-range value.
    std::size_t seen = 0;
    for (const AssetKind kind : ALL_KINDS) {
        for (const DropSurface surface : ALL_SURFACES) {
            for (const bool flag : {false, true}) {
                const DropAction action = classifyAssetDrop(kind, surface, flag);
                // A refused kind is None whatever the surface and whatever the flag.
                if (!assetKindIsDraggable(kind)) {
                    CHECK((action == DropAction::None));
                }
                ++seen;
            }
        }
    }
    CHECK(seen == 56);
}

TEST_CASE("asset_drag: targetHasMeshRenderer is consulted ONLY on the two Material rows (DR9)") {
    for (const AssetKind kind : ALL_KINDS) {
        for (const DropSurface surface : ALL_SURFACES) {
            const DropAction off = classifyAssetDrop(kind, surface, false);
            const DropAction on = classifyAssetDrop(kind, surface, true);
            const bool sensitive = kind == AssetKind::Material &&
                                   (surface == DropSurface::HierarchyRow || surface == DropSurface::Viewport);
            if (sensitive) {
                CHECK((off == DropAction::None));
                CHECK((on == DropAction::AssignMaterial));
            } else {
                CHECK((off == on));
            }
        }
    }
}

TEST_CASE("asset_drag: assetKindIsDraggable answers all seven kinds individually (DR10)") {
    // Asserted one by one, never through a helper, so a future INSERTED kind reddens here rather than
    // silently defaulting to "not draggable".
    CHECK_FALSE(assetKindIsDraggable(AssetKind::Folder));
    CHECK(assetKindIsDraggable(AssetKind::Texture));
    CHECK(assetKindIsDraggable(AssetKind::Model));
    CHECK_FALSE(assetKindIsDraggable(AssetKind::Audio));
    CHECK_FALSE(assetKindIsDraggable(AssetKind::Text));
    CHECK(assetKindIsDraggable(AssetKind::Material));
    CHECK_FALSE(assetKindIsDraggable(AssetKind::Unknown));

    std::size_t draggable = 0;
    for (const AssetKind kind : engine::editor::ASSET_KIND_FILTER_OPTIONS) {
        draggable += assetKindIsDraggable(kind) ? 1U : 0U;
    }
    CHECK(draggable == 3);
}

TEST_CASE("asset_drag: dropSurfaceLabel and dropActionLabel are total and INJECTIVE (DR11)") {
    // Against LITERALS, never against each other: a label swapped between two enumerators is exactly
    // what a mapping-vs-mapping comparison cannot see.
    CHECK(dropSurfaceLabel(DropSurface::HierarchyRow) == std::string_view("hierarchy row"));
    CHECK(dropSurfaceLabel(DropSurface::HierarchyVoid) == std::string_view("hierarchy void"));
    CHECK(dropSurfaceLabel(DropSurface::Viewport) == std::string_view("viewport"));
    CHECK(dropSurfaceLabel(DropSurface::MaterialSlot) == std::string_view("material slot"));

    CHECK(dropActionLabel(DropAction::None) == std::string_view("none"));
    CHECK(dropActionLabel(DropAction::InstantiateModel) == std::string_view("instantiate model"));
    CHECK(dropActionLabel(DropAction::AssignMaterial) == std::string_view("assign material"));
    CHECK(dropActionLabel(DropAction::BindTextureSlot) == std::string_view("bind texture slot"));

    std::vector<std::string_view> surfaces;
    for (const DropSurface surface : ALL_SURFACES) {
        surfaces.push_back(dropSurfaceLabel(surface));
        CHECK_FALSE(surfaces.back().empty());
    }
    for (std::size_t i = 0; i < surfaces.size(); ++i) {
        for (std::size_t j = i + 1; j < surfaces.size(); ++j) {
            CHECK(surfaces[i] != surfaces[j]);
        }
    }
}

TEST_CASE("asset_drag: the payload type string is ImGui-legal and cannot cross-fire (DR12)") {
    const std::string_view type{ASSET_PAYLOAD_TYPE};
    CHECK(type == std::string_view("AERO_ASSET"));
    CHECK(type.size() <= 32);  // ImGui's own IM_ASSERT on SetDragDropPayload's type string
    // The Hierarchy's pre-existing reparent payload. Two DIFFERENT strings is what makes IsDataType
    // refuse each other's payloads, so the two features structurally cannot cross-fire.
    CHECK(type != std::string_view("AERO_ENTITY"));
}

TEST_CASE("asset_drag: the payload's layout is exactly what the decode assumes (DR13)") {
    CHECK(sizeof(AssetDragPayload) == 24);
    CHECK(alignof(AssetDragPayload) == 8);
    CHECK(std::is_trivially_copyable_v<AssetDragPayload>);
    // 24 > 16, so ImGui uses its HEAP buffer rather than the inline one -- which is why the decode
    // must memcpy and may never cast.
    CHECK(sizeof(AssetDragPayload) > 16);
}

TEST_CASE("asset_drag: the seven tail padding bytes cannot change what a payload MEANS (DR14)") {
    // THIS CASE CORRECTS THE PLAN. §0.20 asserts that value-initialising the payload also zeroes its
    // seven tail padding bytes, so SetDragDropPayload's 24-byte memcpy is deterministic. Measured on
    // Apple clang 21 at -O0, it is NOT: AssetDragPayload is not trivially default constructible
    // (engine::Guid's own `hi = 0`/`lo = 0` NSDMIs decide that, not `kind = 0`), so the compiler runs
    // the constructor instead of a whole-object zero-init and the padding survives. See asset_drag.hpp.
    //
    // What IS true, is load-bearing, and is what this case pins: the decode reads ONLY `guid` and
    // `kind`, so two payloads agreeing on those two members decode identically WHATEVER their padding
    // holds. That is why indeterminate padding is harmless here rather than merely undiagnosed.
    AssetDragPayload sent{};
    sent.guid = SAMPLE_GUID;
    sent.kind = static_cast<std::uint8_t>(AssetKind::Material);

    std::array<unsigned char, sizeof(AssetDragPayload)> zeroPadding{};
    std::memcpy(zeroPadding.data(), &sent, sizeof(sent));
    std::array<unsigned char, sizeof(AssetDragPayload)> junkPadding = zeroPadding;
    for (std::size_t i = 17; i < sizeof(AssetDragPayload); ++i) {
        zeroPadding[i] = 0U;
        junkPadding[i] = 0xABU;
    }
    CHECK(zeroPadding != junkPadding);  // the two buffers really do differ, byte for byte

    const std::optional<AssetDragPayload> fromZero =
        decodeAssetDragPayload(zeroPadding.data(), static_cast<int>(sizeof(AssetDragPayload)));
    const std::optional<AssetDragPayload> fromJunk =
        decodeAssetDragPayload(junkPadding.data(), static_cast<int>(sizeof(AssetDragPayload)));
    REQUIRE(fromZero.has_value());
    REQUIRE(fromJunk.has_value());
    CHECK(fromZero->guid == fromJunk->guid);
    CHECK(fromZero->kind == fromJunk->kind);
    CHECK(fromZero->guid == SAMPLE_GUID);
    CHECK(fromZero->kind == static_cast<std::uint8_t>(AssetKind::Material));

    // And value-init still buys what it was always really for: every MEMBER is deterministic, so a
    // future appended field cannot arrive uninitialised at a call site that forgot to set it.
    const AssetDragPayload fresh{};
    CHECK_FALSE(fresh.guid.valid());
    CHECK(fresh.kind == 0);
}

TEST_CASE("asset_drag: every AssetKind survives the payload's kind byte (DR15)") {
    for (const AssetKind kind : ALL_KINDS) {
        AssetDragPayload sent{};
        sent.guid = SAMPLE_GUID;
        sent.kind = static_cast<std::uint8_t>(kind);
        std::array<unsigned char, sizeof(AssetDragPayload)> raw{};
        std::memcpy(raw.data(), &sent, sizeof(sent));
        const std::optional<AssetDragPayload> got =
            decodeAssetDragPayload(raw.data(), static_cast<int>(sizeof(AssetDragPayload)));
        REQUIRE(got.has_value());
        CHECK(got->kind == static_cast<std::uint8_t>(kind));
        CHECK((static_cast<AssetKind>(got->kind) == kind));
    }
}

TEST_CASE("asset_drag: the three drop-request structs carry exactly their gesture's data (DR16)") {
    const HierarchyAssetDrop voidDrop{};
    CHECK_FALSE(voidDrop.targetRow.valid());  // Entity{} IS the void target -- no second flag
    CHECK_FALSE(voidDrop.payload.guid.valid());
    CHECK(voidDrop.payload.kind == 0);

    HierarchyAssetDrop rowDrop{};
    rowDrop.payload.guid = SAMPLE_GUID;
    rowDrop.payload.kind = static_cast<std::uint8_t>(AssetKind::Model);
    rowDrop.targetRow = engine::Entity{7, 1};
    CHECK(rowDrop.targetRow.valid());
    CHECK(rowDrop.payload.guid == SAMPLE_GUID);

    ViewportAssetDrop viewportDrop{};
    CHECK(viewportDrop.ndc.x == doctest::Approx(0.0F));
    CHECK(viewportDrop.ndc.y == doctest::Approx(0.0F));
    viewportDrop.ndc = engine::Vec2{-1.0F, 1.0F};  // the image's top-left corner, NDC y UP
    CHECK(viewportDrop.ndc.x == doctest::Approx(-1.0F));
    CHECK(viewportDrop.ndc.y == doctest::Approx(1.0F));

    const MaterialSlotTextureDrop slotDrop{};
    CHECK(slotDrop.slot == 0);
    CHECK_FALSE(slotDrop.textureGuid.valid());
}

TEST_CASE("asset_drag: this header names no ImGui type and this TU calls no ImGui function (DR17)") {
    // A PLACEMENT rule, held by a source-text pin because no probe can enforce it (R12: doctest puts
    // vcpkg's shared include root on the compile line, so a leaked <imgui.h> would still compile).
    const std::filesystem::path src{AERO_EDITOR_SRC_DIR};
    const std::filesystem::path header = src.parent_path() / "include" / "aero" / "editor" / "asset_drag.hpp";
    const std::string headerRaw = readWholeFile(header);
    REQUIRE_FALSE(headerRaw.empty());
    const std::string headerBody = stripLineComments(headerRaw);
    CHECK(headerBody.find("imgui") == std::string::npos);
    CHECK(headerBody.find("ImGui") == std::string::npos);

    const std::string cppRaw = readWholeFile(src / "asset_drag.cpp");
    REQUIRE_FALSE(cppRaw.empty());
    const std::string cppBody = stripLineComments(cppRaw);
    CHECK(cppBody.find("imgui") == std::string::npos);
    CHECK(cppBody.find("ImGui") == std::string::npos);
    // and the decode is a memcpy, never a cast
    CHECK(cppBody.find("std::memcpy(&out, data, sizeof(out));") != std::string::npos);
}

// S29's PIN. No runtime tier can see a cast of ImGui's alignas(1) payload buffer, so this is a
// source-text check, and the pin is written to encode what it CLAIMS (the I96 lesson): it permits a
// line that hands ->Data straight to decodeAssetDragPayload or to the one legacy memcpy, and it
// rejects everything else -- which is exactly a cast.
TEST_CASE("asset_drag: decodeAssetDragPayload is the ONLY reader of a payload's ->Data (DR18)") {
    // THE PIN'S OWN SELF-TEST FIRST (the I96 lesson: a source-text pin can certify the very invariant
    // it is blind to). It must ACCEPT the cast shape S29 seeds and REJECT the `->DataSize` size read
    // that has been legal on hierarchy_panel.cpp since 2.2.1.
    constexpr std::string_view CAST_LINE = "    const auto p = *static_cast<const AssetDragPayload*>(payload->Data);";
    constexpr std::string_view LEGAL_DECODE =
        "    const auto p = decodeAssetDragPayload(payload->Data, payload->DataSize);";
    constexpr std::string_view LEGAL_MEMCPY = "    std::memcpy(&out, payload->Data, sizeof(Entity));";
    constexpr std::string_view SIZE_ONLY = "    payload->DataSize != static_cast<int>(sizeof(Entity))) {";

    CHECK(readsPayloadDataMember(CAST_LINE));
    CHECK(readsPayloadDataMember(LEGAL_DECODE));
    CHECK(readsPayloadDataMember(LEGAL_MEMCPY));
    CHECK_FALSE(readsPayloadDataMember(SIZE_ONLY));  // ->DataSize is a SIZE read, never a Data read
    CHECK_FALSE(readsPayloadDataMember("nothing to see here"));

    // ... and the exclusion half, which is what decides GREEN vs RED once a line does match.
    CHECK_FALSE(payloadDataLineIsPermitted(CAST_LINE));  // the seed S29 plants -> RED
    CHECK(payloadDataLineIsPermitted(LEGAL_DECODE));     // a legal SECOND decode call -> still green
    CHECK(payloadDataLineIsPermitted(LEGAL_MEMCPY));

    const std::filesystem::path src{AERO_EDITOR_SRC_DIR};
    std::error_code ec;
    REQUIRE(std::filesystem::is_directory(src, ec));

    std::vector<std::string> offenders;
    std::size_t scannedFiles = 0;
    std::size_t scannedLines = 0;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(src, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        REQUIRE(in.good());
        ++scannedFiles;
        std::string line;
        while (std::getline(in, line)) {
            ++scannedLines;
            if (!readsPayloadDataMember(line)) {
                continue;
            }
            if (payloadDataLineIsPermitted(line)) {
                continue;  // the permitted shapes: hand it to the decoder, or the legacy memcpy
            }
            offenders.push_back(entry.path().filename().string() + ": " + line);
        }
    }
    // Anti-vacuity: the scan must have actually traversed the editor's sources.
    CHECK(scannedFiles > 40);
    CHECK(scannedLines > 1000);
    for (const std::string& offender : offenders) {
        CAPTURE(offender);
        CHECK(false);
    }
    CHECK(offenders.empty());
}

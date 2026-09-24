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
#include <aero/editor/asset_picker_model.hpp>  // task E.3.3 -- AR7 states the picker's accept rule here
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
constexpr std::array<DropSurface, 5> ALL_SURFACES{DropSurface::HierarchyRow, DropSurface::HierarchyVoid,
                                                  DropSurface::Viewport, DropSurface::MaterialSlot,
                                                  DropSurface::AssetField};
// task E.3.3: every DropAction, in enum order -- DR11's second injectivity loop, which had only a
// surface loop before the fifth action existed.
constexpr std::array<DropAction, 5> ALL_ACTIONS{DropAction::None, DropAction::InstantiateModel,
                                                DropAction::AssignMaterial, DropAction::BindTextureSlot,
                                                DropAction::AssignAssetReference};

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
    // task E.4.3 widens this ALLOWLIST by exactly one name. The tree now has a SECOND sanctioned
    // decoder -- decodeAssetMoveDragPayload, for the move payload -- and "decodeAssetDragPayload" is
    // NOT a substring of it, so the browser's legitimate call was reported as an offender. Widening
    // the allowlist is the correct response and a widened DENYLIST would not be: the claim is still
    // "a payload's ->Data is handed to a decoder and never cast", and it is still enumerated.
    return line.find("decodeAssetDragPayload") != std::string_view::npos ||
           line.find("decodeAssetMoveDragPayload") != std::string_view::npos ||
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

// THE 35-ROW TABLE. Written as data, not as 35 hand-written checks: a missing row is visible as a
// SHORTER table, and S28 (Texture reaching the Viewport) reddens exactly one row of it.
//
// task E.3.3 added the fieldKind column and SEVEN AssetField rows, one per kind, all with nullopt --
// the unannotated field, which is the row every existing surface's completeness claim is about. The
// 32-cell per-fieldKind cross product lives in AR1 and the Text-on-a-Text-field row in AR2, because
// putting them here would take this table past sixty rows and make the EXISTING matrix harder to
// read, and this table's job is the four old surfaces' completeness -- which a nullopt column
// preserves exactly, since the 28 old rows' expectations are byte-identical.
TEST_CASE("asset_drag: classifyAssetDrop's whole 35-row matrix (DR7)") {
    struct Row {
        AssetKind kind;
        DropSurface surface;
        bool hasMeshRenderer;
        std::optional<AssetKind> fieldKind;
        DropAction expected;
    };
    constexpr std::array<Row, 35> TABLE{{
        // Model -- instantiates on all three scene surfaces, refused on a material slot.
        {AssetKind::Model, DropSurface::HierarchyRow, false, std::nullopt, DropAction::InstantiateModel},
        {AssetKind::Model, DropSurface::HierarchyVoid, false, std::nullopt, DropAction::InstantiateModel},
        {AssetKind::Model, DropSurface::Viewport, false, std::nullopt, DropAction::InstantiateModel},
        {AssetKind::Model, DropSurface::MaterialSlot, false, std::nullopt, DropAction::None},
        // Material -- assigns ONLY where a MeshRenderer exists to assign onto.
        {AssetKind::Material, DropSurface::HierarchyRow, true, std::nullopt, DropAction::AssignMaterial},
        {AssetKind::Material, DropSurface::HierarchyRow, false, std::nullopt, DropAction::None},
        {AssetKind::Material, DropSurface::HierarchyVoid, false, std::nullopt, DropAction::None},
        {AssetKind::Material, DropSurface::Viewport, true, std::nullopt, DropAction::AssignMaterial},
        {AssetKind::Material, DropSurface::Viewport, false, std::nullopt, DropAction::None},
        {AssetKind::Material, DropSurface::MaterialSlot, false, std::nullopt, DropAction::None},
        // Texture -- the material slot and NOWHERE else (S28's row is the Viewport one).
        {AssetKind::Texture, DropSurface::HierarchyRow, false, std::nullopt, DropAction::None},
        {AssetKind::Texture, DropSurface::HierarchyVoid, false, std::nullopt, DropAction::None},
        {AssetKind::Texture, DropSurface::Viewport, false, std::nullopt, DropAction::None},
        {AssetKind::Texture, DropSurface::MaterialSlot, false, std::nullopt, DropAction::BindTextureSlot},
        // The four non-draggable kinds, every surface: None, sixteen times.
        {AssetKind::Folder, DropSurface::HierarchyRow, false, std::nullopt, DropAction::None},
        {AssetKind::Folder, DropSurface::HierarchyVoid, false, std::nullopt, DropAction::None},
        {AssetKind::Folder, DropSurface::Viewport, false, std::nullopt, DropAction::None},
        {AssetKind::Folder, DropSurface::MaterialSlot, false, std::nullopt, DropAction::None},
        {AssetKind::Audio, DropSurface::HierarchyRow, false, std::nullopt, DropAction::None},
        {AssetKind::Audio, DropSurface::HierarchyVoid, false, std::nullopt, DropAction::None},
        {AssetKind::Audio, DropSurface::Viewport, false, std::nullopt, DropAction::None},
        {AssetKind::Audio, DropSurface::MaterialSlot, false, std::nullopt, DropAction::None},
        {AssetKind::Text, DropSurface::HierarchyRow, false, std::nullopt, DropAction::None},
        {AssetKind::Text, DropSurface::HierarchyVoid, false, std::nullopt, DropAction::None},
        {AssetKind::Text, DropSurface::Viewport, false, std::nullopt, DropAction::None},
        {AssetKind::Text, DropSurface::MaterialSlot, false, std::nullopt, DropAction::None},
        {AssetKind::Unknown, DropSurface::HierarchyRow, false, std::nullopt, DropAction::None},
        {AssetKind::Unknown, DropSurface::MaterialSlot, false, std::nullopt, DropAction::None},
        // task E.3.3: the fifth surface, one row per kind, UNCONSTRAINED (nullopt). The four
        // draggable kinds assign; the three that cannot start a drag are refused, which is what makes
        // DR8's "a refused kind is None whatever the surface" true here too.
        {AssetKind::Model, DropSurface::AssetField, false, std::nullopt, DropAction::AssignAssetReference},
        {AssetKind::Material, DropSurface::AssetField, false, std::nullopt, DropAction::AssignAssetReference},
        {AssetKind::Texture, DropSurface::AssetField, false, std::nullopt, DropAction::AssignAssetReference},
        {AssetKind::Audio, DropSurface::AssetField, false, std::nullopt, DropAction::AssignAssetReference},
        {AssetKind::Folder, DropSurface::AssetField, false, std::nullopt, DropAction::None},
        {AssetKind::Text, DropSurface::AssetField, false, std::nullopt, DropAction::None},
        {AssetKind::Unknown, DropSurface::AssetField, false, std::nullopt, DropAction::None},
    }};

    // The table's own shape is asserted first: a row silently deleted during an edit would otherwise
    // make every later assertion pass over a shorter table (the project_settings shape lesson).
    REQUIRE(TABLE.size() == 35);
    std::size_t modelRows = 0;
    std::size_t assignRows = 0;
    std::size_t bindRows = 0;
    std::size_t referenceRows = 0;
    for (const Row& row : TABLE) {
        CAPTURE(dropSurfaceLabel(row.surface));
        CAPTURE(dropActionLabel(row.expected));
        const DropAction got = classifyAssetDrop(row.kind, row.surface, row.hasMeshRenderer, row.fieldKind);
        CHECK((got == row.expected));
        modelRows += (row.expected == DropAction::InstantiateModel) ? 1U : 0U;
        assignRows += (row.expected == DropAction::AssignMaterial) ? 1U : 0U;
        bindRows += (row.expected == DropAction::BindTextureSlot) ? 1U : 0U;
        referenceRows += (row.expected == DropAction::AssignAssetReference) ? 1U : 0U;
    }
    // The three OLD tallies are UNMOVED, which is the claim that the fifth surface changed nothing
    // about the four that came before it.
    CHECK(modelRows == 3);
    CHECK(assignRows == 2);
    CHECK(bindRows == 1);
    CHECK(referenceRows == 4);
}

TEST_CASE("asset_drag: classifyAssetDrop is TOTAL over kind x surface x flag (DR8)") {
    // Every one of the 70 combinations answers something; nothing is left to a default: arm, and the
    // function never depends on an out-of-range value.
    std::size_t seen = 0;
    for (const AssetKind kind : ALL_KINDS) {
        for (const DropSurface surface : ALL_SURFACES) {
            for (const bool flag : {false, true}) {
                const DropAction action = classifyAssetDrop(kind, surface, flag, std::nullopt);
                // A refused kind is None whatever the surface and whatever the flag.
                if (!assetKindIsDraggable(kind)) {
                    CHECK((action == DropAction::None));
                }
                ++seen;
            }
        }
    }
    CHECK(seen == 70);  // 7 kinds x 5 surfaces (task E.3.3 appended AssetField) x 2 flags
}

TEST_CASE("asset_drag: targetHasMeshRenderer is consulted ONLY on the two Material rows (DR9)") {
    for (const AssetKind kind : ALL_KINDS) {
        for (const DropSurface surface : ALL_SURFACES) {
            const DropAction off = classifyAssetDrop(kind, surface, false, std::nullopt);
            const DropAction on = classifyAssetDrop(kind, surface, true, std::nullopt);
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
    CHECK(assetKindIsDraggable(AssetKind::Audio));  // task E.3.3: AudioSource::clip is a drop target
    CHECK_FALSE(assetKindIsDraggable(AssetKind::Text));
    CHECK(assetKindIsDraggable(AssetKind::Material));
    CHECK_FALSE(assetKindIsDraggable(AssetKind::Unknown));

    std::size_t draggable = 0;
    for (const AssetKind kind : engine::editor::ASSET_KIND_FILTER_OPTIONS) {
        draggable += assetKindIsDraggable(kind) ? 1U : 0U;
    }
    CHECK(draggable == 4);  // task E.3.3: Texture, Model, Material, Audio
}

TEST_CASE("asset_drag: dropSurfaceLabel and dropActionLabel are total and INJECTIVE (DR11)") {
    // Against LITERALS, never against each other: a label swapped between two enumerators is exactly
    // what a mapping-vs-mapping comparison cannot see.
    CHECK(dropSurfaceLabel(DropSurface::HierarchyRow) == std::string_view("hierarchy row"));
    CHECK(dropSurfaceLabel(DropSurface::HierarchyVoid) == std::string_view("hierarchy void"));
    CHECK(dropSurfaceLabel(DropSurface::Viewport) == std::string_view("viewport"));
    CHECK(dropSurfaceLabel(DropSurface::MaterialSlot) == std::string_view("material slot"));
    CHECK(dropSurfaceLabel(DropSurface::AssetField) == std::string_view("asset field"));

    CHECK(dropActionLabel(DropAction::None) == std::string_view("none"));
    CHECK(dropActionLabel(DropAction::InstantiateModel) == std::string_view("instantiate model"));
    CHECK(dropActionLabel(DropAction::AssignMaterial) == std::string_view("assign material"));
    CHECK(dropActionLabel(DropAction::BindTextureSlot) == std::string_view("bind texture slot"));
    CHECK(dropActionLabel(DropAction::AssignAssetReference) == std::string_view("assign asset reference"));

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

    // task E.3.3: the SAME claim for the actions, which had no injectivity loop before the fifth one
    // existed -- a label shared between two actions is what a literal-by-literal list cannot see.
    std::vector<std::string_view> actions;
    for (const DropAction action : ALL_ACTIONS) {
        actions.push_back(dropActionLabel(action));
        CHECK_FALSE(actions.back().empty());
    }
    for (std::size_t i = 0; i < actions.size(); ++i) {
        for (std::size_t j = i + 1; j < actions.size(); ++j) {
            CHECK(actions[i] != actions[j]);
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
    // task E.4.3: the move decoder is permitted, and a CAST of the move payload is still refused --
    // the allowlist grew by a name, not by a shape.
    CHECK(payloadDataLineIsPermitted(
        "        source = decodeAssetMoveDragPayload(peek->Data, peek->DataSize).value_or({});"));
    CHECK_FALSE(
        payloadDataLineIsPermitted("        const auto* p = static_cast<const AssetMoveDragPayload*>(payload->Data);"));

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

// ---- task E.3.3: the AssetField surface and the token vocabulary (AR1-AR6) -----------------------
//
// These live beside DR7 rather than inside it: the per-fieldKind cross product is 32 cells, and
// folding it into that table would take it past sixty rows and make the four OLD surfaces' matrix
// harder to read. DR7 keeps the completeness claim; these keep the constraint claim.

TEST_CASE("AR1: an AssetField accepts a draggable kind iff the field names it, or names nothing") {
    struct Cell {
        AssetKind payload;
        std::optional<AssetKind> fieldKind;
        DropAction expected;
    };
    // The four DRAGGABLE payload kinds x {nullopt} u ALL_KINDS = 4 x 8 = 32 cells, spelled out
    // against literals rather than computed from the predicate under test.
    constexpr std::array<Cell, 32> TABLE{{
        {AssetKind::Texture, std::nullopt, DropAction::AssignAssetReference},
        {AssetKind::Texture, AssetKind::Folder, DropAction::None},
        {AssetKind::Texture, AssetKind::Texture, DropAction::AssignAssetReference},
        {AssetKind::Texture, AssetKind::Model, DropAction::None},
        {AssetKind::Texture, AssetKind::Audio, DropAction::None},
        {AssetKind::Texture, AssetKind::Text, DropAction::None},
        {AssetKind::Texture, AssetKind::Material, DropAction::None},
        {AssetKind::Texture, AssetKind::Unknown, DropAction::None},

        {AssetKind::Model, std::nullopt, DropAction::AssignAssetReference},
        {AssetKind::Model, AssetKind::Folder, DropAction::None},
        {AssetKind::Model, AssetKind::Texture, DropAction::None},
        {AssetKind::Model, AssetKind::Model, DropAction::AssignAssetReference},
        {AssetKind::Model, AssetKind::Audio, DropAction::None},
        {AssetKind::Model, AssetKind::Text, DropAction::None},
        {AssetKind::Model, AssetKind::Material, DropAction::None},
        {AssetKind::Model, AssetKind::Unknown, DropAction::None},

        {AssetKind::Audio, std::nullopt, DropAction::AssignAssetReference},
        {AssetKind::Audio, AssetKind::Folder, DropAction::None},
        {AssetKind::Audio, AssetKind::Texture, DropAction::None},
        {AssetKind::Audio, AssetKind::Model, DropAction::None},
        {AssetKind::Audio, AssetKind::Audio, DropAction::AssignAssetReference},
        {AssetKind::Audio, AssetKind::Text, DropAction::None},
        {AssetKind::Audio, AssetKind::Material, DropAction::None},
        {AssetKind::Audio, AssetKind::Unknown, DropAction::None},

        {AssetKind::Material, std::nullopt, DropAction::AssignAssetReference},
        {AssetKind::Material, AssetKind::Folder, DropAction::None},
        {AssetKind::Material, AssetKind::Texture, DropAction::None},
        {AssetKind::Material, AssetKind::Model, DropAction::None},
        {AssetKind::Material, AssetKind::Audio, DropAction::None},
        {AssetKind::Material, AssetKind::Text, DropAction::None},
        {AssetKind::Material, AssetKind::Material, DropAction::AssignAssetReference},
        {AssetKind::Material, AssetKind::Unknown, DropAction::None},
    }};

    REQUIRE(TABLE.size() == 32);
    std::size_t accepted = 0;
    for (const Cell& cell : TABLE) {
        CAPTURE(static_cast<int>(cell.payload));
        CAPTURE(dropActionLabel(cell.expected));
        const DropAction got =
            classifyAssetDrop(cell.payload, DropSurface::AssetField, /*targetHasMeshRenderer=*/false, cell.fieldKind);
        CHECK((got == cell.expected));
        accepted += (got == DropAction::AssignAssetReference) ? 1U : 0U;
    }
    // Eight acceptances: four unconstrained fields plus four matching ones. A row that silently
    // stopped constraining would push this to 28.
    CHECK(accepted == 8);
}

TEST_CASE("AR2: a NON-draggable payload is refused on an asset field, even by a field naming it") {
    for (const AssetKind payload : {AssetKind::Folder, AssetKind::Text, AssetKind::Unknown}) {
        CAPTURE(static_cast<int>(payload));
        CHECK((classifyAssetDrop(payload, DropSurface::AssetField, false, std::nullopt) == DropAction::None));
        for (const AssetKind fieldKind : ALL_KINDS) {
            CAPTURE(static_cast<int>(fieldKind));
            // INCLUDING the diagonal -- Text on a Text-kind field. That is the row a naive "the kinds
            // are equal, so accept" restatement gets wrong, and nothing else in this file can see it:
            // a field can never NAME one of these kinds anyway (assetReferenceKindFromToken refuses
            // every one of them), so the composition with draggability is what does the refusing.
            CHECK((classifyAssetDrop(payload, DropSurface::AssetField, false, fieldKind) == DropAction::None));
        }
    }
}

TEST_CASE("AR3: fieldKind is IGNORED on all four pre-E.3.3 surfaces") {
    constexpr std::array<DropSurface, 4> OLD_SURFACES{DropSurface::HierarchyRow, DropSurface::HierarchyVoid,
                                                      DropSurface::Viewport, DropSurface::MaterialSlot};
    for (const AssetKind kind : ALL_KINDS) {
        for (const DropSurface surface : OLD_SURFACES) {
            for (const bool flag : {false, true}) {
                CAPTURE(dropSurfaceLabel(surface));
                const DropAction none = classifyAssetDrop(kind, surface, flag, std::nullopt);
                const DropAction texture = classifyAssetDrop(kind, surface, flag, AssetKind::Texture);
                const DropAction audio = classifyAssetDrop(kind, surface, flag, AssetKind::Audio);
                CHECK((none == texture));
                CHECK((none == audio));
            }
        }
    }
}

TEST_CASE("AR4: assetReferenceKindFromToken resolves the four draggable labels, ASCII-folded") {
    using engine::editor::assetReferenceKindFromToken;
    REQUIRE(assetReferenceKindFromToken("texture").has_value());
    CHECK((*assetReferenceKindFromToken("texture") == AssetKind::Texture));
    CHECK((*assetReferenceKindFromToken("model") == AssetKind::Model));
    CHECK((*assetReferenceKindFromToken("material") == AssetKind::Material));
    CHECK((*assetReferenceKindFromToken("audio") == AssetKind::Audio));

    // The fold is ASCII and case-insensitive in both directions.
    CHECK((*assetReferenceKindFromToken("Texture") == AssetKind::Texture));
    CHECK((*assetReferenceKindFromToken("TEXTURE") == AssetKind::Texture));
    CHECK((*assetReferenceKindFromToken("MoDeL") == AssetKind::Model));

    // Everything else is nullopt, which the widget reads as UNCONSTRAINED plus one WARN -- never a
    // refused field. `shader` is the grammar-valid, vocabulary-unknown token the tool passes through.
    CHECK_FALSE(assetReferenceKindFromToken("folder").has_value());
    CHECK_FALSE(assetReferenceKindFromToken("text").has_value());
    CHECK_FALSE(assetReferenceKindFromToken("unknown").has_value());
    CHECK_FALSE(assetReferenceKindFromToken("").has_value());
    CHECK_FALSE(assetReferenceKindFromToken("shader").has_value());
    CHECK_FALSE(assetReferenceKindFromToken("audio ").has_value());  // a trailing space is not the label
    CHECK_FALSE(assetReferenceKindFromToken(" audio").has_value());
    CHECK_FALSE(assetReferenceKindFromToken("audi").has_value());
}

TEST_CASE("AR5: the vocabulary IS the draggable set -- the derivation, in both directions") {
    using engine::editor::assetKindLabel;
    using engine::editor::assetReferenceKindFromToken;
    std::size_t resolvable = 0;
    for (const AssetKind kind : engine::editor::ASSET_KIND_FILTER_OPTIONS) {
        CAPTURE(std::string(assetKindLabel(kind)));
        // The label ASCII-folded to lowercase is exactly the token a field would write.
        std::string token(assetKindLabel(kind));
        for (char& c : token) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + ('a' - 'A'));
            }
        }
        const std::optional<AssetKind> resolved = assetReferenceKindFromToken(token);
        // A kind added to ONE set and not the other is this line, not a silent asymmetry.
        CHECK(resolved.has_value() == assetKindIsDraggable(kind));
        if (resolved.has_value()) {
            CHECK((*resolved == kind));  // and it resolves to ITSELF, never to a neighbour
            ++resolvable;
        }
    }
    CHECK(resolvable == 4);
}

TEST_CASE("AR6: the fifth surface and the fifth action have their own labels") {
    CHECK(dropSurfaceLabel(DropSurface::AssetField) == std::string_view("asset field"));
    CHECK(dropActionLabel(DropAction::AssignAssetReference) == std::string_view("assign asset reference"));
}

TEST_CASE("AR7: the PICKER's accept predicate is this matrix, stated at the DRAG tier") {
    // Deliberately here rather than only in asset_picker_model_test.cpp: stated at this tier it is
    // falsifiable WITHOUT the model, so a model that grew a predicate of its own would have to
    // disagree with two independent cases rather than one.
    using engine::editor::assetPickerAccepts;
    using engine::editor::AssetPickerRules;

    const AssetPickerRules textureSlot{DropSurface::MaterialSlot, std::nullopt};
    const AssetPickerRules unconstrainedField{DropSurface::AssetField, std::nullopt};
    const AssetPickerRules audioField{DropSurface::AssetField, AssetKind::Audio};

    std::size_t slotAccepts = 0;
    std::size_t fieldAccepts = 0;
    std::size_t audioAccepts = 0;
    for (const AssetKind kind : ALL_KINDS) {
        CAPTURE(static_cast<int>(kind));
        CHECK(assetPickerAccepts(textureSlot, kind) == (kind == AssetKind::Texture));
        CHECK(assetPickerAccepts(unconstrainedField, kind) == assetKindIsDraggable(kind));
        CHECK(assetPickerAccepts(audioField, kind) == (kind == AssetKind::Audio));
        slotAccepts += assetPickerAccepts(textureSlot, kind) ? 1U : 0U;
        fieldAccepts += assetPickerAccepts(unconstrainedField, kind) ? 1U : 0U;
        audioAccepts += assetPickerAccepts(audioField, kind) ? 1U : 0U;
    }
    // ANTI-VACUITY: a predicate that always answered false would satisfy none of these.
    CHECK(slotAccepts == 1);
    CHECK(fieldAccepts == 4);
    CHECK(audioAccepts == 1);
}

// ==================================================================================================
// task E.4.3 -- the SECOND payload type and the drag source's new isDirectory (DR19-DR27)
// ==================================================================================================

namespace {

using engine::editor::AssetMoveDragPayload;
using engine::editor::assetMovePayloadFits;
using engine::editor::decodeAssetMoveDragPayload;
using engine::editor::MAX_MOVE_PAYLOAD_PATH;

// Fill a payload from a path, exactly as beginAssetMoveBranch does.
[[nodiscard]] AssetMoveDragPayload makeMovePayload(std::string_view path, bool isDirectory = false) {
    AssetMoveDragPayload payload{};
    REQUIRE(path.size() < MAX_MOVE_PAYLOAD_PATH);
    std::memcpy(payload.path.data(), path.data(), path.size());
    payload.path[path.size()] = '\0';
    payload.length = static_cast<std::uint16_t>(path.size());
    payload.isDirectory = isDirectory ? 1U : 0U;
    return payload;
}

[[nodiscard]] std::size_t countOccurrences(const std::string& body, std::string_view needle) {
    std::size_t count = 0;
    std::size_t at = body.find(needle);
    while (at != std::string::npos) {
        ++count;
        at = body.find(needle, at + needle.size());
    }
    return count;
}

}  // namespace

TEST_CASE("asset drag: classifyAssetKind's second argument, BOTH values (DR19, task E.4.3)") {
    // THE CASE THAT MAKES beginAssetDragSource's new isDirectory parameter SAFE. The argument only
    // ever FORCES Folder, so nothing that was draggable changes kind -- which is why the asset arm of
    // the source is byte-identical in behaviour after the change.
    using engine::editor::AssetKind;
    using engine::editor::classifyAssetKind;
    struct Row {
        std::string_view leaf;
        AssetKind asFile;
    };
    constexpr std::array<Row, 12> ROWS{{
        {"wood.png", AssetKind::Texture},
        {"a.ktx2", AssetKind::Texture},
        {"m.aeromat", AssetKind::Material},
        {"s.gltf", AssetKind::Model},
        {"t.wav", AssetKind::Audio},
        {"n.txt", AssetKind::Text},
        {"x.mtl", AssetKind::Unknown},
        {"textures", AssetKind::Unknown},
        {"README", AssetKind::Unknown},
        {"", AssetKind::Unknown},
        {"a.PNG", AssetKind::Texture},
        {"a.tar.gz", AssetKind::Unknown},
    }};
    std::size_t draggableAsFile = 0;
    for (const Row& row : ROWS) {
        CAPTURE(row.leaf);
        CHECK((classifyAssetKind(row.leaf, false) == row.asFile));
        // isDirectory only ever FORCES Folder -- for EVERY leaf, whatever it would classify as.
        CHECK((classifyAssetKind(row.leaf, true) == AssetKind::Folder));
        if (engine::editor::assetKindIsDraggable(row.asFile)) {
            ++draggableAsFile;
        }
    }
    // ANTI-VACUITY: the roster really does contain draggable kinds, so "nothing draggable changed"
    // is a claim about something.
    REQUIRE(draggableAsFile >= 4U);
}

TEST_CASE("asset drag: decodeAssetMoveDragPayload -- the SIZE rungs, both sides (DR20, task E.4.3)") {
    const AssetMoveDragPayload payload = makeMovePayload("a/b.png");
    CHECK_FALSE(decodeAssetMoveDragPayload(nullptr, static_cast<int>(sizeof payload)).has_value());
    // BOTH sides of the size test, because a `>=`-shaped implementation passes exactly one of them.
    CHECK_FALSE(decodeAssetMoveDragPayload(&payload, static_cast<int>(sizeof payload) - 1).has_value());
    CHECK_FALSE(decodeAssetMoveDragPayload(&payload, static_cast<int>(sizeof payload) + 1).has_value());
    CHECK_FALSE(decodeAssetMoveDragPayload(&payload, 0).has_value());
    // The accepting control: the EXACT size works.
    CHECK(decodeAssetMoveDragPayload(&payload, static_cast<int>(sizeof payload)).has_value());
}

TEST_CASE("asset drag: the LENGTH and NUL rungs (DR21, task E.4.3)") {
    SUBCASE("length 0 is never valid") {
        AssetMoveDragPayload payload = makeMovePayload("a/b");
        payload.length = 0;
        CHECK_FALSE(decodeAssetMoveDragPayload(&payload, static_cast<int>(sizeof payload)).has_value());
    }
    SUBCASE("length == MAX_MOVE_PAYLOAD_PATH is refused BEFORE path[length] is indexed") {
        AssetMoveDragPayload payload = makeMovePayload("a/b");
        payload.length = static_cast<std::uint16_t>(MAX_MOVE_PAYLOAD_PATH);
        CHECK_FALSE(decodeAssetMoveDragPayload(&payload, static_cast<int>(sizeof payload)).has_value());
    }
    SUBCASE("a missing NUL at path[length]") {
        AssetMoveDragPayload payload = makeMovePayload("a/b");
        payload.path[3] = 'X';  // where the NUL was
        CHECK_FALSE(decodeAssetMoveDragPayload(&payload, static_cast<int>(sizeof payload)).has_value());
    }
}

TEST_CASE("asset drag: an ESCAPING path in a payload is CORRUPT, not refusable (DR22, task E.4.3)") {
    // nullopt, never a refusal enumerator and never an empty string -- the same nil-guid-is-corrupt
    // posture decodeAssetDragPayload already takes, applied to the other payload.
    for (const std::string_view bad : {"../escape", "a/../b", "/abs", "C:/x", "a\\b"}) {
        CAPTURE(bad);
        const AssetMoveDragPayload payload = makeMovePayload(bad);
        CHECK_FALSE(decodeAssetMoveDragPayload(&payload, static_cast<int>(sizeof payload)).has_value());
    }
    // ANTI-VACUITY: a legal path through the identical construction DOES decode.
    const AssetMoveDragPayload good = makeMovePayload("a/b.png");
    CHECK(decodeAssetMoveDragPayload(&good, static_cast<int>(sizeof good)) == std::optional<std::string>("a/b.png"));
}

TEST_CASE("asset drag: a valid round trip through a BYTE BUFFER (DR23, task E.4.3)") {
    // Decoding from the struct's own address would not exercise the alignas(1) path UBSan is there to
    // catch, which is the whole reason the decode memcpy's rather than casting.
    const AssetMoveDragPayload payload = makeMovePayload("textures/wood.png");
    std::array<std::byte, sizeof(AssetMoveDragPayload)> buffer{};
    std::memcpy(buffer.data(), &payload, sizeof payload);
    const std::optional<std::string> decoded =
        decodeAssetMoveDragPayload(buffer.data(), static_cast<int>(buffer.size()));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == "textures/wood.png");
}

TEST_CASE("asset drag: the boundary, both directions, through ONE comparator (DR24, task E.4.3)") {
    // THE DECISION UNDER TEST: the source's refusal and the decoder's refusal are one comparator, so
    // they cannot drift apart at the boundary. Raising the bound alone would let the encoder write a
    // TRUNCATED path the decoder then accepts as a different, valid path -- silently moving the wrong
    // file; refusing at source alone would leave a legal-but-deep path undraggable.
    const std::string justFits(MAX_MOVE_PAYLOAD_PATH - 1, 'a');
    const std::string tooLong(MAX_MOVE_PAYLOAD_PATH, 'a');
    CHECK(assetMovePayloadFits(justFits));
    CHECK_FALSE(assetMovePayloadFits(tooLong));
    CHECK_FALSE(assetMovePayloadFits(""));
    // And the payload built from the longest fitting path decodes to exactly that string.
    const AssetMoveDragPayload payload = makeMovePayload(justFits);
    const std::optional<std::string> decoded = decodeAssetMoveDragPayload(&payload, static_cast<int>(sizeof payload));
    REQUIRE(decoded.has_value());
    CHECK(decoded->size() == MAX_MOVE_PAYLOAD_PATH - 1);
    CHECK(*decoded == justFits);
}

TEST_CASE("asset drag: the THREE payload type strings are distinct and legal (DR25, task E.4.3)") {
    // "AERO_ASSET" is a strict PREFIX of "AERO_ASSET_MOVE", which would cross-fire under any prefix
    // test. It cannot: ImGuiPayload::IsDataType is `strcmp(type, DataType) == 0` (imgui.h:2838) -- a
    // FULL compare -- and AcceptDragDropPayload calls it before anything else (imgui.cpp:15859).
    // RE-READ BOTH AT EVERY ImGui BUMP; this case cannot see that they moved.
    constexpr std::array<std::string_view, 3> TYPES{engine::editor::ASSET_PAYLOAD_TYPE,
                                                    engine::editor::ASSET_MOVE_PAYLOAD_TYPE, "AERO_ENTITY"};
    CHECK(TYPES[0] == "AERO_ASSET");  // UNCHANGED by this task
    CHECK(TYPES[1] == "AERO_ASSET_MOVE");
    for (const std::string_view type : TYPES) {
        CAPTURE(type);
        // ImGui's DataType is char[32+1] and its assert is ImStrlen(type) < 33.
        CHECK(type.size() < 32U);
    }
    CHECK(TYPES[0] != TYPES[1]);
    CHECK(TYPES[0] != TYPES[2]);
    CHECK(TYPES[1] != TYPES[2]);
    // The prefix relationship is REAL, which is what makes the strcmp fact load-bearing rather than
    // incidental.
    CHECK(TYPES[1].substr(0, TYPES[0].size()) == TYPES[0]);
}

TEST_CASE("asset drag: AssetDragPayload did NOT move, and the new type's shape is pinned (DR26, task E.4.3)") {
    // The three static_asserts restated as runtime CHECKs, so a failure is a named test failure
    // rather than a build failure with no case attached to it.
    CHECK(sizeof(engine::editor::AssetDragPayload) == 24U);
    CHECK(alignof(engine::editor::AssetDragPayload) == 8U);
    CHECK(std::is_trivially_copyable_v<engine::editor::AssetDragPayload>);
    // And the new one: NO PADDING AT ALL, which is what lets its call site skip the memset
    // AssetDragPayload's seven tail bytes require.
    CHECK(sizeof(AssetMoveDragPayload) == MAX_MOVE_PAYLOAD_PATH + 4U);
    CHECK(alignof(AssetMoveDragPayload) == 2U);
    CHECK(std::is_trivially_copyable_v<AssetMoveDragPayload>);
}

TEST_CASE("asset drag: the hardcoded isDirectory=false is GONE from the drag source (DR27, task E.4.3)") {
    // A SOURCE-TEXT PIN IS THE ONLY WITNESS THERE IS -- nothing in tests/ can start a drag.
    //
    // stripLineComments removes `//` comments, and `/*isDirectory=*/false` is a BLOCK comment, so it
    // SURVIVES the strip and the zero below is meaningful. The opposite assumption would make this
    // assertion vacuous.
    const std::filesystem::path src{AERO_EDITOR_SRC_DIR};
    const std::string body = stripLineComments(readWholeFile(src / "asset_browser_panel.cpp"));
    REQUIRE(body.size() > 20000U);                                 // ANTI-VACUITY: the file was really read
    CHECK(countOccurrences(body, "/*isDirectory=*/false") == 1U);  // the SEARCH HIT site alone
    // Three call sites plus the definition. A fourth call site added without converting it is a
    // compile error, which is what the non-defaulted parameter buys; this pins the roster anyway so
    // a future site that passes a literal `false` by habit is visible.
    CHECK(countOccurrences(body, "beginAssetDragSource(") == 4U);
    // And the block comment that DOES remain is the search-results one, whose `false` is correct:
    // searchAssets never matches a folder.
    CHECK(countOccurrences(body, "hit.relativePath.c_str(), /*isDirectory=*/false") == 1U);
}

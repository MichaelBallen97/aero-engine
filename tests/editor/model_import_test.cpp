// tests/editor/model_import_test.cpp -- task 3.2.1: the canonical imported-model types, the pure URI
// policy (classifyUri/normalizeRelativePath/isImportableModelName), importModel's dispatch, and (from
// Step 4 onward) the glTF backend itself. A TU of aero_editor_shell_test, which supplies main() from
// shell_test.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, tier-0 (D4/AC-17/INV-P5, the asset_meta_test.cpp precedent): model_import.hpp depends only
// on aero/core/{guid,math}.hpp, aero/editor/import_settings.hpp and aero/editor/scene_bounds.hpp -- the
// last of those reaches aero::scene, which is a PUBLIC, UNGATED dependency of aero_editor_core (only
// engine/scene_serialize is gated on AERO_REFLECT_TOOLS) -- so every case in this file must be PRESENT
// and PASSING in all three build configurations. No GPU, no window, no ImGui context, no sleeps, and
// no disk on the critical path: every case here is driven from a string literal.
#include <aero/editor/model_import.hpp>

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The fastgltf-free byte-loading pattern this whole file uses: read a fixture as raw bytes (NEVER
// text mode -- scene_golden::readBytes is already binary), then view it as std::span<const std::byte>
// with no copy. The returned span borrows from `text`, which the caller must keep alive.
[[nodiscard]] std::span<const std::byte> asBytes(const std::string& text) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

constexpr std::string_view TRIANGLE_FIXTURE = AERO_ASSET_FIXTURES_DIR "/triangle.gltf";
constexpr std::string_view MATERIALS_FIXTURE = AERO_ASSET_FIXTURES_DIR "/materials.gltf";
constexpr std::string_view DAMAGED_FIXTURE = AERO_ASSET_FIXTURES_DIR "/damaged.gltf";
constexpr std::string_view HIERARCHY_FIXTURE = AERO_ASSET_FIXTURES_DIR "/hierarchy.gltf";
constexpr std::string_view ASYMMETRIC_FIXTURE = AERO_ASSET_FIXTURES_DIR "/asymmetric.gltf";

// AC-17's own text: the GLB counterpart is NOT a committed binary. It is assembled here from a JSON
// string and an optional BIN chunk, so the 12-byte header ("glTF", version 2, total length), the two
// chunk headers (JSON = 0x4E4F534A, BIN = 0x004E4942) and the 4-byte padding (' ' for JSON, '\0' for
// BIN) are all readable in the test source rather than opaque in a blob.
void appendU32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<char>((value >> 16U) & 0xFFU));
    out.push_back(static_cast<char>((value >> 24U) & 0xFFU));
}

[[nodiscard]] std::string buildGlb(std::string_view json, std::string_view bin) {
    std::string paddedJson(json);
    while (paddedJson.size() % 4U != 0U) {
        paddedJson += ' ';  // JSON chunk padding is SPACE, per the GLB container spec
    }
    std::string paddedBin(bin);
    while (paddedBin.size() % 4U != 0U) {
        paddedBin += '\0';  // BIN chunk padding is NUL
    }
    const bool hasBin = !bin.empty();
    const auto jsonChunkLength = static_cast<std::uint32_t>(paddedJson.size());
    const auto binChunkLength = static_cast<std::uint32_t>(paddedBin.size());
    const std::uint32_t totalLength = 12U + 8U + jsonChunkLength + (hasBin ? (8U + binChunkLength) : 0U);

    std::string glb;
    glb += "glTF";       // magic
    appendU32(glb, 2U);  // version
    appendU32(glb, totalLength);
    appendU32(glb, jsonChunkLength);
    appendU32(glb, 0x4E4F534AU);  // 'JSON'
    glb += paddedJson;
    if (hasBin) {
        appendU32(glb, binChunkLength);
        appendU32(glb, 0x004E4942U);  // 'BIN\0'
        glb += paddedBin;
    }
    return glb;
}

}  // namespace

using engine::editor::classifyUri;
using engine::editor::has;
using engine::editor::ImportDepth;
using engine::editor::importModel;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;
using engine::editor::importStatusLabel;
using engine::editor::isImportableModelName;
using engine::editor::normalizeRelativePath;
using engine::editor::UriClass;
using engine::editor::UriClassification;
using engine::editor::VertexAttribute;

// ---- normalizeRelativePath (MI1-MI6) ---------------------------------------------------------------

TEST_CASE("model_import: normalizeRelativePath passes an already-clean path through (MI1)") {
    const std::optional<std::string> result = normalizeRelativePath("a/b/c.png");
    REQUIRE(result.has_value());
    CHECK(*result == "a/b/c.png");
}

TEST_CASE("model_import: normalizeRelativePath drops '.' and empty segments (MI2)") {
    const std::optional<std::string> result = normalizeRelativePath("a/./b//c.png");
    REQUIRE(result.has_value());
    CHECK(*result == "a/b/c.png");
}

TEST_CASE("model_import: normalizeRelativePath resolves an ordinary '..' (MI3)") {
    const std::optional<std::string> result = normalizeRelativePath("a/b/../c.png");
    REQUIRE(result.has_value());
    CHECK(*result == "a/c.png");
}

TEST_CASE("model_import: normalizeRelativePath refuses an underflowing '..' (MI4)") {
    CHECK_FALSE(normalizeRelativePath("../x.png").has_value());
}

TEST_CASE("model_import: normalizeRelativePath refuses a deeper underflow (MI5)") {
    CHECK_FALSE(normalizeRelativePath("a/../../x.png").has_value());
}

TEST_CASE("model_import: normalizeRelativePath collapsing to nothing returns EMPTY, never nullopt (MI6)") {
    const std::optional<std::string> result = normalizeRelativePath("a/..");
    REQUIRE(result.has_value());
    CHECK(result->empty());
}

// ---- classifyUri (MI7-MI18) -------------------------------------------------------------------------

TEST_CASE("model_import: classifyUri refuses an http scheme (MI7, AC-38)") {
    const UriClassification result = classifyUri("http://evil/x.bin", "models");
    CHECK(result.kind == UriClass::RefusedScheme);
    CHECK_FALSE(result.reason.empty());
}

TEST_CASE("model_import: classifyUri refuses every other network/file scheme (MI8, AC-38)") {
    constexpr std::array<std::string_view, 3> SCHEMES = {"https://evil/x.bin", "file:///etc/passwd",
                                                         "ftp://evil/x.bin"};
    for (const std::string_view uri : SCHEMES) {
        const UriClassification result = classifyUri(uri, "models");
        CHECK(result.kind == UriClass::RefusedScheme);
    }
}

TEST_CASE("model_import: classifyUri refuses an absolute path (MI9, AC-38)") {
    const UriClassification result = classifyUri("/abs/x.png", "models");
    CHECK(result.kind == UriClass::RefusedAbsolute);
}

TEST_CASE(
    "model_import: classifyUri refuses a Windows drive letter or UNC path as a BACKSLASH, before the "
    "scheme test ever runs (MI10, AC-38)") {
    CHECK(classifyUri("C:\\x.png", "models").kind == UriClass::RefusedBackslash);
    CHECK(classifyUri("\\\\unc\\x", "models").kind == UriClass::RefusedBackslash);
}

TEST_CASE("model_import: classifyUri refuses a backslash identically regardless of platform (MI11, AC-38, E26)") {
    CHECK(classifyUri("a\\b.png", "models").kind == UriClass::RefusedBackslash);
}

TEST_CASE("model_import: classifyUri refuses the empty URI (MI12, AC-38)") {
    CHECK(classifyUri("", "models").kind == UriClass::RefusedEmpty);
}

TEST_CASE("model_import: classifyUri refuses a control character (MI13, AC-38, plan A1)") {
    std::string withNul = "wood";
    withNul.push_back('\0');
    withNul += ".png";
    const UriClassification nulResult = classifyUri(withNul, "models");
    CHECK(nulResult.kind == UriClass::RefusedControlChar);
    CHECK_FALSE(nulResult.reason.empty());

    const UriClassification newlineResult = classifyUri("wood\n.png", "models");
    CHECK(newlineResult.kind == UriClass::RefusedControlChar);
    CHECK_FALSE(newlineResult.reason.empty());
}

TEST_CASE("model_import: classifyUri accepts a data: URI (MI14)") {
    const UriClassification result = classifyUri("data:application/octet-stream;base64,AAAA", "models");
    CHECK(result.kind == UriClass::DataUri);
    CHECK(result.relativePath.empty());
}

TEST_CASE("model_import: classifyUri accepts an ordinary relative path (MI15, AC-38)") {
    const UriClassification result = classifyUri("wood.png", "models");
    CHECK(result.kind == UriClass::RelativePath);
    CHECK(result.relativePath == "models/wood.png");
}

TEST_CASE(
    "model_import: classifyUri accepts a '..' that stays inside the assets root -- ordinary authoring "
    "(MI16, AC-38, D14)") {
    const UriClassification result = classifyUri("../textures/wood.png", "models");
    CHECK(result.kind == UriClass::RelativePath);
    CHECK(result.relativePath == "textures/wood.png");
}

TEST_CASE("model_import: classifyUri refuses a '..' that escapes the assets root (MI17, AC-38)") {
    const UriClassification result = classifyUri("../../x.png", "models");
    CHECK(result.kind == UriClass::RefusedEscape);
}

TEST_CASE("model_import: classifyUri resolves a model at the assets root with no leading directory (MI18, AC-38)") {
    const UriClassification result = classifyUri("wood.png", "");
    CHECK(result.kind == UriClass::RelativePath);
    CHECK(result.relativePath == "wood.png");
}

// ---- isImportableModelName / importStatusLabel / VertexAttribute (MI19-MI26) -----------------------

TEST_CASE("model_import: isImportableModelName accepts .gltf/.glb case-insensitively (MI19, AC-44)") {
    CHECK(isImportableModelName("a.gltf"));
    CHECK(isImportableModelName("a.GLTF"));
    CHECK(isImportableModelName("a.glb"));
    CHECK(isImportableModelName("a.GLB"));
}

TEST_CASE("model_import: isImportableModelName rejects everything else, including a bare extension (MI20)") {
    CHECK_FALSE(isImportableModelName("a.fbx"));
    CHECK_FALSE(isImportableModelName("a.obj"));
    CHECK_FALSE(isImportableModelName("a.gltf.bak"));
    CHECK_FALSE(isImportableModelName("a"));
    CHECK_FALSE(isImportableModelName(""));
    // 5 bytes, not > 5 -- needs something BEFORE the extension (the isMetaFileName shape).
    CHECK_FALSE(isImportableModelName(".gltf"));
}

TEST_CASE("model_import: isImportableModelName is a SUFFIX test on the FULL name (MI21)") {
    CHECK(isImportableModelName("archive.tar.gltf"));
}

TEST_CASE("model_import: importStatusLabel covers every enumerator with a distinct, non-empty label (MI22)") {
    constexpr std::array<ImportStatus, 7> ALL_STATUSES = {
        ImportStatus::Ok,        ImportStatus::Unsupported,      ImportStatus::ParseFailed,
        ImportStatus::Malformed, ImportStatus::MissingExtension, ImportStatus::MissingBuffer,
        ImportStatus::Truncated,
    };
    std::vector<std::string_view> labels;
    for (const ImportStatus status : ALL_STATUSES) {
        const std::string_view label = importStatusLabel(status);
        CHECK_FALSE(label.empty());
        labels.push_back(label);
    }
    for (std::size_t i = 0; i < labels.size(); ++i) {
        for (std::size_t j = i + 1; j < labels.size(); ++j) {
            CHECK(labels[i] != labels[j]);
        }
    }
}

TEST_CASE("model_import: VertexAttribute operator| combines bits (MI23)") {
    const VertexAttribute combined = VertexAttribute::Position | VertexAttribute::Normal;
    CHECK(has(combined, VertexAttribute::Position));
    CHECK(has(combined, VertexAttribute::Normal));
    CHECK_FALSE(has(combined, VertexAttribute::Tangent));
}

TEST_CASE("model_import: VertexAttribute has() reads a bit independently of every other bit (MI24)") {
    const VertexAttribute all = VertexAttribute::Position | VertexAttribute::Normal | VertexAttribute::Tangent |
                                VertexAttribute::TexCoord0 | VertexAttribute::TexCoord1 | VertexAttribute::Color0 |
                                VertexAttribute::Joints0 | VertexAttribute::Weights0;
    CHECK(has(all, VertexAttribute::Position));
    CHECK(has(all, VertexAttribute::Weights0));
}

TEST_CASE("model_import: VertexAttribute::None has no bit set (MI25)") {
    CHECK_FALSE(has(VertexAttribute::None, VertexAttribute::Position));
    CHECK_FALSE(has(VertexAttribute::None, VertexAttribute::Normal));
}

TEST_CASE("model_import: VertexAttribute operator|= mutates in place (MI26)") {
    VertexAttribute attrs = VertexAttribute::Position;
    attrs |= VertexAttribute::Normal;
    CHECK(has(attrs, VertexAttribute::Position));
    CHECK(has(attrs, VertexAttribute::Normal));
}

// ---- importModel's dispatch on a non-model name (MI27-MI32, AC-44) ---------------------------------

TEST_CASE("model_import: importModel refuses an unrecognised extension (MI27, AC-44)") {
    const ImportResult result = importModel("notes.txt", "", {}, ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Unsupported);
    CHECK_FALSE(result.message.empty());
}

TEST_CASE("model_import: importModel refuses a model format this task does not implement yet (MI28, AC-44)") {
    const ImportResult result = importModel("chair.fbx", "models", {}, ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Unsupported);
}

TEST_CASE("model_import: importModel refuses a name with no extension at all (MI29, AC-44)") {
    const ImportResult result = importModel("README", "", {}, ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Unsupported);
}

TEST_CASE("model_import: importModel refuses the empty file name (MI30, AC-44)") {
    const ImportResult result = importModel("", "", {}, ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Unsupported);
}

TEST_CASE("model_import: importModel never dereferences bytes for a non-model name (MI31, AC-44)") {
    const std::span<const std::byte> emptySpan;  // {nullptr, 0} -- any dereference would crash/ASan-trip
    const ImportResult result = importModel("model.fbx", "", emptySpan, ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Unsupported);
}

TEST_CASE("model_import: importModel on Unsupported yields an entirely empty model (MI32, AC-44)") {
    const ImportResult result = importModel("notes.txt", "", {}, ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Unsupported);
    CHECK(result.model.nodes.empty());
    CHECK(result.model.meshes.empty());
    CHECK(result.model.materials.empty());
    CHECK(result.model.images.empty());
    CHECK(result.model.skins.empty());
    CHECK(result.model.animations.empty());
    CHECK(result.externalUris.empty());
    CHECK(result.warnings.empty());
    CHECK(result.warningTotal == 0);
}

// ---- the glTF backend, phases 1-3: load, extensions, URIs (Step 4) ---------------------------------
//
// NOTE on MI33 and MI42: both are deferred to Step 6 (grouped with MI59+), because both require a
// phase this step does not yet write -- MI33 asserts "1 node, 1 mesh, 1 primitive" (phase 4 and phase
// 6 both build those), and MI42 asserts AC-39's MissingBuffer status, which is only ever set by the
// EditorBufferAdapter's sawMissingBuffer() flag, itself only ever engaged from inside a phase-6
// accessor read. Writing them here against a function that has not yet grown those phases would be a
// case authored red against this step's own gate; §V1 explicitly permits this ("MEASURE, never
// assume", and Step 7's own text sanctions renumbering/relocating a case for the identical reason).
// NOTE on MI45: importStatusLabel's full-enumerator coverage is ALREADY MI22 (Step 2) -- word for word
// the same behaviour under a second plan-assigned number. Not duplicated here.

TEST_CASE("model_import: a truncated document fails to parse with an empty model (MI34, AC-40)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(DAMAGED_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("damaged.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::ParseFailed);
    CHECK_FALSE(result.message.empty());
    CHECK(result.model.nodes.empty());
    CHECK(result.model.meshes.empty());
    CHECK(result.model.images.empty());
}

TEST_CASE("model_import: PNG magic bytes named .gltf fail to parse without crashing (MI35, E6)") {
    const std::string pngMagic =
        "\x89"
        "PNG\r\n\x1a\n"
        "this is not JSON and not a GLB container";
    const ImportResult result =
        importModel("fake.gltf", "", asBytes(pngMagic), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::ParseFailed);
    CHECK_FALSE(result.message.empty());
}

TEST_CASE("model_import: an empty (but non-null) byte span fails to parse (MI36)") {
    const std::string empty;  // .data() is non-null even for an empty std::string
    const ImportResult result =
        importModel("empty.gltf", "", asBytes(empty), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::ParseFailed);
}

TEST_CASE(
    "model_import: extensionsUsed naming two unimplemented extensions succeeds with two warnings "
    "(MI37, AC-41)") {
    const std::string doc = R"({"asset":{"version":"2.0"},"extensionsUsed":["FOO_made_up_one","BAR_made_up_two"]})";
    const ImportResult result = importModel("ext.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    CHECK(result.warnings.size() == 2);
    CHECK(result.warningTotal == 2);
}

TEST_CASE(
    "model_import: extensionsRequired naming an extension fastgltf KNOWS but this build does not "
    "enable fails MissingExtension and NAMES it (MI38, AC-41, §A-6)") {
    const std::string doc = R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_draco_mesh_compression"],)"
                            R"("extensionsRequired":["KHR_draco_mesh_compression"]})";
    const ImportResult result =
        importModel("required.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::MissingExtension);
    CHECK(result.message.find("KHR_draco_mesh_compression") != std::string::npos);
    CHECK(result.model.nodes.empty());
    CHECK(result.model.images.empty());
}

TEST_CASE(
    "model_import: extensionsRequired naming an extension fastgltf does NOT recognise fails "
    "MissingExtension honestly, without inventing a name (MI38b, AC-41, §A-6)") {
    const std::string doc = R"({"asset":{"version":"2.0"},"extensionsUsed":["ACME_nonexistent_extension"],)"
                            R"("extensionsRequired":["ACME_nonexistent_extension"]})";
    const ImportResult result =
        importModel("unknown-ext.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::MissingExtension);
    CHECK(result.message.find("not one fastgltf recognises") != std::string::npos);
    CHECK(result.message.find("ACME_nonexistent_extension") == std::string::npos);
}

TEST_CASE(
    "model_import: a successful import never runs the MissingExtensions recovery re-parse "
    "(MI38c, §A-6)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(TRIANGLE_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("triangle.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    CHECK(result.message.empty());
}

TEST_CASE(
    "model_import: materials.gltf's externalUris holds exactly the accepted paths, deduplicated, "
    "in first-seen order (MI39, AC-38)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MATERIALS_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("materials.gltf", "models", asBytes(fixture.text), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.externalUris.size() == 1);
    CHECK(result.externalUris[0] == "shared/wood.png");
}

TEST_CASE(
    "model_import: materials.gltf's refused images each carry a non-empty refusal and produce "
    "exactly one warning (MI40, AC-5)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MATERIALS_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("materials.gltf", "models", asBytes(fixture.text), ImportSettings{}, ImportDepth::Structure, {});
    REQUIRE(result.model.images.size() == 5);
    CHECK(result.model.images[0].refusal.empty());        // the one accepted relative path
    CHECK_FALSE(result.model.images[1].refusal.empty());  // http://
    CHECK_FALSE(result.model.images[2].refusal.empty());  // ../.. escape
    CHECK_FALSE(result.model.images[3].refusal.empty());  // backslash
    CHECK_FALSE(result.model.images[4].refusal.empty());  // %zz control char
    CHECK(result.warningTotal == 4);
}

TEST_CASE(
    "model_import: two URIs normalising to the same path appear once in externalUris and both "
    "images share the resolved relativePath (MI41, E8)") {
    const std::string doc = R"({"asset":{"version":"2.0"},"images":[{"uri":"a/../wood.png"},{"uri":"wood.png"}]})";
    const ImportResult result =
        importModel("dedup.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    REQUIRE(result.externalUris.size() == 1);
    CHECK(result.externalUris[0] == "wood.png");
    REQUIRE(result.model.images.size() == 2);
    CHECK(result.model.images[0].relativePath == "wood.png");
    CHECK(result.model.images[1].relativePath == "wood.png");
}

TEST_CASE(
    "model_import: an embedded (data:) image is embedded and contributes no externalUris entry "
    "(MI43, D14)") {
    const std::string doc = R"({"asset":{"version":"2.0"},"images":[{"uri":"data:image/png;base64,QQ=="}]})";
    const ImportResult result =
        importModel("embedded.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.images.size() == 1);
    CHECK(result.model.images[0].embedded);
    CHECK(result.model.images[0].refusal.empty());
    CHECK(result.externalUris.empty());
}

TEST_CASE("model_import: MAX_EXTERNAL_URIS truncates a document and names the cap (MI44, AC-42, D15)") {
    std::string doc = R"({"asset":{"version":"2.0"},"images":[)";
    for (std::size_t i = 0; i < engine::editor::MAX_EXTERNAL_URIS + 1; ++i) {
        if (i != 0) {
            doc += ',';
        }
        doc += std::format(R"({{"uri":"img{}.png"}})", i);
    }
    doc += "]}";
    const ImportResult result =
        importModel("many-uris.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Truncated);
    CHECK_FALSE(result.message.empty());
    CHECK(result.externalUris.size() == engine::editor::MAX_EXTERNAL_URIS);
}

TEST_CASE(
    "model_import: MAX_EMBEDDED_BYTES truncates an over-cap embedded image, checked AFTER the "
    "allocation it bounds (MI46, AC-42, plan §A-8)") {
    // A GLB-style image sourced from a bufferView whose DECLARED byteLength exceeds the cap. The
    // underlying buffer is genuinely tiny (8 real bytes) -- fastgltf's parser does not cross-validate a
    // bufferView's claimed range against its buffer's actual size (that is precisely why validateAccessor
    // exists, plan §A-5), so this exercises the cap without materialising a 128+ MiB fixture.
    const std::string doc = R"({"asset":{"version":"2.0"},)"
                            R"("buffers":[{"byteLength":8,"uri":"data:application/octet-stream;base64,QQAAAA=="}],)"
                            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":200000000}],)"
                            R"("images":[{"bufferView":0,"mimeType":"image/png"}]})";
    const ImportResult result =
        importModel("huge-embedded.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Truncated);
    CHECK_FALSE(result.message.empty());
    REQUIRE(result.model.images.size() == 1);
    CHECK_FALSE(result.model.images[0].embedded);
    CHECK_FALSE(result.model.images[0].refusal.empty());
}

// ---- the glTF backend, phase 4: nodes, hierarchy, decomposition, cycles (Step 5) --------------------
//
// hierarchy.gltf's shape (§D-11): six nodes, a depth-4 chain (0->1->2->3), TWO roots (0 and 4), node 4
// using `matrix` instead of TRS, node 1's children naming an out-of-range index (99), and node 5
// claimed by BOTH root's children lists -- node 0 (source order i=0) wins over node 4 (i=4).

TEST_CASE(
    "model_import: hierarchy.gltf's depth-4 chain has correct parent/children edges and TRS "
    "values (MI47, AC-18)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(HIERARCHY_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("hierarchy.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Structure, {});
    REQUIRE(result.model.nodes.size() == 6);

    const auto& n0 = result.model.nodes[0];
    CHECK(n0.name == "RootA");
    CHECK(n0.parent == engine::editor::INVALID_SUBASSET);
    CHECK(n0.translation.x == doctest::Approx(1.0F));
    CHECK(n0.translation.y == doctest::Approx(0.0F));
    CHECK(n0.translation.z == doctest::Approx(0.0F));
    CHECK(n0.rotation.w == doctest::Approx(1.0F));
    CHECK(n0.scale.x == doctest::Approx(1.0F));

    const auto& n1 = result.model.nodes[1];
    CHECK(n1.name == "Child1");
    CHECK(n1.parent == 0);
    CHECK(n1.translation.y == doctest::Approx(2.0F));
    REQUIRE(n1.children.size() == 1);
    CHECK(n1.children[0] == 2);  // the out-of-range 99 was dropped (MI50)

    const auto& n2 = result.model.nodes[2];
    CHECK(n2.name == "Child2");
    CHECK(n2.parent == 1);
    CHECK(n2.translation.z == doctest::Approx(3.0F));
    REQUIRE(n2.children.size() == 1);
    CHECK(n2.children[0] == 3);

    const auto& n3 = result.model.nodes[3];
    CHECK(n3.name == "Child3");
    CHECK(n3.parent == 2);
    CHECK(n3.translation.x == doctest::Approx(4.0F));
    CHECK(n3.translation.y == doctest::Approx(4.0F));
    CHECK(n3.translation.z == doctest::Approx(4.0F));
    CHECK(n3.children.empty());
}

TEST_CASE(
    "model_import: hierarchy.gltf's roots are exactly the parentless nodes, in source order "
    "(MI48, AC-21)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(HIERARCHY_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("hierarchy.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Structure, {});
    REQUIRE(result.model.roots.size() == 2);
    CHECK(result.model.roots[0] == 0);
    CHECK(result.model.roots[1] == 4);
}

TEST_CASE(
    "model_import: hierarchy.gltf's matrix node decomposes to TRS and the document produces "
    "exactly one 'matrix' warning (MI49, AC-19 as amended)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(HIERARCHY_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("hierarchy.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Structure, {});
    REQUIRE(result.model.nodes.size() == 6);
    const auto& n4 = result.model.nodes[4];
    CHECK(n4.name == "RootB-Matrix");
    // matrix = diag(2,2,2) with translation (5,0,0), no rotation -- decomposeTransformMatrix must
    // recover exactly that.
    CHECK(n4.translation.x == doctest::Approx(5.0F));
    CHECK(n4.translation.y == doctest::Approx(0.0F));
    CHECK(n4.translation.z == doctest::Approx(0.0F));
    CHECK(n4.scale.x == doctest::Approx(2.0F));
    CHECK(n4.scale.y == doctest::Approx(2.0F));
    CHECK(n4.scale.z == doctest::Approx(2.0F));
    CHECK(n4.rotation.x == doctest::Approx(0.0F));
    CHECK(n4.rotation.y == doctest::Approx(0.0F));
    CHECK(n4.rotation.z == doctest::Approx(0.0F));
    CHECK(n4.rotation.w == doctest::Approx(1.0F));

    std::size_t matrixWarnings = 0;
    for (const std::string& w : result.warnings) {
        if (w.find("matrix") != std::string::npos) {
            ++matrixWarnings;
        }
    }
    CHECK(matrixWarnings == 1);
}

TEST_CASE(
    "model_import: an out-of-range child index is dropped with one warning; every other edge "
    "survives (MI50)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(HIERARCHY_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("hierarchy.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Structure, {});
    bool sawOutOfRangeWarning = false;
    for (const std::string& w : result.warnings) {
        if (w.find("out of range") != std::string::npos) {
            sawOutOfRangeWarning = true;
        }
    }
    CHECK(sawOutOfRangeWarning);
    REQUIRE(result.model.nodes.size() == 6);
    CHECK(result.model.nodes[1].children.size() == 1);  // [2, 99] -> [2]
}

TEST_CASE(
    "model_import: a child claimed by two parents keeps the FIRST parent in source order, with "
    "one warning (MI51, E25)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(HIERARCHY_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("hierarchy.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Structure, {});
    REQUIRE(result.model.nodes.size() == 6);
    CHECK(result.model.nodes[5].parent == 0);  // node 0 (i=0) claims node 5 before node 4 (i=4) can
    REQUIRE(result.model.nodes[4].children.empty());
    bool sawAlreadyHasParent = false;
    for (const std::string& w : result.warnings) {
        if (w.find("already has a parent") != std::string::npos) {
            sawAlreadyHasParent = true;
        }
    }
    CHECK(sawAlreadyHasParent);
}

TEST_CASE(
    "model_import: a synthesised cyclic children array terminates, imports every node once, and "
    "reports one warning (MI52, AC-20)") {
    // node0 -(child)-> node1 -(child)-> node2 -(child, an ANCESTOR back-edge)-> node1. Node1 is already
    // claimed by node0 (source order i=0, before i=2 tries), so node2's back-edge is refused -- one
    // warning, no edge, and the tree stays a tree (A24).
    const std::string doc = R"({"asset":{"version":"2.0"},"nodes":[)"
                            R"({"name":"Root","children":[1]},)"
                            R"({"name":"Mid","children":[2]},)"
                            R"({"name":"Leaf","children":[1]})"
                            R"(]})";
    const ImportResult result =
        importModel("cycle.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    REQUIRE(result.model.nodes.size() == 3);
    CHECK(result.model.roots.size() == 1);
    CHECK(result.model.roots[0] == 0);
    CHECK(result.model.nodes[1].parent == 0);
    REQUIRE(result.model.nodes[1].children.size() == 1);
    CHECK(result.model.nodes[1].children[0] == 2);
    CHECK(result.model.nodes[2].parent == 1);
    CHECK(result.model.nodes[2].children.empty());  // the back-edge to 1 was refused
    CHECK(result.warningTotal == 1);
}

TEST_CASE(
    "model_import: settings.scale multiplies ONLY roots' translations; a depth-1 child is "
    "unchanged (MI53, AC-30, sabotage S13's discriminator)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(HIERARCHY_FIXTURE);
    REQUIRE(fixture.ok);
    ImportSettings settings;
    settings.scale = 2.0F;
    const ImportResult result =
        importModel("hierarchy.gltf", "", asBytes(fixture.text), settings, ImportDepth::Structure, {});
    REQUIRE(result.model.nodes.size() == 6);
    CHECK(result.model.nodes[0].translation.x == doctest::Approx(2.0F));   // root: 1 * 2
    CHECK(result.model.nodes[4].translation.x == doctest::Approx(10.0F));  // root: 5 * 2
    CHECK(result.model.nodes[1].translation.y == doctest::Approx(2.0F));   // CHILD: unchanged
    CHECK(result.model.nodes[2].translation.z == doctest::Approx(3.0F));   // CHILD: unchanged
}

TEST_CASE(
    "model_import: MAX_NODES_PER_MODEL truncates a document, keeping a coherent smaller model "
    "(MI54, AC-42, D15)") {
    std::string doc = R"({"asset":{"version":"2.0"},"nodes":[)";
    for (std::size_t i = 0; i < engine::editor::MAX_NODES_PER_MODEL + 1; ++i) {
        if (i != 0) {
            doc += ',';
        }
        doc += "{}";
    }
    doc += "]}";
    const ImportResult result =
        importModel("many-nodes.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Truncated);
    CHECK_FALSE(result.message.empty());
    CHECK(result.model.nodes.size() == engine::editor::MAX_NODES_PER_MODEL);
}

TEST_CASE(
    "model_import: a node's mesh/skin indices are set when present, INVALID_SUBASSET otherwise "
    "(MI55)") {
    const std::string doc = R"({"asset":{"version":"2.0"},)"
                            R"("nodes":[{"name":"WithMeshSkin","mesh":0,"skin":0},{"name":"Bare"}]})";
    const ImportResult result =
        importModel("mesh-skin-refs.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    REQUIRE(result.model.nodes.size() == 2);
    CHECK(result.model.nodes[0].meshIndex == 0);
    CHECK(result.model.nodes[0].skinIndex == 0);
    CHECK(result.model.nodes[1].meshIndex == engine::editor::INVALID_SUBASSET);
    CHECK(result.model.nodes[1].skinIndex == engine::editor::INVALID_SUBASSET);
}

TEST_CASE("model_import: a node with no name imports name==\"\" and localId==its index (MI56, D13)") {
    const std::string doc = R"({"asset":{"version":"2.0"},"nodes":[{},{"name":"Named"}]})";
    const ImportResult result =
        importModel("unnamed-node.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    REQUIRE(result.model.nodes.size() == 2);
    CHECK(result.model.nodes[0].name.empty());
    CHECK(result.model.nodes[0].localId == 0);
    CHECK(result.model.nodes[1].name == "Named");
    CHECK(result.model.nodes[1].localId == 1);
}

TEST_CASE(
    "model_import: asymmetric.gltf's node TRS matches the hand-computed literals exactly -- "
    "F7b's pin, the TRS third (MI57, AC-30b)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(ASYMMETRIC_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("asymmetric.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Structure, {});
    REQUIRE(result.model.nodes.size() == 1);
    const auto& node = result.model.nodes[0];

    // t = (1, 2, 3) -- exact.
    CHECK(node.translation.x == doctest::Approx(1.0F));
    CHECK(node.translation.y == doctest::Approx(2.0F));
    CHECK(node.translation.z == doctest::Approx(3.0F));

    // r: a non-identity unit quaternion with four distinct components, in glTF's OWN {x,y,z,w} order --
    // NO reorder, NO renormalisation. Hand-computed from unnormalised (1,2,3,5), magnitude sqrt(39).
    CHECK(node.rotation.x == doctest::Approx(0.1601281464099884).epsilon(0.0001));
    CHECK(node.rotation.y == doctest::Approx(0.3202562928199768).epsilon(0.0001));
    CHECK(node.rotation.z == doctest::Approx(0.4803844690322876).epsilon(0.0001));
    CHECK(node.rotation.w == doctest::Approx(0.8006407618522644).epsilon(0.0001));
    // Every component DISTINCT -- proves no accidental collapse (e.g. a reorder that happened to be a
    // no-op for a symmetric fixture could never be caught; this fixture is deliberately asymmetric).
    CHECK(node.rotation.x != doctest::Approx(node.rotation.y));
    CHECK(node.rotation.y != doctest::Approx(node.rotation.z));
    CHECK(node.rotation.z != doctest::Approx(node.rotation.w));

    // s = (2, 3, 4) -- exact, three distinct values.
    CHECK(node.scale.x == doctest::Approx(2.0F));
    CHECK(node.scale.y == doctest::Approx(3.0F));
    CHECK(node.scale.z == doctest::Approx(4.0F));
}

// ---- the glTF backend, phases 5-6: materials, meshes, accessors, caps, GLB (Step 6) -----------------
//
// MI33 and MI42 land HERE, not in Step 4 where the plan's own table lists them -- both need phase 4
// and/or phase 6, which did not exist until this step and the previous one. See the note at the top of
// the phase 1-3 section above.

TEST_CASE(
    "model_import: triangle.gltf at Structure depth -- 1 node, 1 mesh, 1 primitive, no vertex "
    "data (MI33, INV-M4's Structure half)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(TRIANGLE_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("triangle.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.nodes.size() == 1);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    CHECK(result.model.summary.vertexCount == 0);
    CHECK(result.model.summary.triangleCount == 0);
    const auto& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.positions.empty());
    CHECK(prim.normals.empty());
    CHECK(prim.indices.empty());
}

TEST_CASE(
    "model_import: a Full import with zero supplied externals reports MissingBuffer -- the only "
    "way a read could have happened is if fastgltf opened the file itself (MI42, AC-39)") {
    const std::string doc = R"({"asset":{"version":"2.0"},)"
                            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4}]}],)"
                            R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
                            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
                            R"("buffers":[{"byteLength":36,"uri":"external.bin"}]})";
    const ImportResult result =
        importModel("needs-external.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::MissingBuffer);
}

TEST_CASE("model_import: triangle.gltf at Full depth -- exact positions, indices and AABB (MI59, AC-16)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(TRIANGLE_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("triangle.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& prim = result.model.meshes[0].primitives[0];
    REQUIRE(prim.positions.size() == 3);
    CHECK(prim.positions[0].x == doctest::Approx(0.0F));
    CHECK(prim.positions[0].y == doctest::Approx(0.0F));
    CHECK(prim.positions[0].z == doctest::Approx(0.0F));
    CHECK(prim.positions[1].x == doctest::Approx(1.0F));
    CHECK(prim.positions[1].y == doctest::Approx(0.0F));
    CHECK(prim.positions[2].x == doctest::Approx(0.0F));
    CHECK(prim.positions[2].y == doctest::Approx(1.0F));
    REQUIRE(prim.indices.size() == 3);
    CHECK(prim.indices[0] == 0);
    CHECK(prim.indices[1] == 1);
    CHECK(prim.indices[2] == 2);
    CHECK(prim.bounds.min.x == doctest::Approx(0.0F));
    CHECK(prim.bounds.min.y == doctest::Approx(0.0F));
    CHECK(prim.bounds.max.x == doctest::Approx(1.0F));
    CHECK(prim.bounds.max.y == doctest::Approx(1.0F));
}

TEST_CASE("model_import: Structure and Full agree on every field they share (MI60, INV-M4)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(TRIANGLE_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult structureResult =
        importModel("triangle.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Structure, {});
    const ImportResult fullResult =
        importModel("triangle.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
    CHECK(structureResult.model.nodes.size() == fullResult.model.nodes.size());
    CHECK(structureResult.model.nodes[0].name == fullResult.model.nodes[0].name);
    CHECK(structureResult.model.roots == fullResult.model.roots);
    CHECK(structureResult.model.meshes.size() == fullResult.model.meshes.size());
    CHECK(structureResult.model.materials.size() == fullResult.model.materials.size());
    CHECK(structureResult.model.images.size() == fullResult.model.images.size());
    CHECK(structureResult.externalUris == fullResult.externalUris);
}

TEST_CASE(
    "model_import: the same document as a GLB imports field-for-field equal to the .gltf form "
    "(MI61, AC-17)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(TRIANGLE_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult gltfResult =
        importModel("triangle.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
    const std::string glb = buildGlb(fixture.text, "");
    const ImportResult glbResult =
        importModel("triangle.glb", "", asBytes(glb), ImportSettings{}, ImportDepth::Full, {});
    CHECK(glbResult.status == gltfResult.status);
    REQUIRE(glbResult.model.nodes.size() == gltfResult.model.nodes.size());
    CHECK(glbResult.model.nodes[0].name == gltfResult.model.nodes[0].name);
    REQUIRE(glbResult.model.meshes.size() == gltfResult.model.meshes.size());
    REQUIRE(glbResult.model.meshes[0].primitives.size() == gltfResult.model.meshes[0].primitives.size());
    REQUIRE(glbResult.model.meshes[0].primitives[0].positions.size() ==
            gltfResult.model.meshes[0].primitives[0].positions.size());
    for (std::size_t i = 0; i < glbResult.model.meshes[0].primitives[0].positions.size(); ++i) {
        CHECK(glbResult.model.meshes[0].primitives[0].positions[i].x ==
              doctest::Approx(gltfResult.model.meshes[0].primitives[0].positions[i].x));
        CHECK(glbResult.model.meshes[0].primitives[0].positions[i].y ==
              doctest::Approx(gltfResult.model.meshes[0].primitives[0].positions[i].y));
        CHECK(glbResult.model.meshes[0].primitives[0].positions[i].z ==
              doctest::Approx(gltfResult.model.meshes[0].primitives[0].positions[i].z));
    }
    REQUIRE(glbResult.model.meshes[0].primitives[0].indices.size() ==
            gltfResult.model.meshes[0].primitives[0].indices.size());
    CHECK(glbResult.model.meshes[0].primitives[0].indices == gltfResult.model.meshes[0].primitives[0].indices);
}

TEST_CASE("model_import: four malformed GLB containers each fail to parse cleanly (MI62-65, E4)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(TRIANGLE_FIXTURE);
    REQUIRE(fixture.ok);
    const std::string validGlb = buildGlb(fixture.text, "");

    SUBCASE("bad magic (MI62)") {
        std::string bad = validGlb;
        bad[0] = 'X';
        const ImportResult result =
            importModel("bad.glb", "", asBytes(bad), ImportSettings{}, ImportDepth::Structure, {});
        CHECK(result.status == ImportStatus::ParseFailed);
        CHECK_FALSE(result.message.empty());
    }
    SUBCASE("bad version (MI63)") {
        std::string bad = validGlb;
        bad[4] = static_cast<char>(99);  // the 4-byte version field starts right after the magic
        const ImportResult result =
            importModel("bad.glb", "", asBytes(bad), ImportSettings{}, ImportDepth::Structure, {});
        CHECK(result.status == ImportStatus::ParseFailed);
        CHECK_FALSE(result.message.empty());
    }
    SUBCASE("an overrunning chunk length (MI64)") {
        std::string bad = validGlb;
        // The first chunk's length field is the 4 bytes right after the 12-byte header.
        bad[12] = static_cast<char>(0xFF);
        bad[13] = static_cast<char>(0xFF);
        bad[14] = static_cast<char>(0xFF);
        bad[15] = static_cast<char>(0x7F);
        const ImportResult result =
            importModel("bad.glb", "", asBytes(bad), ImportSettings{}, ImportDepth::Structure, {});
        CHECK(result.status == ImportStatus::ParseFailed);
        CHECK_FALSE(result.message.empty());
    }
    SUBCASE("trailing bytes appended after an otherwise-valid GLB (MI65)") {
        std::string bad = validGlb;
        bad += "trailing garbage that does not belong in this container";
        const ImportResult result =
            importModel("bad.glb", "", asBytes(bad), ImportSettings{}, ImportDepth::Structure, {});
        // fastgltf's own total-length field no longer matches the buffer it was given -- however it
        // reacts, it must not crash, and this case exists to confirm exactly that under ASan.
        CHECK((result.status == ImportStatus::ParseFailed || result.status == ImportStatus::Ok));
    }
}

TEST_CASE(
    "model_import: a primitive with POSITION+NORMAL+TEXCOORD_0 fills exactly those three "
    "vectors and reports exactly those three bits (MI66, AC-22)") {
    const std::string doc =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": )"
        R"({"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "mode": 4}]}], "accessors": )"
        R"([{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}, {"bufferView": 1, )"
        R"("componentType": 5126, "count": 3, "type": "VEC3"}, {"bufferView": 2, "componentType": 5126, )"
        R"("count": 3, "type": "VEC2"}, {"bufferView": 3, "componentType": 5123, "count": 3, "type": )"
        R"("SCALAR"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, )"
        R"("byteOffset": 36, "byteLength": 36}, {"buffer": 0, "byteOffset": 72, "byteLength": 24}, )"
        R"({"buffer": 0, "byteOffset": 96, "byteLength": 6}], "buffers": [{"byteLength": 102, "uri": )"
        R"("data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAA)"
        R"(AIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIA"}]})";
    const ImportResult result = importModel("attrs.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& prim = result.model.meshes[0].primitives[0];
    CHECK(prim.positions.size() == 3);
    CHECK(prim.normals.size() == 3);
    CHECK(prim.uv0.size() == 3);
    CHECK(prim.tangents.empty());
    CHECK(prim.uv1.empty());
    CHECK(prim.colors.empty());
    CHECK(prim.joints.empty());
    CHECK(prim.weights.empty());
    using engine::editor::has;
    using engine::editor::VertexAttribute;
    CHECK(has(prim.attributes, VertexAttribute::Position));
    CHECK(has(prim.attributes, VertexAttribute::Normal));
    CHECK(has(prim.attributes, VertexAttribute::TexCoord0));
    CHECK_FALSE(has(prim.attributes, VertexAttribute::Tangent));
    CHECK_FALSE(has(prim.attributes, VertexAttribute::Color0));
}

TEST_CASE("model_import: a non-indexed primitive synthesises 0..N-1 indices (MI67, AC-23, F6)") {
    const std::string doc =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, )"
        R"("mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": )"
        R"("VEC3"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}], "buffers": )"
        R"([{"byteLength": 36, "uri": "data:application/octet-stream;base64,)"
        R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]})";
    const ImportResult result =
        importModel("no-indices.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const std::vector<std::uint32_t> expected = {0, 1, 2};
    CHECK(result.model.meshes[0].primitives[0].indices == expected);
}

TEST_CASE(
    "model_import: UNSIGNED_BYTE, UNSIGNED_SHORT and UNSIGNED_INT indices all normalise to the "
    "same uint32_t values (MI68, AC-24)") {
    const std::string docU8 =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, )"
        R"("indices": 1, "mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, "count": )"
        R"(3, "type": "VEC3"}, {"bufferView": 1, "componentType": 5121, "count": 3, "type": "SCALAR"}], )"
        R"("bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": )"
        R"(36, "byteLength": 3}], "buffers": [{"byteLength": 39, "uri": )"
        R"("data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAEC"}]})";
    const std::string docU16 =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, )"
        R"("indices": 1, "mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, "count": )"
        R"(3, "type": "VEC3"}, {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}], )"
        R"("bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": )"
        R"(36, "byteLength": 6}], "buffers": [{"byteLength": 42, "uri": )"
        R"("data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA"}]})";
    const std::string docU32 =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, )"
        R"("indices": 1, "mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, "count": )"
        R"(3, "type": "VEC3"}, {"bufferView": 1, "componentType": 5125, "count": 3, "type": "SCALAR"}], )"
        R"("bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": )"
        R"(36, "byteLength": 12}], "buffers": [{"byteLength": 48, "uri": )"
        R"("data:application/octet-stream;base64,)"
        R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAEAAAACAAAA"}]})";

    const ImportResult r8 = importModel("u8.gltf", "", asBytes(docU8), ImportSettings{}, ImportDepth::Full, {});
    const ImportResult r16 = importModel("u16.gltf", "", asBytes(docU16), ImportSettings{}, ImportDepth::Full, {});
    const ImportResult r32 = importModel("u32.gltf", "", asBytes(docU32), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(r8.model.meshes.size() == 1);
    REQUIRE(r16.model.meshes.size() == 1);
    REQUIRE(r32.model.meshes.size() == 1);
    const std::vector<std::uint32_t> expected = {0, 1, 2};
    CHECK(r8.model.meshes[0].primitives[0].indices == expected);
    CHECK(r16.model.meshes[0].primitives[0].indices == expected);
    CHECK(r32.model.meshes[0].primitives[0].indices == expected);
}

TEST_CASE(
    "model_import: TRIANGLE_STRIP/LINES/POINTS primitives are each skipped with a warning; the "
    "mesh survives as an empty mesh with a point AABB (MI69, AC-25, D11)") {
    const std::string doc =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, )"
        R"("mode": 5}, {"attributes": {"POSITION": 0}, "mode": 1}, {"attributes": {"POSITION": 0}, )"
        R"("mode": 0}]}], "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": )"
        R"("VEC3"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}], "buffers": )"
        R"([{"byteLength": 36, "uri": "data:application/octet-stream;base64,)"
        R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]})";
    const ImportResult result =
        importModel("mixed-modes.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    CHECK(result.model.meshes[0].primitives.empty());
    // D11's point AABB: the struct's own untouched default (min == max == the origin), never the
    // invalid Aabb::empty() sentinel -- nothing ever called .expand() on it.
    CHECK(result.model.meshes[0].bounds.min.x == doctest::Approx(0.0F));
    CHECK(result.model.meshes[0].bounds.max.x == doctest::Approx(0.0F));
    CHECK(result.warningTotal == 3);
    for (const std::string& w : result.warnings) {
        CHECK(w.find("mesh") != std::string::npos);
        CHECK(w.find("is not imported") != std::string::npos);
    }
}

TEST_CASE(
    "model_import: a normalised u8vec4 COLOR_0 de-normalises to floats; a VEC3 COLOR_0 widens "
    "with a = 1 (MI70)") {
    const std::string docU8 =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0, )"
        R"("COLOR_0": 1}, "mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, )"
        R"("count": 3, "type": "VEC3"}, {"bufferView": 1, "componentType": 5121, "count": 3, "type": )"
        R"("VEC4", "normalized": true}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": )"
        R"(36}, {"buffer": 0, "byteOffset": 36, "byteLength": 12}], "buffers": [{"byteLength": 48, )"
        R"("uri": "data:application/octet-stream;base64,)"
        R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA/wAA/wD/AP8AAP//"}]})";
    const std::string docVec3 =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0, )"
        R"("COLOR_0": 1}, "mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, )"
        R"("count": 3, "type": "VEC3"}, {"bufferView": 1, "componentType": 5126, "count": 3, "type": )"
        R"("VEC3"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, )"
        R"("byteOffset": 36, "byteLength": 36}], "buffers": [{"byteLength": 72, "uri": )"
        R"("data:application/octet-stream;base64,)"
        R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"}]})";

    const ImportResult r8 = importModel("color-u8.gltf", "", asBytes(docU8), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(r8.model.meshes.size() == 1);
    REQUIRE(r8.model.meshes[0].primitives.size() == 1);
    REQUIRE(r8.model.meshes[0].primitives[0].colors.size() == 3);
    CHECK(r8.model.meshes[0].primitives[0].colors[0].x == doctest::Approx(1.0F));
    CHECK(r8.model.meshes[0].primitives[0].colors[0].y == doctest::Approx(0.0F));
    CHECK(r8.model.meshes[0].primitives[0].colors[0].w == doctest::Approx(1.0F));

    const ImportResult rv3 =
        importModel("color-vec3.gltf", "", asBytes(docVec3), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(rv3.model.meshes.size() == 1);
    REQUIRE(rv3.model.meshes[0].primitives.size() == 1);
    REQUIRE(rv3.model.meshes[0].primitives[0].colors.size() == 3);
    CHECK(rv3.model.meshes[0].primitives[0].colors[0].x == doctest::Approx(1.0F));
    CHECK(rv3.model.meshes[0].primitives[0].colors[0].w == doctest::Approx(1.0F));  // widened, a = 1
}

TEST_CASE(
    "model_import: JOINTS_0 as UNSIGNED_BYTE and as UNSIGNED_SHORT produce identical "
    "std::array<uint16_t,4> values (MI71)") {
    const std::string docU8 =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0, )"
        R"("JOINTS_0": 1}, "mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, )"
        R"("count": 3, "type": "VEC3"}, {"bufferView": 1, "componentType": 5121, "count": 3, "type": )"
        R"("VEC4"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, )"
        R"("byteOffset": 36, "byteLength": 12}], "buffers": [{"byteLength": 48, "uri": )"
        R"("data:application/octet-stream;base64,)"
        R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAECAwECAwACAwAB"}]})";
    const std::string docU16 =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0, )"
        R"("JOINTS_0": 1}, "mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, )"
        R"("count": 3, "type": "VEC3"}, {"bufferView": 1, "componentType": 5123, "count": 3, "type": )"
        R"("VEC4"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, )"
        R"("byteOffset": 36, "byteLength": 24}], "buffers": [{"byteLength": 60, "uri": )"
        R"("data:application/octet-stream;base64,)"
        R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAwABAAIAAwAAAAIAAwAAAAEA"}]})";

    const ImportResult r8 = importModel("joints-u8.gltf", "", asBytes(docU8), ImportSettings{}, ImportDepth::Full, {});
    const ImportResult r16 =
        importModel("joints-u16.gltf", "", asBytes(docU16), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(r8.model.meshes.size() == 1);
    REQUIRE(r16.model.meshes.size() == 1);
    REQUIRE(r8.model.meshes[0].primitives[0].joints.size() == 3);
    REQUIRE(r16.model.meshes[0].primitives[0].joints.size() == 3);
    CHECK(r8.model.meshes[0].primitives[0].joints == r16.model.meshes[0].primitives[0].joints);
    const std::array<std::uint16_t, 4> firstExpected = {0, 1, 2, 3};
    CHECK(r8.model.meshes[0].primitives[0].joints[0] == firstExpected);
}

TEST_CASE(
    "model_import: settings.scale scales positions and the AABB; normals do not (MI72, AC-30, "
    "sabotage S14's discriminator)") {
    const std::string doc =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0, )"
        R"("NORMAL": 1}, "mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, )"
        R"("count": 3, "type": "VEC3"}, {"bufferView": 1, "componentType": 5126, "count": 3, "type": )"
        R"("VEC3"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, )"
        R"("byteOffset": 36, "byteLength": 36}], "buffers": [{"byteLength": 72, "uri": )"
        R"("data:application/octet-stream;base64,)"
        R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/)"
        R"("}]})";
    ImportSettings settings;
    settings.scale = 3.0F;
    const ImportResult result = importModel("scale-normal.gltf", "", asBytes(doc), settings, ImportDepth::Full, {});
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& prim = result.model.meshes[0].primitives[0];
    REQUIRE(prim.positions.size() == 3);
    CHECK(prim.positions[1].x == doctest::Approx(3.0F));  // (1,0,0) * 3
    CHECK(prim.bounds.max.x == doctest::Approx(3.0F));
    REQUIRE(prim.normals.size() == 3);
    CHECK(prim.normals[0].z == doctest::Approx(1.0F));  // UNSCALED -- would be 3.0 if the bug is present
}

TEST_CASE(
    "model_import: a negative settings.scale is honoured; the AABB min/max are re-ordered "
    "after scaling, never assumed (MI73, E13)") {
    const std::string doc =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, )"
        R"("mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": )"
        R"("VEC3"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}], "buffers": )"
        R"([{"byteLength": 36, "uri": "data:application/octet-stream;base64,)"
        R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]})";
    ImportSettings settings;
    settings.scale = -2.0F;
    const ImportResult result = importModel("neg-scale.gltf", "", asBytes(doc), settings, ImportDepth::Full, {});
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& prim = result.model.meshes[0].primitives[0];
    REQUIRE(prim.positions.size() == 3);
    CHECK(prim.positions[1].x == doctest::Approx(-2.0F));  // (1,0,0) * -2
    // min.x must be the SMALLER value even though it came from a negated coordinate.
    CHECK(prim.bounds.min.x == doctest::Approx(-2.0F));
    CHECK(prim.bounds.max.x == doctest::Approx(0.0F));
    CHECK(prim.bounds.min.x <= prim.bounds.max.x);
    CHECK(prim.bounds.min.y <= prim.bounds.max.y);
}

TEST_CASE(
    "model_import: materials.gltf's factors, scales, alpha mode/cutoff and doubleSided all "
    "round-trip (MI74, AC-26)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MATERIALS_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("materials.gltf", "models", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.model.materials.size() == 1);
    const auto& mat = result.model.materials[0];
    CHECK(mat.name == "TestMaterial");
    CHECK(mat.baseColorFactor.x == doctest::Approx(0.8F));
    CHECK(mat.baseColorFactor.y == doctest::Approx(0.2F));
    CHECK(mat.baseColorFactor.z == doctest::Approx(0.1F));
    CHECK(mat.baseColorFactor.w == doctest::Approx(0.9F));
    CHECK(mat.metallicFactor == doctest::Approx(0.3F));
    CHECK(mat.roughnessFactor == doctest::Approx(0.6F));
    CHECK(mat.emissiveFactor.x == doctest::Approx(0.1F));
    CHECK(mat.emissiveFactor.y == doctest::Approx(0.2F));
    CHECK(mat.emissiveFactor.z == doctest::Approx(0.3F));
    CHECK(mat.normalScale == doctest::Approx(2.0F));
    CHECK(mat.occlusionStrength == doctest::Approx(0.5F));
    CHECK(mat.alphaMode == engine::editor::AlphaMode::Mask);
    CHECK(mat.alphaCutoff == doctest::Approx(0.4F));
    CHECK(mat.doubleSided);
}

TEST_CASE(
    "model_import: materials.gltf's five texture slots resolve to the right image index and "
    "TEXCOORD_n (MI75, AC-27, plan A10)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MATERIALS_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("materials.gltf", "models", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.model.materials.size() == 1);
    const auto& mat = result.model.materials[0];
    REQUIRE(mat.baseColor.has_value());
    CHECK(mat.baseColor->imageIndex == 0);
    CHECK(mat.baseColor->uvSet == 0);
    REQUIRE(mat.metallicRoughness.has_value());
    CHECK(mat.metallicRoughness->imageIndex == 1);
    CHECK(mat.metallicRoughness->uvSet == 1);
    REQUIRE(mat.normal.has_value());
    CHECK(mat.normal->imageIndex == 2);
    REQUIRE(mat.occlusion.has_value());
    CHECK(mat.occlusion->imageIndex == 3);
    REQUIRE(mat.emissive.has_value());
    CHECK(mat.emissive->imageIndex == 4);
}

TEST_CASE(
    "model_import: materials.gltf's samplers map wrap/filter correctly; the absent sampler "
    "(emissive) yields repeat/repeat/linear (MI76, AC-28)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MATERIALS_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("materials.gltf", "models", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.model.materials.size() == 1);
    const auto& mat = result.model.materials[0];
    using engine::editor::MipFilter;
    using engine::editor::TextureFilter;
    using engine::editor::TextureWrap;
    REQUIRE(mat.baseColor.has_value());  // sampler 0: repeat/repeat, nearest/nearest-mipmap-nearest
    CHECK(mat.baseColor->wrapU == TextureWrap::Repeat);
    CHECK(mat.baseColor->wrapV == TextureWrap::Repeat);
    CHECK(mat.baseColor->magFilter == TextureFilter::Nearest);
    CHECK(mat.baseColor->minFilter == TextureFilter::Nearest);
    CHECK(mat.baseColor->mipFilter == MipFilter::Nearest);
    REQUIRE(mat.metallicRoughness.has_value());  // sampler 1: clamp/mirrored, linear/linear-mipmap-linear
    CHECK(mat.metallicRoughness->wrapU == TextureWrap::ClampToEdge);
    CHECK(mat.metallicRoughness->wrapV == TextureWrap::MirroredRepeat);
    CHECK(mat.metallicRoughness->magFilter == TextureFilter::Linear);
    CHECK(mat.metallicRoughness->mipFilter == MipFilter::Linear);
    REQUIRE(mat.emissive.has_value());  // texture 4 has NO sampler -- every default in place
    CHECK(mat.emissive->wrapU == TextureWrap::Repeat);
    CHECK(mat.emissive->wrapV == TextureWrap::Repeat);
    CHECK(mat.emissive->magFilter == TextureFilter::Linear);
    CHECK(mat.emissive->minFilter == TextureFilter::Linear);
    CHECK(mat.emissive->mipFilter == MipFilter::Linear);
}

TEST_CASE(
    "model_import: settings.importMaterials == false yields zero materials, every "
    "materialIndex INVALID_SUBASSET, and leaves externalUris unchanged (MI77, AC-29)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(MATERIALS_FIXTURE);
    REQUIRE(fixture.ok);
    ImportSettings settings;
    settings.importMaterials = false;
    const ImportResult withMaterials =
        importModel("materials.gltf", "models", asBytes(fixture.text), ImportSettings{}, ImportDepth::Structure, {});
    const ImportResult withoutMaterials =
        importModel("materials.gltf", "models", asBytes(fixture.text), settings, ImportDepth::Structure, {});
    CHECK(withoutMaterials.model.materials.empty());
    CHECK(withoutMaterials.model.summary.materialCount == 0);
    CHECK(withoutMaterials.externalUris == withMaterials.externalUris);  // AC-29's critical half
    CHECK_FALSE(withMaterials.externalUris.empty());
}

TEST_CASE(
    "model_import: MAX_VERTICES_PER_MODEL and MAX_INDICES_PER_MODEL truncate a document whose "
    "accessor.count claims the excess WITHOUT providing the bytes, and complete quickly (MI78, "
    "AC-42, D15, sabotage S20's discriminator)") {
    SUBCASE("vertex cap") {
        const std::string doc =
            R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": )"
            R"(0}, "mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, "count": )"
            R"(8000001, "type": "VEC3"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": )"
            R"(12}], "buffers": [{"byteLength": 12, "uri": )"
            R"("data:application/octet-stream;base64,AAAAAAAAAAAAAAAA"}]})";
        const ImportResult result =
            importModel("huge-vertex-count.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
        CHECK(result.status == ImportStatus::Truncated);
        CHECK_FALSE(result.message.empty());
        REQUIRE(result.model.meshes.size() == 1);
        CHECK(result.model.meshes[0].primitives.empty());
    }
    SUBCASE("index cap") {
        const std::string doc =
            R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": )"
            R"(0}, "indices": 1, "mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, )"
            R"("count": 3, "type": "VEC3"}, {"bufferView": 1, "componentType": 5123, "count": )"
            R"(24000001, "type": "SCALAR"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, )"
            R"("byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 2}], "buffers": )"
            R"([{"byteLength": 38, "uri": "data:application/octet-stream;base64,)"
            R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAA="}]})";
        const ImportResult result =
            importModel("huge-index-count.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
        CHECK(result.status == ImportStatus::Truncated);
        CHECK_FALSE(result.message.empty());
        REQUIRE(result.model.meshes.size() == 1);
        CHECK(result.model.meshes[0].primitives.empty());
    }
}

TEST_CASE(
    "model_import: a wrong-typed accessor, one with no bufferView, and one whose byteOffset "
    "overruns its view are each skipped with a warning -- no assert fires and no out-of-bounds "
    "read occurs (MI79, plan §A-4/§A-5, run under ASan)") {
    SUBCASE("wrong type (SCALAR declared for POSITION)") {
        const std::string doc =
            R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": )"
            R"(0}, "mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, )"
            R"("type": "SCALAR"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}], )"
            R"("buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,)"
            R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]})";
        const ImportResult result =
            importModel("wrong-type.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
        REQUIRE(result.model.meshes.size() == 1);
        CHECK(result.model.meshes[0].primitives.empty());
        CHECK_FALSE(result.warnings.empty());
    }
    SUBCASE("no bufferView at all") {
        const std::string doc =
            R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": )"
            R"(0}, "mode": 4}]}], "accessors": [{"componentType": 5126, "count": 3, "type": "VEC3"}]})";
        const ImportResult result =
            importModel("no-bufferview.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
        REQUIRE(result.model.meshes.size() == 1);
        CHECK(result.model.meshes[0].primitives.empty());
        CHECK_FALSE(result.warnings.empty());
    }
    SUBCASE("byteOffset overruns its bufferView") {
        const std::string doc =
            R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": )"
            R"(0}, "mode": 4}]}], "accessors": [{"bufferView": 0, "byteOffset": 1000, "componentType": )"
            R"(5126, "count": 3, "type": "VEC3"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, )"
            R"("byteLength": 36}], "buffers": [{"byteLength": 36, "uri": )"
            R"("data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]})";
        const ImportResult result =
            importModel("overrun.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
        REQUIRE(result.model.meshes.size() == 1);
        CHECK(result.model.meshes[0].primitives.empty());
        CHECK_FALSE(result.warnings.empty());
    }
}

TEST_CASE(
    "model_import: asymmetric.gltf at Full depth -- the triangle's positions and winding match "
    "the source exactly (MI40b, AC-30b, F7b's pin, sabotage S29/S30's discriminator)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(ASYMMETRIC_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("asymmetric.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.nodes.size() == 1);
    // The TRS third, re-asserted at Full depth (MI57 asserted it at Structure depth).
    const auto& node = result.model.nodes[0];
    CHECK(node.translation.x == doctest::Approx(1.0F));
    CHECK(node.translation.y == doctest::Approx(2.0F));
    CHECK(node.translation.z == doctest::Approx(3.0F));
    CHECK(node.rotation.x == doctest::Approx(0.1601281464099884).epsilon(0.0001));
    CHECK(node.rotation.w == doctest::Approx(0.8006407618522644).epsilon(0.0001));
    CHECK(node.scale.x == doctest::Approx(2.0F));
    CHECK(node.scale.y == doctest::Approx(3.0F));
    CHECK(node.scale.z == doctest::Approx(4.0F));

    // The asymmetric triangle: exactly the source's values, component for component, in the source's
    // OWN winding order (0, 1, 2 -- as recorded in the fixture). A mirrored, transposed or reordered
    // import fails on the FIRST component that differs.
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& prim = result.model.meshes[0].primitives[0];
    REQUIRE(prim.positions.size() == 3);
    CHECK(prim.positions[0].x == doctest::Approx(0.10000000149011612).epsilon(0.0000001));
    CHECK(prim.positions[0].y == doctest::Approx(0.20000000298023224).epsilon(0.0000001));
    CHECK(prim.positions[0].z == doctest::Approx(0.30000001192092896).epsilon(0.0000001));
    CHECK(prim.positions[1].x == doctest::Approx(0.4000000059604645).epsilon(0.0000001));
    CHECK(prim.positions[1].y == doctest::Approx(0.6000000238418579).epsilon(0.0000001));
    CHECK(prim.positions[1].z == doctest::Approx(0.5).epsilon(0.0000001));
    CHECK(prim.positions[2].x == doctest::Approx(0.8999999761581421).epsilon(0.0000001));
    CHECK(prim.positions[2].y == doctest::Approx(0.699999988079071).epsilon(0.0000001));
    CHECK(prim.positions[2].z == doctest::Approx(0.800000011920929).epsilon(0.0000001));
    REQUIRE(prim.indices.size() == 3);
    CHECK(prim.indices[0] == 0);
    CHECK(prim.indices[1] == 1);
    CHECK(prim.indices[2] == 2);
}

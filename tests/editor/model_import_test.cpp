// tests/editor/model_import_test.cpp -- task 3.2.1: the canonical imported-model types, the pure URI
// policy (classifyUri/normalizeRelativePath/isImportableModelName), importModel's dispatch, and (from
// Step 4 onward) the glTF backend itself. A TU of aero_editor_shell_test, which supplies main() from
// shell_test.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, tier-0 (D4/AC-17/INV-P5, the asset_meta_test.cpp precedent): model_import.hpp depends only
// on aero/core/{guid,math}.hpp, aero/editor/import_settings.hpp and aero/editor/scene_bounds.hpp -- the
// last of those reaches aero::scene, which is a PUBLIC, UNGATED dependency of aero_editor_core (only
// engine/scene_serialize is gated on AERO_REFLECT_TOOLS) -- so every case in this file must be PRESENT
// and PASSING in all three build configurations. No GPU, no window, no ImGui context, no sleeps: every
// importer case is driven from a string literal or a committed text fixture.
//
// THREE cases here deliberately touch disk, and they are the only ones: MI42b needs a scratch working
// directory containing a real file to prove the importer does NOT read it (a read that never succeeds
// is a read no assertion can see), and MI42c/MI42d read editor/src/gltf_import.cpp's own source text.
// Everything else stays literal-driven.
#include <aero/editor/model_import.hpp>
#include <aero/editor/text_file.hpp>  // MI42c/MI42d only: readTextFile over gltf_import.cpp's own text

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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
constexpr std::string_view SKINNED_FIXTURE = AERO_ASSET_FIXTURES_DIR "/skinned.gltf";

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

// MI42b only. A unique scratch directory that removes itself on destruction -- the same shape every
// other TU in this suite keeps its own copy of (asset_database_test.cpp:53's precedent; scaffolding is
// copied, the ASSERTION is shared).
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_model_import_test_" + std::to_string(++counter));
        std::filesystem::remove_all(dirPath, ec);
        std::filesystem::create_directories(dirPath, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dirPath, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return dirPath; }

private:
    std::filesystem::path dirPath;
};

// MI42b only. project_test.cpp's P84/P85 helper, verbatim in shape: the process working directory is
// global state, so it is restored by a destructor that runs on a thrown REQUIRE as well as on success.
class ScopedCwd {
public:
    explicit ScopedCwd(const std::filesystem::path& to) : previous(std::filesystem::current_path()) {
        std::filesystem::current_path(to);
    }
    ~ScopedCwd() {
        std::error_code ec;
        std::filesystem::current_path(previous, ec);
    }
    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;
    ScopedCwd(ScopedCwd&&) = delete;
    ScopedCwd& operator=(ScopedCwd&&) = delete;

private:
    std::filesystem::path previous;
};

// MI42c/MI42d. One source line with its `//` comment removed -- the I60 rule, restated here because
// gltf_import.cpp's own prose legitimately NAMES every forbidden token it must never USE (its header
// comment lists LoadExternalBuffers, LoadExternalImages, FromPath and <filesystem> by name, and the
// most important of those comments is the one documenting this very invariant). A check that cannot
// tell code from comment would fail because somebody documented it correctly.
[[nodiscard]] std::string_view codeOf(std::string_view line) {
    const std::size_t commentStart = line.find("//");
    return commentStart == std::string_view::npos ? line : line.substr(0, commentStart);
}

[[nodiscard]] std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::string_view remaining = text;
    while (true) {
        const std::size_t newline = remaining.find('\n');
        if (newline == std::string_view::npos) {
            lines.push_back(remaining);
            break;
        }
        lines.push_back(remaining.substr(0, newline));
        remaining.remove_prefix(newline + 1U);
    }
    return lines;
}

}  // namespace

using engine::editor::ASSIMP_IMPORTER_NAME;
using engine::editor::ASSIMP_IMPORTER_VERSION;
using engine::editor::classifyUri;
using engine::editor::FBX_IMPORTER_NAME;
using engine::editor::FBX_IMPORTER_VERSION;
using engine::editor::foldBackslashesToSlashes;
using engine::editor::GLTF_IMPORTER_NAME;
using engine::editor::GLTF_IMPORTER_VERSION;
using engine::editor::has;
using engine::editor::ImportDepth;
using engine::editor::ImporterIdentity;
using engine::editor::importModel;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;
using engine::editor::importStatusLabel;
using engine::editor::isImportableModelName;
using engine::editor::looksLikeBinaryContent;
using engine::editor::modelImporterIdentity;
using engine::editor::modelImporterNeedsExternalBuffers;
using engine::editor::normalizeRelativePath;
using engine::editor::OBJ_IMPORTER_NAME;
using engine::editor::OBJ_IMPORTER_VERSION;
using engine::editor::ObjMtlLibScan;
using engine::editor::plyDeclaredCountsExceedBytes;
using engine::editor::scanColladaAssetSpace;
using engine::editor::scanObjMtlLibs;
using engine::editor::scanObjMtlLibsScan;
using engine::editor::scanPlyTextureFiles;
using engine::editor::SourceSpace;
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
    // ".fbx" moved OUT of this list at task 3.2.2 -- MI103 now covers its acceptance, the mirror of
    // this case's rejection. ".obj"/".mtl" moved out at task 3.2.3 for the identical reason -- MI118+
    // now covers their acceptance. ".blend" is 3.2.4's and STAYS unclaimed AFTER it, BY DESIGN
    // (3.2.4 D15) -- 3.2.4 converts a .blend to a GLB above importModel rather than teaching this
    // table a sixth extension. MI133 pins that, together with modelImporterNeedsExternalBuffers.
    CHECK_FALSE(isImportableModelName("a.blend"));
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
    // ".fbx" was this case's example until task 3.2.2 taught the dispatch that extension, then ".obj"
    // took its place until task 3.2.3 taught the dispatch THAT extension too. ".blend" is 3.2.4's, so
    // this file name is still unclaimed by any importer today.
    const ImportResult result = importModel("chair.blend", "models", {}, ImportSettings{}, ImportDepth::Structure, {});
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
    // ".fbx" was this case's non-model example until task 3.2.2, then ".obj" until task 3.2.3; ".blend"
    // (still unclaimed today, 3.2.4's) takes its place so this stays a genuine non-model-name case.
    // OI28 drives the SAME {nullptr, 0} span through a CLAIMED ".obj" name -- the OBJ arm must survive
    // a null span too, and that is now a reachable input nothing else covers.
    const std::span<const std::byte> emptySpan;  // {nullptr, 0} -- any dereference would crash/ASan-trip
    const ImportResult result = importModel("model.blend", "", emptySpan, ImportSettings{}, ImportDepth::Full, {});
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
    "model_import: more than MAX_IMPORT_WARNINGS refusals cap `warnings` while `warningTotal` keeps "
    "the true count (MI44b, D15, seed S27)") {
    // The sabotage matrix's own gap, closed: BEFORE this case, `git grep MAX_IMPORT_WARNINGS -- tests/`
    // was EMPTY and the highest warningTotal any case reached was 4 (MI40), against a cap of 20 -- so
    // seed S27 (dropping addWarning's `warnings.size() < MAX_IMPORT_WARNINGS` guard) reddened nothing
    // anywhere in the suite. A batch of refused image URIs is the cheapest warning source there is:
    // each costs one classifyUri refusal and contributes NO externalUris entry, so no other cap
    // (MAX_EXTERNAL_URIS in particular) interferes with the count.
    constexpr std::size_t REFUSAL_COUNT = engine::editor::MAX_IMPORT_WARNINGS + 5U;  // 25 -- comfortably over
    std::string doc = R"({"asset":{"version":"2.0"},"images":[)";
    for (std::size_t i = 0; i < REFUSAL_COUNT; ++i) {
        if (i != 0) {
            doc += ',';
        }
        doc += std::format(R"({{"uri":"http://evil.example/x{}.png"}})", i);
    }
    doc += "]}";
    const ImportResult result =
        importModel("many-refusals.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});

    // A refusal is a WARNING, never a failure (AC-5) -- the import itself still succeeds.
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.images.size() == REFUSAL_COUNT);
    CHECK_FALSE(result.model.images.front().refusal.empty());
    CHECK_FALSE(result.model.images.back().refusal.empty());

    // THE TWO ASSERTIONS SEED S27 CANNOT SURVIVE: the list is capped, the total is not.
    CHECK(result.warnings.size() <= engine::editor::MAX_IMPORT_WARNINGS);
    CHECK(result.warnings.size() == engine::editor::MAX_IMPORT_WARNINGS);
    CHECK(result.warningTotal == REFUSAL_COUNT);

    // The cap keeps the EARLIEST warnings and drops the tail -- a cap that kept the last 20 instead
    // would satisfy both size assertions above while silently discarding the first thing that went
    // wrong, which is the one a reader needs most.
    REQUIRE(result.warnings.size() == engine::editor::MAX_IMPORT_WARNINGS);
    CHECK(result.warnings.front().find("image 0 ") != std::string::npos);
    CHECK(result.warnings.back().find("image 19 ") != std::string::npos);
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

TEST_CASE(
    "model_import: an over-cap embedded BUFFER is refused, not merely escalated -- no accessor may "
    "still read through it (MI46b, code-review NIT 12)") {
    // Before the fix, an embedded buffer's own over-cap check (phase 3) escalated the result to
    // Truncated but left the DATA fully readable -- nothing stopped a later accessor from reading
    // straight through it. Unlike MI46's bufferView-declared-size trick, the buffer's OWN materialised
    // byte count is what this checks, and fastgltf validates a GLB BIN chunk's length against the real
    // container size at parse time -- there is no shortcut, so this is a real MAX_EMBEDDED_BYTES + 1
    // byte payload. Raw GLB bytes, not base64, to avoid a further third of that size in text inflation.
    std::string json = R"({"asset":{"version":"2.0"},)"
                       R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4}]}],)"
                       R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
                       R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)";
    json += std::format(R"("buffers":[{{"byteLength":{}}}]}})", engine::editor::MAX_EMBEDDED_BYTES + 1);
    const std::string bin(static_cast<std::size_t>(engine::editor::MAX_EMBEDDED_BYTES) + 1, '\0');
    const std::string glb = buildGlb(json, bin);
    const ImportResult result =
        importModel("huge-embedded-buffer.glb", "", asBytes(glb), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Truncated);
    CHECK_FALSE(result.message.empty());
    REQUIRE(result.model.meshes.size() == 1);
    CHECK(result.model.meshes[0].primitives.empty());  // POSITION refused -- the buffer must not be read
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

// The document MI42 and MI42b share: one triangle whose only buffer is an EXTERNAL "external.bin".
constexpr std::string_view NEEDS_EXTERNAL_DOC =
    R"({"asset":{"version":"2.0"},)"
    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4}]}],)"
    R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
    R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
    R"("buffers":[{"byteLength":36,"uri":"external.bin"}]})";

// TITLE CORRECTED (this case's claim was measurably too strong, and the overclaim is what let seed S26
// through). MI42 detects a read only if that read SUCCEEDS, and nothing here controls the working
// directory the read would resolve against -- with an `external.bin` present in the process CWD and
// S26 applied, MI42 goes red; with it absent, which is every real run, MI42 passes against a
// file-reading importer just as happily as against a correct one. MI42b below is the deterministic
// version; this case is kept because the no-externals-supplied path is still worth asserting on its
// own, not because it proves AC-39.
TEST_CASE(
    "model_import: a Full import with zero supplied externals reports MissingBuffer (MI42, AC-39, "
    "see MI42b for the CWD-independent proof)") {
    const std::string doc(NEEDS_EXTERNAL_DOC);
    const ImportResult result =
        importModel("needs-external.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::MissingBuffer);
}

TEST_CASE(
    "model_import: a Full import does not read the named buffer even when it IS on disk in the "
    "working directory (MI42b, AC-39, INV-M3, seed S26)") {
    // THE GAP THIS CLOSES. AC-39's real claim is "a refused or unsupplied URI never causes a file
    // read", and an absence cannot be observed: a read that fails is indistinguishable from a read
    // that never happened. The only way to make the read OBSERVABLE is to guarantee it would SUCCEED,
    // which means putting a valid `external.bin` exactly where a naive implementation would look for
    // it -- the process working directory, since importModel is handed no directory at all and
    // fastgltf is handed `{}`.
    //
    // With those 36 bytes present, an importer that opens the file gets three valid VEC3 positions and
    // reports Ok; the real importer, which is handed an EMPTY external-buffer span, still reports
    // MissingBuffer. The CWD is created, populated and restored by this case, so the verdict does not
    // depend on what happens to be sitting in the directory the test binary was launched from.
    const TempDir dir;
    {
        std::ofstream out(dir.path() / "external.bin", std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(out));
        const std::string bytes(36, '\0');  // 3 * VEC3 of float zeroes -- a VALID buffer for this doc
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    const ScopedCwd cwd{dir.path()};
    std::error_code ec;
    REQUIRE(std::filesystem::exists(std::filesystem::path("external.bin"), ec));  // relative == the CWD copy
    REQUIRE_FALSE(ec);
    REQUIRE(std::filesystem::file_size(std::filesystem::path("external.bin"), ec) == 36U);
    REQUIRE_FALSE(ec);

    const std::string doc(NEEDS_EXTERNAL_DOC);
    const ImportResult result =
        importModel("needs-external.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});

    CHECK(result.status == ImportStatus::MissingBuffer);
    // ... and the geometry is genuinely absent, not merely flagged: the primitive was skipped because
    // its POSITION accessor had no backing bytes. Under seed S26 this vector holds one primitive with
    // three positions in it.
    REQUIRE(result.model.meshes.size() == 1);
    CHECK(result.model.meshes[0].primitives.empty());
    CHECK(result.model.summary.vertexCount == 0);
}

TEST_CASE(
    "model_import: gltf_import.cpp performs NO file operation, in its own source text (MI42c, AC-39, "
    "INV-M3, seed S26)") {
    // §V6's "gltf_import.cpp performs NO FILE OPERATION" grep, moved from a manual gate in the plan
    // into the automated suite. It is deliberately NOT a seventh entry in .github/scripts/ -- AC-58
    // requires that directory to stay byte-identical on this branch -- so it lands here instead, the
    // PU1/I60 shape's third instance: a mechanical proof over ONE file's own text, run as a doctest
    // case because what is checked is that file's text, not a repo-wide invariant needing git ls-files.
    //
    // This is the half MI42b cannot cover: MI42b proves the importer does not read through the ONE
    // path a test can construct (a CWD-relative name), while this case refuses the whole class --
    // every stream, every <filesystem> call, every editor byte primitive, resolved against any path.
    constexpr std::string_view SOURCE_PATH = AERO_EDITOR_SRC_DIR "/gltf_import.cpp";
    const engine::editor::FileReadResult read = engine::editor::readTextFile(SOURCE_PATH);
    REQUIRE(read.text.has_value());
    const std::string& text = *read.text;
    REQUIRE_FALSE(text.empty());
    const std::vector<std::string_view> lines = splitLines(text);
    REQUIRE(lines.size() > 100U);  // the file really was read, not silently truncated to nothing

    // Every token below is a REAL file operation, and every one of them appears in this file's own
    // COMMENTS -- hence codeOf(). `fastgltf` deliberately is NOT in this list: it is the one library
    // this TU exists to hold.
    constexpr std::array<std::string_view, 9> FORBIDDEN = {
        "std::filesystem::", "<fstream>",           "ifstream",   "ofstream", "fopen", "readTextFile",
        "readFileBytes",     "writeTextFileAtomic", "std::fopen",
    };
    for (const std::string_view token : FORBIDDEN) {
        std::size_t hits = 0;
        for (const std::string_view line : lines) {
            if (codeOf(line).find(token) != std::string_view::npos) {
                ++hits;
            }
        }
        INFO("forbidden file-operation token in gltf_import.cpp: ", token);
        CHECK(hits == 0);
    }
}

TEST_CASE(
    "model_import: gltf_import.cpp never names fastgltf's own file-loading surface, and GLTF_OPTIONS "
    "carries exactly two bits (MI42d, AC-56)") {
    // §V6's AC-56 grep, the other half of the same manual gate, likewise moved into the suite rather
    // than into .github/scripts/ (AC-58). The POSITIVE assertion is the stronger of the two: if the
    // option set really is exactly those two bits, then no LoadExternal* bit can be set no matter what
    // any other line says.
    constexpr std::string_view SOURCE_PATH = AERO_EDITOR_SRC_DIR "/gltf_import.cpp";
    const engine::editor::FileReadResult read = engine::editor::readTextFile(SOURCE_PATH);
    REQUIRE(read.text.has_value());
    const std::vector<std::string_view> lines = splitLines(*read.text);
    REQUIRE(lines.size() > 100U);

    constexpr std::array<std::string_view, 5> FORBIDDEN = {
        "GltfDataBuffer::FromPath", "MappedGltfFile", "GltfFileStream", "LoadExternalBuffers", "LoadExternalImages",
    };
    for (const std::string_view token : FORBIDDEN) {
        std::size_t hits = 0;
        for (const std::string_view line : lines) {
            if (codeOf(line).find(token) != std::string_view::npos) {
                ++hits;
            }
        }
        INFO("forbidden fastgltf loading-surface token in gltf_import.cpp: ", token);
        CHECK(hits == 0);
    }

    // The positive half: GLTF_OPTIONS is assigned ONCE, and its two bits are the allowed two.
    std::size_t assignments = 0;
    std::string optionText;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view code = codeOf(lines[i]);
        if (code.find("GLTF_OPTIONS =") == std::string_view::npos) {
            continue;
        }
        ++assignments;
        optionText.clear();
        for (std::size_t j = i; j < lines.size(); ++j) {
            optionText += codeOf(lines[j]);
            if (codeOf(lines[j]).find(';') != std::string_view::npos) {
                break;
            }
        }
    }
    REQUIRE(assignments == 1);
    CHECK(optionText.find("Options::DecomposeNodeMatrices") != std::string::npos);
    CHECK(optionText.find("Options::GenerateMeshIndices") != std::string::npos);
    CHECK(optionText.find("Options::Load") == std::string::npos);  // no LoadExternal*, no LoadGLBBuffers
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
    "model_import: an index count with no complete triangle is skipped, never survives with empty "
    "indices (MI94, code-review SHOULD-FIX 6, INV-M5)") {
    SUBCASE("2 indices truncate to 0 -- the primitive is skipped") {
        const std::string doc =
            R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": )"
            R"(0}, "indices": 1, "mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, )"
            R"("count": 3, "type": "VEC3"}, {"bufferView": 1, "componentType": 5123, "count": 2, )"
            R"("type": "SCALAR"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, )"
            R"({"buffer": 0, "byteOffset": 36, "byteLength": 4}], "buffers": [{"byteLength": 40, "uri": )"
            R"("data:application/octet-stream;base64,)"
            R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAA=="}]})";
        const ImportResult result =
            importModel("two-indices.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
        CHECK(result.status == ImportStatus::Ok);
        REQUIRE(result.model.meshes.size() == 1);
        // INV-M5: positions and indices are ALWAYS non-empty on a SURVIVING primitive -- a primitive
        // with no complete triangle at all must not survive with an empty `indices` instead.
        CHECK(result.model.meshes[0].primitives.empty());
        CHECK_FALSE(result.warnings.empty());
    }
    SUBCASE("4 indices truncate to 3 -- the primitive survives, truncated") {
        const std::string doc =
            R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": )"
            R"(0}, "indices": 1, "mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, )"
            R"("count": 3, "type": "VEC3"}, {"bufferView": 1, "componentType": 5123, "count": 4, )"
            R"("type": "SCALAR"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, )"
            R"({"buffer": 0, "byteOffset": 36, "byteLength": 8}], "buffers": [{"byteLength": 44, "uri": )"
            R"("data:application/octet-stream;base64,)"
            R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAA="}]})";
        const ImportResult result =
            importModel("four-indices.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
        CHECK(result.status == ImportStatus::Ok);
        REQUIRE(result.model.meshes.size() == 1);
        REQUIRE(result.model.meshes[0].primitives.size() == 1);
        const std::vector<std::uint32_t> expected4 = {0, 1, 2};  // truncated to a multiple of 3
        CHECK(result.model.meshes[0].primitives[0].indices == expected4);
        CHECK_FALSE(result.warnings.empty());
    }
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
    "model_import: an off-origin mesh's bounds do not include the world origin; neither does the "
    "model's summary.bounds (MI95, code-review BLOCKING-3)") {
    // positions (10,20,30), (11,20,30), (10,21,30) -- none touch the origin on ANY axis, unlike every
    // other geometry fixture in this file (triangle.gltf's own vertices all touch it, which is exactly
    // why this defect was invisible: ImportedMesh::bounds's bare `Aabb bounds;` defaults to a VALID
    // point box AT the origin -- Aabb's own aggregate default, never the invalid Aabb::empty()
    // sentinel -- so folding real geometry into it via expand() unioned the origin in regardless. That
    // is the exact number this task's Import Details panel prints, and the exact number manual
    // validation row 2 compares against Blender's Dimensions panel.
    const std::string doc =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, )"
        R"("mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": )"
        R"("VEC3"}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}], "buffers": )"
        R"([{"byteLength": 36, "uri": "data:application/octet-stream;base64,)"
        R"(AAAgQQAAoEEAAPBBAAAwQQAAoEEAAPBBAAAgQQAAqEEAAPBB"}]})";
    const ImportResult result =
        importModel("off-origin.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);

    CHECK(result.model.meshes[0].bounds.valid());
    CHECK(result.model.meshes[0].bounds.min.x == doctest::Approx(10.0F));
    CHECK(result.model.meshes[0].bounds.min.y == doctest::Approx(20.0F));
    CHECK(result.model.meshes[0].bounds.min.z == doctest::Approx(30.0F));
    CHECK(result.model.meshes[0].bounds.max.x == doctest::Approx(11.0F));
    CHECK(result.model.meshes[0].bounds.max.y == doctest::Approx(21.0F));
    CHECK(result.model.meshes[0].bounds.max.z == doctest::Approx(30.0F));

    CHECK(result.model.summary.bounds.valid());
    CHECK(result.model.summary.bounds.min.x == doctest::Approx(10.0F));
    CHECK(result.model.summary.bounds.min.y == doctest::Approx(20.0F));
    CHECK(result.model.summary.bounds.min.z == doctest::Approx(30.0F));
    CHECK(result.model.summary.bounds.max.x == doctest::Approx(11.0F));
    CHECK(result.model.summary.bounds.max.y == doctest::Approx(21.0F));
    CHECK(result.model.summary.bounds.max.z == doctest::Approx(30.0F));
}

TEST_CASE(
    "model_import: an empty mesh elsewhere in the model never pollutes summary.bounds with the "
    "origin (MI95b, code-review BLOCKING-3)") {
    // Two meshes sharing the SAME off-origin POSITION accessor: mesh 0 imports it as TRIANGLES (a real
    // primitive survives); mesh 1 imports it as TRIANGLE_STRIP (D11: every primitive skipped, the mesh
    // survives as an empty mesh with a point AABB at the origin). summary.bounds must equal mesh 0's
    // bounds exactly -- folding mesh 1's origin-point placeholder into it would reproduce a variant of
    // BLOCKING-3 for any multi-mesh model containing one legitimately-empty mesh.
    const std::string doc =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, )"
        R"("mode": 4}]}, {"primitives": [{"attributes": {"POSITION": 0}, "mode": 5}]}], "accessors": )"
        R"([{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}], "bufferViews": )"
        R"([{"buffer": 0, "byteOffset": 0, "byteLength": 36}], "buffers": [{"byteLength": 36, "uri": )"
        R"("data:application/octet-stream;base64,)"
        R"(AAAgQQAAoEEAAPBBAAAwQQAAoEEAAPBBAAAgQQAAqEEAAPBB"}]})";
    const ImportResult result =
        importModel("mixed-empty-mesh.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.model.meshes.size() == 2);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);  // mesh 0: the real, off-origin triangle
    CHECK(result.model.meshes[1].primitives.empty());        // mesh 1: TRIANGLE_STRIP, skipped
    // mesh 1's bounds is D11's point box AT the origin -- it must never leak into summary.bounds.
    CHECK(result.model.meshes[1].bounds.min.x == doctest::Approx(0.0F));
    CHECK(result.model.meshes[1].bounds.max.x == doctest::Approx(0.0F));

    CHECK(result.model.summary.bounds.valid());
    CHECK(result.model.summary.bounds.min.x == doctest::Approx(10.0F));
    CHECK(result.model.summary.bounds.max.x == doctest::Approx(11.0F));
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

// ---- sparse accessors (code-review round, BLOCKING-2) -------------------------------------------------
//
// git grep -rn 'sparse' tests/ was EMPTY before these three cases. validateAccessor's sparse check was
// gated on `accessor.sparse->count > 0`, which is the right gate for copyFromAccessor but wrong for the
// tools THIS file actually calls: both IterableAccessor's constructor (tools.hpp:588) and
// AccessorIterator's constructor (tools.hpp:494) branch on `sparse.has_value()` alone and dereference
// indicesBytes[0] UNCONDITIONALLY. The old check also never bounded the sparse views to their FULL
// EXTENT, only that they existed and were non-empty -- so an over-running sparse view read past its
// bufferView.

TEST_CASE(
    "model_import: a valid sparse accessor overrides exactly its named index; every other index "
    "comes from the base bufferView, untouched (MI96, code-review BLOCKING-2)") {
    // 3 base positions, all (0,0,0); sparse overrides index 0 to (1,2,3).
    const std::string doc =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, )"
        R"("mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": )"
        R"("VEC3", "sparse": {"count": 1, "indices": {"bufferView": 1, "componentType": 5123}, )"
        R"("values": {"bufferView": 2}}}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": )"
        R"(36}, {"buffer": 0, "byteOffset": 36, "byteLength": 2}, {"buffer": 0, "byteOffset": 38, )"
        R"("byteLength": 12}], "buffers": [{"byteLength": 50, "uri": )"
        R"("data:application/octet-stream;base64,)"
        R"(AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIA/AAAAQAAAQEA="}]})";
    const ImportResult result =
        importModel("sparse-ok.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    REQUIRE(result.model.meshes[0].primitives.size() == 1);
    const auto& prim = result.model.meshes[0].primitives[0];
    REQUIRE(prim.positions.size() == 3);
    CHECK(prim.positions[0].x == doctest::Approx(1.0F));  // overridden by the sparse entry
    CHECK(prim.positions[0].y == doctest::Approx(2.0F));
    CHECK(prim.positions[0].z == doctest::Approx(3.0F));
    CHECK(prim.positions[1].x == doctest::Approx(0.0F));  // from the base bufferView, untouched
    CHECK(prim.positions[2].x == doctest::Approx(0.0F));
}

TEST_CASE(
    "model_import: a sparse accessor with count == 0 and an out-of-range sparse view is refused, "
    "never dereferenced (MI97, code-review BLOCKING-2, run under ASan/UBSan)") {
    // Before the fix: `accessor.sparse->count > 0` gated the ENTIRE sparse check, so count == 0 skipped
    // validation outright -- fastgltf's own constructors dereference indicesBytes[0] regardless of
    // count, aborting with "reference binding to null pointer" (a Debug abort, exit 134) once bufferView
    // 99 (which does not exist) resolved to an empty span.
    const std::string doc =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, )"
        R"("mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, "count": 1, "type": )"
        R"("VEC3", "sparse": {"count": 0, "indices": {"bufferView": 99, "componentType": 5123}, )"
        R"("values": {"bufferView": 99}}}], "bufferViews": [{"buffer": 0, "byteOffset": 0, )"
        R"("byteLength": 12}], "buffers": [{"byteLength": 12, "uri": )"
        R"("data:application/octet-stream;base64,AACgQAAAwEAAAOBA"}]})";
    const ImportResult result =
        importModel("sparse-count0.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    CHECK(result.model.meshes[0].primitives.empty());  // POSITION refused -- the whole primitive skipped
    CHECK_FALSE(result.warnings.empty());
}

TEST_CASE(
    "model_import: a sparse accessor whose values view is too small for its declared count is "
    "refused, never read out of bounds (MI98, code-review BLOCKING-2, run under ASan)") {
    // 2 base positions; sparse claims 2 overrides, but the values bufferView is declared with room for
    // only ONE vec3 (12 bytes). Before the fix, the sparse check only confirmed the view was non-empty,
    // never that it was large enough for `count` entries -- the second override read 12 bytes past the
    // end of the buffer (an ASan heap-buffer-overflow at tools.hpp:533).
    const std::string doc =
        R"({"asset": {"version": "2.0"}, "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, )"
        R"("mode": 4}]}], "accessors": [{"bufferView": 0, "componentType": 5126, "count": 2, "type": )"
        R"("VEC3", "sparse": {"count": 2, "indices": {"bufferView": 1, "componentType": 5123}, )"
        R"("values": {"bufferView": 2}}}], "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": )"
        R"(24}, {"buffer": 0, "byteOffset": 24, "byteLength": 4}, {"buffer": 0, "byteOffset": 28, )"
        R"("byteLength": 12}], "buffers": [{"byteLength": 40, "uri": )"
        R"("data:application/octet-stream;base64,)"
        R"(AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAEEEAABBBAAAQQQ=="}]})";
    const ImportResult result =
        importModel("sparse-overrun.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.meshes.size() == 1);
    CHECK(result.model.meshes[0].primitives.empty());  // POSITION refused -- the whole primitive skipped
    CHECK_FALSE(result.warnings.empty());
}

// ---- the glTF backend, phases 7-8: skins and animation clips (Step 7) -------------------------------
//
// skinned.gltf's shape (§D-11): one skin, four joints (nodes 0-3), non-identity inverse bind matrices
// (joint i's translation column is (10i+1, 10i+2, 10i+3), rotation/scale block identity), and three
// animation clips -- StepAnim (translation on node 0, plus a 'weights' channel that must be skipped),
// LinearAnim (rotation on node 1), CubicAnim (scale on node 2, CUBICSPLINE).

TEST_CASE(
    "model_import: skinned.gltf's skin imports its joints in source order with exactly as many "
    "inverse bind matrices as joints (MI80, AC-31, INV-M7)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(SKINNED_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("skinned.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.skins.size() == 1);
    const auto& skin = result.model.skins[0];
    CHECK(skin.name == "TestSkin");
    const std::vector<std::uint32_t> expectedJoints = {0, 1, 2, 3};
    CHECK(skin.joints == expectedJoints);
    CHECK(skin.skeletonRoot == 0);
    REQUIRE(skin.inverseBindMatrices.size() == skin.joints.size());
    CHECK(result.model.summary.skinCount == 1);
    CHECK(result.model.summary.jointCount == 4);
}

TEST_CASE(
    "model_import: a skin with no inverseBindMatrices accessor yields joints.size() identity "
    "matrices, never an empty vector, at either depth (MI81, AC-32)") {
    const std::string doc = R"({"asset": {"version": "2.0"}, "nodes": [{}, {}], "skins": )"
                            R"([{"name": "NoIbmSkin", "joints": [0, 1]}]})";
    SUBCASE("Structure depth") {
        const ImportResult result =
            importModel("no-ibm.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
        REQUIRE(result.model.skins.size() == 1);
        REQUIRE(result.model.skins[0].inverseBindMatrices.size() == 2);
        CHECK(result.model.skins[0].inverseBindMatrices[0] == engine::Mat4::identity());
        CHECK(result.model.skins[0].inverseBindMatrices[1] == engine::Mat4::identity());
    }
    SUBCASE("Full depth") {
        const ImportResult result =
            importModel("no-ibm.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
        REQUIRE(result.model.skins.size() == 1);
        REQUIRE(result.model.skins[0].inverseBindMatrices.size() == 2);
        CHECK(result.model.skins[0].inverseBindMatrices[0] == engine::Mat4::identity());
        CHECK(result.model.skins[0].inverseBindMatrices[1] == engine::Mat4::identity());
    }
}

TEST_CASE(
    "model_import: a joint/inverse-bind-matrix count mismatch is a warning and the whole skin is "
    "skipped, never a truncated palette (MI82, AC-33)") {
    const std::string doc =
        R"({"asset": {"version": "2.0"}, "nodes": [{}, {}], "skins": [{"name": "Mismatched", )"
        R"("joints": [0, 1], "inverseBindMatrices": 0}], "accessors": [{"bufferView": 0, )"
        R"("componentType": 5126, "count": 1, "type": "MAT4"}], "bufferViews": [{"buffer": 0, )"
        R"("byteOffset": 0, "byteLength": 64}], "buffers": [{"byteLength": 64, "uri": )"
        R"("data:application/octet-stream;base64,)"
        R"(AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=="}]})";
    const ImportResult result = importModel("mismatch.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.model.skins.empty());
    bool sawMismatchWarning = false;
    for (const std::string& w : result.warnings) {
        if (w.find("inverse bind matri") != std::string::npos) {
            sawMismatchWarning = true;
        }
    }
    CHECK(sawMismatchWarning);
}

TEST_CASE(
    "model_import: a skin whose joints array names an out-of-range node index is skipped with a "
    "warning (MI83)") {
    const std::string doc = R"({"asset": {"version": "2.0"}, "nodes": [{}], "skins": )"
                            R"([{"name": "OutOfRange", "joints": [0, 99]}]})";
    const ImportResult result =
        importModel("bad-joint.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.model.skins.empty());
    bool sawOutOfRangeWarning = false;
    for (const std::string& w : result.warnings) {
        if (w.find("out of range") != std::string::npos) {
            sawOutOfRangeWarning = true;
        }
    }
    CHECK(sawOutOfRangeWarning);
}

TEST_CASE(
    "model_import: MAX_JOINTS_PER_SKIN truncates the document and skips that skin, keeping a "
    "coherent smaller model (MI84, AC-42, D15)") {
    std::string doc = R"({"asset": {"version": "2.0"}, "nodes": [{}], "skins": [{"joints": [)";
    for (std::size_t i = 0; i < engine::editor::MAX_JOINTS_PER_SKIN + 1; ++i) {
        if (i != 0) {
            doc += ',';
        }
        doc += '0';
    }
    doc += "]}]}";
    const ImportResult result =
        importModel("many-joints.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status == ImportStatus::Truncated);
    CHECK_FALSE(result.message.empty());
    CHECK(result.model.skins.empty());
}

TEST_CASE(
    "model_import: settings.scale scales ONLY the inverse bind matrices' translation column; the "
    "rotation/scale block does not (MI85, AC-30, plan A22)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(SKINNED_FIXTURE);
    REQUIRE(fixture.ok);
    ImportSettings settings;
    settings.scale = 2.0F;
    const ImportResult result = importModel("skinned.gltf", "", asBytes(fixture.text), settings, ImportDepth::Full, {});
    REQUIRE(result.model.skins.size() == 1);
    REQUIRE(result.model.skins[0].inverseBindMatrices.size() == 4);

    const engine::Mat4& ibm0 = result.model.skins[0].inverseBindMatrices[0];
    // The rotation/scale block -- UNCHANGED.
    CHECK(ibm0.columns[0].x == doctest::Approx(1.0F));
    CHECK(ibm0.columns[1].y == doctest::Approx(1.0F));
    CHECK(ibm0.columns[2].z == doctest::Approx(1.0F));
    // The translation column -- scaled; w is left untouched (it is not a coordinate).
    CHECK(ibm0.columns[3].x == doctest::Approx(2.0F));  // 1 * 2
    CHECK(ibm0.columns[3].y == doctest::Approx(4.0F));  // 2 * 2
    CHECK(ibm0.columns[3].z == doctest::Approx(6.0F));  // 3 * 2
    CHECK(ibm0.columns[3].w == doctest::Approx(1.0F));

    const engine::Mat4& ibm3 = result.model.skins[0].inverseBindMatrices[3];
    CHECK(ibm3.columns[3].x == doctest::Approx(62.0F));  // 31 * 2
    CHECK(ibm3.columns[3].y == doctest::Approx(64.0F));  // 32 * 2
    CHECK(ibm3.columns[3].z == doctest::Approx(66.0F));  // 33 * 2
}

TEST_CASE(
    "model_import: skinned.gltf's three clips import every surviving channel's target node, path "
    "and interpolation correctly, with duration equal to the max sample time and times strictly "
    "increasing (MI86, AC-34)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(SKINNED_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("skinned.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Ok);
    REQUIRE(result.model.animations.size() == 3);
    using engine::editor::AnimationInterpolation;
    using engine::editor::AnimationPath;

    const auto& stepAnim = result.model.animations[0];
    CHECK(stepAnim.name == "StepAnim");
    REQUIRE(stepAnim.channels.size() == 1);  // the weights channel was skipped (MI89)
    CHECK(stepAnim.channels[0].targetNode == 0);
    CHECK(stepAnim.channels[0].path == AnimationPath::Translation);
    CHECK(stepAnim.channels[0].interpolation == AnimationInterpolation::Step);
    CHECK(stepAnim.duration == doctest::Approx(1.0F));
    const std::vector<float> stepTimes = {0.0F, 0.5F, 1.0F};
    CHECK(stepAnim.channels[0].times == stepTimes);
    for (std::size_t i = 1; i < stepAnim.channels[0].times.size(); ++i) {
        CHECK(stepAnim.channels[0].times[i] > stepAnim.channels[0].times[i - 1]);
    }

    const auto& linearAnim = result.model.animations[1];
    CHECK(linearAnim.name == "LinearAnim");
    REQUIRE(linearAnim.channels.size() == 1);
    CHECK(linearAnim.channels[0].targetNode == 1);
    CHECK(linearAnim.channels[0].path == AnimationPath::Rotation);
    CHECK(linearAnim.channels[0].interpolation == AnimationInterpolation::Linear);
    CHECK(linearAnim.duration == doctest::Approx(0.75F));

    const auto& cubicAnim = result.model.animations[2];
    CHECK(cubicAnim.name == "CubicAnim");
    REQUIRE(cubicAnim.channels.size() == 1);
    CHECK(cubicAnim.channels[0].targetNode == 2);
    CHECK(cubicAnim.channels[0].path == AnimationPath::Scale);
    CHECK(cubicAnim.channels[0].interpolation == AnimationInterpolation::CubicSpline);
    CHECK(cubicAnim.duration == doctest::Approx(2.0F));

    CHECK(result.model.summary.animationCount == 3);
    CHECK(result.model.summary.animationDuration == doctest::Approx(2.0F));  // the longest clip
}

TEST_CASE(
    "model_import: the CUBICSPLINE clip stores three values per key, in glTF's own in-tangent / "
    "value / out-tangent order, and the channel is flagged accordingly (MI87, AC-35, INV-M6)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(SKINNED_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("skinned.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.model.animations.size() == 3);
    REQUIRE(result.model.animations[2].channels.size() == 1);
    const auto& channel = result.model.animations[2].channels[0];
    CHECK(channel.interpolation == engine::editor::AnimationInterpolation::CubicSpline);
    REQUIRE(channel.times.size() == 2);
    REQUIRE(channel.values.size() == channel.times.size() * 3);
    // key 0: in-tangent (0,0,0), value (1,1,1), out-tangent (0.1,0.1,0.1) -- glTF's own order.
    CHECK(channel.values[0].x == doctest::Approx(0.0F));
    CHECK(channel.values[1].x == doctest::Approx(1.0F));
    CHECK(channel.values[2].x == doctest::Approx(0.1F));
    // key 1: in-tangent (0.2,0.2,0.2), value (2,2,2), out-tangent (0,0,0).
    CHECK(channel.values[3].x == doctest::Approx(0.2F));
    CHECK(channel.values[4].x == doctest::Approx(2.0F));
    CHECK(channel.values[5].x == doctest::Approx(0.0F));
    // Widened to Vec4 with w = 0 for glTF's three-component paths.
    CHECK(channel.values[1].w == doctest::Approx(0.0F));
}

TEST_CASE("model_import: the STEP and LINEAR clips store exactly one value per key (MI88)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(SKINNED_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("skinned.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.model.animations.size() == 3);
    REQUIRE(result.model.animations[0].channels.size() == 1);
    REQUIRE(result.model.animations[1].channels.size() == 1);
    const auto& stepChannel = result.model.animations[0].channels[0];
    CHECK(stepChannel.values.size() == stepChannel.times.size());
    const auto& linearChannel = result.model.animations[1].channels[0];
    CHECK(linearChannel.values.size() == linearChannel.times.size());
    // Rotation values arrive in glTF's own {x,y,z,w} order, un-reordered.
    REQUIRE(linearChannel.values.size() == 3);
    CHECK(linearChannel.values[1].x == doctest::Approx(0.1F));
    CHECK(linearChannel.values[1].y == doctest::Approx(0.2F));
    CHECK(linearChannel.values[1].z == doctest::Approx(0.3F));
    CHECK(linearChannel.values[1].w == doctest::Approx(0.4F));
}

TEST_CASE(
    "model_import: a weights (morph) animation channel is skipped with exactly one warning; the "
    "clip's other channel imports normally (MI89, AC-36, D12)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(SKINNED_FIXTURE);
    REQUIRE(fixture.ok);
    const ImportResult result =
        importModel("skinned.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.model.animations.size() == 3);
    const auto& stepAnim = result.model.animations[0];
    REQUIRE(stepAnim.channels.size() == 1);
    CHECK(stepAnim.channels[0].path == engine::editor::AnimationPath::Translation);
    std::size_t weightsWarnings = 0;
    for (const std::string& w : result.warnings) {
        if (w.find("weights") != std::string::npos) {
            ++weightsWarnings;
        }
    }
    CHECK(weightsWarnings == 1);
}

TEST_CASE(
    "model_import: a channel whose keyframe times are not strictly increasing is skipped with a "
    "warning, never sorted (MI90, AC-34)") {
    const std::string doc =
        R"({"asset": {"version": "2.0"}, "nodes": [{}], "animations": [{"channels": [{"sampler": 0, )"
        R"("target": {"node": 0, "path": "translation"}}], "samplers": [{"input": 0, "output": 1}]}], )"
        R"("accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "SCALAR"}, )"
        R"({"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"}], "bufferViews": )"
        R"([{"buffer": 0, "byteOffset": 0, "byteLength": 12}, {"buffer": 0, "byteOffset": 12, )"
        R"("byteLength": 36}], "buffers": [{"byteLength": 48, "uri": )"
        R"("data:application/octet-stream;base64,)"
        R"(AAAAAAAAAEAAAMA+AACAPwAAAAAAAAAAAAAAQAAAAAAAAAAAAABAQAAAAAAAAAAA"}]})";
    const ImportResult result =
        importModel("not-increasing.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.model.animations.size() == 1);
    CHECK(result.model.animations[0].channels.empty());  // skipped, never sorted into shape
    bool sawWarning = false;
    for (const std::string& w : result.warnings) {
        if (w.find("increasing") != std::string::npos) {
            sawWarning = true;
        }
    }
    CHECK(sawWarning);
}

TEST_CASE(
    "model_import: a channel targeting an out-of-range node is skipped with a warning; the clip "
    "survives with its other channel intact (MI91, E23)") {
    const std::string doc = R"({"asset": {"version": "2.0"}, "nodes": [{}], "animations": [{"channels": [)"
                            R"({"sampler": 0, "target": {"node": 99, "path": "translation"}}, )"
                            R"({"sampler": 0, "target": {"node": 0, "path": "scale"}}], )"
                            R"("samplers": [{"input": 0, "output": 1}]}], "accessors": [{"bufferView": 0, )"
                            R"("componentType": 5126, "count": 1, "type": "SCALAR"}, {"bufferView": 1, )"
                            R"("componentType": 5126, "count": 1, "type": "VEC3"}], "bufferViews": [{"buffer": 0, )"
                            R"("byteOffset": 0, "byteLength": 4}, {"buffer": 0, "byteOffset": 4, "byteLength": 12}], )"
                            R"("buffers": [{"byteLength": 16, "uri": )"
                            R"("data:application/octet-stream;base64,AAAAAAAAgD8AAIA/AACAPw=="}]})";
    const ImportResult result =
        importModel("bad-target.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    REQUIRE(result.model.animations.size() == 1);
    REQUIRE(result.model.animations[0].channels.size() == 1);  // the out-of-range channel was dropped
    CHECK(result.model.animations[0].channels[0].targetNode == 0);
    CHECK(result.model.animations[0].channels[0].path == engine::editor::AnimationPath::Scale);
    bool sawWarning = false;
    for (const std::string& w : result.warnings) {
        if (w.find("out of range") != std::string::npos) {
            sawWarning = true;
        }
    }
    CHECK(sawWarning);
}

TEST_CASE(
    "model_import: importAnimations == false and importSkins == false each yield zero of their "
    "kind and leave everything else identical (MI92, AC-37)") {
    const scene_golden::FileBytes fixture = scene_golden::readBytes(SKINNED_FIXTURE);
    REQUIRE(fixture.ok);
    ImportSettings disabled;
    disabled.importAnimations = false;
    disabled.importSkins = false;
    const ImportResult withBoth =
        importModel("skinned.gltf", "", asBytes(fixture.text), ImportSettings{}, ImportDepth::Full, {});
    const ImportResult withoutEither =
        importModel("skinned.gltf", "", asBytes(fixture.text), disabled, ImportDepth::Full, {});

    CHECK(withoutEither.model.skins.empty());
    CHECK(withoutEither.model.summary.skinCount == 0);
    CHECK(withoutEither.model.summary.jointCount == 0);
    CHECK(withoutEither.model.animations.empty());
    CHECK(withoutEither.model.summary.animationCount == 0);
    CHECK(withoutEither.model.summary.animationDuration == doctest::Approx(0.0F));

    CHECK_FALSE(withBoth.model.skins.empty());
    CHECK_FALSE(withBoth.model.animations.empty());

    // Everything else -- field by field.
    REQUIRE(withoutEither.model.nodes.size() == withBoth.model.nodes.size());
    for (std::size_t i = 0; i < withBoth.model.nodes.size(); ++i) {
        CHECK(withoutEither.model.nodes[i].name == withBoth.model.nodes[i].name);
        CHECK(withoutEither.model.nodes[i].translation.x == doctest::Approx(withBoth.model.nodes[i].translation.x));
        CHECK(withoutEither.model.nodes[i].rotation.w == doctest::Approx(withBoth.model.nodes[i].rotation.w));
        CHECK(withoutEither.model.nodes[i].scale.x == doctest::Approx(withBoth.model.nodes[i].scale.x));
    }
    CHECK(withoutEither.model.roots == withBoth.model.roots);
    CHECK(withoutEither.model.meshes.size() == withBoth.model.meshes.size());
    CHECK(withoutEither.model.materials.size() == withBoth.model.materials.size());
    CHECK(withoutEither.model.images.size() == withBoth.model.images.size());
    CHECK(withoutEither.externalUris == withBoth.externalUris);
    CHECK(withoutEither.status == withBoth.status);
}

TEST_CASE(
    "model_import: MAX_ANIMATION_KEYS_PER_MODEL truncates a document whose accessor.count claims "
    "the excess WITHOUT providing the bytes, and completes quickly (MI93, AC-42, D15)") {
    const std::string doc =
        R"({"asset": {"version": "2.0"}, "nodes": [{}], "animations": [{"channels": [{"sampler": 0, )"
        R"("target": {"node": 0, "path": "translation"}}], "samplers": [{"input": 0, "output": 1}]}], )"
        R"("accessors": [{"bufferView": 0, "componentType": 5126, "count": 2000001, "type": "SCALAR"}, )"
        R"({"bufferView": 1, "componentType": 5126, "count": 1, "type": "VEC3"}], "bufferViews": )"
        R"([{"buffer": 0, "byteOffset": 0, "byteLength": 4}, {"buffer": 0, "byteOffset": 4, )"
        R"("byteLength": 12}], "buffers": [{"byteLength": 16, "uri": )"
        R"("data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAAAAAA=="}]})";
    const ImportResult result =
        importModel("huge-keys.gltf", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Truncated);
    CHECK_FALSE(result.message.empty());
    REQUIRE(result.model.animations.size() == 1);
    CHECK(result.model.animations[0].channels.empty());
}

// ---- the glTF backend, F7b's pin, extended a third time (Step 7) -------------------------------------
//
// MI58's role (plan §S, Step 5's table): "asymmetric.gltf, all three halves at Full depth" -- the TRS
// half was written at Step 5 as MI57 (Structure depth) and re-asserted here; the mesh half was written
// at Step 6; this step finishes it with the skin half, so MI58 is fully folded into MI40b and is never
// a separate case of its own.

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

    // The skin half (Step 7, finishing MI58's role): the single joint and its non-identity inverse
    // bind matrix, exactly the source's sixteen DISTINCT values, in the source's own column-major
    // order -- a transposed or reordered read fails on the first component that differs, exactly like
    // the triangle and TRS halves above.
    REQUIRE(result.model.skins.size() == 1);
    const auto& skin = result.model.skins[0];
    CHECK(skin.name == "AsymmetricSkin");
    REQUIRE(skin.joints.size() == 1);
    CHECK(skin.joints[0] == 0);
    CHECK(skin.skeletonRoot == 0);
    REQUIRE(skin.inverseBindMatrices.size() == 1);
    const engine::Mat4& ibm = skin.inverseBindMatrices[0];
    CHECK(ibm.columns[0].x == doctest::Approx(1.0F));
    CHECK(ibm.columns[0].y == doctest::Approx(2.0F));
    CHECK(ibm.columns[0].z == doctest::Approx(3.0F));
    CHECK(ibm.columns[0].w == doctest::Approx(4.0F));
    CHECK(ibm.columns[1].x == doctest::Approx(5.0F));
    CHECK(ibm.columns[1].y == doctest::Approx(6.0F));
    CHECK(ibm.columns[1].z == doctest::Approx(7.0F));
    CHECK(ibm.columns[1].w == doctest::Approx(8.0F));
    CHECK(ibm.columns[2].x == doctest::Approx(9.0F));
    CHECK(ibm.columns[2].y == doctest::Approx(10.0F));
    CHECK(ibm.columns[2].z == doctest::Approx(11.0F));
    CHECK(ibm.columns[2].w == doctest::Approx(12.0F));
    CHECK(ibm.columns[3].x == doctest::Approx(13.0F));
    CHECK(ibm.columns[3].y == doctest::Approx(14.0F));
    CHECK(ibm.columns[3].z == doctest::Approx(15.0F));
    CHECK(ibm.columns[3].w == doctest::Approx(16.0F));
}

// ---- task 3.2.2 (ufbx): the pure predicates and the dispatch arm, no ufbx anywhere (MI103-117) -------
//
// Confirmed the file's last case before this block was MI102 (`grep -c '^TEST_CASE('` -> 102).

TEST_CASE("model_import: isImportableModelName accepts .fbx case-insensitively, suffix-on-full-name (MI103)") {
    CHECK(isImportableModelName("a.fbx"));
    CHECK(isImportableModelName("a.FBX"));
    CHECK(isImportableModelName("a.FbX"));
    CHECK(isImportableModelName("archive.tar.fbx"));
}

TEST_CASE("model_import: isImportableModelName rejects a bare or malformed .fbx name (MI104)") {
    // 4 bytes, not > 4 -- needs something BEFORE the extension (the isMetaFileName shape), same rule
    // MI20 already proves for .gltf.
    CHECK_FALSE(isImportableModelName(".fbx"));
    CHECK_FALSE(isImportableModelName("a.fbx.bak"));
    CHECK_FALSE(isImportableModelName("fbx"));
    CHECK_FALSE(isImportableModelName(""));
}

TEST_CASE(
    "model_import: importModel's dispatch never silently refuses an extension isImportableModelName "
    "accepts (MI105, D5)") {
    // The ONLY thing keeping the suffix table (isImportableModelName's EXTENSIONS) and the dispatch
    // if-chain (importModel) in sync: a future importer added to one but not the other is a RED case
    // here, not a silent refusal. A one-byte body is enough -- "not Unsupported" is all this asserts;
    // each backend's own tier-0 suite proves its content-level behaviour.
    //
    // Grows to FIVE here (code-review round: this array and the comment below it stopped at FOUR,
    // stale from Step 3, and never grew to include ".mtl" once importMtlOnly became real at Step 7),
    // and to EIGHT at task 3.2.5, which claims .dae, .ply and .stl through one Assimp backend. The
    // array's SIZE TEMPLATE ARGUMENT moves with it, or this file does not compile.
    constexpr std::array<std::string_view, 8> ACCEPTED_EXTENSIONS = {".gltf", ".glb", ".fbx", ".obj",
                                                                     ".mtl",  ".dae", ".ply", ".stl"};
    const std::string oneByte = "x";
    for (const std::string_view ext : ACCEPTED_EXTENSIONS) {
        const std::string name = "model" + std::string(ext);
        REQUIRE(isImportableModelName(name));
        const ImportResult result =
            importModel(name, "", asBytes(oneByte), ImportSettings{}, ImportDepth::Structure, {});
        INFO("name: ", name);
        CHECK(result.status != ImportStatus::Unsupported);
    }
}

TEST_CASE(
    "model_import: the suffix table, the identity table and the dispatch chain agree for every claimed "
    "and unclaimed name (MI105b, task 3.2.3, §A-8)") {
    // ".mtl" is now included (code-review round: it was DELIBERATELY absent through Step 6, while
    // importMtlOnly was still a stub, but the comment and the table were never updated once Step 7 made
    // the .mtl arm real -- this IS the three-way sync check, and .mtl was the one claimed extension it
    // did not bind).
    // ".ply" and ".stl" join at task 3.2.5, beside the ".dae" that was already here as a NEGATIVE and
    // is now a positive. The SIZE TEMPLATE ARGUMENT moves with the array, or this file does not compile.
    constexpr std::array<std::string_view, 13> NAMES = {
        "a.gltf", "a.glb", "a.fbx", "a.obj",  "a.mtl", "a.blend", "a.dae",
        "a.ply",  "a.stl", "a.png", "README", "",      ".obj",
    };
    const std::string oneByte = "x";
    for (const std::string_view name : NAMES) {
        const bool importable = isImportableModelName(name);
        INFO("name: '", name, "'");
        CHECK(importable == !modelImporterIdentity(name).name.empty());
        const ImportResult result =
            importModel(name, "", asBytes(oneByte), ImportSettings{}, ImportDepth::Structure, {});
        CHECK(importable == (result.status != ImportStatus::Unsupported));
    }
}

TEST_CASE(
    "model_import: the byte-identical body routes to three DIFFERENT backends by extension alone "
    "(MI105c, task 3.2.3, §A-8 -- the routing discriminator)") {
    // A seed that swaps the dispatch's arm order (routing .obj into importGltf, as it transiently did
    // between Steps 2 and 3 -- see obj_import_test.cpp's OI1 history) reddens THIS case and only this
    // one: MI105's "not Unsupported" loop would stay green either way, since fastgltf and ufbx both
    // fail on this body with a status other than Unsupported too.
    const std::string body = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    // Strengthened per plan §A-8 (code-review round: this stayed at Step 3's own "WEAKENED for now"
    // shape long after geometry conversion landed at Step 5) -- ".obj" now asserts "Ok with exactly one
    // mesh", not merely "Ok".
    const ImportResult obj = importModel("t.obj", "", asBytes(body), ImportSettings{}, ImportDepth::Full, {});
    CHECK(obj.status == ImportStatus::Ok);
    CHECK(obj.model.meshes.size() == 1);
    const ImportResult gltf = importModel("t.gltf", "", asBytes(body), ImportSettings{}, ImportDepth::Full, {});
    CHECK(gltf.status != ImportStatus::Ok);
    const ImportResult fbx = importModel("t.fbx", "", asBytes(body), ImportSettings{}, ImportDepth::Full, {});
    CHECK(fbx.status != ImportStatus::Ok);

    // task 3.2.5: three more arms, and the one-byte body is a FAILURE for all three (each format's
    // parser rejects it), which is what makes this a ROUTING discriminator rather than a smoke test --
    // "not Ok" AND "not Unsupported" together can only be produced by a backend that actually ran.
    const std::string oneByte = "x";
    for (const std::string_view name : {"t.dae", "t.ply", "t.stl"}) {
        const ImportResult result = importModel(name, "", asBytes(oneByte), ImportSettings{}, ImportDepth::Full, {});
        INFO("name: ", name);
        CHECK(result.status != ImportStatus::Ok);
        CHECK(result.status != ImportStatus::Unsupported);
    }
}

TEST_CASE("model_import: modelImporterIdentity(\"a.fbx\") is the FBX pair, not a shared constant (MI106)") {
    const ImporterIdentity identity = modelImporterIdentity("a.fbx");
    CHECK(identity.name == FBX_IMPORTER_NAME);
    CHECK(identity.version == FBX_IMPORTER_VERSION);
}

TEST_CASE(
    "model_import: modelImporterIdentity resolves .gltf and .glb to the glTF pair -- the FBX arm did "
    "not capture them (MI107)") {
    const ImporterIdentity gltf = modelImporterIdentity("a.gltf");
    CHECK(gltf.name == GLTF_IMPORTER_NAME);
    CHECK(gltf.version == GLTF_IMPORTER_VERSION);
    const ImporterIdentity glb = modelImporterIdentity("a.glb");
    CHECK(glb.name == GLTF_IMPORTER_NAME);
    CHECK(glb.version == GLTF_IMPORTER_VERSION);
}

TEST_CASE(
    "model_import: modelImporterIdentity is (\"\", 0) for a name no importer claims -- ImportInput's "
    "own un-probed defaults (MI108)") {
    CHECK(modelImporterIdentity("a.png") == ImporterIdentity{});
    CHECK(modelImporterIdentity("a") == ImporterIdentity{});
    CHECK(modelImporterIdentity("") == ImporterIdentity{});
}

TEST_CASE("model_import: modelImporterIdentity case-folds exactly like isImportableModelName (MI109)") {
    const ImporterIdentity identity = modelImporterIdentity("A.FBX");
    CHECK(identity.name == FBX_IMPORTER_NAME);
    CHECK(identity.version == FBX_IMPORTER_VERSION);
}

TEST_CASE(
    "model_import: modelImporterNeedsExternalBuffers is true for glTF, false for FBX and non-models "
    "(MI110, AC-56a)") {
    CHECK(modelImporterNeedsExternalBuffers("a.gltf"));
    CHECK(modelImporterNeedsExternalBuffers("a.glb"));
    CHECK_FALSE(modelImporterNeedsExternalBuffers("a.fbx"));
    CHECK_FALSE(modelImporterNeedsExternalBuffers("a.png"));
}

TEST_CASE("model_import: foldBackslashesToSlashes folds every backslash and touches nothing else (MI111)") {
    CHECK(foldBackslashesToSlashes("textures\\wood.png") == "textures/wood.png");
    CHECK(foldBackslashesToSlashes("a/b") == "a/b");
    CHECK(foldBackslashesToSlashes("").empty());
    CHECK(foldBackslashesToSlashes("\\\\unc\\x") == "//unc/x");
}

TEST_CASE(
    "model_import: fold-then-classify and classify-then-fold are DISTINGUISHABLE (MI112, D14 -- what "
    "makes FI49 meaningful)") {
    // Folding BEFORE classifying: the escape is already '../../x.png' when the check runs, so it is
    // caught as RefusedEscape.
    CHECK(classifyUri(foldBackslashesToSlashes("..\\..\\x.png"), "models").kind == UriClass::RefusedEscape);
    // classifyUri alone, UNFOLDED: the backslash is caught FIRST, as RefusedBackslash -- proving the
    // two orderings produce genuinely different, distinguishable results.
    CHECK(classifyUri("..\\..\\x.png", "models").kind == UriClass::RefusedBackslash);
}

TEST_CASE(
    "model_import: classifyUri is UNMODIFIED by task 3.2.2 -- MI10/MI11's backslash refusals still "
    "fire for an unfolded URI (MI113, AC-63's sibling)") {
    CHECK(classifyUri("C:\\x.png", "models").kind == UriClass::RefusedBackslash);
    CHECK(classifyUri("\\\\unc\\x", "models").kind == UriClass::RefusedBackslash);
    CHECK(classifyUri("a\\b.png", "models").kind == UriClass::RefusedBackslash);
}

TEST_CASE(
    "model_import: importModel(\"a.fbx\", ...) with a non-FBX body fails without needing ufbx to "
    "succeed -- separates \"the dispatch works\" from \"the backend works\" (MI114)") {
    const std::string doc = "this is not an FBX document";
    const ImportResult result = importModel("a.fbx", "", asBytes(doc), ImportSettings{}, ImportDepth::Structure, {});
    CHECK(result.status != ImportStatus::Ok);
    CHECK(result.model.nodes.empty());
    CHECK(result.model.meshes.empty());
}

TEST_CASE(
    "model_import: importModel(\"a.png\", ...) stays Unsupported and never dereferences bytes, "
    "unchanged across a three-arm dispatch (MI115, AC-48)") {
    const std::span<const std::byte> emptySpan;  // {nullptr, 0} -- any dereference would crash/ASan-trip
    const ImportResult result = importModel("a.png", "", emptySpan, ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::Unsupported);
    CHECK(result.model.nodes.empty());
}

TEST_CASE("model_import: SourceSpace{} defaults to \"declares nothing\" (MI116)") {
    const SourceSpace space;
    CHECK_FALSE(space.declared);
    CHECK(space.unitMeters == doctest::Approx(1.0F));
    CHECK(space.upAxis == 'Y');
    CHECK(space.generator.empty());
    CHECK(space.formatVersion.empty());
}

TEST_CASE(
    "model_import: ImportedModel{} designated-initialized with sourceSpace set compiles, every other "
    "field stays default -- sourceSpace was APPENDED, never inserted (MI117, 3.1.2's A2 trap)") {
    const engine::editor::ImportedModel model{.sourceSpace = SourceSpace{.declared = true, .upAxis = 'Z'}};
    CHECK(model.nodes.empty());
    CHECK(model.roots.empty());
    CHECK(model.meshes.empty());
    CHECK(model.materials.empty());
    CHECK(model.images.empty());
    CHECK(model.skins.empty());
    CHECK(model.animations.empty());
    CHECK(model.summary.nodeCount == 0);
    CHECK(model.summary.vertexCount == 0);
    CHECK(model.sourceSpace.declared);
    CHECK(model.sourceSpace.upAxis == 'Z');
}

// ---- task 3.2.3 (tinyobjloader): the widened predicate, the OBJ identity arm, the corrected
// modelImporterNeedsExternalBuffers split, and the two new pure Wavefront helpers -- no tinyobjloader
// anywhere (MI118-131) ----------------------------------------------------------------------------
//
// Confirmed the file's last case before this block was MI117 (`grep -c '^TEST_CASE('` -> 117).

TEST_CASE("model_import: isImportableModelName accepts .obj/.mtl case-insensitively, suffix-on-full-name (MI118)") {
    CHECK(isImportableModelName("a.obj"));
    CHECK(isImportableModelName("a.OBJ"));
    CHECK(isImportableModelName("a.mtl"));
    CHECK(isImportableModelName("a.MTL"));
    CHECK(isImportableModelName("archive.tar.obj"));
    CHECK(isImportableModelName("archive.tar.mtl"));
}

TEST_CASE("model_import: isImportableModelName rejects a bare or malformed .obj/.mtl name (MI119)") {
    // Same rule MI20/MI104 already prove for .gltf/.fbx -- needs something BEFORE the extension.
    CHECK_FALSE(isImportableModelName(".obj"));
    CHECK_FALSE(isImportableModelName(".mtl"));
    CHECK_FALSE(isImportableModelName("a.obj.bak"));
    CHECK_FALSE(isImportableModelName("obj"));
    CHECK_FALSE(isImportableModelName("mtl"));
}

TEST_CASE("model_import: the two asymmetries that survive task 3.2.5 (MI120, AC-2 corrected)") {
    // REWRITTEN, not deleted: this case's number and its job -- pinning where isImportableModelName and
    // asset_view.hpp's AssetKind::Model deliberately disagree -- are unchanged; what changed is the
    // answer. Task 3.2.5 claims .dae/.ply/.stl, so after it the two tables differ in exactly TWO places.
    //
    // 1. `.blend` is AssetKind::Model and NOT importable (3.2.4's D15 -- the scan must never spawn a
    //    process, and phase 7.5 gates its probe on exactly this predicate).
    CHECK_FALSE(isImportableModelName("a.blend"));
    // 2. `.mtl` is importable and NOT AssetKind::Model at all (3.2.3's D4 -- it classifies Unknown).
    CHECK(isImportableModelName("a.mtl"));
    // Everything else now agrees, which is the whole content of this task's table widening.
    CHECK(isImportableModelName("a.dae"));
    CHECK(isImportableModelName("a.ply"));
    CHECK(isImportableModelName("a.stl"));
}

TEST_CASE("model_import: modelImporterIdentity's full four-row table (MI121, AC-4 corrected)") {
    const ImporterIdentity gltf = modelImporterIdentity("a.gltf");
    CHECK(gltf.name == GLTF_IMPORTER_NAME);
    CHECK(gltf.version == GLTF_IMPORTER_VERSION);
    const ImporterIdentity fbx = modelImporterIdentity("a.fbx");
    CHECK(fbx.name == FBX_IMPORTER_NAME);
    CHECK(fbx.version == FBX_IMPORTER_VERSION);
    const ImporterIdentity obj = modelImporterIdentity("a.obj");
    CHECK(obj.name == OBJ_IMPORTER_NAME);
    CHECK(obj.version == OBJ_IMPORTER_VERSION);
    // task 3.2.5: a FIFTH row, and it is ONE identity across THREE claimed extensions -- which is what
    // makes an ASSIMP_IMPORTER_VERSION bump re-trigger .dae, .ply and .stl together and nothing else.
    for (const std::string_view name : {"a.dae", "a.ply", "a.stl"}) {
        const ImporterIdentity assimp = modelImporterIdentity(name);
        INFO("name: ", name);
        CHECK(assimp.name == ASSIMP_IMPORTER_NAME);
        CHECK(assimp.version == ASSIMP_IMPORTER_VERSION);
    }
    const ImporterIdentity none = modelImporterIdentity("a.png");
    CHECK(none == ImporterIdentity{});
}

TEST_CASE(
    "model_import: modelImporterIdentity(\".obj\") and (\".mtl\") share the SAME pair, and it case-folds "
    "(MI122, D17)") {
    const ImporterIdentity obj = modelImporterIdentity("chair.obj");
    const ImporterIdentity mtl = modelImporterIdentity("chair.mtl");
    CHECK(obj.name == OBJ_IMPORTER_NAME);
    CHECK(obj.version == OBJ_IMPORTER_VERSION);
    CHECK(obj == mtl);
    const ImporterIdentity folded = modelImporterIdentity("A.OBJ");
    CHECK(folded.name == OBJ_IMPORTER_NAME);
}

TEST_CASE(
    "model_import: modelImporterNeedsExternalBuffers's four-way split -- glTF/.obj true, FBX/.mtl false "
    "(MI123, §A-5, R8)") {
    CHECK(modelImporterNeedsExternalBuffers("a.gltf"));
    CHECK(modelImporterNeedsExternalBuffers("a.glb"));
    CHECK_FALSE(modelImporterNeedsExternalBuffers("a.fbx"));
    CHECK(modelImporterNeedsExternalBuffers("a.obj"));
    CHECK_FALSE(modelImporterNeedsExternalBuffers("a.mtl"));
    CHECK_FALSE(modelImporterNeedsExternalBuffers("a.png"));
}

TEST_CASE("model_import: scanObjMtlLibs collects a single mtllib operand (MI124)") {
    const std::string body = "mtllib cube.mtl\n";
    const std::vector<std::string> candidates = scanObjMtlLibs(asBytes(body), 1024);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0] == "cube.mtl");
}

TEST_CASE(
    "model_import: scanObjMtlLibs offers the WHOLE trimmed operand first, then each whitespace-separated "
    "token (MI125, D16)") {
    const std::string body = "mtllib my file.mtl\n";
    const std::vector<std::string> candidates = scanObjMtlLibs(asBytes(body), 1024);
    REQUIRE(candidates.size() == 3);
    CHECK(candidates[0] == "my file.mtl");
    CHECK(candidates[1] == "my");
    CHECK(candidates[2] == "file.mtl");
}

TEST_CASE(
    "model_import: scanObjMtlLibs accepts leading spaces/tabs, is CASE-SENSITIVE, and never matches inside "
    "a comment (MI126, E5, D16)") {
    CHECK(scanObjMtlLibs(asBytes(std::string(" \tmtllib a.mtl\n")), 1024) == std::vector<std::string>{"a.mtl"});
    CHECK(scanObjMtlLibs(asBytes(std::string("# mtllib a.mtl\n")), 1024).empty());
    CHECK(scanObjMtlLibs(asBytes(std::string("MTLLIB a.mtl\n")), 1024).empty());
}

TEST_CASE(
    "model_import: scanObjMtlLibs collects several mtllib lines in order, deduplicated BY RAW TEXT "
    "(MI127, E3)") {
    const std::string body = "mtllib a.mtl\nmtllib b.mtl\nmtllib a.mtl\n";
    const std::vector<std::string> candidates = scanObjMtlLibs(asBytes(body), 1024);
    REQUIRE(candidates.size() == 2);
    CHECK(candidates[0] == "a.mtl");
    CHECK(candidates[1] == "b.mtl");
}

TEST_CASE("model_import: scanObjMtlLibs's maxNames caps BEFORE the token expansion (MI128, INV-O10)") {
    const std::string body = "mtllib a.mtl b.mtl\n";
    const std::vector<std::string> candidates = scanObjMtlLibs(asBytes(body), 1);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0] == "a.mtl b.mtl");  // the whole operand filled the cap; neither token got in
}

TEST_CASE("model_import: scanObjMtlLibs produces no candidate for an empty operand (MI129, E4)") {
    CHECK(scanObjMtlLibs(asBytes(std::string("mtllib   \n")), 1024).empty());
    CHECK(scanObjMtlLibs(asBytes(std::string("mtllib\t\n")), 1024).empty());
}

TEST_CASE(
    "model_import: scanObjMtlLibs strips ONE trailing '\\r', still scans a line with no trailing newline, "
    "and an empty span yields nothing (MI130, E7, E8)") {
    CHECK(scanObjMtlLibs(asBytes(std::string("mtllib a.mtl\r\n")), 1024) == std::vector<std::string>{"a.mtl"});
    CHECK(scanObjMtlLibs(asBytes(std::string("mtllib a.mtl")), 1024) == std::vector<std::string>{"a.mtl"});
    CHECK(scanObjMtlLibs({}, 1024).empty());
}

TEST_CASE(
    "model_import: looksLikeBinaryContent finds a NUL inside the probe window and never one outside it "
    "(MI131, AC-54)") {
    const std::string text = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    CHECK_FALSE(looksLikeBinaryContent(asBytes(text)));
    CHECK_FALSE(looksLikeBinaryContent({}));

    std::string withNul = text;
    withNul[3] = '\0';
    CHECK(looksLikeBinaryContent(asBytes(withNul)));

    // The NUL sits past a probe window of 8 -- outside the window, so it must NOT be seen.
    CHECK_FALSE(looksLikeBinaryContent(asBytes(withNul), 3));
    CHECK(looksLikeBinaryContent(asBytes(withNul), 4));
}

TEST_CASE(
    "model_import: scanObjMtlLibsScan recovers candidates AND the E4 empty-operand-line count in ONE "
    "pass, and scanObjMtlLibs stays candidates-only (MI132, code-review round, gap 10)") {
    const std::string body = "mtllib a.mtl\nmtllib   \nv 0 0 0\nmtllib\t\nmtllib b.mtl\n";
    const ObjMtlLibScan scan = scanObjMtlLibsScan(asBytes(body), 1024);
    REQUIRE(scan.candidates.size() == 2);
    CHECK(scan.candidates[0] == "a.mtl");
    CHECK(scan.candidates[1] == "b.mtl");
    CHECK(scan.emptyOperandLines == 2);  // "mtllib   " and "mtllib\t"

    // scanObjMtlLibs delegates to scanObjMtlLibsScan and returns ONLY the candidates -- identical to a
    // direct call, for every existing MI124-MI131 caller.
    CHECK(scanObjMtlLibs(asBytes(body), 1024) == scan.candidates);

    // an empty span yields an empty scan outright, matching scanObjMtlLibs's own rule.
    CHECK(scanObjMtlLibsScan({}, 1024).candidates.empty());
    CHECK(scanObjMtlLibsScan({}, 1024).emptyOperandLines == 0);
    // maxNames == 0 caps `candidates` at zero (scanObjMtlLibs's own pre-existing rule, unchanged), but
    // emptyOperandLines is ORTHOGONAL to that cap and is still counted correctly -- there is no
    // candidate to cap for an empty operand in the first place.
    const ObjMtlLibScan zeroCap = scanObjMtlLibsScan(asBytes(body), 0);
    CHECK(zeroCap.candidates.empty());
    CHECK(zeroCap.emptyOperandLines == 2);
}

TEST_CASE(
    "model_import: .blend is claimed by NEITHER name predicate, and the two facts break independently "
    "(MI133, task 3.2.4 D15/AC-29/AC-44, seeds S22 and S35)") {
    // TWO assertions, load-bearing TOGETHER and breakable SEPARATELY, which is why they share a case
    // and why the sabotage matrix attacks each half with its own seed.
    //
    // Half 1 (seed S22 -- add ".blend" to isImportableModelName's five-entry table): the SCAN's phase
    // 7.5 gates its probe on this predicate, so claiming .blend would make the scan feed raw .blend
    // bytes to importModel -- which returns Unsupported -- and every .blend in the project would report
    // an import failure on every scan. Task 3.2.4 converts a .blend to a GLB ABOVE importModel instead.
    CHECK_FALSE(isImportableModelName("statue.blend"));

    // Half 2 (seed S35 -- special-case ".blend" to true inside modelImporterNeedsExternalBuffers while
    // leaving the table alone): today this answers false only BECAUSE the table does not claim .blend
    // (it is not .fbx, not .mtl, and falls through to isImportableModelName). The two facts are
    // therefore coupled in one direction and independent in the other, and a future edit can break
    // either without the other noticing.
    CHECK_FALSE(modelImporterNeedsExternalBuffers("statue.blend"));

    // The identity table has no .blend arm either, and that is correct: nothing probes a .blend, so its
    // import-cache entry stays at ("", 0) rather than oscillating.
    CHECK(modelImporterIdentity("statue.blend") == ImporterIdentity{});
}

// ---- task 3.2.5, step 2: the two PURE helpers ------------------------------------------------------
// Both are driven entirely from string literals -- no disk, no Assimp, no locale. scanPlyTextureFiles
// IS the .ply Structure pass, and scanColladaAssetSpace IS where .dae's SourceSpace comes from, so
// these batteries are the cheapest place to pin both rules before anything depends on them.

TEST_CASE("model_import: scanPlyTextureFiles reproduces the loader's operand rule VERBATIM (MI134)") {
    // MEASURED against assimp 6.0.4 rather than predicted: the operand is the rest of the line with
    // the LINE TERMINATOR removed and NOTHING else, so a trailing space SURVIVES into the name. The
    // library's own `strlen - 1` reads like an off-by-one and is not one -- the buffer it trims still
    // carries the terminator. These two assertions are the record of that measurement.
    const std::string ply = "ply\ncomment TextureFile cube.png \nend_header\n";
    const std::vector<std::string> names = scanPlyTextureFiles(asBytes(ply), 16);
    REQUIRE(names.size() == 1U);
    CHECK(names[0] == "cube.png ");  // WITH the trailing space -- exactly what the loader hands back

    const std::string tight = "ply\ncomment TextureFile cube.png\nend_header\n";
    const std::vector<std::string> tightNames = scanPlyTextureFiles(asBytes(tight), 16);
    REQUIRE(tightNames.size() == 1U);
    CHECK(tightNames[0] == "cube.png");
}

TEST_CASE("model_import: an `element`-prefixed line carries the semantic identically (MI135)") {
    const std::string ply = "ply\nelement TextureFile skin.png \nend_header\n";
    const std::vector<std::string> names = scanPlyTextureFiles(asBytes(ply), 16);
    REQUIRE(names.size() == 1U);
    CHECK(names[0] == "skin.png ");

    // A line that begins with neither keyword is not a candidate at all.
    const std::string other = "ply\nproperty TextureFile skin.png \nend_header\n";
    CHECK(scanPlyTextureFiles(asBytes(other), 16).empty());
}

TEST_CASE("model_import: the TextureFile semantic is CASE-SENSITIVE (MI136)") {
    for (const std::string_view spelling : {"texturefile", "TEXTUREFILE", "Texturefile"}) {
        const std::string ply = std::string("ply\ncomment ") + std::string(spelling) + " x.png \nend_header\n";
        CHECK(scanPlyTextureFiles(asBytes(ply), 16).empty());
    }
}

TEST_CASE("model_import: two identical TextureFile lines dedup to one entry (MI137)") {
    const std::string ply = "ply\ncomment TextureFile a.png \ncomment TextureFile a.png \nend_header\n";
    const std::vector<std::string> names = scanPlyTextureFiles(asBytes(ply), 16);
    REQUIRE(names.size() == 1U);
    CHECK(names[0] == "a.png ");
}

TEST_CASE("model_import: two different TextureFile lines both survive, in first-seen order (MI138)") {
    const std::string ply = "ply\ncomment TextureFile a.png \ncomment TextureFile b.png \nend_header\n";
    const std::vector<std::string> names = scanPlyTextureFiles(asBytes(ply), 16);
    // NOTE, and it is a deliberate divergence: the loader keeps only the LAST TextureFile line (each
    // overwrites the previous), while this scan returns BOTH. Over-reporting a dependency the header
    // genuinely names is the safe direction, and the Full pass seeds externalUris from this same scan,
    // so the two depths still agree exactly -- which is the property AC-19 asserts.
    REQUIRE(names.size() == 2U);
    CHECK(names[0] == "a.png ");
    CHECK(names[1] == "b.png ");
}

TEST_CASE("model_import: scanPlyTextureFiles caps at maxNames and does not throw (MI139)") {
    const std::string ply = "ply\ncomment TextureFile a.png \ncomment TextureFile b.png \nend_header\n";
    const std::vector<std::string> one = scanPlyTextureFiles(asBytes(ply), 1);
    REQUIRE(one.size() == 1U);
    CHECK(one[0] == "a.png ");  // the cap keeps the FIRST, never the last
    CHECK(scanPlyTextureFiles(asBytes(ply), 0).empty());
}

TEST_CASE("model_import: CRLF line endings are handled -- one trailing '\\r' stripped (MI140)") {
    // MEASURED: the loader answers `a.png ` for this file too, so stripping exactly one trailing
    // '\r' and nothing else is what keeps the two depths in agreement.
    const std::string ply = "ply\r\ncomment TextureFile a.png \r\nend_header\r\n";
    const std::vector<std::string> names = scanPlyTextureFiles(asBytes(ply), 16);
    REQUIRE(names.size() == 1U);
    CHECK(names[0] == "a.png ");
}

// MI153 (code-review finding 3) -- LEADING WHITESPACE IS SPACE AND TAB, because that is what the library
// means by it. PLY::Element::ParseElement opens with PLY::DOM::SkipSpaces, which forwards to
// Assimp::SkipSpaces, whose test is `(in == ' ' || in == '\t')` (the port's own ParsingUtils.h). This
// scan skipped only ' ', so a TAB-indented header line was invisible to it and visible to the loader:
// Structure returned {} where Full returned {wood.png} -- the depth disagreement AC-19 forbids -- and
// phase 7.5's Structure-depth probe recorded NO dependency, so editing wood.png would never mark the
// model DependencyChanged. The end-to-end half is the `tab.ply` fixture in assimp_import_test.cpp's
// table, which AI10 drives.
TEST_CASE("model_import: a TAB-indented header line is a candidate, exactly as the library reads it (MI153)") {
    const std::string tabbed = "ply\n\tcomment TextureFile wood.png\nend_header\n";
    const std::vector<std::string> names = scanPlyTextureFiles(asBytes(tabbed), 16);
    REQUIRE(names.size() == 1U);
    CHECK(names[0] == "wood.png");

    // The `element` spelling and a MIXED run of spaces and tabs behave identically -- SkipSpaces skips
    // both, in any order, for as long as they last.
    const std::string mixed = "ply\n \t \telement TextureFile skin.png\nend_header\n";
    const std::vector<std::string> mixedNames = scanPlyTextureFiles(asBytes(mixed), 16);
    REQUIRE(mixedNames.size() == 1U);
    CHECK(mixedNames[0] == "skin.png");

    // And an indented `end_header` still bounds the scan, for the same reason: the library reaches its
    // own TokenMatch on that line only after the leading whitespace has been skipped.
    const std::string indentedEnd = "ply\n\tend_header\ncomment TextureFile body.png\n";
    CHECK(scanPlyTextureFiles(asBytes(indentedEnd), 16).empty());
}

TEST_CASE("model_import: nothing after end_header is scanned (MI141)") {
    // BOUNDED BY THE HEADER, which is what makes a 400 MB binary .ply cost a few hundred bytes. A
    // `TextureFile` line in the BODY -- or a byte sequence that happens to look like one -- is invisible.
    const std::string ply = "ply\nend_header\ncomment TextureFile body.png \n";
    CHECK(scanPlyTextureFiles(asBytes(ply), 16).empty());
}

TEST_CASE("model_import: an empty span returns an empty vector without reading (MI142)") {
    CHECK(scanPlyTextureFiles({}, 16).empty());
    const SourceSpace space = scanColladaAssetSpace({});
    CHECK_FALSE(space.declared);
}

TEST_CASE("model_import: scanColladaAssetSpace reads a Z-up centimetre <asset> block (MI143)") {
    const std::string dae =
        "<?xml version=\"1.0\"?>\n<COLLADA version=\"1.4.1\">\n  <asset>\n"
        "    <unit meter=\"0.01\" name=\"centimeter\"/>\n    <up_axis>Z_UP</up_axis>\n"
        "  </asset>\n</COLLADA>\n";
    const SourceSpace space = scanColladaAssetSpace(asBytes(dae));
    CHECK(space.declared);
    CHECK(space.unitMeters == doctest::Approx(0.01F));
    CHECK(space.upAxis == 'Z');
}

TEST_CASE("model_import: scanColladaAssetSpace reads a Y-up metre <asset> block (MI144)") {
    const std::string dae =
        "<COLLADA>\n  <asset>\n    <unit meter=\"1\" name=\"meter\"/>\n"
        "    <up_axis>Y_UP</up_axis>\n  </asset>\n</COLLADA>\n";
    const SourceSpace space = scanColladaAssetSpace(asBytes(dae));
    CHECK(space.declared);
    CHECK(space.unitMeters == doctest::Approx(1.0F));
    CHECK(space.upAxis == 'Y');
}

TEST_CASE("model_import: a .dae with no <asset> block declares nothing (MI145)") {
    const std::string dae = "<COLLADA version=\"1.4.1\">\n  <library_geometries/>\n</COLLADA>\n";
    const SourceSpace space = scanColladaAssetSpace(asBytes(dae));
    CHECK_FALSE(space.declared);
    CHECK(space.unitMeters == doctest::Approx(1.0F));  // the struct's own defaults, untouched
    CHECK(space.upAxis == 'Y');
    CHECK(space.generator.empty());
    CHECK(space.formatVersion.empty());
}

TEST_CASE("model_import: .ply and .stl text declare no space at all (MI146)") {
    // AC-36's second half. Neither format declares a unit or an axis, and inventing one is the option
    // this task's scoping deliberately rejected -- the Source Space row means something precisely
    // because it is ABSENT when the format declares nothing.
    const std::string ply = "ply\nformat ascii 1.0\ncomment TextureFile a.png \nend_header\n";
    CHECK_FALSE(scanColladaAssetSpace(asBytes(ply)).declared);
    const std::string stl = "solid Cube\nfacet normal 0 0 1\nendfacet\nendsolid Cube\n";
    CHECK_FALSE(scanColladaAssetSpace(asBytes(stl)).declared);
}

TEST_CASE("model_import: a <unit> with only name= leaves unitMeters at 1 and declares nothing (MI147)") {
    const std::string dae = "<COLLADA>\n  <asset>\n    <unit name=\"centimeter\"/>\n  </asset>\n</COLLADA>\n";
    const SourceSpace space = scanColladaAssetSpace(asBytes(dae));
    CHECK_FALSE(space.declared);
    CHECK(space.unitMeters == doctest::Approx(1.0F));
}

TEST_CASE("model_import: meter=\"0.01abc\" is rejected whole (MI148)") {
    // The `parsed.ptr == last` check. A trailing-garbage value is NOT read as 0.01 with the tail
    // ignored -- from_chars would happily stop at 'a', and accepting that would silently believe a
    // number the document never wrote.
    const std::string dae = "<COLLADA>\n  <asset>\n    <unit meter=\"0.01abc\"/>\n  </asset>\n</COLLADA>\n";
    const SourceSpace space = scanColladaAssetSpace(asBytes(dae));
    CHECK_FALSE(space.declared);
    CHECK(space.unitMeters == doctest::Approx(1.0F));
}

TEST_CASE("model_import: a non-finite or non-positive unit is rejected (MI149)") {
    for (const std::string_view value : {"0", "-1", "nan", "inf"}) {
        const std::string dae = std::string("<COLLADA>\n  <asset>\n    <unit meter=\"") + std::string(value) +
                                "\"/>\n  </asset>\n</COLLADA>\n";
        const SourceSpace space = scanColladaAssetSpace(asBytes(dae));
        CHECK_FALSE(space.declared);
        CHECK(space.unitMeters == doctest::Approx(1.0F));
    }
}

TEST_CASE("model_import: maxBytes is honoured -- an <asset> block beyond the bound is invisible (MI150)") {
    const std::string padding(4096, ' ');
    const std::string dae =
        "<COLLADA>" + padding + "<asset><unit meter=\"0.01\"/><up_axis>Z_UP</up_axis></asset></COLLADA>";
    CHECK(scanColladaAssetSpace(asBytes(dae)).declared);            // the default 64 KiB bound sees it
    CHECK_FALSE(scanColladaAssetSpace(asBytes(dae), 32).declared);  // a 32-byte bound does not
}

TEST_CASE("model_import: <up_axis> alone still declares, with unitMeters at 1 (MI151)") {
    // The HONEST row: the file said "Z-up, and nothing about units", so the panel reads
    // "Z-up, 1 m/unit", which is exactly what the file said.
    const std::string dae = "<COLLADA>\n  <asset>\n    <up_axis>Z_UP</up_axis>\n  </asset>\n</COLLADA>\n";
    const SourceSpace space = scanColladaAssetSpace(asBytes(dae));
    CHECK(space.declared);
    CHECK(space.upAxis == 'Z');
    CHECK(space.unitMeters == doctest::Approx(1.0F));
}

// MI152 (R8) -- the pre-allocation bound, at the pure level. The behavioural half is AI13, which
// takes ~3 s WITH this check and ground for over SEVENTEEN MINUTES at unbounded and climbing RSS
// without it: the PLY loader works from the DECLARED count and never compares it against the file
// size (unlike STLLoader.cpp, which refuses at `mFileSize < 84 + mNumFaces * 50` before allocating).
// Every honest shape below must pass, because a pre-check that rejects a real file is worse than the
// hang it prevents.
TEST_CASE("model_import: plyDeclaredCountsExceedBytes refuses a lying header, passes honest ones (MI152)") {
    constexpr std::string_view HEADER = "ply\nformat ascii 1.0\n";

    SUBCASE("the pathological header AI13 drives: 2^32-1 vertices, empty body") {
        const std::string lying = std::string(HEADER) +
                                  "element vertex 4294967295\nproperty float x\nproperty float y\n"
                                  "property float z\nend_header\n";
        CHECK(plyDeclaredCountsExceedBytes(asBytes(lying)));
    }
    SUBCASE("a count that would OVERFLOW a 64-bit accumulator still refuses, never wraps small") {
        const std::string overflow = std::string(HEADER) + "element vertex 99999999999999999999999999\nend_header\n";
        CHECK(plyDeclaredCountsExceedBytes(asBytes(overflow)));
    }
    SUBCASE("counts are SUMMED across elements -- neither alone exceeds, together they do") {
        const std::string split = std::string(HEADER) + "element vertex 20\nelement face 20\nend_header\n" +
                                  std::string(30, 'x');  // 40 declared, 30 bytes present
        CHECK(plyDeclaredCountsExceedBytes(asBytes(split)));
    }

    SUBCASE("an HONEST body passes -- one byte per instance is the weakest defensible bound") {
        const std::string honest = std::string(HEADER) + "element vertex 3\nend_header\n0 0 0\n1 0 0\n0 1 0\n";
        CHECK_FALSE(plyDeclaredCountsExceedBytes(asBytes(honest)));
    }
    SUBCASE("exactly enough bytes passes -- the comparison is >, never >=") {
        const std::string exact = std::string(HEADER) + "element vertex 4\nend_header\nabcd";
        CHECK_FALSE(plyDeclaredCountsExceedBytes(asBytes(exact)));
    }
    SUBCASE("a zero-element header passes, and so does one declaring no elements at all") {
        CHECK_FALSE(plyDeclaredCountsExceedBytes(asBytes(std::string(HEADER) + "element vertex 0\nend_header\n")));
        CHECK_FALSE(plyDeclaredCountsExceedBytes(asBytes(std::string(HEADER) + "end_header\n")));
    }
    SUBCASE("CRLF line endings are handled, exactly as scanPlyTextureFiles handles them") {
        const std::string crlf = "ply\r\nformat ascii 1.0\r\nelement vertex 900\r\nend_header\r\n";
        CHECK(plyDeclaredCountsExceedBytes(asBytes(crlf)));
    }
    SUBCASE("a TAB-indented element line is counted -- the same whitespace rule as MI153's") {
        // Code-review finding 3's second site. Here skipping only ' ' failed SAFE -- an indented element
        // line was simply not counted, so the check UNDER-counted and could never reject an honest file
        // -- but the two scans read the same header and a reader who checks one must find the other
        // agrees. Assimp::SkipSpaces is the rule for both.
        const std::string tabbed = "ply\r\nformat ascii 1.0\r\n\telement vertex 900\r\nend_header\r\n";
        CHECK(plyDeclaredCountsExceedBytes(asBytes(tabbed)));
    }
    SUBCASE("`elementary` is not `element` -- the keyword needs its trailing whitespace") {
        const std::string decoy = std::string(HEADER) + "elementary 4294967295\nend_header\n";
        CHECK_FALSE(plyDeclaredCountsExceedBytes(asBytes(decoy)));
    }
    SUBCASE("no end_header at all: no verdict, because there is no body to compare against") {
        // A truncated-but-honest file must reach the loader and get the loader's own message.
        CHECK_FALSE(plyDeclaredCountsExceedBytes(asBytes(std::string(HEADER) + "element vertex 4294967295\n")));
        CHECK_FALSE(plyDeclaredCountsExceedBytes(std::span<const std::byte>{}));
    }
}

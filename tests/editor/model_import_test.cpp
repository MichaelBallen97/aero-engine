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

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

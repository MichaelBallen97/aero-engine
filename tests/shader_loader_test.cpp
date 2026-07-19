// Aero Engine — shader loader tests, tier 0 (task 0.4.4; AC-1, AC-2). Pure: parseShaderMetadata and
// selectArtifact need no GPU, no SDL, and (mostly) no artifacts — they run on every lane. The one
// exception, guarded by AERO_SHADER_TOOLS_ENABLED, reads the REAL triangle.vert.json aero_shaderc
// produces (no GPU either) to prove producer/consumer agreement on real bytes (F11).
#include <aero/rhi/shader_loader.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
// <ostream> is load-bearing on MSVC: doctest stringifies the std::string_view operands below
// (ShaderArtifact::extension/entryPoint) via the stdlib's operator<<(std::ostream&, std::string_view),
// which MS STL defines inline in <string_view> against an INCOMPLETE std::basic_ostream — only
// <ostream> completes it (rhi_device_test.cpp / rhi_format_test.cpp precedent).
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

// docs/04 forbids `using namespace` in HEADERS; this is a test TU (rhi_types_test.cpp precedent).
using namespace engine::rhi;

namespace {

// Shared assertion battery for the two "happy" cases below (real-vs-reordered): if this passes for
// both, the parser is proven key-order- and whitespace-insensitive, and the non-zero counts prove the
// fields are READ, not merely defaulted (the shipped triangle's all-zero counts can't distinguish
// "parsed" from "defaulted" — this crafted object can).
void checkHappyTriangleVertexMetadata(const ShaderMetadata& meta) {
    CHECK(meta.stage == ShaderStage::Vertex);
    CHECK(meta.name == "triangle.vert");
    CHECK(meta.entryPoint == "main");
    CHECK(meta.mslEntryPoint == "main0");
    CHECK(meta.samplerCount == 1);
    CHECK(meta.storageTextureCount == 0);
    CHECK(meta.storageBufferCount == 0);
    CHECK(meta.uniformBufferCount == 2);
    CHECK(meta.hasSpirv);
    CHECK(meta.hasMsl);
    CHECK(meta.hasDxil);
    CHECK(meta.toolchain == "sdl-shadercross@1ca46e0e");
}

}  // namespace

// -------------------------------------------------------------------------------------------
// parseShaderMetadata — happy paths (AC-1)
// -------------------------------------------------------------------------------------------

TEST_CASE("shader loader: parseShaderMetadata reads a crafted schema-1 object with non-zero counts") {
    const std::string_view json = R"({
        "schema": 1,
        "toolchain": "sdl-shadercross@1ca46e0e",
        "name": "triangle.vert",
        "stage": "vertex",
        "entryPoint": "main",
        "mslEntryPoint": "main0",
        "samplerCount": 1,
        "storageTextureCount": 0,
        "storageBufferCount": 0,
        "uniformBufferCount": 2,
        "formats": ["spirv", "msl", "dxil"]
    })";
    const std::optional<ShaderMetadata> meta = parseShaderMetadata(json);
    REQUIRE(meta.has_value());
    checkHappyTriangleVertexMetadata(*meta);
}

TEST_CASE("shader loader: parseShaderMetadata is key-order/whitespace-insensitive and skips unknown keys") {
    // Same logical object as above: keys reordered, irregular whitespace/blank lines, and one unknown
    // ADDITIVE key ("debugSourceHash") thrown in — must parse to an IDENTICAL result (forward compat).
    const std::string_view json = R"({
    "formats":["spirv" ,  "msl","dxil"] ,


        "uniformBufferCount":2,
    "samplerCount":1,"debugSourceHash":  "abc123",
            "storageBufferCount":0,
    "mslEntryPoint":"main0","entryPoint":"main",
    "stage":"vertex",
    "name":"triangle.vert",
    "toolchain":"sdl-shadercross@1ca46e0e","storageTextureCount":0,
    "schema" :1
    })";
    const std::optional<ShaderMetadata> meta = parseShaderMetadata(json);
    REQUIRE(meta.has_value());
    checkHappyTriangleVertexMetadata(*meta);
}

#if AERO_SHADER_TOOLS_ENABLED
TEST_CASE("shader loader: parseShaderMetadata reads the real triangle.vert.json (producer/consumer agreement)") {
    const std::string path = AERO_SHADERS_DIR "/triangle.vert.json";
    std::ifstream stream{path, std::ios::binary};
    REQUIRE_MESSAGE(stream.is_open(), "expected aero_shaderc to have produced ", path);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string json = buffer.str();

    const std::optional<ShaderMetadata> meta = parseShaderMetadata(json);
    REQUIRE(meta.has_value());
    CHECK(meta->stage == ShaderStage::Vertex);
    CHECK(meta->entryPoint == "main");
    CHECK(meta->mslEntryPoint == "main0");
    CHECK(meta->hasSpirv);
    CHECK(meta->hasMsl);
    CHECK(meta->hasDxil);
    // F11: triangle.vert/frag are zero-resource (SV_VertexID vertex-pulling, varying passthrough) —
    // all four reflected counts are 0 for this pair.
    CHECK(meta->samplerCount == 0);
    CHECK(meta->storageTextureCount == 0);
    CHECK(meta->storageBufferCount == 0);
    CHECK(meta->uniformBufferCount == 0);
}
#endif

// -------------------------------------------------------------------------------------------
// parseShaderMetadata — the malformed battery (AC-1): every case must yield nullopt.
// -------------------------------------------------------------------------------------------

TEST_CASE("shader loader: parseShaderMetadata rejects a non-object root") {
    CHECK_FALSE(parseShaderMetadata("[]").has_value());
    CHECK_FALSE(parseShaderMetadata("42").has_value());
    CHECK_FALSE(parseShaderMetadata("").has_value());
}

TEST_CASE("shader loader: parseShaderMetadata enforces schema == 1") {
    // schema missing entirely.
    const std::string_view missingSchema = R"({
        "toolchain": "t", "name": "n", "stage": "vertex", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": 0, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": ["spirv"]
    })";
    CHECK_FALSE(parseShaderMetadata(missingSchema).has_value());

    // schema present but wrong VALUE (a future/unknown schema version).
    const std::string_view wrongSchemaValue = R"({
        "schema": 2, "toolchain": "t", "name": "n", "stage": "vertex", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": 0, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": ["spirv"]
    })";
    CHECK_FALSE(parseShaderMetadata(wrongSchemaValue).has_value());

    // schema present but wrong TYPE (a JSON string instead of an integer).
    const std::string_view wrongSchemaType = R"({
        "schema": "1", "toolchain": "t", "name": "n", "stage": "vertex", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": 0, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": ["spirv"]
    })";
    CHECK_FALSE(parseShaderMetadata(wrongSchemaType).has_value());
}

TEST_CASE("shader loader: parseShaderMetadata enforces required keys and rejects duplicates") {
    // "stage" dropped entirely.
    const std::string_view missingStage = R"({
        "schema": 1, "toolchain": "t", "name": "n", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": 0, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": ["spirv"]
    })";
    CHECK_FALSE(parseShaderMetadata(missingStage).has_value());

    // "stage" appears twice.
    const std::string_view duplicateStage = R"({
        "schema": 1, "toolchain": "t", "name": "n", "stage": "vertex", "stage": "fragment",
        "entryPoint": "main", "mslEntryPoint": "main0", "samplerCount": 0, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": ["spirv"]
    })";
    CHECK_FALSE(parseShaderMetadata(duplicateStage).has_value());
}

TEST_CASE("shader loader: parseShaderMetadata validates the stage token") {
    const std::string_view unknownStage = R"({
        "schema": 1, "toolchain": "t", "name": "n", "stage": "compute", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": 0, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": ["spirv"]
    })";
    CHECK_FALSE(parseShaderMetadata(unknownStage).has_value());
}

TEST_CASE("shader loader: parseShaderMetadata validates count fields") {
    const std::string_view negative = R"({
        "schema": 1, "toolchain": "t", "name": "n", "stage": "vertex", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": -1, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": ["spirv"]
    })";
    CHECK_FALSE(parseShaderMetadata(negative).has_value());

    const std::string_view fractional = R"({
        "schema": 1, "toolchain": "t", "name": "n", "stage": "vertex", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": 1.5, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": ["spirv"]
    })";
    CHECK_FALSE(parseShaderMetadata(fractional).has_value());

    const std::string_view nonNumeric = R"({
        "schema": 1, "toolchain": "t", "name": "n", "stage": "vertex", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": "four", "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": ["spirv"]
    })";
    CHECK_FALSE(parseShaderMetadata(nonNumeric).has_value());

    // One past UINT32_MAX (4294967295) — must overflow-reject, not silently truncate.
    const std::string_view overflow = R"({
        "schema": 1, "toolchain": "t", "name": "n", "stage": "vertex", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": 4294967296, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": ["spirv"]
    })";
    CHECK_FALSE(parseShaderMetadata(overflow).has_value());
}

TEST_CASE("shader loader: parseShaderMetadata validates the formats array") {
    const std::string_view emptyFormats = R"({
        "schema": 1, "toolchain": "t", "name": "n", "stage": "vertex", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": 0, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": []
    })";
    CHECK_FALSE(parseShaderMetadata(emptyFormats).has_value());

    const std::string_view unknownToken = R"({
        "schema": 1, "toolchain": "t", "name": "n", "stage": "vertex", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": 0, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": ["dxbc"]
    })";
    CHECK_FALSE(parseShaderMetadata(unknownToken).has_value());

    const std::string_view notAnArray = R"({
        "schema": 1, "toolchain": "t", "name": "n", "stage": "vertex", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": 0, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": "spirv"
    })";
    CHECK_FALSE(parseShaderMetadata(notAnArray).has_value());
}

TEST_CASE("shader loader: parseShaderMetadata rejects structural corruption") {
    // Truncated: a well-formed prefix with the closing '}' (and everything after the formats array)
    // chopped off.
    const std::string_view truncated = R"({
        "schema": 1, "toolchain": "t", "name": "n", "stage": "vertex", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": 0, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": ["spirv"])";
    CHECK_FALSE(parseShaderMetadata(truncated).has_value());

    // A bad escape: the only two escapes a well-formed sidecar ever contains are \" and \\ (F1); \x is
    // neither.
    const std::string_view badEscape = R"({
        "schema": 1, "toolchain": "t", "name": "triangle\x.vert", "stage": "vertex", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": 0, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": ["spirv"]
    })";
    CHECK_FALSE(parseShaderMetadata(badEscape).has_value());

    // Trailing garbage after the closing '}'.
    const std::string_view trailingGarbage = R"({
        "schema": 1, "toolchain": "t", "name": "n", "stage": "vertex", "entryPoint": "main",
        "mslEntryPoint": "main0", "samplerCount": 0, "storageTextureCount": 0,
        "storageBufferCount": 0, "uniformBufferCount": 0, "formats": ["spirv"]
    }  garbage)";
    CHECK_FALSE(parseShaderMetadata(trailingGarbage).has_value());
}

// -------------------------------------------------------------------------------------------
// selectArtifact — all three format arms (AC-2, the crux robustness win of D3)
// -------------------------------------------------------------------------------------------

TEST_CASE("shader loader: selectArtifact picks the right sidecar + entry point per format") {
    ShaderMetadata meta{};
    meta.entryPoint = "main";
    meta.mslEntryPoint = "main0";
    meta.hasSpirv = true;
    meta.hasDxil = true;
    meta.hasMsl = true;

    const std::optional<ShaderArtifact> spirv = selectArtifact(meta, ShaderFormat::SpirV);
    REQUIRE(spirv.has_value());
    CHECK(spirv->extension == ".spv");
    CHECK(spirv->entryPoint == "main");

    const std::optional<ShaderArtifact> dxil = selectArtifact(meta, ShaderFormat::Dxil);
    REQUIRE(dxil.has_value());
    CHECK(dxil->extension == ".dxil");
    CHECK(dxil->entryPoint == "main");

    const std::optional<ShaderArtifact> msl = selectArtifact(meta, ShaderFormat::Msl);
    REQUIRE(msl.has_value());
    CHECK(msl->extension == ".msl");
    CHECK(msl->entryPoint == "main0");
}

TEST_CASE("shader loader: selectArtifact returns nullopt for a format absent from meta.formats") {
    ShaderMetadata spirvOnly{};
    spirvOnly.hasSpirv = true;
    spirvOnly.hasDxil = false;
    spirvOnly.hasMsl = false;
    CHECK_FALSE(selectArtifact(spirvOnly, ShaderFormat::Msl).has_value());
    CHECK_FALSE(selectArtifact(spirvOnly, ShaderFormat::Dxil).has_value());
}

// tests/render_material_test.cpp — task 3.4.1: the render-side material system (PB*).
//
// Tier 0 (no GPU, every lane): the primitive generators' tangent/UV invariants, the cooked ->
// rhi format mapping's totality, the upload-size formula cross-checked against docs/09 section 10's
// own level arithmetic, the default material's params, and the three built-in default texels.
// Tier 1 (a real Device, NO window — RenderTarget supplies the formats, gated by AERO_SKIP_OR_FAIL):
// the cooked-texture bridge per block family, the committed golden uploaded verbatim, the two
// D3-refusal goldens, and the registry's lifecycle / stale-handle / sampler-dedup behaviour.
//
// primitives.hpp and material_pack.hpp are PRIVATE to engine/render (src/, never installed), so they
// are reached by a relative include — the tests/editor/blender_service_test.cpp precedent. The SYMBOLS
// come from aero_render, which aero_tests already links; no link-line and no include-directory change.
//
// <ostream> is included preventively: MSVC alone needs the complete type to stringify a string_view
// inside a doctest CHECK (the four-time trap in .claude/rules/ci-portability.md). Enum comparisons
// use double parentheses, because engine::rhi::toString is found by ADL from doctest's stringifier.
//
// Every case-local table pins a LITERAL row count, never TABLE.size() against itself: a guard derived
// from the table it guards cannot see a row deleted (3.3.2's anti-vacuity lesson). The tables are
// `constexpr std::array` with CTAD and no explicit size, so a deleted row SHRINKS the array and the
// literal count reddens.

#include <aero/assets/cooked_texture.hpp>
#include <aero/assets/texture_cook.hpp>
#include <aero/platform/platform.hpp>
#include <aero/render/render.hpp>
#include <aero/rhi/rhi.hpp>

#include "../engine/render/src/material_pack.hpp"
#include "../engine/render/src/primitives.hpp"
#include "cooked_texture_golden.hpp"  // the frozen byte goldens, shared with the cooked-texture suite
#include "rhi_test_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

using engine::Vec2;
using engine::Vec3;
using engine::Vec4;
using engine::assets::CookedTextureFormat;
using engine::render::MeshVertex;
using engine::rhi::TextureFormat;

namespace {

// The generator invariants a broken tangent frame must trip (task 3.4.1, R6). Tolerances are loose
// enough for float trig at 17 sphere rings and tight enough that a WRONG axis (the S21 seed: sphere
// tangents forced to +X, whose dot with the normal reaches 1) cannot slip through.
constexpr float ORTHOGONALITY_TOLERANCE = 1e-4F;
constexpr float UNIT_TOLERANCE = 1e-4F;

void checkVertexInvariants(const MeshVertex& vertex, std::string_view primitive) {
    INFO("primitive: ", primitive);
    const Vec3 tangent{vertex.tangent.x, vertex.tangent.y, vertex.tangent.z};
    CHECK(std::abs(engine::dot(engine::normalize(vertex.normal), tangent)) <= ORTHOGONALITY_TOLERANCE);
    CHECK(std::abs(engine::length(tangent) - 1.0F) <= UNIT_TOLERANCE);
    CHECK(vertex.tangent.w == 1.0F);
    CHECK(vertex.uv.x >= 0.0F);
    CHECK(vertex.uv.x <= 1.0F);
    CHECK(vertex.uv.y >= 0.0F);
    CHECK(vertex.uv.y <= 1.0F);
}

}  // namespace

TEST_CASE("render material: the 48-byte vertex layout is what the pipeline describes (PB3)") {
    // The offsets ForwardRenderer::create hands to rhi::VertexAttribute, restated as literals: a
    // silent member reorder in mesh.hpp would move them and the pipeline would read garbage.
    CHECK(sizeof(MeshVertex) == 48);
    CHECK(offsetof(MeshVertex, position) == 0);
    CHECK(offsetof(MeshVertex, normal) == 12);
    CHECK(offsetof(MeshVertex, tangent) == 24);
    CHECK(offsetof(MeshVertex, uv) == 40);
}

TEST_CASE("render material: cube/sphere/plane generate a legal tangent frame and bounded UVs (PB3)") {
    const engine::render::detail::PrimitiveGeometry cube = engine::render::detail::makeCube();
    const engine::render::detail::PrimitiveGeometry sphere = engine::render::detail::makeSphere();
    const engine::render::detail::PrimitiveGeometry plane = engine::render::detail::makePlane();

    // Literal counts, never .size() against itself: an emptied generator must redden here.
    CHECK(cube.vertices.size() == 24);
    CHECK(sphere.vertices.size() == 289);
    CHECK(plane.vertices.size() == 4);

    for (const MeshVertex& vertex : cube.vertices) {
        checkVertexInvariants(vertex, "cube");
    }
    for (const MeshVertex& vertex : sphere.vertices) {
        checkVertexInvariants(vertex, "sphere");
    }
    for (const MeshVertex& vertex : plane.vertices) {
        checkVertexInvariants(vertex, "plane");
    }
}

TEST_CASE("render material: the cube's per-face UV corners follow the generator's own convention (PB3)") {
    const engine::render::detail::PrimitiveGeometry cube = engine::render::detail::makeCube();
    REQUIRE(cube.vertices.size() == 24);

    // appendQuad emits (-u-v, +u-v, +u+v, -u+v) and assigns (0,1), (1,1), (1,0), (0,0): texture u runs
    // along +u and texture v runs DOWN along -v, so the image's (0,0) is the -u+v corner. This is
    // orientation WITHIN the generator's convention, which is tier-0 checkable; whether the checker
    // then reads upright ON SCREEN is the validation page's row (the S22 seed's only witness).
    constexpr std::array<Vec2, 4> EXPECTED_CORNER_UVS{Vec2{0.0F, 1.0F}, Vec2{1.0F, 1.0F}, Vec2{1.0F, 0.0F},
                                                      Vec2{0.0F, 0.0F}};
    CHECK(EXPECTED_CORNER_UVS.size() == 4);

    for (std::size_t face = 0; face < 6; ++face) {
        INFO("face: ", face);
        for (std::size_t corner = 0; corner < EXPECTED_CORNER_UVS.size(); ++corner) {
            CHECK(cube.vertices[(face * 4) + corner].uv == EXPECTED_CORNER_UVS[corner]);
        }
    }

    // The +X face is faces[0] (normal +X, u = -Z, v = +Y): its tangent must be the face's own u axis,
    // and its (0,0) texel must sit on the -u+v corner, i.e. at (+0.5, +0.5, +0.5).
    CHECK(cube.vertices[3].uv == Vec2{0.0F, 0.0F});
    CHECK(cube.vertices[3].position == Vec3{0.5F, 0.5F, 0.5F});
    CHECK(cube.vertices[3].tangent == Vec4{0.0F, 0.0F, -1.0F, 1.0F});
}

TEST_CASE("render material: the plane's tangent is +X and its normal invariant survives the axis reorder (PB3)") {
    const engine::render::detail::PrimitiveGeometry plane = engine::render::detail::makePlane();
    REQUIRE(plane.vertices.size() == 4);
    for (const MeshVertex& vertex : plane.vertices) {
        CHECK(vertex.normal == Vec3{0.0F, 1.0F, 0.0F});
        CHECK(vertex.tangent == Vec4{1.0F, 0.0F, 0.0F, 1.0F});
    }
    // u x v == normal still holds for (u=+X, v=-Z): the quad spans x in [-0.5, 0.5], z in [-0.5, 0.5]
    // and its (0,0) texel is the -u+v corner, (-0.5, 0, -0.5).
    CHECK(plane.vertices[3].uv == Vec2{0.0F, 0.0F});
    CHECK(plane.vertices[3].position == Vec3{-0.5F, 0.0F, -0.5F});
}

TEST_CASE("render material: the sphere's UVs span the full [0,1] range in both axes (PB3)") {
    const engine::render::detail::PrimitiveGeometry sphere = engine::render::detail::makeSphere();
    REQUIRE(sphere.vertices.size() == 289);
    // North pole row: v == 0 and u sweeps 0 -> 1 across the 17 columns (the last is the duplicated
    // seam). A generator that forgot the seam column, or one that indexed rings and sectors the wrong
    // way round, cannot satisfy both ends.
    CHECK(sphere.vertices[0].uv == Vec2{0.0F, 0.0F});
    CHECK(sphere.vertices[16].uv == Vec2{1.0F, 0.0F});
    CHECK(sphere.vertices[288].uv == Vec2{1.0F, 1.0F});
    CHECK(sphere.vertices[272].uv == Vec2{0.0F, 1.0F});
}

// ================================================================================================
// Tier 0 — the cooked-texture format mapping, the upload-size arithmetic, the material defaults.
// ================================================================================================

namespace {

struct FormatMappingRow {
    CookedTextureFormat cooked;
    TextureFormat rhi;
};

struct LevelSizeRow {
    TextureFormat format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint64_t expectedBytes;
};

struct DefaultTexelRow {
    std::size_t slotIndex;
    std::array<std::uint8_t, 4> texel;
    TextureFormat format;
};

struct Extent {
    std::uint32_t width;
    std::uint32_t height;
};

struct CutoffRow {
    engine::render::MaterialAlpha alpha;
    float fileCutoff;
    float pushedCutoff;
};

}  // namespace

TEST_CASE("render material: the cooked -> rhi format mapping is total and injective (PB1)") {
    // The mapping RE-STATED as a literal table, deliberately: reading it back out of the same switch
    // under test would agree with any source-side swap. Eight rows for the eight cooked formats.
    constexpr std::array MAPPING_ROWS{
        FormatMappingRow{CookedTextureFormat::Rgba8Unorm, TextureFormat::RGBA8Unorm},
        FormatMappingRow{CookedTextureFormat::Rgba8Srgb, TextureFormat::RGBA8UnormSrgb},
        FormatMappingRow{CookedTextureFormat::Bc1RgbUnorm, TextureFormat::BC1RGBAUnorm},
        FormatMappingRow{CookedTextureFormat::Bc1RgbSrgb, TextureFormat::BC1RGBAUnormSrgb},
        FormatMappingRow{CookedTextureFormat::Bc3Unorm, TextureFormat::BC3RGBAUnorm},
        FormatMappingRow{CookedTextureFormat::Bc3Srgb, TextureFormat::BC3RGBAUnormSrgb},
        FormatMappingRow{CookedTextureFormat::Bc4Unorm, TextureFormat::BC4RUnorm},
        FormatMappingRow{CookedTextureFormat::Bc5Unorm, TextureFormat::BC5RGUnorm},
    };
    CHECK(MAPPING_ROWS.size() == 8);

    for (const FormatMappingRow& row : MAPPING_ROWS) {
        // Double parentheses: engine::rhi::toString(TextureFormat) is found by ADL from doctest's
        // stringifier and would be a hard compile error on every lane without them.
        CHECK((engine::render::cookedTextureToRhiFormat(row.cooked) == row.rhi));
        // Colour space must survive the crossing in BOTH directions — an sRGB cooked format maps to
        // an sRGB rhi format and never the reverse, which is the half a swapped pair of arms breaks.
        CHECK(engine::assets::isSrgbCookedFormat(row.cooked) == engine::rhi::isSrgbFormat(row.rhi));
        // ...and so must the block geometry, which is what makes levelBytes' length and
        // textureLevelByteSize's expectation agree by construction rather than by luck.
        CHECK(engine::assets::cookedTextureBlockBytes(row.cooked) == engine::rhi::texelBlockSize(row.rhi));
        CHECK(engine::assets::cookedTextureBlockWidth(row.cooked) == engine::rhi::texelBlockWidth(row.rhi));
        CHECK(engine::assets::cookedTextureBlockHeight(row.cooked) == engine::rhi::texelBlockHeight(row.rhi));
    }

    // Injective: eight cooked formats, eight DISTINCT rhi formats. A swap that maps two cooked values
    // onto one rhi value passes every per-row check above (both would still be BCn, both still sRGB)
    // and fails only here.
    for (std::size_t i = 0; i < MAPPING_ROWS.size(); ++i) {
        for (std::size_t j = i + 1; j < MAPPING_ROWS.size(); ++j) {
            CHECK((MAPPING_ROWS[i].rhi != MAPPING_ROWS[j].rhi));
        }
    }
}

TEST_CASE("render material: textureLevelByteSize matches docs/09 section 10's level arithmetic (PB2)") {
    // Hand-computed against blockBytes * ceil(w/blockW) * ceil(h/blockH). The mip-tail rows (2x2, 1x1)
    // are the whole point: a 2x2 BC1 level is ONE 8-byte block, not four texels' worth of anything.
    // The 5x3 BC5 row reads 32 because ceil(5/4) * ceil(3/4) == 2 blocks of 16 — which the committed
    // 5x3 BC5 golden's own level-0 length confirms independently.
    constexpr std::array LEVEL_SIZE_ROWS{
        LevelSizeRow{TextureFormat::BC1RGBAUnorm, 4, 4, 8},      LevelSizeRow{TextureFormat::BC1RGBAUnorm, 5, 3, 16},
        LevelSizeRow{TextureFormat::BC1RGBAUnorm, 2, 2, 8},      LevelSizeRow{TextureFormat::BC1RGBAUnorm, 1, 1, 8},
        LevelSizeRow{TextureFormat::BC1RGBAUnormSrgb, 8, 8, 32}, LevelSizeRow{TextureFormat::BC3RGBAUnorm, 8, 8, 64},
        LevelSizeRow{TextureFormat::BC3RGBAUnormSrgb, 2, 2, 16}, LevelSizeRow{TextureFormat::BC4RUnorm, 7, 5, 32},
        LevelSizeRow{TextureFormat::BC5RGUnorm, 5, 3, 32},       LevelSizeRow{TextureFormat::RGBA8Unorm, 5, 3, 60},
        LevelSizeRow{TextureFormat::D32Float, 4, 4, 0},          LevelSizeRow{TextureFormat::Invalid, 4, 4, 0},
    };
    CHECK(LEVEL_SIZE_ROWS.size() == 12);

    for (const LevelSizeRow& row : LEVEL_SIZE_ROWS) {
        INFO("format: ", engine::rhi::toString(row.format), " ", row.width, "x", row.height);
        CHECK(engine::rhi::textureLevelByteSize(row.format, row.width, row.height) == row.expectedBytes);
    }

    // The independent half: for every cooked format, the rhi formula must agree with the cooked
    // container's OWN block arithmetic over a range of extents including both mip tails and two
    // partial-block shapes. Two implementations of one rule, cross-checked (INV-M4).
    constexpr std::array COOKED_FORMATS{CookedTextureFormat::Rgba8Unorm,  CookedTextureFormat::Rgba8Srgb,
                                        CookedTextureFormat::Bc1RgbUnorm, CookedTextureFormat::Bc1RgbSrgb,
                                        CookedTextureFormat::Bc3Unorm,    CookedTextureFormat::Bc3Srgb,
                                        CookedTextureFormat::Bc4Unorm,    CookedTextureFormat::Bc5Unorm};
    constexpr std::array EXTENTS{Extent{1, 1}, Extent{2, 2}, Extent{3, 7},  Extent{4, 4},
                                 Extent{5, 3}, Extent{8, 8}, Extent{16, 9}, Extent{32, 32}};
    CHECK(COOKED_FORMATS.size() == 8);
    CHECK(EXTENTS.size() == 8);

    for (const CookedTextureFormat cooked : COOKED_FORMATS) {
        const TextureFormat rhiFormat = engine::render::cookedTextureToRhiFormat(cooked);
        const std::uint64_t blockW = engine::assets::cookedTextureBlockWidth(cooked);
        const std::uint64_t blockH = engine::assets::cookedTextureBlockHeight(cooked);
        for (const Extent& extent : EXTENTS) {
            const std::uint64_t blocksX = (extent.width + blockW - 1) / blockW;
            const std::uint64_t blocksY = (extent.height + blockH - 1) / blockH;
            const std::uint64_t cookedBytes = blocksX * blocksY * engine::assets::cookedTextureBlockBytes(cooked);
            CHECK(engine::rhi::textureLevelByteSize(rhiFormat, extent.width, extent.height) == cookedBytes);
        }
    }
}

TEST_CASE("render material: the default material's params, and packMaterial's cutoff rule (PB4)") {
    // Two different defaults for two different questions, both pinned as literals here. The FILE
    // FORMAT keeps glTF's metallic 1 (docs/09 section 11); the RENDER-SIDE fallback is metallic 0,
    // because a metal with no environment renders near-black under analytic lights and v1 has no IBL.
    CHECK(engine::render::MaterialParams{}.metallicFactor == 1.0F);   // the struct default: glTF's
    CHECK(engine::render::MaterialParams{}.roughnessFactor == 1.0F);  //
    CHECK(engine::render::DEFAULT_MATERIAL_PARAMS.metallicFactor == 0.0F);
    CHECK(engine::render::DEFAULT_MATERIAL_PARAMS.roughnessFactor == 1.0F);
    CHECK(engine::render::DEFAULT_MATERIAL_PARAMS.baseColorFactor == Vec4::one());
    CHECK(engine::render::DEFAULT_MATERIAL_PARAMS.emissiveFactor == Vec3{});
    CHECK(engine::render::DEFAULT_MATERIAL_PARAMS.normalScale == 1.0F);
    CHECK(engine::render::DEFAULT_MATERIAL_PARAMS.occlusionStrength == 1.0F);
    CHECK((engine::render::DEFAULT_MATERIAL_PARAMS.alpha == engine::render::MaterialAlpha::Opaque));
    CHECK(engine::render::DEFAULT_MATERIAL_PARAMS.doubleSided == false);

    // The cutoff-packing arm (AC-39). The shader carries ONE `if (alpha < uAlphaCutoff) discard;`
    // and no variant, so "Opaque never discards" is a property of what the CPU pushes, not of the
    // HLSL — and it is the CPU side that must be falsifiable. Mask pushes the material's own cutoff;
    // Opaque and Blend push 0.0, which no texel's alpha can fall below.
    constexpr std::array CUTOFF_ROWS{
        CutoffRow{engine::render::MaterialAlpha::Opaque, 0.75F, 0.0F},
        CutoffRow{engine::render::MaterialAlpha::Mask, 0.75F, 0.75F},
        CutoffRow{engine::render::MaterialAlpha::Blend, 0.75F, 0.0F},
        CutoffRow{engine::render::MaterialAlpha::Mask, 0.0F, 0.0F},
    };
    CHECK(CUTOFF_ROWS.size() == 4);

    for (const CutoffRow& row : CUTOFF_ROWS) {
        INFO("alpha mode index: ", static_cast<int>(row.alpha), " file cutoff: ", row.fileCutoff);
        const engine::render::MaterialParams params{.alpha = row.alpha, .alphaCutoff = row.fileCutoff};
        CHECK(engine::render::detail::packMaterial(params).alphaCutoff == row.pushedCutoff);
    }

    // Everything else rides through untouched — a packer that dropped or transposed a field would
    // push a plausible-looking block, so each member is checked against a DISTINCT value.
    const engine::render::MaterialParams full{.baseColorFactor = Vec4{0.1F, 0.2F, 0.3F, 0.4F},
                                              .emissiveFactor = Vec3{0.5F, 0.6F, 0.7F},
                                              .metallicFactor = 0.125F,
                                              .roughnessFactor = 0.25F,
                                              .normalScale = 0.375F,
                                              .occlusionStrength = 0.5F};
    const engine::render::detail::GpuMaterialParams packed = engine::render::detail::packMaterial(full);
    CHECK(packed.baseColorFactor == Vec4{0.1F, 0.2F, 0.3F, 0.4F});
    CHECK(packed.emissiveFactor == Vec3{0.5F, 0.6F, 0.7F});
    CHECK(packed.metallicFactor == 0.125F);
    CHECK(packed.roughnessFactor == 0.25F);
    CHECK(packed.normalScale == 0.375F);
    CHECK(packed.occlusionStrength == 0.5F);
    // The block the shader's b1 cbuffer declares: 48 bytes, three 16-byte registers, no straddle.
    CHECK(sizeof(engine::render::detail::GpuMaterialParams) == 48);
}

TEST_CASE("render material: the five slots index in D7's binding order (PB4)") {
    engine::render::MaterialTextureSlots slots;
    slots.baseColor.texture = engine::rhi::TextureHandle{10, 1};
    slots.metallicRoughness.texture = engine::rhi::TextureHandle{11, 1};
    slots.normal.texture = engine::rhi::TextureHandle{12, 1};
    slots.occlusion.texture = engine::rhi::TextureHandle{13, 1};
    slots.emissive.texture = engine::rhi::TextureHandle{14, 1};

    CHECK(engine::render::MATERIAL_TEXTURE_SLOT_COUNT == 5);
    for (std::size_t i = 0; i < engine::render::MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        INFO("slot: ", i);
        CHECK(engine::render::materialSlotAt(slots, i).texture.index == 10 + static_cast<std::uint32_t>(i));
    }
}

TEST_CASE("render material: the three built-in 1x1 default texels and their colour spaces (PB10)") {
    // ForwardRenderer::create uploads exactly what defaultTextureTexel returns, so a texel typo here
    // is a red test rather than a silently wrong-looking surface — no pixel readback needed. The
    // expectations are literals: reading them back out of the function under test would prove nothing.
    constexpr std::array DEFAULT_TEXEL_ROWS{
        DefaultTexelRow{0, {0xFF, 0xFF, 0xFF, 0xFF}, TextureFormat::RGBA8UnormSrgb},  // baseColor
        DefaultTexelRow{1, {0xFF, 0xFF, 0xFF, 0xFF}, TextureFormat::RGBA8Unorm},      // metallicRoughness
        DefaultTexelRow{2, {0x80, 0x80, 0xFF, 0xFF}, TextureFormat::RGBA8Unorm},      // normal (flat +Z)
        DefaultTexelRow{3, {0xFF, 0xFF, 0xFF, 0xFF}, TextureFormat::RGBA8Unorm},      // occlusion
        DefaultTexelRow{4, {0xFF, 0xFF, 0xFF, 0xFF}, TextureFormat::RGBA8UnormSrgb},  // emissive
    };
    CHECK(DEFAULT_TEXEL_ROWS.size() == 5);

    for (const DefaultTexelRow& row : DEFAULT_TEXEL_ROWS) {
        INFO("slot: ", row.slotIndex);
        const engine::render::MaterialDefaultTexture entry = engine::render::defaultTextureTexel(row.slotIndex);
        CHECK(entry.texel == row.texel);
        CHECK((entry.format == row.format));
    }
}

// ================================================================================================
// Tier 1 — a real Device, no window. The bridge's first cooked-texture-to-GPU crossing in this
// project's history, and the registry's lifecycle.
// ================================================================================================

namespace {

// A deterministic 8x8 RGBA8 source: a red/blue ramp across, green down, fully opaque. 64 distinct
// texels, so no encoder takes its degenerate flat-block arm by accident, and 8x8 is block-aligned in
// every format here so the top level uploads under the alignment rule while the chain's 2x2 and 1x1
// tail exercises the single-partial-block upload on a real GPU.
[[nodiscard]] std::vector<std::byte> makeSourceTexels() {
    std::vector<std::byte> texels(static_cast<std::size_t>(8U) * 8U * 4U);
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const std::size_t base = ((static_cast<std::size_t>(y) * 8U) + x) * 4U;
            texels[base + 0] = static_cast<std::byte>(255U - (x * 32U));
            texels[base + 1] = static_cast<std::byte>(y * 32U);
            texels[base + 2] = static_cast<std::byte>(x * 32U);
            texels[base + 3] = static_cast<std::byte>(255U);
        }
    }
    return texels;
}

struct CookedFamilyRow {
    CookedTextureFormat format;
    std::uint32_t expectedLevels;
};

}  // namespace

TEST_CASE("render material: every cooked format uploads through the bridge, mip tail included (PB5)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = engine::rhi::Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    // Artifacts COOKED IN MEMORY rather than read from a committed golden: only one of the four
    // frozen goldens (the 4x4 BC1) has a block-aligned top level, so "one committed golden per block
    // family" is not executable — the other two BC goldens are the refusal fixtures below. Cooking
    // here is sound because 3.3.3's manifest makes the cook deterministic, and it commits no fixture.
    constexpr std::array COOKED_FAMILY_ROWS{
        CookedFamilyRow{CookedTextureFormat::Bc1RgbUnorm, 4}, CookedFamilyRow{CookedTextureFormat::Bc1RgbSrgb, 4},
        CookedFamilyRow{CookedTextureFormat::Bc3Unorm, 4},    CookedFamilyRow{CookedTextureFormat::Bc3Srgb, 4},
        CookedFamilyRow{CookedTextureFormat::Bc4Unorm, 4},    CookedFamilyRow{CookedTextureFormat::Bc5Unorm, 4},
        CookedFamilyRow{CookedTextureFormat::Rgba8Unorm, 4},  CookedFamilyRow{CookedTextureFormat::Rgba8Srgb, 4},
    };
    CHECK(COOKED_FAMILY_ROWS.size() == 8);

    const std::vector<std::byte> source = makeSourceTexels();
    for (const CookedFamilyRow& row : COOKED_FAMILY_ROWS) {
        INFO("cooked format: ", engine::assets::toString(row.format));
        const engine::assets::TextureCookResult cooked = engine::assets::cookTexture(
            {.sourceGuid = {}, .width = 8, .height = 8, .rgba8 = source, .format = row.format, .generateMips = true});
        REQUIRE(cooked.status == engine::assets::TextureCookStatus::Ok);

        const engine::assets::CookedTextureParse parse = engine::assets::parseCookedTexture(cooked.bytes);
        REQUIRE(parse.status == engine::assets::CookedTextureStatus::Ok);
        CHECK(parse.view.levelCount() == row.expectedLevels);
        // The tail really is one partial block: at level 3 the extent is 1x1 and the level's own byte
        // length equals one block, which is exactly what uploadTexture validates against.
        CHECK(parse.view.levelWidth(3) == 1);
        CHECK(parse.view.levelBytes(3).size() == engine::assets::cookedTextureBlockBytes(row.format));

        const engine::rhi::TextureHandle texture = engine::render::createTextureFromCookedTexture(*device, parse.view);
        CHECK(texture.valid());
        if (texture.valid()) {
            device->destroyTexture(texture);
        }
    }
}

TEST_CASE("render material: the committed 4x4 BC1 golden uploads verbatim (PB6)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = engine::rhi::Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    // Golden A's bytes as committed — the one literal committed-artifact upload in this suite, and
    // the only place a file this project froze in 3.3.2 reaches a GPU unchanged.
    const engine::assets::CookedTextureParse parse =
        engine::assets::parseCookedTexture(std::as_bytes(std::span{aero_test::COOKED_TEXTURE_GOLDEN_BC1_4X4}));
    REQUIRE(parse.status == engine::assets::CookedTextureStatus::Ok);
    CHECK(parse.view.width() == 4);
    CHECK(parse.view.height() == 4);
    CHECK(parse.view.levelCount() == 3);
    CHECK((parse.view.format() == CookedTextureFormat::Bc1RgbSrgb));

    const engine::rhi::TextureHandle texture = engine::render::createTextureFromCookedTexture(*device, parse.view);
    CHECK(texture.valid());
    if (texture.valid()) {
        device->destroyTexture(texture);
    }
}

TEST_CASE("render material: a cooked artifact with an unaligned top level is refused, not uploaded (PB7)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = engine::rhi::Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    // Golden C (5x3 BC5) and Golden D (2x2 BC3) are PERFECTLY VALID cooked artifacts that this device
    // will not create a texture for: the block-alignment rule is D3D12's, adopted uniformly on every
    // backend rather than shipping a texture that uploads on macOS and fails on Windows. They are the
    // refusal fixtures precisely because they exist.
    const engine::assets::CookedTextureParse bc5 =
        engine::assets::parseCookedTexture(std::as_bytes(std::span{aero_test::COOKED_TEXTURE_GOLDEN_BC5_5X3}));
    REQUIRE(bc5.status == engine::assets::CookedTextureStatus::Ok);
    CHECK(bc5.view.width() == 5);
    CHECK(bc5.view.height() == 3);
    CHECK_FALSE(engine::render::createTextureFromCookedTexture(*device, bc5.view).valid());

    const engine::assets::CookedTextureParse bc3 =
        engine::assets::parseCookedTexture(std::as_bytes(std::span{aero_test::COOKED_TEXTURE_GOLDEN_BC3_SRGB_2X2}));
    REQUIRE(bc3.status == engine::assets::CookedTextureStatus::Ok);
    CHECK(bc3.view.width() == 2);
    CHECK(bc3.view.height() == 2);
    CHECK_FALSE(engine::render::createTextureFromCookedTexture(*device, bc3.view).valid());

    // The 1x1 RGBA8 golden is the CONTRAST that keeps the two refusals honest: a 1x1-block format is
    // trivially aligned, so a bridge that refused everything would redden here.
    const engine::assets::CookedTextureParse rgba8 =
        engine::assets::parseCookedTexture(std::as_bytes(std::span{aero_test::COOKED_TEXTURE_GOLDEN_RGBA8_1X1}));
    REQUIRE(rgba8.status == engine::assets::CookedTextureStatus::Ok);
    const engine::rhi::TextureHandle texture = engine::render::createTextureFromCookedTexture(*device, rgba8.view);
    CHECK(texture.valid());
    if (texture.valid()) {
        device->destroyTexture(texture);
    }
}

#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/core/vfs.hpp>

    #include <memory>
    #include <utility>

namespace {

// A ForwardRenderer needs a colour and a depth format, which a RenderTarget supplies without a window
// (render_target_test.cpp's own pattern). Returns nullopt only when the caller has already gated on a
// live Device, so a nullopt here is a genuine failure rather than an environment skip.
[[nodiscard]] std::optional<engine::render::ForwardRenderer> makeForwardRenderer(
    engine::rhi::Device& device, const engine::render::RenderTarget& target) {
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    return engine::render::ForwardRenderer::create(
        device, vfs, {.colorFormat = target.colorFormat(), .depthFormat = target.depthFormat()});
}

// Cook an 8x8 source in memory and push it through the bridge — the whole chain this task built,
// used here only so the draw has a REAL texture in every slot rather than the built-in defaults.
// Invalid on any failure; the caller REQUIREs validity, so a silent fallback cannot hide.
[[nodiscard]] engine::rhi::TextureHandle makeCookedTexture(engine::rhi::Device& device, CookedTextureFormat format) {
    const std::vector<std::byte> source = makeSourceTexels();
    const engine::assets::TextureCookResult cooked = engine::assets::cookTexture(
        {.sourceGuid = {}, .width = 8, .height = 8, .rgba8 = source, .format = format, .generateMips = true});
    if (cooked.status != engine::assets::TextureCookStatus::Ok) {
        return {};
    }
    const engine::assets::CookedTextureParse parse = engine::assets::parseCookedTexture(cooked.bytes);
    if (parse.status != engine::assets::CookedTextureStatus::Ok) {
        return {};
    }
    return engine::render::createTextureFromCookedTexture(device, parse.view);
}

// One instance at the origin with an identity model matrix, so normalMatrix is identity too (the
// embed(transpose(inverse(toMat3(model)))) scene_render computes, for this trivial case).
[[nodiscard]] engine::render::MeshInstance makeInstance(engine::render::PrimitiveId primitive,
                                                        const engine::Mat4& viewProj,
                                                        engine::render::MaterialHandle material) {
    engine::render::MeshInstance instance;
    instance.primitive = primitive;
    instance.model = engine::Mat4::identity();
    instance.normalMatrix = engine::Mat4::identity();
    instance.mvp = viewProj;
    instance.material = material;
    return instance;
}

// A camera looking at the origin from a fixed eye. eyePosition is the field 1.4.1 carried unused and
// the GGX view vector finally reads, so it is set to the SAME point the view matrix was built from —
// a mismatch there is invisible in every tier-0 case and wrong in every specular highlight.
[[nodiscard]] engine::render::CameraView makeCamera() {
    const Vec3 eye{0.0F, 1.5F, 3.0F};
    return {engine::lookAt(eye, Vec3{}, Vec3{0.0F, 1.0F, 0.0F}),
            engine::perspective(engine::radians(60.0F), 1.0F, 0.1F, 100.0F), eye};
}

}  // namespace

TEST_CASE("render material: registry create/update/destroy, and stale handles are logged no-ops (PB8)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = engine::rhi::Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto target = engine::render::RenderTarget::create(*device, {64, 64});
    REQUIRE(target.has_value());
    auto forward = makeForwardRenderer(*device, *target);
    REQUIRE(forward.has_value());

    const engine::render::MaterialHandle fallback = forward->defaultMaterial();
    CHECK(fallback.valid());

    const engine::render::MaterialParams params{
        .baseColorFactor = Vec4{0.2F, 0.4F, 0.6F, 1.0F}, .metallicFactor = 0.5F, .roughnessFactor = 0.25F};
    const engine::render::MaterialHandle material = forward->createMaterial(params, {});
    CHECK(material.valid());
    CHECK((material != fallback));

    CHECK(forward->updateMaterial(material, engine::render::MaterialParams{}, {}));

    forward->destroyMaterial(material);
    // Stale from here on: update reports false, and a second destroy is a no-op rather than a
    // double-free (ASan is the backstop for the half a bool cannot express).
    CHECK_FALSE(forward->updateMaterial(material, params, {}));
    forward->destroyMaterial(material);

    // A never-minted handle is equally inert.
    CHECK_FALSE(forward->updateMaterial(engine::render::MaterialHandle{99, 7}, params, {}));
    forward->destroyMaterial(engine::render::MaterialHandle{});

    // Destroying the built-in default is a LOGGED NO-OP: it stays live, which is exactly what an
    // invalid MeshInstance::material needs in order to have something to fall back to.
    forward->destroyMaterial(fallback);
    CHECK((forward->defaultMaterial() == fallback));
    CHECK(forward->updateMaterial(fallback, engine::render::DEFAULT_MATERIAL_PARAMS, {}));

    // A slot reused after a destroy mints a NEW generation, so the old handle stays rejected.
    const engine::render::MaterialHandle reused = forward->createMaterial(params, {});
    CHECK(reused.valid());
    CHECK((reused != material));
    CHECK_FALSE(forward->updateMaterial(material, params, {}));
}

TEST_CASE("render material: identical sampler state dedups; mip mode and maxLod are in the key (PB9)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = engine::rhi::Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto target = engine::render::RenderTarget::create(*device, {64, 64});
    REQUIRE(target.has_value());
    auto forward = makeForwardRenderer(*device, *target);
    REQUIRE(forward.has_value());

    // create() pre-resolves the default SamplerDesc, so exactly one entry exists before anything here
    // asks for a sampler. A literal, not a re-read of the value under test.
    CHECK(forward->samplerCacheSize() == 1);

    const engine::render::MaterialHandle first = forward->createMaterial({}, {});
    const engine::render::MaterialHandle second = forward->createMaterial({}, {});
    CHECK(first.valid());
    CHECK(second.valid());
    CHECK(forward->samplerCacheSize() == 1);  // ten default descs, still one handle

    // Vary the MIP MODE alone: a dedup key that omitted it would report 1 here, which is the whole
    // point of the case (a wrong mip mode is invisible until a texture is minified on screen).
    engine::render::MaterialTextureSlots nearestMips;
    nearestMips.baseColor.sampler.mipmapMode = engine::rhi::MipmapMode::Nearest;
    CHECK(forward->createMaterial({}, nearestMips).valid());
    CHECK(forward->samplerCacheSize() == 2);
    CHECK(forward->createMaterial({}, nearestMips).valid());
    CHECK(forward->samplerCacheSize() == 2);  // the second ask hits the cache

    // Vary maxLod alone — the "mipFilter: none" idiom is Nearest + maxLod 0, so a key that stopped at
    // the mip mode would alias clamp-to-base onto plain nearest mipping.
    engine::render::MaterialTextureSlots clampToBase;
    clampToBase.baseColor.sampler.mipmapMode = engine::rhi::MipmapMode::Nearest;
    clampToBase.baseColor.sampler.maxLod = 0.0F;
    CHECK(forward->createMaterial({}, clampToBase).valid());
    CHECK(forward->samplerCacheSize() == 3);

    // Vary the wrap mode on a NON-FIRST slot, proving all five slots run through the same resolver.
    engine::render::MaterialTextureSlots clampedEmissive;
    clampedEmissive.emissive.sampler.addressU = engine::rhi::AddressMode::ClampToEdge;
    CHECK(forward->createMaterial({}, clampedEmissive).valid());
    CHECK(forward->samplerCacheSize() == 4);

    // updateMaterial runs the same resolution path — it may GROW the cache and never shrinks it.
    CHECK(forward->updateMaterial(first, {}, clampedEmissive));
    CHECK(forward->samplerCacheSize() == 4);

    // The Blend latch is renderer state, and nothing has drawn yet: it must read false here. Step 7's
    // draw loop is what sets it, and PB12 is what watches it latch exactly once.
    CHECK_FALSE(forward->hasWarnedBlendOpaque());
}

TEST_CASE("render material: a moved-from ForwardRenderer releases nothing twice (PB8)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = engine::rhi::Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto target = engine::render::RenderTarget::create(*device, {64, 64});
    REQUIRE(target.has_value());
    auto first = makeForwardRenderer(*device, *target);
    REQUIRE(first.has_value());
    const engine::render::MaterialHandle fallback = first->defaultMaterial();

    // The registry, the sampler cache and the three default textures all joined the members a move has
    // to transfer; ASan is what catches a missed one as a double free at scope exit.
    engine::render::ForwardRenderer movedTo = std::move(*first);
    CHECK((movedTo.defaultMaterial() == fallback));
    CHECK(movedTo.samplerCacheSize() == 1);
    CHECK_FALSE(first->defaultMaterial().valid());
    CHECK(first->samplerCacheSize() == 0);

    auto second = makeForwardRenderer(*device, *target);
    REQUIRE(second.has_value());
    movedTo = std::move(*second);  // move-assign over a LIVE renderer: exactly one release of each
    CHECK(movedTo.defaultMaterial().valid());
    CHECK(movedTo.samplerCacheSize() == 1);
    CHECK_FALSE(second->defaultMaterial().valid());
}

TEST_CASE("render material: a five-slot draw pushes BOTH fragment uniform blocks (PB11)") {
    // D11's VERIFY, the runtime half. The cooked sidecars say the fragment stage declares five
    // samplers and TWO uniform buffers (recorded before any visual judgment); this case is what
    // proves HLSL register b1 reaches pushFragmentUniforms(cmd, 1, ...) on a real device. The two
    // blocks are deliberately SIZE-DISTINGUISHABLE — Lights is 320 bytes and MaterialParams is 48 —
    // so a slot-crossed push is a size mismatch the backend surfaces, not a plausible picture.
    // No pixel assertions: this suite records draws without asserting their output (the task's own
    // posture), so the assertion is that recording and submission complete.
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = engine::rhi::Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto target = engine::render::RenderTarget::create(*device, {64, 64});
    REQUIRE(target.has_value());
    auto forward = makeForwardRenderer(*device, *target);
    REQUIRE(forward.has_value());

    // One cooked texture per slot, each in the format the sample cooks that slot in, so every block
    // family this task added reaches a BOUND SAMPLER and not merely an upload. Literal count 5.
    constexpr std::array SLOT_FORMATS{CookedTextureFormat::Bc1RgbSrgb, CookedTextureFormat::Bc1RgbUnorm,
                                      CookedTextureFormat::Bc5Unorm, CookedTextureFormat::Bc4Unorm,
                                      CookedTextureFormat::Bc1RgbSrgb};
    CHECK(SLOT_FORMATS.size() == 5);
    std::array<engine::rhi::TextureHandle, 5> textures{};
    for (std::size_t i = 0; i < SLOT_FORMATS.size(); ++i) {
        INFO("slot: ", i);
        textures[i] = makeCookedTexture(*device, SLOT_FORMATS[i]);
        REQUIRE(textures[i].valid());
    }

    engine::render::MaterialTextureSlots slots;
    slots.baseColor.texture = textures[0];
    slots.metallicRoughness.texture = textures[1];
    slots.normal.texture = textures[2];
    slots.occlusion.texture = textures[3];
    slots.emissive.texture = textures[4];
    const engine::render::MaterialParams params{.baseColorFactor = Vec4{0.0F, 1.0F, 0.0F, 1.0F},
                                                .emissiveFactor = Vec3{0.0F, 0.0F, 0.25F},
                                                .metallicFactor = 0.75F,
                                                .roughnessFactor = 0.3F};
    const engine::render::MaterialHandle mapped = forward->createMaterial(params, slots);
    REQUIRE(mapped.valid());

    // A double-sided Mask material, so the SAME recording exercises the cull-none pipeline, the flip
    // back to cull-back, and a non-zero pushed cutoff.
    engine::render::MaterialTextureSlots maskSlots;
    maskSlots.baseColor.texture = textures[0];
    const engine::render::MaterialHandle masked = forward->createMaterial(
        {.alpha = engine::render::MaterialAlpha::Mask, .alphaCutoff = 0.5F, .doubleSided = true}, maskSlots);
    REQUIRE(masked.valid());

    const engine::render::CameraView camera = makeCamera();
    const engine::Mat4 viewProj = camera.proj * camera.view;
    // Three instances in one view: the five-slot material, the double-sided mask (pipeline flips out
    // and back), and a DEFAULT-INVALID handle that must resolve to the built-in default material.
    const std::array<engine::render::MeshInstance, 3> instances{
        makeInstance(engine::render::PrimitiveId::Sphere, viewProj, mapped),
        makeInstance(engine::render::PrimitiveId::Cube, viewProj, masked),
        makeInstance(engine::render::PrimitiveId::Plane, viewProj, engine::render::MaterialHandle{}),
    };

    engine::render::RenderView view;
    view.camera = camera;
    view.ambient = Vec3{1.0F, 0.0F, 0.0F};  // distinguishable from the material block's green
    view.directional = {.direction = Vec3{-0.5F, -1.0F, -0.3F}, .color = Vec3::one(), .intensity = 2.0F};
    view.instances = instances;

    for (int frame = 0; frame < 2; ++frame) {
        INFO("frame: ", frame);
        std::optional<engine::render::Frame> open = target->beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
        REQUIRE(open.has_value());
        forward->draw(*open, view);
        CHECK(target->endFrame(std::move(*open)));
    }

    CHECK_FALSE(forward->hasWarnedBlendOpaque());  // nothing here is Blend

    // Textures are BORROWED: destroying them is the caller's job and the registry never touches them.
    for (const engine::rhi::TextureHandle texture : textures) {
        device->destroyTexture(texture);
    }
}

TEST_CASE("render material: a Blend material draws opaque behind a latch that fires once (PB12)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = engine::rhi::Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto target = engine::render::RenderTarget::create(*device, {64, 64});
    REQUIRE(target.has_value());
    auto forward = makeForwardRenderer(*device, *target);
    REQUIRE(forward.has_value());

    const engine::render::MaterialHandle blend =
        forward->createMaterial({.alpha = engine::render::MaterialAlpha::Blend}, {});
    REQUIRE(blend.valid());
    // Registering one must NOT warn: the latch belongs to the DRAW path, so a material that is
    // created and never drawn stays silent.
    CHECK_FALSE(forward->hasWarnedBlendOpaque());

    const engine::render::CameraView camera = makeCamera();
    const engine::Mat4 viewProj = camera.proj * camera.view;
    const std::array<engine::render::MeshInstance, 2> instances{
        makeInstance(engine::render::PrimitiveId::Cube, viewProj, blend),
        makeInstance(engine::render::PrimitiveId::Sphere, viewProj, engine::render::MaterialHandle{}),
    };
    engine::render::RenderView view;
    view.camera = camera;
    view.directional = {.direction = Vec3{-0.5F, -1.0F, -0.3F}, .color = Vec3::one(), .intensity = 2.0F};
    view.instances = instances;

    std::optional<engine::render::Frame> first = target->beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(first.has_value());
    forward->draw(*first, view);
    CHECK(target->endFrame(std::move(*first)));
    // Drawn, not refused — Blend renders OPAQUE in v1 — and the latch is now set.
    CHECK(forward->hasWarnedBlendOpaque());

    // A second frame with the same material: the latch stays set and the WARN cannot fire again.
    // "Once per renderer lifetime" is a property of the `if (!warnedBlendOnce)` guard rather than of
    // anything a bool can count, so this arm pins the guard's other half — the latch is never reset.
    std::optional<engine::render::Frame> second = target->beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(second.has_value());
    forward->draw(*second, view);
    CHECK(target->endFrame(std::move(*second)));
    CHECK(forward->hasWarnedBlendOpaque());
}

#endif  // AERO_SHADER_TOOLS_ENABLED

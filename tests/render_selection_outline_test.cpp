// tests/render_selection_outline_test.cpp — task E.1.4: the selection mask's vocabulary, the
// composite's params and packers, the shader source pin (SO1-SO13, every configuration), and the GPU
// mask + outline (OG1-OG17, gated).
//
// TWO TIERS, and the split is the point. SO1-SO13 need NO device and NO shader toolchain: everything
// assertable without a GPU is asserted without one, and both reduced configurations run all of it.
// OG1-OG17 read PIXELS BACK off a real device and live inside the file's ONE #if
// AERO_SHADER_TOOLS_ENABLED, which opens below the SO block and is closed by the file's LAST LINE.
// That gate is not the prohibition CLAUDE.md's test-file rule names -- AERO_SHADERS_DIR is defined
// only inside if(AERO_SHADER_TOOLS), so an ungated GPU block is a HARD COMPILE ERROR in the
// -DAERO_SHADER_TOOLS=OFF configuration. render_tonemap_test.cpp and render_debug_draw_test.cpp are
// the tree's two precedents, in the same shape, for the same reason.

// <aero/render/selection_outline.hpp> IS DELIBERATELY NOT INCLUDED HERE. SO10's whole claim is that
// the UMBRELLA carries it, and with both includes present that case passes on a seeded umbrella and
// proves nothing. Everything below is reached through render.hpp alone.
#include <aero/render/render.hpp>

#include "../engine/render/src/selection_outline_pack.hpp"
#include "../engine/render/src/tonemap_pack.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <ostream>  // MSVC: any CHECK on a string_view needs this (the 0.4.1 trap)
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using engine::Vec2;
using engine::Vec4;
namespace rs = engine::render;

constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
constexpr float INF_F = std::numeric_limits<float>::infinity();

// The R8/sRGB byte of a display channel. THE MULTIPLY IS BY 255.0F, NEVER BY (1.0F / 255.0F): E.1.1
// measured that k * fl(1/255) is bit-unequal to fl(k/255) for 126 of the 256 byte values, and this
// is a round trip.
[[nodiscard]] long expectedByte(float channel) { return std::lround(std::clamp(channel, 0.0F, 1.0F) * 255.0F); }

// Comment-stripped shader source, the JP14/TM29/DD pattern, reached through AERO_SHADERS_SRC_DIR (the
// SOURCE tree, distinct from AERO_SHADERS_DIR's build output). UNGATED: the HLSL text exists whether
// or not AERO_SHADER_TOOLS built it.
[[nodiscard]] std::string strippedSourceAt(std::string_view absolutePath) {
    std::ifstream file{std::string{absolutePath}};
    std::string out;
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t comment = line.find("//");
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        out += line;
        out += '\n';
    }
    return out;
}

// The umbrella header's own path, derived from the SOURCE tree's shaders directory -- the one route
// into the source tree this TU already has (SO9 reads the HLSL through it). Deliberately NOT a new
// compile definition: tests/CMakeLists.txt is held to the two source lines this task adds.
// DD23's own constant, verbatim.
constexpr std::string_view RENDER_UMBRELLA_PATH =
    AERO_SHADERS_SRC_DIR "/../engine/render/include/aero/render/render.hpp";

}  // namespace

TEST_CASE("render selection outline: sanitizeSelectionOutlineParams is TOTAL over hostile input (SO1)") {
    // Every hostile value, placed in EACH of the eight colour channels in turn, and the postcondition
    // asserted on every output channel: finite and in [0, 1]. Totality is the claim, so the table is
    // driven rather than the eight channels written out.
    constexpr std::array<float, 7> HOSTILE{NAN_F, INF_F, -INF_F, -1.0F, 2.0F, std::numeric_limits<float>::min(), -0.0F};
    for (const float bad : HOSTILE) {
        for (int slot = 0; slot < 8; ++slot) {
            rs::SelectionOutlineParams params;
            const std::array<float*, 8> channels{&params.primaryColorSrgb.x,   &params.primaryColorSrgb.y,
                                                 &params.primaryColorSrgb.z,   &params.primaryColorSrgb.w,
                                                 &params.secondaryColorSrgb.x, &params.secondaryColorSrgb.y,
                                                 &params.secondaryColorSrgb.z, &params.secondaryColorSrgb.w};
            *channels[static_cast<std::size_t>(slot)] = bad;
            const rs::SelectionOutlineParams sane = rs::sanitizeSelectionOutlineParams(params);
            const std::array<float, 8> out{sane.primaryColorSrgb.x,   sane.primaryColorSrgb.y,
                                           sane.primaryColorSrgb.z,   sane.primaryColorSrgb.w,
                                           sane.secondaryColorSrgb.x, sane.secondaryColorSrgb.y,
                                           sane.secondaryColorSrgb.z, sane.secondaryColorSrgb.w};
            for (const float v : out) {
                CHECK(std::isfinite(v));
                CHECK(v >= 0.0F);
                CHECK(v <= 1.0F);
            }
        }
    }
}

TEST_CASE("render selection outline: the FINITENESS arm comes FIRST, so a NaN channel is exactly 0 (SO2)") {
    // std::clamp(NaN, 0, 1) RETURNS NaN on libc++ (the 3.7.2 rule, hit again by E.1.2's
    // std::min(NaN, 48.0F)), so a clamp-then-check would let a NaN through into the uniform ring.
    // BOTH assertions are needed: `== 0.0F` alone would pass if clamp happened to return 0, and
    // isfinite alone would pass for any finite value at all.
    rs::SelectionOutlineParams params;
    params.primaryColorSrgb = Vec4{NAN_F, NAN_F, NAN_F, NAN_F};
    params.secondaryColorSrgb = Vec4{NAN_F, 0.5F, NAN_F, 1.0F};
    const rs::SelectionOutlineParams sane = rs::sanitizeSelectionOutlineParams(params);
    CHECK(std::isfinite(sane.primaryColorSrgb.x));
    CHECK(sane.primaryColorSrgb.x == 0.0F);
    CHECK(std::isfinite(sane.primaryColorSrgb.w));
    CHECK(sane.primaryColorSrgb.w == 0.0F);
    CHECK(std::isfinite(sane.secondaryColorSrgb.x));
    CHECK(sane.secondaryColorSrgb.x == 0.0F);
    CHECK(sane.secondaryColorSrgb.y == 0.5F);  // the finite neighbour is untouched
    CHECK(sane.secondaryColorSrgb.w == 1.0F);
}

TEST_CASE("render selection outline: radiusPixels clamps into [MIN, MAX] and 0 becomes MIN (SO3)") {
    const auto radiusOf = [](std::uint32_t requested) {
        rs::SelectionOutlineParams params;
        params.radiusPixels = requested;
        return rs::sanitizeSelectionOutlineParams(params).radiusPixels;
    };
    // 0 takes NO taps at all and would make the outline unconditionally absent -- it must not be
    // passed through.
    CHECK(radiusOf(0U) == rs::SELECTION_OUTLINE_MIN_RADIUS);
    CHECK(radiusOf(9U) == rs::SELECTION_OUTLINE_MAX_RADIUS);
    CHECK(radiusOf(std::numeric_limits<std::uint32_t>::max()) == rs::SELECTION_OUTLINE_MAX_RADIUS);
    for (std::uint32_t r = rs::SELECTION_OUTLINE_MIN_RADIUS; r <= rs::SELECTION_OUTLINE_MAX_RADIUS; ++r) {
        CHECK(radiusOf(r) == r);
    }
    CHECK(rs::SELECTION_OUTLINE_MIN_RADIUS == 1U);
    CHECK(rs::SELECTION_OUTLINE_MAX_RADIUS == 8U);
    // The default is the MIN, so an unconfigured params draws the thinnest legible band.
    CHECK(rs::SelectionOutlineParams{}.radiusPixels == rs::SELECTION_OUTLINE_MIN_RADIUS);
}

TEST_CASE("render selection outline: the two defaults round-trip to the editor's EXACT bytes (SO4)") {
    // D18: the editor derives its ImU32 marker colours from these two constants, so a drift here is a
    // drift in the point marker as well as the band. The multiply is by 255.0F, never a reciprocal.
    CHECK(expectedByte(rs::SELECTION_OUTLINE_PRIMARY_DEFAULT.x) == 255L);
    CHECK(expectedByte(rs::SELECTION_OUTLINE_PRIMARY_DEFAULT.y) == 176L);
    CHECK(expectedByte(rs::SELECTION_OUTLINE_PRIMARY_DEFAULT.z) == 64L);
    CHECK(expectedByte(rs::SELECTION_OUTLINE_PRIMARY_DEFAULT.w) == 255L);
    CHECK(expectedByte(rs::SELECTION_OUTLINE_SECONDARY_DEFAULT.x) == 255L);
    CHECK(expectedByte(rs::SELECTION_OUTLINE_SECONDARY_DEFAULT.y) == 148L);
    CHECK(expectedByte(rs::SELECTION_OUTLINE_SECONDARY_DEFAULT.z) == 32L);
    CHECK(expectedByte(rs::SELECTION_OUTLINE_SECONDARY_DEFAULT.w) == 190L);
    // ...and the two are DISTINCT in both the alpha and the green channel, which is what makes the
    // primary readable as "brighter and opaque" at a glance.
    CHECK(rs::SELECTION_OUTLINE_PRIMARY_DEFAULT.w > rs::SELECTION_OUTLINE_SECONDARY_DEFAULT.w);
    CHECK(rs::SELECTION_OUTLINE_PRIMARY_DEFAULT.y > rs::SELECTION_OUTLINE_SECONDARY_DEFAULT.y);
}

TEST_CASE("render selection outline: packSelectionOutlineFragment writes twelve floats, EVERY byte (SO5)") {
    CHECK(rs::detail::SELECTION_OUTLINE_FRAGMENT_UNIFORM_BYTES == 48U);
    rs::SelectionOutlineParams params;
    params.primaryColorSrgb = Vec4{0.125F, 0.25F, 0.375F, 0.5F};
    params.secondaryColorSrgb = Vec4{0.625F, 0.75F, 0.875F, 1.0F};
    params.radiusPixels = 3U;
    // NOT sanitized on the way in: the packer does not sanitize, and the contract being the CALLER's
    // is what this pins (composite() is the one production caller and it does sanitize).
    const Vec2 step{0.011718F, 0.015625F};
    // The LAST DRAWN TEXEL'S CENTRE for the editor's own pair (200x140 drawn in 256x192), spelled as
    // exactly-representable literals: 199.5/256 and 139.5/192. NOT the exclusive drawExtent/
    // textureExtent -- that is the VERTEX stage's quantity and SO6 pins it there.
    const Vec2 clampUvMax{0.779296875F, 0.7265625F};
    const auto block = rs::detail::packSelectionOutlineFragment(params, step, clampUvMax);
    CHECK(block.size() == 48U);

    // A FULLY SPECIFIED expectation, so a padding byte cannot be indeterminate and a swapped pair
    // cannot hide: build the 48 bytes independently and compare the whole array.
    const std::array<float, 12> expected{0.125F, 0.25F, 0.375F, 0.5F,   0.625F,       0.75F,
                                         0.875F, 1.0F,  step.x, step.y, clampUvMax.x, clampUvMax.y};
    std::array<std::byte, 48> want{};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        std::memcpy(want.data() + (i * 4U), &expected[i], sizeof(float));
    }
    CHECK(std::memcmp(block.data(), want.data(), block.size()) == 0);

    // ...and read each field back at its named offset, so a failure names WHICH slot moved.
    const auto floatAt = [&block](std::size_t offset) {
        float v = 0.0F;
        std::memcpy(&v, block.data() + offset, sizeof(float));
        return v;
    };
    CHECK(floatAt(0) == 0.125F);
    CHECK(floatAt(12) == 0.5F);
    CHECK(floatAt(16) == 0.625F);
    CHECK(floatAt(28) == 1.0F);
    CHECK(floatAt(32) == step.x);
    CHECK(floatAt(36) == step.y);
    CHECK(floatAt(40) == clampUvMax.x);
    CHECK(floatAt(44) == clampUvMax.y);
}

TEST_CASE("render selection outline: the VERTEX block IS packTonemapVertex, reused not respelled (SO6)") {
    // fullscreen.vert.hlsl's FullscreenParams has exactly ONE packer in this tree. A second,
    // byte-identical one here would be free to disagree, and the disagreement would be a
    // vertically-flipped or scaled overlay that no tier-0 case would see.
    struct ExtentPair {
        engine::rhi::Extent2D draw;
        engine::rhi::Extent2D texture;
    };
    const std::array<ExtentPair, 5> table{{
        {{256U, 192U}, {256U, 192U}},  // quantum 1: uvMax == (1, 1)
        {{200U, 140U}, {256U, 192U}},  // quantum 64: a real sub-rect
        {{1U, 1U}, {64U, 64U}},
        {{64U, 64U}, {64U, 64U}},
        {{0U, 0U}, {0U, 0U}},  // a not-renderable target: tonemapSourceUvMax's own 1.0 arm
    }};
    for (const ExtentPair& pair : table) {
        const Vec2 uvMax = rs::tonemapSourceUvMax(pair.draw, pair.texture);
        const auto viaTonemap = rs::detail::packTonemapVertex(uvMax);
        CHECK(viaTonemap.size() == rs::detail::TONEMAP_VERTEX_UNIFORM_BYTES);
        CHECK(rs::detail::TONEMAP_VERTEX_UNIFORM_BYTES == 16U);
        // What composite() pushes at slot 0 is exactly this block, so reading the two floats back
        // asserts the shape the shader consumes rather than the identity of the call.
        float x = 0.0F;
        float y = 0.0F;
        std::memcpy(&x, viaTonemap.data() + 0, sizeof(float));
        std::memcpy(&y, viaTonemap.data() + 4, sizeof(float));
        CHECK(x == uvMax.x);
        CHECK(y == uvMax.y);
        // ...and the 8 tail bytes are ZERO, never indeterminate.
        for (std::size_t i = 8; i < viaTonemap.size(); ++i) {
            CHECK(viaTonemap[i] == std::byte{0});
        }
    }
}

TEST_CASE("render selection outline: the mask levels are ORDERED, SEPARATED and R8-exact (SO7)") {
    CHECK(0.0F < rs::SELECTION_MASK_SECONDARY);
    CHECK(rs::SELECTION_MASK_SECONDARY < rs::SELECTION_MASK_PRIMARY_THRESHOLD);
    CHECK(rs::SELECTION_MASK_PRIMARY_THRESHOLD < rs::SELECTION_MASK_PRIMARY);
    // R8Unorm stores the two levels as bytes 128 and 255 EXACTLY.
    CHECK(expectedByte(rs::SELECTION_MASK_SECONDARY) == 128L);
    CHECK(expectedByte(rs::SELECTION_MASK_PRIMARY) == 255L);
    // ...and byte 128, read back as a float, classifies SECONDARY while byte 255 classifies PRIMARY.
    // That is what gives the threshold its headroom on BOTH sides rather than only above.
    const float secondaryAsRead = 128.0F / 255.0F;
    CHECK(secondaryAsRead < rs::SELECTION_MASK_PRIMARY_THRESHOLD);
    CHECK(rs::SELECTION_MASK_PRIMARY_THRESHOLD < 1.0F);
    // DOUBLE PARENTHESES: an rhi::TextureFormat has an engine-side label function found by ADL, and
    // doctest's decomposer then tries std::string_view + const char* -- a hard compile error.
    CHECK((rs::SELECTION_MASK_FORMAT == engine::rhi::TextureFormat::R8Unorm));
}

TEST_CASE("render selection outline: a DEFAULT SelectionMaskView is the silent 'draw nothing' state (SO8)") {
    const rs::SelectionMaskView view{};
    CHECK_FALSE(view.valid);
    CHECK_FALSE(view.texture.valid());
    CHECK(view.textureExtent.width == 0U);
    CHECK(view.textureExtent.height == 0U);
    CHECK(view.drawExtent.width == 0U);
    CHECK(view.drawExtent.height == 0U);
}

TEST_CASE("render selection outline: the two HLSL stages transcribe this header (SO9)") {
    const std::string outline = strippedSourceAt(AERO_SHADERS_SRC_DIR "/selection_outline.frag.hlsl");
    const std::string mask = strippedSourceAt(AERO_SHADERS_SRC_DIR "/selection_mask.frag.hlsl");
    // ANTI-VACUITY, first: a missing file strips to an empty string and every `find` below would
    // report npos, which reads as a clean failure rather than a vacuous pass.
    REQUIRE(outline.size() > 200U);
    REQUIRE(mask.size() > 100U);
    CHECK(outline.find("definitely-not-in-this-shader") == std::string::npos);
    CHECK(mask.find("definitely-not-in-this-shader") == std::string::npos);

    // THE THRESHOLD, tied to the constant beside it: change one, change the other.
    CHECK(rs::SELECTION_MASK_PRIMARY_THRESHOLD == 0.75F);
    CHECK(outline.find("0.75") != std::string::npos);
    // THE EIGHT BOX NEIGHBOURS, each spelled out.
    CHECK(outline.find("float2(-1.0, -1.0)") != std::string::npos);
    CHECK(outline.find("float2( 0.0, -1.0)") != std::string::npos);
    CHECK(outline.find("float2( 1.0, -1.0)") != std::string::npos);
    CHECK(outline.find("float2(-1.0,  0.0)") != std::string::npos);
    CHECK(outline.find("float2( 1.0,  0.0)") != std::string::npos);
    CHECK(outline.find("float2(-1.0,  1.0)") != std::string::npos);
    CHECK(outline.find("float2( 0.0,  1.0)") != std::string::npos);
    CHECK(outline.find("float2( 1.0,  1.0)") != std::string::npos);
    // THE WHOLE RULE, in its refusing form, and the drawn-rect clamp beside it.
    CHECK(outline.find("mn >= mx") != std::string::npos);
    CHECK(outline.find("clamp(") != std::string::npos);
    // uUvClampMax, NOT uUvMax: the fragment stage's bound is the last drawn texel's CENTRE, half a
    // texel short of the vertex stage's exclusive far edge, and the distinct name is what stops the
    // two from being respelled into one.
    CHECK(outline.find("uUvClampMax") != std::string::npos);
    // The binding law (0.4.3 F8), which no runtime tier in this tree can read back.
    CHECK(outline.find("register(t0, space2)") != std::string::npos);
    CHECK(outline.find("register(s0, space2)") != std::string::npos);
    CHECK(outline.find("register(b0, space3)") != std::string::npos);

    // THE MASK STAGE declares scene.frag.hlsl's five inputs, character for character -- declaring a
    // SUBSET changes the MSL attribute indices, which are assigned by DECLARATION ORDER.
    CHECK(mask.find("TEXCOORD0") != std::string::npos);
    CHECK(mask.find("TEXCOORD1") != std::string::npos);
    CHECK(mask.find("TEXCOORD2") != std::string::npos);
    CHECK(mask.find("TEXCOORD3") != std::string::npos);
    CHECK(mask.find("TEXCOORD4") != std::string::npos);
    CHECK(mask.find("register(b0, space3)") != std::string::npos);
    // AND IT MUST NOT DISCARD: a Mask material's cut-away pixels are covered and the outline traces
    // the quad, which is D6's stated gap and 8.2.1's to close.
    CHECK(mask.find("discard") == std::string::npos);
}

TEST_CASE("render selection outline: the umbrella carries selection_outline.hpp (SO10)") {
    // DD23's pattern. This file includes render.hpp ALONE, so every symbol above already proves the
    // umbrella reaches the header -- but a future edit could add a direct include and hide a removal,
    // which is why the include line itself is pinned as text.
    // The COMMENT-STRIPPED text, so a commented-out include does not satisfy it either.
    const std::string umbrella = strippedSourceAt(RENDER_UMBRELLA_PATH);
    REQUIRE_FALSE(umbrella.empty());  // non-vacuity: the path really resolved and the file was read
    CHECK(umbrella.find("#include <aero/render/selection_outline.hpp>") != std::string::npos);
    // ...and the search can say NO, so a reader that matched everything could not fake the line above.
    CHECK(umbrella.find("#include <aero/render/does_not_exist.hpp>") == std::string::npos);
}

TEST_CASE("render selection outline: SelectionOutlineParams::operator== sees a ONE-ULP change (SO11)") {
    const rs::SelectionOutlineParams a{};
    const rs::SelectionOutlineParams b{};
    CHECK(a == b);

    rs::SelectionOutlineParams nudgedPrimary{};
    nudgedPrimary.primaryColorSrgb.y = std::nextafter(nudgedPrimary.primaryColorSrgb.y, 1.0F);
    CHECK_FALSE(nudgedPrimary == a);
    CHECK(nudgedPrimary.primaryColorSrgb.y != a.primaryColorSrgb.y);  // the nudge really moved it

    rs::SelectionOutlineParams nudgedSecondary{};
    nudgedSecondary.secondaryColorSrgb.w = std::nextafter(nudgedSecondary.secondaryColorSrgb.w, 0.0F);
    CHECK_FALSE(nudgedSecondary == a);

    rs::SelectionOutlineParams nudgedRadius{};
    nudgedRadius.radiusPixels = a.radiusPixels + 1U;
    CHECK_FALSE(nudgedRadius == a);
}

TEST_CASE("render selection outline: both new depth-store flags DEFAULT FALSE (SO12)") {
    // The "every existing caller behaves identically" claim, ASSERTED rather than argued: a default
    // flipped to true would make every RenderTarget in the tree store its depth, which is a bandwidth
    // cost with no failing test anywhere.
    CHECK_FALSE(rs::RenderTargetConfig{}.depthStore);
    CHECK_FALSE(rs::PostProcessConfig{}.sceneDepthStore);
    // ...and their two neighbours are untouched, so this case also witnesses that the field was
    // APPENDED beside `depth` rather than in place of anything.
    CHECK(rs::RenderTargetConfig{}.depth);
    CHECK(rs::PostProcessConfig{}.sceneDepth);
}

TEST_CASE("render selection outline: SelectionOutlineConfig's four defaults (SO13)") {
    const rs::SelectionOutlineConfig cfg{};
    CHECK((cfg.outputColorFormat == engine::rhi::TextureFormat::Invalid));
    CHECK((cfg.outputDepthFormat == engine::rhi::TextureFormat::Invalid));
    CHECK(cfg.vertexShaderPath == "res://fullscreen.vert");
    CHECK(cfg.fragmentShaderPath == "res://selection_outline.frag");
}

TEST_CASE("render selection outline: the tap clamp names the LAST DRAWN TEXEL, not the margin (SO14)") {
    // THE DEFECT THIS FUNCTION EXISTS FOR, asserted as the arithmetic that produces it rather than as
    // a picture. Under Nearest filtering the texel a uv names is floor(uv * extent). The drawn
    // sub-rect's EXCLUSIVE far edge, drawExtent / textureExtent, therefore names texel drawExtent --
    // the FIRST CLEARED MARGIN texel -- so a tap clamped there reads 0 against a silhouette of 1,
    // mn < mx holds, and a band is drawn along the frame edge. The bound must be the last drawn
    // texel's CENTRE, which names drawExtent - 1.
    const auto texelIndex = [](float uv, std::uint32_t extent) {
        return static_cast<int>(std::floor(uv * static_cast<float>(extent)));
    };
    // The editor's own pair: 200x140 drawn inside a 256x192 allocation (VIEWPORT_EXTENT_QUANTUM = 64).
    const Vec2 clamped = rs::detail::selectionOutlineClampUvMax({200U, 140U}, {256U, 192U});
    const Vec2 exclusive = rs::tonemapSourceUvMax({200U, 140U}, {256U, 192U});
    CHECK(texelIndex(clamped.x, 256U) == 199);  // the LAST DRAWN texel
    CHECK(texelIndex(clamped.y, 192U) == 139);
    // ...and the CONTRAST, which is the whole finding: the exclusive edge names the first margin one.
    CHECK(texelIndex(exclusive.x, 256U) == 200);
    CHECK(texelIndex(exclusive.y, 192U) == 140);
    // Both denominators are powers of two, so the two values are exact rather than approximately so.
    CHECK(clamped.x == 199.5F / 256.0F);
    CHECK(clamped.y == 139.5F / 192.0F);

    // AN EXACT TARGET (quantum = 1) IS WHERE THE DEFECT HIDES, and the bound still differs there --
    // which is why no test whose drawExtent equals its textureExtent can see the difference in a
    // picture (the hardware's own ClampToEdge returns the last drawn texel for it anyway).
    const Vec2 exact = rs::detail::selectionOutlineClampUvMax({256U, 192U}, {256U, 192U});
    CHECK(texelIndex(exact.x, 256U) == 255);
    CHECK(texelIndex(exact.y, 192U) == 191);
    CHECK(exact.x < 1.0F);
    CHECK(rs::tonemapSourceUvMax({256U, 192U}, {256U, 192U}).x == 1.0F);

    // THE THREE DEGENERATE ARMS, tonemapSourceUvMax's own answers for its own reasons.
    const Vec2 noTexture = rs::detail::selectionOutlineClampUvMax({200U, 140U}, {0U, 0U});
    CHECK(noTexture.x == 1.0F);  // not renderable: 1.0 rather than a division by zero
    CHECK(noTexture.y == 1.0F);
    const Vec2 noDraw = rs::detail::selectionOutlineClampUvMax({0U, 0U}, {256U, 192U});
    CHECK(noDraw.x == 0.0F);  // nothing drawn: every tap collapses onto texel 0
    CHECK(noDraw.y == 0.0F);
    const Vec2 overDraw = rs::detail::selectionOutlineClampUvMax({300U, 300U}, {256U, 192U});
    CHECK(overDraw.x == 255.5F / 256.0F);  // clamped to the allocation FIRST, so it never leaves it
    CHECK(overDraw.y == 191.5F / 192.0F);
    // ...and one axis at a time, so a both-axes clamp cannot hide a single-axis one.
    const Vec2 mixed = rs::detail::selectionOutlineClampUvMax({300U, 140U}, {256U, 192U});
    CHECK(mixed.x == 255.5F / 256.0F);
    CHECK(mixed.y == 139.5F / 192.0F);
}

// ================================================================================================
// TIER 1 — the GPU battery. THE FILE'S ONE #if, opened here and closed by the LAST LINE.
//
// AERO_SHADERS_DIR is defined only inside if(AERO_SHADER_TOOLS) in tests/CMakeLists.txt, so an
// ungated block here is a HARD COMPILE ERROR in the -DAERO_SHADER_TOOLS=OFF configuration. Every
// SO case above runs in EVERY configuration; only OG1-OG17 are gated. render_tonemap_test.cpp and
// render_debug_draw_test.cpp carry the identical shape for the identical reason.
//
// THE FRAME OF REFERENCE, and every OG case rests on it. A 256x192 RGBA16Float HDR pair with a
// STORED depth, resolved into a 256x192 RGBA8Unorm output, drawn through an IDENTITY CameraView
// (view = proj = identity), so world coordinates ARE NDC: a point (x, y, z) lands at column
// (x+1)/2 * 256 and row (1-y)/2 * 192, and row 0 is the TOP (fullscreen.vert.hlsl's own
// convention). Depth is [0,1] with 0 at the near plane (ADR-005).
//
// EVERY SCENE QUAD IS A CUBE PRIMITIVE SCALED INTO A SLAB, deliberately: its screen footprint is
// then exactly the rectangle its scale names, and its two z-facing quads bracket a known depth
// range -- so which of them survives back-face culling never decides an assertion.
// ================================================================================================

#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/assets/mesh_cook.hpp>
    #include <aero/core/log.hpp>
    #include <aero/core/vfs.hpp>
    #include <aero/platform/platform.hpp>
    #include <aero/rhi/rhi.hpp>

    #include "rhi_test_support.hpp"

    #include <memory>
    #include <optional>
    #include <utility>
    #include <vector>

namespace {

using engine::Mat4;
using engine::Vec3;

constexpr std::uint32_t OG_W = 256;
constexpr std::uint32_t OG_H = 192;

// The composite's two colours FORCED OPAQUE and mutually exclusive, so "is this texel a band texel"
// is an EXACT byte comparison rather than an alpha-blend arithmetic with a rounding tolerance. The
// DEFAULTS are pinned byte for byte by SO4; what this tier is about is WHICH colour lands WHERE and
// HOW WIDE the band is, and an opaque pair is what makes both exact.
constexpr engine::Vec4 OG_PRIMARY_SRGB{1.0F, 0.0F, 0.0F, 1.0F};    // pure red
constexpr engine::Vec4 OG_SECONDARY_SRGB{0.0F, 0.0F, 1.0F, 1.0F};  // pure blue

struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
    [[nodiscard]] bool operator==(const Rgba&) const = default;
};

// WITHOUT THIS every pixel assertion below would print `{?} == {?}` on the one run that matters. It
// lives HERE, in the file's anonymous namespace, and not inside a TEST_CASE, for a hard reason:
// [class.friend]/6 forbids DEFINING a friend inside a local class, so a comparison type declared in
// a case body cannot carry one at all. An operator<<, NEVER a toString -- a toString is the ADL trap
// that hard-errors inside doctest.h.
std::ostream& operator<<(std::ostream& out, const Rgba& value) {
    out << "rgba(" << static_cast<int>(value.r) << ", " << static_cast<int>(value.g) << ", "
        << static_cast<int>(value.b) << ", " << static_cast<int>(value.a) << ")";
    return out;
}

[[nodiscard]] Rgba texelAt(const std::vector<std::byte>& pixels, std::uint32_t width, std::uint32_t row,
                           std::uint32_t column) {
    const std::size_t base = ((static_cast<std::size_t>(row) * width) + column) * 4U;
    REQUIRE(base + 3U < pixels.size());
    return Rgba{static_cast<std::uint8_t>(pixels[base]), static_cast<std::uint8_t>(pixels[base + 1U]),
                static_cast<std::uint8_t>(pixels[base + 2U]), static_cast<std::uint8_t>(pixels[base + 3U])};
}

// THE PREDICATE IS "one of the two OUTLINE colours", not "non-black": the scene behind the band is
// LIT GEOMETRY, so a non-black predicate would measure the object rather than the band.
[[nodiscard]] bool isBandTexel(const Rgba& texel) {
    return texel == Rgba{255U, 0U, 0U, 255U} || texel == Rgba{0U, 0U, 255U, 255U};
}

[[nodiscard]] bool isPrimaryBandTexel(const Rgba& texel) { return texel == Rgba{255U, 0U, 0U, 255U}; }
[[nodiscard]] bool isSecondaryBandTexel(const Rgba& texel) { return texel == Rgba{0U, 0U, 255U, 255U}; }

// The longest run of OUTLINE-coloured texels through (row, centreColumn). 0 when the centre itself
// is not one, which is what makes a missing band a 0 rather than a silent 1.
[[nodiscard]] int bandRunAt(const std::vector<std::byte>& pixels, std::uint32_t width, std::uint32_t row,
                            std::uint32_t centreColumn) {
    if (!isBandTexel(texelAt(pixels, width, row, centreColumn))) {
        return 0;
    }
    std::uint32_t left = centreColumn;
    while (left > 0U && isBandTexel(texelAt(pixels, width, row, left - 1U))) {
        --left;
    }
    std::uint32_t right = centreColumn;
    while (right + 1U < width && isBandTexel(texelAt(pixels, width, row, right + 1U))) {
        ++right;
    }
    return static_cast<int>((right - left) + 1U);
}

[[nodiscard]] int countBandTexels(const std::vector<std::byte>& pixels, std::uint32_t width, std::uint32_t row,
                                  std::uint32_t firstColumn, std::uint32_t lastColumn) {
    int count = 0;
    for (std::uint32_t c = firstColumn; c <= lastColumn; ++c) {
        if (isBandTexel(texelAt(pixels, width, row, c))) {
            ++count;
        }
    }
    return count;
}

// Where an NDC coordinate lands, computed the way the rasteriser does rather than guessed.
[[nodiscard]] std::uint32_t columnForNdcX(float x, std::uint32_t width) {
    return static_cast<std::uint32_t>(((x * 0.5F) + 0.5F) * static_cast<float>(width));
}
[[nodiscard]] std::uint32_t rowForNdcY(float y, std::uint32_t height) {
    return static_cast<std::uint32_t>((0.5F - (y * 0.5F)) * static_cast<float>(height));
}

// The IDENTITY camera: world == NDC. eyePosition is BEHIND the near plane rather than at the origin
// -- ForwardRenderer's fragment stage computes V = normalize(eye - worldPos), and an eye ON a drawn
// surface makes that a 0/0.
[[nodiscard]] engine::render::CameraView identityCamera() {
    return engine::render::CameraView{
        .view = Mat4::identity(), .proj = Mat4::identity(), .eyePosition = Vec3{0.0F, 0.0F, -1.0F}};
}

// A screen-aligned slab: a Cube primitive scaled so its footprint is exactly
// [cx - halfW, cx + halfW] x [cy - halfH, cy + halfH] in NDC, spanning [zNear, zFar] in depth.
// mvp == model because the view-projection is the identity (the cullingEnabled contract).
[[nodiscard]] engine::render::MeshInstance slab(float cx, float cy, float halfW, float halfH, float zNear, float zFar,
                                                Vec3 color) {
    const Mat4 model = engine::translation(Vec3{cx, cy, (zNear + zFar) * 0.5F}) *
                       engine::scaling(Vec3{halfW * 2.0F, halfH * 2.0F, zFar - zNear});
    engine::render::MeshInstance instance;
    instance.primitive = engine::render::PrimitiveId::Cube;
    instance.model = model;
    instance.mvp = model;
    instance.normalMatrix = Mat4::identity();
    instance.color = color;
    return instance;
}

// Everything that could vary is pinned OFF so a drawn pixel's colour is a constant: shadows off,
// culling off, directional intensity 0 (with a UNIT direction, so nothing normalizes a zero
// vector), ambient = {1,1,1}, the default material. The fragment then reduces to
// ambient * baseColor * instanceColor -- DG6's own construction.
[[nodiscard]] engine::render::RenderView flatView(std::span<const engine::render::MeshInstance> instances) {
    engine::render::RenderView view;
    view.camera = identityCamera();
    view.instances = instances;
    view.ambient = Vec3::one();
    view.directional = {.direction = Vec3{0.0F, -1.0F, 0.0F}, .color = Vec3::one(), .intensity = 0.0F};
    view.cullingEnabled = false;
    view.shadowsEnabled = false;
    return view;
}

// THE WHOLE FRAME, in one call, so no case can forget a step or an endFrame:
//   beginScene -> draw -> endScene (submits A) -> renderSelectionMask (its OWN buffer) ->
//   beginFrame -> resolve -> composite -> endFrame (submits B) -> readback.
// readbackTexture is called AFTER endFrame and NEVER maps before its fence wait -- that wait is what
// PERFORMS the copy on D3D12, so mapping first reads garbage on Windows alone.
[[nodiscard]] std::vector<std::byte> renderOnce(
    engine::rhi::Device& device, engine::render::PostProcess& post, engine::render::RenderTarget& target,
    engine::render::ForwardRenderer& forward, engine::render::SelectionOutline& outline,
    const engine::render::RenderView& view, std::span<const engine::render::MeshInstance> secondary,
    std::span<const engine::render::MeshInstance> primary, const engine::render::SelectionOutlineParams& params,
    bool withOutline = true) {
    std::optional<engine::render::Frame> sceneFrame = post.beginScene({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(sceneFrame.has_value());
    forward.draw(*sceneFrame, view);
    REQUIRE(post.endScene(std::move(*sceneFrame)));

    engine::render::SelectionMaskView mask{};
    if (withOutline) {
        mask = forward.renderSelectionMask(post.sceneDepthTexture(), post.sceneTextureExtent(), post.sceneDrawExtent(),
                                           secondary, primary);
    }
    std::optional<engine::render::Frame> outFrame = target.beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(outFrame.has_value());
    post.resolve(*outFrame, {});
    if (withOutline) {
        outline.composite(*outFrame, mask, params);
    }
    REQUIRE(target.endFrame(std::move(*outFrame)));

    const engine::rhi::Extent2D allocation = target.textureExtent();
    std::vector<std::byte> pixels(static_cast<std::size_t>(allocation.width) * allocation.height * 4U, std::byte{0xAB});
    REQUIRE(device.readbackTexture(target.colorTexture(), 0, pixels));
    return pixels;
}

// The RAII log-capture guard, copied verbatim from render_debug_draw_test.cpp's own copy of PP4's --
// its destructor detaches, which a code-review round required there after the bare form was found to
// be a latent use-after-free.
struct LogCallbackGuard {
    ~LogCallbackGuard() { engine::setLogCallback({}); }
    LogCallbackGuard() = default;
    LogCallbackGuard(const LogCallbackGuard&) = delete;
    LogCallbackGuard& operator=(const LogCallbackGuard&) = delete;
    LogCallbackGuard(LogCallbackGuard&&) = delete;
    LogCallbackGuard& operator=(LogCallbackGuard&&) = delete;
};

// render_debug_draw_test.cpp's SingleShaderPrefixBackend under a distinct name. OG1 needs the arm
// where SOME shaders load and others do not: with nothing mounted BOTH handles are invalid and
// destroying an invalid handle is a documented no-op, so an empty mount CANNOT see a create() that
// forgot to release the shader it DID make.
class PrefixOnlyBackend final : public engine::FileSystemBackend {
public:
    PrefixOnlyBackend(std::string_view rootDirectory, std::string_view allowedPrefix)
        : inner(rootDirectory), prefix(allowedPrefix) {}

    [[nodiscard]] bool exists(std::string_view relPath) const override {
        return admits(relPath) && inner.exists(relPath);
    }
    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view relPath) const override {
        return admits(relPath) ? inner.fileSize(relPath) : std::nullopt;
    }
    [[nodiscard]] std::optional<engine::ByteBuffer> readFile(std::string_view relPath) const override {
        return admits(relPath) ? inner.readFile(relPath) : std::nullopt;
    }

private:
    [[nodiscard]] bool admits(std::string_view relPath) const { return relPath.starts_with(prefix); }

    engine::DirectoryBackend inner;
    std::string prefix;
};

}  // namespace

// The tier-1 preamble, written out per case exactly as render_debug_draw_test.cpp does it:
// AERO_SKIP_OR_FAIL returns from the enclosing function, so it cannot live in a helper.
// NOTE `.sceneDepthStore = true` -- THE WHOLE TASK. Without it the mask attaches a depth attachment
// whose contents were DontCare'd, which is empty on Metal and correct on D3D12/Vulkan.
    #define AERO_OG_PREAMBLE()                                                                                        \
        const engine::platform::Context ctx{{.headless = false}};                                                     \
        if (!ctx.valid()) {                                                                                           \
            AERO_SKIP_OR_FAIL("no real video driver available");                                                      \
        }                                                                                                             \
        auto device = engine::rhi::Device::create();                                                                  \
        if (!device.has_value()) {                                                                                    \
            AERO_SKIP_OR_FAIL("no GPU device available");                                                             \
        }                                                                                                             \
        engine::VirtualFileSystem vfs;                                                                                \
        vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));                                      \
        auto post = engine::render::PostProcess::create(*device, vfs, {OG_W, OG_H},                                   \
                                                        {.outputColorFormat = engine::rhi::TextureFormat::RGBA8Unorm, \
                                                         .outputDepthFormat = engine::rhi::TextureFormat::Invalid,    \
                                                         .sceneDepthStore = true,                                     \
                                                         .quantum = 1});                                              \
        REQUIRE(post.has_value());                                                                                    \
        auto target = engine::render::RenderTarget::create(                                                           \
            *device, {OG_W, OG_H},                                                                                    \
            {.colorFormat = engine::rhi::TextureFormat::RGBA8Unorm, .depth = false, .quantum = 1});                   \
        REQUIRE(target.has_value());                                                                                  \
        auto forward = engine::render::ForwardRenderer::create(*device, vfs,                                          \
                                                               {.colorFormat = post->sceneColorFormat(),              \
                                                                .depthFormat = post->sceneDepthFormat(),              \
                                                                .shadowMapResolution = 0});                           \
        REQUIRE(forward.has_value());                                                                                 \
        auto outline =                                                                                                \
            engine::render::SelectionOutline::create(*device, vfs, {.outputColorFormat = target->colorFormat()});     \
        REQUIRE(outline.has_value())

TEST_CASE("render selection outline: create succeeds, refuses four ways and leaks NOTHING (OG1)") {
    AERO_OG_PREAMBLE();
    // The editor's real pair: an RGBA8Unorm OUTPUT target and a DEPTH-FREE frame.
    CHECK((target->depthFormat() == engine::rhi::TextureFormat::Invalid));
    CHECK(outline->compositeCount() == 0U);
    CHECK_FALSE(outline->hasWarnedInvalidMask());
    CHECK_FALSE(outline->hasWarnedNotRenderable());

    SUBCASE("refusal 1: an Invalid outputColorFormat") {
        CHECK_FALSE(engine::render::SelectionOutline::create(*device, vfs,
                                                             {.outputColorFormat = engine::rhi::TextureFormat::Invalid})
                        .has_value());
    }
    SUBCASE("refusal 2: a DEPTH format as the output colour") {
        const engine::rhi::TextureFormat depth = post->sceneDepthFormat();
        REQUIRE((depth != engine::rhi::TextureFormat::Invalid));
        CHECK_FALSE(engine::render::SelectionOutline::create(*device, vfs, {.outputColorFormat = depth}).has_value());
    }
    SUBCASE("refusals 3 and 4: a missing vertex or fragment shader, and NEITHER leaks") {
        // The PARTIAL mount is what makes this arm real: with NOTHING mounted both handles are
        // invalid and destroying an invalid handle is a documented no-op, so an empty mount could not
        // see a create() that forgot to release the shader that DID load. ScopedShader is what makes
        // that unspellable; this is its witness.
        std::vector<std::string> warnings;
        const LogCallbackGuard detachOnExit;
        engine::setLogCallback([&warnings](const engine::LogRecord& record) {
            if (record.level >= engine::LogLevel::Warn) {
                warnings.emplace_back(record.message);
            }
        });
        {
            engine::VirtualFileSystem vertexOnly;
            vertexOnly.mount(std::make_unique<PrefixOnlyBackend>(AERO_SHADERS_DIR, "fullscreen"));
            CHECK(vertexOnly.exists("res://fullscreen.vert.json"));
            CHECK_FALSE(vertexOnly.exists("res://selection_outline.frag.json"));
            CHECK_FALSE(engine::render::SelectionOutline::create(*device, vertexOnly,
                                                                 {.outputColorFormat = target->colorFormat()})
                            .has_value());

            engine::VirtualFileSystem fragmentOnly;
            fragmentOnly.mount(std::make_unique<PrefixOnlyBackend>(AERO_SHADERS_DIR, "selection_outline"));
            CHECK(fragmentOnly.exists("res://selection_outline.frag.json"));
            CHECK_FALSE(fragmentOnly.exists("res://fullscreen.vert.json"));
            CHECK_FALSE(engine::render::SelectionOutline::create(*device, fragmentOnly,
                                                                 {.outputColorFormat = target->colorFormat()})
                            .has_value());
        }
        outline.reset();  // every object holding a Device* must die FIRST
        forward.reset();
        target.reset();
        post.reset();
        device.reset();  // ...then ~Device is what WOULD report a leak
        engine::setLogCallback({});
        // The exact wording comes from sdl_gpu_backend.cpp's ~Impl: "rhi: ~Device releasing N leaked
        // <kind>(s)". A leaked shader is the one this arm exists for; the other three are asserted
        // too because create() builds them in the same function.
        for (const std::string& message : warnings) {
            CHECK(message.find("leaked shader") == std::string::npos);
            CHECK(message.find("leaked graphics pipeline") == std::string::npos);
            CHECK(message.find("leaked sampler") == std::string::npos);
            CHECK(message.find("leaked texture") == std::string::npos);
        }
        return;  // `device` is reset; nothing below may run
    }
}

TEST_CASE("render selection outline: an empty selection costs NOTHING and changes NO pixel (OG2)") {
    AERO_OG_PREAMBLE();
    const engine::render::MeshInstance quad = slab(0.0F, 0.0F, 0.25F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});
    const std::array<engine::render::MeshInstance, 1> scene{quad};
    const engine::render::RenderView view = flatView(scene);
    const engine::render::SelectionOutlineParams params{
        .primaryColorSrgb = OG_PRIMARY_SRGB, .secondaryColorSrgb = OG_SECONDARY_SRGB, .radiusPixels = 2U};

    // (a) THE ZERO-COST PATH: nothing selected, so nothing is acquired and nothing is recorded.
    const std::vector<std::byte> selectedNothing =
        renderOnce(*device, *post, *target, *forward, *outline, view, {}, {}, params);
    CHECK(forward->selectionMaskPassCount() == 0U);
    CHECK(outline->compositeCount() == 0U);
    CHECK_FALSE(forward->hasWarnedSelectionMaskUnavailable());  // an empty selection is not a diagnostic
    CHECK_FALSE(outline->hasWarnedInvalidMask());

    // (b) BIT-IDENTICAL to the same frame with the two calls OMITTED ENTIRELY.
    const std::vector<std::byte> noOutlineAtAll =
        renderOnce(*device, *post, *target, *forward, *outline, view, {}, {}, params, /*withOutline=*/false);
    REQUIRE(selectedNothing.size() == noOutlineAtAll.size());
    std::size_t firstDiff = selectedNothing.size();
    for (std::size_t i = 0; i < selectedNothing.size(); ++i) {
        if (selectedNothing[i] != noOutlineAtAll[i]) {
            firstDiff = i;
            break;
        }
    }
    INFO("first differing byte index: ", firstDiff, " of ", selectedNothing.size());
    CHECK(firstDiff == selectedNothing.size());

    // (c) THE ANTI-VACUITY CONTROL. An image comparison that cannot fail is worse than no comparison:
    // select the quad and confirm the SAME comparison reports a LARGE difference.
    const std::array<engine::render::MeshInstance, 1> selection{quad};
    const std::vector<std::byte> selectedSomething =
        renderOnce(*device, *post, *target, *forward, *outline, view, {}, selection, params);
    CHECK(forward->selectionMaskPassCount() == 1U);
    CHECK(outline->compositeCount() == 1U);
    int differing = 0;
    for (std::size_t i = 0; i < selectedSomething.size(); ++i) {
        if (selectedSomething[i] != noOutlineAtAll[i]) {
            ++differing;
        }
    }
    INFO("control: differing bytes = ", differing);
    CHECK(differing > 1000);
}

TEST_CASE("render selection outline: the band across an axis-aligned edge is an EXACT integer (OG3)") {
    AERO_OG_PREAMBLE();
    // A quad whose RIGHT edge lands on the boundary BETWEEN columns 159 and 160 -- so the two
    // neighbouring pixel CENTRES are 0.004 NDC either side of it and the coverage of neither is in
    // doubt. Column 159 is therefore the last lit mask column, and it is a band pixel for every
    // radius, which is what makes it a stable place to measure from.
    const engine::render::MeshInstance quad = slab(0.0F, 0.0F, 0.25F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});
    const std::array<engine::render::MeshInstance, 1> scene{quad};
    const engine::render::RenderView view = flatView(scene);
    constexpr std::uint32_t ROW = 96U;  // the vertical middle, well inside |y| <= 0.4

    REQUIRE(columnForNdcX(0.25F, OG_W) == 160U);
    for (const std::uint32_t radius : {1U, 2U, 4U, 8U}) {
        INFO("radius = ", radius);
        const engine::render::SelectionOutlineParams params{
            .primaryColorSrgb = OG_PRIMARY_SRGB, .secondaryColorSrgb = OG_SECONDARY_SRGB, .radiusPixels = radius};
        const std::vector<std::byte> pixels =
            renderOnce(*device, *post, *target, *forward, *outline, view, {}, scene, params);
        // AN EXACT INTEGER, never a tolerance: the sampler is Nearest, so each tap IS a texel value
        // and the neighbourhood in x is exactly {c - r, c, c + r}.
        CHECK(bandRunAt(pixels, OG_W, ROW, 159U) == static_cast<int>(2U * radius));
    }
}

TEST_CASE("render selection outline: primary and secondary read as their own colours (OG4)") {
    AERO_OG_PREAMBLE();
    // Two quads side by side, SEPARATED by a gap wider than 2 * radius so their bands cannot touch:
    // the left one secondary, the right one primary.
    const engine::render::MeshInstance left = slab(-0.5F, 0.0F, 0.15F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});
    const engine::render::MeshInstance right = slab(0.5F, 0.0F, 0.15F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});
    const std::array<engine::render::MeshInstance, 2> scene{left, right};
    const std::array<engine::render::MeshInstance, 1> secondary{left};
    const std::array<engine::render::MeshInstance, 1> primary{right};
    const engine::render::RenderView view = flatView(scene);
    const engine::render::SelectionOutlineParams params{
        .primaryColorSrgb = OG_PRIMARY_SRGB, .secondaryColorSrgb = OG_SECONDARY_SRGB, .radiusPixels = 1U};
    const std::vector<std::byte> pixels =
        renderOnce(*device, *post, *target, *forward, *outline, view, secondary, primary, params);

    constexpr std::uint32_t ROW = 96U;
    // The last lit column of each quad, which is a band pixel at every radius.
    const std::uint32_t leftEdge = columnForNdcX(-0.35F, OG_W) - 1U;
    const std::uint32_t rightEdge = columnForNdcX(0.65F, OG_W) - 1U;
    CHECK(isSecondaryBandTexel(texelAt(pixels, OG_W, ROW, leftEdge)));
    CHECK(isPrimaryBandTexel(texelAt(pixels, OG_W, ROW, rightEdge)));
    // ...and each is EXACTLY its colour, so a swapped pair cannot pass on "some band is there".
    CHECK(texelAt(pixels, OG_W, ROW, leftEdge) == Rgba{0U, 0U, 255U, 255U});
    CHECK(texelAt(pixels, OG_W, ROW, rightEdge) == Rgba{255U, 0U, 0U, 255U});

    SUBCASE("the OVERLAP: where a primary interior meets a secondary interior, PRIMARY wins") {
        // ADJACENT quads sharing the boundary between columns 127 and 128: the mask reads 0.5 on the
        // left and 1.0 on the right, so mn = 0.5, mx = 1.0 -- an outline pixel coloured from mx.
        const engine::render::MeshInstance a = slab(-0.125F, 0.0F, 0.125F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});
        const engine::render::MeshInstance b = slab(0.125F, 0.0F, 0.125F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});
        const std::array<engine::render::MeshInstance, 2> pair{a, b};
        const std::array<engine::render::MeshInstance, 1> sec{a};
        const std::array<engine::render::MeshInstance, 1> pri{b};
        const engine::render::RenderView pairView = flatView(pair);
        const std::vector<std::byte> shared =
            renderOnce(*device, *post, *target, *forward, *outline, pairView, sec, pri, params);
        REQUIRE(columnForNdcX(0.0F, OG_W) == 128U);
        // BOTH sides of the shared edge take the primary colour, because both neighbourhoods reach a
        // 1.0 texel and mx decides the colour.
        CHECK(texelAt(shared, OG_W, ROW, 127U) == Rgba{255U, 0U, 0U, 255U});
        CHECK(texelAt(shared, OG_W, ROW, 128U) == Rgba{255U, 0U, 0U, 255U});
        // ...and the pair's OUTER edges still read their own colours, so this is not "everything is
        // primary now".
        CHECK(texelAt(shared, OG_W, ROW, columnForNdcX(-0.25F, OG_W)) == Rgba{0U, 0U, 255U, 255U});
        CHECK(texelAt(shared, OG_W, ROW, columnForNdcX(0.25F, OG_W) - 1U) == Rgba{255U, 0U, 0U, 255U});
    }
}

TEST_CASE("render selection outline: the mask is OCCLUDED by geometry in front of it (OG5)") {
    AERO_OG_PREAMBLE();
    // The selected quad spans columns 96..159 at depth [0.6, 0.7]. An UNSELECTED occluder covers its
    // RIGHT half (columns 128..191) at depth [0.1, 0.2] -- nearer on BOTH of its z-facing quads, so
    // which one survives back-face culling never decides the outcome.
    const engine::render::MeshInstance selected = slab(0.0F, 0.0F, 0.25F, 0.4F, 0.6F, 0.7F, Vec3{0.0F, 1.0F, 0.0F});
    const engine::render::MeshInstance occluder = slab(0.25F, 0.0F, 0.25F, 0.45F, 0.1F, 0.2F, Vec3{0.0F, 0.0F, 0.0F});
    const std::array<engine::render::MeshInstance, 2> scene{selected, occluder};
    const std::array<engine::render::MeshInstance, 1> primary{selected};
    const engine::render::RenderView view = flatView(scene);
    const engine::render::SelectionOutlineParams params{
        .primaryColorSrgb = OG_PRIMARY_SRGB, .secondaryColorSrgb = OG_SECONDARY_SRGB, .radiusPixels = 2U};
    const std::vector<std::byte> pixels =
        renderOnce(*device, *post, *target, *forward, *outline, view, {}, primary, params);

    constexpr std::uint32_t ROW = 96U;
    // THE VISIBLE HALF HAS A BAND: the selected quad's own left edge, at the boundary between columns
    // 95 and 96.
    CHECK(bandRunAt(pixels, OG_W, ROW, 96U) > 0);
    // THE COVERED HALF HAS NONE. Columns 140..191 are deep inside the occluder and well clear of the
    // band the occlusion boundary itself produces around column 128.
    CHECK(countBandTexels(pixels, OG_W, ROW, 140U, 191U) == 0);
    // ...including the selected quad's own RIGHT edge at column 159/160, which is under the occluder:
    // this is the pixel that lights up the moment the depth test stops rejecting hidden fragments.
    CHECK_FALSE(isBandTexel(texelAt(pixels, OG_W, ROW, 159U)));
    CHECK_FALSE(isBandTexel(texelAt(pixels, OG_W, ROW, 160U)));

    SUBCASE("ENTIRELY behind an occluder: no outline pixels at all") {
        const engine::render::MeshInstance wide = slab(0.0F, 0.0F, 0.45F, 0.45F, 0.1F, 0.2F, Vec3{0.0F, 0.0F, 0.0F});
        const std::array<engine::render::MeshInstance, 2> hiddenScene{selected, wide};
        const engine::render::RenderView hiddenView = flatView(hiddenScene);
        const std::vector<std::byte> hidden =
            renderOnce(*device, *post, *target, *forward, *outline, hiddenView, {}, primary, params);
        CHECK(countBandTexels(hidden, OG_W, ROW, 0U, OG_W - 1U) == 0);
    }
}

TEST_CASE("render selection outline: a SPHERE outlines its silhouette, not its box (OG6)") {
    AERO_OG_PREAMBLE();
    // THE DELIVERABLE'S OWN PROOF, and the reason it is a Sphere: its local AABB is the unit cube,
    // so the box corners are 0.41 of a radius clear of the limb -- the exact place the old
    // twelve-edge highlight drew a line and the silhouette must not.
    //
    // The z scale is 0.4 rather than 1 so the ellipsoid stays inside [0, 1] depth; the SCREEN
    // silhouette is unaffected, because x and y are untouched.
    const Mat4 model = engine::translation(Vec3{0.0F, 0.0F, 0.5F}) * engine::scaling(Vec3{1.0F, 1.0F, 0.4F});
    engine::render::MeshInstance sphere;
    sphere.primitive = engine::render::PrimitiveId::Sphere;
    sphere.model = model;
    sphere.mvp = model;
    sphere.normalMatrix = Mat4::identity();
    sphere.color = Vec3{0.0F, 1.0F, 0.0F};
    const std::array<engine::render::MeshInstance, 1> scene{sphere};
    const engine::render::RenderView view = flatView(scene);
    const engine::render::SelectionOutlineParams params{
        .primaryColorSrgb = OG_PRIMARY_SRGB, .secondaryColorSrgb = OG_SECONDARY_SRGB, .radiusPixels = 2U};
    const std::vector<std::byte> pixels =
        renderOnce(*device, *post, *target, *forward, *outline, view, {}, scene, params);

    // THE EXPECTATION IS COMPUTED FROM THE CATALOG'S BOX THROUGH THE CORNER ENUMERATION, a quantity
    // NO PART of the mask path touches: the mask rasterises the sphere's TRIANGLES and never sees a
    // box at all. engine/render/src/primitives.cpp makes the sphere with RADIUS 0.5, which
    // scene_bounds.hpp's own comment restates; the bit rule is that one -- bit 0 selects X, bit 1 Y,
    // bit 2 Z, min when 0 and max when 1.
    constexpr Vec3 BOX_MIN{-0.5F, -0.5F, -0.5F};
    constexpr Vec3 BOX_MAX{0.5F, 0.5F, 0.5F};
    const auto corner = [&](std::size_t i) {
        return Vec3{((i & 1U) != 0U) ? BOX_MAX.x : BOX_MIN.x, ((i & 2U) != 0U) ? BOX_MAX.y : BOX_MIN.y,
                    ((i & 4U) != 0U) ? BOX_MAX.z : BOX_MIN.z};
    };
    int cornersChecked = 0;
    for (std::size_t i = 0; i < 8U; ++i) {
        const engine::Vec4 clip = model * engine::Vec4{corner(i).x, corner(i).y, corner(i).z, 1.0F};
        REQUIRE(clip.w != 0.0F);
        const std::uint32_t column = columnForNdcX(clip.x / clip.w, OG_W);
        const std::uint32_t row = rowForNdcY(clip.y / clip.w, OG_H);
        REQUIRE(column < OG_W);
        REQUIRE(row < OG_H);
        INFO("box corner ", i, " at row ", row, " column ", column);
        // A 5x5 neighbourhood, because a single-pixel probe on generated geometry is a coin flip
        // (E.1.2's DG18 lesson) and the corner is 26 pixels clear of the limb either way.
        for (std::uint32_t dr = 0; dr < 5U; ++dr) {
            for (std::uint32_t dc = 0; dc < 5U; ++dc) {
                const std::uint32_t r = row + dr - 2U;
                const std::uint32_t c = column + dc - 2U;
                if (r < OG_H && c < OG_W) {
                    CHECK_FALSE(isBandTexel(texelAt(pixels, OG_W, r, c)));
                }
            }
        }
        ++cornersChecked;
    }
    CHECK(cornersChecked == 8);  // anti-vacuity: the loop really ran over all eight

    // ...AND THE LIMB DOES CARRY A BAND. Row 96 is the vertical middle, so the sphere's right limb is
    // at x ~ +0.5 -- column 192, exactly where the box's right face also is, which is why the
    // interesting corners are the four DIAGONAL ones above.
    CHECK(countBandTexels(pixels, OG_W, 96U, 185U, 199U) > 0);
    CHECK(countBandTexels(pixels, OG_W, 96U, 57U, 71U) > 0);  // the left limb
}

TEST_CASE("render selection outline: a MARGINED target puts the band where an exact one does (OG7)") {
    AERO_OG_PREAMBLE();
    // D10'S TRAP, AND THE ONLY CASE THAT CAN SEE IT. renderShadowMap sets neither viewport nor
    // scissor, correctly, because its texture has no margin; the mask texture HAS one. With the
    // viewport left at beginRenderPass's default the mask maps across the ALLOCATION while the
    // resolve maps across the DRAWN rect, and the mask is silently RESCALED against the colour image
    // -- invisible in every test whose drawExtent equals its textureExtent, which is every target
    // created with quantum = 1.
    constexpr std::uint32_t DRAW_W = 200;
    constexpr std::uint32_t DRAW_H = 140;
    auto marginedPost =
        engine::render::PostProcess::create(*device, vfs, {DRAW_W, DRAW_H},
                                            {.outputColorFormat = engine::rhi::TextureFormat::RGBA8Unorm,
                                             .outputDepthFormat = engine::rhi::TextureFormat::Invalid,
                                             .sceneDepthStore = true,
                                             .quantum = 64});
    REQUIRE(marginedPost.has_value());
    auto marginedTarget = engine::render::RenderTarget::create(
        *device, {DRAW_W, DRAW_H},
        {.colorFormat = engine::rhi::TextureFormat::RGBA8Unorm, .depth = false, .quantum = 64});
    REQUIRE(marginedTarget.has_value());
    // THE MARGIN IS REAL, asserted rather than assumed: 200x140 rounds up to 256x192 on BOTH axes.
    CHECK(marginedPost->sceneDrawExtent().width == DRAW_W);
    CHECK(marginedPost->sceneDrawExtent().height == DRAW_H);
    CHECK(marginedPost->sceneTextureExtent().width == 256U);
    CHECK(marginedPost->sceneTextureExtent().height == 192U);
    CHECK(marginedTarget->drawExtent().width == DRAW_W);
    CHECK(marginedTarget->drawExtent().height == DRAW_H);
    CHECK(marginedTarget->textureExtent().width == 256U);
    CHECK(marginedTarget->textureExtent().height == 192U);

    // ...and an EXACT pair at the same DRAWN size, which is the comparison. Both share `forward` and
    // `outline`: the formats are identical, so a second of each would prove nothing and cost two
    // more pipeline sets.
    auto exactPost = engine::render::PostProcess::create(*device, vfs, {DRAW_W, DRAW_H},
                                                         {.outputColorFormat = engine::rhi::TextureFormat::RGBA8Unorm,
                                                          .outputDepthFormat = engine::rhi::TextureFormat::Invalid,
                                                          .sceneDepthStore = true,
                                                          .quantum = 1});
    REQUIRE(exactPost.has_value());
    auto exactTarget = engine::render::RenderTarget::create(
        *device, {DRAW_W, DRAW_H},
        {.colorFormat = engine::rhi::TextureFormat::RGBA8Unorm, .depth = false, .quantum = 1});
    REQUIRE(exactTarget.has_value());
    CHECK(exactTarget->textureExtent().width == DRAW_W);
    CHECK(exactTarget->textureExtent().height == DRAW_H);

    const engine::render::MeshInstance quad = slab(0.0F, 0.0F, 0.25F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});
    const std::array<engine::render::MeshInstance, 1> scene{quad};
    const engine::render::RenderView view = flatView(scene);
    const engine::render::SelectionOutlineParams params{
        .primaryColorSrgb = OG_PRIMARY_SRGB, .secondaryColorSrgb = OG_SECONDARY_SRGB, .radiusPixels = 2U};

    const std::vector<std::byte> margined =
        renderOnce(*device, *marginedPost, *marginedTarget, *forward, *outline, view, {}, scene, params);
    const std::vector<std::byte> exact =
        renderOnce(*device, *exactPost, *exactTarget, *forward, *outline, view, {}, scene, params);

    // The row through the middle of the DRAWN rect, and the first band column found from the left in
    // each. +/- 1 is the width of the genuine fill-rule ambiguity at a quad edge, not slack.
    const std::uint32_t row = DRAW_H / 2U;
    const auto firstBandColumn = [&](const std::vector<std::byte>& pixels, std::uint32_t stride) {
        for (std::uint32_t c = 0; c < DRAW_W; ++c) {
            if (isBandTexel(texelAt(pixels, stride, row, c))) {
                return static_cast<int>(c);
            }
        }
        return -1;
    };
    const int marginedFirst = firstBandColumn(margined, 256U);
    const int exactFirst = firstBandColumn(exact, DRAW_W);
    INFO("margined first band column = ", marginedFirst, ", exact = ", exactFirst);
    CHECK(marginedFirst >= 0);  // anti-vacuity: there IS a band in the margined render
    CHECK(exactFirst >= 0);
    CHECK(marginedFirst >= exactFirst - 1);
    CHECK(marginedFirst <= exactFirst + 1);
}

TEST_CASE("render selection outline: an object leaving the frame draws NO border there (OG8)") {
    AERO_OG_PREAMBLE();
    // D9, and it is a BEHAVIOURAL property rather than a defensive clamp: without the tap clamp a
    // selected object continuing past the right edge draws a bright frame around the picture, which
    // looks like a bug and is not one.
    const engine::render::MeshInstance overhang =
        slab(0.9F, 0.0F, 1.0F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});  // x from -0.1 to +1.9
    const std::array<engine::render::MeshInstance, 1> scene{overhang};
    const engine::render::RenderView view = flatView(scene);
    const engine::render::SelectionOutlineParams params{
        .primaryColorSrgb = OG_PRIMARY_SRGB, .secondaryColorSrgb = OG_SECONDARY_SRGB, .radiusPixels = 2U};
    const std::vector<std::byte> pixels =
        renderOnce(*device, *post, *target, *forward, *outline, view, {}, scene, params);

    constexpr std::uint32_t ROW = 96U;
    // NO band column at the frame's own right edge...
    CHECK_FALSE(isBandTexel(texelAt(pixels, OG_W, ROW, OG_W - 1U)));
    CHECK(countBandTexels(pixels, OG_W, ROW, OG_W - 4U, OG_W - 1U) == 0);
    // ...and the ANTI-VACUITY arm: the quad's LEFT edge, which is inside the frame, does have one.
    CHECK(countBandTexels(pixels, OG_W, ROW, columnForNdcX(-0.1F, OG_W) - 4U, columnForNdcX(-0.1F, OG_W) + 4U) > 0);
}

TEST_CASE("render selection outline: OUTPUT ALPHA IS 255 EVERYWHERE, band or no band (OG9)") {
    AERO_OG_PREAMBLE();
    // The blend writes Zero/One on the ALPHA channel, deliberately: SrcAlpha/OneMinusSrcAlpha there
    // would write 0 wherever this shader outputs a transparent fragment -- MOST of the image -- and
    // the editor's ImGui::Image, which alpha-blends this texture over the panel background, would
    // show the viewport SEE-THROUGH.
    const engine::render::MeshInstance quad = slab(0.0F, 0.0F, 0.25F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});
    const std::array<engine::render::MeshInstance, 1> scene{quad};
    const engine::render::RenderView view = flatView(scene);
    // BOTH default colours, so the SECONDARY's own 0.745 alpha is exercised rather than the opaque
    // pair the width cases use.
    const engine::render::SelectionOutlineParams params{.radiusPixels = 3U};
    const std::vector<std::byte> pixels =
        renderOnce(*device, *post, *target, *forward, *outline, view, scene, {}, params);

    // THE WHOLE BUFFER, not a sample.
    int opaque = 0;
    int transparent = 0;
    for (std::uint32_t row = 0; row < OG_H; ++row) {
        for (std::uint32_t column = 0; column < OG_W; ++column) {
            if (texelAt(pixels, OG_W, row, column).a == 255U) {
                ++opaque;
            } else {
                ++transparent;
            }
        }
    }
    CHECK(transparent == 0);
    CHECK(opaque == static_cast<int>(OG_W * OG_H));
    // ...and the band really was drawn, so this is not "alpha is 255 because nothing composited".
    CHECK(outline->compositeCount() == 1U);
}

TEST_CASE("render selection outline: the mask MIRRORS the forward pass's per-instance cull (OG10)") {
    AERO_OG_PREAMBLE();
    // D5, and INV-1's whole point: the mask is the set of pixels the forward pass SHADED for those
    // instances. A mask drawn CullMode::None would cover a single-sided quad the forward pass culled
    // and outline an object that is NOT ON SCREEN.
    //
    // The two orientations are a Plane rotated to face the camera and its MIRROR -- a negative X
    // scale, which reverses the winding without moving a single vertex on screen. Exactly one of them
    // survives back-face culling, and the case asserts the PAIRING rather than which.
    const engine::Quat faceCamera = engine::fromAxisAngle(Vec3{1.0F, 0.0F, 0.0F}, engine::HALF_PI);
    const Mat4 place =
        engine::translation(Vec3{0.0F, 0.0F, 0.5F}) *
        engine::compose({.translation = Vec3{}, .rotation = faceCamera, .scale = Vec3{0.5F, 1.0F, 0.8F}});
    const Mat4 mirrored = place * engine::scaling(Vec3{-1.0F, 1.0F, 1.0F});

    const auto planeAt = [](const Mat4& model) {
        engine::render::MeshInstance instance;
        instance.primitive = engine::render::PrimitiveId::Plane;
        instance.model = model;
        instance.mvp = model;
        instance.normalMatrix = Mat4::identity();
        instance.color = Vec3{0.0F, 1.0F, 0.0F};
        return instance;
    };
    const engine::render::SelectionOutlineParams params{
        .primaryColorSrgb = OG_PRIMARY_SRGB, .secondaryColorSrgb = OG_SECONDARY_SRGB, .radiusPixels = 2U};
    constexpr std::uint32_t ROW = 96U;
    constexpr std::uint32_t COLUMN = 128U;

    const auto renderPlane = [&](const Mat4& model, engine::render::MaterialHandle material) {
        engine::render::MeshInstance instance = planeAt(model);
        instance.material = material;
        const std::array<engine::render::MeshInstance, 1> scene{instance};
        const engine::render::RenderView view = flatView(scene);
        return renderOnce(*device, *post, *target, *forward, *outline, view, {}, scene, params);
    };

    const std::vector<std::byte> front = renderPlane(place, {});
    const std::vector<std::byte> back = renderPlane(mirrored, {});
    const bool frontDrew = texelAt(front, OG_W, ROW, COLUMN) != Rgba{0U, 0U, 0U, 255U};
    const bool backDrew = texelAt(back, OG_W, ROW, COLUMN) != Rgba{0U, 0U, 0U, 255U};
    INFO("front drew = ", frontDrew, ", back drew = ", backDrew);
    // EXACTLY ONE of the two orientations is in the picture -- if BOTH were, this case would be
    // asserting nothing about culling at all.
    CHECK(frontDrew != backDrew);

    const std::vector<std::byte>& visible = frontDrew ? front : back;
    const std::vector<std::byte>& culled = frontDrew ? back : front;
    // THE PAIRING: the visible one has a band, the culled one has NONE ANYWHERE.
    CHECK(countBandTexels(visible, OG_W, ROW, 0U, OG_W - 1U) > 0);
    CHECK(countBandTexels(culled, OG_W, ROW, 0U, OG_W - 1U) == 0);

    SUBCASE("...and a doubleSided material puts BOTH back") {
        // FROM DEFAULT_MATERIAL_PARAMS, never from MaterialParams{}: the struct's own default is
        // glTF's metallicFactor = 1, and a metal with no environment to reflect renders NEAR-BLACK
        // under analytic lights -- so a MaterialParams{} quad IS drawn and is indistinguishable from
        // the background, which would make the arm below assert nothing at all.
        engine::render::MaterialParams twoSidedParams = engine::render::DEFAULT_MATERIAL_PARAMS;
        twoSidedParams.doubleSided = true;
        const engine::render::MaterialHandle twoSided = forward->createMaterial(twoSidedParams, {});
        REQUIRE(twoSided.valid());
        const std::vector<std::byte> frontTwo = renderPlane(place, twoSided);
        const std::vector<std::byte> backTwo = renderPlane(mirrored, twoSided);
        CHECK(texelAt(frontTwo, OG_W, ROW, COLUMN) != Rgba{0U, 0U, 0U, 255U});
        CHECK(texelAt(backTwo, OG_W, ROW, COLUMN) != Rgba{0U, 0U, 0U, 255U});
        CHECK(countBandTexels(frontTwo, OG_W, ROW, 0U, OG_W - 1U) > 0);
        CHECK(countBandTexels(backTwo, OG_W, ROW, 0U, OG_W - 1U) > 0);
    }
}

TEST_CASE("render selection outline: a SKINNED instance masks where its palette puts it (OG11)") {
    AERO_OG_PREAMBLE();
    // A two-triangle quad whose every vertex is bound to palette slot 0 with weight 1, so ONE
    // palette matrix moves the whole quad by a known amount -- and the mask must follow it, because
    // the mask pass pairs with scene_skinned.vert UNCHANGED and pushes the SAME palette block draw()
    // does.
    constexpr std::array<Vec3, 4> POSITIONS{Vec3{-0.25F, -0.4F, 0.45F}, Vec3{0.25F, -0.4F, 0.45F},
                                            Vec3{0.25F, 0.4F, 0.45F}, Vec3{-0.25F, 0.4F, 0.45F}};
    constexpr std::array<Vec3, 4> NORMALS{Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 0.0F, -1.0F},
                                          Vec3{0.0F, 0.0F, -1.0F}};
    constexpr std::array<engine::Vec4, 4> TANGENTS{
        engine::Vec4{1.0F, 0.0F, 0.0F, 1.0F}, engine::Vec4{1.0F, 0.0F, 0.0F, 1.0F},
        engine::Vec4{1.0F, 0.0F, 0.0F, 1.0F}, engine::Vec4{1.0F, 0.0F, 0.0F, 1.0F}};
    constexpr std::array<Vec2, 4> UV0{Vec2{0.0F, 0.0F}, Vec2{1.0F, 0.0F}, Vec2{1.0F, 1.0F}, Vec2{0.0F, 1.0F}};
    constexpr std::array<std::array<std::uint16_t, 4>, 4> JOINTS{
        std::array<std::uint16_t, 4>{0, 0, 0, 0}, std::array<std::uint16_t, 4>{0, 0, 0, 0},
        std::array<std::uint16_t, 4>{0, 0, 0, 0}, std::array<std::uint16_t, 4>{0, 0, 0, 0}};
    constexpr std::array<engine::Vec4, 4> WEIGHTS{
        engine::Vec4{1.0F, 0.0F, 0.0F, 0.0F}, engine::Vec4{1.0F, 0.0F, 0.0F, 0.0F},
        engine::Vec4{1.0F, 0.0F, 0.0F, 0.0F}, engine::Vec4{1.0F, 0.0F, 0.0F, 0.0F}};
    // BOTH windings, so back-face culling cannot decide the outcome -- the case is about the palette.
    constexpr std::array<std::uint32_t, 12> INDICES{0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};

    engine::assets::MeshCookPrimitive primitive;
    primitive.positions = POSITIONS;
    primitive.normals = NORMALS;
    primitive.tangents = TANGENTS;
    primitive.uv0 = UV0;
    primitive.joints = JOINTS;
    primitive.weights = WEIGHTS;
    primitive.indices = INDICES;
    const std::array<engine::assets::MeshCookPrimitive, 1> primitives{primitive};
    engine::assets::MeshCookResult cooked = engine::assets::cookMesh({.sourceGuid = {}, .primitives = primitives});
    REQUIRE(cooked.status == engine::assets::MeshCookStatus::Ok);
    // The parse BUFFER must outlive createMesh: a CookedMesh RETAINS a span into it.
    const std::vector<std::byte> bytes = std::move(cooked.bytes);
    const engine::assets::CookedMeshParseResult parse = engine::assets::parseCookedMesh(bytes);
    REQUIRE(parse.status == engine::assets::CookedMeshStatus::Ok);
    const engine::render::MeshHandle mesh = forward->createMesh(parse.mesh);
    REQUIRE(mesh.valid());

    constexpr float SHIFT = 0.3F;
    const std::array<Mat4, 1> bindPalette{Mat4::identity()};
    const std::array<Mat4, 1> shiftedPalette{engine::translation(Vec3{SHIFT, 0.0F, 0.0F})};
    const engine::render::SelectionOutlineParams params{
        .primaryColorSrgb = OG_PRIMARY_SRGB, .secondaryColorSrgb = OG_SECONDARY_SRGB, .radiusPixels = 2U};
    constexpr std::uint32_t ROW = 96U;

    const auto rightBandColumn = [&](std::span<const Mat4> palette) {
        engine::render::MeshInstance instance;
        instance.mesh = mesh;
        instance.submesh = 0;
        instance.palette = palette;
        instance.model = Mat4::identity();
        instance.mvp = Mat4::identity();
        instance.normalMatrix = Mat4::identity();
        instance.color = Vec3{0.0F, 1.0F, 0.0F};
        const std::array<engine::render::MeshInstance, 1> scene{instance};
        const engine::render::RenderView view = flatView(scene);
        const std::vector<std::byte> pixels =
            renderOnce(*device, *post, *target, *forward, *outline, view, {}, scene, params);
        int last = -1;
        for (std::uint32_t c = 0; c < OG_W; ++c) {
            if (isBandTexel(texelAt(pixels, OG_W, ROW, c))) {
                last = static_cast<int>(c);
            }
        }
        return last;
    };

    const int bindRight = rightBandColumn(bindPalette);
    const int shiftedRight = rightBandColumn(shiftedPalette);
    INFO("bind-pose right band column = ", bindRight, ", shifted = ", shiftedRight);
    CHECK(bindRight > 0);  // anti-vacuity: the skinned quad masked at all
    CHECK(shiftedRight > 0);
    CHECK(forward->skinnedDrawCount() > 0U);  // ...through the SKINNED arm, not the static one
    // THE DIRECTION, not merely inequality: a palette that translates +X must move the band RIGHT by
    // the pixels that translation is worth, within the +/-2 the band's own edges allow.
    const int expectedShift = static_cast<int>(SHIFT * 0.5F * static_cast<float>(OG_W));
    CHECK(shiftedRight - bindRight >= expectedShift - 2);
    CHECK(shiftedRight - bindRight <= expectedShift + 2);
}

TEST_CASE("render selection outline: a Mask material warns EXACTLY ONCE per renderer (OG12)") {
    AERO_OG_PREAMBLE();
    // D6's stated gap: the mask stage has no UVs and must not discard, so a cut-out material is
    // masked as a SOLID quad and the outline traces the quad. Recorded rather than silently absent,
    // and LATCHED, because a 60 Hz WARN is a flood.
    engine::render::MaterialParams cutoutParams = engine::render::DEFAULT_MATERIAL_PARAMS;
    cutoutParams.alpha = engine::render::MaterialAlpha::Mask;
    const engine::render::MaterialHandle cutout = forward->createMaterial(cutoutParams, {});
    REQUIRE(cutout.valid());
    engine::render::MeshInstance quad = slab(0.0F, 0.0F, 0.25F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});
    quad.material = cutout;
    const std::array<engine::render::MeshInstance, 1> scene{quad};
    const engine::render::RenderView view = flatView(scene);
    const engine::render::SelectionOutlineParams params{
        .primaryColorSrgb = OG_PRIMARY_SRGB, .secondaryColorSrgb = OG_SECONDARY_SRGB, .radiusPixels = 2U};

    std::vector<std::string> warnings;
    const LogCallbackGuard detachOnExit;
    engine::setLogCallback([&warnings](const engine::LogRecord& record) {
        if (record.level >= engine::LogLevel::Warn && record.message.find("renderSelectionMask") != std::string::npos) {
            warnings.emplace_back(record.message);
        }
    });
    CHECK_FALSE(forward->hasWarnedSelectionMaskCaster());
    for (int frame = 0; frame < 10; ++frame) {
        (void)renderOnce(*device, *post, *target, *forward, *outline, view, {}, scene, params);
    }
    engine::setLogCallback({});
    CHECK(forward->hasWarnedSelectionMaskCaster());
    CHECK(forward->selectionMaskPassCount() == 10U);
    // ONE LINE over ten frames, counted rather than sampled.
    int maskCasterLines = 0;
    for (const std::string& message : warnings) {
        if (message.find("cannot discard") != std::string::npos) {
            ++maskCasterLines;
        }
    }
    CHECK(maskCasterLines == 1);
}

TEST_CASE("render selection outline: the mask is CLEARED every frame, never accumulated (OG13)") {
    AERO_OG_PREAMBLE();
    // A reused texture that is Loaded rather than Cleared shows every selection the renderer has ever
    // drawn -- and looks entirely plausible until you deselect something.
    const engine::render::MeshInstance a = slab(-0.5F, 0.0F, 0.15F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});
    const engine::render::MeshInstance b = slab(0.5F, 0.0F, 0.15F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});
    const std::array<engine::render::MeshInstance, 2> scene{a, b};
    const std::array<engine::render::MeshInstance, 1> selectA{a};
    const std::array<engine::render::MeshInstance, 1> selectB{b};
    const engine::render::RenderView view = flatView(scene);
    const engine::render::SelectionOutlineParams params{
        .primaryColorSrgb = OG_PRIMARY_SRGB, .secondaryColorSrgb = OG_SECONDARY_SRGB, .radiusPixels = 2U};

    constexpr std::uint32_t ROW = 96U;
    const std::uint32_t aLeft = columnForNdcX(-0.65F, OG_W);
    const std::uint32_t aRight = columnForNdcX(-0.35F, OG_W);
    const std::uint32_t bLeft = columnForNdcX(0.35F, OG_W);
    const std::uint32_t bRight = columnForNdcX(0.65F, OG_W);

    // TEN CONSECUTIVE FRAMES, alternating, and each must show ONLY its own selection.
    for (int frame = 0; frame < 10; ++frame) {
        const bool wantA = (frame % 2) == 0;
        INFO("frame ", frame, ", selecting ", wantA ? "A" : "B");
        const std::vector<std::byte> pixels = renderOnce(*device, *post, *target, *forward, *outline, view, {},
                                                         wantA ? std::span<const engine::render::MeshInstance>{selectA}
                                                               : std::span<const engine::render::MeshInstance>{selectB},
                                                         params);
        const int aBand = countBandTexels(pixels, OG_W, ROW, aLeft - 4U, aRight + 4U);
        const int bBand = countBandTexels(pixels, OG_W, ROW, bLeft - 4U, bRight + 4U);
        if (wantA) {
            CHECK(aBand > 0);
            CHECK(bBand == 0);  // LAST FRAME'S selection is gone, which is the whole case
        } else {
            CHECK(aBand == 0);
            CHECK(bBand > 0);
        }
    }
    CHECK(forward->selectionMaskPassCount() == 10U);
    CHECK(outline->compositeCount() == 10U);
}

TEST_CASE("render selection outline: a moved-to composites and a moved-from is SILENT (OG14)") {
    AERO_OG_PREAMBLE();
    const engine::render::MeshInstance quad = slab(0.0F, 0.0F, 0.25F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});
    const std::array<engine::render::MeshInstance, 1> scene{quad};
    const engine::render::RenderView view = flatView(scene);
    const engine::render::SelectionOutlineParams params{
        .primaryColorSrgb = OG_PRIMARY_SRGB, .secondaryColorSrgb = OG_SECONDARY_SRGB, .radiusPixels = 2U};

    (void)renderOnce(*device, *post, *target, *forward, *outline, view, {}, scene, params);
    REQUIRE(outline->compositeCount() == 1U);

    // MOVE-CONSTRUCT. The count TRANSFERS and the destination composites; the source is inert.
    engine::render::SelectionOutline moved{std::move(*outline)};
    CHECK(moved.compositeCount() == 1U);
    const std::vector<std::byte> afterMove =
        renderOnce(*device, *post, *target, *forward, moved, view, {}, scene, params);
    CHECK(moved.compositeCount() == 2U);
    CHECK(countBandTexels(afterMove, OG_W, 96U, 0U, OG_W - 1U) > 0);

    // THE MOVED-FROM OBJECT: a LATCHED no-op, and its count is back to zero rather than stale.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move) -- that IS the subject
    CHECK(outline->compositeCount() == 0U);
    std::optional<engine::render::Frame> outFrame = target->beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(outFrame.has_value());
    const engine::render::SelectionMaskView pretend{
        .texture = target->colorTexture(), .textureExtent = {OG_W, OG_H}, .drawExtent = {OG_W, OG_H}, .valid = true};
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    outline->composite(*outFrame, pretend, params);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    outline->composite(*outFrame, pretend, params);
    REQUIRE(target->endFrame(std::move(*outFrame)));
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    CHECK(outline->compositeCount() == 0U);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    CHECK(outline->hasWarnedNotRenderable());

    SUBCASE("move-ASSIGN releases the destination's own handles rather than leaking them") {
        auto second =
            engine::render::SelectionOutline::create(*device, vfs, {.outputColorFormat = target->colorFormat()});
        REQUIRE(second.has_value());
        *second = std::move(moved);
        CHECK(second->compositeCount() == 2U);
        (void)renderOnce(*device, *post, *target, *forward, *second, view, {}, scene, params);
        CHECK(second->compositeCount() == 3U);
    }
}

TEST_CASE("render selection outline: selectionMask = false builds NOTHING and latches ONCE (OG15)") {
    AERO_OG_PREAMBLE();
    auto disabled = engine::render::ForwardRenderer::create(*device, vfs,
                                                            {.colorFormat = post->sceneColorFormat(),
                                                             .depthFormat = post->sceneDepthFormat(),
                                                             .shadowMapResolution = 0,
                                                             .selectionMask = false});
    REQUIRE(disabled.has_value());  // the escape hatch does NOT fail create()
    const engine::render::MeshInstance quad = slab(0.0F, 0.0F, 0.25F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});
    const std::array<engine::render::MeshInstance, 1> scene{quad};

    std::vector<std::string> warnings;
    const LogCallbackGuard detachOnExit;
    engine::setLogCallback([&warnings](const engine::LogRecord& record) {
        if (record.level >= engine::LogLevel::Warn && record.message.find("renderSelectionMask") != std::string::npos) {
            warnings.emplace_back(record.message);
        }
    });
    CHECK_FALSE(disabled->hasWarnedSelectionMaskUnavailable());
    for (int call = 0; call < 10; ++call) {
        const engine::render::SelectionMaskView view = disabled->renderSelectionMask(
            post->sceneDepthTexture(), post->sceneTextureExtent(), post->sceneDrawExtent(), {}, scene);
        CHECK_FALSE(view.valid);
        CHECK_FALSE(view.texture.valid());
    }
    engine::setLogCallback({});
    CHECK(disabled->hasWarnedSelectionMaskUnavailable());
    // NOTHING WAS ACQUIRED on any of the ten calls -- the counter is what says so.
    CHECK(disabled->selectionMaskPassCount() == 0U);
    CHECK(disabled->lastFrameSelectionMaskDrawn() == 0U);
    CHECK(static_cast<int>(warnings.size()) == 1);
}

TEST_CASE("render selection outline: a resize REALLOCATES exactly when the extent changes (OG16)") {
    AERO_OG_PREAMBLE();
    // small -> large -> small on a quantum-64 pair, so the middle step is a REAL reallocation and the
    // last is a KEPT one (nextTargetExtent's hysteresis: a shrink only reallocates once the need at
    // least halves). The mask is slaved to whatever extent it is told about, so the band must land
    // correctly at all three -- a skipped reallocation gives SDL a dimension mismatch instead.
    auto sizedPost = engine::render::PostProcess::create(*device, vfs, {128U, 96U},
                                                         {.outputColorFormat = engine::rhi::TextureFormat::RGBA8Unorm,
                                                          .outputDepthFormat = engine::rhi::TextureFormat::Invalid,
                                                          .sceneDepthStore = true,
                                                          .quantum = 64});
    REQUIRE(sizedPost.has_value());
    auto sizedTarget = engine::render::RenderTarget::create(
        *device, {128U, 96U}, {.colorFormat = engine::rhi::TextureFormat::RGBA8Unorm, .depth = false, .quantum = 64});
    REQUIRE(sizedTarget.has_value());

    const engine::render::MeshInstance quad = slab(0.0F, 0.0F, 0.25F, 0.4F, 0.4F, 0.5F, Vec3{0.0F, 1.0F, 0.0F});
    const std::array<engine::render::MeshInstance, 1> scene{quad};
    const engine::render::RenderView view = flatView(scene);
    const engine::render::SelectionOutlineParams params{
        .primaryColorSrgb = OG_PRIMARY_SRGB, .secondaryColorSrgb = OG_SECONDARY_SRGB, .radiusPixels = 2U};

    const auto stepTo = [&](std::uint32_t width, std::uint32_t height) {
        REQUIRE(sizedPost->resize({width, height}));
        REQUIRE(sizedTarget->resize({width, height}));
        const engine::rhi::Extent2D allocation = sizedTarget->textureExtent();
        const std::vector<std::byte> pixels =
            renderOnce(*device, *sizedPost, *sizedTarget, *forward, *outline, view, {}, scene, params);
        // The band's LEFT edge must land at the quad's own -0.25 NDC boundary IN THE DRAWN RECT, not
        // in the allocation -- which is the whole of D10 restated as a resize invariant.
        const std::uint32_t expected = columnForNdcX(-0.25F, width);
        int first = -1;
        for (std::uint32_t c = 0; c < width; ++c) {
            if (isBandTexel(texelAt(pixels, allocation.width, height / 2U, c))) {
                first = static_cast<int>(c);
                break;
            }
        }
        INFO("draw ", width, "x", height, " in allocation ", allocation.width, "x", allocation.height,
             ": first band column ", first, ", expected about ", expected);
        CHECK(first >= 0);
        CHECK(first >= static_cast<int>(expected) - 3);
        CHECK(first <= static_cast<int>(expected) + 3);
        return allocation;
    };

    const engine::rhi::Extent2D small = stepTo(128U, 96U);
    const engine::rhi::Extent2D large = stepTo(250U, 190U);
    const engine::rhi::Extent2D kept = stepTo(200U, 150U);
    // PER AXIS, because rhi::Extent2D carries no operator<< and a whole-struct comparison prints
    // `{?} == {?}` on the run that matters. Note 96 rounds UP to 128 at quantum 64: the small step's
    // allocation is 128x128, which is the sizing policy doing its job rather than a surprise.
    CHECK(small.width == 128U);
    CHECK(small.height == 128U);
    CHECK(large.width == 256U);  // a REAL reallocation on both axes
    CHECK(large.height == 192U);
    CHECK(kept.width == 256U);  // ...and a KEPT one, by nextTargetExtent's shrink hysteresis
    CHECK(kept.height == 192U);
}

TEST_CASE("render selection outline: the two depth accessors report what they own (OG17)") {
    AERO_OG_PREAMBLE();
    // R6 -- the arms commit 1's four defaulted growths would otherwise leave unasserted.
    SUBCASE("a depth-FREE RenderTarget reports an INVALID depth texture") {
        CHECK_FALSE(target->depthTexture().valid());  // the preamble's output target is .depth = false
        CHECK((target->depthFormat() == engine::rhi::TextureFormat::Invalid));
    }
    SUBCASE("a .depth = true, .depthStore = true target reports a VALID one") {
        auto stored = engine::render::RenderTarget::create(
            *device, {64U, 64U},
            {.colorFormat = engine::rhi::TextureFormat::RGBA8Unorm, .depth = true, .depthStore = true, .quantum = 1});
        REQUIRE(stored.has_value());
        CHECK(stored->depthTexture().valid());
        CHECK((stored->depthFormat() != engine::rhi::TextureFormat::Invalid));
        // ...and depthStore = FALSE changes NEITHER: the flag is about the STORE OP, never about
        // whether the texture exists. Nothing in the API can detect an unstored depth, which is the
        // whole reason the flag is opt-in and documented rather than inferred.
        auto unstored = engine::render::RenderTarget::create(
            *device, {64U, 64U}, {.colorFormat = engine::rhi::TextureFormat::RGBA8Unorm, .depth = true, .quantum = 1});
        REQUIRE(unstored.has_value());
        CHECK(unstored->depthTexture().valid());
    }
    SUBCASE("PostProcess FORWARDS the owned target's handle, and a moved-from reports invalid") {
        CHECK(post->sceneDepthTexture().valid());
        const engine::render::PostProcess moved{std::move(*post)};
        CHECK(moved.sceneDepthTexture().valid());
        // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move) -- that IS the subject
        CHECK_FALSE(post->sceneDepthTexture().valid());
    }
    SUBCASE("a sceneDepth = false pass reports an invalid handle") {
        // ForwardRenderer refuses an Invalid depthFormat, so this pass is not renderable through one
        // -- which is exactly why the accessor has to answer rather than assume.
        auto depthless = engine::render::PostProcess::create(
            *device, vfs, {64U, 64U},
            {.outputColorFormat = engine::rhi::TextureFormat::RGBA8Unorm, .sceneDepth = false, .quantum = 1});
        REQUIRE(depthless.has_value());
        CHECK_FALSE(depthless->sceneDepthTexture().valid());
        CHECK((depthless->sceneDepthFormat() == engine::rhi::TextureFormat::Invalid));
    }
}

#endif  // AERO_SHADER_TOOLS_ENABLED

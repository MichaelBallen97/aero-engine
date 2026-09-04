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
    const Vec2 uvMax{0.78125F, 0.729166F};
    const auto block = rs::detail::packSelectionOutlineFragment(params, step, uvMax);
    CHECK(block.size() == 48U);

    // A FULLY SPECIFIED expectation, so a padding byte cannot be indeterminate and a swapped pair
    // cannot hide: build the 48 bytes independently and compare the whole array.
    const std::array<float, 12> expected{0.125F, 0.25F, 0.375F, 0.5F,   0.625F,  0.75F,
                                         0.875F, 1.0F,  step.x, step.y, uvMax.x, uvMax.y};
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
    CHECK(floatAt(40) == uvMax.x);
    CHECK(floatAt(44) == uvMax.y);
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
    CHECK(outline.find("uUvMax") != std::string::npos);
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

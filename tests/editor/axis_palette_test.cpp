// tests/editor/axis_palette_test.cpp — task E.1.2: the axis palette's two spellings, and the pin
// that stops them drifting.
//
// UNGATED and tier 0: no GPU, no window, no ImGui context, no #if of any kind. It reaches
// render::linearToSrgbEncode because aero::render has been PUBLIC on aero_editor_core since 3.4.2.
#include <aero/editor/axis_palette.hpp>
#include <aero/render/tonemap.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>

namespace {

using engine::Vec4;
namespace ed = engine::editor;

// THE ENCODE PATH, exactly as the display pipeline runs it: the engine's own OETF, then the engine's
// own rounding. NOT a hand-written pow(x, 1/2.2) -- that would be a second implementation agreeing
// with the first by luck, which is the drift AX1 exists to catch rather than to commit.
[[nodiscard]] int encodedByte(float linear) {
    return static_cast<int>(std::lround(engine::render::linearToSrgbEncode(linear) * 255.0F));
}

struct AxisPair {
    const char* name;
    Vec4 linear;
    std::array<std::uint8_t, 3> srgb;
};

}  // namespace

TEST_CASE("editor axis palette: the linear and sRGB spellings are the SAME colour (AX1)") {
    // THE WHOLE POINT OF THE HEADER. Two authoritative copies of one colour drift the first time
    // anyone retunes one of them, and nothing else in the tree would notice: the linear value feeds
    // the GPU and the bytes feed ImGui, so a mismatch shows as "the grid axis and the inspector row
    // are slightly different reds" -- a defect nobody files and nobody can bisect.
    const std::array<AxisPair, 3> axes{{
        {"X", ed::AXIS_X_LINEAR, ed::AXIS_X_SRGB},
        {"Y", ed::AXIS_Y_LINEAR, ed::AXIS_Y_SRGB},
        {"Z", ed::AXIS_Z_LINEAR, ed::AXIS_Z_SRGB},
    }};
    for (const AxisPair& axis : axes) {
        CAPTURE(axis.name);
        const std::array<float, 3> channels{axis.linear.x, axis.linear.y, axis.linear.z};
        for (std::size_t c = 0; c < 3U; ++c) {
            CAPTURE(c);
            CAPTURE(channels[c]);
            CAPTURE(static_cast<int>(axis.srgb[c]));
            // EXACT. Measured on this tree for all nine components before the case was written, so
            // this is green on the first run rather than fitted to. A tolerance here would let a
            // fourth-decimal typo through, which is precisely the drift being prevented.
            CHECK(encodedByte(channels[c]) == static_cast<int>(axis.srgb[c]));
        }
    }
    SUBCASE("the encode really can say NO -- anti-vacuity for the nine checks above") {
        // Without this, an encodedByte that returned its own argument's byte would pass everything.
        CHECK(encodedByte(0.0F) == 0);
        CHECK(encodedByte(1.0F) == 255);
        CHECK(encodedByte(ed::AXIS_X_LINEAR.x) != static_cast<int>(ed::AXIS_Y_SRGB[0]));
        CHECK(encodedByte(0.5F) == 188);  // MEASURED. 0.5 LINEAR is NOT the midpoint byte --
                                          // that it is 188 rather than 128 is the OETF doing work.
    }
}

TEST_CASE("editor axis palette: the three colours are distinct, opaque, in range and dominant (AX2)") {
    const std::array<Vec4, 3> linear{ed::AXIS_X_LINEAR, ed::AXIS_Y_LINEAR, ed::AXIS_Z_LINEAR};

    SUBCASE("opaque, and every channel inside [0, 1]") {
        for (std::size_t i = 0; i < linear.size(); ++i) {
            CAPTURE(i);
            CHECK(linear[i].w == 1.0F);  // exact: an axis colour is never translucent
            for (const float channel : {linear[i].x, linear[i].y, linear[i].z}) {
                CHECK(channel >= 0.0F);
                CHECK(channel <= 1.0F);
                CHECK(std::isfinite(channel));
            }
        }
    }
    SUBCASE("each is DOMINANT in its own channel -- red X, green Y, blue Z") {
        // The convention Unity, Unreal, Blender, Godot and ImGuizmo share. A palette that swapped
        // two of them would still pass AX1 (both spellings would be consistently wrong), which is
        // exactly why this arm exists beside it.
        CHECK(ed::AXIS_X_LINEAR.x > ed::AXIS_X_LINEAR.y);
        CHECK(ed::AXIS_X_LINEAR.x > ed::AXIS_X_LINEAR.z);
        CHECK(ed::AXIS_Y_LINEAR.y > ed::AXIS_Y_LINEAR.x);
        CHECK(ed::AXIS_Y_LINEAR.y > ed::AXIS_Y_LINEAR.z);
        CHECK(ed::AXIS_Z_LINEAR.z > ed::AXIS_Z_LINEAR.x);
        CHECK(ed::AXIS_Z_LINEAR.z > ed::AXIS_Z_LINEAR.y);
    }
    SUBCASE("mutually distinct, in BOTH spellings") {
        CHECK_FALSE((ed::AXIS_X_LINEAR == ed::AXIS_Y_LINEAR));
        CHECK_FALSE((ed::AXIS_Y_LINEAR == ed::AXIS_Z_LINEAR));
        CHECK_FALSE((ed::AXIS_X_LINEAR == ed::AXIS_Z_LINEAR));
        CHECK_FALSE((ed::AXIS_X_SRGB == ed::AXIS_Y_SRGB));
        CHECK_FALSE((ed::AXIS_Y_SRGB == ed::AXIS_Z_SRGB));
        CHECK_FALSE((ed::AXIS_X_SRGB == ed::AXIS_Z_SRGB));
    }
    SUBCASE("visible against the viewport's clear colour, which is 0.06 linear grey") {
        // viewport_panel.cpp's VIEWPORT_CLEAR_COLOR is {0.06, 0.06, 0.07, 1.0}, LINEAR. An axis
        // whose dominant channel sat below that would be invisible on the default background --
        // stated as a relationship rather than as a number, so a retune of either end is caught.
        constexpr float CLEAR_MAX = 0.07F;
        CHECK(ed::AXIS_X_LINEAR.x > CLEAR_MAX);
        CHECK(ed::AXIS_Y_LINEAR.y > CLEAR_MAX);
        CHECK(ed::AXIS_Z_LINEAR.z > CLEAR_MAX);
    }
}

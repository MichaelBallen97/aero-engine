// tests/editor/ui_scale_test.cpp -- task E.6.1: the UI-scale model (US1-US6).
// TIER 0, EVERY CONFIGURATION: resolveUiScale is pure and total, so every case is a table over plain floats
// with no window, no device and no ImGui context. NO #if of any kind (the 3.6.3 rule). Exact float
// assertions throughout: the quantiser DIVIDES, so every expected value below is exact arithmetic.
#include <aero/editor/editor_theme.hpp>

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>  // MSVC: a CHECK over a std::string_view needs the complete std::ostream (the 0.4.1 trap)

namespace {

namespace ed = engine::editor;

// NaN from quiet_NaN(), never from a signalling one: MSVC quiets an sNaN that passes through a float lvalue.
constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
constexpr float INF_F = std::numeric_limits<float>::infinity();

}  // namespace

TEST_CASE("ui scale: the per-OS table (task E.6.1, US1)") {
    CHECK(ed::resolveUiScale(2.0F, 2.0F, 1.0F) == 1.0F);    // macOS Retina; Wayland 2x without the hint
    CHECK(ed::resolveUiScale(1.0F, 1.0F, 1.0F) == 1.0F);    // a 1x display anywhere
    CHECK(ed::resolveUiScale(2.0F, 1.0F, 1.0F) == 2.0F);    // Windows 200 %; X11 Xft.dpi 192; Wayland + hint
    CHECK(ed::resolveUiScale(1.25F, 1.0F, 1.0F) == 1.25F);  // Windows 125 %
    CHECK(ed::resolveUiScale(1.5F, 1.0F, 1.0F) == 1.5F);    // Windows 150 %
    CHECK(ed::resolveUiScale(1.75F, 1.0F, 1.0F) == 1.75F);  // Windows 175 %
    CHECK(ed::resolveUiScale(3.0F, 2.0F, 1.0F) == 1.5F);    // a scaled Retina mode
    CHECK(ed::resolveUiScale(1.5F, 1.5F, 1.0F) == 1.0F);    // density and display scale agree
}

TEST_CASE("ui scale: quantised by division (task E.6.1, US2)") {
    // Every quantum in [0.5, 4] is returned BIT-EQUAL to k / 20 -- and for 14 of those k, k * 0.05F is a
    // different float, so a multiplicative quantiser is red here (the anti-vacuity count below).
    int multiplicativeDiffers = 0;
    for (int k = 10; k <= 80; ++k) {
        CAPTURE(k);
        const float expected = static_cast<float>(k) / 20.0F;
        const float resolved = ed::resolveUiScale(expected, 1.0F, 1.0F);
        CHECK(std::bit_cast<std::uint32_t>(resolved) == std::bit_cast<std::uint32_t>(expected));
        if (static_cast<float>(k) * 0.05F != expected) {
            ++multiplicativeDiffers;
        }
    }
    // k = 13, 18, 21, 26, 31, 36, 42, 47, 52, 57, 62, 67, 72, 77 (measured).
    CHECK(multiplicativeDiffers == 14);
    CHECK(ed::resolveUiScale(1.1458F, 1.0F, 1.0F) == 23.0F / 20.0F);  // rounds to the NEAREST quantum
    // Density jitter around 2 (a fractional window width) still lands on exactly 1.
    CHECK(ed::resolveUiScale(2.0F, 2879.0F / 1440.0F, 1.0F) == 1.0F);
    CHECK(ed::resolveUiScale(2.0F, 2881.0F / 1440.0F, 1.0F) == 1.0F);
}

TEST_CASE("ui scale: clamped (task E.6.1, US3)") {
    CHECK(ed::resolveUiScale(100.0F, 1.0F, 1.0F) == 4.0F);
    CHECK(ed::resolveUiScale(4.02F, 1.0F, 1.0F) == 4.0F);
    CHECK(ed::resolveUiScale(0.3F, 1.0F, 1.0F) == 0.5F);
    CHECK(ed::resolveUiScale(0.5F, 1.0F, 1.0F) == 0.5F);
}

TEST_CASE("ui scale: unusable inputs keep previous (task E.6.1, US4)") {
    for (const float display : {0.0F, -1.0F, NAN_F, INF_F, -INF_F}) {
        CAPTURE(display);
        CHECK(ed::resolveUiScale(display, 1.0F, 1.25F) == 1.25F);
    }
    for (const float density : {0.0F, -2.0F, NAN_F, INF_F}) {
        CAPTURE(density);
        CHECK(ed::resolveUiScale(2.0F, density, 1.25F) == 1.25F);
    }
    // A previous that cannot be kept is replaced by 1.
    for (const float previous : {NAN_F, 0.0F, -1.0F, INF_F}) {
        CAPTURE(previous);
        CHECK(ed::resolveUiScale(0.0F, 1.0F, previous) == 1.0F);
    }
    // Usable inputs ignore previous entirely.
    CHECK(ed::resolveUiScale(2.0F, 1.0F, NAN_F) == 2.0F);
    CHECK(ed::resolveUiScale(2.0F, 1.0F, 3.5F) == 2.0F);
}

TEST_CASE("ui scale: noexcept and deterministic (task E.6.1, US5)") {
    static_assert(noexcept(ed::resolveUiScale(1.0F, 1.0F, 1.0F)));
    const float first = ed::resolveUiScale(1.37F, 1.0F, 1.0F);
    const float second = ed::resolveUiScale(1.37F, 1.0F, 1.0F);
    CHECK(std::bit_cast<std::uint32_t>(first) == std::bit_cast<std::uint32_t>(second));
}

TEST_CASE("ui scale: total over every special (task E.6.1, US6)") {
    constexpr std::array SPECIALS{0.0F, -1.0F, NAN_F, INF_F, -INF_F, -2.0F, 1.25F, 1.0F, 2.0F, 1.5F};
    std::size_t checked = 0;
    for (const float display : SPECIALS) {
        for (const float density : SPECIALS) {
            for (const float previous : SPECIALS) {
                CAPTURE(display);
                CAPTURE(density);
                CAPTURE(previous);
                const float scale = ed::resolveUiScale(display, density, previous);
                CHECK(std::isfinite(scale));
                CHECK(scale >= ed::UI_SCALE_MIN);
                CHECK(scale <= ed::UI_SCALE_MAX);
                ++checked;
            }
        }
    }
    CHECK(checked == 1000U);
    // A KEPT value is clamped like a computed one (decision D-8): a previous past the range never escapes.
    CHECK(ed::resolveUiScale(0.0F, 1.0F, 10.0F) == 4.0F);
}

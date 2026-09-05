// tests/editor/gizmo_style_test.cpp — task E.1.5: the pure ImGuizmo style and screen-size model.
// Joins aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. Tier-0 and UNGATED: no GPU, no window, no ImGui context,
// and it must pass identically with AERO_REQUIRE_GPU unset and set. NO #if of any kind.
//
// EVERY CASE ASSERTS A RELATIONSHIP, NEVER A MAGNITUDE (gizmo.hpp:66-67's rule, inherited). The
// numbers in gizmo_style.hpp are tuning values judged on the manual validation pass; a case that
// pinned one would turn a retune into a red test and teach nothing.
//
// It reaches render::SELECTION_OUTLINE_PRIMARY_DEFAULT because aero::render has been PUBLIC on
// aero_editor_core since 3.4.2 -- axis_palette_test.cpp's AX1 is the precedent for that exact reach.
#include <aero/editor/gizmo_style.hpp>
#include <aero/render/selection_outline.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <ostream>
#include <random>

namespace {

namespace ed = engine::editor;

// A COMPARISON STRUCT IN THE FILE'S ANONYMOUS NAMESPACE, NOT INSIDE A TEST_CASE. [class.friend]/6
// forbids defining a friend in a local class, and doctest's stringification finds an operator<< by
// ADL -- which cannot reach one declared for std::array<std::uint8_t, 4>, because that type's
// associated namespace is std. Wrapping is what makes both work. render_debug_draw_test.cpp:793 is
// the precedent. It is an operator<<, NEVER a toString: an unqualified toString is the ADL trap that
// hard-errors inside doctest.h on every lane.
struct Rgba {
    int r = 0;
    int g = 0;
    int b = 0;
    int a = 0;
    [[nodiscard]] bool operator==(const Rgba&) const = default;
};

std::ostream& operator<<(std::ostream& out, const Rgba& value) {
    out << "rgba(" << value.r << ", " << value.g << ", " << value.b << ", " << value.a << ")";
    return out;
}

[[nodiscard]] Rgba asRgba(ed::Rgba8 bytes) noexcept {
    return Rgba{static_cast<int>(bytes[0]), static_cast<int>(bytes[1]), static_cast<int>(bytes[2]),
                static_cast<int>(bytes[3])};
}

[[nodiscard]] Rgba colorOf(const ed::GizmoStyle& style, ed::GizmoColor slot) noexcept {
    return asRgba(style.colors[static_cast<std::size_t>(slot)]);
}

// The enum is a MIRROR of ImGuizmo::COLOR, so its three axis slots and its three plane slots are each
// contiguous and in Axis order -- which is what lets GS1 loop over AXIS_COUNT and catch a fourth axis
// as a red case rather than as a silent gap.
[[nodiscard]] ed::GizmoColor axisSlot(std::size_t index) noexcept {
    return static_cast<ed::GizmoColor>(static_cast<std::size_t>(ed::GizmoColor::AxisX) + index);
}

[[nodiscard]] ed::GizmoColor planeSlot(std::size_t index) noexcept {
    return static_cast<ed::GizmoColor>(static_cast<std::size_t>(ed::GizmoColor::PlaneX) + index);
}

// The same wrapping for GizmoScreenSize, for the same reason: a bare CHECK((a == b)) on the engine
// type prints `CHECK( true )` on a FAILURE as well as a pass, which makes the assertion carrying the
// deliverable unreadable at exactly the wrong moment.
struct Size4 {
    float axisLengthPoints = 0.0F;
    float clipSpaceSize = 0.0F;
    float axisHideClipLength = 0.0F;
    float planeHideClipArea = 0.0F;
    [[nodiscard]] bool operator==(const Size4&) const = default;
};

std::ostream& operator<<(std::ostream& out, const Size4& value) {
    out << "{L=" << value.axisLengthPoints << ", size=" << value.clipSpaceSize
        << ", axisHide=" << value.axisHideClipLength << ", planeHide=" << value.planeHideClipArea << "}";
    return out;
}

[[nodiscard]] Size4 asSize4(const ed::GizmoScreenSize& size) noexcept {
    return Size4{size.axisLengthPoints, size.clipSpaceSize, size.axisHideClipLength, size.planeHideClipArea};
}

[[nodiscard]] Size4 resolved(float w, float h) noexcept {
    return asSize4(ed::resolveGizmoScreenSize(engine::Vec2{w, h}));
}

// ULPs between two finite floats, as an integer, so an assertion can say "two ulps" and PRINT the
// number it measured. std::bit_cast is C++20 and is already used across engine/assets and five test
// TUs, so this introduces nothing new.
[[nodiscard]] std::int64_t ulpsBetween(float a, float b) noexcept {
    if (!std::isfinite(a) || !std::isfinite(b)) {
        return std::numeric_limits<std::int64_t>::max();
    }
    const auto ia = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(a));
    const auto ib = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(b));
    return ia > ib ? ia - ib : ib - ia;
}

// E.1.4's SO4 conversion: a display-referred Vec4 to its bytes. Used ONCE, by GS2, to compare the
// highlight against the selection outline's amber without restating either.
[[nodiscard]] Rgba asRgbaFromUnit(const engine::Vec4& color) noexcept {
    const auto toByte = [](float v) { return static_cast<int>(std::lround(v * 255.0F)); };
    return Rgba{toByte(color.x), toByte(color.y), toByte(color.z), toByte(color.w)};
}

[[nodiscard]] int maxChannelGap(const Rgba& a, const Rgba& b) noexcept {
    const std::array<int, 3> gaps{std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)};
    return std::max(gaps[0], std::max(gaps[1], gaps[2]));
}

// A DETERMINISTIC viewport generator. std::mt19937 IS specified by the standard;
// std::uniform_real_distribution is NOT -- its output differs between libstdc++, libc++ and MS STL,
// so the three lanes would sample different viewports and a lane-specific failure would be
// unreproducible. Derive the float arithmetically from the raw 32-bit output instead.
[[nodiscard]] float nextViewportSide(std::mt19937& rng) noexcept {
    const auto whole = static_cast<float>(rng() % 16384U);
    const float fraction = static_cast<float>(rng() % 1024U) / 1024.0F;
    return 1.0F + whole + fraction;
}

// The gap floor GS2 rests on. FILE-LOCAL, and load-bearing: without it a one-byte "distinct" would
// satisfy the case, which is E.1.4's sabotage row 20 turned into an assertion instead of a lesson.
constexpr int HIGHLIGHT_MIN_CHANNEL_GAP = 32;

// GS4's and GS8's shared table. Every entry's LARGER side is a power-of-two multiple of
// GIZMO_AXIS_LENGTH_POINTS, so 2L / max(w, h) is EXACT in binary32 and the expected sizes below can
// be literals rather than a second evaluation of the formula under test -- which is what stops this
// from being E.1.2's "both sides computed from one source" failure. Two entries are PORTRAIT.
struct DyadicViewport {
    float width = 0.0F;
    float height = 0.0F;
    float expectedClipSpaceSize = 0.0F;
};

constexpr std::array<DyadicViewport, 6> DYADIC_VIEWPORTS{{{1440.0F, 900.0F, 0.125F},
                                                          {720.0F, 720.0F, 0.25F},
                                                          {2880.0F, 900.0F, 0.0625F},
                                                          {5760.0F, 1200.0F, 0.03125F},
                                                          {900.0F, 1440.0F, 0.125F},
                                                          {1200.0F, 5760.0F, 0.03125F}}};

}  // namespace

TEST_CASE(
    "editor gizmo style: the axis colours are DERIVED from the palette and the planes share their bytes "
    "(task E.1.5, GS1)") {
    // D6/INV-1. The three axis colours are stated ONCE in the tree, in axis_palette.hpp; this header
    // derives them. That is what makes the gizmo's X arrow, the grid's X line and the corner widget's
    // +X ball the same three bytes -- validation row 1 reads all three off one screenshot.
    const ed::GizmoStyle style = ed::defaultGizmoStyle();

    SUBCASE("each axis slot is the palette's bytes at full alpha") {
        for (std::size_t i = 0; i < ed::AXIS_COUNT; ++i) {
            CAPTURE(i);
            const std::array<std::uint8_t, 3> palette = ed::axisColorSrgbBytes(static_cast<ed::Axis>(i));
            const Rgba expected{static_cast<int>(palette[0]), static_cast<int>(palette[1]),
                                static_cast<int>(palette[2]), 255};
            CHECK(colorOf(style, axisSlot(i)) == expected);
        }
    }

    SUBCASE("each plane slot is its OWN axis slot's bytes at the plane alpha") {
        // Read off the STYLE, not off the palette a second time: a plane that took some other
        // colour's bytes at the right alpha would satisfy a palette-vs-palette comparison.
        for (std::size_t i = 0; i < ed::AXIS_COUNT; ++i) {
            CAPTURE(i);
            const Rgba axis = colorOf(style, axisSlot(i));
            const Rgba plane = colorOf(style, planeSlot(i));
            CHECK(plane.r == axis.r);
            CHECK(plane.g == axis.g);
            CHECK(plane.b == axis.b);
            CHECK(plane.a == static_cast<int>(ed::GIZMO_PLANE_FILL_ALPHA));
        }
        CHECK(static_cast<int>(ed::GIZMO_PLANE_FILL_ALPHA) < 255);
    }

    SUBCASE("the three axis colours are pairwise distinct") {
        // ANTI-VACUITY for both arms above: a derivation that returned one shared array would satisfy
        // every equality in this case and fail here.
        CHECK(colorOf(style, ed::GizmoColor::AxisX) != colorOf(style, ed::GizmoColor::AxisY));
        CHECK(colorOf(style, ed::GizmoColor::AxisY) != colorOf(style, ed::GizmoColor::AxisZ));
        CHECK(colorOf(style, ed::GizmoColor::AxisX) != colorOf(style, ed::GizmoColor::AxisZ));
    }
}

TEST_CASE("editor gizmo style: the highlight is OPAQUE and is not the selection outline's amber (task E.1.5, GS2)") {
    // D7. A 54 %-alpha handle is a DIFFERENT COLOUR on each side of an edge -- E.1.4's own validation
    // pass measured ImGuizmo's Y arrow at rgb(237,137,48) on one side and rgb(156,87,28) on the other
    // in ONE frame. And the gizmo's origin sits inside the selected object, so a hot handle in the
    // outline's colour would read as part of the outline.
    const ed::GizmoStyle style = ed::defaultGizmoStyle();
    const Rgba highlight = colorOf(style, ed::GizmoColor::Highlight);

    SUBCASE("it is fully opaque") { CHECK(highlight.a == 255); }

    SUBCASE("it stands off every axis colour AND the outline's amber by a per-channel floor") {
        // THE FLOOR IS THE ASSERTION, not "differs": a one-byte difference passes != and fails this.
        // The amber is read from render::'s OWN constant through E.1.4's own conversion, so a retune
        // THERE is caught here rather than silently narrowing the gap.
        for (std::size_t i = 0; i < ed::AXIS_COUNT; ++i) {
            CAPTURE(i);
            const int gap = maxChannelGap(highlight, colorOf(style, axisSlot(i)));
            CAPTURE(gap);
            CHECK(gap >= HIGHLIGHT_MIN_CHANNEL_GAP);
        }
        const Rgba amber = asRgbaFromUnit(engine::render::SELECTION_OUTLINE_PRIMARY_DEFAULT);
        const int amberGap = maxChannelGap(highlight, amber);
        CAPTURE(amber);
        CAPTURE(amberGap);
        CHECK(amberGap >= HIGHLIGHT_MIN_CHANNEL_GAP);
    }

    SUBCASE("the rotate sector DERIVES from the highlight rather than restating it") {
        CHECK(colorOf(style, ed::GizmoColor::RotationBorder) == highlight);
        const Rgba fill = colorOf(style, ed::GizmoColor::RotationFill);
        CHECK(fill.r == highlight.r);
        CHECK(fill.g == highlight.g);
        CHECK(fill.b == highlight.b);
        CHECK(fill.a < colorOf(style, ed::GizmoColor::RotationBorder).a);
    }
}

TEST_CASE(
    "editor gizmo style: every drawn dimension is positive, ordered and inside the library's own hit box "
    "(task E.1.5, GS3)") {
    // D8. Each clause is a CORRECTNESS claim, never a taste one -- the magnitudes themselves are
    // judged on validation rows 1-6 and 10, and nothing here pins one.
    const ed::GizmoStyle style = ed::defaultGizmoStyle();

    // A zero thickness is an invisible handle.
    CHECK(style.translationLineThicknessPoints > 0.0F);
    CHECK(style.translationArrowSizePoints > 0.0F);
    CHECK(style.rotationLineThicknessPoints > 0.0F);
    CHECK(style.rotationScreenRingThicknessPoints > 0.0F);
    CHECK(style.scaleLineThicknessPoints > 0.0F);
    CHECK(style.scaleDiscRadiusPoints > 0.0F);
    CHECK(style.centerDiscRadiusPoints > 0.0F);
    // D12: EXACTLY zero, and the exception proves the rule. AllowAxisFlip(false) makes DrawHatchedAxis
    // unreachable, and the library early-returns on <= 0 (ImGuizmo.cpp:1389), so the value DOCUMENTS
    // that no negative axis is ever drawn. A task that re-enables flip must pick one deliberately.
    CHECK(style.hatchedAxisThicknessPoints == 0.0F);

    // A head narrower than its own shaft is not a head.
    CHECK(style.translationArrowSizePoints > style.translationLineThicknessPoints);
    // A centre disc drawn larger than the library's hard-coded +/-10-point hit square (ImGuizmo.cpp:
    // 1132-1133) LIES about where it can be grabbed. Sabotage row 17 sets the radius to 12.
    CHECK(style.centerDiscRadiusPoints <= ed::GIZMO_CENTER_HIT_HALF_EXTENT_POINTS);
    // The screen-facing white ring must not dominate the three axis rings it encloses.
    CHECK(style.rotationScreenRingThicknessPoints <= style.rotationLineThicknessPoints);
    // A disc thinner than half its own shaft is not an end cap.
    CHECK(style.scaleDiscRadiusPoints >= style.scaleLineThicknessPoints / 2.0F);

    // A TINTED "disabled" reads as a fourth axis. Achromatic is what makes it read as absence.
    const Rgba inactive = colorOf(style, ed::GizmoColor::Inactive);
    CHECK(inactive.r == inactive.g);
    CHECK(inactive.g == inactive.b);
    // A shadow the colour of its own text is not a shadow.
    CHECK(colorOf(style, ed::GizmoColor::Text) != colorOf(style, ed::GizmoColor::TextShadow));

    for (std::size_t i = 0; i < ed::AXIS_COUNT; ++i) {
        CAPTURE(i);
        CHECK(colorOf(style, axisSlot(i)).a == 255);
        CHECK(colorOf(style, planeSlot(i)).a < 255);
    }
    CHECK(colorOf(style, ed::GizmoColor::RotationFill).a < colorOf(style, ed::GizmoColor::RotationBorder).a);
}

TEST_CASE(
    "editor gizmo style: the clip-space size is exactly 2L / max(w, h), and inverting it returns L "
    "(task E.1.5, GS4)") {
    // D3. One screen point is 2 / max(w, h) of the library's unit in BOTH orientations -- derived from
    // GetSegmentLengthClipSpace's two arms (ImGuizmo.cpp:880-884), not quoted.
    SUBCASE("the dyadic table resolves EXACTLY, to literals rather than to the formula again") {
        for (const DyadicViewport& entry : DYADIC_VIEWPORTS) {
            CAPTURE(entry.width);
            CAPTURE(entry.height);
            const Size4 size = resolved(entry.width, entry.height);
            CHECK(size.clipSpaceSize == entry.expectedClipSpaceSize);
            CHECK(size.axisLengthPoints == ed::GIZMO_AXIS_LENGTH_POINTS);
        }
    }

    SUBCASE("the INVERSE property holds -- size * max(w, h) / 2 recovers the axis length") {
        // A DIFFERENT expression from the resolver's, which is what catches a dropped factor of 2
        // (sabotage row 10) or an uncapped length used for the size (row 11). Measured worst: ONE ulp
        // over 300 000 seeded viewports; the bound here is two.
        constexpr std::int64_t MAX_INVERSE_ULPS = 2;
        for (const DyadicViewport& entry : DYADIC_VIEWPORTS) {
            CAPTURE(entry.width);
            CAPTURE(entry.height);
            const Size4 size = resolved(entry.width, entry.height);
            const float recovered = (size.clipSpaceSize * std::max(entry.width, entry.height)) / 2.0F;
            const std::int64_t distance = ulpsBetween(recovered, size.axisLengthPoints);
            CAPTURE(recovered);
            CAPTURE(distance);
            CHECK(distance <= MAX_INVERSE_ULPS);
        }

        std::mt19937 rng(0xE15A4U);
        std::int64_t worstDistance = 0;
        float worstWidth = 0.0F;
        float worstHeight = 0.0F;
        for (int sample = 0; sample < 20; ++sample) {
            const float w = nextViewportSide(rng);
            const float h = nextViewportSide(rng);
            const Size4 size = resolved(w, h);
            const float recovered = (size.clipSpaceSize * std::max(w, h)) / 2.0F;
            const std::int64_t distance = ulpsBetween(recovered, size.axisLengthPoints);
            if (distance > worstDistance) {
                worstDistance = distance;
                worstWidth = w;
                worstHeight = h;
            }
        }
        CAPTURE(worstWidth);
        CAPTURE(worstHeight);
        CAPTURE(worstDistance);
        CHECK(worstDistance <= MAX_INVERSE_ULPS);
    }
}

TEST_CASE("editor gizmo style: below the knee the gizmo scales with the dock (task E.1.5, GS5)") {
    // D3's other arm. The knee is the one deliberate departure from Unity, whose handle is
    // unconditionally constant and does overflow a tiny scene view: the rotation rings are 1.2 * L in
    // RADIUS (ImGuizmo.cpp:52), a 216-point diameter at L = 90, which does not fit a 300-point dock.
    constexpr float LARGER_SIDE = 2000.0F;
    const std::array<float, 4> smallerSides{100.0F, 300.0F, 400.0F, 599.0F};
    for (const float smaller : smallerSides) {
        CAPTURE(smaller);
        const Size4 size = resolved(smaller, LARGER_SIDE);
        const float ratio = size.axisLengthPoints / smaller;
        CAPTURE(ratio);
        // The epsilon lives IN the assertion (D8's rule). Measured, the ratio is bit-exactly 0.15F at
        // all four of these points on this machine -- recorded here, deliberately NOT asserted,
        // because nothing guarantees it in general.
        CHECK(ratio == doctest::Approx(ed::GIZMO_AXIS_MAX_VIEWPORT_FRACTION).epsilon(1e-6));
        // THE ANTI-VACUITY: without the strict <, a resolver that ignored the knee and always
        // returned GIZMO_AXIS_LENGTH_POINTS would satisfy the ratio test at exactly one dock size.
        CHECK(size.axisLengthPoints < ed::GIZMO_AXIS_LENGTH_POINTS);
    }
}

TEST_CASE(
    "editor gizmo style: the axis length is monotone, capped, flat above the knee and continuous "
    "(task E.1.5, GS6)") {
    // 1 951 samples across the knee, which sits at GIZMO_AXIS_LENGTH_POINTS / the fraction = 600
    // points. The STEP BOUND is what makes "continuous" an assertion rather than a word.
    constexpr float LARGER_SIDE = 4000.0F;
    const float knee = ed::GIZMO_AXIS_LENGTH_POINTS / ed::GIZMO_AXIS_MAX_VIEWPORT_FRACTION;

    bool monotone = true;
    bool capped = true;
    bool flatAboveKnee = true;
    float firstDecreaseAt = 0.0F;
    float firstOverCapAt = 0.0F;
    float firstNotFlatAt = 0.0F;
    float largestStep = 0.0F;
    float previous = -1.0F;
    for (int smaller = 50; smaller <= 2000; ++smaller) {
        const auto side = static_cast<float>(smaller);
        const Size4 size = resolved(side, LARGER_SIDE);
        if (previous >= 0.0F) {
            if (size.axisLengthPoints < previous) {
                monotone = false;
                firstDecreaseAt = (firstDecreaseAt == 0.0F) ? side : firstDecreaseAt;
            }
            largestStep = std::max(largestStep, size.axisLengthPoints - previous);
        }
        if (size.axisLengthPoints > ed::GIZMO_AXIS_LENGTH_POINTS) {
            capped = false;
            firstOverCapAt = (firstOverCapAt == 0.0F) ? side : firstOverCapAt;
        }
        if (side >= knee && size.axisLengthPoints != ed::GIZMO_AXIS_LENGTH_POINTS) {
            flatAboveKnee = false;
            firstNotFlatAt = (firstNotFlatAt == 0.0F) ? side : firstNotFlatAt;
        }
        previous = size.axisLengthPoints;
    }

    CAPTURE(knee);
    CAPTURE(firstDecreaseAt);
    CHECK(monotone);
    CAPTURE(firstOverCapAt);
    CHECK(capped);
    CAPTURE(firstNotFlatAt);
    CHECK(flatAboveKnee);
    // A resolver that JUMPED at the knee shows a step far larger than the per-point fraction.
    CAPTURE(largestStep);
    CHECK(largestStep <= ed::GIZMO_AXIS_MAX_VIEWPORT_FRACTION + 1e-5F);
}

TEST_CASE("editor gizmo style: the resolver is aspect-independent (task E.1.5, GS7)") {
    // THE DIRECT WITNESS for the derivation: one screen point is 2 / max(w, h) in BOTH orientations,
    // which is why the formula uses max and not w. Sabotage row 8 replaces `larger` with `w` and every
    // non-square entry here reddens.
    const std::array<std::array<float, 2>, 6> pairs{
        {{1440.0F, 900.0F}, {900.0F, 1440.0F}, {1.0F, 10000.0F}, {10000.0F, 1.0F}, {640.0F, 480.0F}, {1.0F, 1.0F}}};
    for (const std::array<float, 2>& pair : pairs) {
        CAPTURE(pair[0]);
        CAPTURE(pair[1]);
        CHECK(resolved(pair[0], pair[1]) == resolved(pair[1], pair[0]));
    }
}

TEST_CASE(
    "editor gizmo style: the two hide thresholds are proportional to the size, and the reference point is "
    "the library's (task E.1.5, GS8)") {
    // D5. The library's defaults are calibrated against ITS size -- 0.02 is 0.2 * 0.1 and 0.0025 is
    // 0.25 * 0.1^2 -- so a CONSTANT threshold would hide axes at a different foreshortening in every
    // dock once the size varies per viewport.
    SUBCASE("both ratios are the SAME value at every dyadic viewport") {
        // Exact, because scaling by a power of two is exact.
        const Size4 first = resolved(DYADIC_VIEWPORTS[0].width, DYADIC_VIEWPORTS[0].height);
        const float lengthRatio = first.axisHideClipLength / first.clipSpaceSize;
        const float areaRatio = first.planeHideClipArea / (first.clipSpaceSize * first.clipSpaceSize);
        CAPTURE(lengthRatio);
        CAPTURE(areaRatio);
        for (const DyadicViewport& entry : DYADIC_VIEWPORTS) {
            CAPTURE(entry.width);
            CAPTURE(entry.height);
            const Size4 size = resolved(entry.width, entry.height);
            CHECK(size.axisHideClipLength / size.clipSpaceSize == lengthRatio);
            CHECK(size.planeHideClipArea / (size.clipSpaceSize * size.clipSpaceSize) == areaRatio);
        }
    }

    SUBCASE("at the viewport where the size resolves to the library's own, so do both thresholds") {
        // THE ARM THAT CARRIES THE CASE: the reference values are the LIBRARY's constants, which do
        // not come from the resolver at all, so this cannot be satisfied by a resolver that is
        // self-consistently wrong. Sabotage row 12 returns the library constants UNSCALED -- the
        // proportionality arm above reddens while this one stays green, which is the pair's shape.
        constexpr std::int64_t MAX_REFERENCE_ULPS = 2;
        const Size4 size = resolved(1800.0F, 900.0F);
        // Measured bit-exact, so this is an equality rather than a tolerance.
        CHECK(size.clipSpaceSize == ed::GIZMO_LIBRARY_DEFAULT_CLIP_SIZE);
        const std::int64_t lengthDistance =
            ulpsBetween(size.axisHideClipLength, ed::GIZMO_LIBRARY_DEFAULT_AXIS_HIDE_CLIP_LENGTH);
        const std::int64_t areaDistance =
            ulpsBetween(size.planeHideClipArea, ed::GIZMO_LIBRARY_DEFAULT_PLANE_HIDE_CLIP_AREA);
        CAPTURE(lengthDistance);
        CAPTURE(areaDistance);
        CHECK(lengthDistance <= MAX_REFERENCE_ULPS);
        CHECK(areaDistance <= MAX_REFERENCE_ULPS);
    }
}

TEST_CASE("editor gizmo style: a degenerate viewport fails CLOSED to the library's own numbers (task E.1.5, GS9)") {
    // AC-14. Unreachable from the panel -- onDraw returns at step 1 for any such rect -- and asserted
    // anyway, because the four inputs fail in four DIFFERENT ways.
    const float quietNan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const Size4 fallback = asSize4(ed::gizmoLibraryDefaultScreenSize());

    SUBCASE("every degenerate extent returns exactly the fallback") {
        const std::array<std::array<float, 2>, 7> degenerate{{{quietNan, 600.0F},
                                                              {600.0F, quietNan},
                                                              {infinity, 600.0F},
                                                              {600.0F, -infinity},
                                                              {0.0F, 600.0F},
                                                              {600.0F, 0.0F},
                                                              {-1.0F, 600.0F}}};
        // NaN defeats every direct comparison; +inf PASSES `> 0` and is caught only by isfinite; 0
        // and -1 are caught only by `> 0`. Deleting the finiteness arm (row 13) reddens the two inf
        // rows here and the whole of GS11.
        for (const std::array<float, 2>& extent : degenerate) {
            CAPTURE(extent[0]);
            CAPTURE(extent[1]);
            CHECK(resolved(extent[0], extent[1]) == fallback);
        }
    }

    SUBCASE("the fallback really is the library's numbers, not a zeroed struct") {
        // ANTI-VACUITY: a fallback returning {} would pass "they are all equal" and fail this.
        CHECK(fallback.axisLengthPoints == 0.0F);
        CHECK(fallback.clipSpaceSize == ed::GIZMO_LIBRARY_DEFAULT_CLIP_SIZE);
        CHECK(fallback.axisHideClipLength == ed::GIZMO_LIBRARY_DEFAULT_AXIS_HIDE_CLIP_LENGTH);
        CHECK(fallback.planeHideClipArea == ed::GIZMO_LIBRARY_DEFAULT_PLANE_HIDE_CLIP_AREA);
    }
}

TEST_CASE("editor gizmo style: operator== discriminates on every field (task E.1.5, GS10)") {
    // E.1.4's SO11 pattern. Every other case in this file AND I124 rest on the defaulted operator==
    // actually comparing the array: a GizmoStyle whose colors member was excluded from the comparison
    // would make I124 and GS1 vacuous SIMULTANEOUSLY, and nothing else would show it.
    SUBCASE("a GizmoStyle differing by one byte of one colour is unequal") {
        const ed::GizmoStyle base = ed::defaultGizmoStyle();
        ed::GizmoStyle mutated = base;
        constexpr auto SLOT = static_cast<std::size_t>(ed::GizmoColor::PlaneY);
        mutated.colors[SLOT][2] = static_cast<std::uint8_t>(mutated.colors[SLOT][2] ^ 1U);
        const bool oneByteIsUnequal = !(mutated == base);
        CHECK(oneByteIsUnequal);
        const bool anUntouchedCopyIsEqual = (ed::GizmoStyle{base} == base);
        CHECK(anUntouchedCopyIsEqual);
    }

    SUBCASE("a GizmoStyle differing by one ulp of one float is unequal") {
        const ed::GizmoStyle base = ed::defaultGizmoStyle();
        ed::GizmoStyle mutated = base;
        mutated.centerDiscRadiusPoints = std::nextafter(base.centerDiscRadiusPoints, 1000.0F);
        CAPTURE(mutated.centerDiscRadiusPoints);
        const bool oneUlpIsUnequal = !(mutated == base);
        CHECK(oneUlpIsUnequal);
    }

    SUBCASE("a GizmoScreenSize differing by one ulp of one field is unequal") {
        const engine::editor::GizmoScreenSize base = ed::resolveGizmoScreenSize(engine::Vec2{1440.0F, 900.0F});
        engine::editor::GizmoScreenSize mutated = base;
        mutated.planeHideClipArea = std::nextafter(base.planeHideClipArea, 1000.0F);
        const bool oneUlpIsUnequal = !(mutated == base);
        CHECK(oneUlpIsUnequal);
        engine::editor::GizmoScreenSize lengthMutated = base;
        lengthMutated.axisLengthPoints = std::nextafter(base.axisLengthPoints, 1000.0F);
        const bool lengthIsUnequal = !(lengthMutated == base);
        CHECK(lengthIsUnequal);
        const bool anUntouchedCopyIsEqual = (engine::editor::GizmoScreenSize{base} == base);
        CHECK(anUntouchedCopyIsEqual);
    }
}

TEST_CASE("editor gizmo style: the resolver is total and bounded over the whole viewport range (task E.1.5, GS11)") {
    // 10 000 seeded viewports, IDENTICAL on all three lanes because nextViewportSide derives from
    // std::mt19937's raw output rather than from std::uniform_real_distribution, which is not portable.
    std::mt19937 rng(0xE15B1U);
    int nonFinite = 0;
    int nonPositive = 0;
    int overBound = 0;
    float worstSize = 0.0F;
    float worstWidth = 0.0F;
    float worstHeight = 0.0F;
    // WRITTEN AS AN EXPRESSION, NEVER AS A 0.3F LITERAL: measured, the worst case is a SQUARE
    // viewport, where the value is bit-EQUAL to float(2 * 0.15F) -- a literal would turn an exact
    // equality into a failure.
    const float bound = 2.0F * ed::GIZMO_AXIS_MAX_VIEWPORT_FRACTION;
    for (int sample = 0; sample < 10000; ++sample) {
        const float w = nextViewportSide(rng);
        const float h = nextViewportSide(rng);
        const Size4 size = resolved(w, h);
        const std::array<float, 4> fields{size.axisLengthPoints, size.clipSpaceSize, size.axisHideClipLength,
                                          size.planeHideClipArea};
        for (const float field : fields) {
            nonFinite += std::isfinite(field) ? 0 : 1;
            nonPositive += (field > 0.0F) ? 0 : 1;
        }
        overBound += (size.clipSpaceSize <= bound) ? 0 : 1;
        if (size.clipSpaceSize > worstSize) {
            worstSize = size.clipSpaceSize;
            worstWidth = w;
            worstHeight = h;
        }
    }
    CAPTURE(worstWidth);
    CAPTURE(worstHeight);
    CAPTURE(worstSize);
    CAPTURE(bound);
    CHECK(nonFinite == 0);
    CHECK(nonPositive == 0);
    CHECK(overBound == 0);
    // The bound is TIGHT, with equality at a square viewport -- so it is a real bound, not slack.
    CHECK(resolved(1.0F, 1.0F).clipSpaceSize == bound);
}

TEST_CASE(
    "editor gizmo style: the derivation is a COMPILE-TIME fact and every enumerator indexes inside the array "
    "(task E.1.5, GS12)") {
    // AX3's precedent: for a constexpr accessor a static_assert is the strongest form available and
    // costs nothing at run time.
    static_assert(ed::GIZMO_COLOR_COUNT == 15);
    static_assert(sizeof(ed::GizmoColor) == 1);
    static_assert(ed::defaultGizmoStyle().colors[static_cast<std::size_t>(ed::GizmoColor::AxisX)][3] == 255U);
    static_assert(ed::defaultGizmoStyle().colors[static_cast<std::size_t>(ed::GizmoColor::PlaneX)][3] ==
                  ed::GIZMO_PLANE_FILL_ALPHA);
    static_assert(ed::gizmoLibraryDefaultScreenSize().clipSpaceSize == ed::GIZMO_LIBRARY_DEFAULT_CLIP_SIZE);

    SUBCASE("every enumerator indexes inside colors, and they are not all one colour") {
        const ed::GizmoStyle style = ed::defaultGizmoStyle();
        CHECK(style.colors.size() == ed::GIZMO_COLOR_COUNT);
        int distinctFromFirst = 0;
        for (std::size_t i = 0; i < ed::GIZMO_COLOR_COUNT; ++i) {
            CAPTURE(i);
            const auto slot = static_cast<ed::GizmoColor>(i);
            CHECK(static_cast<std::size_t>(slot) < style.colors.size());
            distinctFromFirst += (colorOf(style, slot) == colorOf(style, ed::GizmoColor::AxisX)) ? 0 : 1;
        }
        // THE ANTI-VACUITY FOR THE WHOLE FILE: a defaultGizmoStyle() that filled every slot with one
        // colour would satisfy several relationship checks above.
        CAPTURE(distinctFromFirst);
        CHECK(distinctFromFirst > 0);
    }
}

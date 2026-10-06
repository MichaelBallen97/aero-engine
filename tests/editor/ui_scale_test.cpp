// tests/editor/ui_scale_test.cpp -- task E.6.1: the UI-scale model (US1-US6) and the viewport chrome's pure
// functions at a scale (US7-US9).
// TIER 0, EVERY CONFIGURATION: resolveUiScale is pure and total, so every case is a table over plain floats
// with no window, no device and no ImGui context. NO #if of any kind (the 3.6.3 rule). Exact float
// assertions throughout: the quantiser DIVIDES, so every expected value below is exact arithmetic, and a
// length times 2 is exact in float.
#include <aero/core/math.hpp>
#include <aero/editor/editor_camera.hpp>
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/gizmo_style.hpp>
#include <aero/editor/picking.hpp>  // projectToViewport, ProjectionMode
#include <aero/editor/selection_overlay.hpp>
#include <aero/editor/text_file.hpp>  // readTextFile (US9's source-text claim)
#include <aero/editor/view_axis_gizmo.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <ostream>  // MSVC: a CHECK over a std::string_view needs the complete std::ostream (the 0.4.1 trap)
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

// ---- US7-US9: the viewport chrome's pure functions at a scale (task E.6.1, step 7) ----------------------
namespace {

using engine::Vec2;
using engine::Vec3;

// The eight lengths of a GizmoStyle, by name, so a failure says which one.
struct GizmoLength {
    std::string_view name;
    float value;
};

[[nodiscard]] std::array<GizmoLength, 8> gizmoLengths(const ed::GizmoStyle& s) {
    return {{
        {"translationLineThicknessPoints", s.translationLineThicknessPoints},
        {"translationArrowSizePoints", s.translationArrowSizePoints},
        {"rotationLineThicknessPoints", s.rotationLineThicknessPoints},
        {"rotationScreenRingThicknessPoints", s.rotationScreenRingThicknessPoints},
        {"scaleLineThicknessPoints", s.scaleLineThicknessPoints},
        {"scaleDiscRadiusPoints", s.scaleDiscRadiusPoints},
        {"hatchedAxisThicknessPoints", s.hatchedAxisThicknessPoints},
        {"centerDiscRadiusPoints", s.centerDiscRadiusPoints},
    }};
}

[[nodiscard]] ed::EditorCamera cameraAtRest() {
    ed::EditorCamera camera;
    camera.setYaw(0.0F);
    camera.setPitch(0.0F);
    return camera;
}

[[nodiscard]] float distanceBetween(Vec2 a, Vec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt((dx * dx) + (dy * dy));
}

// The comment-stripped text of a file -- a citation in prose must never satisfy or break a claim about code.
[[nodiscard]] std::string codeOf(const std::string& path) {
    const ed::FileReadResult read = ed::readTextFile(path);
    REQUIRE(read.text.has_value());
    std::string code;
    std::string_view text = *read.text;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        const std::string_view line = newline == std::string_view::npos ? text : text.substr(0, newline);
        const std::size_t comment = line.find("//");
        code.append(comment == std::string_view::npos ? line : line.substr(0, comment));
        code.push_back('\n');
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1U);
    }
    return code;
}

[[nodiscard]] bool isIdentifierChar(char c) {
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_';
}

}  // namespace

TEST_CASE("ui scale: the gizmo's lengths follow the UI (task E.6.1, US7)") {
    const ed::GizmoStyle base = ed::defaultGizmoStyle();
    CHECK((ed::scaledGizmoStyle(base, 1.0F) == base));  // bit for bit at 1 -- GizmoStyle's defaulted ==

    const ed::GizmoStyle doubled = ed::scaledGizmoStyle(base, 2.0F);
    const std::array<GizmoLength, 8> one = gizmoLengths(base);
    const std::array<GizmoLength, 8> two = gizmoLengths(doubled);
    for (std::size_t i = 0; i < one.size(); ++i) {
        CAPTURE(one[i].name);
        CHECK(two[i].value == 2.0F * one[i].value);
    }
    for (std::size_t i = 0; i < base.colors.size(); ++i) {
        CAPTURE(i);
        CHECK((doubled.colors[i] == base.colors[i]));
    }

    // ABOVE the knee (0.15 x the smaller side >= 180) the length is pixel-constant, so it follows the scale.
    for (const Vec2 viewport : {Vec2{1800.0F, 1200.0F}, Vec2{2400.0F, 1600.0F}, Vec2{3000.0F, 2000.0F}}) {
        CAPTURE(viewport.x);
        const float atOne = ed::resolveGizmoScreenSize(viewport, 1.0F).axisLengthPoints;
        CHECK(ed::resolveGizmoScreenSize(viewport, 2.0F).axisLengthPoints == 2.0F * atOne);
    }
    // BELOW it the length follows the dock and must NOT double -- the knee's own anti-vacuity.
    const float smallAtOne = ed::resolveGizmoScreenSize(Vec2{400.0F, 300.0F}, 1.0F).axisLengthPoints;
    CHECK(smallAtOne == 45.0F);
    CHECK(ed::resolveGizmoScreenSize(Vec2{400.0F, 300.0F}, 2.0F).axisLengthPoints == smallAtOne);
}

TEST_CASE("ui scale: the view-axis widget follows the UI, hit test included (task E.6.1, US8)") {
    const ed::EditorCamera camera = cameraAtRest();
    const Vec2 origin{0.0F, 0.0F};
    const Vec2 size{800.0F, 600.0F};
    const ed::ViewAxisLayout one = ed::viewAxisLayout(camera, origin, size, 1.0F);
    const ed::ViewAxisLayout two = ed::viewAxisLayout(camera, origin, size, 2.0F);
    REQUIRE(one.visible);
    REQUIRE(two.visible);
    CHECK(one.uiScale == 1.0F);
    CHECK(two.uiScale == 2.0F);

    // The centre, from the image's top-right corner: margin + half-extent, doubled.
    const Vec2 corner{origin.x + size.x, origin.y};
    CHECK(one.centerPoints.x - corner.x == -48.0F);
    CHECK(one.centerPoints.y - corner.y == 48.0F);
    CHECK(two.centerPoints.x - corner.x == -96.0F);
    CHECK(two.centerPoints.y - corner.y == 96.0F);
    for (std::size_t i = 0; i < ed::VIEW_AXIS_COUNT; ++i) {
        CAPTURE(i);
        CHECK(two.balls[i].offsetPoints.x == 2.0F * one.balls[i].offsetPoints.x);
        CHECK(two.balls[i].offsetPoints.y == 2.0F * one.balls[i].offsetPoints.y);
    }

    // Visibility: the minimum image doubles too (140 -> 280).
    CHECK_FALSE(ed::viewAxisLayout(camera, origin, Vec2{279.0F, 1000.0F}, 2.0F).visible);
    CHECK(ed::viewAxisLayout(camera, origin, Vec2{280.0F, 280.0F}, 2.0F).visible);

    // THE HIT TEST reads the scale the layout CARRIES (decision D-6). Distances are multiples of the
    // UNSCALED ball radius, 8: 15.2 is inside the scaled 16 and outside the unscaled 8 (the plan's C19).
    const ed::ViewAxisBall& posX = two.balls[static_cast<std::size_t>(ed::ViewAxis::PosX)];
    const float length =
        std::sqrt((posX.offsetPoints.x * posX.offsetPoints.x) + (posX.offsetPoints.y * posX.offsetPoints.y));
    REQUIRE(length > 0.0F);  // ANTI-VACUITY: the +X ball is not collapsed onto the centre at this pose
    const Vec2 u{posX.offsetPoints.x / length, posX.offsetPoints.y / length};
    const Vec2 b2 = two.centerPoints + posX.offsetPoints;
    const Vec2 inside{b2.x + (u.x * 15.2F), b2.y + (u.y * 15.2F)};
    const Vec2 outside{b2.x + (u.x * 16.8F), b2.y + (u.y * 16.8F)};
    const ed::ViewAxisPick hit = ed::viewAxisPickAt(two, inside);
    CHECK((hit.kind == ed::ViewAxisHit::Axis));
    CHECK((hit.axis == ed::ViewAxis::PosX));
    CHECK((ed::viewAxisPickAt(two, outside).kind == ed::ViewAxisHit::None));
    // At scale 1 the SAME distance from the scale-1 layout's OWN +X ball misses -- 15.2 is outside the
    // unscaled 8 -- so only a hit test that reads the layout's scale answers None here (seed S21). A point
    // taken from the scale-2 ball would miss at scale 1 whatever radius the hit test used: that ball is not
    // where the scale-1 layout drew it, which is how this arm used to pass under exactly that seed.
    const Vec2 o1 = one.balls[static_cast<std::size_t>(ed::ViewAxis::PosX)].offsetPoints;
    const float length1 = std::sqrt((o1.x * o1.x) + (o1.y * o1.y));
    REQUIRE(length1 > 0.0F);
    const Vec2 u1{o1.x / length1, o1.y / length1};
    const Vec2 b1 = one.centerPoints + o1;
    const Vec2 within1{b1.x + (u1.x * 7.6F), b1.y + (u1.y * 7.6F)};
    const Vec2 beyond1{b1.x + (u1.x * 15.2F), b1.y + (u1.y * 15.2F)};
    // ANTI-VACUITY: the probe sits on the scale-1 ball -- inside its unscaled radius it is a hit.
    const ed::ViewAxisPick onBall = ed::viewAxisPickAt(one, within1);
    CHECK((onBall.kind == ed::ViewAxisHit::Axis));
    CHECK((onBall.axis == ed::ViewAxis::PosX));
    CHECK((ed::viewAxisPickAt(one, beyond1).kind == ed::ViewAxisHit::None));
}

TEST_CASE(
    "ui scale: every scaled pure function scales by exactly the factor, and none defaults it "
    "(task E.6.1, US9)") {
    // viewAxisRect: the box and its inset from the image's top-right corner both double.
    const Vec2 origin{0.0F, 0.0F};
    const Vec2 size{800.0F, 600.0F};
    Vec2 min1{};
    Vec2 max1{};
    Vec2 min2{};
    Vec2 max2{};
    ed::viewAxisRect(origin, size, 1.0F, min1, max1);
    ed::viewAxisRect(origin, size, 2.0F, min2, max2);
    CHECK(max2.x - min2.x == 2.0F * (max1.x - min1.x));
    CHECK(max2.y - min2.y == 2.0F * (max1.y - min1.y));
    CHECK((origin.x + size.x) - max2.x == 2.0F * ((origin.x + size.x) - max1.x));
    CHECK(min2.y - origin.y == 2.0F * (min1.y - origin.y));
    CHECK(max1.x - min1.x == 76.0F);  // ANTI-VACUITY: a real box at 1

    // buildSelectionOverlay: one point-marker entity; every diamond vertex sits twice as far from the
    // projected centre at 2 as at 1 (6 -> 12).
    engine::World world;
    const engine::Entity light = world.create();
    world.add<engine::Transform>(light, engine::Transform{});
    ed::EditorCamera camera;
    camera.setPivot(Vec3::zero());
    camera.setYaw(0.0F);
    camera.setPitch(0.0F);
    camera.setDistance(10.0F);
    const engine::Mat4 viewProj = camera.projectionMatrix(1.0F) * camera.viewMatrix();
    constexpr auto PERSP = ed::ProjectionMode::Perspective;
    const Vec2 viewport{800.0F, 600.0F};
    Vec2 center{};
    REQUIRE(ed::projectToViewport(viewProj, PERSP, Vec3::zero(), viewport, center));
    const std::array<engine::Entity, 1> marked{light};
    std::vector<ed::OverlaySegment> atOne;
    std::vector<ed::OverlaySegment> atTwo;
    ed::buildSelectionOverlay(world, marked, light, viewProj, PERSP, viewport, 1.0F, atOne);
    ed::buildSelectionOverlay(world, marked, light, viewProj, PERSP, viewport, 2.0F, atTwo);
    REQUIRE(atOne.size() == 4U);
    REQUIRE(atTwo.size() == 4U);
    for (std::size_t i = 0; i < 4U; ++i) {
        CAPTURE(i);
        CHECK(distanceBetween(atOne[i].a, center) == doctest::Approx(6.0F).epsilon(1e-6));
        CHECK(distanceBetween(atTwo[i].a, center) == doctest::Approx(12.0F).epsilon(1e-6));
        CHECK(distanceBetween(atTwo[i].b, center) == doctest::Approx(12.0F).epsilon(1e-6));
    }

    // THE SET CLAIM, over every public editor header, comment-stripped. `float uiScale` is classified by the
    // next non-space character: `(` is a function NAMED uiScale (ignored), `)` or `,` a PARAMETER, `=` a
    // MEMBER with a default. A parameter is never defaulted, so `=` may follow only the one member.
    const std::filesystem::path headers = std::filesystem::path(AERO_EDITOR_INCLUDE_DIR) / "aero" / "editor";
    std::map<std::string, std::string> found;  // header -> its classifications, in order: "P" or "M"
    std::size_t read = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(headers, ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".hpp") {
            continue;
        }
        ++read;
        const std::string code = codeOf(entry.path().string());
        constexpr std::string_view NEEDLE = "float uiScale";
        for (std::size_t at = code.find(NEEDLE); at != std::string::npos; at = code.find(NEEDLE, at + 1U)) {
            const bool startsAToken = at == 0U || !isIdentifierChar(code[at - 1U]);
            const std::size_t after = at + NEEDLE.size();
            const bool endsTheName = after < code.size() && !isIdentifierChar(code[after]);
            if (!startsAToken || !endsTheName) {
                continue;
            }
            const std::size_t next = code.find_first_not_of(" \t\r\n", after);
            REQUIRE(next != std::string::npos);
            const char c = code[next];
            if (c == '(') {
                continue;  // a function named uiScale, e.g. ImGuiLayer::uiScale()
            }
            const std::string file = entry.path().filename().string();
            CAPTURE(file);
            CHECK((c == ')' || c == ',' || c == '='));
            found[file].push_back(c == '=' ? 'M' : 'P');
        }
    }
    REQUIRE_FALSE(ec);
    CHECK(read > 60U);  // ANTI-VACUITY: the walk really read the public headers
    const std::map<std::string, std::string> expected{
        {"gizmo_style.hpp", "PP"},
        {"selection_overlay.hpp", "P"},
        {"view_axis_gizmo.hpp", "MPP"},
    };
    CHECK(found.size() == expected.size());
    for (const auto& [file, kinds] : expected) {
        CAPTURE(file);
        const auto it = found.find(file);
        REQUIRE(it != found.end());
        std::string sorted = it->second;
        std::sort(sorted.begin(), sorted.end());
        CHECK(sorted == kinds);
    }
}

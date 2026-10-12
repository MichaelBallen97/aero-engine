// Aero Engine -- the Viewport's HUD as values (task E.6.3, spec D9). PURE: see viewport_hud.hpp.
#include <aero/editor/editor_glyphs.hpp>  // AERO_GLYPH_DEGREE, _EM_DASH, _MULTIPLY, _MIDDLE_DOT
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/view_axis_gizmo.hpp>  // viewAxisRect, VIEW_AXIS_HALF_EXTENT_POINTS
#include <aero/editor/viewport_hud.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <numbers>

namespace engine::editor {

namespace {

[[nodiscard]] float layoutScale(float uiScale) noexcept {
    return (std::isfinite(uiScale) && uiScale > 0.0F) ? uiScale : 1.0F;
}
[[nodiscard]] bool finitePoint(Vec2 p) noexcept { return std::isfinite(p.x) && std::isfinite(p.y); }
// A measured size that is not a finite, positive number counts as 0 -- an empty rect, never shown.
[[nodiscard]] Vec2 usableSize(Vec2 size) noexcept {
    const auto usable = [](float v) { return (std::isfinite(v) && v > 0.0F) ? v : 0.0F; };
    return Vec2{usable(size.x), usable(size.y)};
}
[[nodiscard]] bool nonEmpty(const HudRect& r) noexcept { return r.max.x > r.min.x && r.max.y > r.min.y; }
[[nodiscard]] bool overlaps(const HudRect& a, Vec2 bMin, Vec2 bMax) noexcept {
    const bool bNonEmpty = bMax.x > bMin.x && bMax.y > bMin.y;  // a degenerate rect overlaps nothing
    return bNonEmpty && a.min.x < bMax.x && bMin.x < a.max.x && a.min.y < bMax.y && bMin.y < a.max.y;
}
[[nodiscard]] bool overlapsShown(const HudRect& a, const HudRect& b) noexcept {
    return b.shown && overlaps(a, b.min, b.max);
}
[[nodiscard]] bool inside(const HudRect& r, Vec2 areaMin, Vec2 areaMax) noexcept {
    return r.min.x >= areaMin.x && r.min.y >= areaMin.y && r.max.x <= areaMax.x && r.max.y <= areaMax.y;
}
[[nodiscard]] bool owns(const HudRect& r, Vec2 p) noexcept {
    return r.shown && p.x >= r.min.x && p.x < r.max.x && p.y >= r.min.y && p.y < r.max.y;  // NaN: every test false
}

}  // namespace

ViewportHudLayout viewportHudLayout(Vec2 imageMin, Vec2 imageMax, const ViewportHudInputs& inputs,
                                    float uiScale) noexcept {
    ViewportHudLayout out;
    const bool usable =
        finitePoint(imageMin) && finitePoint(imageMax) && imageMax.x > imageMin.x && imageMax.y > imageMin.y;
    if (!usable) {
        return out;  // nothing shown, every rect at the origin
    }
    const float s = layoutScale(uiScale);
    const float inset = EDITOR_THEME.panels.hudInset * s;
    const float gap = EDITOR_THEME.panels.hudGap * s;
    const Vec2 areaMin{imageMin.x + inset, imageMin.y + inset};
    const Vec2 areaMax{imageMax.x - inset, imageMax.y - inset};
    Vec2 axisMin{};
    Vec2 axisMax{};
    if (inputs.viewAxisVisible) {  // the CIRCLE's box: the circle reaches five past viewAxisRect and draws last
        viewAxisCircleRect(imageMin, Vec2{imageMax.x - imageMin.x, imageMax.y - imageMin.y}, s, axisMin, axisMax);
    }
    const auto keep = [&](const HudRect& r) {
        return nonEmpty(r) && inside(r, areaMin, areaMax) && !overlaps(r, axisMin, axisMax);
    };

    // 1. The view chip, top-left: the only control, kept longest.
    const Vec2 chip = usableSize(inputs.chipSize);
    out.chip = HudRect{areaMin, Vec2{areaMin.x + chip.x, areaMin.y + chip.y}, false};
    out.chip.shown = keep(out.chip);

    // 2. Bottom-left: the fly chip while flying, else the two hints, hudGap apart.
    if (inputs.flying) {
        const Vec2 fly = usableSize(inputs.flyChipSize);
        out.flyChip = HudRect{Vec2{areaMin.x, areaMax.y - fly.y}, Vec2{areaMin.x + fly.x, areaMax.y}, false};
        out.flyChip.shown = keep(out.flyChip) && !overlapsShown(out.flyChip, out.chip);
    }
    float x = areaMin.x;
    for (std::size_t i = 0; i < out.hints.size(); ++i) {
        const Vec2 size = usableSize(inputs.hintSizes[i]);
        out.hints[i] = HudRect{Vec2{x, areaMax.y - size.y}, Vec2{x + size.x, areaMax.y}, false};
        out.hints[i].shown = !inputs.flying && keep(out.hints[i]) && !overlapsShown(out.hints[i], out.chip);
        x = out.hints[i].max.x + gap;
    }

    // 3. The stats card, top-right, sized by the LAYOUT: dropped first.
    const auto rows = static_cast<std::uint32_t>(std::min<std::size_t>(inputs.statsRows, 64U));
    const float width = std::max(EDITOR_THEME.panels.statsCardWidth * s, usableSize(inputs.statsContentSize).x);
    const float height = statsCardHeightDp(EDITOR_THEME, rows) * s;
    out.stats = HudRect{Vec2{areaMax.x - width, areaMin.y}, Vec2{areaMax.x, areaMin.y + height}, false};
    out.stats.shown = inputs.statsVisible && keep(out.stats) && !overlapsShown(out.stats, out.chip) &&
                      !overlapsShown(out.stats, out.flyChip) && !overlapsShown(out.stats, out.hints[0]) &&
                      !overlapsShown(out.stats, out.hints[1]);
    return out;
}

bool hudOwnsPoint(const ViewportHudLayout& layout, Vec2 point) noexcept {
    return owns(layout.chip, point) || owns(layout.stats, point) || owns(layout.hints[0], point) ||
           owns(layout.hints[1], point) || owns(layout.flyChip, point);
}

float viewAxisCircleRadius(float uiScale) noexcept {
    return (VIEW_AXIS_HALF_EXTENT_POINTS + EDITOR_THEME.panels.viewAxisCircleGap) * layoutScale(uiScale);
}

void viewAxisCircleRect(Vec2 imageOriginPoints, Vec2 imageSizePoints, float uiScale, Vec2& outMin,
                        Vec2& outMax) noexcept {
    viewAxisRect(imageOriginPoints, imageSizePoints, uiScale, outMin, outMax);
    if (!(outMax.x > outMin.x)) {
        return;  // hidden: viewAxisRect's degenerate (0,0)-(0,0), kept -- a hidden widget's circle owns nothing
    }
    const float grow = EDITOR_THEME.panels.viewAxisCircleGap * uiScale;  // visible, so the scale is finite and > 0
    outMin = Vec2{outMin.x - grow, outMin.y - grow};
    outMax = Vec2{outMax.x + grow, outMax.y + grow};
}

std::size_t viewportDrawCalls(const ViewportDrawCounts& counts) noexcept {
    constexpr std::size_t MAX = std::numeric_limits<std::size_t>::max();
    std::size_t total = 0;
    for (const std::size_t part :
         {counts.forward, counts.shadow, counts.selectionMask, static_cast<std::size_t>(counts.debug), counts.sky,
          counts.resolve, counts.composite}) {
        total = part > MAX - total ? MAX : total + part;  // SATURATING: a poisoned counter never wraps to small
    }
    return total;
}

void abbreviateCount(std::uint64_t value, std::string& out) {
    constexpr std::uint64_t THOUSAND = 1000U;
    constexpr std::uint64_t MILLION = 1000000U;
    constexpr std::uint64_t BILLION = 1000000000U;
    if (value < 10000U) {
        out = std::to_string(value);  // plain digits: this range never goes through groupThousands
        return;
    }
    if (value < MILLION) {
        out = std::to_string(value / THOUSAND) + " k";  // FLOOR: 999 999 is "999 k", never "1000 k"
        return;
    }
    const bool giga = value >= BILLION;
    const std::uint64_t unit = giga ? BILLION : MILLION;
    const std::string_view suffix = giga ? " G" : " M";
    if (value / unit < 10U) {
        const std::uint64_t tenths = value / (unit / 10U);  // FLOOR to one decimal
        out = std::to_string(tenths / 10U) + "." + std::to_string(tenths % 10U);
    } else {
        out = std::to_string(value / unit);
    }
    out += suffix;
}

void viewChipLabel(ProjectionMode mode, float fovYRadians, std::string& out) {
    if (mode == ProjectionMode::Orthographic) {
        out = "Orthographic";
        return;
    }
    const float degrees = fovYRadians * (180.0F / std::numbers::pi_v<float>);
    if (!std::isfinite(degrees) || !(std::abs(degrees) < 100000.0F)) {
        out = "Perspective";  // no number to state -- and lround of a huge value is unspecified
        return;
    }
    out = "Perspective " + std::to_string(std::lround(degrees));
    out += AERO_GLYPH_DEGREE;
}

void flySpeedText(float unitsPerSecond, std::string& out) {
    if (!std::isfinite(unitsPerSecond)) {
        out = "fly -- u/s";
        return;
    }
    std::array<char, 32> number{};
    const int written = std::snprintf(number.data(), number.size(), "%.1f", static_cast<double>(unitsPerSecond));
    out = "fly ";
    out += written > 0 ? std::string(number.data()) : std::string("--");
    out += " u/s";
}

void viewportStatsRows(const FrameTimeReadout& frame, std::size_t drawCalls, std::size_t triangles,
                       std::size_t entities, std::uint32_t extentW, std::uint32_t extentH,
                       std::vector<ViewportStatsRow>& out) {
    out.clear();
    // The Frame row is the ONE readout's ms half -- the status bar's own text, split at its dot, so the two
    // consumers can never disagree (D9: no second readout).
    const std::string readout = frameReadoutText(frame);
    constexpr std::string_view DOT = AERO_GLYPH_MIDDLE_DOT " ";
    const std::size_t at = readout.find(DOT);
    std::string ms = at == std::string::npos ? readout : readout.substr(at + DOT.size());
    out.push_back(ViewportStatsRow{"Frame", std::move(ms)});
    std::string value;
    abbreviateCount(drawCalls, value);
    out.push_back(ViewportStatsRow{"Draw calls", value});
    abbreviateCount(triangles, value);
    out.push_back(ViewportStatsRow{"Triangles", value});
    abbreviateCount(entities, value);
    out.push_back(ViewportStatsRow{"Entities", value});
    out.push_back(ViewportStatsRow{"GPU", std::string(AERO_GLYPH_EM_DASH)});
    std::string extent = std::to_string(extentW);
    extent += " " AERO_GLYPH_MULTIPLY " ";
    extent += std::to_string(extentH);
    out.push_back(ViewportStatsRow{"Extent", std::move(extent)});
}

void beginViewportHudRecordFrame(ViewportHudRecord& record) noexcept {
    ++record.framesRecorded;
    record.layout = ViewportHudLayout{};
    record.chipLabel.clear();
    record.chipFontName.clear();
    record.chipFontSize = 0.0F;
    record.statsRows.clear();
    record.hintTexts.clear();
    record.viewAxisCornerMin = Vec2{};
    record.viewAxisCornerMax = Vec2{};
    record.viewAxisCircleCenter = Vec2{};
    record.viewAxisCircleRadius = 0.0F;  // drawViewAxisGizmo re-fills these only on a frame it draws
}

}  // namespace engine::editor

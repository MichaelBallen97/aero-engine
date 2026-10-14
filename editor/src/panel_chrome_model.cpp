// Aero Engine -- the panels' shared chrome as values (task E.6.3). PURE: see panel_chrome_model.hpp.
#include <aero/editor/editor_icons.hpp>
#include <aero/editor/panel_chrome_model.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

namespace engine::editor {

namespace {

// US10's rule, restated per TU (toolbar_model.cpp's shape): a non-finite or non-positive scale lays out at 1.
[[nodiscard]] float layoutScale(float uiScale) noexcept {
    return (std::isfinite(uiScale) && uiScale > 0.0F) ? uiScale : 1.0F;
}

// The clip's own arithmetic is double; its inputs and its answer are float.
struct ClipPoint {
    double x = 0.0;
    double y = 0.0;
};

[[nodiscard]] bool allFinite(std::span<const Vec2> polygon) noexcept {
    return std::all_of(polygon.begin(), polygon.end(), [](Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); });
}

[[nodiscard]] std::vector<ClipPoint> toClipPoints(std::span<const Vec2> polygon) {
    std::vector<ClipPoint> points;
    points.reserve(polygon.size());
    for (const Vec2 p : polygon) {
        points.push_back(ClipPoint{p.x, p.y});
    }
    return points;
}

// Twice the signed area, the shoelace sum: POSITIVE for a polygon wound clockwise on a y-down screen.
template <typename Point>
[[nodiscard]] double twiceSignedArea(const std::vector<Point>& polygon) noexcept {
    double sum = 0.0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point a = polygon[i];
        const Point b = polygon[(i + 1U) % polygon.size()];
        sum += (static_cast<double>(a.x) * b.y) - (static_cast<double>(b.x) * a.y);
    }
    return sum;
}

[[nodiscard]] bool withinMergeEpsilon(Vec2 a, Vec2 b) noexcept {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return (dx * dx) + (dy * dy) <= CLIP_MERGE_EPSILON * CLIP_MERGE_EPSILON;
}

}  // namespace

// D2: each palette role read ONCE, here -- I287 pins the 26 rows. NO default: a 27th role is a red lint lane.
Srgb8 chromeRoleColor(ChromeRole role) noexcept {
    switch (role) {
        case ChromeRole::Canvas:
            return EDITOR_THEME.palette.canvas;
        case ChromeRole::Inset:
            return EDITOR_THEME.palette.inset;
        case ChromeRole::Chrome:
            return EDITOR_THEME.palette.chrome;
        case ChromeRole::Panel:
            return EDITOR_THEME.palette.panel;
        case ChromeRole::Tile:
            return EDITOR_THEME.palette.tile;
        case ChromeRole::Raised:
            return EDITOR_THEME.palette.raised;
        case ChromeRole::RaisedHeader:
            return EDITOR_THEME.palette.raisedHeader;
        case ChromeRole::Hover:
            return EDITOR_THEME.palette.hover;
        case ChromeRole::Active:
            return EDITOR_THEME.palette.active;
        case ChromeRole::Selection:
            return EDITOR_THEME.palette.selection;
        case ChromeRole::Border:
            return EDITOR_THEME.palette.border;
        case ChromeRole::Divider:
            return EDITOR_THEME.palette.divider;
        case ChromeRole::AccentBorder:
            return EDITOR_THEME.palette.accentBorder;
        case ChromeRole::TextBright:
            return EDITOR_THEME.palette.textBright;
        case ChromeRole::Text:
            return EDITOR_THEME.palette.text;
        case ChromeRole::TextSecondary:
            return EDITOR_THEME.palette.textSecondary;
        case ChromeRole::TextLabel:
            return EDITOR_THEME.palette.textLabel;
        case ChromeRole::TextMuted:
            return EDITOR_THEME.palette.textMuted;
        case ChromeRole::TextFaint:
            return EDITOR_THEME.palette.textFaint;
        case ChromeRole::OnAccent:
            return EDITOR_THEME.palette.onAccent;
        case ChromeRole::Accent:
            return EDITOR_THEME.palette.accent;
        case ChromeRole::Warning:
            return EDITOR_THEME.palette.warning;
        case ChromeRole::Error:
            return EDITOR_THEME.palette.error;
        case ChromeRole::Success:
            return EDITOR_THEME.palette.success;
        case ChromeRole::Info:
            return EDITOR_THEME.palette.info;
        case ChromeRole::Critical:
            return EDITOR_THEME.palette.critical;
    }
    return Srgb8{};  // unreachable: every enumerator returns above (the log.cpp:30 shape) -- and reads no role
}

Srgb8 chromeRoleColor(ChromeRole role, std::uint8_t alpha) noexcept {
    Srgb8 color = chromeRoleColor(role);
    color.a = alpha;
    return color;
}

CardGeometry cardGeometry(float uiScale) noexcept {
    const float s = layoutScale(uiScale);
    const ThemePanels& p = EDITOR_THEME.panels;
    const ThemeMetrics& m = EDITOR_THEME.metrics;
    return CardGeometry{.headerHeight = cardHeaderHeightDp(EDITOR_THEME) * s,
                        .headerPaddingX = p.cardHeaderPaddingX * s,
                        .headerPaddingY = p.cardHeaderPaddingY * s,
                        .paddingX = p.cardPaddingX * s,
                        .paddingY = p.cardPaddingY * s,
                        .gap = p.cardGap * s,
                        .rounding = m.childRounding * s,
                        .chevronWidth = p.cardChevronWidth * s,
                        .iconGap = p.cardIconGap * s,
                        .overflowWidth = (EDITOR_THEME.type.bodySize + (2.0F * m.framePadding.y)) * s};
}

CardHeaderLayout cardHeaderLayout(float spanMinX, float spanMaxX, float iconAdvance, float overflowWidth,
                                  float uiScale) noexcept {
    const float s = layoutScale(uiScale);
    const ThemePanels& p = EDITOR_THEME.panels;
    CardHeaderLayout out{};
    out.chevronX = spanMinX + (p.cardHeaderPaddingX * s);
    out.iconX = out.chevronX + ((p.cardChevronWidth + p.cardIconGap) * s);
    out.titleX = out.iconX + iconAdvance + (p.cardIconGap * s);
    out.overflowX = spanMaxX - (p.cardHeaderPaddingX * s) - overflowWidth;
    return out;
}

float chipHeight(float uiScale) noexcept { return chipHeightDp(EDITOR_THEME) * layoutScale(uiScale); }

float hairline(float thicknessDp, float uiScale, float framebufferScale) noexcept {
    const float thickness = (std::isfinite(thicknessDp) && thicknessDp > 0.0F) ? thicknessDp : 1.0F;
    const float framebuffer = std::isfinite(framebufferScale) ? framebufferScale : 1.0F;
    const float pixel = 1.0F / std::max(framebuffer, 1.0F);
    return std::max(thickness * layoutScale(uiScale), pixel);
}

float faceSizePoints(float sizeDp, float uiScale) noexcept {
    // IM_ROUND is (float)(int)(x + 0.5f): floor(x + 0.5) for the positive sizes a face has.
    const float rounded = std::floor((sizeDp * layoutScale(uiScale)) + 0.5F);
    return (std::isfinite(rounded) && rounded >= 1.0F) ? rounded : 1.0F;
}

float chevronFaceSize(float bodyPoints, float inkAtBody, float uiScale) noexcept {
    const float body = (std::isfinite(bodyPoints) && bodyPoints > 0.0F) ? bodyPoints : EDITOR_THEME.type.bodySize;
    if (!std::isfinite(inkAtBody) || !(inkAtBody > 0.0F)) {
        return body;
    }
    const float size = body * (EDITOR_THEME.panels.cardChevronWidth * layoutScale(uiScale)) / inkAtBody;
    return (std::isfinite(size) && size > 0.0F) ? size : body;
}

float cardChevronPenX(float chevronX, float columnWidth, float inkLeft, float inkWidth) noexcept {
    const auto finiteOrZero = [](float v) noexcept { return std::isfinite(v) ? v : 0.0F; };
    const float centring = (finiteOrZero(columnWidth) - finiteOrZero(inkWidth)) * 0.5F;
    const float pen = std::round(finiteOrZero(chevronX) + centring - finiteOrZero(inkLeft));
    return std::isfinite(pen) ? pen : 0.0F;
}

std::vector<Vec2> clipConvexPolygon(std::span<const Vec2> subject, std::span<const Vec2> clipper) {
    if (!allFinite(subject) || !allFinite(clipper) || subject.size() < 3U || clipper.size() < 3U) {
        return {};
    }
    const std::vector<ClipPoint> clip = toClipPoints(clipper);
    // The clipper's own sign: +1 clockwise on a y-down screen (ImGui's winding), -1 counter-clockwise, and no area
    // at all is refused -- with no inside, Sutherland-Hodgman has nothing to keep.
    const double area = twiceSignedArea(clip);
    if (!(std::abs(area) > 0.0)) {
        return {};
    }
    const double orientation = area > 0.0 ? 1.0 : -1.0;
    const std::size_t n = clip.size();
    // CONVEX, or refused: at a reflex corner a half-plane walk keeps what the clipper does not hold. A turn may
    // lean the wrong way by rounding alone -- 1e-6 of its two edges' lengths -- where the corner is straight.
    for (std::size_t i = 0; i < n; ++i) {
        const ClipPoint previous = clip[(i + n - 1U) % n];
        const ClipPoint at = clip[i];
        const ClipPoint next = clip[(i + 1U) % n];
        const double inX = at.x - previous.x;
        const double inY = at.y - previous.y;
        const double outX = next.x - at.x;
        const double outY = next.y - at.y;
        const double turn = orientation * ((inX * outY) - (inY * outX));
        if (turn < -1.0e-6 * std::hypot(inX, inY) * std::hypot(outX, outY)) {
            return {};
        }
    }
    // Sutherland-Hodgman: the subject cut by each clipper edge's inner half-plane in turn, its order kept.
    std::vector<ClipPoint> kept = toClipPoints(subject);
    std::vector<ClipPoint> cut;
    for (std::size_t i = 0; i < n && !kept.empty(); ++i) {
        const ClipPoint from = clip[i];
        const ClipPoint to = clip[(i + 1U) % n];
        const double edgeX = to.x - from.x;
        const double edgeY = to.y - from.y;
        // >= 0 inside the edge (on it counts as inside), < 0 outside.
        const auto side = [&](ClipPoint p) noexcept {
            return orientation * ((edgeX * (p.y - from.y)) - (edgeY * (p.x - from.x)));
        };
        cut.clear();
        for (std::size_t j = 0; j < kept.size(); ++j) {
            const ClipPoint p = kept[j];
            const ClipPoint q = kept[(j + 1U) % kept.size()];
            const double sideP = side(p);
            const double sideQ = side(q);
            if (sideP >= 0.0) {
                cut.push_back(p);
            }
            if ((sideP >= 0.0) != (sideQ >= 0.0)) {
                const double t = sideP / (sideP - sideQ);  // the signs differ, so the divisor is never 0
                cut.push_back(ClipPoint{p.x + (t * (q.x - p.x)), p.y + (t * (q.y - p.y))});
            }
        }
        kept.swap(cut);
    }
    // In float, with a near-repeat merged into the vertex before it (and the last into the first).
    std::vector<Vec2> out;
    out.reserve(kept.size());
    for (const ClipPoint p : kept) {
        const Vec2 v{static_cast<float>(p.x), static_cast<float>(p.y)};
        if (out.empty() || !withinMergeEpsilon(out.back(), v)) {
            out.push_back(v);
        }
    }
    while (out.size() > 1U && withinMergeEpsilon(out.front(), out.back())) {
        out.pop_back();
    }
    const double minArea = static_cast<double>(CLIP_MERGE_EPSILON) * CLIP_MERGE_EPSILON;
    if (out.size() < 3U || !(std::abs(twiceSignedArea(out)) * 0.5 > minArea)) {
        return {};
    }
    return out;
}

bool hatchPieceTakesFringe(std::span<const Vec2> piece) noexcept {
    if (piece.size() < 3U || !allFinite(piece)) {
        return false;
    }
    // A stripe's own corner is EXACTLY 45 degrees, so the 1e-3 only absorbs rounding. The next sharper corner a
    // band can meet is 41.25 degrees: PathArcToFast's segments run at multiples of 3.75 degrees, half the step of
    // its 48-sample table (IM_DRAWLIST_ARCFAST_TABLE_SIZE, imgui_internal.h:869; imgui_draw.cpp:406-410).
    const double sharpestCosine = (std::numbers::sqrt2 * 0.5) + 1.0e-3;
    const std::size_t n = piece.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2 previous = piece[(i + n - 1U) % n];
        const Vec2 at = piece[i];
        const Vec2 next = piece[(i + 1U) % n];
        const double backX = static_cast<double>(previous.x) - at.x;
        const double backY = static_cast<double>(previous.y) - at.y;
        const double onX = static_cast<double>(next.x) - at.x;
        const double onY = static_cast<double>(next.y) - at.y;
        const double lengths = std::hypot(backX, backY) * std::hypot(onX, onY);
        if (!(lengths > 0.0)) {
            return false;  // a zero-length edge has no corner to measure
        }
        if (((backX * onX) + (backY * onY)) / lengths > sharpestCosine) {
            return false;
        }
    }
    return true;
}

ReferenceFieldParts referenceFieldParts(float frameWidth, float framePaddingX, float innerSpacingX, float swatchEdge,
                                        float hintWidth, float ellipsisWidth, float thumbnailEdge) noexcept {
    const std::array<float, 7> inputs{frameWidth, framePaddingX, innerSpacingX, swatchEdge,
                                      hintWidth,  ellipsisWidth, thumbnailEdge};
    if (!std::all_of(inputs.begin(), inputs.end(), [](float v) { return std::isfinite(v) && v >= 0.0F; })) {
        return ReferenceFieldParts{};  // nothing optional, and a zero budget
    }
    // The thumbnail's column is always kept (it is the slot's identity and sets the frame's height); a swatch is a
    // row's other lead, never drawn beside a thumbnail.
    const bool thumbnailRow = thumbnailEdge > 0.0F;
    const bool swatchRow = !thumbnailRow && swatchEdge > 0.0F;
    const float leadEnd = framePaddingX + (thumbnailRow ? thumbnailEdge + innerSpacingX : 0.0F);
    const float swatchColumn = swatchRow ? swatchEdge + innerSpacingX : 0.0F;
    // 1. The hint, beside the row's swatch and its spacing: the name keeps one ellipsis before the hint's spacing.
    const float hintX = frameWidth - framePaddingX - hintWidth;
    const float nameRoomBesideHint = hintX - innerSpacingX - (leadEnd + swatchColumn);
    ReferenceFieldParts parts{};
    parts.hint = hintWidth > 0.0F && nameRoomBesideHint >= ellipsisWidth;
    // 2. The swatch, where the name keeps one ellipsis beside it and its spacing, before the frame's inner edge.
    parts.swatch = swatchRow && frameWidth - framePaddingX - (leadEnd + swatchColumn) >= ellipsisWidth;
    // 3. The name, after what is kept, up to the hint's spacing or the frame's inner edge -- never a negative budget.
    parts.textX = leadEnd + (parts.swatch ? swatchColumn : 0.0F);
    const float textEnd = parts.hint ? hintX - innerSpacingX : frameWidth - framePaddingX;
    parts.textAvail = std::max(0.0F, textEnd - parts.textX);
    return parts;
}

// D10: a roster glyph per section, chosen for what the section holds (the plan's §21 records the choice).
std::string_view materialSectionIcon(MaterialSectionId id) noexcept {
    switch (id) {
        case MaterialSectionId::Material:
            return AERO_ICON_PALETTE;
        case MaterialSectionId::BaseColor:
            return AERO_ICON_CIRCLE;
        case MaterialSectionId::MetallicRoughness:
            return AERO_ICON_SLIDERS_HORIZONTAL;
        case MaterialSectionId::Normal:
            return AERO_ICON_IMAGE;
        case MaterialSectionId::Occlusion:
            return AERO_ICON_CIRCLE_DASHED;
        case MaterialSectionId::Emission:
            return AERO_ICON_LIGHTBULB;
        case MaterialSectionId::Rendering:
            return AERO_ICON_EYE;
        case MaterialSectionId::File:
            return AERO_ICON_FILE_TEXT;
    }
    return AERO_ICON_SHAPES;
}

// The seven section titles -- verbatim the branch point's CollapsingHeader labels -- drawn as the card's TITLE; the
// card's ImGui id is the host's own ("##card" under the section's PushID in drawImported, "##blender").
std::string_view importDetailsSectionTitle(ImportDetailsSection section) noexcept {
    switch (section) {
        case ImportDetailsSection::Overview:
            return "Overview";
        case ImportDetailsSection::ImportSettings:
            return "Import Settings";
        case ImportDetailsSection::Hierarchy:
            return "Hierarchy";
        case ImportDetailsSection::Meshes:
            return "Meshes";
        case ImportDetailsSection::Materials:
            return "Materials";
        case ImportDetailsSection::SkeletonAnimation:
            return "Skeleton & Animation";
        case ImportDetailsSection::Blender:
            return "Blender";
    }
    return "";
}

std::string_view importDetailsSectionIcon(ImportDetailsSection section) noexcept {
    switch (section) {
        case ImportDetailsSection::Overview:
            return AERO_ICON_INFO;
        case ImportDetailsSection::ImportSettings:
            return AERO_ICON_SETTINGS;
        case ImportDetailsSection::Hierarchy:
            return AERO_ICON_LIST_TREE;
        case ImportDetailsSection::Meshes:
            return AERO_ICON_BOX;
        case ImportDetailsSection::Materials:
            return AERO_ICON_PALETTE;
        case ImportDetailsSection::SkeletonAnimation:
            return AERO_ICON_FILM;
        case ImportDetailsSection::Blender:
            return AERO_ICON_EXTERNAL_LINK;
    }
    return AERO_ICON_SHAPES;
}

std::string_view projectSettingsGroupIcon(std::size_t groupIndex) noexcept {
    if (groupIndex == 0U) {
        return AERO_ICON_FILE_TEXT;  // the manifest group
    }
    if (groupIndex == 1U) {
        return AERO_ICON_FOLDER;  // the location group
    }
    return AERO_ICON_SETTINGS;  // any later group: the fallback, never an error
}

void beginCardRecordFrame(CardRecord& record) noexcept {
    ++record.framesRecorded;
    record.cardsDrawn = 0;
    record.bodiesSubmitted = 0;
    record.primaryButtonsDrawn = 0;
    record.cards.clear();
    record.lastBodyTableMaxX = 0.0F;
    record.lastBodyTableMinY = 0.0F;
    record.lastBodyTableMaxY = 0.0F;
    record.lastSlotClearMaxX = 0.0F;
    record.slotNoteCard = -1;
    record.slotNoteMinX = 0.0F;
    record.slotNoteMaxX = 0.0F;
    record.lastSpanFullWidthMinX = 0.0F;
    record.bodyInChild = false;
}

}  // namespace engine::editor

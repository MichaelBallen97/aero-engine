#pragma once
// Aero Engine -- the Viewport's HUD as VALUES (task E.6.3, spec D9). PUBLIC and PURE: no ImGui, no GPU, so tier 0 walks
// the layout, its drop order, the press claim, the draw-call sum, the count abbreviation and every text (VO1-VO14) that
// viewport_panel.cpp only measures and draws. The record at the bottom is what the panel DREW.
#include <aero/core/math.hpp>             // Vec2
#include <aero/editor/editor_camera.hpp>  // ProjectionMode
#include <aero/editor/status_bar.hpp>     // FrameTimeReadout, frameReadoutText

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// ---- the layout (D9) -------------------------------------------------------------------------------------------
// A HUD rect in SCREEN points. Its geometry is kept when it is not shown, so a test can probe "inside a hidden rect".
struct HudRect {
    Vec2 min{};
    Vec2 max{};
    bool shown = false;
    friend constexpr bool operator==(const HudRect&, const HudRect&) = default;
};
// What the panel MEASURED, in points: the view chip, the stats card's content (its height is the layout's), the two
// hint chips, the fly-speed chip; and what it knows.
struct ViewportHudInputs {
    Vec2 chipSize{};
    Vec2 statsContentSize{};
    std::array<Vec2, 2> hintSizes{};
    Vec2 flyChipSize{};
    bool statsVisible = true;
    bool flying = false;
    bool viewAxisVisible = true;
    std::size_t statsRows = 0;
};
struct ViewportHudLayout {
    HudRect chip{};
    HudRect stats{};
    std::array<HudRect, 2> hints{};
    HudRect flyChip{};
    friend constexpr bool operator==(const ViewportHudLayout&, const ViewportHudLayout&) = default;
};
// Every rect hudInset x uiScale in from the image: the chip top-left, the stats card top-right, the hints (or, while
// flying, the fly chip) bottom-left, hudGap apart. The stats card is sized by the LAYOUT -- width
// max(statsCardWidth x s, statsContentSize.x), height statsCardHeightDp(theme, statsRows) x s -- so it never moves
// when a value changes width.
// PRIORITY, which IS the drop order when the image shrinks: the chip is placed first, the hints next, the stats card
// last; a rect is shown only when it fits inside the inset image and overlaps neither a SHOWN rect placed before it
// nor the view-axis widget's circle (viewAxisCircleRect at the same scale, when the widget is visible -- the circle is
// drawn last, over the HUD). A non-finite or empty image shows nothing; a bad scale is 1.
[[nodiscard]] ViewportHudLayout viewportHudLayout(Vec2 imageMin, Vec2 imageMax, const ViewportHudInputs& inputs,
                                                  float uiScale) noexcept;
// True when `point` lies in a SHOWN rect, half-open (max excluded); a NaN point is owned by nothing. A FREE function:
// ViewportPanel calls it on the layout latched at its last draw and gains no wrapper member (D9).
[[nodiscard]] bool hudOwnsPoint(const ViewportHudLayout& layout, Vec2 point) noexcept;
// The circle the view-axis balls sit in: VIEW_AXIS_HALF_EXTENT_POINTS + viewAxisCircleGap, times the scale (43 at 1).
[[nodiscard]] float viewAxisCircleRadius(float uiScale) noexcept;
// That circle's bounding SQUARE in screen points: viewAxisRect grown by viewAxisCircleGap x uiScale on every side, i.e.
// centre +- viewAxisCircleRadius. The circle reaches five past the half-extent rect at the middle of each side, so
// this is the ONE extent the widget claims: viewportHudLayout's keep test and ViewportPanel's press claim (the scene
// pick and ImGuizmo's Enable term) both read it. DEGENERATE (0,0)-(0,0) exactly when viewAxisRect is -- a hidden
// widget, a bad scale included (viewAxisRect's rule: nothing draws there, so nothing is claimed).
// The box is the circle's NOMINAL extent. AddCircle strokes its path at radius - 0.5 (imgui_draw.cpp:1616) and a
// stroke under one point draws one point thick (:841), so at and below scale 1 the stroke's nominal outer edge is the
// box's edge; above it the stroke is ruleHeight(), s points thick, and that edge sits 0.5 x (s - 1) points past the
// box (half a point at 2x). At EVERY scale the anti-aliasing fringe lies past the nominal edge, up to one point: a
// whole-point stroke takes the textured path, thickness x 0.5 + 1 either side of the path (:887; NewFrame enables it,
// imgui.cpp:5702), a fractional one a half-point fringe (:978). Neither the overhang nor the fringe is claimed or kept
// clear: every roleOutline overhangs its rect the same way (AddRect strokes from min + 0.5, :1524), the stats card's
// too.
void viewAxisCircleRect(Vec2 imageOriginPoints, Vec2 imageSizePoints, float uiScale, Vec2& outMin,
                        Vec2& outMax) noexcept;

// ---- the stats card (D9, D15) -----------------------------------------------------------------------------------
// The viewport frame's draw calls, pass by pass, as the panel latched them after its render (F21).
struct ViewportDrawCounts {
    std::size_t forward = 0;        // ForwardRenderer::lastFrameDrawn
    std::size_t shadow = 0;         // ForwardRenderer::lastFrameShadowDrawn
    std::size_t selectionMask = 0;  // ForwardRenderer::lastFrameSelectionMaskDrawn
    std::uint32_t debug = 0;        // DebugDraw::lastFrameDrawCalls, 0..4
    std::size_t sky = 0;            // SkyPass::drawCount, as a delta across the render
    std::size_t resolve = 0;        // PostProcess::resolveCount, as a delta
    std::size_t composite = 0;      // SelectionOutline::compositeCount, as a delta
    friend constexpr bool operator==(const ViewportDrawCounts&, const ViewportDrawCounts&) = default;
};
[[nodiscard]] std::size_t viewportDrawCalls(const ViewportDrawCounts& counts) noexcept;  // the sum, SATURATING
// 0..9999 plain digits (never grouped); then k = 10^3 whole, M and G with one decimal under 10 of the unit and whole
// above; FLOOR throughout (a printed value is a lower bound and never carries into the next unit); G is the last unit;
// a space before the unit. "184 k", "1.2 M", "18446744073 G". Overwrites `out`.
void abbreviateCount(std::uint64_t value, std::string& out);

// Moved from viewport_panel.cpp:64-65 (D1): the popover's combo and slider widths, in font units.
inline constexpr float VIEW_OPTIONS_COMBO_FONT_MULTIPLE = 7.0F;
inline constexpr float VIEW_OPTIONS_SLIDER_FONT_MULTIPLE = 10.0F;

// "Perspective 60" AERO_GLYPH_DEGREE (the fov rounded to whole degrees), "Orthographic" whatever the fov, and
// "Perspective" alone for a non-finite fov. Overwrites `out`.
void viewChipLabel(ProjectionMode mode, float fovYRadians, std::string& out);
// "fly 4.0 u/s"; a non-finite speed "fly -- u/s". Overwrites `out`.
void flySpeedText(float unitsPerSecond, std::string& out);

inline constexpr std::string_view HINT_FLY = "RMB + WASD to fly";
inline constexpr std::string_view HINT_FRAME = "F to frame";
inline constexpr std::string_view GPU_TIME_UNAVAILABLE_TOOLTIP =
    "GPU time is not measured: SDL_GPU exposes no timestamp queries";
inline constexpr std::string_view STATS_STALENESS_TOOLTIP = "Counted on the previous frame";

struct ViewportStatsRow {
    std::string_view label;  // a string literal: "Frame", "Draw calls", ...
    std::string value;
    friend bool operator==(const ViewportStatsRow&, const ViewportStatsRow&) = default;
};
inline constexpr std::size_t VIEWPORT_STATS_ROW_COUNT = 6;
// Frame (frameReadoutText's ms half: "9.9 ms", "-- ms" before publication), Draw calls, Triangles, Entities (each
// abbreviated), GPU (AERO_GLYPH_EM_DASH), Extent ("1280 " AERO_GLYPH_MULTIPLY " 720"). Clears `out`, keeps capacity.
void viewportStatsRows(const FrameTimeReadout& frame, std::size_t drawCalls, std::size_t triangles,
                       std::size_t entities, std::uint32_t extentW, std::uint32_t extentH,
                       std::vector<ViewportStatsRow>& out);

// ---- what the panel DREW (D14, §6.5) ----------------------------------------------------------------------------
struct ViewportHudRecord {
    std::uint64_t framesRecorded = 0;
    Vec2 imageMin{};             // GetItemRectMin right after ImGui::Image, inset by ImageBorderSize
    Vec2 imageMax{};             // GetItemRectMax, likewise
    Vec2 imageAvail{};           // the size ImGui::Image was handed -- I327's cross-check against max - min
    ViewportHudLayout layout{};  // the rects as DRAWN: what the next frame's press claims read
    std::string chipLabel;
    std::string chipFontName;  // off the ImFont* the chip's text was drawn with
    float chipFontSize = 0.0F;
    std::vector<ViewportStatsRow> statsRows;  // as drawn; empty when the card was not
    ViewportDrawCounts drawCounts{};          // the latch the drawn Draw-calls row consumed
    std::vector<std::string> hintTexts;       // as drawn: the two hints, or the fly chip's text
    Vec2 viewAxisCornerMin{};                 // the widget's half-extent rect, as drawn
    Vec2 viewAxisCornerMax{};
    Vec2 viewAxisCircleCenter{};
    float viewAxisCircleRadius = 0.0F;  // as handed to the circle's draw call
    // The View popover's window rect (GetWindowPos, + GetWindowSize) on a frame it is SHOWN. Zero while it is closed
    // and on an opening's appearing frame, which ImGui hides and auto-fits from zero content (imgui.cpp:8098-8107).
    Vec2 viewOptionsMin{};
    Vec2 viewOptionsMax{};
};
// Opens one recorded frame: + 1 frame, the per-frame lists and texts cleared (capacity kept), every rect hidden, and
// every view-axis widget and View popover field zeroed (each is re-filled only on a frame it draws).
void beginViewportHudRecordFrame(ViewportHudRecord& record) noexcept;

}  // namespace engine::editor

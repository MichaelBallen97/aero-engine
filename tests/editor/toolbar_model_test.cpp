// tests/editor/toolbar_model_test.cpp -- task E.6.2: the toolbar's pure model (TB1-TB11). A TU of
// aero_editor_shell_test (main() is shell_test.cpp's). Tier 0, every configuration, no #if of any kind. Expected
// values are INDEPENDENT literals -- never recomputed by the function under test -- and the theme's shell metrics
// (toolbarPaddingX 12, toolbarGroupGap 14) are restated as literals so a retune is a visible decision here.
#include <aero/editor/editor_glyphs.hpp>
#include <aero/editor/editor_icons.hpp>
#include <aero/editor/toolbar_model.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ostream>  // MSVC: a CHECK over a std::string_view needs the complete std::ostream (the 0.4.1 trap)
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ed = engine::editor;
using ed::ToolbarLayout;
using ed::ToolbarMode;
using ed::ToolbarWidths;
using ed::TransformTool;

constexpr float PAD = 12.0F;  // EDITOR_THEME.shell.toolbarPaddingX, restated
constexpr float GAP = 14.0F;  // EDITOR_THEME.shell.toolbarGroupGap, restated
// Full needs 12+200+14+100+14+120+14+150+14+250+12 = 900; Compact 12+100+14+100+14+120+14+80+14+150+12 = 630.
constexpr ToolbarWidths FULL{.tools = 200.0F, .space = 100.0F, .snap = 120.0F, .play = 150.0F, .undo = 250.0F};
constexpr ToolbarWidths COMPACT{.tools = 100.0F, .space = 100.0F, .snap = 120.0F, .play = 80.0F, .undo = 150.0F};

[[nodiscard]] float bytesX10(std::string_view text) { return 10.0F * static_cast<float>(text.size()); }

[[nodiscard]] int richness(ToolbarMode mode) {  // Full 2, Compact 1, Minimal 0
    return mode == ToolbarMode::Full ? 2 : (mode == ToolbarMode::Compact ? 1 : 0);
}

// The drawn group rects, left to right, so "no two overlap" is one loop.
[[nodiscard]] std::vector<std::array<float, 2>> groupRects(const ToolbarLayout& l, const ToolbarWidths& w) {
    std::vector<std::array<float, 2>> rects{
        {l.toolsX, l.toolsX + w.tools}, {l.spaceX, l.spaceX + w.space}, {l.snapX, l.snapX + w.snap}};
    if (l.playDrawn) {
        rects.push_back({l.playX, l.playX + w.play});
    }
    rects.push_back({l.undoX, l.undoX + w.undo});
    return rects;
}

}  // namespace

TEST_CASE("toolbar: the tool table -- order, icons, labels, keys (task E.6.2, TB1)") {
    REQUIRE(ed::TOOLBAR_TOOLS.size() == 4U);
    for (std::size_t i = 0; i < ed::TOOLBAR_TOOLS.size(); ++i) {
        CHECK((ed::TOOLBAR_TOOLS[i].tool == static_cast<TransformTool>(i)));  // enum order == left to right
    }
    CHECK(ed::TOOLBAR_TOOLS[0].icon == std::string_view(AERO_ICON_MOUSE_POINTER_2));
    CHECK(ed::TOOLBAR_TOOLS[1].icon == std::string_view(AERO_ICON_MOVE));
    CHECK(ed::TOOLBAR_TOOLS[2].icon == std::string_view(AERO_ICON_ROTATE_CW));
    CHECK(ed::TOOLBAR_TOOLS[3].icon == std::string_view(AERO_ICON_SCALING));
    CHECK(ed::TOOLBAR_TOOLS[0].iconLabel == std::string_view(AERO_ICON_MOUSE_POINTER_2 " Select"));
    CHECK(ed::TOOLBAR_TOOLS[1].iconLabel == std::string_view(AERO_ICON_MOVE " Move"));
    CHECK(ed::TOOLBAR_TOOLS[2].iconLabel == std::string_view(AERO_ICON_ROTATE_CW " Rotate"));
    CHECK(ed::TOOLBAR_TOOLS[3].iconLabel == std::string_view(AERO_ICON_SCALING " Scale"));
    CHECK(ed::TOOLBAR_TOOLS[0].tooltip == "Select (Q)");
    CHECK(ed::TOOLBAR_TOOLS[1].tooltip == "Move (W)");
    CHECK(ed::TOOLBAR_TOOLS[2].tooltip == "Rotate (E)");
    CHECK(ed::TOOLBAR_TOOLS[3].tooltip == "Scale (R)");
    CHECK(ed::LOCAL_SEGMENT_LABEL == "Local");
    CHECK(ed::WORLD_SEGMENT_LABEL == "World");
    CHECK(ed::LOCAL_SEGMENT_TOOLTIP == "Local space (X toggles)");
    CHECK(ed::WORLD_SEGMENT_TOOLTIP == "World space (X toggles)");
    CHECK(ed::SCALE_IS_LOCAL_TOOLTIP == "Scale is always Local");
}

TEST_CASE("toolbar: the play table, its one tooltip, and the snap and undo words (task E.6.2, TB2)") {
    REQUIRE(ed::TOOLBAR_PLAY_CONTROLS.size() == 3U);
    CHECK(ed::TOOLBAR_PLAY_CONTROLS[0].iconLabel == std::string_view(AERO_ICON_PLAY " Play"));
    CHECK(ed::TOOLBAR_PLAY_CONTROLS[1].iconLabel == std::string_view(AERO_ICON_PAUSE " Pause"));
    CHECK(ed::TOOLBAR_PLAY_CONTROLS[2].iconLabel == std::string_view(AERO_ICON_STEP_FORWARD " Step"));
    CHECK(ed::TOOLBAR_PLAY_CONTROLS[2].icon == std::string_view(AERO_ICON_STEP_FORWARD));
    CHECK(ed::PLAY_CONTROLS_TOOLTIP == "Not implemented yet -- task 4.7.1");
    CHECK(ed::SNAP_TOGGLE_LABEL == std::string_view(AERO_ICON_MAGNET " Snap"));
    CHECK(ed::SNAP_FIELD_SELECT_TOOLTIP == "Select draws no gizmo");
    CHECK(ed::UNDO_BUTTON_LABEL == std::string_view(AERO_ICON_UNDO_2 " Undo"));
    CHECK(ed::NOTHING_TO_UNDO_TOOLTIP == "Nothing to undo");
    CHECK(ed::snapToggleTooltip(ed::HostOs::MacOs) == "Snap while dragging (hold Cmd to invert)");
    CHECK(ed::snapToggleTooltip(ed::HostOs::Windows) == "Snap while dragging (hold Ctrl to invert)");
    CHECK(ed::snapToggleTooltip(ed::HostOs::Linux) == "Snap while dragging (hold Ctrl to invert)");
}

TEST_CASE("toolbar: the snap field's operation, format, value and text are one source (task E.6.2, TB3)") {
    CHECK((ed::snapFieldOperation(TransformTool::Select) == ed::GizmoOperation::Translate));
    CHECK((ed::snapFieldOperation(TransformTool::Move) == ed::GizmoOperation::Translate));
    CHECK((ed::snapFieldOperation(TransformTool::Rotate) == ed::GizmoOperation::Rotate));
    CHECK((ed::snapFieldOperation(TransformTool::Scale) == ed::GizmoOperation::Scale));
    CHECK(std::string_view(ed::snapStepFormat(TransformTool::Move)) == "%.4g m");
    CHECK(std::string_view(ed::snapStepFormat(TransformTool::Select)) == "%.4g m");
    CHECK(std::string_view(ed::snapStepFormat(TransformTool::Rotate)) == "%.4g\xC2\xB0");  // the degree sign
    CHECK(std::string_view(ed::snapStepFormat(TransformTool::Scale)) == "%.4g");
    const ed::SnapSettings s{
        .enabled = false,
        .translateStep = 2.5F,
        .rotateStepDegrees = 30.0F,
        .scaleStep = 0.25F,
    };
    CHECK(ed::snapStepValue(TransformTool::Select, s) == 2.5F);
    CHECK(ed::snapStepValue(TransformTool::Move, s) == 2.5F);
    CHECK(ed::snapStepValue(TransformTool::Rotate, s) == 30.0F);
    CHECK(ed::snapStepValue(TransformTool::Scale, s) == 0.25F);
    CHECK(ed::snapStepText(TransformTool::Move, s) == "2.5 m");
    CHECK(ed::snapStepText(TransformTool::Rotate, s) == "30\xC2\xB0");
    CHECK(ed::snapStepText(TransformTool::Scale, s) == "0.25");
    const ed::SnapSettings ends{.translateStep = 1000.0F, .rotateStepDegrees = 0.1F, .scaleStep = 0.001F};
    CHECK(ed::snapStepText(TransformTool::Move, ends) == "1000 m");  // %.4g, never 1e+03
    CHECK(ed::snapStepText(TransformTool::Rotate, ends) == "0.1\xC2\xB0");
    CHECK(ed::snapStepText(TransformTool::Scale, ends) == "0.001");
}

TEST_CASE("toolbar: the widest mode that fits, at and around each boundary (task E.6.2, TB4)") {
    CHECK((ed::toolbarLayout(900.0F, FULL, COMPACT, 1.0F).mode == ToolbarMode::Full));  // at the boundary: fits
    CHECK((ed::toolbarLayout(900.5F, FULL, COMPACT, 1.0F).mode == ToolbarMode::Full));
    CHECK((ed::toolbarLayout(899.5F, FULL, COMPACT, 1.0F).mode == ToolbarMode::Compact));
    CHECK((ed::toolbarLayout(630.0F, FULL, COMPACT, 1.0F).mode == ToolbarMode::Compact));
    CHECK((ed::toolbarLayout(629.5F, FULL, COMPACT, 1.0F).mode == ToolbarMode::Minimal));
    CHECK((ed::toolbarLayout(0.0F, FULL, COMPACT, 1.0F).mode == ToolbarMode::Minimal));
    int previous = richness(ed::toolbarLayout(0.0F, FULL, COMPACT, 1.0F).mode);
    for (int width = 1; width <= 1100; ++width) {
        CAPTURE(width);
        const int now = richness(ed::toolbarLayout(static_cast<float>(width), FULL, COMPACT, 1.0F).mode);
        CHECK(now >= previous);  // monotonic: a wider window never loses a mode
        previous = now;
    }
}

TEST_CASE("toolbar: placement -- packed left, undo right, play centred, nothing overlapping (task E.6.2, TB5)") {
    const ToolbarLayout wide = ed::toolbarLayout(1000.0F, FULL, COMPACT, 1.0F);
    CHECK(wide.toolsX == PAD);
    CHECK(wide.spaceX == 226.0F);         // 12 + 200 + 14
    CHECK(wide.snapX == 340.0F);          // 226 + 100 + 14
    CHECK(wide.leftGroupsEnd == 460.0F);  // 340 + 120
    CHECK(wide.undoX == 738.0F);          // 1000 - 12 - 250: right-aligned
    REQUIRE(wide.playDrawn);
    CHECK(wide.playX == 524.0F);  // centred in [474, 724]: (474 + 724) / 2 - 75
    // At the Full boundary the play group exactly fills its span.
    const ToolbarLayout edge = ed::toolbarLayout(900.0F, FULL, COMPACT, 1.0F);
    CHECK(edge.undoX == 638.0F);
    CHECK(edge.playX == 474.0F);
    // Minimal at 500: the undo group is PACKED after the left groups (the right-aligned x would overlap them).
    const ToolbarLayout narrow = ed::toolbarLayout(500.0F, FULL, COMPACT, 1.0F);
    REQUIRE((narrow.mode == ToolbarMode::Minimal));
    CHECK(narrow.undoX == 374.0F);  // 360 + 14, not 500 - 12 - 150 = 338
    for (int width = 0; width <= 1100; ++width) {
        CAPTURE(width);
        const ToolbarLayout l = ed::toolbarLayout(static_cast<float>(width), FULL, COMPACT, 1.0F);
        const ToolbarWidths& w = l.mode == ToolbarMode::Full ? FULL : COMPACT;
        const std::vector<std::array<float, 2>> rects = groupRects(l, w);
        for (std::size_t i = 1; i < rects.size(); ++i) {
            CHECK(rects[i - 1U][1] <= rects[i][0]);  // no two group rects overlap, at any width (seed S46)
        }
        CHECK(l.undoX >= l.leftGroupsEnd + GAP);
    }
}

TEST_CASE("toolbar: which elements each mode shows (task E.6.2, TB6)") {
    const ToolbarLayout full = ed::toolbarLayout(1000.0F, FULL, COMPACT, 1.0F);
    const ToolbarLayout compact = ed::toolbarLayout(700.0F, FULL, COMPACT, 1.0F);
    const ToolbarLayout minimal = ed::toolbarLayout(600.0F, FULL, COMPACT, 1.0F);
    CHECK((full.mode == ToolbarMode::Full));
    CHECK(full.playDrawn);
    CHECK((compact.mode == ToolbarMode::Compact));
    CHECK(compact.playDrawn);
    CHECK(compact.undoX == 538.0F);  // 700 - 12 - 150: COMPACT's undo (no label slot)
    CHECK(compact.playX == 409.0F);  // centred in [374, 524]: (374 + 524) / 2 - 40
    CHECK((minimal.mode == ToolbarMode::Minimal));
    CHECK_FALSE(minimal.playDrawn);
}

TEST_CASE("toolbar: a NaN, negative or infinite width is the packed placement, all finite (task E.6.2, TB7)") {
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), -5.0F, 0.0F,
                            -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()}) {
        CAPTURE(bad);
        const ToolbarLayout l = ed::toolbarLayout(bad, FULL, COMPACT, 1.0F);
        CHECK((l.mode == ToolbarMode::Minimal));
        CHECK_FALSE(l.playDrawn);
        for (const float x : {l.toolsX, l.spaceX, l.snapX, l.leftGroupsEnd, l.undoX, l.playX}) {
            CHECK(std::isfinite(x));
        }
        CHECK(l.undoX == 374.0F);  // packed after the left groups
    }
}

TEST_CASE("toolbar: the undo label -- empty, verbatim, or elided into its slot (task E.6.2, TB8)") {
    CHECK(ed::undoAffordanceLabel("", 100.0F, bytesX10).empty());  // nothing to undo: no label
    CHECK(ed::undoAffordanceLabel("Rename", 100.0F, bytesX10) == "Rename");
    CHECK(ed::undoAffordanceLabel("100% ##x", 100.0F, bytesX10) == "100% ##x");  // text, never a format or an id
    // 29 bytes into a 10-byte slot: the longest prefix + the 3-byte ellipsis that fits is 7 + 3.
    const std::string elided = ed::undoAffordanceLabel("DirectionalLight.castsShadows", 100.0F, bytesX10);
    CHECK(elided == "Directi" AERO_GLYPH_ELLIPSIS);
    CHECK(bytesX10(elided) <= 100.0F);
}

TEST_CASE("toolbar: every group's width, and the undo group's FIXED slot (task E.6.2, TB9)") {
    const ed::ToolbarMetrics m{.toolFull = {80.0F, 70.0F, 75.0F, 72.0F},
                               .toolCompact = {30.0F, 30.0F, 30.0F, 30.0F},
                               .localSegment = 52.0F,
                               .worldSegment = 54.0F,
                               .snapToggleFull = 66.0F,
                               .snapToggleCompact = 30.0F,
                               .snapField = 64.0F,
                               .playFull = {60.0F, 70.0F, 62.0F},
                               .playCompact = {30.0F, 30.0F, 30.0F},
                               .undoButton = 64.0F,
                               .undoChip = 44.0F,
                               .itemSpacing = 8.0F,
                               .groupPadding = 4.0F,
                               .fontSize = 16.0F};
    const ed::ToolbarWidthPair w = ed::toolbarWidths(m);
    CHECK(w.full.tools == 305.0F);     // 8 + 80 + 70 + 75 + 72
    CHECK(w.compact.tools == 128.0F);  // 8 + 4 x 30
    CHECK(w.full.space == 114.0F);     // 8 + 52 + 54, both modes
    CHECK(w.compact.space == 114.0F);
    CHECK(w.full.snap == 146.0F);     // 8 + 66 + 8 + 64
    CHECK(w.compact.snap == 110.0F);  // 8 + 30 + 8 + 64
    CHECK(w.full.play == 208.0F);     // 60 + 70 + 62 + 2 x 8
    CHECK(w.compact.play == 106.0F);  // 3 x 30 + 2 x 8
    CHECK(w.full.undo == 284.0F);     // 64 + 8 + 44 + 8 + 10 x 16 -- the SLOT, whatever the label (seed S45)
    CHECK(w.compact.undo == 116.0F);  // 64 + 8 + 44: no slot
    CHECK(ed::UNDO_LABEL_SLOT_EM == 10.0F);
    CHECK(ed::SNAP_FIELD_WIDTH_EM == 4.0F);
}

TEST_CASE("toolbar: a scale of 2 doubles every x and every gap, and flips a fixed-width mode (task E.6.2, TB10)") {
    constexpr ToolbarWidths FULL2{
        .tools = 400.0F,
        .space = 200.0F,
        .snap = 240.0F,
        .play = 300.0F,
        .undo = 500.0F,
    };
    constexpr ToolbarWidths COMPACT2{
        .tools = 200.0F,
        .space = 200.0F,
        .snap = 240.0F,
        .play = 160.0F,
        .undo = 300.0F,
    };
    for (const float width : {400.0F, 500.0F, 629.0F, 700.0F, 899.0F, 900.0F, 1000.0F, 1200.0F}) {
        CAPTURE(width);
        const ToolbarLayout one = ed::toolbarLayout(width, FULL, COMPACT, 1.0F);
        const ToolbarLayout two = ed::toolbarLayout(2.0F * width, FULL2, COMPACT2, 2.0F);
        CHECK((two.mode == one.mode));
        CHECK(two.toolsX == 2.0F * one.toolsX);
        CHECK(two.spaceX == 2.0F * one.spaceX);
        CHECK(two.snapX == 2.0F * one.snapX);
        CHECK(two.undoX == 2.0F * one.undoX);
        CHECK(two.playX == 2.0F * one.playX);
    }
    // Fixed widths: at 900, scale 1 fits Full exactly; at scale 2 the padding and gaps alone grow by 80 (seed S47).
    CHECK((ed::toolbarLayout(900.0F, FULL, COMPACT, 1.0F).mode == ToolbarMode::Full));
    CHECK((ed::toolbarLayout(900.0F, FULL, COMPACT, 2.0F).mode == ToolbarMode::Compact));  // needs 980
}

TEST_CASE("toolbar: bar heights are whole points at every quantised scale (task E.6.2, TB11)") {
    for (int k = 10; k <= 80; ++k) {
        const float s = static_cast<float>(k) / 20.0F;  // resolveUiScale's quantum, by DIVISION
        CAPTURE(s);
        const float toolbar = ed::shellBarHeight(44.0F, s);
        const float status = ed::shellBarHeight(26.0F, s);
        CHECK(toolbar == std::round(44.0F * s));
        CHECK(status == std::round(26.0F * s));
        CHECK(toolbar == std::trunc(toolbar));  // whole
        CHECK(status == std::trunc(status));
        // What Begin's truncation does to a Down bar at the bottom of a whole-point viewport: nothing.
        constexpr float VIEWPORT = 600.0F;
        CHECK(std::trunc(VIEWPORT - status) + status == VIEWPORT);
    }
    CHECK(ed::shellBarHeight(26.0F, 1.25F) == 33.0F);  // 32.5 rounds to 33 -- the unpainted-row case (seed S48)
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), 0.0F, -1.0F}) {
        CHECK(ed::shellBarHeight(44.0F, bad) == 44.0F);
    }
}

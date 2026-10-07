#pragma once
// Aero Engine — what the shell's chrome DREW, for the GPU tier (task E.6.2, D21). PUBLIC, PURE and header-only:
// EditorApp holds the value, editor/src/shell_chrome_ui.cpp fills it each frame FROM WHAT ImGui DID -- item rects
// from GetItemRectMin/Max, flags from GetItemFlags, fonts from GetFont()->GetDebugName() and GetFontSize(), the
// viewport and its work area from GetMainViewport() -- plus the exact inputs it handed each pure layout, so a
// GPU case compares what was drawn against the layout's own answer for the geometry the machine delivered (#111).
// NOTHING here is a request read back. The one stated exception: snapFormat/snapValue are what was HANDED to
// DragFloat, because a DragFloat formats into a stack buffer that nothing can read back.
#include <aero/core/math.hpp>
#include <aero/editor/toolbar_model.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace engine::editor {

struct ToolbarButtonRecord {
    Vec2 min{};
    Vec2 max{};
    bool drawnActive = false;  // drawn with the active push set (accent + onAccent, or active + textBright)
    bool enabled = false;      // ImGuiItemFlags_Disabled was CLEAR on the item, read back with GetItemFlags()
};

struct ShellChromeRecord {
    std::uint64_t framesRecorded = 0;  // + 1 per drawToolbar: every read below is vacuous while this is 0
    float uiScaleAtDraw = 0.0F;        // currentUiScale() as the chrome drew (named apart from US9's token)
    // GetMainViewport() during the draw: this frame's work area, fixed at NewFrame from LAST frame's insets.
    Vec2 viewportPos{};
    Vec2 viewportSize{};
    Vec2 workPos{};
    Vec2 workSize{};
    // The three bars as ImGui laid them out (GetWindowPos / GetWindowSize inside each Begin). menuNavFocusable is
    // the positive control for the two bars' false (the menu bar carries no NoNavFocus).
    Vec2 menuMin{};
    Vec2 menuMax{};
    bool menuNavFocusable = false;
    bool toolbarDrawn = false;  // Begin's own answer
    Vec2 toolbarMin{};
    Vec2 toolbarMax{};
    float toolbarAxis = 0.0F;  // the height BeginViewportSideBar received
    bool toolbarNavFocusable = true;
    bool statusDrawn = false;
    Vec2 statusMin{};
    Vec2 statusMax{};
    float statusAxis = 0.0F;
    bool statusNavFocusable = true;
    // The toolbar layout: its exact inputs, and its answer as used.
    float toolbarAvailableWidth = 0.0F;
    ToolbarMetrics metrics{};
    ToolbarWidths fullWidths{};
    ToolbarWidths compactWidths{};
    ToolbarLayout layout{};
    std::array<ToolbarButtonRecord, 4> tools{};  // TOOLBAR_TOOLS order
    std::array<ToolbarButtonRecord, 2> space{};  // Local, World
    ToolbarButtonRecord snapToggle{};
    bool snapFieldEnabled = false;
    std::string snapFormat;  // HANDED to DragFloat (the header's one stated exception)
    float snapValue = 0.0F;  // likewise
    bool playGroupDrawn = false;
    std::array<bool, 3> playDisabled{};  // GetItemFlags() & ImGuiItemFlags_Disabled after each play button
    ToolbarButtonRecord undo{};
    std::string undoChip;   // the chip's text as drawn
    std::string undoLabel;  // the label as drawn; "" when none was drawn
    // The breadcrumb, in SCREEN space.
    float breadcrumbBarLeft = 0.0F;
    float breadcrumbBarRight = 0.0F;
    float breadcrumbMenusEnd = 0.0F;
    std::string breadcrumbProject;  // as drawn; "" when dropped or absent
    std::string breadcrumbScene;
    bool breadcrumbSeparatorDrawn = false;
    bool dirtyDotDrawn = false;
    std::string breadcrumbProjectFont;  // GetFont()->GetDebugName() at each text
    std::string breadcrumbSceneFont;
    float breadcrumbProjectX = 0.0F;  // GetItemRectMin().x after each text
    float breadcrumbSceneX = 0.0F;
    float breadcrumbSceneCenterY = 0.0F;  // the scene text's item-rect mid-y
    float breadcrumbDotCenterY = 0.0F;    // the dirty dot's centre y, when dotDrawn (D11: centred on the text line)
    // The status bar.
    float statusBarWidth = 0.0F;  // the width statusBarLayout received
    std::string statusRoot;       // each string as drawn
    std::string statusWatch;
    std::string statusFrame;
    std::string statusBackend;
    std::string statusFont;
    float statusFontSize = 0.0F;  // GetFontSize() inside the push
};

}  // namespace engine::editor

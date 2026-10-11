#pragma once
// Aero Engine -- the panels' shared chrome (task E.6.3, spec D2, D4, D11). SRC-PRIVATE, and the ONE ImGui TU every
// panel paints its cards, chips, buttons, scopes and roled text through: it MEASURES with ImGui, takes every number
// from panel_chrome_model's pure functions, and DRAWS -- it decides nothing a tier-0 case cannot see. This header
// includes <imgui.h> (the text_input.hpp precedent): only ImGui TUs include it.
//
// THREE RULES this file lives by.
//   * Every push is popped 1:1 with nothing between them that can return: the four scoped styles push in their
//     constructor and pop in their destructor, and PanelCard ends its card in its destructor.
//   * No helper takes a format string: roleText, mutedText, mutedTextWrapped, countBadge and drawRoleText draw
//     their string UNFORMATTED, so the D11 sweep is format-safe by construction (I333: nothing here is variadic).
//   * Every colour is a ChromeRole through chromeRoleColor; a slot that must paint nothing is clearOver(surface) --
//     the surface's role at alpha 0, the ONE transparent spelling (I333). The one face switch is ScopedFace (I286).
#include <aero/editor/panel_chrome_model.hpp>

#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <string_view>

namespace engine::editor {

// The faces a panel draws in: E.6.1's three fonts at the theme's sizes (Small = smallSize, Title = titleSize).
enum class PanelFace : std::uint8_t { Body, Strong, Mono, BodySmall, StrongSmall, MonoSmall, StrongTitle };

[[nodiscard]] ImFont* panelFaceFont(PanelFace face) noexcept;
[[nodiscard]] float panelFaceSizeDp(PanelFace face) noexcept;  // the theme's size, unscaled
[[nodiscard]] float panelFacePoints(PanelFace face);           // faceSizePoints(dp, the live UI scale)
// The one measurer a layout is handed: the EXACT advance sum (ImFont::CalcTextSizeA), never ImGui::CalcTextSize's
// whole-point round-up (imgui.cpp:6456) -- a column of advances must not accumulate it.
[[nodiscard]] float faceTextWidth(PanelFace face, std::string_view text);

// The one PushFont / PopFont pair a panel uses (I286): the face at its UNSCALED size, which ImGui scales and rounds.
class ScopedFace {
public:
    explicit ScopedFace(PanelFace face);
    ~ScopedFace();
    ScopedFace(const ScopedFace&) = delete;
    ScopedFace& operator=(const ScopedFace&) = delete;
    ScopedFace(ScopedFace&&) = delete;
    ScopedFace& operator=(ScopedFace&&) = delete;
};

// Colours at the draw. clearOver is THE transparent spelling: chromeRoleColor(surface, 0), converted once.
[[nodiscard]] ImVec4 roleVec4(ChromeRole role) noexcept;
[[nodiscard]] ImU32 roleU32(ChromeRole role) noexcept;
[[nodiscard]] ImVec4 clearOver(ChromeRole surface) noexcept;

// ---- text: unformatted, always -----------------------------------------------------------------------------
void roleText(ChromeRole role, std::string_view text);  // TextUnformatted under a pushed Text colour
void mutedText(std::string_view text);                  // D11's helper: roleText(TextMuted, text)
void mutedTextWrapped(std::string_view text);           // the same, wrapped at the content edge
void countBadge(std::string_view text);                 // Mono small, textFaint, as an ImGui item
void drawRoleText(ImDrawList* list, PanelFace face, ImVec2 pos, ChromeRole role, std::string_view text);

// ---- buttons and scopes ------------------------------------------------------------------------------------
// Around a caller's own ImGui::Button: a clear fill over `panel`, the `active` outline (D3's nearest to the mock's
// 42,47,55), textSecondary; hover and press take `hover` / `active`. I77 keeps its Button lines this way.
class GhostStyle {
public:
    GhostStyle();
    ~GhostStyle();
    GhostStyle(const GhostStyle&) = delete;
    GhostStyle& operator=(const GhostStyle&) = delete;
    GhostStyle(GhostStyle&&) = delete;
    GhostStyle& operator=(GhostStyle&&) = delete;
};
[[nodiscard]] bool ghostButton(const char* label, ImVec2 size = ImVec2(0.0F, 0.0F));
// Header / HeaderHovered / HeaderActive at clearOver(surface): a tile or tree Selectable / node paints no fill (D8).
class ClearHeaderStyle {
public:
    explicit ClearHeaderStyle(ChromeRole surface);
    ~ClearHeaderStyle();
    ClearHeaderStyle(const ClearHeaderStyle&) = delete;
    ClearHeaderStyle& operator=(const ClearHeaderStyle&) = delete;
    ClearHeaderStyle(ClearHeaderStyle&&) = delete;
    ClearHeaderStyle& operator=(ClearHeaderStyle&&) = delete;
};
// FrameBg / FrameBgHovered / FrameBgActive at clearOver(surface) and FrameBorderSize 0: the frameless axis drag (D5).
// The border push is what makes the drag frameless: DragScalar strokes a border through RenderFrameBorder
// (imgui_widgets.cpp:2793) and its temp-input InputTextEx through RenderFrame(..., true, ...) (:5459).
class ClearFrameStyle {
public:
    explicit ClearFrameStyle(ChromeRole surface);
    ~ClearFrameStyle();
    ClearFrameStyle(const ClearFrameStyle&) = delete;
    ClearFrameStyle& operator=(const ClearFrameStyle&) = delete;
    ClearFrameStyle(ClearFrameStyle&&) = delete;
    ClearFrameStyle& operator=(ClearFrameStyle&&) = delete;
};
[[nodiscard]] bool primaryButton(const char* label, bool enabled);  // accent on all three slots, onAccent text
// E.6.2's groupButton, hoisted: one button inside a `raised` segmented group (the toolbar, Grid | List).
[[nodiscard]] bool segmentButton(const char* label, bool active, ChromeRole activeFill, ChromeRole activeText,
                                 ChromeRole idleText, ItemRectRecord& record);
void segmentGroupBackground(ImDrawList* list, ImVec2 min, ImVec2 max);  // E.6.2's groupBackground, hoisted

// ---- chips, fills, outlines, marks --------------------------------------------------------------------------
struct ChipDraw {
    bool pressed = false;
    bool hovered = false;
    ImVec2 min{0.0F, 0.0F};
    ImVec2 max{0.0F, 0.0F};
    const ImFont* font = nullptr;  // the face the label was drawn in -- a record copies its debug name
    float fontSize = 0.0F;
};
[[nodiscard]] ImVec2 chipSize(std::string_view label, std::string_view trailingIcon);
// An InvisibleButton with a drawn frame and its label and icon in PanelFace::BodySmall -- never Mono, which carries
// no icons and would draw U+FFFD (D9).
[[nodiscard]] ChipDraw chipButton(const char* strId, std::string_view label, std::string_view trailingIcon);
void chipFrame(ImDrawList* list, ImVec2 min, ImVec2 max, ChromeRole fill);  // + a divider outline, frameRounding
void roleFill(ImDrawList* list, ImVec2 min, ImVec2 max, ChromeRole role, float rounding);
void roleOutline(ImDrawList* list, ImVec2 min, ImVec2 max, ChromeRole role, float rounding, float thickness);
void roleDot(ImDrawList* list, ImVec2 center, float diameter, ChromeRole role);
void hatchedWell(ImDrawList* list, ImVec2 min, ImVec2 max, float rounding);  // tile + 45-degree hatchStripe bands
void dashedRect(ImDrawList* list, ImVec2 min, ImVec2 max);                   // addComponentDash, ThemePanels dashes
void horizontalRule(ImDrawList* list, float minX, float maxX, float y, ChromeRole role);  // ruleHeight() thick
[[nodiscard]] float ruleHeight();      // hairline(shell.ruleThickness, live scale, framebuffer scale)
[[nodiscard]] float tileRingHeight();  // hairline(panels.tileRing, live scale, framebuffer scale)

// ---- the card (D4) -------------------------------------------------------------------------------------------
struct CardSpec {
    std::string_view title;  // drawn on the draw list, never an ImGui label: a "##" in it cannot truncate it
    std::string_view icon;   // an AERO_ICON_* view; empty draws none
    bool overflow = false;   // a trailing "..." button whose press the host reads off CardHeaderResult
};
// Every per-header answer a host may read: after begin() the LAST ITEM is the overflow button whenever the card
// has one, so a host never calls IsItemHovered / BeginPopupContextItem after a PanelCard (D4).
struct CardHeaderResult {
    bool open = false;
    bool overflowPressed = false;
    bool headerRightClicked = false;  // IsMouseReleased(Right) && IsItemHovered(AllowWhenBlockedByPopup)
    bool headerHovered = false;       // read immediately after the header item, before the overflow
};

// ONE per host panel, a MEMBER: its splitter is persistent so a table's own splitter can stack inside a body
// (imgui.h:3532-3533). begin / end are 1:1; nesting is refused.
class CardPainter {
public:
    CardHeaderResult begin(const char* strId, const CardSpec& spec, bool* panelOwnedOpen, CardRecord* record);
    void end();

private:
    ImDrawListSplitter splitter;
    ImDrawList* list = nullptr;
    CardRecord* recordPtr = nullptr;
    CardGeometry geometry{};
    CardEntryRecord entry{};  // filled across begin / end, appended to the record at end
    float spanMinX = 0.0F;
    float spanMaxX = 0.0F;
    float topY = 0.0F;
    float stripBottomY = 0.0F;
    float savedWorkMinX = 0.0F;
    float savedContentMaxX = 0.0F;
    float savedWorkMaxX = 0.0F;
    bool active = false;
    bool open = false;
};

// The scoped form every host uses: ~PanelCard() ends the card on every path.
class PanelCard {
public:
    PanelCard(CardPainter& painter, const char* strId, const CardSpec& spec, bool* panelOwnedOpen, CardRecord* record);
    ~PanelCard();
    PanelCard(const PanelCard&) = delete;
    PanelCard& operator=(const PanelCard&) = delete;
    PanelCard(PanelCard&&) = delete;
    PanelCard& operator=(PanelCard&&) = delete;
    [[nodiscard]] const CardHeaderResult& header() const noexcept { return result; }

private:
    CardPainter* painterPtr;
    CardHeaderResult result;
};

}  // namespace engine::editor

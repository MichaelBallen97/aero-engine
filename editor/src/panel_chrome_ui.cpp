// Aero Engine -- the panels' shared chrome (task E.6.3). The ONE ImGui TU every panel's card, chip, button, scope and
// roled text goes through; panel_chrome_ui.hpp states its three rules.
//
// THE CARD, mechanically (spec section 6.4; every cited line is the pinned 1.92.8-docking):
//   * the header ITEM is a framed TreeNodeEx with an EMPTY label -- CollapsingHeader's own behaviour
//     (imgui_widgets.cpp:7304-7311) -- under pushes that make ImGui draw NOTHING: its three Header slots and its Text
//     (the arrow, :7110) at alpha 0, and FrameBorderSize 0 (RenderFrame strokes a border otherwise, imgui.cpp:
//     4063-4074). Its frame_bb is the cursor..WorkRect span widened by IM_TRUNC(WindowPadding.x * 0.5) on each side
//     (:6902-6911) -- the overhang, accepted; every drawn x comes from the span RECORDED at begin.
//   * the strip, chevron, icon and title are drawn by this painter AFTER the item answers, on the splitter's
//     background channel (0) and content channel (1), so the colours follow hover and open with no frame of lag.
//   * the body is narrowed on BOTH edges by writing WorkRect.Min.x, ContentRegionRect.Max.x and WorkRect.Max.x --
//     GetContentRegionAvail reads ContentRegionRect outside a table (imgui.cpp:12374-12380), a SpanFullWidth row
//     starts at WorkRect.Min.x (imgui_widgets.cpp:6902) -- saved at begin and restored at end. Begin rewrites
//     ContentRegionRect every frame (imgui.cpp:8544-8547), so a missed restore leaks for the rest of THIS frame,
//     where I318's cross-card comparison sees it.
//   * every vertical position is PLACED, net of ItemSpacing.y: ItemSize adds it (imgui.cpp:12129), so the body
//     starts at a SetCursorScreenPos and the next card is reached by ONE Dummy of the net height -- a bare
//     SetCursorScreenPos with no item after it asserts at End() (imgui.cpp:8839-8840, :11533-11543).
#include "panel_chrome_ui.hpp"

#include <aero/editor/editor_icons.hpp>
#include <aero/editor/editor_theme.hpp>

#include "editor_fonts.hpp"        // editorFonts(): Body, Strong, Mono
#include "editor_theme_imgui.hpp"  // toImVec4
#include "editor_theme_ui.hpp"     // currentUiScale, toImU32

#include <algorithm>
#include <cfloat>  // FLT_MAX -- ImFont::CalcTextSizeA's "no limit" width
#include <cmath>
#include <exception>         // std::terminate (~PanelCard)
#include <imgui_internal.h>  // ImGuiWindow (the body's edges), ImTextCharFromUtf8, GetItemFlags, ImGuiItemFlags_*

namespace engine::editor {

namespace {

constexpr ImGuiTreeNodeFlags CARD_HEADER_FLAGS = ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                 ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth |
                                                 ImGuiTreeNodeFlags_DefaultOpen;

[[nodiscard]] Vec2 toVec2(ImVec2 v) noexcept { return Vec2{v.x, v.y}; }

void recordItemRect(ItemRectRecord& record, bool drawnActive) {
    record.min = toVec2(ImGui::GetItemRectMin());
    record.max = toVec2(ImGui::GetItemRectMax());
    record.drawnActive = drawnActive;
    record.enabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) == 0;
}

// A glyph's INK at `points`: the COLUMNS of its baked bitmap that hold any coverage, mapped onto its quad -- the
// column twin of editor_fonts.cpp's measureInk. X1 - X0 is the glyph BOX, and Lucide's glyf boxes start at x = 0
// whatever the outline does (chevron-right: box 0..666, ink 334..666 of 1000). X0, X1 and PackId are copied at once
// (a lookup can bake, and a bake can reallocate the glyph table -- E.6.1's rule); GetCustomRect resolves the pack id
// against the CURRENT texture. {0, 0} when the face has no such glyph or the atlas holds no pixels.
struct GlyphInk {
    float left = 0.0F;   // the ink's left edge, relative to the pen x, at `points`
    float width = 0.0F;  // the ink's width at `points`
    float box = 0.0F;    // the glyph BOX's width (X1 - X0) at `points` -- what a chevron is never sized by
};
[[nodiscard]] GlyphInk glyphInk(ImFont* font, float points, std::string_view glyph) {
    if (font == nullptr || glyph.empty()) {
        return {};
    }
    unsigned int codepoint = 0;
    ImTextCharFromUtf8(&codepoint, glyph.data(), glyph.data() + glyph.size());
    ImFontBaked* const baked = font->GetFontBaked(points);
    const ImFontGlyph* const found =
        baked != nullptr ? baked->FindGlyphNoFallback(static_cast<ImWchar>(codepoint)) : nullptr;
    if (found == nullptr || found->Visible == 0U) {
        return {};
    }
    const float x0 = found->X0;
    const float x1 = found->X1;
    const int packId = found->PackId;
    const ImFontAtlas* const atlas = ImGui::GetIO().Fonts;  // the atlas every editor face is loaded into
    const ImTextureData* const texture = atlas != nullptr ? atlas->TexData : nullptr;
    ImFontAtlasRect rect;
    if (texture == nullptr || texture->Pixels == nullptr || !atlas->GetCustomRect(packId, &rect) || rect.w == 0) {
        return {};
    }
    const auto bytesPerPixel = static_cast<std::size_t>(texture->BytesPerPixel);
    const std::size_t alphaByte = texture->Format == ImTextureFormat_RGBA32 ? 3U : 0U;
    const auto pitch = static_cast<std::size_t>(texture->Width) * bytesPerPixel;
    int first = -1;
    int last = -1;
    for (int column = 0; column < rect.w; ++column) {
        for (int row = 0; row < rect.h; ++row) {
            const std::size_t at = (static_cast<std::size_t>(rect.y + row) * pitch) +
                                   (static_cast<std::size_t>(rect.x + column) * bytesPerPixel) + alphaByte;
            if (texture->Pixels[at] != 0U) {
                first = first < 0 ? column : first;
                last = column;
                break;
            }
        }
    }
    if (first < 0) {
        return {};
    }
    const float columnWidth = (x1 - x0) / static_cast<float>(rect.w);
    return GlyphInk{.left = x0 + (static_cast<float>(first) * columnWidth),
                    .width = static_cast<float>(last + 1 - first) * columnWidth,
                    .box = x1 - x0};
}

// One dash run along an edge: whole dash + gap steps from the edge's start (an INTEGER count -- E.1.2's
// float-lattice trap), each dash a `thickness`-wide rect on the edge's inner side.
void dashRun(ImDrawList* list, ImVec2 start, float length, bool horizontal, float thickness, ImU32 color) {
    const float s = currentUiScale();
    const float dash = EDITOR_THEME.panels.dashLength * s;
    const float step = dash + (EDITOR_THEME.panels.dashGap * s);
    if (!std::isfinite(length) || !(length > 0.0F) || !(step > 0.0F)) {
        return;
    }
    const auto count = static_cast<int>(std::ceil(length / step));  // modernize-use-auto: a bare cast
    for (int k = 0; k < count; ++k) {
        const float from = static_cast<float>(k) * step;
        const float to = std::min(from + dash, length);
        const ImVec2 a = horizontal ? ImVec2(start.x + from, start.y) : ImVec2(start.x, start.y + from);
        const ImVec2 b =
            horizontal ? ImVec2(start.x + to, start.y + thickness) : ImVec2(start.x + thickness, start.y + to);
        list->AddRectFilled(a, b, color);
    }
}

}  // namespace

// ---- faces ---------------------------------------------------------------------------------------------------

ImFont* panelFaceFont(PanelFace face) noexcept {
    const EditorFonts& fonts = editorFonts();
    switch (face) {
        case PanelFace::Body:
        case PanelFace::BodySmall:
            return fonts.body;
        case PanelFace::Strong:
        case PanelFace::StrongSmall:
        case PanelFace::StrongTitle:
            return fonts.strong;
        case PanelFace::Mono:
        case PanelFace::MonoSmall:
            return fonts.mono;
    }
    return fonts.body;
}

float panelFaceSizeDp(PanelFace face) noexcept {
    const ThemeTypeScale& type = EDITOR_THEME.type;
    switch (face) {
        case PanelFace::Body:
            return type.bodySize;
        case PanelFace::Strong:
            return type.strongSize;
        case PanelFace::Mono:
            return type.monoSize;
        case PanelFace::BodySmall:
        case PanelFace::StrongSmall:
        case PanelFace::MonoSmall:
            return type.smallSize;
        case PanelFace::StrongTitle:
            return type.titleSize;
    }
    return type.bodySize;
}

float panelFacePoints(PanelFace face) { return faceSizePoints(panelFaceSizeDp(face), currentUiScale()); }

float faceTextWidth(PanelFace face, std::string_view text) {
    ImFont* const font = panelFaceFont(face);
    if (font == nullptr || text.empty()) {
        return 0.0F;
    }
    return font->CalcTextSizeA(panelFacePoints(face), FLT_MAX, 0.0F, text.data(), text.data() + text.size()).x;
}

ScopedFace::ScopedFace(PanelFace face) { ImGui::PushFont(panelFaceFont(face), panelFaceSizeDp(face)); }
ScopedFace::~ScopedFace() { ImGui::PopFont(); }

// ---- colours -------------------------------------------------------------------------------------------------

ImVec4 roleVec4(ChromeRole role) noexcept { return toImVec4(chromeRoleColor(role)); }
ImU32 roleU32(ChromeRole role) noexcept { return toImU32(chromeRoleColor(role)); }
ImVec4 clearOver(ChromeRole surface) noexcept { return toImVec4(chromeRoleColor(surface, 0U)); }

// ---- text ----------------------------------------------------------------------------------------------------

void roleText(ChromeRole role, std::string_view text) {
    const char* const begin = text.empty() ? "" : text.data();
    ImGui::PushStyleColor(ImGuiCol_Text, roleVec4(role));
    ImGui::TextUnformatted(begin, begin + text.size());
    ImGui::PopStyleColor();
}

void mutedText(std::string_view text) { roleText(ChromeRole::TextMuted, text); }

void mutedTextWrapped(std::string_view text) {
    ImGui::PushTextWrapPos(0.0F);  // 0: wrap at the content region's edge
    roleText(ChromeRole::TextMuted, text);
    ImGui::PopTextWrapPos();
}

void countBadge(std::string_view text) {
    const ScopedFace face(PanelFace::MonoSmall);
    roleText(ChromeRole::TextFaint, text);
}

void drawRoleText(ImDrawList* list, PanelFace face, ImVec2 pos, ChromeRole role, std::string_view text) {
    if (text.empty()) {
        return;
    }
    list->AddText(panelFaceFont(face), panelFacePoints(face), pos, roleU32(role), text.data(),
                  text.data() + text.size());
}

// ---- buttons and scopes ----------------------------------------------------------------------------------------

GhostStyle::GhostStyle() {
    ImGui::PushStyleColor(ImGuiCol_Button, clearOver(ChromeRole::Panel));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, roleVec4(ChromeRole::Hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, roleVec4(ChromeRole::Active));
    ImGui::PushStyleColor(ImGuiCol_Border, roleVec4(ChromeRole::Active));
    ImGui::PushStyleColor(ImGuiCol_Text, roleVec4(ChromeRole::TextSecondary));
}
GhostStyle::~GhostStyle() { ImGui::PopStyleColor(5); }

bool ghostButton(const char* label, ImVec2 size) {
    const GhostStyle ghost;
    return ImGui::Button(label, size);
}

ClearHeaderStyle::ClearHeaderStyle(ChromeRole surface) {
    ImGui::PushStyleColor(ImGuiCol_Header, clearOver(surface));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, clearOver(surface));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, clearOver(surface));
}
ClearHeaderStyle::~ClearHeaderStyle() { ImGui::PopStyleColor(3); }

ClearFrameStyle::ClearFrameStyle(ChromeRole surface) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, clearOver(surface));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, clearOver(surface));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, clearOver(surface));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
}
ClearFrameStyle::~ClearFrameStyle() {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

// Accent on all three button slots and onAccent on the text, so hover and press keep the pair (TH13): onAccent on
// `hover` or `active` is unreadable (1.34:1, 1.49:1).
bool primaryButton(const char* label, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    const ImVec4 accentFill = roleVec4(ChromeRole::Accent);
    ImGui::PushStyleColor(ImGuiCol_Button, accentFill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accentFill);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, accentFill);
    ImGui::PushStyleColor(ImGuiCol_Text, roleVec4(ChromeRole::OnAccent));
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    return pressed;
}

// E.6.2's groupButton (D19 (a)-(d), D1), hoisted: one button INSIDE a `raised` group, no border of its own,
// ImGuiItemFlags_NoFocus so a click leaves the keyboard where it was (imgui_widgets.cpp:553-554), and ACTIVE
// pushing all three button slots with the text slot.
bool segmentButton(const char* label, bool active, ChromeRole activeFill, ChromeRole activeText, ChromeRole idleText,
                   ItemRectRecord& record) {
    if (active) {
        const ImVec4 fill = roleVec4(activeFill);
        ImGui::PushStyleColor(ImGuiCol_Button, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, fill);
        ImGui::PushStyleColor(ImGuiCol_Text, roleVec4(activeText));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, roleVec4(idleText));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
    ImGui::PushItemFlag(ImGuiItemFlags_NoFocus, true);
    const bool pressed = ImGui::Button(label);
    ImGui::PopItemFlag();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(active ? 4 : 1);
    recordItemRect(record, active);
    return pressed;
}

// A group's `raised` body and its 1-dp `border`, drawn BEHIND its buttons (submission order is draw order).
void segmentGroupBackground(ImDrawList* list, ImVec2 min, ImVec2 max) {
    const ImGuiStyle& style = ImGui::GetStyle();
    list->AddRectFilled(min, max, roleU32(ChromeRole::Raised), style.FrameRounding);
    list->AddRect(min, max, roleU32(ChromeRole::Border), style.FrameRounding, 0, style.FrameBorderSize);
}

// ---- chips, fills, outlines, marks -----------------------------------------------------------------------------

ImVec2 chipSize(std::string_view label, std::string_view trailingIcon) {
    const float s = currentUiScale();
    float width = faceTextWidth(PanelFace::BodySmall, label) + (2.0F * EDITOR_THEME.panels.chipPaddingX * s);
    if (!trailingIcon.empty()) {
        width += (EDITOR_THEME.metrics.itemInnerSpacing.x * s) + faceTextWidth(PanelFace::BodySmall, trailingIcon);
    }
    return ImVec2(width, chipHeight(s));
}

ChipDraw chipButton(const char* strId, std::string_view label, std::string_view trailingIcon) {
    ChipDraw out{};
    out.pressed = ImGui::InvisibleButton(strId, chipSize(label, trailingIcon));
    out.hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    out.min = ImGui::GetItemRectMin();
    out.max = ImGui::GetItemRectMax();
    ImDrawList* const list = ImGui::GetWindowDrawList();
    const ChromeRole fill = held ? ChromeRole::Active : (out.hovered ? ChromeRole::Hover : ChromeRole::Canvas);
    chipFrame(list, out.min, out.max, fill);
    const float s = currentUiScale();
    const float points = panelFacePoints(PanelFace::BodySmall);
    const float textY = out.min.y + ((out.max.y - out.min.y - points) * 0.5F);
    const float textX = out.min.x + (EDITOR_THEME.panels.chipPaddingX * s);
    drawRoleText(list, PanelFace::BodySmall, ImVec2(textX, textY), ChromeRole::TextSecondary, label);
    if (!trailingIcon.empty()) {
        const float labelWidth = faceTextWidth(PanelFace::BodySmall, label);
        const float iconX = textX + labelWidth + (EDITOR_THEME.metrics.itemInnerSpacing.x * s);
        drawRoleText(list, PanelFace::BodySmall, ImVec2(iconX, textY), ChromeRole::TextFaint, trailingIcon);
    }
    out.font = panelFaceFont(PanelFace::BodySmall);
    out.fontSize = points;
    return out;
}

void chipFrame(ImDrawList* list, ImVec2 min, ImVec2 max, ChromeRole fill) {
    const float rounding = EDITOR_THEME.metrics.frameRounding * currentUiScale();
    list->AddRectFilled(min, max, roleU32(fill), rounding);
    list->AddRect(min, max, roleU32(ChromeRole::Divider), rounding, 0, ruleHeight());
}

void roleFill(ImDrawList* list, ImVec2 min, ImVec2 max, ChromeRole role, float rounding) {
    list->AddRectFilled(min, max, roleU32(role), rounding);
}

void roleOutline(ImDrawList* list, ImVec2 min, ImVec2 max, ChromeRole role, float rounding, float thickness) {
    list->AddRect(min, max, roleU32(role), rounding, 0, thickness);
}

void roleDot(ImDrawList* list, ImVec2 center, float diameter, ChromeRole role) {
    list->AddCircleFilled(center, diameter * 0.5F, roleU32(role));
}

// The tile fill under 45-degree hatchStripe bands rising to the right: a band that starts at x on the bottom edge
// ends at x + height on the top. Clipped to the well; an INTEGER band count (E.1.2's float-lattice trap).
void hatchedWell(ImDrawList* list, ImVec2 min, ImVec2 max, float rounding) {
    list->AddRectFilled(min, max, roleU32(ChromeRole::Tile), rounding);
    const float s = currentUiScale();
    const float period = EDITOR_THEME.panels.hatchPeriod * s;
    const float stripe = EDITOR_THEME.panels.hatchStripeWidth * s;
    const float height = max.y - min.y;
    const float width = max.x - min.x;
    if (!(period > 0.0F) || !(height > 0.0F) || !(width > 0.0F) || !std::isfinite(width + height)) {
        return;
    }
    const ImU32 color = toImU32(EDITOR_THEME.panels.hatchStripe);
    const int bands = static_cast<int>(std::ceil((width + height) / period)) + 1;
    list->PushClipRect(min, max, true);
    for (int k = 0; k < bands; ++k) {
        const float x0 = min.x - height + (static_cast<float>(k) * period);
        list->AddQuadFilled(ImVec2(x0 + height, min.y), ImVec2(x0 + stripe + height, min.y), ImVec2(x0 + stripe, max.y),
                            ImVec2(x0, max.y), color);  // clockwise (imgui.h:3497)
    }
    list->PopClipRect();
}

void dashedRect(ImDrawList* list, ImVec2 min, ImVec2 max) {
    const float thickness = ruleHeight();
    const ImU32 color = toImU32(EDITOR_THEME.panels.addComponentDash);
    const float width = max.x - min.x;
    const float height = max.y - min.y;
    dashRun(list, min, width, true, thickness, color);                                 // top
    dashRun(list, ImVec2(min.x, max.y - thickness), width, true, thickness, color);    // bottom
    dashRun(list, min, height, false, thickness, color);                               // left
    dashRun(list, ImVec2(max.x - thickness, min.y), height, false, thickness, color);  // right
}

void horizontalRule(ImDrawList* list, float minX, float maxX, float y, ChromeRole role) {
    list->AddRectFilled(ImVec2(minX, y), ImVec2(maxX, y + ruleHeight()), roleU32(role));
}

float ruleHeight() {
    return hairline(EDITOR_THEME.shell.ruleThickness, currentUiScale(), ImGui::GetIO().DisplayFramebufferScale.y);
}

float tileRingHeight() {
    return hairline(EDITOR_THEME.panels.tileRing, currentUiScale(), ImGui::GetIO().DisplayFramebufferScale.y);
}

// ---- the card (D4, spec section 6.4) ----------------------------------------------------------------------------

CardHeaderResult CardPainter::begin(const char* strId, const CardSpec& spec, bool* panelOwnedOpen, CardRecord* record) {
    IM_ASSERT(!active && "CardPainter: a card is never nested");
    active = true;
    recordPtr = record;
    entry = CardEntryRecord{};
    const float s = currentUiScale();
    geometry = cardGeometry(s);
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    list = window->DrawList;
    // ONE x span, read once (D4): the strip, the body, the rule and the outline are all drawn from it.
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    spanMinX = cursor.x;
    spanMaxX = cursor.x + ImGui::GetContentRegionAvail().x;
    topY = cursor.y;
    if (record != nullptr && !record->cards.empty()) {
        record->cards.back().gapAfter = topY - record->cards.back().outerMax.y;  // the previous card's DRAWN gap
    }
    splitter.Split(list, 2);
    splitter.SetCurrentChannel(list, 1);

    // ---- the header ITEM: ImGui answers hover, held and open -- and draws nothing ---------------------------
    if (panelOwnedOpen != nullptr) {
        ImGui::SetNextItemOpen(*panelOwnedOpen, ImGuiCond_Always);
    }
    ImGui::PushStyleColor(ImGuiCol_Header, clearOver(ChromeRole::RaisedHeader));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, clearOver(ChromeRole::RaisedHeader));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, clearOver(ChromeRole::RaisedHeader));
    ImGui::PushStyleColor(ImGuiCol_Text, clearOver(ChromeRole::TextFaint));  // ImGui's own arrow, drawn invisibly
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(geometry.headerPaddingX, geometry.headerPaddingY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
    open = ImGui::TreeNodeEx(strId, CARD_HEADER_FLAGS, "%s", "");
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    CardHeaderResult result{};
    result.open = open;
    result.headerHovered = ImGui::IsItemHovered();  // BEFORE the overflow, which becomes the last item
    // The release rule BeginPopupContextItem uses (imgui.cpp:13280-13285), never IsItemClicked(Right), which an
    // open popup blocks (imgui.cpp:4909-4929): a right-click on a second header closes the first menu on the press
    // and opens this one on the release.
    result.headerRightClicked = ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
                                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    if (panelOwnedOpen != nullptr && ImGui::IsItemToggledOpen()) {
        *panelOwnedOpen = !*panelOwnedOpen;  // the panel owns the bit; the header is the item whose toggle flips it
    }
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    stripBottomY = itemMax.y;

    // ---- the overflow: a real item, so AllowOverlap lets it take the press over the header ------------------
    const float iconAdvance = faceTextWidth(PanelFace::Body, spec.icon);
    const CardHeaderLayout layout = cardHeaderLayout(spanMinX, spanMaxX, iconAdvance, geometry.overflowWidth, s);
    if (spec.overflow) {
        const ImVec2 overflowSize(geometry.overflowWidth, geometry.overflowWidth);
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(layout.overflowX, topY + ((stripBottomY - topY - overflowSize.y) * 0.5F)));
        ImGui::PushStyleColor(ImGuiCol_Button, clearOver(ChromeRole::RaisedHeader));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, clearOver(ChromeRole::RaisedHeader));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, clearOver(ChromeRole::RaisedHeader));
        ImGui::PushStyleColor(ImGuiCol_Text, roleVec4(ChromeRole::TextFaint));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
        result.overflowPressed = ImGui::Button(AERO_ICON_ELLIPSIS "##more", overflowSize);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        entry.overflowDrawn = true;
    }

    // ---- the strip (channel 0) and its chevron, icon and title (channel 1), from the RECORDED span ----------
    const float rule = ruleHeight();
    splitter.SetCurrentChannel(list, 0);
    const ChromeRole stripRole =
        result.headerHovered ? ChromeRole::Hover : (open ? ChromeRole::RaisedHeader : ChromeRole::Raised);
    // Open: the strip stops one rule above its bottom edge, where end() draws the header/body rule.
    list->AddRectFilled(ImVec2(spanMinX, topY), ImVec2(spanMaxX, open ? stripBottomY - rule : stripBottomY),
                        roleU32(stripRole), geometry.rounding,
                        open ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll);
    splitter.SetCurrentChannel(list, 1);
    ImFont* const body = panelFaceFont(PanelFace::Body);
    const float bodyPoints = panelFacePoints(PanelFace::Body);
    const std::string_view chevron =
        open ? std::string_view(AERO_ICON_CHEVRON_DOWN) : std::string_view(AERO_ICON_CHEVRON_RIGHT);
    const GlyphInk inkAtBody = glyphInk(body, bodyPoints, chevron);
    const float chevronSize = chevronFaceSize(bodyPoints, inkAtBody.width, s);
    // The pen sits the ink's left offset (scaled to the drawn size) left of chevronX, so the INK -- not the box --
    // starts there.
    const float penX = layout.chevronX - (inkAtBody.left * chevronSize / bodyPoints);
    const float midY = (topY + stripBottomY) * 0.5F;
    list->AddText(body, chevronSize, ImVec2(penX, midY - (chevronSize * 0.5F)), roleU32(ChromeRole::TextFaint),
                  chevron.data(), chevron.data() + chevron.size());
    // Re-measured at the size AddText was handed -- a second bake, not the body-size reading above -- so I336 sees
    // the ink that was DRAWN.
    const GlyphInk inkDrawn = glyphInk(body, chevronSize, chevron);
    const float textY = midY - (bodyPoints * 0.5F);
    const ChromeRole iconRole = open ? ChromeRole::Accent : ChromeRole::TextLabel;
    drawRoleText(list, PanelFace::Body, ImVec2(layout.iconX, textY), iconRole, spec.icon);
    const float titleMaxX = spec.overflow ? layout.overflowX : spanMaxX - geometry.headerPaddingX;
    list->PushClipRect(ImVec2(layout.titleX, topY), ImVec2(std::max(layout.titleX, titleMaxX), stripBottomY), true);
    drawRoleText(list, PanelFace::Strong, ImVec2(layout.titleX, textY),
                 open ? ChromeRole::TextBright : ChromeRole::TextSecondary, spec.title);
    list->PopClipRect();

    entry.stripMinX = spanMinX;
    entry.stripMaxX = spanMaxX;
    entry.stripBottomY = stripBottomY;
    entry.headerItemMin = toVec2(itemMin);
    entry.headerItemMax = toVec2(itemMax);
    entry.chevronX = layout.chevronX;
    entry.iconX = layout.iconX;
    entry.titleX = layout.titleX;
    entry.iconAdvance = iconAdvance;
    entry.overflowWidth = geometry.overflowWidth;
    entry.chevronSize = chevronSize;
    entry.chevronInkMinX = penX + inkDrawn.left;
    entry.chevronInkWidth = inkDrawn.width;
    entry.chevronBoxWidth = inkDrawn.box;
    entry.open = open;
    if (record != nullptr) {
        const ImFont* const strong = panelFaceFont(PanelFace::Strong);
        record->titleFontName = strong != nullptr ? strong->GetDebugName() : "";
        record->titleFontSize = panelFacePoints(PanelFace::Strong);
        record->scaleAtDraw = s;
    }

    // ---- the body: placed paddingY below the strip, narrowed on BOTH edges (D4) ----------------------------
    if (open) {
        ImGui::SetCursorScreenPos(ImVec2(spanMinX, stripBottomY + geometry.paddingY));  // an item always follows
        ImGui::Indent(geometry.paddingX);
        savedWorkMinX = window->WorkRect.Min.x;
        savedContentMaxX = window->ContentRegionRect.Max.x;
        savedWorkMaxX = window->WorkRect.Max.x;
        window->WorkRect.Min.x = window->DC.CursorPos.x;
        window->ContentRegionRect.Max.x -= geometry.paddingX;
        window->WorkRect.Max.x -= geometry.paddingX;
        entry.bodyMin = Vec2{window->DC.CursorPos.x, stripBottomY};
        entry.topPadding = window->DC.CursorPos.y - stripBottomY;  // where ImGui places the first body item
    }
    return result;
}

void CardPainter::end() {
    IM_ASSERT(active && "CardPainter: end() without begin()");
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    float bottomY = stripBottomY;
    if (open) {
        const float lastItemBottom = ImGui::GetItemRectMax().y;  // the last BODY item (a table's is its OuterRect)
        bottomY = lastItemBottom + geometry.paddingY;            // never the raw cursor after a spacing item
        entry.bottomPadding = bottomY - lastItemBottom;
        entry.bodyMax = Vec2{window->ContentRegionRect.Max.x, bottomY};
        window->WorkRect.Min.x = savedWorkMinX;
        window->ContentRegionRect.Max.x = savedContentMaxX;
        window->WorkRect.Max.x = savedWorkMaxX;
        ImGui::Unindent(geometry.paddingX);
    }
    const float rule = ruleHeight();
    splitter.SetCurrentChannel(list, 0);
    if (open) {
        list->AddRectFilled(ImVec2(spanMinX, stripBottomY), ImVec2(spanMaxX, bottomY), roleU32(ChromeRole::Raised),
                            geometry.rounding, ImDrawFlags_RoundCornersBottom);
    }
    splitter.SetCurrentChannel(list, 1);
    if (open) {
        // The mock's header/body rule, on the strip's bottom row. A closed card draws none: its outline is the edge.
        // The record reads what the list RECEIVED (the channels share one VtxBuffer; AddRectFilled adds no vertex at
        // alpha 0, imgui_draw.cpp:1532), never a flag set beside the call.
        const int ruleVertices = list->VtxBuffer.Size;
        list->AddRectFilled(ImVec2(spanMinX, stripBottomY - rule), ImVec2(spanMaxX, stripBottomY),
                            roleU32(ChromeRole::Border));
        entry.headerRuleDrawn = list->VtxBuffer.Size > ruleVertices;
    }
    list->AddRect(ImVec2(spanMinX, topY), ImVec2(spanMaxX, bottomY), roleU32(ChromeRole::Border), geometry.rounding, 0,
                  rule);
    splitter.Merge(list);
    // The next card's top lands exactly `gap` below this card's bottom: ONE Dummy of the NET height -- ItemSize adds
    // ItemSpacing.y after it (imgui.cpp:12129) -- never a bare SetCursorScreenPos (imgui.cpp:8839-8840).
    const float cursorY = ImGui::GetCursorScreenPos().y;
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    ImGui::Dummy(ImVec2(0.0F, std::max(0.0F, bottomY + geometry.gap - cursorY - spacing)));
    entry.outerMin = Vec2{spanMinX, topY};
    entry.outerMax = Vec2{spanMaxX, bottomY};
    if (recordPtr != nullptr) {
        recordPtr->cards.push_back(entry);
        ++recordPtr->cardsDrawn;
        recordPtr->bodiesSubmitted += open ? 1U : 0U;
    }
    active = false;
    open = false;
    list = nullptr;
    recordPtr = nullptr;
}

PanelCard::PanelCard(CardPainter& painter, const char* strId, const CardSpec& spec, bool* panelOwnedOpen,
                     CardRecord* record)
    : painterPtr(&painter), result(painter.begin(strId, spec, panelOwnedOpen, record)) {}

// The no-throw contract (docs/04), enforced rather than documented -- the jobs.cpp / ~Device precedent: end()'s one
// throwing call is the record's push_back (std::bad_alloc), and no exception may unwind past a destructor. An explicit
// std::terminate(), never a rethrow (bugprone-exception-escape).
PanelCard::~PanelCard() {
    try {
        painterPtr->end();
    } catch (...) {
        std::terminate();
    }
}

}  // namespace engine::editor

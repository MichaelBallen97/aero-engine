// Aero Engine — the Welcome window and the New Project modal (task 2.6.1; restyled by task E.6.4). Its own TU because
// shell_ui.cpp would pass 550 lines otherwise. Every control here sets exactly ONE request field and nothing else
// (AC-46) -- neither window mutates the World, the ProjectSession or the recents list, and neither reads the disk:
// both route through FileAction / the form's *Requested fields, consumed OUTSIDE the draw walk in applyFileRequests,
// whose step 0e also keeps the form's probe (task E.6.4, D12). Every decision drawn here is project_ui_view.hpp's;
// this TU measures, draws and records what ImGui did.
#include "project_ui.hpp"

#include <aero/editor/editor_icons.hpp>
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/project.hpp>
#include <aero/editor/project_ui_view.hpp>
#include <aero/editor/status_bar.hpp>  // abbreviateHome

#include "editor_theme_imgui.hpp"  // toImVec4
#include "editor_theme_ui.hpp"     // currentUiScale
#include "panel_chrome_ui.hpp"     // faces, roles, the ghost and primary buttons, fills, rules
#include "text_input.hpp"          // 2.2.2's inputTextString helper

#include <algorithm>
#include <array>
#include <cfloat>  // FLT_MAX: the unbounded line width bodyWrappedHeight hands CalcTextSizeA
#include <cstddef>
#include <imgui.h>
#include <imgui_internal.h>  // task E.6.4 (D27): GetInputTextState + ImGuiInputTextState -- the input seam's reload
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

namespace {

constexpr const char* WELCOME_WINDOW_ID = "Welcome to Aero Editor###aero_welcome";
// task E.6.4 (D5, D7, D21): no title bar (the header strip is drawn), never moved (it is re-centred every frame
// instead), never saved to aero_editor.ini (AC-29), never docked (it is not a panel), and never focused when it
// appears -- the I256 family runs with it up and the keyboard on the Hierarchy. Drawn IFF no project is open, so it
// needs no close button and no visibility state.
constexpr ImGuiWindowFlags WELCOME_FLAGS = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                           ImGuiWindowFlags_NoFocusOnAppearing;

constexpr const char* NEW_PROJECT_MODAL_ID = "New Project###aero_new_project";
// The ### form makes the ID stable even though the visible label never changes -- ImHashStr restarts the hash at
// "###", so IsPopupOpen and BeginPopupModal agree by construction (the UNSAVED_MODAL_ID precedent).
// task E.6.4 (D21): BeginPopupModal adds Popup | Modal | NoCollapse | NoDocking and strips nothing (imgui.cpp:13232).
// NoSavedSettings because the modal used to reopen at its saved corner ([Window][aero_new_project] in the ini);
// NoMove because it is re-centred every frame. No NoScrollbar: under a 523-point work area the dialog is clamped and
// scrolls whole (D34) -- vertically only (drawNewProjectModal pins x).
constexpr ImGuiWindowFlags NEW_PROJECT_FLAGS = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

[[nodiscard]] Vec2 toVec2(ImVec2 v) noexcept { return Vec2{v.x, v.y}; }
[[nodiscard]] ImVec2 toImVec2(Vec2 v) noexcept { return ImVec2{v.x, v.y}; }

// D23: THE one read of the palette's dialog token -- both windows' body. A direct read, never a ChromeRole (PC2 asserts
// no role reaches these bytes); I287 counts it.
[[nodiscard]] ImVec4 dialogFill() noexcept { return toImVec4(EDITOR_THEME.palette.dialog); }

// G9: under BeginDisabled a draw-list colour fades only through GetColorU32 (imgui.cpp:3661-3669); roleU32 is raw.
[[nodiscard]] ImU32 fadedU32(ChromeRole role) { return ImGui::GetColorU32(roleU32(role)); }

void drawFadedText(ImDrawList* list, PanelFace face, ImVec2 pos, ChromeRole role, std::string_view text) {
    if (!text.empty()) {
        list->AddText(panelFaceFont(face), panelFacePoints(face), pos, fadedU32(role), text.data(),
                      text.data() + text.size());
    }
}

// What ImGui drew for the last item: its rect, and its Disabled flag as ImGui recorded it -- never a predicate.
void recordItem(ItemRectRecord& record, bool drawnActive) {
    record.min = toVec2(ImGui::GetItemRectMin());
    record.max = toVec2(ImGui::GetItemRectMax());
    record.drawnActive = drawnActive;
    record.enabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) == 0;
}

// The editor's per-item tooltip idiom (shell_ui.cpp's ioTooltip): at once, disabled or not. "%s": never a format.
void itemTooltip(std::string_view text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", std::string(text).c_str());
    }
}

[[nodiscard]] float monoSmallWidth(std::string_view text) { return faceTextWidth(PanelFace::MonoSmall, text); }

// D34: a Body text's height as TextEx will wrap it at `wrapWidth` -- ImFont::CalcTextSizeA with that wrap width, the
// call ImGui::CalcTextSize makes for TextEx (imgui.cpp:6449), and faceTextWidth's twin (panel_chrome_ui.cpp). A
// missing face answers 0, which welcomeLayout counts as one line.
[[nodiscard]] float bodyWrappedHeight(std::string_view text, float wrapWidth) {
    ImFont* const font = panelFaceFont(PanelFace::Body);
    if (font == nullptr || text.empty()) {
        return 0.0F;
    }
    const char* const end = text.data() + text.size();
    return font->CalcTextSizeA(panelFacePoints(PanelFace::Body), FLT_MAX, wrapWidth, text.data(), end).y;
}

void gap(float height) { ImGui::Dummy(ImVec2(0.0F, height)); }  // ItemSpacing is 0: every gap is stated (D5)

// D8, D15: the one accent button both windows draw -- New Project... and Create project. Its label in Strong, and no
// outline: primaryButton pushes no FrameBorderSize, so under the theme's 1-dp frameBorderSize the accent fill would be
// ringed in `border`; the mock's Create is borderless.
bool accentButton(const char* label, bool enabled) {
    const ScopedFace strong(PanelFace::Strong);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
    const bool pressed = primaryButton(label, enabled);
    ImGui::PopStyleVar();
    return pressed;
}

// D5: what a first-contact window takes BEFORE its Begin -- its fill slot, the outline, the corner and no padding --
// popped by the destructor on the statement after Begin, whatever Begin returned (a modal reads all four inside
// Begin, imgui.cpp:7541/7480/8312/8126), so a tooltip inside keeps the theme's own popup look.
class DialogFrameStyle {
public:
    DialogFrameStyle(ImGuiCol fillSlot, float rounding) {
        ImGui::PushStyleColor(fillSlot, dialogFill());
        ImGui::PushStyleColor(ImGuiCol_Border, roleVec4(ChromeRole::Active));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, rounding);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    }
    ~DialogFrameStyle() {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }
    DialogFrameStyle(const DialogFrameStyle&) = delete;
    DialogFrameStyle& operator=(const DialogFrameStyle&) = delete;
    DialogFrameStyle(DialogFrameStyle&&) = delete;
    DialogFrameStyle& operator=(DialogFrameStyle&&) = delete;
};

// D5/D15: the body's own two -- no item spacing (every gap is a ThemeDialogs Dummy) and the controls' corner.
class DialogBodyStyle {
public:
    explicit DialogBodyStyle(float controlRounding) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0F, 0.0F));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, controlRounding);
    }
    ~DialogBodyStyle() { ImGui::PopStyleVar(2); }
    DialogBodyStyle(const DialogBodyStyle&) = delete;
    DialogBodyStyle& operator=(const DialogBodyStyle&) = delete;
    DialogBodyStyle(DialogBodyStyle&&) = delete;
    DialogBodyStyle& operator=(DialogBodyStyle&&) = delete;
};

class FramePaddingScope {  // D15: a field's or a button's padding, around its widgets only
public:
    explicit FramePaddingScope(Vec2 padding) { ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, toImVec2(padding)); }
    ~FramePaddingScope() { ImGui::PopStyleVar(); }
    FramePaddingScope(const FramePaddingScope&) = delete;
    FramePaddingScope& operator=(const FramePaddingScope&) = delete;
    FramePaddingScope(FramePaddingScope&&) = delete;
    FramePaddingScope& operator=(FramePaddingScope&&) = delete;
};

// D5, D26, D27: the shared header strip, from the window's own origin (no padding, no title bar). Only the close box is
// an item; the band, its rule, the mark and the texts are draw-list work. The band is the outer border's row, the 52-dp
// strip and its rule, so the fill starts one border in on three sides and the outline the window drew inside Begin is
// never painted over. `record` is written HERE, where each thing is drawn: the title, the version only inside its draw
// branch, the close box only when it is drawn. Returns the close box's press; leaves the cursor on the band's next row.
bool drawDialogHeader(float width, float windowRounding, float controlRounding, std::string_view title,
                      std::string_view version, bool closeButton, float uiScale, DialogHeaderRecord* record) {
    ImDrawList* const list = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const DialogHeaderLayout header = dialogHeaderLayout(width, closeButton, uiScale);
    // Reserves the band's HEIGHT only. A full-width item makes the content as wide as the window, and a clamped window
    // (D34) draws a vertical scrollbar that narrows its inner rect, so ScrollMax.x would be ScrollbarSize
    // (imgui.cpp:8478) -- which a horizontal wheel scrolls whatever the window's flags (:11053).
    ImGui::Dummy(ImVec2(0.0F, header.height));
    const float border = ImGui::GetStyle().WindowBorderSize;
    list->AddRectFilled(ImVec2(origin.x + border, origin.y + border),
                        ImVec2(origin.x + width - border, origin.y + header.height), roleU32(ChromeRole::Panel),
                        std::max(0.0F, windowRounding - border), ImDrawFlags_RoundCornersTop);
    horizontalRule(list, origin.x + border, origin.x + width - border, origin.y + header.height - ruleHeight(),
                   ChromeRole::Divider);
    const std::array<Vec2, 4> mark = aeroMarkPoints(header.markCenter + toVec2(origin), uiScale);
    const std::array<ImVec2, 4> corners{toImVec2(mark[0]), toImVec2(mark[1]), toImVec2(mark[2]), toImVec2(mark[3])};
    list->AddPolyline(corners.data(), static_cast<int>(corners.size()), roleU32(ChromeRole::Accent), ImDrawFlags_Closed,
                      ruleHeight());
    const float middle = origin.y + header.centerY;
    drawRoleText(list, PanelFace::StrongTitle,
                 ImVec2(origin.x + header.titleX, middle - (panelFacePoints(PanelFace::StrongTitle) * 0.5F)),
                 ChromeRole::TextBright, title);
    if (record != nullptr) {
        record->title = std::string(title);
    }
    if (!version.empty()) {
        drawRoleText(list, PanelFace::MonoSmall,
                     ImVec2(origin.x + header.versionRight - monoSmallWidth(version),
                            middle - (panelFacePoints(PanelFace::MonoSmall) * 0.5F)),
                     ChromeRole::TextFaint, version);
        if (record != nullptr) {
            record->version = std::string(version);
        }
    }
    bool pressed = false;
    if (header.closeDrawn) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + header.closeMin.x, origin.y + header.closeMin.y));
        const ImVec2 closeSize(header.closeMax.x - header.closeMin.x, header.closeMax.y - header.closeMin.y);
        pressed = ImGui::InvisibleButton("##close", closeSize);  // a literal "##" id on the call's own line (I358)
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        if (hovered) {
            roleFill(list, min, max, ChromeRole::Hover, controlRounding);
        }
        drawRoleText(list, PanelFace::Body,
                     ImVec2((min.x + max.x - faceTextWidth(PanelFace::Body, AERO_ICON_X)) * 0.5F,
                            (min.y + max.y - panelFacePoints(PanelFace::Body)) * 0.5F),
                     hovered ? ChromeRole::TextBright : ChromeRole::TextMuted, AERO_ICON_X);
        if (record != nullptr) {
            recordItem(record->close, false);
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + header.height));  // back below the band
    }
    return pressed;
}

// ---- the Welcome window (D6-D9) ------------------------------------------------------------------------------------

// D6, D23, D34: the well -- a plain `border` outline (never dashedRect, whose accent-tinted dash marks an add target),
// at the height the layout folded it to, its three lines centred as one block of the layout's emptyStateBlock.
void drawEmptyState(ImDrawList* list, const WelcomeLayout& layout) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(layout.contentWidth, layout.emptyStateHeight));
    const ImVec2 max(min.x + layout.contentWidth, min.y + layout.emptyStateHeight);
    roleOutline(list, min, max, ChromeRole::Border, layout.controlRounding, ruleHeight());
    const float body = panelFacePoints(PanelFace::Body);
    float y = min.y + ((layout.emptyStateHeight - layout.emptyStateBlock) * 0.5F);
    const auto centred = [&](PanelFace face, ChromeRole role, std::string_view text) {
        const float x = min.x + ((layout.contentWidth - faceTextWidth(face, text)) * 0.5F);
        drawRoleText(list, face, ImVec2(x, y), role, text);
    };
    centred(PanelFace::Body, ChromeRole::TextFaint, AERO_ICON_FOLDER_OPEN);
    y += body + layout.labelGap;
    centred(PanelFace::Body, ChromeRole::TextSecondary, WELCOME_EMPTY_TITLE);
    y += body + layout.cardLineGap;
    centred(PanelFace::BodySmall, ChromeRole::TextMuted, WELCOME_EMPTY_BODY);
}

void drawRecentCards(FileMenuContext& fileMenu, const WelcomeLayout& layout, WelcomeRecord* record) {
    const std::vector<std::string>& paths = fileMenu.project.recents.paths;
    // ONE child for the whole list, so a long list scrolls (D6); walked only while BeginChild returned true, EndChild
    // unconditional (E.6.3's rule). Its padding is latched inside BeginChild, so the zero is pushed around that call.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
    const bool listOpen = ImGui::BeginChild("##recents", ImVec2(layout.contentWidth, layout.listHeight));
    ImGui::PopStyleVar();
    if (listOpen) {
        if (record != nullptr) {
            record->listHeight = ImGui::GetWindowSize().y;  // the child as ImGui laid it out
        }
        ImDrawList* const list = ImGui::GetWindowDrawList();
        const auto strongWidth = [](std::string_view text) { return faceTextWidth(PanelFace::Strong, text); };
        for (std::size_t i = 0; i < paths.size(); ++i) {
            if (i > 0U) {
                gap(layout.cardGap);
            }
            const std::string& path = paths[i];
            const RecentCardText text = recentCardText(path, fileMenu.homeDirectory);
            ImGui::PushID(static_cast<int>(i));
            // D9: a LITERAL id, never the path -- a label hides everything after a "##" (imgui.cpp:3912-3920).
            const bool pressed =
                ImGui::InvisibleButton("##recent", ImVec2(ImGui::GetContentRegionAvail().x, layout.cardHeight));
            ImGui::PopID();
            const bool hovered = ImGui::IsItemHovered();
            const bool held = ImGui::IsItemActive();
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            roleFill(list, min, max, held ? ChromeRole::Active : (hovered ? ChromeRole::Hover : ChromeRole::Raised),
                     layout.controlRounding);
            roleOutline(list, min, max, ChromeRole::Border, layout.controlRounding, ruleHeight());
            const float textX = min.x + layout.textInset;
            const float budget = max.x - layout.iconInset - textX;
            const float titleY = min.y + layout.cardTitleTop;
            const float pathY = titleY + panelFacePoints(PanelFace::Strong) + layout.cardLineGap;
            drawRoleText(list, PanelFace::Body, ImVec2(min.x + layout.iconInset, titleY), ChromeRole::TextLabel,
                         AERO_ICON_FOLDER);
            const std::string title = fitTextRight(text.title, budget, strongWidth);
            drawRoleText(list, PanelFace::Strong, ImVec2(textX, titleY), ChromeRole::TextBright, title);
            const std::string shown = fitPathLeft(text.path, budget, monoSmallWidth);
            drawRoleText(list, PanelFace::MonoSmall, ImVec2(textX, pathY), ChromeRole::TextMuted, shown);
            itemTooltip(path);  // the whole path, unabbreviated
            if (record != nullptr) {
                record->cards.push_back(RecentCardRecord{title, shown, toVec2(min), toVec2(max)});
            }
            if (pressed) {  // exactly what File > Open Recent writes (shell_ui.cpp:137)
                fileMenu.project.flow.requestedPath = path;
                fileMenu.flow.requested = FileAction::OpenProject;
            }
        }
    }
    ImGui::EndChild();
}

void drawWelcomeBody(FileMenuContext& fileMenu, const WelcomeLayout& layout, float uiScale, WelcomeRecord* record) {
    const DialogBodyStyle body(layout.controlRounding);
    if (record != nullptr) {
        ++record->framesDrawn;
        record->drawnThisFrame = true;
        record->uiScaleAtDraw = uiScale;
        record->windowPos = toVec2(ImGui::GetWindowPos());
        record->windowSize = toVec2(ImGui::GetWindowSize());
        record->workPos = toVec2(ImGui::GetMainViewport()->WorkPos);
        record->workSize = toVec2(ImGui::GetMainViewport()->WorkSize);
        record->scrollMaxX = ImGui::GetScrollMaxX();
        record->scrollbarY = ImGui::GetCurrentWindow()->ScrollbarY;
        record->layout = layout;
    }
    drawDialogHeader(layout.width, layout.windowRounding, layout.controlRounding, WELCOME_TITLE,
                     fileMenu.project.engineVersion, false, uiScale, record != nullptr ? &record->header : nullptr);
    ImGui::Indent(layout.paddingX);
    gap(layout.bodyTop);
    {
        const ScopedFace strong(PanelFace::Strong);
        roleText(ChromeRole::TextBright, WELCOME_HEADLINE);
    }
    // Wrapped at the CONTENT edge (the window has no padding, so a 0 wrap position would run to the outline) -- the
    // width welcomeLayout asked bodyWrappedHeight about, so the fold counted every line this draws (D34).
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + layout.contentWidth);
    mutedText(WELCOME_BLURB);
    ImGui::PopTextWrapPos();
    gap(layout.sectionGap);
    {
        const FramePaddingScope padding(layout.buttonPadding);
        const bool newProjectPressed = accentButton(WELCOME_NEW_PROJECT_LABEL.data(), true);  // D8: the dialog's Create
        if (record != nullptr) {
            recordItem(record->newProject, false);
        }
        if (newProjectPressed) {
            fileMenu.flow.requested = FileAction::NewProject;
        }
        ImGui::SameLine(0.0F, layout.buttonGap);
        const bool openPressed = ghostButton(WELCOME_OPEN_PROJECT_LABEL.data());
        if (record != nullptr) {
            recordItem(record->openProject, false);
        }
        if (openPressed) {
            fileMenu.flow.requested = FileAction::OpenProject;
        }
    }
    gap(layout.sectionGap);
    {
        const ScopedFace heading(PanelFace::StrongSmall);
        roleText(ChromeRole::TextMuted, WELCOME_RECENTS_HEADING);
    }
    gap(layout.labelGap);
    if (fileMenu.project.recents.paths.empty()) {
        drawEmptyState(ImGui::GetWindowDrawList(), layout);
        if (record != nullptr) {
            record->emptyStateDrawn = true;
        }
    } else {
        drawRecentCards(fileMenu, layout, record);
    }
    gap(layout.paddingX);
    ImGui::Unindent(layout.paddingX);
}

// ---- the New Project dialog (D10-D20) --------------------------------------------------------------------------

void drawLanguageCards(ImDrawList* list, const NewProjectDialogLayout& layout, NewProjectRecord* record) {
    const auto strongWidth = [](std::string_view text) { return faceTextWidth(PanelFace::Strong, text); };
    const auto smallWidth = [](std::string_view text) { return faceTextWidth(PanelFace::BodySmall, text); };
    // D18: ONE disabled pair around both cards, whatever the build -- task 4.6.1 owns the choice, and the card drawn
    // selected is the language Create writes (NEW_PROJECT_LANGUAGE).
    ImGui::BeginDisabled();
    for (std::size_t i = 0; i < NEW_PROJECT_LANGUAGE_CARDS.size(); ++i) {
        const LanguageCard& card = NEW_PROJECT_LANGUAGE_CARDS[i];
        const bool selected = languageCardSelected(card.language);
        if (i > 0U) {
            ImGui::SameLine(0.0F, layout.cardGap);
        }
        ImGui::PushID(static_cast<int>(i));
        ImGui::InvisibleButton("##language", ImVec2(layout.cardWidth, layout.cardHeight));
        ImGui::PopID();
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        list->AddRectFilled(min, max, fadedU32(selected ? ChromeRole::Selection : ChromeRole::Raised),
                            layout.controlRounding);
        list->AddRect(min, max, fadedU32(selected ? ChromeRole::AccentBorder : ChromeRole::Border),
                      layout.controlRounding, ImDrawFlags_None, ruleHeight());
        const float radius = layout.radioDiameter * 0.5F;
        const ImVec2 center(min.x + layout.radioInset + radius, min.y + layout.radioInset + radius);
        if (selected) {
            list->AddCircleFilled(center, radius, fadedU32(ChromeRole::Inset));
            list->AddCircleFilled(center, layout.radioDot * 0.5F, fadedU32(ChromeRole::Accent));
        }
        list->AddCircle(center, radius, fadedU32(selected ? ChromeRole::Accent : ChromeRole::Active), 0, ruleHeight());
        const float textX = min.x + layout.cardTextInset;
        const float budget = max.x - layout.radioInset - textX;
        const std::string title = fitTextRight(card.title, budget, strongWidth);
        const std::string subtitle = fitTextRight(card.subtitle, budget, smallWidth);
        const float titleY = min.y + layout.cardTitleTop;
        const float subtitleY = titleY + panelFacePoints(PanelFace::Strong) + layout.cardLineGap;
        const ChromeRole titleRole = selected ? ChromeRole::TextBright : ChromeRole::Text;
        drawFadedText(list, PanelFace::Strong, ImVec2(textX, titleY), titleRole, title);
        drawFadedText(list, PanelFace::BodySmall, ImVec2(textX, subtitleY), ChromeRole::TextLabel, subtitle);
        itemTooltip(LANGUAGE_CONTROL_TOOLTIP);
        if (record != nullptr) {
            recordItem(record->language[i], selected);
        }
    }
    ImGui::EndDisabled();
}

void drawInfoBox(ImDrawList* list, const NewProjectDialogLayout& layout) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(layout.contentWidth, layout.infoHeight));
    const ImVec2 max(min.x + layout.contentWidth, min.y + layout.infoHeight);
    roleFill(list, min, max, ChromeRole::Tile, layout.controlRounding);
    roleOutline(list, min, max, ChromeRole::Border, layout.controlRounding, ruleHeight());
    const float middle = (min.y + max.y) * 0.5F;
    const float dotX = min.x + layout.infoDotInset + (layout.infoDotDiameter * 0.5F);
    roleDot(list, ImVec2(dotX, middle), layout.infoDotDiameter, ChromeRole::Accent);
    list->PushClipRect(min, max, true);  // one line, clipped to the box (D16)
    float x = min.x + layout.infoTextInset;
    for (const InfoSpan& span : NEW_PROJECT_INFO_SPANS) {
        const PanelFace face = span.code ? PanelFace::MonoSmall : PanelFace::BodySmall;
        drawRoleText(list, face, ImVec2(x, middle - (panelFacePoints(face) * 0.5F)),
                     span.code ? ChromeRole::TextSecondary : ChromeRole::TextLabel, span.text);
        x += faceTextWidth(face, span.text);
    }
    list->PopClipRect();
}

struct FooterPresses {
    bool cancel = false;
    bool create = false;
};

FooterPresses drawFooter(ImDrawList* list, const NewProjectDialogLayout& layout, std::string_view name,
                         bool createEnabled, NewProjectRecord* record) {
    // The band is the 1-dp rule, the 64-dp strip and the outer border's row (D5): the rule paints its first row and the
    // fill stops one border short of the bottom, so the outline the window drew is never painted over.
    const ImVec2 top = ImGui::GetCursorScreenPos();   // the window's left edge: the body is unindented
    ImGui::Dummy(ImVec2(0.0F, layout.footerHeight));  // the band's height only, as the header's (drawDialogHeader)
    const float border = ImGui::GetStyle().WindowBorderSize;
    const ImVec2 fillMin(top.x + border, top.y);
    const ImVec2 fillMax(top.x + layout.width - border, top.y + layout.footerHeight - border);
    const float fillRounding = std::max(0.0F, layout.windowRounding - border);
    list->AddRectFilled(fillMin, fillMax, roleU32(ChromeRole::Chrome), fillRounding, ImDrawFlags_RoundCornersBottom);
    horizontalRule(list, top.x + border, top.x + layout.width - border, top.y, ChromeRole::Divider);
    // Measured the way ImGui::Button sizes itself, under each button's own face, so it lands where the layout put it.
    const float cancelLabel = ImGui::CalcTextSize(NEW_PROJECT_CANCEL_LABEL.data()).x;
    const float cancelWidth = cancelLabel + (2.0F * layout.buttonPadding.x);
    float createWidth = 0.0F;
    {
        const ScopedFace strong(PanelFace::Strong);
        createWidth = ImGui::CalcTextSize(NEW_PROJECT_CREATE_LABEL.data()).x + (2.0F * layout.buttonPadding.x);
    }
    const NewProjectFooterLayout footer = newProjectFooterLayout(layout, cancelWidth, createWidth);
    const std::string summary = fitTextRight(newProjectFooterSummary(name), footer.summaryMaxWidth, monoSmallWidth);
    const float summaryY = top.y + ((layout.footerHeight - panelFacePoints(PanelFace::MonoSmall)) * 0.5F);
    drawRoleText(list, PanelFace::MonoSmall, ImVec2(top.x + footer.summaryX, summaryY), ChromeRole::TextMuted, summary);
    FooterPresses presses;
    const FramePaddingScope padding(layout.buttonPadding);
    ImGui::SetCursorScreenPos(ImVec2(top.x + footer.cancelX, top.y + footer.buttonY));
    presses.cancel = ghostButton(NEW_PROJECT_CANCEL_LABEL.data());
    if (record != nullptr) {
        recordItem(record->cancel, false);
    }
    ImGui::SetCursorScreenPos(ImVec2(top.x + footer.createX, top.y + footer.buttonY));
    presses.create = accentButton(NEW_PROJECT_CREATE_LABEL.data(), createEnabled);  // D15: no outline, Strong label
    if (record != nullptr) {
        recordItem(record->create, false);
        record->summary = summary;
    }
    return presses;
}

void drawLocationRow(ImDrawList* list, const NewProjectDialogLayout& layout, NewProjectForm& form,
                     std::string_view home, NewProjectRecord* record) {
    const float browseLabel = ImGui::CalcTextSize(NEW_PROJECT_BROWSE_LABEL.data()).x;
    const float browseWidth = browseLabel + (2.0F * layout.buttonPadding.x);
    const float locationWidth = std::max(0.0F, layout.contentWidth - layout.browseGap - browseWidth);
    // U3: a DRAWN inset box, never an InputText -- no caret, no selection, no tab stop (an InvisibleButton is NoNav,
    // imgui_widgets.cpp:857); Browse... stays the only writer of form.location.
    ImGui::InvisibleButton("##location", ImVec2(locationWidth, layout.fieldHeight));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    roleFill(list, min, max, ChromeRole::Inset, layout.controlRounding);
    roleOutline(list, min, max, ChromeRole::Border, layout.controlRounding, ruleHeight());
    std::string shown;
    const float textX = min.x + layout.fieldPadding.x;
    if (form.location.empty()) {
        const float textY = (min.y + max.y - panelFacePoints(PanelFace::Body)) * 0.5F;
        drawRoleText(list, PanelFace::Body, ImVec2(textX, textY), ChromeRole::TextFaint, NEW_PROJECT_NO_LOCATION_TEXT);
    } else {
        const auto monoWidth = [](std::string_view text) { return faceTextWidth(PanelFace::Mono, text); };
        const float pathBudget = locationWidth - (2.0F * layout.fieldPadding.x);
        shown = fitPathLeft(abbreviateHome(form.location, home), pathBudget, monoWidth);
        const float textY = (min.y + max.y - panelFacePoints(PanelFace::Mono)) * 0.5F;
        drawRoleText(list, PanelFace::Mono, ImVec2(textX, textY), ChromeRole::TextSecondary, shown);
        itemTooltip(form.location);
    }
    if (record != nullptr) {
        recordItem(record->locationField, false);
        record->location = shown;
    }
    ImGui::SameLine(0.0F, layout.browseGap);
    const bool browsePressed = ghostButton(NEW_PROJECT_BROWSE_LABEL.data(), ImVec2(browseWidth, layout.fieldHeight));
    if (record != nullptr) {
        recordItem(record->browse, false);
    }
    if (browsePressed) {
        form.browseRequested = true;  // and NOTHING else -- the dialog is launched from
                                      // applyFileRequests, NEVER from inside the draw walk
    }
}

void drawNewProjectBody(FileMenuContext& fileMenu, NewProjectForm& form, const NewProjectDialogLayout& layout,
                        float uiScale, NewProjectRecord* record) {
    ImDrawList* const list = ImGui::GetWindowDrawList();
    const std::string_view home = fileMenu.homeDirectory;
    bool dismiss = false;
    bool commit = false;
    bool createEnabled = false;
    {
        const DialogBodyStyle body(layout.controlRounding);
        if (record != nullptr) {
            ++record->framesDrawn;
            record->drawnThisFrame = true;
            record->uiScaleAtDraw = uiScale;
            record->windowPos = toVec2(ImGui::GetWindowPos());
            record->windowSize = toVec2(ImGui::GetWindowSize());
            record->workPos = toVec2(ImGui::GetMainViewport()->WorkPos);
            record->workSize = toVec2(ImGui::GetMainViewport()->WorkSize);
            record->scrollMaxX = ImGui::GetScrollMaxX();
            record->scrollbarY = ImGui::GetCurrentWindow()->ScrollbarY;
            record->layout = layout;
        }
        dismiss = drawDialogHeader(layout.width, layout.windowRounding, layout.controlRounding, NEW_PROJECT_TITLE,
                                   fileMenu.project.engineVersion, true, uiScale,
                                   record != nullptr ? &record->header : nullptr);
        ImGui::Indent(layout.paddingX);
        gap(layout.bodyTop);
        roleText(ChromeRole::Text, NEW_PROJECT_NAME_LABEL);
        gap(layout.labelGap);
        {
            const FramePaddingScope padding(layout.fieldPadding);
            ImGui::PushStyleColor(ImGuiCol_Text, roleVec4(ChromeRole::TextBright));
            ImGui::PushStyleColor(ImGuiCol_InputTextCursor, roleVec4(ChromeRole::Accent));  // the mock's caret (D14)
            ImGui::SetNextItemWidth(layout.contentWidth);
            if (ImGui::IsWindowAppearing()) {
                ImGui::SetKeyboardFocusHere();  // every appearance, a failed Create's re-open included (D14)
            }
            // D27: while the field is ACTIVE its own buffer, not form.name, owns the text, and it writes that buffer
            // back over form.name every frame the two differ (imgui_widgets.cpp:5393-5399). On the one tick the GPU
            // tier's input landed, the live widget is reloaded from form.name first (ReloadUserBufAndMoveToEnd,
            // consumed before the write-back at :4834-4846); GetInputTextState answers null unless this field owns
            // the state.
            if (fileMenu.newProjectInputApplied) {
                ImGuiInputTextState* const liveName = ImGui::GetInputTextState(ImGui::GetID("##name"));
                if (liveName != nullptr) {
                    liveName->ReloadUserBufAndMoveToEnd();
                }
            }
            inputTextString("##name", NEW_PROJECT_NAME_HINT.data(), form.name, ImGuiInputTextFlags_None);
            ImGui::PopStyleColor(2);
            const bool active = ImGui::IsItemActive();
            if (active) {  // ImGui draws no focused outline of its own (G8)
                roleOutline(list, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ChromeRole::AccentBorder,
                            layout.controlRounding, ruleHeight());
            }
            if (record != nullptr) {
                recordItem(record->nameField, active);
            }
        }
        // AFTER the field: the name typed THIS frame is what the line and Create answer (the probe trails it, D12).
        const NewProjectTargetState state = classifyNewProjectTarget(form.name, form.location, form.probe);
        createEnabled = newProjectCreateEnabled(state);
        gap(layout.fieldGap);
        roleText(ChromeRole::Text, NEW_PROJECT_LOCATION_LABEL);
        gap(layout.labelGap);
        drawLocationRow(list, layout, form, home, record);
        gap(layout.lineGap);
        {
            const NewProjectLine line =
                fitNewProjectLine(newProjectLine(state, form.name, form.location, form.probe, form.error, home),
                                  layout.contentWidth, monoSmallWidth);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(layout.contentWidth, panelFacePoints(PanelFace::MonoSmall)));  // ONE row, always (D10)
            float x = at.x;
            for (const LineSpan* span : std::array<const LineSpan*, 3>{&line.lead, &line.path, &line.tail}) {
                drawRoleText(list, PanelFace::MonoSmall, ImVec2(x, at.y), span->role, span->text);
                x += monoSmallWidth(span->text);
            }
            if (record != nullptr) {
                record->state = state;
                record->line = line;
            }
        }
        gap(layout.dividerGap);
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(layout.contentWidth, ruleHeight()));
            horizontalRule(list, at.x, at.x + layout.contentWidth, at.y, ChromeRole::Divider);
        }
        gap(layout.dividerGap);
        {
            const ScopedFace heading(PanelFace::StrongSmall);
            roleText(ChromeRole::TextMuted, NEW_PROJECT_SETTINGS_HEADING);  // the mock's tracking is not drawn (D28)
        }
        gap(layout.headingGap);
        {
            const ScopedFace label(PanelFace::BodySmall);
            roleText(ChromeRole::TextLabel, NEW_PROJECT_LANGUAGE_LABEL);
        }
        gap(layout.labelGap);
        drawLanguageCards(list, layout, record);
        gap(layout.hintGap);
        {
            const ScopedFace face(PanelFace::MonoSmall);
            const std::string hint = fitTextRight(NEW_PROJECT_LANGUAGE_HINT, layout.contentWidth, monoSmallWidth);
            roleText(ChromeRole::TextFaint, hint);
        }
        gap(layout.infoGap);
        drawInfoBox(list, layout);
        gap(layout.infoGap);
        ImGui::Unindent(layout.paddingX);
        const FooterPresses presses = drawFooter(list, layout, form.name, createEnabled, record);
        dismiss = dismiss || presses.cancel;
        commit = presses.create;
    }
    // D20: ONE resolution, the #109 recipe (asset_browser_panel.cpp; the containment modal in shell_ui.cpp binds its
    // keys the same way). Enter and KeypadEnter -- a distinct key, imgui.h:1678 -- commit under Create's own
    // predicate; IsKeyPressed(key, false) is not routed, so it fires over the active Name field
    // (imgui.cpp:10478-10481). Escape, Cancel and the close box dismiss: ImGui cannot dismiss a modal itself (its nav
    // cancel skips a Modal, imgui.cpp:15032, and the editor never enables NavEnableKeyboard). The GPU tier's answer
    // takes the same predicates. A dismiss WINS over a same-frame commit.
    // 2.6.1's BLOCKING-1, on every path: clearing form.open only stops this TU re-submitting the popup -- ImGui owns
    // g.OpenPopupStack and never collects an entry for a popup that merely stops being submitted (GetTopMostPopupModal
    // tests only the Modal flag), after which g.HoveredWindow is NULL for ever and every menu, panel, button and dock
    // tab is unclickable. So each arm closes the popup itself, on the line after its one-shot (PU1 reads it there),
    // and a FAILED create, which leaves form.open true, is re-opened next frame by drawNewProjectModal's own open
    // call, exactly like a fresh request. No tier can press a key here; validation rows 9-12 and 25 are the witness.
    const bool enterPressed =
        ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    const bool escapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    dismiss = escapePressed || dismiss || fileMenu.newProjectAnswer == NewProjectAnswer::Dismiss;
    commit = ((enterPressed || fileMenu.newProjectAnswer == NewProjectAnswer::Create) && createEnabled) || commit;
    if (dismiss) {
        form.cancelRequested = true;
        ImGui::CloseCurrentPopup();
    } else if (commit) {
        form.createRequested = true;
        ImGui::CloseCurrentPopup();
    }
}

}  // namespace

void drawWelcomeWindow(FileMenuContext& fileMenu) {
    WelcomeRecord* const record = fileMenu.projectUiRecord != nullptr ? &fileMenu.projectUiRecord->welcome : nullptr;
    if (record != nullptr) {
        beginWelcomeRecordFrame(*record);  // BEFORE the early return: a frame without the window reads "not drawn"
    }
    if (fileMenu.project.session.isOpen()) {
        return;  // AC-29: shown IFF no project is open
    }
    const float uiScale = currentUiScale();
    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    // D34: the layout asks for the blurb's height at its own content width, measured as the body will wrap it.
    const WelcomeLayout layout = welcomeLayout(
        fileMenu.project.recents.paths.size(), toVec2(viewport->WorkSize),
        [](float wrapWidth) { return bodyWrappedHeight(WELCOME_BLURB, wrapWidth); }, uiScale);
    // D21: centred on the work area EVERY frame -- NoMove makes Always free -- at the layout's width, its height
    // auto-fit. GetWorkCenter() (a METHOD in this pinned ImGui) == WorkPos + WorkSize * 0.5.
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(layout.width, 0.0F), ImGuiCond_Always);
    // Never scrolled sideways: Begin takes this target over a wheel's (imgui.cpp:8003-8009), and -1 leaves y alone.
    // At the minWidth floor the button row is wider than the content, so a clamped window keeps a few points of
    // ScrollMax.x the bands' height-only Dummies cannot remove.
    ImGui::SetNextWindowScroll(ImVec2(0.0F, -1.0F));
    bool open = false;
    {
        const DialogFrameStyle frame(ImGuiCol_WindowBg, layout.windowRounding);
        // Begin/End are 1:1 -- End() is UNCONDITIONAL; Begin's return value gates only the BODY
        // (.claude/rules/editor.md). This is the OPPOSITE of the popup family below.
        open = ImGui::Begin(WELCOME_WINDOW_ID, nullptr, WELCOME_FLAGS);
    }
    if (open) {
        drawWelcomeBody(fileMenu, layout, uiScale, record);
    }
    ImGui::End();
}

void drawNewProjectModal(FileMenuContext& fileMenu) {
    NewProjectRecord* const record =
        fileMenu.projectUiRecord != nullptr ? &fileMenu.projectUiRecord->newProject : nullptr;
    if (record != nullptr) {
        beginNewProjectRecordFrame(*record);
        record->popupOpenAtEntry = ImGui::IsPopupOpen(NEW_PROJECT_MODAL_ID);  // FIRST: false once an answer closed it
    }
    NewProjectForm& form = fileMenu.project.flow.form;
    if (form.open && !ImGui::IsPopupOpen(NEW_PROJECT_MODAL_ID)) {
        ImGui::OpenPopup(NEW_PROJECT_MODAL_ID);
    }
    if (!form.open) {
        return;
    }
    const float uiScale = currentUiScale();
    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    const NewProjectDialogLayout layout = newProjectDialogLayout(viewport->WorkSize.x, uiScale);
    // D21: the Welcome window's rule; a popup that is not open consumes these (imgui.cpp:13215-13221).
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSize(ImVec2(layout.width, 0.0F), ImGuiCond_Always);
    ImGui::SetNextWindowScroll(ImVec2(0.0F, -1.0F));  // the Welcome window's rule: never scrolled sideways
    bool open = false;
    {
        const DialogFrameStyle frame(ImGuiCol_PopupBg, layout.windowRounding);
        // The popup family, not the Begin one: the body runs, and is closed, ONLY when this returned true.
        open = ImGui::BeginPopupModal(NEW_PROJECT_MODAL_ID, nullptr, NEW_PROJECT_FLAGS);
    }
    if (open) {
        drawNewProjectBody(fileMenu, form, layout, uiScale, record);
        ImGui::EndPopup();
    } else {
        // A SAFETY NET, not the Esc mechanism above: the only thing that can reach here is a
        // PROGRAMMATIC close, because a modal also swallows outside clicks. Treating it as Cancel
        // keeps the flow from wedging with form.open stuck true (the drawUnsavedChangesModal
        // precedent, shell_ui.cpp).
        form.cancelRequested = true;
    }
}

}  // namespace engine::editor

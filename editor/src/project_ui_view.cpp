// Aero Engine -- the Welcome window's and the New Project dialog's model (task E.6.4, D29). PURE: no ImGui, no
// <filesystem>, no logging. Every length is a ThemeDialogs dp x the sanitised scale; every type size is
// ThemeTypeScale's through faceSizePoints, ImGui's own rounding of a face, so a frame lands on the theme's height at
// every scale.
#include <aero/editor/editor_theme.hpp>   // EDITOR_THEME.dialogs, EDITOR_THEME.type
#include <aero/editor/project_files.hpp>  // rootDisplayName
#include <aero/editor/project_ui_view.hpp>
#include <aero/editor/status_bar.hpp>  // abbreviateHome, elidePathLeft

#include <algorithm>
#include <cmath>
#include <numbers>  // sqrt2_v: modernize-use-std-numbers refuses a spelled sqrt(2)
#include <utility>  // std::move

namespace engine::editor {

namespace {

[[nodiscard]] float layoutScale(float uiScale) noexcept {
    return (std::isfinite(uiScale) && uiScale > 0.0F) ? uiScale : 1.0F;
}

// The width rule both windows share (D6, D21): the theme's width, narrowed to the work area less screenMargin a side,
// never below minWidth -- in WHOLE points, because ImGui truncates an API-set size (SetWindowSize, imgui.cpp:9178),
// so the layout and the window agree at every scale. A non-finite work width keeps the theme's width.
[[nodiscard]] float windowWidth(float targetDp, float workWidth, float s) noexcept {
    const ThemeDialogs& d = EDITOR_THEME.dialogs;
    const float target = targetDp * s;
    if (!std::isfinite(workWidth)) {
        return std::floor(target);
    }
    return std::floor(std::max(d.minWidth * s, std::min(target, workWidth - (2.0F * d.screenMargin * s))));
}

[[nodiscard]] float bodyPoints(float s) noexcept { return faceSizePoints(EDITOR_THEME.type.bodySize, s); }

[[nodiscard]] NewProjectLine onlyTail(std::string_view text, ChromeRole role) {
    NewProjectLine line;
    line.tail = LineSpan{std::string(text), role};
    return line;
}

[[nodiscard]] NewProjectLine pathAndTail(std::string path, ChromeRole pathRole, std::string_view tail,
                                         ChromeRole tailRole) {
    NewProjectLine line;
    line.path = LineSpan{std::move(path), pathRole};
    line.tail = LineSpan{std::string(tail), tailRole};
    return line;
}

[[nodiscard]] float lineWidth(const NewProjectLine& line, const TextWidth& width) {
    return width(line.lead.text) + width(line.path.text) + width(line.tail.text);
}

}  // namespace

bool languageCardSelected(ProjectLanguage language) noexcept { return language == NEW_PROJECT_LANGUAGE; }

std::string_view languageCardTitle(ProjectLanguage language) noexcept {
    for (const LanguageCard& card : NEW_PROJECT_LANGUAGE_CARDS) {
        if (card.language == language) {
            return card.title;
        }
    }
    return {};
}

NewProjectTargetState classifyNewProjectTarget(std::string_view name, std::string_view location,
                                               const NewProjectTargetFacts& facts) noexcept {
    const NameProblem problem = validateProjectName(name);
    if (problem == NameProblem::Empty) {
        return NewProjectTargetState::NoName;
    }
    if (problem != NameProblem::Ok) {
        return NewProjectTargetState::BadName;
    }
    if (location.empty()) {
        return NewProjectTargetState::NoLocation;
    }
    if (!facts.touched || facts.location != location) {
        return NewProjectTargetState::Checking;
    }
    if (!facts.locationIsDirectory) {
        return NewProjectTargetState::LocationMissing;
    }
    switch (facts.target) {
        case NewProjectTargetKind::File:
            return NewProjectTargetState::TargetIsFile;
        case NewProjectTargetKind::NonEmptyDirectory:
            return NewProjectTargetState::TargetNotEmpty;
        case NewProjectTargetKind::Absent:
            return NewProjectTargetState::CreatesFolder;
        case NewProjectTargetKind::EmptyDirectory:
            return NewProjectTargetState::AdoptsEmptyFolder;
    }
    return NewProjectTargetState::Checking;  // every enumerator returned above; the compilers want a return
}

bool newProjectCreateEnabled(NewProjectTargetState state) noexcept {
    return state == NewProjectTargetState::CreatesFolder || state == NewProjectTargetState::AdoptsEmptyFolder;
}

NewProjectLine newProjectLine(NewProjectTargetState state, std::string_view name, std::string_view location,
                              const NewProjectTargetFacts& facts, std::string_view createError, std::string_view home) {
    if (newProjectCreateEnabled(state) && !createError.empty()) {
        return onlyTail(createError, ChromeRole::Error);  // D11: what the probe cannot see
    }
    switch (state) {
        case NewProjectTargetState::NoName:
            return onlyTail(NEW_PROJECT_HINT_NO_NAME, ChromeRole::TextMuted);
        case NewProjectTargetState::BadName:
            return onlyTail(nameProblemMessage(validateProjectName(name)), ChromeRole::Error);
        case NewProjectTargetState::NoLocation:
            return onlyTail(NEW_PROJECT_HINT_NO_LOCATION, ChromeRole::TextMuted);
        case NewProjectTargetState::Checking:
            return onlyTail(NEW_PROJECT_HINT_CHECKING, ChromeRole::TextFaint);
        case NewProjectTargetState::LocationMissing:
            return pathAndTail(abbreviateHome(location, home), ChromeRole::TextSecondary,
                               NEW_PROJECT_TAIL_LOCATION_MISSING, ChromeRole::Error);
        case NewProjectTargetState::TargetIsFile:
            return pathAndTail(abbreviateHome(facts.targetPath, home), ChromeRole::TextSecondary,
                               NEW_PROJECT_TAIL_TARGET_IS_FILE, ChromeRole::Error);
        case NewProjectTargetState::TargetNotEmpty:
            return pathAndTail(abbreviateHome(facts.targetPath, home), ChromeRole::TextSecondary,
                               NEW_PROJECT_TAIL_TARGET_NOT_EMPTY, ChromeRole::Error);
        case NewProjectTargetState::CreatesFolder:
        case NewProjectTargetState::AdoptsEmptyFolder: {
            const bool newFolder = state == NewProjectTargetState::CreatesFolder;
            const std::string_view tail = newFolder ? NEW_PROJECT_TAIL_NEW_FOLDER : NEW_PROJECT_TAIL_EMPTY_FOLDER;
            std::string target = abbreviateHome(facts.targetPath, home);
            NewProjectLine line = pathAndTail(std::move(target), ChromeRole::Success, tail, ChromeRole::TextFaint);
            line.lead = LineSpan{std::string(NEW_PROJECT_LEAD_CREATES), ChromeRole::TextMuted};
            return line;
        }
    }
    return onlyTail(NEW_PROJECT_HINT_CHECKING, ChromeRole::TextFaint);  // every enumerator returned above
}

std::string fitPathLeft(std::string_view path, float budget, const TextWidth& width) {
    if (width(path) <= budget) {
        return std::string(path);
    }
    if (budget < width(AERO_GLYPH_ELLIPSIS)) {
        return {};
    }
    return elidePathLeft(path, [&](std::string_view candidate) { return width(candidate) <= budget; });
}

std::string fitTextRight(std::string_view text, float budget, const TextWidth& width) {
    if (width(text) <= budget) {
        return std::string(text);
    }
    if (budget < width(AERO_GLYPH_ELLIPSIS)) {
        return {};
    }
    return elideCaptionRight(text, [&](std::string_view candidate) { return width(candidate) <= budget; });
}

NewProjectLine fitNewProjectLine(const NewProjectLine& line, float available, const TextWidth& width) {
    NewProjectLine fitted = line;
    if (lineWidth(fitted, width) <= available) {
        return fitted;
    }
    // 1. The path, from the left: the folder name at its end is what tells two targets apart.
    if (!line.path.text.empty()) {
        const float pathBudget = available - width(line.lead.text) - width(line.tail.text);
        fitted.path.text = fitPathLeft(line.path.text, pathBudget, width);
        if (!fitted.path.text.empty()) {
            return fitted;  // fitPathLeft's answer fits its budget, so the line fits
        }
    }
    // 2. No room for even the path's ellipsis: the tail, from the right.
    fitted.tail.text = fitTextRight(line.tail.text, available - width(fitted.lead.text), width);
    if (lineWidth(fitted, width) <= available) {
        return fitted;
    }
    // 3. Then the lead.
    fitted.lead.text = fitTextRight(line.lead.text, available - width(fitted.tail.text), width);
    return fitted;
}

std::string newProjectFooterSummary(std::string_view name) {
    const std::string_view language = languageCardTitle(NEW_PROJECT_LANGUAGE);
    if (name.empty()) {
        return std::string(language);
    }
    std::string summary(name);
    summary += " " AERO_GLYPH_MIDDLE_DOT " ";
    summary += language;
    return summary;
}

std::array<Vec2, 4> aeroMarkPoints(Vec2 center, float uiScale) noexcept {
    const float half = (EDITOR_THEME.dialogs.markSide * layoutScale(uiScale)) / std::numbers::sqrt2_v<float>;
    return {{Vec2{center.x, center.y - half}, Vec2{center.x + half, center.y}, Vec2{center.x, center.y + half},
             Vec2{center.x - half, center.y}}};
}

DialogHeaderLayout dialogHeaderLayout(float width, bool closeButton, float uiScale) noexcept {
    const ThemeDialogs& d = EDITOR_THEME.dialogs;
    const float s = layoutScale(uiScale);
    DialogHeaderLayout layout;
    layout.height = d.headerHeight * s;
    layout.centerY = layout.height * 0.5F;
    layout.markCenter = Vec2{d.markCenterX * s, layout.centerY};
    layout.titleX = (d.markCenterX + (d.markBox * 0.5F) + d.markTitleGap) * s;
    layout.closeDrawn = closeButton;
    if (closeButton) {
        const float half = d.closeHitSize * s * 0.5F;
        const float centerX = width - (d.closeCenterInset * s);
        layout.closeMin = Vec2{centerX - half, layout.centerY - half};
        layout.closeMax = Vec2{centerX + half, layout.centerY + half};
        layout.versionRight = layout.closeMin.x - (d.versionGap * s);
    } else {
        layout.versionRight = width - (d.paddingX * s);
    }
    return layout;
}

NewProjectDialogLayout newProjectDialogLayout(float workWidth, float uiScale) noexcept {
    const ThemeDialogs& d = EDITOR_THEME.dialogs;
    const float s = layoutScale(uiScale);
    const float body = bodyPoints(s);
    NewProjectDialogLayout l;
    l.width = windowWidth(d.newProjectWidth, workWidth, s);
    l.paddingX = d.paddingX * s;
    l.contentWidth = l.width - (2.0F * l.paddingX);
    l.headerHeight = d.headerHeight * s;
    l.footerHeight = d.footerHeight * s;
    l.windowRounding = d.windowRounding * s;
    l.controlRounding = d.controlRounding * s;
    l.fieldHeight = d.fieldHeight * s;
    l.buttonHeight = d.buttonHeight * s;
    l.fieldPadding = Vec2{d.fieldPaddingX * s, (l.fieldHeight - body) * 0.5F};
    l.buttonPadding = Vec2{d.buttonPaddingX * s, (l.buttonHeight - body) * 0.5F};
    l.browseGap = d.browseGap * s;
    l.bodyTop = d.bodyTop * s;
    l.labelGap = d.labelGap * s;
    l.fieldGap = d.fieldGap * s;
    l.lineGap = d.lineGap * s;
    l.dividerGap = d.dividerGap * s;
    l.headingGap = d.headingGap * s;
    l.cardGap = d.cardGap * s;
    l.cardWidth = (l.contentWidth - l.cardGap) * 0.5F;
    l.cardHeight = d.cardHeight * s;
    l.radioInset = d.radioInset * s;
    l.radioDiameter = d.radioDiameter * s;
    l.radioDot = d.radioDot * s;
    l.cardTextInset = d.cardTextInset * s;
    l.cardTitleTop = d.cardTitleTop * s;
    l.cardLineGap = d.cardLineGap * s;
    l.hintGap = d.hintGap * s;
    l.infoGap = d.infoGap * s;
    l.infoHeight = d.infoHeight * s;
    l.infoDotInset = d.infoDotInset * s;
    l.infoDotDiameter = d.infoDotDiameter * s;
    l.infoTextInset = d.infoTextInset * s;
    l.footerButtonGap = d.footerButtonGap * s;
    return l;
}

NewProjectFooterLayout newProjectFooterLayout(const NewProjectDialogLayout& layout, float cancelWidth,
                                              float createWidth) noexcept {
    NewProjectFooterLayout footer;
    footer.createX = layout.width - layout.paddingX - createWidth;
    footer.cancelX = footer.createX - layout.footerButtonGap - cancelWidth;
    footer.buttonY = (layout.footerHeight - layout.buttonHeight) * 0.5F;
    footer.summaryX = layout.paddingX;
    footer.summaryMaxWidth = std::max(0.0F, footer.cancelX - layout.footerButtonGap - layout.paddingX);
    return footer;
}

RecentCardText recentCardText(std::string_view root, std::string_view home) {
    return RecentCardText{rootDisplayName(root), abbreviateHome(root, home)};
}

WelcomeLayout welcomeLayout(std::size_t recentCount, Vec2 workSize, const WrappedTextHeight& blurbHeight,
                            float uiScale) {
    const ThemeDialogs& d = EDITOR_THEME.dialogs;
    const ThemeTypeScale& type = EDITOR_THEME.type;
    const float s = layoutScale(uiScale);
    WelcomeLayout l;
    l.width = windowWidth(d.welcomeWidth, workSize.x, s);
    l.paddingX = d.paddingX * s;
    l.contentWidth = l.width - (2.0F * l.paddingX);
    l.headerHeight = d.headerHeight * s;
    l.windowRounding = d.windowRounding * s;
    l.controlRounding = d.controlRounding * s;
    l.buttonHeight = d.buttonHeight * s;
    l.buttonPadding = Vec2{d.buttonPaddingX * s, (l.buttonHeight - bodyPoints(s)) * 0.5F};
    l.buttonGap = d.footerButtonGap * s;
    l.bodyTop = d.bodyTop * s;
    l.sectionGap = d.welcomeSectionGap * s;
    l.labelGap = d.labelGap * s;
    l.cardHeight = d.cardHeight * s;
    l.cardGap = d.recentCardGap * s;
    l.cardTitleTop = d.cardTitleTop * s;
    l.cardLineGap = d.cardLineGap * s;
    l.iconInset = d.recentIconInset * s;
    l.textInset = d.recentTextInset * s;
    l.emptyStateHeight = d.emptyStateHeight * s;
    l.emptyStateBlock = (2.0F * bodyPoints(s)) + l.labelGap + l.cardLineGap + faceSizePoints(type.smallSize, s);
    // D34: the blurb as the draw wraps it at the content edge -- a narrow window wraps it to a second Body line, and
    // the fold counts it. An empty measurer, a non-finite answer or one shorter than a line counts one Body line.
    const float oneLine = bodyPoints(s);
    const float measured = blurbHeight ? blurbHeight(l.contentWidth) : oneLine;
    l.blurbHeight = (std::isfinite(measured) && measured > oneLine) ? measured : oneLine;
    // Every row but the list (or the well): the header, bodyTop, the headline (Strong), the blurb, a gap, the
    // buttons, a gap, the heading (StrongSmall), labelGap -- and paddingX under the list.
    const float fixedRows = l.headerHeight + l.bodyTop + faceSizePoints(type.strongSize, s) + l.blurbHeight +
                            l.sectionGap + l.buttonHeight + l.sectionGap + faceSizePoints(type.smallSize, s) +
                            l.labelGap + l.paddingX;
    const float room = workSize.y - (2.0F * d.screenMargin * s) - fixedRows;  // what the list or the well may take
    if (recentCount == 0U) {
        // D34: the well folds like the list -- to the room, never above emptyStateHeight, never below its block.
        // std::clamp passes a NaN through on libc++, so the finite arm is explicit and a NaN room keeps the full well.
        if (std::isfinite(room)) {
            l.emptyStateHeight = std::max(l.emptyStateBlock, std::min(room, l.emptyStateHeight));
        }
        return l;  // no list: the empty state draws instead
    }
    const std::size_t cap = std::min(recentCount, WELCOME_MAX_VISIBLE_CARDS);
    std::size_t fit = cap;
    if (std::isfinite(room)) {
        // n cards take n x cardHeight + (n - 1) x cardGap, so n fit when n x pitch <= room + cardGap.
        const float cards = std::floor((room + l.cardGap) / (l.cardHeight + l.cardGap));
        if (!(cards >= static_cast<float>(cap))) {  // NaN-safe: a NaN count folds to the floor below
            fit = cards >= 1.0F ? static_cast<std::size_t>(cards) : 1U;
        }
    }
    l.visibleCards = fit;
    l.listScrolls = recentCount > fit;
    l.listHeight = (static_cast<float>(fit) * l.cardHeight) + (static_cast<float>(fit - 1U) * l.cardGap);
    return l;
}

void beginWelcomeRecordFrame(WelcomeRecord& record) noexcept {
    record.drawnThisFrame = false;
    record.uiScaleAtDraw = 0.0F;
    record.windowPos = Vec2{};
    record.windowSize = Vec2{};
    record.workPos = Vec2{};
    record.workSize = Vec2{};
    record.scrollMaxX = 0.0F;
    record.scrollbarY = false;
    record.layout = WelcomeLayout{};
    record.header.title.clear();
    record.header.version.clear();
    record.header.close = ItemRectRecord{};
    record.emptyStateDrawn = false;
    record.listHeight = 0.0F;
    record.cards.clear();
    record.newProject = ItemRectRecord{};
    record.openProject = ItemRectRecord{};
}

void beginNewProjectRecordFrame(NewProjectRecord& record) noexcept {
    record.popupOpenAtEntry = false;
    record.drawnThisFrame = false;
    record.uiScaleAtDraw = 0.0F;
    record.windowPos = Vec2{};
    record.windowSize = Vec2{};
    record.workPos = Vec2{};
    record.workSize = Vec2{};
    record.scrollMaxX = 0.0F;
    record.scrollbarY = false;
    record.layout = NewProjectDialogLayout{};
    record.header.title.clear();
    record.header.version.clear();
    record.header.close = ItemRectRecord{};
    record.state = NewProjectTargetState::NoName;
    record.line = NewProjectLine{};
    record.location.clear();
    record.summary.clear();
    record.nameField = ItemRectRecord{};
    record.locationField = ItemRectRecord{};
    record.browse = ItemRectRecord{};
    record.cancel = ItemRectRecord{};
    record.create = ItemRectRecord{};
    record.language = {};
}

}  // namespace engine::editor

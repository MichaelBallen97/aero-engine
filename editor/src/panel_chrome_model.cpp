// Aero Engine -- the panels' shared chrome as values (task E.6.3). PURE: see panel_chrome_model.hpp.
#include <aero/editor/editor_icons.hpp>
#include <aero/editor/panel_chrome_model.hpp>

#include <algorithm>
#include <cmath>

namespace engine::editor {

namespace {

// US10's rule, restated per TU (toolbar_model.cpp's shape): a non-finite or non-positive scale lays out at 1.
[[nodiscard]] float layoutScale(float uiScale) noexcept {
    return (std::isfinite(uiScale) && uiScale > 0.0F) ? uiScale : 1.0F;
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

// Today's seven CollapsingHeader labels, verbatim (import_details_panel.cpp:481-498, :715) -- drawn as the
// card's TITLE; the card's ImGui id is the host's own ("##card" under the section's PushID, "##blender").
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
    record.lastSpanFullWidthMinX = 0.0F;
}

}  // namespace engine::editor

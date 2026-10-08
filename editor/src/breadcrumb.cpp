// Aero Engine — the breadcrumb's pure half (task E.6.2, D11). No ImGui: see breadcrumb.hpp.
#include <aero/editor/breadcrumb.hpp>
#include <aero/editor/editor_glyphs.hpp>
#include <aero/editor/editor_theme.hpp>

#include <algorithm>
#include <cmath>

namespace engine::editor {

namespace {

[[nodiscard]] float layoutScale(float uiScale) noexcept {  // copied, not shared (toolbar_model.cpp's note)
    return (std::isfinite(uiScale) && uiScale > 0.0F) ? uiScale : 1.0F;
}

// `text` right-elided into `budget`, or "" when not even the ellipsis fits.
[[nodiscard]] std::string elideInto(std::string_view text, float budget, const TextWidth& measure) {
    if (measure(text) <= budget) {
        return std::string(text);
    }
    if (!(budget >= measure(AERO_GLYPH_ELLIPSIS))) {
        return {};
    }
    return elideCaptionRight(text, [&measure, budget](std::string_view line) { return measure(line) <= budget; });
}

}  // namespace

BreadcrumbText breadcrumbText(std::string_view projectName, std::string_view documentName, bool dirty) {
    return BreadcrumbText{.project = std::string(projectName),
                          .separator = projectName.empty() ? std::string() : std::string("/"),
                          .scene = std::string(documentName),
                          .dirty = dirty};
}

BreadcrumbLayout breadcrumbLayout(float barLeft, float barRight, float menusEnd, const BreadcrumbText& text,
                                  const BreadcrumbMeasure& measure, float uiScale) {
    BreadcrumbLayout out;
    if (!std::isfinite(barLeft) || !std::isfinite(barRight) || !std::isfinite(menusEnd)) {
        return out;
    }
    const float s = layoutScale(uiScale);
    const float gap = EDITOR_THEME.shell.breadcrumbGap * s;
    const float dot = EDITOR_THEME.shell.breadcrumbDotDiameter * s;
    const float spanLeft = std::max(menusEnd, barLeft) + (EDITOR_THEME.shell.breadcrumbMinGap * s);
    const float span = barRight - spanLeft;
    if (!(span > 0.0F)) {
        return out;
    }
    const auto bodyWidth = [&measure](std::string_view t) { return t.empty() ? 0.0F : measure.body(t); };
    const auto strongWidth = [&measure](std::string_view t) { return t.empty() ? 0.0F : measure.strong(t); };
    const float separatorWidth = bodyWidth(text.separator);
    const float sceneWidth = strongWidth(text.scene);
    const float dotPart = text.dirty ? gap + dot : 0.0F;

    // 1. The project yields first: right-elided into what the rest leaves, else dropped with its separator.
    std::string project = text.project;
    if (!project.empty()) {
        const float rest = gap + separatorWidth + gap + sceneWidth + dotPart;
        project = elideInto(project, span - rest, measure.body);
    }
    const bool withProject = !project.empty();
    const float prefix = withProject ? bodyWidth(project) + gap + separatorWidth + gap : 0.0F;

    // 2. Then the scene name, into what the project and the dot leave.
    const std::string scene = elideInto(text.scene, span - prefix - dotPart, measure.strong);
    const float drawnScene = strongWidth(scene);

    // 3. The dot, while there is room for it.
    const float beforeDot = prefix + drawnScene + (scene.empty() ? 0.0F : gap);
    out.dotDrawn = text.dirty && beforeDot + dot <= span;
    const float run = (out.dotDrawn ? beforeDot + dot : prefix + drawnScene);
    if (!(run > 0.0F)) {
        return BreadcrumbLayout{};
    }

    // 4. Centred in the free span; a run that needed elision fills it and starts at its left edge.
    const float x0 = spanLeft + std::max(0.0F, (span - run) * 0.5F);
    out.runStart = x0;
    out.runEnd = x0 + run;
    out.project = project;
    out.projectX = x0;
    out.separatorDrawn = withProject;
    out.separatorX = withProject ? x0 + bodyWidth(project) + gap : 0.0F;
    out.scene = scene;
    out.sceneX = x0 + prefix;
    out.dotDiameter = out.dotDrawn ? dot : 0.0F;
    out.dotCenterX = out.dotDrawn ? x0 + beforeDot + (dot * 0.5F) : 0.0F;
    return out;
}

}  // namespace engine::editor

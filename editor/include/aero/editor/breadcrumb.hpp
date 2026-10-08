#pragma once
// Aero Engine — the menu bar's breadcrumb, PURE (task E.6.2, D11): `Project / scene` and the dirty dot. The ImGui
// TU measures and draws; the segments, the elision and every x are decided here, at tier 0 (breadcrumb_test.cpp).
// Every x is SCREEN space -- the menu bar's cursor is read mid-window, so a local/screen mix would be the
// easiest wrong answer, and BD2 drives a bar whose left edge is not 0 to see one.
#include <aero/editor/asset_view.hpp>  // TextWidth, elideCaptionRight

#include <string>
#include <string_view>

namespace engine::editor {

struct BreadcrumbText {
    std::string project;    // ProjectSession::name(); "" with no project open
    std::string separator;  // "/" when there is a project, else ""
    std::string scene;      // SceneSession::documentName(): "Untitled" or the file leaf, verbatim
    bool dirty = false;     // !CommandStack::isClean() -- dirtiness is not stored anywhere else (2.5.1 D3)
    bool operator==(const BreadcrumbText&) const = default;
};
[[nodiscard]] BreadcrumbText breadcrumbText(std::string_view projectName, std::string_view documentName, bool dirty);

// The project and the separator draw in Body, the scene name in Strong (D11's colours and faces).
struct BreadcrumbMeasure {
    TextWidth body;
    TextWidth strong;
};

struct BreadcrumbLayout {
    std::string project;  // as drawn; "" when absent or dropped
    float projectX = 0.0F;
    bool separatorDrawn = false;
    float separatorX = 0.0F;
    std::string scene;  // as drawn; "" when not even an ellipsis fits
    float sceneX = 0.0F;
    bool dotDrawn = false;
    float dotCenterX = 0.0F;
    float dotDiameter = 0.0F;
    float runStart = 0.0F;  // the drawn run's extent, for the no-overlap claims
    float runEnd = 0.0F;
    bool operator==(const BreadcrumbLayout&) const = default;
};

// The free span is [max(menusEnd, barLeft) + breadcrumbMinGap x s, barRight]. A run that fits is CENTRED in it
// (the mock's placement, not the window's centre). One that does not: the PROJECT is right-elided first, then
// dropped with its separator; then the scene name is right-elided; the dot stays while there is room for it.
// Nothing ever starts left of the span, so nothing overlaps the menus; every cut is on a code-point boundary
// (elideCaptionRight). A non-finite input or an empty span draws nothing. `uiScale` is NON-DEFAULTED and
// multiplies breadcrumbMinGap, breadcrumbGap and breadcrumbDotDiameter exactly once (US9); a bad scale is 1.
[[nodiscard]] BreadcrumbLayout breadcrumbLayout(float barLeft, float barRight, float menusEnd,
                                                const BreadcrumbText& text, const BreadcrumbMeasure& measure,
                                                float uiScale);

}  // namespace engine::editor

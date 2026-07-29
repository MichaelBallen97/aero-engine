// editor/src/gizmo.cpp — task 2.3.3: the pure gizmo model. ImGuizmo-free by construction -- nothing
// here names an ImGuizmo type; the enum bridge lives in viewport_panel.cpp's anonymous namespace.
#include <aero/editor/gizmo.hpp>
#include <aero/editor/picking.hpp>  // projectToViewport + CLIP_W_EPSILON (step 3)
#include <aero/scene/world.hpp>     // (step 3)

#include <cmath>

namespace engine::editor {

GizmoMode nextGizmoMode(GizmoMode current, const GizmoModeInput& in) noexcept {
    GizmoMode next = current;
    if (in.translatePressed) {
        next.operation = GizmoOperation::Translate;
    } else if (in.rotatePressed) {
        next.operation = GizmoOperation::Rotate;
    } else if (in.scalePressed) {
        next.operation = GizmoOperation::Scale;
    }
    // Deliberately NOT an `else if`: W and X in the same frame is a legal, unambiguous combination
    // (two fingers) and both should apply.
    if (in.spaceTogglePressed) {
        next.space = (next.space == GizmoSpace::Local) ? GizmoSpace::World : GizmoSpace::Local;
    }
    return next;
}

GizmoSpace effectiveSpace(GizmoOperation op, GizmoSpace requested) noexcept {
    return (op == GizmoOperation::Scale) ? GizmoSpace::Local : requested;
}

std::optional<Vec3> gizmoSnapStep(GizmoOperation op, bool snapHeld) noexcept {
    if (!snapHeld) {
        return std::nullopt;
    }
    float s = GIZMO_SNAP_TRANSLATE;
    switch (op) {
        case GizmoOperation::Translate:
            s = GIZMO_SNAP_TRANSLATE;
            break;
        case GizmoOperation::Rotate:
            s = GIZMO_SNAP_ROTATE_DEGREES;
            break;
        case GizmoOperation::Scale:
            s = GIZMO_SNAP_SCALE;
            break;
    }
    return Vec3{s, s, s};
}

GizmoDragEdge gizmoDragEdge(bool wasUsing, bool isUsing) noexcept {
    if (!wasUsing && !isUsing) {
        return GizmoDragEdge::None;
    }
    if (!wasUsing && isUsing) {
        return GizmoDragEdge::Begin;
    }
    if (wasUsing && isUsing) {
        return GizmoDragEdge::Continue;
    }
    return GizmoDragEdge::End;  // wasUsing && !isUsing
}

}  // namespace engine::editor

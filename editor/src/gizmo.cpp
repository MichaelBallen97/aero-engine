// editor/src/gizmo.cpp — task 2.3.3: the pure gizmo model. ImGuizmo-free by construction -- nothing
// here names an ImGuizmo type; the enum bridge lives in viewport_panel.cpp's anonymous namespace.
#include <aero/editor/gizmo.hpp>
#include <aero/editor/picking.hpp>  // projectToViewport + CLIP_W_EPSILON (step 3)
#include <aero/scene/world.hpp>     // (step 3)

#include <cmath>

namespace engine::editor {

namespace {

// F14: there is still no isFinite in the public math surface -- the picking.cpp:35-36 /
// scene_bounds.cpp:22 "copied, not shared" precedent, not a new one.
[[nodiscard]] bool allFinite(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

[[nodiscard]] bool allFinite(const Mat4& m) noexcept {
    for (const Vec4& c : m.columns) {
        if (!std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z) || !std::isfinite(c.w)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool allFinite(const Transform& t) noexcept {
    return allFinite(t.position) && std::isfinite(t.rotation.x) && std::isfinite(t.rotation.y) &&
           std::isfinite(t.rotation.z) && std::isfinite(t.rotation.w) && allFinite(t.scale);
}

// EXACT compare against Mat4::identity() -- the common-case fast path (most entities are roots) and
// what removes an inverse()'s rounding from the result.
[[nodiscard]] bool isIdentityMatrix(const Mat4& m) noexcept { return m == Mat4::identity(); }

}  // namespace

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

std::optional<Mat4> gizmoModelMatrix(const World& world, Entity entity) {
    if (!world.alive(entity) || !world.has<Transform>(entity)) {
        return std::nullopt;
    }
    const Mat4 model = worldMatrix(world, entity);
    if (!allFinite(model)) {
        return std::nullopt;  // E15: a hostile Transform must not reach ImGuizmo
    }
    return model;
}

Mat4 gizmoParentMatrix(const World& world, Entity entity) {
    const Entity p = world.parent(entity);
    const Mat4 result = p.valid() ? worldMatrix(world, p) : Mat4::identity();
    return allFinite(result) ? result : Mat4::identity();  // never hand a non-finite parent to inverse
}

bool gizmoOriginBehindCamera(const Mat4& viewProj, const Mat4& model, Vec2 viewportSizePoints) noexcept {
    const Vec3 origin{model.columns[3].x, model.columns[3].y, model.columns[3].z};
    Vec2 scratch{};
    // D9: projectToViewport already fails closed for w <= CLIP_W_EPSILON AND any non-finite result
    // (2.3.2 E4) -- reusing it is what gives pick, highlight and gizmo ONE shared "behind the camera".
    return !projectToViewport(viewProj, origin, viewportSizePoints, scratch);
}

GizmoWrite gizmoWriteFromWorld(const Mat4& parentWorld, const Mat4& newWorld, const Transform& before,
                               GizmoOperation op) noexcept {
    if (!allFinite(newWorld)) {
        return GizmoWrite{.status = GizmoWriteStatus::NotFinite};
    }
    // The identity check is EXACT: most entities are roots, so this is the common path, and it also
    // removes an inverse()'s rounding from the result entirely.
    const Mat4 local = isIdentityMatrix(parentWorld) ? newWorld : inverse(parentWorld) * newWorld;
    if (!allFinite(local)) {
        // An inverse of a near-singular parent yields NaN/inf: GLM multiplies a zero adjugate by
        // 1/0 -- exactly the mechanism 2.3.2 recorded for DETERMINANT_EPSILON (E8).
        return GizmoWrite{.status = GizmoWriteStatus::NotFinite};
    }
    Trs trs;
    if (!decompose(local, trs)) {
        return GizmoWrite{.status = GizmoWriteStatus::NotDecomposable};
    }
    Transform result = before;
    switch (op) {  // exhaustive, NO default: (D5)
        case GizmoOperation::Translate:
            result.position = trs.translation;
            break;
        case GizmoOperation::Rotate:
            result.rotation = trs.rotation;
            break;
        case GizmoOperation::Scale:
            result.scale = trs.scale;
            break;
    }
    if (!allFinite(result)) {
        // NOT redundant with the `local` check above: decompose can produce a non-finite component
        // from a FINITE matrix -- a scale axis whose length underflows the guard but whose normalised
        // column overflows.
        return GizmoWrite{.status = GizmoWriteStatus::NotFinite};
    }
    if (result == before) {
        return GizmoWrite{.status = GizmoWriteStatus::NoChange};  // EXACT ==, Transform's own semantics (D6)
    }
    return GizmoWrite{.status = GizmoWriteStatus::Applied, .transform = result};
}

}  // namespace engine::editor

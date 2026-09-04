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

// Code-review finding (2026-07-29), verified at source against the pinned engine::decompose()
// (glm_backend.cpp:126): it guards ONLY column length and finiteness (plus a determinant-sign flip
// for handedness) -- it has NO orthogonality test, so it never rejects shear. That is in contract for
// decompose() itself (transform.hpp:38-41: a sheared matrix "decomposes to nonsense, which is out of
// contract"), but gizmoWriteFromWorld is the layer that must not FEED it out-of-contract input --
// this is that guard. A world-space rotation/translation delta applied under a non-uniformly-scaled
// parent is genuine shear: decompose() would otherwise silently SUCCEED with a numerically wrong
// (and possibly non-unit, scale-corrupting per transform.hpp:35) Trs, `Applied` instead of refused.
//
// GIZMO_ORTHOGONALITY_EPSILON = 1e-4 is MEASURED, not guessed (2026-07-29 review, exact figures
// recorded here so a future retune has the baseline): every LEGITIMATE (non-sheared) construction
// tested tops out at 8.0e-08 |cos| between normalised column pairs (a rotated + uniformly-scaled
// parent with a rotated child); an 8-deep transform chain measures 3.3e-08; the worst numerical
// conditioning tried (a 1e3-scaled parent at a 1e4 offset with a 1e-3-scaled child) measures 1.7e-08.
// The hardest REAL shear case tried -- a barely-non-uniform {1.01,1,1} parent with a 0.5-degree delta
// -- measures 1.7e-04. 1e-4 sits three orders of magnitude above the legitimate-construction ceiling
// and one below the smallest shear tried, with room either way. Every shipped tier-0 case was
// re-evaluated under this guard and no verdict changed (G7 6.5e-10, G8 0, G9 0, G10 1.3e-08 --
// all far under the threshold).
inline constexpr float GIZMO_ORTHOGONALITY_EPSILON = 1.0e-4F;

// True iff `m`'s upper-left 3x3 is NOT (numerically) an orthogonal basis once each column is
// normalised -- i.e. the matrix carries shear. Zero-length or non-finite columns are deliberately
// NOT flagged here: that is decompose()'s own job (its length/EPSILON guard), and normalising a
// near-zero column would divide by ~0 -- this function judges ORTHOGONALITY only, nothing else, so
// it must not disturb the existing NotFinite/degenerate rejection paths.
[[nodiscard]] bool isSheared(const Mat4& m) noexcept {
    const Vec3 c0{m.columns[0].x, m.columns[0].y, m.columns[0].z};
    const Vec3 c1{m.columns[1].x, m.columns[1].y, m.columns[1].z};
    const Vec3 c2{m.columns[2].x, m.columns[2].y, m.columns[2].z};
    const float l0 = length(c0);
    const float l1 = length(c1);
    const float l2 = length(c2);
    if (!std::isfinite(l0) || !(l0 > EPSILON) || !std::isfinite(l1) || !(l1 > EPSILON) || !std::isfinite(l2) ||
        !(l2 > EPSILON)) {
        return false;  // decompose()'s own guard rejects this; not this function's question to answer
    }
    const Vec3 n0 = c0 / l0;
    const Vec3 n1 = c1 / l1;
    const Vec3 n2 = c2 / l2;
    return std::abs(dot(n0, n1)) > GIZMO_ORTHOGONALITY_EPSILON || std::abs(dot(n0, n2)) > GIZMO_ORTHOGONALITY_EPSILON ||
           std::abs(dot(n1, n2)) > GIZMO_ORTHOGONALITY_EPSILON;
}

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

bool gizmoOriginBehindCamera(const Mat4& viewProj, ProjectionMode mode, const Mat4& model,
                             Vec2 viewportSizePoints) noexcept {
    const Vec3 origin{model.columns[3].x, model.columns[3].y, model.columns[3].z};
    Vec2 scratch{};
    // D9: projectToViewport already fails closed for w <= CLIP_W_EPSILON AND any non-finite result
    // (2.3.2 E4) -- reusing it is what gives pick, highlight and gizmo ONE shared "behind the camera"
    // definition for the w-based half of this test.
    if (!projectToViewport(viewProj, mode, origin, viewportSizePoints, scratch)) {
        return true;
    }
    // Code-review finding (2026-07-29): our w-based test and ImGuizmo's OWN behind-camera test
    // (ImGuizmo.cpp:2696-2698 -- `camSpacePosition.z < 0.001f`, the RAW clip-space z, no perspective
    // divide) are DIFFERENT quantities. With the real EditorCamera defaults (near 0.1, far 1000,
    // 60-degree fov) there is a reachable band -- roughly 0.0002 to 0.11 world units in front of the
    // eye -- where our w-test says "in front" but ImGuizmo's own z-test would have refused, so
    // Manipulate gets called and takes exactly the early return that leaks an unmatched
    // PushClipRect (F5: the `PushClipRect` at `:2682` has no matching `PopClipRect` on that path --
    // the only one is at `:2728`). Mirroring ImGuizmo's own z-test here, from the SAME
    // viewProj*origin, closes the band completely: nothing can reach Manipulate that ImGuizmo itself
    // would have refused.
    //
    // TASK E.1.3 -- THIS TEST STOPS MIRRORING IN ORTHOGRAPHIC, AND IT IS KEPT ANYWAY. ImGuizmo's own
    // guard at ImGuizmo.cpp:2696 reads `!gContext.mIsOrthographic && camSpacePosition.z < 0.001f &&
    // !gContext.mbUsing`, so with SetOrthographic(true) the FIRST term is false and the early return
    // -- and therefore the PushClipRect leak -- cannot happen at all. There is consequently nothing
    // to mirror in that mode. The test stays UNCONDITIONAL rather than gaining a `mode ==
    // Perspective` arm, because in ortho it is simply a slightly stricter near-plane cut of our own:
    // test 1 above now refuses at clip.z <= CLIP_Z_EPSILON (1e-6) and this refuses at clip.z < 0.001,
    // which is a narrower band on the same normalised axis and is safe. Do not delete it as dead
    // code -- it is live and load-bearing in perspective, which is the mode this editor is in by
    // default.
    //
    // `!(clip.z >= 0.001F)`, not `clip.z < 0.001F`: the negated form fails CLOSED on a non-finite
    // clip.z (every direct `<`/`>=` comparison with NaN is false, so the negated form is true) --
    // the same NaN-safety idiom this codebase uses throughout (2.3.1/2.3.2).
    const Vec4 clip = viewProj * toVec4(origin, 1.0F);
    return !(clip.z >= 0.001F);
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
    if (isSheared(local)) {
        // Code-review finding: decompose() itself has no orthogonality test and would otherwise
        // silently SUCCEED on this out-of-contract input (see isSheared's comment above).
        return GizmoWrite{.status = GizmoWriteStatus::NotDecomposable};
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

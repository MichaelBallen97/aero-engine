// editor/src/picking.cpp — task 2.3.2: the screen mapping, the basis ray, the local-box slab test,
// the world pick walk and the pure click decision. ImGui-free by construction -- nothing here names
// an ImGui type.
#include <aero/editor/picking.hpp>
#include <aero/editor/scene_bounds.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace engine::editor {

namespace {

constexpr float INF = std::numeric_limits<float>::infinity();

// A direction component below this counts as PARALLEL to that slab. ANY positive value removes the
// 0 * inf -> NaN path (an infinite 1/d is produced only by an exactly-zero d), so it is chosen small
// enough not to misclassify a genuinely oblique ray inside a heavily scaled entity's LOCAL space --
// a 1e6x scale divides every direction component by 1e6.
constexpr float DIR_EPSILON = 1.0e-12F;

// |det(model)| below this is a SINGULAR matrix -- a zero scale on some axis (E5). Deliberately tiny:
// it is here to reject zero VOLUME, not small OBJECTS. A uniform 1e-4 scale has det 1e-12 and must
// stay pickable.
constexpr float DETERMINANT_EPSILON = 1.0e-20F;

// F14: there is still no isFinite in the public math surface, and scene_bounds.cpp already carries
// its own file-local copy (:22). Two TUs each holding three lines beats widening ADR-005 territory
// for one consumer -- the same "copied, not shared" call 2.2.4 recorded.
[[nodiscard]] bool allFinite(Vec2 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y); }
[[nodiscard]] bool allFinite(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Vec3 has NO operator[] and no component accessor -- verified, not assumed: vec3.hpp declares only
// .x/.y/.z plus free functions. The slab loop below needs one.
[[nodiscard]] float axisValue(Vec3 v, int axis) noexcept { return axis == 0 ? v.x : (axis == 1 ? v.y : v.z); }

}  // namespace

Vec2 viewportNdc(Vec2 mousePoints, Vec2 imageOriginPoints, Vec2 imageSizePoints) noexcept {
    // The explicit allFinite is what stops +inf: `+inf > 0.0F` is TRUE, so the negated `>` alone lets
    // infinity sail straight through a totality guard (2.3.1's own recorded trap).
    if (!allFinite(imageSizePoints) || !(imageSizePoints.x > 0.0F) || !(imageSizePoints.y > 0.0F)) {
        return Vec2::zero();
    }
    if (!allFinite(mousePoints) || !allFinite(imageOriginPoints)) {
        return Vec2::zero();
    }
    const float u = (mousePoints.x - imageOriginPoints.x) / imageSizePoints.x;  // 0 at the LEFT edge
    const float v = (mousePoints.y - imageOriginPoints.y) / imageSizePoints.y;  // 0 at the TOP edge
    // y is FLIPPED: ImGui's +y is screen-DOWN, NDC's is UP (AC-1).
    return Vec2{(u * 2.0F) - 1.0F, 1.0F - (v * 2.0F)};
}

Vec2 ndcToViewportPoints(Vec2 ndc, Vec2 viewportSizePoints) noexcept {
    return Vec2{(ndc.x + 1.0F) * 0.5F * viewportSizePoints.x, (1.0F - ndc.y) * 0.5F * viewportSizePoints.y};
}

namespace {

// task E.1.3: the ONE place the mode picks a clip-space component and its threshold, so the two
// predicates below cannot disagree about what "in front of the eye" means.
[[nodiscard]] float inFrontComponent(const Vec4& clip, ProjectionMode mode) noexcept {
    return (mode == ProjectionMode::Orthographic) ? clip.z : clip.w;
}
[[nodiscard]] constexpr float inFrontEpsilon(ProjectionMode mode) noexcept {
    return (mode == ProjectionMode::Orthographic) ? CLIP_Z_EPSILON : CLIP_W_EPSILON;
}

}  // namespace

bool projectToViewport(const Mat4& viewProj, ProjectionMode mode, Vec3 worldPoint, Vec2 viewportSizePoints,
                       Vec2& outPoints) noexcept {
    const Vec4 clip = viewProj * toVec4(worldPoint, 1.0F);
    // PERSPECTIVE (F13, unchanged and byte-identical in behaviour): w > 0 means "in front of the eye"
    // under this engine's RH / -Z / clip-Z-in-[0,1] convention. The near plane is z_clip = 0, NOT
    // z_clip = -w, so this is the correct and sufficient test.
    // ORTHOGRAPHIC (task E.1.3): clip.w does not depend on the world point at all -- an ortho proj's
    // bottom row is (0,0,0,1) and the view matrix is affine -- so the w test is VACUOUS, and
    // z_clip == 0 IS the near plane. Two arms, because a single test correct in both would change the
    // perspective behaviour this task has no business changing (D12). The negated `>` keeps the
    // NaN-safety of the original in both arms.
    if (!(inFrontComponent(clip, mode) > inFrontEpsilon(mode))) {
        return false;
    }
    const Vec2 points = ndcToViewportPoints(Vec2{clip.x / clip.w, clip.y / clip.w}, viewportSizePoints);
    if (!allFinite(points)) {
        return false;  // E4
    }
    outPoints = points;
    return true;
}

bool clipSegmentToNearPlane(Vec4& a, Vec4& b, ProjectionMode mode) noexcept {
    const float epsilon = inFrontEpsilon(mode);
    const bool aInFront = inFrontComponent(a, mode) > epsilon;
    const bool bInFront = inFrontComponent(b, mode) > epsilon;
    if (aInFront && bInFront) {
        return true;
    }
    if (!aInFront && !bInFront) {
        return false;  // the whole segment is at or behind the eye; the edge does not exist on screen
    }
    Vec4& behind = aInFront ? b : a;
    const Vec4& front = aInFront ? a : b;
    const float denominator = inFrontComponent(front, mode) - inFrontComponent(behind, mode);
    if (!(denominator > 0.0F)) {
        return false;  // unreachable given aInFront != bInFront; NaN-safe and total anyway
    }
    // D14: interpolate IN CLIP SPACE, BEFORE the perspective divide. The clipped endpoint lands on
    // the mode's own threshold exactly -- w == CLIP_W_EPSILON in perspective, z == CLIP_Z_EPSILON in
    // ortho -- and stays on the original clip-space line, which is what keeps the drawn edge
    // straight. S8 seeds the post-divide lerp; this is what catches it.
    const float t = (epsilon - inFrontComponent(behind, mode)) / denominator;
    behind = behind + ((front - behind) * t);
    return std::isfinite(behind.x) && std::isfinite(behind.y) && std::isfinite(behind.z) && std::isfinite(behind.w);
}

Ray viewportRay(const EditorCamera& camera, Vec2 ndc, float aspect) noexcept {
    // A ZERO direction is the documented "unbuildable" answer; the origin is still the eye, so the
    // result is TOTAL rather than garbage.
    const Ray unbuildable{.origin = camera.position(), .direction = Vec3::zero()};
    if (!allFinite(ndc) || !std::isfinite(aspect) || !(aspect > 0.0F)) {
        return unbuildable;
    }
    if (camera.projectionMode() == ProjectionMode::Orthographic) {
        // task E.1.3: the PARALLEL arm. The direction is constant and the ORIGIN varies across the
        // image plane -- the exact inverse of the perspective arm below.
        const float halfHeight = camera.orthoHalfHeight();
        // GUARD 1, for F2's reason: orthoHalfHeight is distance * tan(fovY/2), and clampState
        // deliberately leaves a directly-set NaN in fovYValue for stateIsFinite() to sweep on the next
        // update() -- so tan(NaN/2) is NaN here. The perspective arm already does this for tanHalf.
        if (!std::isfinite(halfHeight)) {
            return unbuildable;
        }
        const float halfWidth = halfHeight * aspect;
        const Vec3 origin =
            camera.position() + (camera.right() * (ndc.x * halfWidth)) + (camera.up() * (ndc.y * halfHeight));
        const Vec3 direction = camera.forward();
        // GUARDS 2 AND 3. The DIRECTION needs one too, not only the origin: forward() is NaN whenever
        // yaw or pitch is poisoned, and normalizeOrZero returns NaN -- NOT zero -- for a NaN input
        // (vec3.hpp: `lenSq <= eps*eps` is false for NaN), so the test must happen HERE, before it.
        if (!allFinite(origin) || !allFinite(direction)) {
            return unbuildable;
        }
        // normalizeOrZero, never normalize: the latter ASSERTS on a zero-length vector (vec3.hpp D15).
        return Ray{.origin = origin, .direction = normalizeOrZero(direction)};
    }
    const float tanHalf = std::tan(camera.fovYRadians() * 0.5F);
    if (!std::isfinite(tanHalf)) {
        return unbuildable;
    }
    const Vec3 direction =
        camera.forward() + (camera.right() * (ndc.x * aspect * tanHalf)) + (camera.up() * (ndc.y * tanHalf));
    // normalizeOrZero returns NaN, NOT zero, for a NaN input (vec3.hpp: `lenSq <= eps*eps` is false
    // for NaN), so the finiteness test must happen HERE, before it -- A9.
    if (!allFinite(direction)) {
        return unbuildable;
    }
    // normalizeOrZero, never normalize: the latter ASSERTS on a zero-length vector and would abort a
    // Debug build (vec3.hpp D15). |forward + a*right + b*up|^2 == 1 + a^2 + b^2 >= 1 for an
    // orthonormal basis (INV-2 gives us one), so the zero branch is reachable only via the guards.
    return Ray{.origin = camera.position(), .direction = normalizeOrZero(direction)};
}

Vec3 dropPlacementPoint(const Ray& ray) noexcept {
    // FOUR ARMS, AND THE ORDER IS THE SPECIFICATION. Every one returns a finite Vec3 for every ray
    // viewportRay can build -- its direction is either exactly zero or a normalised finite vector and
    // its origin is the camera's clamped position -- because an infinite Transform::position would
    // poison worldMatrix, then bounds, then focusOn: the exact NaN-into-the-pose failure
    // Aabb::valid()'s finiteness half exists to stop.
    const Vec3 fallback = ray.origin + (ray.direction * DROP_FALLBACK_DISTANCE);
    if (lengthSquared(ray.direction) <= 0.0F) {
        return ray.origin;  // arm 1: viewportRay's documented unbuildable zero. Nothing better exists.
    }
    if (std::abs(ray.direction.y) < DROP_PLANE_EPSILON) {
        return fallback;  // arm 2: parallel to the ground plane -- it is never met
    }
    const float t = -ray.origin.y / ray.direction.y;
    if (!(t > 0.0F)) {
        return fallback;  // arm 3: the negated `>` also rejects a NaN t; the plane is behind the eye
    }
    const Vec3 point = ray.origin + (ray.direction * t);
    // arm 4: a camera 1e30 units up with a nearly-horizontal ray gives a FINITE t and an INFINITE
    // point, so this re-check is load-bearing rather than belt-and-braces.
    return allFinite(point) ? point : fallback;
}

bool rayLocalBoxHit(Vec3 origin, Vec3 direction, float halfExtent, float& outT) noexcept {
    // The half-extent form is now a THIN WRAPPER, so there is exactly one slab ladder in the tree. Its
    // own precondition survives verbatim -- `halfExtent > 0` is STRICTER than Aabb::valid() and rejects
    // a zero/negative/NaN extent before a box is built, which is what its published cases assert.
    if (!std::isfinite(halfExtent) || !(halfExtent > 0.0F)) {
        return false;
    }
    return rayLocalBoxHit(origin, direction,
                          Aabb{Vec3{-halfExtent, -halfExtent, -halfExtent}, Vec3{halfExtent, halfExtent, halfExtent}},
                          outT);
}

bool rayLocalBoxHit(Vec3 origin, Vec3 direction, const Aabb& box, float& outT) noexcept {
    // Aabb::valid() IS the precondition, restated nowhere: every component finite and min <= max on all
    // three axes. min == max on an axis is LEGAL and is the flat Plane primitive's own shape (task
    // 3.1.5); a strict min < max here makes every plane in every scene unclickable.
    if (!allFinite(origin) || !allFinite(direction) || !box.valid()) {
        return false;
    }
    // The max/min ladder rather than early returns, so an infinite 1/d (a ray parallel to a slab)
    // behaves correctly under IEEE -- and the explicit |d| < DIR_EPSILON branch, rather than relying
    // on 1/0 == inf, is what keeps the 0 * inf -> NaN path from ever being entered when the origin
    // sits exactly on a slab plane. That mattered for the fat Plane box (F19/D13) and matters MORE now
    // that the plane's box really is zero-thickness: NO EPSILON is added anywhere, because the ladder
    // already handles a zero-width slab -- a ray crossing it gets t1 == t2 and a ray inside it (o
    // exactly on both planes at once) takes the parallel-and-inside branch.
    float tMin = -INF;
    float tMax = INF;
    for (int axis = 0; axis < 3; ++axis) {
        const float o = axisValue(origin, axis);
        const float d = axisValue(direction, axis);
        const float lo = axisValue(box.min, axis);
        const float hi = axisValue(box.max, axis);
        if (std::abs(d) < DIR_EPSILON) {
            if (o < lo || o > hi) {
                return false;  // parallel to this slab and OUTSIDE it
            }
            continue;  // parallel and inside: this axis constrains nothing
        }
        const float inv = 1.0F / d;
        float t1 = (lo - o) * inv;
        float t2 = (hi - o) * inv;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax) {
            return false;
        }
    }
    // D3: ENTRY hits only. tMin <= 0 < tMax means the origin is INSIDE the box -- counting that as a
    // hit at t = 0 would make a box you have flown into win EVERY click at distance zero, forever,
    // and nothing else in the scene could be selected until you flew back out. It also matches what
    // the user SEES: the forward pipeline culls back faces (F20), so from inside a box you look
    // straight through it.
    if (!std::isfinite(tMin) || !(tMin > 0.0F)) {
        return false;
    }
    outT = tMin;
    return true;
}

PickResult pickEntity(const World& world, const EditorCamera& camera, const PickRequest& request) {
    const Ray ray = viewportRay(camera, request.ndc, request.aspect);
    if (lengthSquared(ray.direction) <= 0.0F) {
        return {};  // E6: an unbuildable ray is a guaranteed miss
    }
    const Mat4 viewProj = camera.projectionMatrix(request.aspect) * camera.viewMatrix();
    // A11: loop-invariant, so hoisted out of the walk below.
    const Vec2 clickPoints = ndcToViewportPoints(request.ndc, request.viewportSizePoints);
    // task E.1.3: the RAY's origin, not the camera's position. Under perspective viewportRay returns
    // `.origin = camera.position()` on every path (including the unbuildable one), so this is provably
    // a NO-OP in that mode -- and in orthographic it is the fix: the origin there sits on the eye
    // PLANE, offset across the image, so measuring a point candidate's depth from the eye POINT would
    // rank two entities at the same true depth differently depending on where they are on screen.
    // PK15 is its witness, in both directions.
    const Vec3 rayOrigin = ray.origin;

    PickResult mesh{};
    PickResult point{};
    float bestScreenDistance = INF;

    // eachEntity + has/get, NEVER a typed query walk -- see the header. This is also why the
    // signature can take a const World& at all (F15/F17).
    world.eachEntity([&](Entity e) {
        if (!world.alive(e)) {
            return;
        }
        const Mat4 model = worldMatrix(world, e);  // silent identity when untransformed (F16/E3)
        if (world.has<MeshRenderer>(e)) {          // silent for an unregistered type (F15)
            // task 3.1.5: ONE function decides the local box, shared with the frame walk and the
            // highlight (INV-D6). nullopt means the entity has a reference the editor cannot resolve
            // yet, and it FALLS THROUGH to the point/disc candidate below rather than returning --
            // otherwise an entity stays unclickable for the whole of its load, which is AC-34.
            const std::optional<Aabb> local = localBoundsFor(*world.get<MeshRenderer>(e), request.meshBounds);
            if (local.has_value()) {
                const float det = determinant(model);
                if (!std::isfinite(det) || std::abs(det) < DETERMINANT_EPSILON) {
                    return;  // E5 zero scale / E4 poisoned Transform: not pickable
                }
                const Mat4 inverseModel = inverse(model);
                float t = 0.0F;
                // D2: the local direction is deliberately NOT normalised, so the t that comes back is
                // in WORLD units and hits from entities with wildly different scales are comparable.
                if (!rayLocalBoxHit(transformPoint(inverseModel, ray.origin),
                                    transformDirection(inverseModel, ray.direction), *local, t)) {
                    return;
                }
                // D16's tie-break, spelled `!(t > best)` rather than `t == best`: equivalent for the
                // finite values that reach here, and it keeps a float equality comparison out of the
                // tree.
                if (!mesh.hit() || t < mesh.distance || (!(t > mesh.distance) && e.index < mesh.entity.index)) {
                    mesh = PickResult{.entity = e, .distance = t, .isPoint = false};
                }
                return;
            }
        }
        // D5: the non-mesh candidate, reached by an entity with no MeshRenderer and -- since task
        // 3.1.5 -- by one whose reference has not resolved yet. entityBounds(..., false).center() IS
        // the world translation for both (F3): with no lookup passed, contribute() takes the same
        // POINT branch localBoundsFor's nullopt just took here, so the two agree by construction and
        // nothing new is written for the unresolved case.
        const Aabb bounds = entityBounds(world, e, /*includeDescendants=*/false);
        if (!bounds.valid()) {
            return;
        }
        Vec2 screen{};
        if (!projectToViewport(viewProj, camera.projectionMode(), bounds.center(), request.viewportSizePoints,
                               screen)) {
            return;  // at or behind the eye
        }
        const float screenDistance = length(screen - clickPoints);
        // A10: the NaN-safe NEGATED `<=`. The positive `> radius` form accepts a NaN distance (every
        // comparison with NaN is false) and would carry it into the tie-break below.
        if (!(screenDistance <= request.pointRadiusPoints)) {
            return;
        }
        // Among point candidates the smallest SCREEN distance wins -- they are competing for a click,
        // not for depth. Ties break on lowest entity index (D16).
        if (screenDistance < bestScreenDistance ||
            (!(screenDistance > bestScreenDistance) && e.index < point.entity.index)) {
            bestScreenDistance = screenDistance;
            point = PickResult{.entity = e, .distance = length(bounds.center() - rayOrigin), .isPoint = true};
        }
    });

    // D5's depth rule, and it is NOT optional: markers are invisible until selected (D8), so a light
    // hidden behind a wall stealing every click on the wall would be inexplicable to the user. A point
    // candidate wins iff there is no mesh hit, or it is not BEHIND the one there is. No bias constant,
    // no fudge -- the plain comparison.
    if (point.hit() && (!mesh.hit() || point.distance <= mesh.distance)) {
        return point;
    }
    return mesh;
}

// See the header for why `alreadySelected` is unnamed and why it nonetheless stays in the signature.
PickAction pickSelectionAction(bool hit, bool /*alreadySelected*/, bool ctrlOrCmd, bool shift) noexcept {
    if (!hit) {
        return (ctrlOrCmd || shift) ? PickAction::None : PickAction::Clear;
    }
    if (ctrlOrCmd) {
        return PickAction::Toggle;  // Ctrl/Cmd BEFORE Shift, so the matrix is order-independent
    }
    if (shift) {
        return PickAction::Add;  // grows only -- Selection::add is already a no-op when present
    }
    return PickAction::Select;
}

void applyPickAction(Selection& selection, PickAction action, Entity entity) {
    switch (action) {
        case PickAction::None:
            break;
        case PickAction::Select:
            selection.set(entity);
            break;
        case PickAction::Toggle:
            selection.toggle(entity);
            break;
        case PickAction::Add:
            selection.add(entity);
            break;
        case PickAction::Clear:
            selection.clear();
            break;
    }
}

}  // namespace engine::editor

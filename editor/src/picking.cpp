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

bool projectToViewport(const Mat4& viewProj, Vec3 worldPoint, Vec2 viewportSizePoints, Vec2& outPoints) noexcept {
    const Vec4 clip = viewProj * toVec4(worldPoint, 1.0F);
    // F13: w > 0 means "in front of the eye" under this engine's RH / -Z / clip-Z-in-[0,1] convention.
    // The near plane is z_clip = 0, NOT z_clip = -w, so this is the correct and sufficient test.
    if (!(clip.w > CLIP_W_EPSILON)) {
        return false;
    }
    const Vec2 points = ndcToViewportPoints(Vec2{clip.x / clip.w, clip.y / clip.w}, viewportSizePoints);
    if (!allFinite(points)) {
        return false;  // E4
    }
    outPoints = points;
    return true;
}

bool clipSegmentToNearPlane(Vec4& a, Vec4& b) noexcept {
    const bool aInFront = a.w > CLIP_W_EPSILON;
    const bool bInFront = b.w > CLIP_W_EPSILON;
    if (aInFront && bInFront) {
        return true;
    }
    if (!aInFront && !bInFront) {
        return false;  // the whole segment is at or behind the eye; the edge does not exist on screen
    }
    Vec4& behind = aInFront ? b : a;
    const Vec4& front = aInFront ? a : b;
    const float denominator = front.w - behind.w;
    if (!(denominator > 0.0F)) {
        return false;  // unreachable given aInFront != bInFront; NaN-safe and total anyway
    }
    // D14: interpolate IN CLIP SPACE, BEFORE the perspective divide. The clipped endpoint lands on
    // w == CLIP_W_EPSILON exactly and stays on the original clip-space line -- which is what keeps
    // the drawn edge straight. S8 seeds the post-divide lerp; this is what catches it.
    const float t = (CLIP_W_EPSILON - behind.w) / denominator;
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

bool rayLocalBoxHit(Vec3 origin, Vec3 direction, float halfExtent, float& outT) noexcept {
    if (!allFinite(origin) || !allFinite(direction) || !(halfExtent > 0.0F)) {
        return false;
    }
    // The max/min ladder rather than early returns, so an infinite 1/d (a ray parallel to a slab)
    // behaves correctly under IEEE -- and the explicit |d| < DIR_EPSILON branch, rather than relying
    // on 1/0 == inf, is what keeps the 0 * inf -> NaN path from ever being entered when the origin
    // sits exactly on a slab plane. That matters today for the Plane primitive (F19/D13) and will
    // still matter when 3.1.x gives it a zero-thickness box.
    float tMin = -INF;
    float tMax = INF;
    for (int axis = 0; axis < 3; ++axis) {
        const float o = axisValue(origin, axis);
        const float d = axisValue(direction, axis);
        if (std::abs(d) < DIR_EPSILON) {
            if (o < -halfExtent || o > halfExtent) {
                return false;  // parallel to this slab and OUTSIDE it
            }
            continue;  // parallel and inside: this axis constrains nothing
        }
        const float inv = 1.0F / d;
        float t1 = (-halfExtent - o) * inv;
        float t2 = (halfExtent - o) * inv;
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

}  // namespace engine::editor

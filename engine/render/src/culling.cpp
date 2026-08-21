// engine/render/src/culling.cpp — task 3.6.1: the culling vocabulary behind culling.hpp. Pure
// arithmetic over engine math types. Nothing here allocates, logs, recurses, touches a GPU or holds
// static mutable state, and there is no profiling include either (a deliberate absence, matching
// animation.cpp and skinning.cpp: this is a few dozen float operations and a Tracy zone would cost
// more than the work it measured). Every entry point is TOTAL — a hostile matrix or a corrupt box
// yields a defined answer, never UB, because at Phase 5 these numbers come out of a .pak.

#include <aero/render/culling.hpp>

#include <cmath>
#include <cstddef>

namespace engine::render {

bool Aabb::valid() const noexcept {
    // Finiteness FIRST, then ordering. The ordering half alone already rejects the cook's inverted
    // sentinel (+inf min, -inf max) and any NaN corner (NaN <= x is false in both directions); the
    // finiteness half's own catch is the ORDERED-infinite box, e.g. min.x = -inf with max.x = +inf,
    // which passes min <= max and whose center() is inf - inf = NaN.
    return std::isfinite(min.x) && std::isfinite(min.y) && std::isfinite(min.z) && std::isfinite(max.x) &&
           std::isfinite(max.y) && std::isfinite(max.z) && min.x <= max.x && min.y <= max.y && min.z <= max.z;
}

Vec3 Aabb::center() const noexcept { return (min + max) * 0.5F; }

Vec3 Aabb::halfExtent() const noexcept { return (max - min) * 0.5F; }

float signedDistance(const Plane& plane, Vec3 p) noexcept { return dot(plane.normal, p) + plane.d; }

bool Frustum::valid() const noexcept {
    for (const Plane& p : planes) {
        // d is checked alongside the normal: a viewProj with an infinite translation column yields
        // finite normals and infinite d, and without this the whole frustum would pass, every
        // s + r comparison against NaN would be false, and culling would silently draw everything.
        if (!std::isfinite(p.normal.x) || !std::isfinite(p.normal.y) || !std::isfinite(p.normal.z) ||
            !std::isfinite(p.d)) {
            return false;
        }
        if (lengthSquared(p.normal) <= EPSILON * EPSILON) {
            return false;
        }
    }
    return true;
}

const Plane& Frustum::plane(FrustumPlane which) const noexcept {
    const auto index = static_cast<std::size_t>(which);
    // FrustumPlane::Count (or any out-of-range cast) reads slot 0 rather than past the array. The
    // caller asked a question with no answer; a defined one beats UB and costs one compare.
    return planes[index < planes.size() ? index : 0];
}

Frustum extractFrustum(const Mat4& viewProj) noexcept {
    // Gribb-Hartmann needs the ROWS of viewProj, and Mat4 is column-major, so transpose once and
    // read its columns. `rows.columns[i]` is exactly {m.columns[0][i], ..., m.columns[3][i]}.
    const Mat4 rows = transpose(viewProj);
    const Vec4& r0 = rows.columns[0];
    const Vec4& r1 = rows.columns[1];
    const Vec4& r2 = rows.columns[2];
    const Vec4& r3 = rows.columns[3];

    // Normalise BOTH halves by the same |n|: dividing the normal alone leaves d in the raw matrix's
    // units, which shifts every plane's position by ~1 per mille for a typical projection -- small
    // enough to look right and wrong everywhere.
    const auto normalisedPlane = [](const Vec4& raw) noexcept -> Plane {
        const Vec3 n = xyz(raw);
        const float k = std::sqrt(lengthSquared(n));
        if (k <= EPSILON) {
            return Plane{};  // degenerate; Frustum::valid() reports it. NaN k falls through below.
        }
        return Plane{n / k, raw.w / k};
    };

    Frustum frustum{};
    // Slot order IS FrustumPlane's order. NEAR is r2 ALONE -- the clip volume is 0 <= z <= w
    // (ADR-005/SDL_GPU), not GL's -w <= z <= w, so the r3 + r2 form would put the near plane at
    // roughly half the distance and cull geometry that is in front of the camera.
    frustum.planes = {normalisedPlane(r3 + r0), normalisedPlane(r3 - r0), normalisedPlane(r3 + r1),
                      normalisedPlane(r3 - r1), normalisedPlane(r2),      normalisedPlane(r3 - r2)};
    return frustum;
}

Aabb transformAabb(const Mat4& model, const Aabb& local) noexcept {
    if (!local.valid()) {
        return local;  // the predicate propagates; never fabricate corners from a NaN centre
    }
    const Vec3 center = transformPoint(model, local.center());
    const Vec3 e = local.halfExtent();
    // Arvo: he'[i] = sum_j |A[i][j]| * e[j], with A the upper-left 3x3 and A[i][j] == columns[j][i],
    // which is the same thing as combining the componentwise-ABSOLUTE basis columns by e. Written
    // out per component because Vec3 has no componentwise abs (nothing is added to engine/core for
    // this). The abs is what makes a MIRROR correct rather than inverted.
    const Vec3 absX{std::abs(model.columns[0].x), std::abs(model.columns[0].y), std::abs(model.columns[0].z)};
    const Vec3 absY{std::abs(model.columns[1].x), std::abs(model.columns[1].y), std::abs(model.columns[1].z)};
    const Vec3 absZ{std::abs(model.columns[2].x), std::abs(model.columns[2].y), std::abs(model.columns[2].z)};
    const Vec3 half = (absX * e.x) + (absY * e.y) + (absZ * e.z);
    return Aabb{center - half, center + half};
}

bool isVisible(const Frustum& frustum, const Aabb& world) noexcept {
    // Precedence, decided once: an invalid FRUSTUM wins. It means "we cannot test", and the safe
    // answer to that is to draw -- culling to black on a degenerate projection is the one failure
    // mode this whole feature must never have. Only then does an invalid BOX mean "nothing to draw".
    if (!frustum.valid()) {
        return true;
    }
    if (!world.valid()) {
        return false;
    }
    const Vec3 center = world.center();
    const Vec3 e = world.halfExtent();
    for (const Plane& plane : frustum.planes) {
        const float s = signedDistance(plane, center);
        const float r =
            std::abs(plane.normal.x) * e.x + std::abs(plane.normal.y) * e.y + std::abs(plane.normal.z) * e.z;
        if (s + r < 0.0F) {
            return false;  // entirely on the outside of this plane -- one rejection is enough
        }
    }
    return true;  // tangency (s + r == 0) counts as visible
}

Aabb toAabb(const assets::CookedBounds& bounds) noexcept { return Aabb{bounds.min, bounds.max}; }

}  // namespace engine::render

// engine/render/src/shadow.cpp — task 3.6.2: the directional shadow fit behind shadow.hpp. Pure
// arithmetic over engine math types. Nothing here allocates, logs, recurses, touches a GPU or holds
// static mutable state, and there is no profiling include either (the deliberate absence
// culling.cpp, animation.cpp and skinning.cpp all share: this is a few dozen float operations per
// VIEW, and a Tracy zone would cost more than the work it measured).
//
// TWO THINGS THIS FILE MUST NEVER DO, both of which abort a Debug build rather than misbehaving:
// call engine::ortho with a non-positive extent or an inverted depth range, and call
// engine::normalize with a zero-length vector. Both are guarded before the call, not after.

#include <aero/render/shadow.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace engine::render {

namespace {

// Every element finite. Used on the two camera matrices before anything is inverted, and on
// lightView after it is built -- glm::lookAtRH normalises cross(f, up) with no guard, so a
// degenerate basis arrives as a NaN matrix rather than as a diagnostic.
[[nodiscard]] bool allFinite(const Mat4& m) noexcept {
    for (const Vec4& column : m.columns) {
        if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) ||
            !std::isfinite(column.w)) {
            return false;
        }
    }
    return true;
}

// The NDC z of the view-space depth `d` (a POSITIVE distance along -Z), CLAMPED to [0, 1] -- which,
// under ADR-005's clip volume, IS clamping to the camera's own near and far planes, whatever the
// projection. A non-finite result -- the perspective divide by w == 0 that d == 0 produces -- clamps
// to 0, the near plane, rather than propagating.
[[nodiscard]] float ndcDepthOf(const Mat4& proj, float d) noexcept {
    const Vec4 clip = proj * Vec4{0.0F, 0.0F, -d, 1.0F};
    if (clip.w == 0.0F || !std::isfinite(clip.w) || !std::isfinite(clip.z)) {
        return 0.0F;
    }
    const float z = clip.z / clip.w;
    if (!std::isfinite(z)) {
        return 0.0F;
    }
    return std::clamp(z, 0.0F, 1.0F);
}

// (ndc, 1) through invViewProj WITH the perspective divide. nullopt on a zero or non-finite w, or on
// any non-finite component -- never a fabricated corner.
[[nodiscard]] std::optional<Vec3> unprojectNdc(const Mat4& invViewProj, Vec3 ndc) noexcept {
    const Vec4 h = invViewProj * Vec4{ndc.x, ndc.y, ndc.z, 1.0F};
    if (h.w == 0.0F || !std::isfinite(h.w)) {
        return std::nullopt;
    }
    const Vec3 p{h.x / h.w, h.y / h.w, h.z / h.w};
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        return std::nullopt;
    }
    return p;
}

}  // namespace

Vec3 shadowUpAxis(Vec3 lightDirection) noexcept {
    // normalizeOrZero, never normalize: normalize() ASSERTS on a zero-length vector (vec3.hpp), and
    // every function in this file is total.
    const Vec3 unit = normalizeOrZero(lightDirection);
    return std::abs(unit.y) > SHADOW_UP_AXIS_THRESHOLD ? Vec3{0.0F, 0.0F, 1.0F} : Vec3{0.0F, 1.0F, 0.0F};
}

std::optional<std::array<Vec3, 8>> frustumCornersWorld(const CameraView& camera, float sliceNear,
                                                       float sliceFar) noexcept {
    if (!allFinite(camera.view) || !allFinite(camera.proj)) {
        return std::nullopt;
    }
    const Mat4 invViewProj = inverse(camera.proj * camera.view);
    if (!allFinite(invViewProj)) {
        return std::nullopt;  // a singular product: glm divides by a zero determinant
    }

    float ndcNear = ndcDepthOf(camera.proj, sliceNear);
    float ndcFar = ndcDepthOf(camera.proj, sliceFar);
    if (ndcFar < ndcNear) {
        std::swap(ndcNear, ndcFar);  // a caller that passed them backwards gets a defined slice
    }

    // THE ORDER IS THE CONTRACT (shadow.hpp): index = (z * 4) + (y * 2) + x.
    std::array<Vec3, 8> corners{};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const float x = (i & 1U) != 0U ? 1.0F : -1.0F;
        const float y = (i & 2U) != 0U ? 1.0F : -1.0F;
        const float z = (i & 4U) != 0U ? ndcFar : ndcNear;
        const std::optional<Vec3> world = unprojectNdc(invViewProj, Vec3{x, y, z});
        if (!world.has_value()) {
            return std::nullopt;
        }
        corners[i] = *world;
    }
    return corners;
}

Sphere boundingSphere(std::span<const Vec3> points) noexcept {
    if (points.empty()) {
        return {};
    }
    Vec3 sum{};
    for (const Vec3 p : points) {
        sum = sum + p;
    }
    const Sphere sphere{sum * (1.0F / static_cast<float>(points.size())), 0.0F};
    float worst = 0.0F;
    for (const Vec3 p : points) {
        worst = std::max(worst, lengthSquared(p - sphere.center));
    }
    return Sphere{sphere.center, std::sqrt(worst)};
}

Vec3 snapToTexel(Vec3 lightAxisPoint, float texelWorldSize) noexcept {
    if (!(texelWorldSize > 0.0F) || !std::isfinite(texelWorldSize)) {
        return lightAxisPoint;  // no grid to snap to; fabricating one would be worse
    }
    return Vec3{std::floor(lightAxisPoint.x / texelWorldSize) * texelWorldSize,
                std::floor(lightAxisPoint.y / texelWorldSize) * texelWorldSize, lightAxisPoint.z};
}

ShadowFit fitDirectionalShadow(const CameraView& camera, Vec3 lightDirection, float shadowDistance,
                               std::uint32_t resolution, const Aabb& casterBoundsWorld) noexcept {
    // STEP 0 -- the degenerate classes, all of them, before anything is computed. Every one returns
    // a default ShadowFit: valid == false and every other field default. Nothing here logs; the
    // caller owns the latched WARN, because only the caller knows whether an invalid fit is a
    // problem (a disabled light is not).
    if (resolution == 0) {
        return {};
    }
    if (!(shadowDistance > 0.0F) || !std::isfinite(shadowDistance)) {
        return {};  // written as !(x > 0) so a NaN takes this arm too
    }
    if (!std::isfinite(lightDirection.x) || !std::isfinite(lightDirection.y) || !std::isfinite(lightDirection.z)) {
        return {};
    }
    const Vec3 dir = normalizeOrZero(lightDirection);
    if (lengthSquared(dir) <= 0.0F) {
        return {};  // zero-length, or so short that normalizeOrZero refused
    }

    // STEP 1 -- the camera's frustum slice, in WORLD space. sliceNear 0 means "the camera's own near
    // plane"; both ends are clamped in NDC inside frustumCornersWorld, which is also where the
    // non-invertible and non-finite camera classes are refused.
    const std::optional<std::array<Vec3, 8>> corners = frustumCornersWorld(camera, 0.0F, shadowDistance);
    if (!corners.has_value()) {
        return {};
    }

    // STEP 2 -- the stabiliser. The radius is invariant under camera ROTATION, so texelWorldSize
    // does not change as the view turns and the snap in step 4 snaps to a FIXED grid.
    const Sphere sphere = boundingSphere(*corners);
    if (!std::isfinite(sphere.radius) || sphere.radius <= 0.0F) {
        // No automated witness, and that is recorded rather than claimed (the 3.6.1 !isfinite(k)
        // precedent): reaching a zero radius needs eight coincident corners, which needs a singular
        // proj * view, which step 1 already refused. It ships because engine::ortho ASSERTS
        // right > left, so a zero extent would ABORT a Debug build -- the configuration CI
        // sanitizes -- and this function is public and callable with a hand-built CameraView.
        return {};
    }
    const float radius = sphere.radius;

    // STEP 5 (computed before the basis, because the eye needs it) -- how far the furthest caster
    // lies BEHIND the sphere along -dir, i.e. toward the light. The EYE moves; the near plane never
    // does, which is what makes a caster behind the eye unrepresentable rather than clipped.
    float casterBack = 0.0F;
    if (casterBoundsWorld.valid()) {
        const Vec3 n = dir * -1.0F;  // toward the light
        // The sphere's far face, measured toward the light. Plane's normalisation is what makes
        // signedDistance a TRUE world distance here -- culling.hpp's own note says this fit is why
        // it normalises, and this is that use.
        const Plane back{n, -dot(n, sphere.center) - radius};
        const Vec3 c = casterBoundsWorld.center();
        const Vec3 e = casterBoundsWorld.halfExtent();
        // max over the eight corners of dot(n, p) IS dot(n, center) + the support term -- exactly,
        // not approximately, and in three multiplies rather than twenty-four. isVisible's own
        // s + r idiom.
        const float support = (std::abs(n.x) * e.x) + (std::abs(n.y) * e.y) + (std::abs(n.z) * e.z);
        casterBack = std::max(0.0F, signedDistance(back, c) + support);
        if (!std::isfinite(casterBack)) {
            casterBack = 0.0F;  // an ordered-infinite box passes valid()'s ordering half; ignore it
        }
    }

    // STEP 3 -- the light basis, on the world axis least parallel to the sun.
    const float eyeBack = radius + casterBack + SHADOW_NEAR_MARGIN;
    const Vec3 eye = sphere.center - (dir * eyeBack);
    ShadowFit fit;
    fit.lightView = lookAt(eye, sphere.center, shadowUpAxis(dir));
    if (!allFinite(fit.lightView)) {
        return {};  // glm::lookAtRH normalises cross(f, up) with no guard -> a NaN matrix
    }

    // STEP 4 -- THE TEXEL SNAP, onto a WORLD-ANCHORED lattice.
    //
    // THE TRAP THIS AVOIDS, stated because it is invisible and it cost this task a step: the obvious
    // form snaps transformPoint(fit.lightView, sphere.center), and THAT QUANTITY IS IDENTICALLY
    // (0, 0, -k) FOR EVERY INPUT -- glm::lookAtRH puts the eye at the origin looking down -Z at the
    // target, so the target lands on the axis. Snapping it quantises nothing; worse, std::floor turns
    // the SIGN of the +-2e-6 rounding noise into a discrete ONE-FULL-TEXEL jump, so the lattice hops
    // between two positions frame to frame and the edge crawls exactly as much as with no snap at
    // all. Measured over a 360-step yaw sweep at 2048: the volume's left edge sits up to 0.4975
    // texels off the world lattice that way, and 2.3e-13 off it this way.
    //
    // What must be quantised is the centre's position on the light's LATERAL AXES -- dot(s, c) and
    // dot(u, c), which ARE camera-dependent -- so this reads the basis ROWS rather than applying the
    // matrix. Flooring the ortho's MIN CORNER onto that lattice pins every texel boundary to a fixed
    // set of world planes: as the camera turns, the volume slides in whole-texel steps and each texel
    // keeps covering the same world slab.
    //
    // THE SIGN IS SUBTRACT. A lightView-space lateral coordinate X sits at the world-anchored value
    // X + a, so the left edge at X = -r - residueX lands on a - r - residueX, which IS
    // floor((a - r) / texel) * texel. Adding the residue lands on a multiple of nothing, and the
    // difference is invisible by eye (seed SH8b).
    //
    // The quantum is 2r / resolution, NOT r / resolution: the ortho spans 2r across `resolution`
    // texels.
    fit.texelWorldSize = (2.0F * radius) / static_cast<float>(resolution);
    const Vec3 sAxis{fit.lightView.columns[0].x, fit.lightView.columns[1].x, fit.lightView.columns[2].x};
    const Vec3 uAxis{fit.lightView.columns[0].y, fit.lightView.columns[1].y, fit.lightView.columns[2].y};
    const Vec3 anchor{dot(sAxis, sphere.center) - radius, dot(uAxis, sphere.center) - radius, 0.0F};
    const Vec3 snapped = snapToTexel(anchor, fit.texelWorldSize);
    const float residueX = anchor.x - snapped.x;  // in [0, texelWorldSize)
    const float residueY = anchor.y - snapped.y;

    // STEP 5 (applied) -- the depth range is [NEAR_MARGIN, 2r + casterBack + NEAR_MARGIN], so with no
    // caster the sphere's near face lands EXACTLY on the near plane and its far face EXACTLY on the
    // far plane. zNear is always the same constant, by construction.
    //
    // The lateral bounds are the sphere's SHIFTED, never widened: the width stays exactly 2r, so
    // texelWorldSize is still exactly 2r / resolution (AC-14), and the price is that up to ONE TEXEL
    // of the sphere's +x/+y side falls outside the volume. Accepted knowingly -- the sphere already
    // circumscribes the frustum slice by 20-35 %, and one texel of 2048 is 0.05 % of the width.
    // ortho's own asserts still hold by construction: right - left == 2 * radius > 0 either way.
    fit.lightProj = ortho(-radius - residueX, radius - residueX, -radius - residueY, radius - residueY,
                          SHADOW_NEAR_MARGIN, (2.0F * radius) + casterBack + SHADOW_NEAR_MARGIN);
    fit.valid = true;
    return fit;
}

Mat4 shadowViewProj(const ShadowFit& fit) noexcept {
    return fit.valid ? fit.lightProj * fit.lightView : Mat4::identity();
}

}  // namespace engine::render

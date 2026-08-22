#pragma once
// Aero Engine — render culling vocabulary (task 3.6.1): the axis-aligned box, the plane, the
// frustum, the [0,1]-depth Gribb-Hartmann extraction, the conservative (Arvo) box transform and
// the six-plane visibility test. Everything here is PURE and GPU-free: no rhi type, no Device, no
// Frame, no logging, no allocation, no static mutable state — every function is callable from a
// tier-0 test with no GPU.
//
// SIGN CONVENTION, stated once: a point p is INSIDE a plane  <=>  dot(plane.normal, p) + plane.d >= 0.
//
// NORMALISATION: extractFrustum normalises every plane. The boolean visibility test does NOT
// require it — s and r scale by the same positive factor, so sign(s + r) is invariant — it exists
// so signedDistance is a true distance (task 3.6.2's shadow fit measures the caster extension with
// it, in world units) and so the six planes' coefficients stay within one order of magnitude of
// each other.

#include <aero/assets/cooked_mesh.hpp>  // assets::CookedBounds — the ONE assets:: name here
#include <aero/core/math.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace engine::render {

// An axis-aligned bounding box, in whatever space the caller is holding it in (local for a
// registry entry, world after transformAabb). No space tag: the two spaces never meet inside a
// single expression here, and a tag would have to be threaded through the cooked format too.
struct Aabb {
    Vec3 min{};
    Vec3 max{};
    // All six components FINITE and min <= max on every axis. The ordering half alone rejects the
    // cook's inverted sentinel AND any NaN corner (NaN <= x is false both ways); the finiteness
    // half's unique catch is an ORDERED-infinite box (e.g. max.x = +inf), whose center() is NaN.
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] Vec3 center() const noexcept;      // (min + max) * 0.5F — requires valid(); does not defend
    [[nodiscard]] Vec3 halfExtent() const noexcept;  // (max - min) * 0.5F — requires valid(); does not defend
};
static_assert(sizeof(Aabb) == 6 * sizeof(float));
static_assert(std::is_standard_layout_v<Aabb>);
static_assert(std::is_trivially_copyable_v<Aabb>);

struct Plane {
    Vec3 normal{};
    float d = 0.0F;  // INSIDE <=> dot(normal, p) + d >= 0 (the header-top convention)
};
static_assert(sizeof(Plane) == 4 * sizeof(float));

[[nodiscard]] float signedDistance(const Plane& plane, Vec3 p) noexcept;  // dot(normal, p) + d

// Fixed plane order — tests name planes, they never count.
enum class FrustumPlane : std::uint8_t { Left, Right, Bottom, Top, Near, Far, Count };
// NEVER add a toString(FrustumPlane) on this or any engine header: doctest's unqualified
// stringifier finds it by ADL and every lane hard-fails inside doctest.h (the standing rule in
// .claude/rules/ci-portability.md). A label helper, if one is ever needed, is frustumPlaneLabel.

struct Frustum {
    std::array<Plane, static_cast<std::size_t>(FrustumPlane::Count)> planes{};
    // Every normal finite AND longer than EPSILON, and every d finite. The d half matters on its
    // own: a viewProj with an infinite translation column yields six FINITE normals with infinite
    // d, every s = dot(n, c) + d is +-inf or NaN, `s + r < 0` is false for NaN, and the draw would
    // degrade to all-visible SILENTLY. Checking d makes that class disable culling loudly instead.
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const Plane& plane(FrustumPlane which) const noexcept;
};

// Gribb-Hartmann for ADR-005's clip volume (-w <= x,y <= w, 0 <= z <= w). With
// row_i = {m.columns[0][i], m.columns[1][i], m.columns[2][i], m.columns[3][i]}:
//   left = r3 + r0   right = r3 - r0   bottom = r3 + r1   top = r3 - r1
//   NEAR = r2 ALONE  (the r3 + r2 form is the GL [-w,w] convention this engine has never used)
//   far  = r3 - r2
// Every plane is normalised. The only rows refused are GENUINELY degenerate ones -- zero-length, or
// carrying inf/NaN -- never merely SHORT ones: these are raw projection coefficients, and the far
// row's length is zNear/(zFar - zNear), which shrinks with the depth ratio. Applying a
// normalised-vector tolerance here rejects valid wide-range cameras (recorded in docs/10's 3.6.1
// entry). Frustum::valid()'s own EPSILON length test is a different question and is correct, because
// it runs on normals this function has already normalised.
[[nodiscard]] Frustum extractFrustum(const Mat4& viewProj) noexcept;

// Arvo: worldCenter = transformPoint(model, c); worldHalfExtent = |A| * e (componentwise absolute
// value of the 3x3 block). Conservative under rotation (the AABB around the OBB); exact under
// translation and axis-aligned scale; CORRECT under mirror (negative determinant) and shear — the
// abs discards the sign by construction, and WITHOUT it a mirrored box comes out INSIDE OUT (min
// above max), which isVisible then reads as "nothing to draw", so an on-screen mirrored object
// disappears. An invalid input yields an invalid output (the predicate propagates; no NaN corners).
[[nodiscard]] Aabb transformAabb(const Mat4& model, const Aabb& local) noexcept;

// Rejection on the first plane with s + r < 0, where s = signedDistance(plane, center) and
// r = |n.x|*he.x + |n.y|*he.y + |n.z|*he.z. Tangency (s + r == 0) is VISIBLE.
// Precedence: !frustum.valid() -> true for EVERYTHING (the belt-and-braces arm, checked FIRST, so
// a degenerate projection never culls to black); then !world.valid() -> false (an empty submesh
// has nothing to draw).
[[nodiscard]] bool isVisible(const Frustum& frustum, const Aabb& world) noexcept;

// A field copy and nothing else — the registry stores the file's numbers verbatim.
[[nodiscard]] Aabb toAabb(const assets::CookedBounds& bounds) noexcept;

}  // namespace engine::render

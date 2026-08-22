#pragma once
// Aero Engine — directional shadow fitting (task 3.6.2): the light basis, the camera-frustum corner
// unprojection, the rotation-invariant bounding sphere, the texel snap and the orthographic fit.
// Everything here is PURE and GPU-free: no rhi type, no Device, no Frame, no logging, no allocation,
// no static mutable state — every function is callable from a tier-0 test with no GPU. This is
// engine/render's THIRD pure module (after animation, 3.5.2, and culling, 3.6.1) and the SECOND that
// names no rhi type at all.
//
// EVERY FUNCTION IS TOTAL. A hostile matrix, a zero-length direction or a corrupt caster box yields a
// defined answer -- an invalid ShadowFit -- never UB, never an assert and never a log line. That
// matters more than usual here: engine::ortho and engine::perspective ASSERT their own preconditions
// (glm_backend.cpp), so a fit that reached ortho() with a zero extent would ABORT a Debug build,
// which is the configuration CI sanitizes.
//
// WHAT LIVES HERE AND WHAT DOES NOT: this header exports geometry. The shadow texture, the
// comparison sampler, the two depth pipelines, the depth pass and the caster walk all live in
// forward_renderer.cpp, because none of them can be written without a Device.

#include <aero/core/math.hpp>
#include <aero/render/culling.hpp>   // Aabb, Plane, signedDistance -- used exactly as written (D17)
#include <aero/render/lighting.hpp>  // CameraView

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace engine::render {

// One world unit of head-room in front of the light eye, and the ortho's zNear. ABSOLUTE rather than
// a fraction of the fit radius, deliberately: the near plane must be the SAME small positive
// constant for every view, which is what makes a caster behind the light eye unrepresentable rather
// than clipped -- the caster extension moves the EYE, never this. Exactly representable in binary,
// so every literal the tier-0 battery pins is exact rather than nearly so.
inline constexpr float SHADOW_NEAR_MARGIN = 1.0F;

// The up-axis switch: a sun within acos(0.99) == 8.11 degrees of straight down gets +Z instead of
// +Y, so lookAt() never receives an `up` parallel to its forward axis. This is the one place a naive
// implementation silently produces a NaN matrix (glm::lookAtRH normalises cross(f, up) with no
// guard), which is why fitDirectionalShadow checks lightView for finiteness afterwards as well.
inline constexpr float SHADOW_UP_AXIS_THRESHOLD = 0.99F;

// The world axis least parallel to `lightDirection`. The predicate is on the NORMALISED y: this
// function is public and inspectable on its own, and on a raw y a long, nearly-horizontal vector
// like (100, 0.995, 0) would wrongly take the straight-down branch. A zero-length input yields +Y,
// which is harmless -- fitDirectionalShadow rejects a zero direction before it gets here.
[[nodiscard]] Vec3 shadowUpAxis(Vec3 lightDirection) noexcept;

// The eight WORLD-space corners of `camera`'s frustum restricted to the VIEW-depth slice
// [sliceNear, sliceFar], by unprojecting the NDC cube through inverse(proj * view) with the
// perspective divide applied. Correct for a perspective AND an orthographic projection: the slice is
// built in NDC, never in view depth, so nothing here assumes a projection's form.
//
// BOTH ENDS ARE CLAMPED to the camera's own depth range, and the clamp is what makes that possible
// without a zNear/zFar CameraView does not carry: under ADR-005's clip volume, ndc.z == 0 IS the
// near plane and ndc.z == 1 IS the far plane, whatever the projection, so clamping the projected
// depth into [0, 1] IS clamping the view depth into [zNear, zFar]. Pass sliceNear == 0 to mean "the
// camera's own near plane". If the two ends arrive out of order they are swapped, so a caller that
// passes them backwards gets a defined slice rather than an inverted one.
//
// CORNER ORDER IS FIXED AND IS PART OF THE CONTRACT, because the tier-0 battery pins the array
// against literals:  index = (z * 4) + (y * 2) + x  over ndc x in {-1,+1}, y in {-1,+1},
// z in {near, far}, i.e.
//     0 (-1,-1,n)  1 (+1,-1,n)  2 (-1,+1,n)  3 (+1,+1,n)
//     4 (-1,-1,f)  5 (+1,-1,f)  6 (-1,+1,f)  7 (+1,+1,f)
//
// nullopt when proj * view is not invertible, when any element of view or proj is non-finite, or
// when any corner comes back non-finite -- never a fabricated corner.
[[nodiscard]] std::optional<std::array<Vec3, 8>> frustumCornersWorld(const CameraView& camera, float sliceNear,
                                                                     float sliceFar) noexcept;

struct Sphere {
    Vec3 center{};
    float radius = 0.0F;
};

// CENTROID + MAX RADIUS. Contains every input point BY CONSTRUCTION (the radius is the maximum
// distance), and its radius is invariant under a RIGID ROTATION of the input -- exactly, in exact
// arithmetic, and to a few ulp in IEEE. That invariance is HALF the stabilisation argument: the
// camera-frustum slice's corners rotate rigidly as the camera turns, so the fit's extent, and
// therefore the world SIZE of a texel, does not change.
//
// It is only half, and saying so is load-bearing. A texel of constant size on a lattice that slides
// continuously still crawls. The other half is fitDirectionalShadow's step 6, which pins the
// lattice's POSITION to the world by flooring the ortho's min corner onto a world-anchored grid. A
// stable extent without that snap is a shadow edge that shimmers at frame rate; the snap without a
// stable extent is a grid whose spacing changes under it. Both are required.
//
// It is NOT the minimal enclosing sphere, and neither alternative is wanted. Ritter's result depends
// on the ORDER the points arrive in, which would make the radius change as the corner order rotated
// relative to the light -- reintroducing the crawl the stabiliser exists to remove. Welzl's is
// recursive, and no recursive function ships in engine or editor sources (clang-tidy's
// misc-no-recursion is a --warnings-as-errors finding on the Linux Debug lane).
//
// An empty span yields {center {0,0,0}, radius 0} -- a defined answer, never UB.
[[nodiscard]] Sphere boundingSphere(std::span<const Vec3> points) noexcept;

// Floors x and y to whole multiples of texelWorldSize; z is returned UNTOUCHED, because the depth
// axis is not rasterised into texels. FLOOR, never round: round moves the grid by half a texel on
// one side of zero and not the other, which puts a seam at the origin.
//
// THE ARGUMENT IS A POINT ON THE LIGHT'S LATERAL AXES -- (dot(s, p), dot(u, p)) for the light basis
// rows -- and deliberately NOT a point in lightView space. Those are different quantities: the
// lightView-space position of the fit's own centre is identically (0, 0, -k) for every input, by the
// definition of lookAt, so snapping THAT quantises nothing at all. The parameter is named for what it
// must be handed.
//
// A non-positive or non-finite texelWorldSize returns the input unchanged -- there is no grid to
// snap to, and fabricating one would be worse than not snapping.
[[nodiscard]] Vec3 snapToTexel(Vec3 lightAxisPoint, float texelWorldSize) noexcept;

struct ShadowFit {
    Mat4 lightView{};
    Mat4 lightProj{};  // engine::ortho -> orthoRH_ZO, ADR-005's [0,1] clip
    float texelWorldSize = 0.0F;
    bool valid = false;  // false => the caller shades unshadowed, SILENTLY
};

// The fit, in five steps: unproject the camera's frustum slice; bound it with a rotation-invariant
// sphere; build the light basis on the axis least parallel to the sun; floor the volume's min corner
// onto a WORLD-ANCHORED texel lattice; and extend the light EYE backwards over the caster bounds so
// an off-screen caster still writes depth.
//
// `lightDirection` is the direction the light TRAVELS and need not be unit length.
// `shadowDistance` is the view-depth reach of the map, clamped to the camera's own far plane.
// `casterBoundsWorld` extends the NEAR side ONLY, by moving the eye -- zNear is always
// SHADOW_NEAR_MARGIN, which is what makes a caster behind the eye unrepresentable rather than
// clipped. An INVALID box (the cook's inverted sentinel) contributes nothing and is NOT an error.
//
// TOTAL: never asserts, never logs, never allocates. Returns valid == false with every other field
// default for each degenerate class -- a zero resolution, a non-invertible or non-finite camera, a
// zero-length or non-finite direction, a non-positive or non-finite shadowDistance, a degenerate fit
// radius, or a non-finite light basis.
[[nodiscard]] ShadowFit fitDirectionalShadow(const CameraView& camera, Vec3 lightDirection, float shadowDistance,
                                             std::uint32_t resolution, const Aabb& casterBoundsWorld) noexcept;

// THE ONE place lightProj * lightView is formed. Returns identity for an invalid fit, so a caller
// that ignores `valid` gets a defined matrix rather than a default-constructed pair's product.
[[nodiscard]] Mat4 shadowViewProj(const ShadowFit& fit) noexcept;

}  // namespace engine::render

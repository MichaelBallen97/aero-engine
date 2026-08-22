// tests/render_shadow_test.cpp -- task 3.6.2: the directional shadow map (SF1-SF28, SM1-SM14). A TU
// of aero_tests, which supplies main() from test_main.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier 0 (no GPU, every lane INCLUDING both reduced configurations, SF*): the fit, pinned against
// HAND-COMPUTED LITERALS rather than against the formulae that produced them. The reference cameras
// below are chosen so every intermediate value is an EXACT binary fraction -- an orthographic camera
// whose eight frustum corners are (+-2, +-4, -1) and (+-2, +-4, -9), giving a bounding sphere of
// centre (0, 0, -5) and radius EXACTLY 6, and a hand-built perspective camera whose corners are
// (+-1, +-1, -1) and (+-5, +-5, -5). Nothing here compares a function against itself.
//
// Tier 1 (a real Device, NO window -- RenderTarget supplies the formats, gated by AERO_SKIP_OR_FAIL,
// SM*): renderShadowMap itself -- the six opt-outs, the counters, the light cull, the skinned
// exemption, the resolution policy and the end-to-end record.
//
// <ostream> is included preventively: MSVC alone needs the complete type to stringify a string_view
// inside a doctest CHECK (the standing trap in .claude/rules/ci-portability.md).
//
// Nothing here is named lowercase near/far: <windows.h> #defines both as empty macros.

#include <aero/core/math.hpp>
#include <aero/render/culling.hpp>
#include <aero/render/lighting.hpp>
#include <aero/render/shadow.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <ostream>
#include <span>

using engine::Mat4;
using engine::Vec3;
using engine::Vec4;
using engine::render::Aabb;
using engine::render::boundingSphere;
using engine::render::CameraView;
using engine::render::fitDirectionalShadow;
using engine::render::frustumCornersWorld;
using engine::render::SHADOW_NEAR_MARGIN;
using engine::render::ShadowFit;
using engine::render::shadowUpAxis;
using engine::render::shadowViewProj;
using engine::render::snapToTexel;
using engine::render::Sphere;

namespace {

constexpr float INF = std::numeric_limits<float>::infinity();
constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();

// ---- reference camera A: ORTHOGRAPHIC, and every number below is EXACT ------------------------
// ortho(-2, 2, -4, 4, 1, 9) has m00 = 2/4 = 0.5, m11 = 2/8 = 0.25, m22 = -1/8 = -0.125 and
// m32 = -1/8 = -0.125 -- all exact binary fractions. The view is a pure TRANSLATION by exact
// integers, so proj * view and its inverse are exact too and the eight unprojected corners land on
// the integers below with no rounding at all. That is why the fit's matrices can be pinned against
// literals rather than against tolerances.
//
// THE CAMERA SITS AT (1, 2, 3) RATHER THAN AT THE ORIGIN, AND THAT IS THE WHOLE POINT OF THE
// FIXTURE. At the origin the fit's texel residues come out (0, 0) and every snap assertion is
// vacuously satisfied -- which is exactly how a snap that quantised nothing survived this battery
// once. Here the residues are 0.5 in x and 0.25 in y: both NON-ZERO, both exact binary fractions,
// and both large enough that a fit which skipped the snap reddens rather than agreeing.
[[nodiscard]] CameraView orthoCamera() {
    CameraView camera;
    camera.view = engine::translation(Vec3{-1.0F, -2.0F, -3.0F});  // camera at world (1, 2, 3), unrotated
    camera.proj = engine::ortho(-2.0F, 2.0F, -4.0F, 4.0F, 1.0F, 9.0F);
    camera.eyePosition = Vec3{1.0F, 2.0F, 3.0F};
    return camera;
}

// The eight corners in the header's FIXED order: index = (z * 4) + (y * 2) + x. These are the
// origin-camera corners (+-2, +-4, -1) and (+-2, +-4, -9) translated by (1, 2, 3).
constexpr std::array<Vec3, 8> ORTHO_CORNERS{
    Vec3{-1.0F, -2.0F, 2.0F},  Vec3{3.0F, -2.0F, 2.0F},  Vec3{-1.0F, 6.0F, 2.0F},  Vec3{3.0F, 6.0F, 2.0F},
    Vec3{-1.0F, -2.0F, -6.0F}, Vec3{3.0F, -2.0F, -6.0F}, Vec3{-1.0F, 6.0F, -6.0F}, Vec3{3.0F, 6.0F, -6.0F}};

// centroid = ((4*(-1) + 4*3) / 8, (4*(-2) + 4*6) / 8, (4*2 + 4*(-6)) / 8) = (1, 2, -2); every corner
// is at distance sqrt(2^2 + 4^2 + 4^2) = sqrt(4 + 16 + 16) = sqrt(36) = 6, EXACTLY.
constexpr Vec3 ORTHO_SPHERE_CENTER{1.0F, 2.0F, -2.0F};
constexpr float ORTHO_SPHERE_RADIUS = 6.0F;

// ---- reference camera B: PERSPECTIVE, built as COLUMN LITERALS --------------------------------
// perspectiveRH_ZO(90 deg, aspect 1, zNear 1, zFar 5):
//   m00 = m11 = 1 / (1 * tan(45 deg)) = 1        m22 = 5 / (1 - 5) = -1.25
//   m23 = -1                                      m32 = -(5 * 1) / (5 - 1) = -1.25
// Written out by hand rather than through engine::perspective, for the 3.6.1 FC16 reason: it removes
// libm from the literal battery entirely, and engine::perspective ASSERTS its preconditions, so a
// degenerate variant could not be built through it at all.
[[nodiscard]] Mat4 referencePerspective() {
    Mat4 m = Mat4::zero();
    m.columns[0] = Vec4{1.0F, 0.0F, 0.0F, 0.0F};
    m.columns[1] = Vec4{0.0F, 1.0F, 0.0F, 0.0F};
    m.columns[2] = Vec4{0.0F, 0.0F, -1.25F, -1.0F};
    m.columns[3] = Vec4{0.0F, 0.0F, -1.25F, 0.0F};
    return m;
}

[[nodiscard]] CameraView perspectiveCamera() {
    CameraView camera;
    camera.view = Mat4::identity();
    camera.proj = referencePerspective();
    camera.eyePosition = Vec3{};
    return camera;
}

// At z = -1 the projection is the identity in x and y (m00 == 1), so ndc.x == x; at z = -5 the
// divide by w == 5 makes ndc.x == x / 5. Hence +-1 at the near plane and +-5 at the far one.
constexpr std::array<Vec3, 8> PERSP_CORNERS{
    Vec3{-1.0F, -1.0F, -1.0F}, Vec3{1.0F, -1.0F, -1.0F}, Vec3{-1.0F, 1.0F, -1.0F}, Vec3{1.0F, 1.0F, -1.0F},
    Vec3{-5.0F, -5.0F, -5.0F}, Vec3{5.0F, -5.0F, -5.0F}, Vec3{-5.0F, 5.0F, -5.0F}, Vec3{5.0F, 5.0F, -5.0F}};

// ---- the reference FIT, worked by hand in the plan and restated here as literals ---------------
// orthoCamera(), lightDirection (0, -1, 0) (straight down), shadowDistance 9, resolution 16, an
// INVALID caster box:
//   up          = +Z                       (|y| == 1 > 0.99)
//   casterBack  = 0                        (the box is invalid)
//   k           = 6 + 0 + 1 = 7            (radius + casterBack + SHADOW_NEAR_MARGIN)
//   eye         = (1, 2, -2) - (0, -1, 0) * 7 = (1, 9, -2)
//   lookAt      : f = (0, -1, 0), s = normalize(cross(f, up)) = (-1, 0, 0), u = cross(s, f) = (0, 0, 1)
//   texel       = 2 * 6 / 16 = 0.75
//   a           = dot(s, c) = dot((-1,0,0), (1,2,-2))  = -1     <- the WORLD-ANCHORED lateral pair,
//   b           = dot(u, c) = dot(( 0,0,1), (1,2,-2))  = -2        which is what gets quantised
//   anchor      = (a - 6, b - 6) = (-7, -8)
//   snapped     = (floor(-7/0.75)*0.75, floor(-8/0.75)*0.75) = (-10*0.75, -11*0.75) = (-7.5, -8.25)
//   residue     = (-7 - -7.5, -8 - -8.25) = (0.5, 0.25)         <- BOTH NON-ZERO, by fixture design
//   lightProj   = ortho(-6.5, 5.5, -6.25, 5.75, 1, 13)
//
// NOTE, because it is the identity that hid a defect: transformPoint(lightView, c) is (0, 0, -7)
// here, and it is (0, 0, -k) for EVERY fixture -- lookAt puts its target on the light's own axis. It
// is asserted in SF20 as a property of lookAt, and it is precisely the quantity the snap must NOT be
// applied to.
constexpr Vec3 SUN_DOWN{0.0F, -1.0F, 0.0F};
constexpr float REF_SHADOW_DISTANCE = 9.0F;
constexpr std::uint32_t REF_RESOLUTION = 16;
constexpr Aabb INVALID_BOX{Vec3{INF, INF, INF}, Vec3{-INF, -INF, -INF}};

[[nodiscard]] ShadowFit referenceFit() {
    return fitDirectionalShadow(orthoCamera(), SUN_DOWN, REF_SHADOW_DISTANCE, REF_RESOLUTION, INVALID_BOX);
}

// A defaulted ShadowFit, for the "every other field default" half of the degenerate cases. NOTE the
// trap this helper exists to avoid restating: a default-constructed engine::Mat4 is the IDENTITY,
// not the zero matrix -- mat4.hpp gives `columns` an NSDMI -- so "every other field default" means
// identity here. Mat4, Vec3 and Vec4 all have a defaulted operator==, so these are EXACT comparisons
// rather than tolerant ones, which is what "default" has to mean.
[[nodiscard]] bool isDefaultFit(const ShadowFit& fit) {
    return !fit.valid && fit.texelWorldSize == 0.0F && fit.lightView == Mat4::identity() &&
           fit.lightProj == Mat4::identity();
}

// Project a WORLD point through a shadow view-projection, with the divide. The tests use this to
// assert where the fit puts things, which is a stronger statement than comparing matrix cells.
[[nodiscard]] Vec3 toNdc(const Mat4& viewProj, Vec3 world) {
    const Vec4 clip = viewProj * Vec4{world.x, world.y, world.z, 1.0F};
    return Vec3{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
}

}  // namespace

TEST_CASE("render shadow: shadowUpAxis returns +Y for a sun away from vertical (SF1)") {
    // |unit(dir).y| ~= 0.9798 <= 0.99 -> +Y.
    CHECK(shadowUpAxis(Vec3{0.2F, 0.98F, 0.0F}) == Vec3{0.0F, 1.0F, 0.0F});
    // 3-4-5: |y| == 0.6, comfortably below. Seeding the threshold to 0.5 reddens THIS line.
    CHECK(shadowUpAxis(Vec3{0.0F, 0.6F, -0.8F}) == Vec3{0.0F, 1.0F, 0.0F});
    // THE NORMALISATION ARM: raw |y| == 0.995 is ABOVE the threshold, but the vector is very nearly
    // horizontal, so the answer must be +Y. A predicate on the raw y returns +Z here and reddens.
    CHECK(shadowUpAxis(Vec3{100.0F, 0.995F, 0.0F}) == Vec3{0.0F, 1.0F, 0.0F});
    // Total: a zero-length input must not assert (normalizeOrZero, never normalize) and yields +Y.
    CHECK(shadowUpAxis(Vec3{}) == Vec3{0.0F, 1.0F, 0.0F});
}

TEST_CASE("render shadow: shadowUpAxis returns +Z for a near-vertical sun (SF2)") {
    // |unit(dir).y| ~= 0.99499 > 0.99 -> +Z, from BOTH signs.
    CHECK(shadowUpAxis(Vec3{0.1F, -0.995F, 0.0F}) == Vec3{0.0F, 0.0F, 1.0F});
    CHECK(shadowUpAxis(Vec3{0.1F, 0.995F, 0.0F}) == Vec3{0.0F, 0.0F, 1.0F});
    // Non-unit input, same answer: (0, -100, 0) normalises to (0, -1, 0).
    CHECK(shadowUpAxis(Vec3{0.0F, -100.0F, 0.0F}) == Vec3{0.0F, 0.0F, 1.0F});
}

TEST_CASE("render shadow: a straight-down sun yields a finite light basis (SF3)") {
    // THE seed this case exists for: shadowUpAxis returning +Y here makes cross(f, up) ==
    // cross((0,-1,0),(0,1,0)) == (0,0,0), glm::lookAtRH normalises it with no guard, and every cell
    // of lightView becomes NaN. The fit's finiteness check then refuses -- so `valid` is the witness.
    const ShadowFit fit = referenceFit();  // lightDirection is exactly (0, -1, 0)
    REQUIRE(fit.valid);
    for (const Vec4& column : fit.lightView.columns) {
        CHECK(std::isfinite(column.x));
        CHECK(std::isfinite(column.y));
        CHECK(std::isfinite(column.z));
        CHECK(std::isfinite(column.w));
    }
    // ...and the basis is orthonormal, which a NaN or a degenerate cross product is not.
    const Vec3 sAxis{fit.lightView.columns[0].x, fit.lightView.columns[1].x, fit.lightView.columns[2].x};
    const Vec3 uAxis{fit.lightView.columns[0].y, fit.lightView.columns[1].y, fit.lightView.columns[2].y};
    CHECK(std::abs(engine::lengthSquared(sAxis) - 1.0F) <= 1.0e-5F);
    CHECK(std::abs(engine::lengthSquared(uAxis) - 1.0F) <= 1.0e-5F);
    CHECK(std::abs(engine::dot(sAxis, uAxis)) <= 1.0e-5F);
}

TEST_CASE("render shadow: frustumCornersWorld unprojects a perspective slice to literals (SF4)") {
    const std::optional<std::array<Vec3, 8>> corners = frustumCornersWorld(perspectiveCamera(), 1.0F, 5.0F);
    REQUIRE(corners.has_value());
    for (std::size_t i = 0; i < corners->size(); ++i) {
        INFO("corner ", i);
        CHECK(engine::approxEquals((*corners)[i], PERSP_CORNERS[i], 1.0e-4F));
    }
    // THE DIVIDE IS LOAD-BEARING and this is what pins it: without it the far corners come back at
    // (+-1, +-1) rather than (+-5, +-5), because w == 5 there and w == 1 at the near plane.
    CHECK(std::abs((*corners)[7].x - 5.0F) <= 1.0e-4F);
    CHECK(std::abs((*corners)[3].x - 1.0F) <= 1.0e-4F);
}

TEST_CASE("render shadow: the slice clamps to the camera's own near and far planes (SF5)") {
    const CameraView camera = perspectiveCamera();  // zNear 1, zFar 5
    const std::optional<std::array<Vec3, 8>> exact = frustumCornersWorld(camera, 1.0F, 5.0F);
    const std::optional<std::array<Vec3, 8>> beyond = frustumCornersWorld(camera, 0.0F, 1000.0F);
    REQUIRE(exact.has_value());
    REQUIRE(beyond.has_value());
    for (std::size_t i = 0; i < exact->size(); ++i) {
        INFO("corner ", i);
        // BIT-IDENTICAL, not approximate: both are the same two clamped ndc.z values (0 and 1)
        // through the same inverse. A sliceFar that was NOT clamped would put the far corners at
        // x == +-1000 and this would fail by three orders of magnitude.
        CHECK((*exact)[i] == (*beyond)[i]);
    }
    CHECK(std::abs((*beyond)[7].z + 5.0F) <= 1.0e-4F);  // the far corners are at z == -5, not -1000
    // A slice given BACKWARDS is defined, not inverted.
    const std::optional<std::array<Vec3, 8>> swapped = frustumCornersWorld(camera, 5.0F, 1.0F);
    REQUIRE(swapped.has_value());
    for (std::size_t i = 0; i < exact->size(); ++i) {
        CHECK((*swapped)[i] == (*exact)[i]);
    }
}

TEST_CASE("render shadow: frustumCornersWorld is correct for an ORTHOGRAPHIC projection (SF6)") {
    // The slice is built in NDC and unprojected, so nothing here assumes a perspective form. An
    // orthographic editor camera does not exist yet, but CameraView cannot refuse one.
    const std::optional<std::array<Vec3, 8>> corners = frustumCornersWorld(orthoCamera(), 0.0F, 9.0F);
    REQUIRE(corners.has_value());
    for (std::size_t i = 0; i < corners->size(); ++i) {
        INFO("corner ", i);
        CHECK(engine::approxEquals((*corners)[i], ORTHO_CORNERS[i], 1.0e-5F));
    }
    // Every matrix cell of this projection and of its translating view is an exact binary fraction,
    // so the corners are EXACT.
    CHECK((*corners)[0] == Vec3{-1.0F, -2.0F, 2.0F});
    CHECK((*corners)[7] == Vec3{3.0F, 6.0F, -6.0F});
}

TEST_CASE("render shadow: a non-invertible camera yields nullopt (SF7)") {
    CameraView camera = perspectiveCamera();
    camera.proj = Mat4::zero();  // determinant 0 -> glm divides by it -> inf/NaN throughout
    CHECK_FALSE(frustumCornersWorld(camera, 1.0F, 5.0F).has_value());
    // A collapsed VIEW is the same class from the other side: a scale of 0 on one axis.
    CameraView collapsed = perspectiveCamera();
    collapsed.view = engine::scaling(Vec3{1.0F, 1.0F, 0.0F});
    CHECK_FALSE(frustumCornersWorld(collapsed, 1.0F, 5.0F).has_value());
}

TEST_CASE("render shadow: a non-finite camera element yields nullopt (SF8)") {
    CameraView nanProj = perspectiveCamera();
    nanProj.proj.columns[2].z = NAN_F;
    CHECK_FALSE(frustumCornersWorld(nanProj, 1.0F, 5.0F).has_value());
    CameraView infProj = perspectiveCamera();
    infProj.proj.columns[3].z = INF;
    CHECK_FALSE(frustumCornersWorld(infProj, 1.0F, 5.0F).has_value());
    CameraView nanView = perspectiveCamera();
    nanView.view.columns[3].x = NAN_F;
    CHECK_FALSE(frustumCornersWorld(nanView, 1.0F, 5.0F).has_value());
}

TEST_CASE("render shadow: the corners come back in WORLD space, not view space (SF9)") {
    // A camera at (10, 3, -7) looking down -X. The identity-view corners transformed by the camera's
    // WORLD matrix must equal the corners this camera produces -- which is what "world space" means,
    // and what a version that forgot to apply `view` would fail.
    const Vec3 eye{10.0F, 3.0F, -7.0F};
    CameraView moved = perspectiveCamera();
    moved.view = engine::lookAt(eye, eye + Vec3{-1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F});
    moved.eyePosition = eye;
    const Mat4 cameraWorld = engine::inverse(moved.view);

    const std::optional<std::array<Vec3, 8>> corners = frustumCornersWorld(moved, 1.0F, 5.0F);
    REQUIRE(corners.has_value());
    for (std::size_t i = 0; i < corners->size(); ++i) {
        INFO("corner ", i);
        CHECK(engine::approxEquals((*corners)[i], engine::transformPoint(cameraWorld, PERSP_CORNERS[i]), 1.0e-3F));
    }
    // ...and they are NOT the identity-view corners, which is the half that makes the above non-vacuous.
    CHECK_FALSE(engine::approxEquals((*corners)[0], PERSP_CORNERS[0], 1.0e-3F));
}

TEST_CASE("render shadow: boundingSphere contains every corner, at the literal centre and radius (SF10)") {
    const Sphere sphere = boundingSphere(ORTHO_CORNERS);
    // Hand-computed: centroid (1, 2, -2); every corner at sqrt(4 + 16 + 16) == 6 exactly.
    CHECK(engine::approxEquals(sphere.center, ORTHO_SPHERE_CENTER, 1.0e-5F));
    CHECK(std::abs(sphere.radius - ORTHO_SPHERE_RADIUS) <= 1.0e-5F);
    for (const Vec3 corner : ORTHO_CORNERS) {
        CHECK(engine::length(corner - sphere.center) <= sphere.radius + 1.0e-4F);
    }
    // The perspective set, whose radius is NOT the same for every corner: the near corners sit at
    // sqrt(1 + 1 + 4) == sqrt(6) ~= 2.4494897 from the centroid (0, 0, -3) and the far ones at
    // sqrt(25 + 25 + 4) == sqrt(54) ~= 7.3484692. The radius is the LARGER.
    const Sphere wide = boundingSphere(PERSP_CORNERS);
    CHECK(engine::approxEquals(wide.center, Vec3{0.0F, 0.0F, -3.0F}, 1.0e-5F));
    CHECK(std::abs(wide.radius - 7.3484692F) <= 1.0e-5F);
    for (const Vec3 corner : PERSP_CORNERS) {
        CHECK(engine::length(corner - wide.center) <= wide.radius + 1.0e-4F);
    }
}

TEST_CASE("render shadow: the bounding radius is invariant under camera ROTATION (SF11)") {
    // THE case A6 rests on: a light-space BOX fit's extent swings by roughly 2x over this sweep,
    // while the sphere's does not move beyond float noise. The tolerance is RELATIVE, not
    // bit-identity, for the reason AC-11 already gives -- but the number is 1e-3, not 1e-4, and it is
    // MEASURED rather than guessed.
    //
    // The residual is dominated by precision loss in the NDC-depth round trip through
    // inverse(proj * view), and it scales a clean decade per decade of DEPTH RATIO:
    //     zFar/zNear = 1e1 -> 1.366e-6      1e2 -> 1.01e-5      1e3 -> 1.132e-4
    // This camera is 0.1/100, a 1000:1 ratio, so 1.132e-4 is the floor and 1e-3 leaves ~9x headroom
    // against flakiness while still being ~1000x tighter than the ~100 % swing a box fit produces.
    // The second arm below runs a SHALLOWER camera at the tighter 1e-4, so the claim that the
    // residual is inherent -- rather than a defect in the fit -- is itself under test.
    const Vec3 eye{2.0F, 3.0F, 4.0F};
    float minRadius = INF;
    float maxRadius = 0.0F;
    for (int step = 0; step < 36; ++step) {
        const float yaw = engine::radians(static_cast<float>(step) * 10.0F);
        CameraView camera;
        camera.view = engine::lookAt(eye, eye + Vec3{std::sin(yaw), -0.2F, -std::cos(yaw)}, Vec3{0.0F, 1.0F, 0.0F});
        camera.proj = engine::perspective(engine::radians(60.0F), 16.0F / 9.0F, 0.1F, 100.0F);
        camera.eyePosition = eye;
        const std::optional<std::array<Vec3, 8>> corners = frustumCornersWorld(camera, 0.0F, 40.0F);
        REQUIRE(corners.has_value());
        const Sphere sphere = boundingSphere(*corners);
        INFO("yaw step ", step);
        // The CENTRE moves (it follows the camera) -- only the RADIUS is invariant. Asserting the
        // centre were fixed would be asserting the wrong thing.
        CHECK(engine::length(sphere.center - eye) > 1.0F);
        minRadius = std::min(minRadius, sphere.radius);
        maxRadius = std::max(maxRadius, sphere.radius);
    }
    CHECK(minRadius > 0.0F);
    CHECK((maxRadius - minRadius) / maxRadius <= 1.0e-3F);

    // THE SHALLOW ARM: the same sweep at zNear 1.0 (a 100:1 ratio) must hold the TIGHTER 1e-4, which
    // is what says the residual above is round-trip precision rather than a fit that genuinely moves.
    // A box fit swings ~100 % at BOTH ratios and fails this arm by four orders of magnitude.
    //
    // The ORDERING (wide residual > shallow residual) is deliberately NOT asserted, even though it
    // measures ~11x: lookAt and perspective reach sqrt and tan, libm differs between three C
    // libraries, and a cross-lane flake in a stability case would cost more than the assertion buys.
    // The two bounds are asserted; the scaling law is recorded here.
    float shallowMin = INF;
    float shallowMax = 0.0F;
    for (int step = 0; step < 36; ++step) {
        const float yaw = engine::radians(static_cast<float>(step) * 10.0F);
        CameraView camera;
        camera.view = engine::lookAt(eye, eye + Vec3{std::sin(yaw), -0.2F, -std::cos(yaw)}, Vec3{0.0F, 1.0F, 0.0F});
        camera.proj = engine::perspective(engine::radians(60.0F), 16.0F / 9.0F, 1.0F, 100.0F);
        camera.eyePosition = eye;
        const std::optional<std::array<Vec3, 8>> corners = frustumCornersWorld(camera, 0.0F, 40.0F);
        INFO("shallow yaw step ", step);
        REQUIRE(corners.has_value());
        const float radius = boundingSphere(*corners).radius;
        shallowMin = std::min(shallowMin, radius);
        shallowMax = std::max(shallowMax, radius);
    }
    CHECK(shallowMin > 0.0F);
    CHECK((shallowMax - shallowMin) / shallowMax <= 1.0e-4F);
}

TEST_CASE("render shadow: boundingSphere of a single point is that point at radius 0 (SF12)") {
    const std::array<Vec3, 1> one{Vec3{3.0F, -4.0F, 12.0F}};
    const Sphere sphere = boundingSphere(one);
    CHECK(sphere.center == one[0]);
    CHECK(sphere.radius == 0.0F);
    // Two coincident points are the same answer, which a mean-of-distinct-points implementation
    // could get wrong if it divided by anything other than the count.
    const std::array<Vec3, 2> twice{one[0], one[0]};
    CHECK(boundingSphere(twice).center == one[0]);
    CHECK(boundingSphere(twice).radius == 0.0F);
}

TEST_CASE("render shadow: boundingSphere of an EMPTY span is defined (SF13)") {
    const Sphere sphere = boundingSphere(std::span<const Vec3>{});
    CHECK(sphere.center == Vec3{});
    CHECK(sphere.radius == 0.0F);
}

TEST_CASE("render shadow: the texel quantum is 2r / resolution, pinned against a literal (SF14)") {
    // 2 * 6 / 16 == 0.75, EXACTLY, and it is not 6 / 16 == 0.375. The ortho spans 2r across
    // `resolution` texels; halving the quantum snaps to half-texels and leaves visible crawl.
    const ShadowFit fit = referenceFit();
    REQUIRE(fit.valid);
    CHECK(fit.texelWorldSize == 0.75F);
    CHECK(fit.texelWorldSize != 0.375F);
    // The snap itself, against hand-computed values on that same quantum:
    //   1.3 / 0.75 == 1.7333 -> floor 1 -> 0.75
    //   2.9 / 0.75 == 3.8667 -> floor 3 -> 2.25
    const Vec3 snapped = snapToTexel(Vec3{1.3F, 2.9F, 0.0F}, 0.75F);
    CHECK(std::abs(snapped.x - 0.75F) <= 1.0e-6F);
    CHECK(std::abs(snapped.y - 2.25F) <= 1.0e-6F);
    // Idempotent: snapping an already-snapped value changes nothing.
    CHECK(snapToTexel(snapped, 0.75F) == snapped);
    // A non-positive or non-finite quantum returns the input unchanged rather than fabricating a grid.
    CHECK(snapToTexel(Vec3{1.3F, 2.9F, 0.0F}, 0.0F) == Vec3{1.3F, 2.9F, 0.0F});
    CHECK(snapToTexel(Vec3{1.3F, 2.9F, 0.0F}, NAN_F) == Vec3{1.3F, 2.9F, 0.0F});
}

TEST_CASE("render shadow: the snap FLOORS, including for negative coordinates (SF15)") {
    // Hand-computed on a quantum of 0.5:
    //   -1.1 / 0.5 == -2.2  -> floor -3 -> -1.5    (std::round would give -2 -> -1.0)
    //    0.9 / 0.5 ==  1.8  -> floor  1 ->  0.5    (std::round would give  2 ->  1.0)
    // Both sides are asserted, because a round-based implementation is wrong on both and a
    // truncate-based one (std::trunc) is wrong only on the negative side.
    const Vec3 snapped = snapToTexel(Vec3{-1.1F, 0.9F, 0.0F}, 0.5F);
    CHECK(std::abs(snapped.x + 1.5F) <= 1.0e-6F);
    CHECK(std::abs(snapped.y - 0.5F) <= 1.0e-6F);
    CHECK(snapped.x != -1.0F);  // the round answer, refused
    CHECK(snapped.y != 1.0F);   // likewise
    // trunc(-2.2) == -2 -> -1.0, so this is the same literal from the other failure mode.
    CHECK(std::abs(snapToTexel(Vec3{-1.1F, 0.0F, 0.0F}, 0.5F).x + 1.5F) <= 1.0e-6F);
}

TEST_CASE("render shadow: the snap leaves z bit-identical (SF16)") {
    // The depth axis is not rasterised into texels, so quantising it would move the near/far planes
    // by up to one texel every frame for no benefit at all.
    const Vec3 input{0.0F, 0.0F, -7.3F};
    CHECK(snapToTexel(input, 0.5F).z == -7.3F);
    CHECK(snapToTexel(input, 0.5F).z == input.z);
    // And a z that IS a whole multiple is equally untouched -- which a "snap z too" implementation
    // would pass, so the value above is deliberately NOT one.
    CHECK(snapToTexel(Vec3{0.0F, 0.0F, -7.5F}, 0.5F).z == -7.5F);
}

TEST_CASE("render shadow: resolution 0 and a degenerate camera yield an invalid fit (SF17)") {
    CHECK(isDefaultFit(fitDirectionalShadow(orthoCamera(), SUN_DOWN, REF_SHADOW_DISTANCE, 0, INVALID_BOX)));
    CameraView singular = orthoCamera();
    singular.proj = Mat4::zero();
    CHECK(isDefaultFit(fitDirectionalShadow(singular, SUN_DOWN, REF_SHADOW_DISTANCE, REF_RESOLUTION, INVALID_BOX)));
    CameraView nanCamera = orthoCamera();
    nanCamera.view.columns[1].y = NAN_F;
    CHECK(isDefaultFit(fitDirectionalShadow(nanCamera, SUN_DOWN, REF_SHADOW_DISTANCE, REF_RESOLUTION, INVALID_BOX)));
}

TEST_CASE("render shadow: a zero-length or non-finite light direction yields an invalid fit (SF18)") {
    CHECK(isDefaultFit(fitDirectionalShadow(orthoCamera(), Vec3{}, REF_SHADOW_DISTANCE, REF_RESOLUTION, INVALID_BOX)));
    CHECK(isDefaultFit(fitDirectionalShadow(orthoCamera(), Vec3{0.0F, NAN_F, 0.0F}, REF_SHADOW_DISTANCE, REF_RESOLUTION,
                                            INVALID_BOX)));
    CHECK(isDefaultFit(
        fitDirectionalShadow(orthoCamera(), Vec3{INF, 0.0F, 0.0F}, REF_SHADOW_DISTANCE, REF_RESOLUTION, INVALID_BOX)));
    // A direction shorter than normalizeOrZero's epsilon is the same class, and it is the one an
    // "if (v == Vec3{})" guard would miss.
    CHECK(isDefaultFit(fitDirectionalShadow(orthoCamera(), Vec3{0.0F, -1.0e-9F, 0.0F}, REF_SHADOW_DISTANCE,
                                            REF_RESOLUTION, INVALID_BOX)));
}

TEST_CASE("render shadow: a non-positive or non-finite shadowDistance yields an invalid fit (SF19)") {
    for (const float bad : {0.0F, -1.0F, NAN_F, INF, -INF}) {
        INFO("shadowDistance ", bad);
        CHECK(isDefaultFit(fitDirectionalShadow(orthoCamera(), SUN_DOWN, bad, REF_RESOLUTION, INVALID_BOX)));
    }
    // ...and the positive control, so the five above are not passing for the wrong reason.
    CHECK(fitDirectionalShadow(orthoCamera(), SUN_DOWN, REF_SHADOW_DISTANCE, REF_RESOLUTION, INVALID_BOX).valid);
}

TEST_CASE("render shadow: the light view matrix matches its hand-computed literals (SF20)") {
    // Worked in the plan, not derived here:
    //   sphere = { (1, 2, -2), r = 6 }, dir = (0, -1, 0), up = shadowUpAxis(dir) = (0, 0, 1)
    //   k   = r + casterBack + SHADOW_NEAR_MARGIN = 6 + 0 + 1 = 7
    //   eye = (1, 2, -2) - (0, -1, 0) * 7 = (1, 9, -2)
    //   lookAtRH: f = normalize(centre - eye) = (0, -1, 0)
    //             s = normalize(cross(f, up)) = normalize((-1, 0, 0)) = (-1, 0, 0)
    //             u = cross(s, f)             = (0, 0, 1)
    //   columns[0] = (s.x, u.x, -f.x, 0) = (-1, 0, 0, 0)
    //   columns[1] = (s.y, u.y, -f.y, 0) = ( 0, 0, 1, 0)
    //   columns[2] = (s.z, u.z, -f.z, 0) = ( 0, 1, 0, 0)
    //   columns[3] = (-dot(s,eye), -dot(u,eye), dot(f,eye), 1)
    //              = (-(-1), -(-2), -9, 1)   = (1, 2, -9, 1)
    const ShadowFit fit = referenceFit();
    REQUIRE(fit.valid);
    CHECK(engine::approxEquals(fit.lightView.columns[0], Vec4{-1.0F, 0.0F, 0.0F, 0.0F}, 1.0e-5F));
    CHECK(engine::approxEquals(fit.lightView.columns[1], Vec4{0.0F, 0.0F, 1.0F, 0.0F}, 1.0e-5F));
    CHECK(engine::approxEquals(fit.lightView.columns[2], Vec4{0.0F, 1.0F, 0.0F, 0.0F}, 1.0e-5F));
    CHECK(engine::approxEquals(fit.lightView.columns[3], Vec4{1.0F, 2.0F, -9.0F, 1.0F}, 1.0e-5F));

    // THE IDENTITY, asserted as an identity rather than as a coincidence of this fixture, because it
    // is what hid a defect through this whole battery once: lookAt maps its TARGET onto the light's
    // own -Z axis, so transformPoint(lightView, centre) is (0, 0, -k) for EVERY input. It is
    // therefore useless as a snap input -- there is nothing camera-dependent in it to quantise -- and
    // the fit reads the basis ROWS instead (SF21/SF22/SF24/SF28 are what pin that).
    CHECK(engine::approxEquals(engine::transformPoint(fit.lightView, ORTHO_SPHERE_CENTER), Vec3{0.0F, 0.0F, -7.0F},
                               1.0e-5F));
    // ...and the same identity on a DIFFERENT fit, so the line above cannot be read as a fixture fact.
    // The SUN and the RESOLUTION differ, which is what makes lightView a different matrix; the
    // shadowDistance deliberately does NOT, and that is the whole subtlety. The identity is about
    // the fit's OWN sphere centre, and ORTHO_SPHERE_CENTER is only that centre at this distance --
    // a 7-unit slice centres on (1, 2, -1) instead, and the same confusion between "the centre" and
    // "a centre" is exactly what let a dead snap survive this battery once.
    const ShadowFit other =
        fitDirectionalShadow(orthoCamera(), Vec3{-0.4F, -0.8F, -0.45F}, REF_SHADOW_DISTANCE, 32, INVALID_BOX);
    REQUIRE(other.valid);
    CHECK_FALSE(other.lightView == fit.lightView);  // genuinely a different basis, not the same fit
    const Vec3 otherCentre = engine::transformPoint(other.lightView, ORTHO_SPHERE_CENTER);
    CHECK(std::abs(otherCentre.x) <= 1.0e-4F);
    CHECK(std::abs(otherCentre.y) <= 1.0e-4F);
}

TEST_CASE("render shadow: the light projection matches its hand-computed literals (SF21)") {
    // lightProj = ortho(-6.5, 5.5, -6.25, 5.75, 1, 13) -- the sphere's +-6 bounds SHIFTED by the
    // residues (0.5, 0.25) -- i.e. orthoRH_ZO with
    //   m00 = 2 / (5.5 - -6.5)     = 2 / 12  =  0.16666667
    //   m11 = 2 / (5.75 - -6.25)   = 2 / 12  =  0.16666667
    //   m22 = -1 / (13 - 1)        = -1 / 12 = -0.08333333
    //   m30 = -(5.5 + -6.5) / 12   =  1 / 12 =  0.08333333    <- the x snap, NON-ZERO
    //   m31 = -(5.75 + -6.25) / 12 =  1 / 24 =  0.04166667    <- the y snap, NON-ZERO
    //   m32 = -1 / (13 - 1)        = -1 / 12 = -0.08333333
    // Getting ortho's near/far arguments swapped or sign-flipped moves m22 and m32 and NOTHING else,
    // which is why they are pinned as literals rather than checked for "reasonableness": a map that
    // is entirely far-plane (all-lit) or entirely near-plane (all-shadowed) looks like a bias problem
    // and is not.
    const ShadowFit fit = referenceFit();
    REQUIRE(fit.valid);
    CHECK(engine::approxEquals(fit.lightProj.columns[0], Vec4{0.16666667F, 0.0F, 0.0F, 0.0F}, 1.0e-6F));
    CHECK(engine::approxEquals(fit.lightProj.columns[1], Vec4{0.0F, 0.16666667F, 0.0F, 0.0F}, 1.0e-6F));
    CHECK(engine::approxEquals(fit.lightProj.columns[2], Vec4{0.0F, 0.0F, -0.08333333F, 0.0F}, 1.0e-6F));
    CHECK(engine::approxEquals(fit.lightProj.columns[3], Vec4{0.08333333F, 0.04166667F, -0.08333333F, 1.0F}, 1.0e-6F));
    // THE SNAP WITNESS, stated separately from the literals above so it cannot be lost in a
    // regenerated table: m30 and m31 are the ONLY cells the texel snap writes, and a fit that
    // skipped the snap -- or that snapped the identically-zero lightView-space centre -- leaves both
    // at EXACTLY zero.
    CHECK(fit.lightProj.columns[3].x != 0.0F);
    CHECK(fit.lightProj.columns[3].y != 0.0F);
    // Recovered zNear and zFar, stated as the quantities they are rather than as matrix cells:
    // orthoRH_ZO gives m22 == -1/(f-n) and m32 == -n/(f-n), so n == m32/m22 and f == n - 1/m22.
    const float m22 = fit.lightProj.columns[2].z;
    const float m32 = fit.lightProj.columns[3].z;
    CHECK(std::abs((m32 / m22) - SHADOW_NEAR_MARGIN) <= 1.0e-4F);      // zNear == 1
    CHECK(std::abs(((m32 / m22) - (1.0F / m22)) - 13.0F) <= 1.0e-3F);  // zFar  == 2r + 0 + 1 == 13
}

TEST_CASE("render shadow: texelWorldSize and the ortho extent agree with the sphere (SF22)") {
    const ShadowFit fit = referenceFit();
    REQUIRE(fit.valid);
    CHECK(fit.texelWorldSize == 0.75F);  // 2 * 6 / 16, exactly
    // The ortho's half-width, recovered: m00 == 2/(r-l), so (r-l)/2 == 1/m00 == 6 == the radius.
    CHECK(std::abs((1.0F / fit.lightProj.columns[0].x) - ORTHO_SPHERE_RADIUS) <= 1.0e-4F);
    CHECK(std::abs((1.0F / fit.lightProj.columns[1].y) - ORTHO_SPHERE_RADIUS) <= 1.0e-4F);
    // ...and the quantum IS the extent divided by the resolution, stated the other way round so a
    // reader can see the two are consistent rather than two independent constants.
    CHECK(std::abs((fit.texelWorldSize * static_cast<float>(REF_RESOLUTION)) - (2.0F * ORTHO_SPHERE_RADIUS)) <=
          1.0e-4F);

    // THE RESIDUES, recovered from the matrix and pinned against the hand-computed pair. The ortho's
    // lateral centre in lightView space is -m30/m00 == -residueX, and likewise -m31/m11 == -residueY.
    // Both are in (-texel, 0] by construction, and both are NON-ZERO for this fixture BY DESIGN --
    // a fixture whose residues were zero would satisfy every snap assertion vacuously, which is
    // precisely how a snap that quantised nothing survived this battery once.
    const float centreX = -fit.lightProj.columns[3].x / fit.lightProj.columns[0].x;
    const float centreY = -fit.lightProj.columns[3].y / fit.lightProj.columns[1].y;
    CHECK(std::abs(centreX + 0.5F) <= 1.0e-5F);   // residueX == 0.5  = -7 - floor(-7/0.75)*0.75
    CHECK(std::abs(centreY + 0.25F) <= 1.0e-5F);  // residueY == 0.25 = -8 - floor(-8/0.75)*0.75
    CHECK(centreX != 0.0F);
    CHECK(centreY != 0.0F);
    CHECK(std::abs(centreX) < fit.texelWorldSize);  // a correction, never a whole-texel jump
    CHECK(std::abs(centreY) < fit.texelWorldSize);
}

TEST_CASE("render shadow: the sphere's near and far faces map to ndc.z 0 and 1 exactly (SF23)") {
    // With no caster the depth range is [NEAR_MARGIN, 2r + NEAR_MARGIN] and the sphere spans it
    // EXACTLY, so these two numbers are 0 and 1 rather than "close to" them:
    //   the face TOWARD the light (dir is (0,-1,0), so the light is at +Y): world (1, +8, -2)
    //     -> light space (0, 0, -1)  -> ndc.z = (-1/12)(-1) + (-1/12) = 0
    //   the face AWAY:                 world (1, -4, -2)
    //     -> light space (0, 0, -13) -> ndc.z = (13/12) - (1/12) = 1
    // Swapping ortho's near/far, or flipping their signs, moves BOTH of these off their literals.
    // The snap does not touch depth, so these two are unchanged by it.
    const ShadowFit fit = referenceFit();
    REQUIRE(fit.valid);
    const Mat4 viewProj = shadowViewProj(fit);
    const Vec3 towardLight = toNdc(viewProj, Vec3{1.0F, 8.0F, -2.0F});
    const Vec3 awayFromLight = toNdc(viewProj, Vec3{1.0F, -4.0F, -2.0F});
    CHECK(std::abs(towardLight.z - 0.0F) <= 1.0e-5F);
    CHECK(std::abs(awayFromLight.z - 1.0F) <= 1.0e-5F);
    // ...and the ORDER is the point: nearer the light is SMALLER, which is ADR-005's 0 = near and
    // what makes CompareOp::Less "closer wins".
    CHECK(towardLight.z < awayFromLight.z);

    // THE LATERAL EXTREMES, and they are now a SNAP assertion as well as a bounds one. lightView's
    // first row is (-1, 0, 0, 1), so a world point's lateral coordinate is -p.x + 1; its second row
    // is (0, 0, 1, 2), so the other is p.z + 2. The volume's edges sit on the WORLD LATTICE:
    //   left  edge: a - r - residueX = -1 - 6 - 0.5  = -7.5  = -10 * 0.75  -> world p.x =  7.5
    //   right edge: left + 2r        = -7.5 + 12     =  4.5  =   6 * 0.75  -> world p.x = -4.5
    //   bottom    : b - r - residueY = -2 - 6 - 0.25 = -8.25 = -11 * 0.75  -> world p.z = -8.25
    //   top       : bottom + 2r      = -8.25 + 12    =  3.75 =   5 * 0.75  -> world p.z =  3.75
    // EVERY ONE of those four is a whole multiple of the 0.75 texel. Without the snap the edges land
    // at -7 / 5 / -8 / 4, none of which is.
    CHECK(std::abs(toNdc(viewProj, Vec3{7.5F, 2.0F, -2.0F}).x + 1.0F) <= 1.0e-4F);
    CHECK(std::abs(toNdc(viewProj, Vec3{-4.5F, 2.0F, -2.0F}).x - 1.0F) <= 1.0e-4F);
    CHECK(std::abs(toNdc(viewProj, Vec3{1.0F, 2.0F, -8.25F}).y + 1.0F) <= 1.0e-4F);
    CHECK(std::abs(toNdc(viewProj, Vec3{1.0F, 2.0F, 3.75F}).y - 1.0F) <= 1.0e-4F);
}

TEST_CASE("render shadow: shadowViewProj maps the sphere centre to NDC (0, 0, 0.5) (SF24)") {
    // Hand-computed: lightView * (1,2,-2,1) = (0, 0, -7, 1); then
    //   lightProj * (0,0,-7,1):
    //     x = m00 * 0 + m30 = 1/12 = 0.08333333     <- the x residue, in NDC units (residueX / r)
    //     y = m11 * 0 + m31 = 1/24 = 0.04166667     <- the y residue
    //     z = (-1/12)(-7) + (-1/12) = 7/12 - 1/12 = 6/12 = 0.5
    // The 0.5 is the statement that the fit centred the DEPTH range on the sphere. The x and y are
    // the statement that it did NOT centre the LATERAL range on it -- the volume is offset by the
    // sub-texel residue, which is exactly what pins the lattice to the world. A fit with no snap puts
    // the centre at (0, 0, 0.5), so these two literals are the discriminator.
    const ShadowFit fit = referenceFit();
    REQUIRE(fit.valid);
    const Vec3 ndc = toNdc(shadowViewProj(fit), ORTHO_SPHERE_CENTER);
    CHECK(std::abs(ndc.x - 0.08333333F) <= 1.0e-5F);
    CHECK(std::abs(ndc.y - 0.04166667F) <= 1.0e-5F);
    CHECK(std::abs(ndc.z - 0.5F) <= 1.0e-5F);
    CHECK(ndc.x != 0.0F);  // the no-snap answer, refused
    CHECK(ndc.y != 0.0F);
    // THE ORDER. lightView * lightProj is a DIFFERENT matrix, and this is what says so rather than
    // trusting the name: reversed, the centre does not land at the origin of the map.
    const Vec3 reversed = toNdc(fit.lightView * fit.lightProj, ORTHO_SPHERE_CENTER);
    CHECK_FALSE(engine::approxEquals(reversed, ndc, 1.0e-3F));
    // An INVALID fit yields the identity, so a caller who ignores `valid` gets a defined matrix.
    CHECK(shadowViewProj(ShadowFit{}) == Mat4::identity());
}

TEST_CASE("render shadow: a caster behind the sphere moves the EYE, not the near plane (SF25)") {
    // The caster box, chosen so casterBack is an exact integer:
    //   n = -dir = (0, 1, 0); the sphere's far face plane is { n, -dot(n, c) - r } = { (0,1,0), -8 }
    //   box centre (1, 11, -2), half-extent (1, 1, 1)
    //   signedDistance = 11 - 8 = 3;  support = |0|*1 + |1|*1 + |0|*1 = 1
    //   casterBack = max(0, 3 + 1) = 4      (equivalently: the box's top y is 12, and 12 - 2 - 6 = 4)
    // so k becomes 6 + 4 + 1 = 11 and the depth range becomes [1, 2*6 + 4 + 1] = [1, 17].
    constexpr Aabb BEHIND{Vec3{0.0F, 10.0F, -3.0F}, Vec3{2.0F, 12.0F, -1.0F}};
    const ShadowFit none = referenceFit();
    const ShadowFit with = fitDirectionalShadow(orthoCamera(), SUN_DOWN, REF_SHADOW_DISTANCE, REF_RESOLUTION, BEHIND);
    REQUIRE(none.valid);
    REQUIRE(with.valid);

    // The BASIS is bit-identical -- the eye moved along the light axis, so the rotation did not change.
    CHECK(with.lightView.columns[0] == none.lightView.columns[0]);
    CHECK(with.lightView.columns[1] == none.lightView.columns[1]);
    CHECK(with.lightView.columns[2] == none.lightView.columns[2]);
    // ...and the eye moved back by EXACTLY casterBack: lightView's columns[3].z is dot(f, eye), which
    // was -9 and is now -13. columns[3].x and .y are UNCHANGED and NON-ZERO, which is a real
    // assertion here rather than "0 == 0": the eye slid along the light axis and nothing else moved.
    CHECK(engine::approxEquals(with.lightView.columns[3], Vec4{1.0F, 2.0F, -13.0F, 1.0F}, 1.0e-5F));

    // ALL FOUR LATERAL ORTHO BOUNDS ARE BIT-IDENTICAL (AC-25's own wording): the extension is a
    // DEPTH-only change, and an implementation that widened the box instead would fail here. Note
    // that columns[3].x/.y are the SNAP cells and are non-zero, so this pair now also asserts that
    // the caster extension does not disturb the texel lattice.
    CHECK(with.lightProj.columns[0] == none.lightProj.columns[0]);
    CHECK(with.lightProj.columns[1] == none.lightProj.columns[1]);
    CHECK(with.lightProj.columns[3].x == none.lightProj.columns[3].x);
    CHECK(with.lightProj.columns[3].y == none.lightProj.columns[3].y);
    CHECK(with.texelWorldSize == none.texelWorldSize);  // r did not move, so neither did the quantum

    // zNear is UNCHANGED at SHADOW_NEAR_MARGIN and zFar grew by exactly 4.
    const auto zNearOf = [](const ShadowFit& f) { return f.lightProj.columns[3].z / f.lightProj.columns[2].z; };
    const auto zFarOf = [&](const ShadowFit& f) { return zNearOf(f) - (1.0F / f.lightProj.columns[2].z); };
    CHECK(std::abs(zNearOf(with) - zNearOf(none)) <= 1.0e-5F);
    CHECK(std::abs(zNearOf(with) - SHADOW_NEAR_MARGIN) <= 1.0e-4F);
    CHECK(std::abs((zFarOf(with) - zFarOf(none)) - 4.0F) <= 1.0e-3F);
    CHECK(std::abs(zFarOf(with) - 17.0F) <= 1.0e-3F);
    // AND THE POINT OF ALL OF IT, worked as literals rather than as a range check. The caster's
    // topmost point is world (1, 12, -2):
    //   WITHOUT the extension (eye at (1, 9, -2)): light space (0, 0, +3) -- BEHIND the eye --
    //     and ndc.z = (-1/12)(3) + (-1/12) = -0.25 - 0.08333 = -0.33333, i.e. clipped away.
    //   WITH it (eye at (1, 13, -2)): light space (0, 0, -1), exactly one unit from the eye, and
    //     ndc.z = (-1/16)(-1) + (-1/16) = 0.0625 - 0.0625 = 0 -- EXACTLY the near plane, which is
    //     what "the margin is the head-room" means numerically.
    const Vec3 casterNdc = toNdc(shadowViewProj(with), Vec3{1.0F, 12.0F, -2.0F});
    CHECK(std::abs(casterNdc.z - 0.0F) <= 1.0e-5F);
    const Vec3 wasClipped = toNdc(shadowViewProj(none), Vec3{1.0F, 12.0F, -2.0F});
    CHECK(std::abs(wasClipped.z + 0.33333333F) <= 1.0e-5F);
    CHECK(wasClipped.z < 0.0F);  // without the extension it is in FRONT of the near plane
}

TEST_CASE("render shadow: an invalid or in-front caster box contributes nothing (SF26)") {
    // THE FIXTURE THAT ACTUALLY WITNESSES THE GUARD IS THE SECOND ONE, and the reason is measured
    // rather than assumed. The INF sentinel's center() is NaN and its halfExtent() is -inf, so an
    // UNGUARDED computation reaches std::max(0.0F, NaN) -- which returns 0.0F, because std::max is
    // `a < b ? b : a` and `0 < NaN` is false. The NaN is LAUNDERED, so deleting Aabb::valid()
    // changes nothing for this box. An inverted but FINITE box is the one that separates them: its
    // halfExtent is negative and its arithmetic stays finite, so an unguarded fit extends the depth
    // range by a real amount.
    const ShadowFit sentinel = referenceFit();
    REQUIRE(sentinel.valid);
    const auto zFarOf = [](const ShadowFit& f) {
        return (f.lightProj.columns[3].z / f.lightProj.columns[2].z) - (1.0F / f.lightProj.columns[2].z);
    };
    CHECK(std::abs(zFarOf(sentinel) - 13.0F) <= 1.0e-3F);   // 2r + 0 + 1, i.e. casterBack == 0
    CHECK(std::isfinite(sentinel.lightView.columns[3].z));  // not NaN

    // A box entirely IN FRONT of the sphere (away from the light) also gives casterBack == 0:
    //   box centre (1, -17, -2); signedDistance = -17 - 8 = -25; support = 1; max(0, -24) = 0.
    constexpr Aabb IN_FRONT{Vec3{0.0F, -18.0F, -3.0F}, Vec3{2.0F, -16.0F, -1.0F}};
    const ShadowFit front =
        fitDirectionalShadow(orthoCamera(), SUN_DOWN, REF_SHADOW_DISTANCE, REF_RESOLUTION, IN_FRONT);
    REQUIRE(front.valid);
    // BIT-IDENTICAL to the sentinel fit, every cell -- a clamp that let a negative casterBack through
    // would pull the eye FORWARD and fail here.
    CHECK(front.lightView == sentinel.lightView);
    CHECK(front.lightProj == sentinel.lightProj);
    CHECK(front.texelWorldSize == sentinel.texelWorldSize);

    // AN INVERTED BUT FINITE BOX -- invalid by valid()'s ORDERING half alone, with no infinity to be
    // laundered. Hand-computed for the unguarded path, so the numbers say what the guard is worth:
    //   center (1, 19, -1), halfExtent (-1, -1, -1)   <- NEGATIVE, which is what `invalid` means here
    //   the sphere's far-face plane is { (0,1,0), -8 }; signedDistance = 19 - 8 = 11
    //   support = |0|*(-1) + |1|*(-1) + |0|*(-1) = -1
    //   casterBack (unguarded) = max(0, 11 - 1) = 10, so zFar would become 2r + 10 + 1 = 23
    // WITH the guard it is 0 and zFar stays 13. Deleting Aabb::valid() reddens HERE and nowhere
    // else in this battery.
    constexpr Aabb INVERTED{Vec3{2.0F, 20.0F, 0.0F}, Vec3{0.0F, 18.0F, -2.0F}};
    CHECK_FALSE(INVERTED.valid());
    const ShadowFit inverted =
        fitDirectionalShadow(orthoCamera(), SUN_DOWN, REF_SHADOW_DISTANCE, REF_RESOLUTION, INVERTED);
    REQUIRE(inverted.valid);
    CHECK(std::abs(zFarOf(inverted) - 13.0F) <= 1.0e-3F);  // NOT 23: the box contributed nothing
    CHECK(inverted.lightView == sentinel.lightView);
    CHECK(inverted.lightProj == sentinel.lightProj);
}

TEST_CASE("render shadow: the ortho extent does not move as the camera yaws (SF27)") {
    // RELATIVE 1e-3, not bit-identity, and the number is MEASURED (the plan's section R.16 records
    // the class and the correction to its own first estimate). The residual is dominated by precision
    // loss in the NDC-depth round trip through inverse(proj * view) and scales a decade per decade of
    // depth ratio: 1e1 -> 1.366e-6, 1e2 -> 1.01e-5, 1e3 -> 1.132e-4. This camera is 0.1/100, so
    // 1.132e-4 is the floor and 1e-3 leaves ~9x headroom. What the assertion discriminates against is
    // a light-space BOX fit, whose half-extent swings by roughly 2x over this same sweep -- THREE
    // orders of magnitude outside even this looser tolerance.
    const Vec3 eye{2.0F, 3.0F, 4.0F};
    float minExtent = INF;
    float maxExtent = 0.0F;
    float minTexel = INF;
    float maxTexel = 0.0F;
    for (int step = 0; step < 36; ++step) {
        const float yaw = engine::radians(static_cast<float>(step) * 10.0F);
        CameraView camera;
        camera.view = engine::lookAt(eye, eye + Vec3{std::sin(yaw), -0.2F, -std::cos(yaw)}, Vec3{0.0F, 1.0F, 0.0F});
        camera.proj = engine::perspective(engine::radians(60.0F), 16.0F / 9.0F, 0.1F, 100.0F);
        camera.eyePosition = eye;
        const ShadowFit fit = fitDirectionalShadow(camera, Vec3{-0.4F, -0.8F, -0.45F}, 40.0F, 2048, INVALID_BOX);
        INFO("yaw step ", step);
        REQUIRE(fit.valid);
        const float halfExtent = 1.0F / fit.lightProj.columns[0].x;  // (right - left) / 2
        minExtent = std::min(minExtent, halfExtent);
        maxExtent = std::max(maxExtent, halfExtent);
        minTexel = std::min(minTexel, fit.texelWorldSize);
        maxTexel = std::max(maxTexel, fit.texelWorldSize);
    }
    CHECK(minExtent > 0.0F);
    CHECK((maxExtent - minExtent) / maxExtent <= 1.0e-3F);
    // The texel size is the extent over the resolution, so it is stable for the same reason -- and it
    // is the quantity that actually matters, because a texel whose WORLD size changes every frame
    // makes every shadow edge crawl even when the extent looks steady.
    CHECK((maxTexel - minTexel) / maxTexel <= 1.0e-3F);
    // ...and a CONSTANT extent is only half of "does not crawl". The other half -- that the lattice's
    // POSITION is pinned to the world -- is SF28, and neither case is evidence without the other.
}

TEST_CASE("render shadow: the ortho volume's edge sits on a world-fixed texel lattice (SF28)") {
    // THE ASSERTION THAT ACTUALLY CATCHES A DEAD SNAP, and it took a defect to find it. The obvious
    // form -- "the recovered ortho centre is a whole multiple of the texel" -- is satisfied by a
    // centre that is ALWAYS ZERO, which is exactly what snapping the identically-(0, 0, -k)
    // lightView-space centre produces. A quantisation claim is worthless without a companion that
    // fails when the quantity is constant (INV-8).
    //
    // What is pinned instead is the WORLD-ANCHORED position of the volume's left/bottom edge:
    //     worldEdgeX = dot(s, centre) + (lightView-space left bound)
    //                = a + (-m30/m00) - (1/m00)
    // and THAT must be a whole multiple of texelWorldSize at every yaw. Under a dead snap it is
    // a - r for an arbitrary real a, which is a multiple of nothing.
    const Vec3 eye{2.0F, 3.0F, 4.0F};
    const Vec3 sun{-0.4F, -0.8F, -0.45F};
    const auto fitAt = [&](float yawDegrees) {
        const float yaw = engine::radians(yawDegrees);
        CameraView camera;
        camera.view = engine::lookAt(eye, eye + Vec3{std::sin(yaw), -0.2F, -std::cos(yaw)}, Vec3{0.0F, 1.0F, 0.0F});
        camera.proj = engine::perspective(engine::radians(60.0F), 16.0F / 9.0F, 0.1F, 100.0F);
        camera.eyePosition = eye;
        return fitDirectionalShadow(camera, sun, 40.0F, 2048, INVALID_BOX);
    };
    // The light's lateral axes depend on the SUN and the up-axis rule alone, so they are the same at
    // every yaw and can be taken once. Reading them from a fit rather than recomputing them keeps
    // this case from re-implementing the thing it is testing.
    const ShadowFit reference = fitAt(0.0F);
    REQUIRE(reference.valid);
    const Vec3 sAxis{reference.lightView.columns[0].x, reference.lightView.columns[1].x,
                     reference.lightView.columns[2].x};
    const Vec3 uAxis{reference.lightView.columns[0].y, reference.lightView.columns[1].y,
                     reference.lightView.columns[2].y};
    CHECK(std::abs(engine::dot(sAxis, uAxis)) <= 1.0e-5F);  // orthonormal, so `a` below is meaningful

    float minResidueX = INF;
    float maxResidueX = -INF;
    float minEdgeX = INF;
    float maxEdgeX = -INF;
    for (int step = 0; step < 36; ++step) {
        const ShadowFit fit = fitAt(static_cast<float>(step) * 10.0F);
        INFO("yaw step ", step);
        REQUIRE(fit.valid);
        REQUIRE(fit.texelWorldSize > 0.0F);
        // The centre's world-anchored lateral pair, recovered from the light view: lookAt puts the
        // eye at centre - dir * k, and columns[3].x/.y are -dot(s, eye) / -dot(u, eye) with s and u
        // perpendicular to dir -- so dot(s, centre) == -columns[3].x.
        const float a = -fit.lightView.columns[3].x;
        const float b = -fit.lightView.columns[3].y;
        const float halfExtent = 1.0F / fit.lightProj.columns[0].x;
        const float centreX = -fit.lightProj.columns[3].x / fit.lightProj.columns[0].x;
        const float centreY = -fit.lightProj.columns[3].y / fit.lightProj.columns[1].y;

        // (1) THE PRIMARY: the world-anchored left and bottom edges are ON the lattice. The tolerance
        // is 1e-2 of a TEXEL; a dead snap lands uniformly anywhere in [0, 1).
        const float edgeX = a + centreX - halfExtent;
        const float edgeY = b + centreY - halfExtent;
        const float ticksX = edgeX / fit.texelWorldSize;
        const float ticksY = edgeY / fit.texelWorldSize;
        CHECK(std::abs(ticksX - std::round(ticksX)) <= 1.0e-2F);
        CHECK(std::abs(ticksY - std::round(ticksY)) <= 1.0e-2F);

        // (2) The correction is SUB-TEXEL and never a whole-texel jump, and it is never positive --
        // the residue is in [0, texel), so the shift is in (-texel, 0]. A dead snap driven by the
        // sign of rounding noise alternates between 0 and -texel, and -texel fails the first line.
        CHECK(std::abs(centreX) < fit.texelWorldSize);
        CHECK(std::abs(centreY) < fit.texelWorldSize);
        CHECK(centreX <= 0.0F);
        CHECK(centreY <= 0.0F);

        minResidueX = std::min(minResidueX, -centreX);
        maxResidueX = std::max(maxResidueX, -centreX);
        minEdgeX = std::min(minEdgeX, edgeX);
        maxEdgeX = std::max(maxEdgeX, edgeX);
    }

    // (3) THE NON-VACUITY HALF (INV-8), in two parts, because (1) alone is satisfied by a fit that
    // never moves at all:
    //   (a) the volume genuinely TRAVELS across the sweep -- many texels, not a jitter;
    //   (b) the RESIDUE itself genuinely varies, so the snap is doing work at more than one yaw and
    //       (1) is not passing because the residue happens to be zero everywhere.
    CHECK((maxEdgeX - minEdgeX) > 10.0F * reference.texelWorldSize);
    CHECK((maxResidueX - minResidueX) > 0.25F * reference.texelWorldSize);
    CHECK(maxResidueX > 0.0F);
}

// ================================================================================================
// Tier 1 — a real Device, no window. Gated on AERO_SHADER_TOOLS_ENABLED for the reason
// render_culling_test.cpp's own tier-1 block is: a ForwardRenderer loads its shaders from
// build/<preset>/shaders, which only exists when the shader toolchain is built. The SF battery
// above runs in every configuration, because shadow.{hpp,cpp} is pure.
// ================================================================================================

#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/assets/mesh_cook.hpp>
    #include <aero/core/vfs.hpp>
    #include <aero/platform/platform.hpp>
    #include <aero/render/render.hpp>
    #include <aero/rhi/rhi.hpp>
    #include <aero/rhi/shader_loader.hpp>  // SM15 -- rhi.hpp deliberately does not re-export it

    #include "../engine/render/src/material_pack.hpp"
    #include "rhi_test_support.hpp"

    #include <memory>
    #include <utility>
    #include <vector>

using engine::render::MeshInstance;
using engine::render::PrimitiveId;
using engine::render::ShadowView;

namespace {

// The shadow map's resolution is a create()-time decision, so unlike the CD battery this one needs a
// PARAMETERISED renderer factory.
[[nodiscard]] std::optional<engine::render::ForwardRenderer> makeForwardRenderer(
    engine::rhi::Device& device, const engine::render::RenderTarget& target, std::uint32_t shadowResolution) {
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    return engine::render::ForwardRenderer::create(device, vfs,
                                                   {.colorFormat = target.colorFormat(),
                                                    .depthFormat = target.depthFormat(),
                                                    .shadowMapResolution = shadowResolution});
}

// A camera looking at the origin from above and behind, and a sun coming from up and to the left.
[[nodiscard]] engine::render::CameraView smCamera() {
    const Vec3 eye{0.0F, 4.0F, 8.0F};
    return {engine::lookAt(eye, Vec3{}, Vec3{0.0F, 1.0F, 0.0F}),
            engine::perspective(engine::radians(60.0F), 1.0F, 0.1F, 100.0F), eye};
}

[[nodiscard]] engine::render::RenderView smView(const engine::render::CameraView& camera,
                                                std::span<const MeshInstance> instances) {
    engine::render::RenderView view;
    view.camera = camera;
    view.instances = instances;
    view.directional = {.direction = engine::normalize(Vec3{-0.4F, -0.8F, -0.45F}),
                        .color = Vec3::one(),
                        .intensity = 2.0F,
                        .castsShadows = true,
                        .shadowBias = 0.0015F,
                        .shadowNormalBias = 0.02F,
                        .shadowDistance = 40.0F};
    return view;
}

[[nodiscard]] MeshInstance primitiveInstance(PrimitiveId primitive, const Mat4& model, const Mat4& viewProj) {
    MeshInstance instance;
    instance.primitive = primitive;
    instance.model = model;
    instance.normalMatrix = Mat4::identity();
    instance.mvp = viewProj * model;
    return instance;
}

}  // namespace

    #define AERO_SHADOW_TIER1_PREAMBLE(RES)                                    \
        const engine::platform::Context ctx{{.headless = false}};              \
        if (!ctx.valid()) {                                                    \
            AERO_SKIP_OR_FAIL("no real video driver available");               \
        }                                                                      \
        auto device = engine::rhi::Device::create();                           \
        if (!device.has_value()) {                                             \
            AERO_SKIP_OR_FAIL("no GPU device available");                      \
        }                                                                      \
        auto target = engine::render::RenderTarget::create(*device, {64, 64}); \
        REQUIRE(target.has_value());                                           \
        auto forward = makeForwardRenderer(*device, *target, (RES));           \
        REQUIRE(forward.has_value())

TEST_CASE("render shadow: create() allocates a shadow map and reports its resolution (SM1)") {
    AERO_SHADOW_TIER1_PREAMBLE(1024);
    CHECK(forward->shadowMapResolution() == 1024);
    CHECK(forward->shadowPassCount() == 0);  // nothing has been recorded yet
    CHECK(forward->lastFrameShadowDrawn() == 0);
    CHECK(forward->lastFrameShadowCulled() == 0);
    CHECK_FALSE(forward->hasWarnedShadowFit());
    // The 3.4.1 observable must NOT have moved: the comparison sampler is renderer-owned and
    // deliberately outside samplerCache, so a material-facing count is unaffected by shadows.
    CHECK(forward->samplerCacheSize() == 1);
}

TEST_CASE("render shadow: resolution 0 means OFF and is exact (SM2)") {
    AERO_SHADOW_TIER1_PREAMBLE(0);
    // 0 is never rounded and never clamped to 256 -- it means shadows off. The renderer still holds a
    // 1x1 DEPTH texture as the bind placeholder, because SPIRV-Cross emits depth2d<float> for the
    // comparison slot and binding an RGBA8 default there is a Metal type mismatch. Nothing exposes
    // the texture handle, so what this case can assert is the CONSEQUENCE: a draw still records
    // cleanly with slot 5 bound (SM13 proves that end to end) and renderShadowMap opts out.
    CHECK(forward->shadowMapResolution() == 0);
    const std::array<MeshInstance, 1> instances{
        primitiveInstance(PrimitiveId::Cube, Mat4::identity(), Mat4::identity())};
    const ShadowView shadow = forward->renderShadowMap(smView(smCamera(), instances));
    CHECK_FALSE(shadow.valid);
    CHECK(forward->shadowPassCount() == 0);
    CHECK_FALSE(forward->hasWarnedShadowFit());  // a DISABLED map is not a FAILED fit
}

TEST_CASE("render shadow: a non-power-of-two resolution rounds up then clamps (SM3)") {
    // One case rather than five, because the four arms share a fifty-line preamble and the FIFTH arm
    // -- the exact one -- only means something beside the other four.
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = engine::rhi::Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto target = engine::render::RenderTarget::create(*device, {64, 64});
    REQUIRE(target.has_value());

    struct Arm {
        std::uint32_t requested;
        std::uint32_t allocated;
    };
    // Hand-worked from the policy (round UP to the next power of two, THEN clamp into [256, 8192]):
    //   300   -> 512    (the next power of two, already in range)
    //   3000  -> 4096
    //   100   -> 128    -> clamped UP to 256
    //   20000 -> 8192   (the round stops at MAX, and the clamp agrees)
    //   2048  -> 2048   (exact: no WARN at all)
    constexpr std::array<Arm, 5> ARMS{{{300, 512}, {3000, 4096}, {100, 256}, {20000, 8192}, {2048, 2048}}};
    for (const Arm arm : ARMS) {
        INFO("requested ", arm.requested);
        auto renderer = makeForwardRenderer(*device, *target, arm.requested);
        REQUIRE(renderer.has_value());
        CHECK(renderer->shadowMapResolution() == arm.allocated);
    }
    // ...and the exact arm's real assertion is that it is NOT clamped to something else: 2048 is both
    // a power of two and in range, so requested == allocated and the WARN cannot fire.
    auto exact = makeForwardRenderer(*device, *target, 2048);
    REQUIRE(exact.has_value());
    CHECK(exact->shadowMapResolution() == 2048);
    CHECK(exact->shadowMapResolution() != 4096);
}

TEST_CASE("render shadow: every opt-out returns an invalid view and acquires no command buffer (SM4)") {
    AERO_SHADOW_TIER1_PREAMBLE(512);
    const engine::render::CameraView camera = smCamera();
    const Mat4 viewProj = camera.proj * camera.view;
    const std::array<MeshInstance, 1> instances{primitiveInstance(PrimitiveId::Cube, Mat4::identity(), viewProj)};

    // 1. no camera
    engine::render::RenderView noCamera = smView(camera, instances);
    noCamera.hasCamera = false;
    CHECK_FALSE(forward->renderShadowMap(noCamera).valid);
    // 2. the view opted out
    engine::render::RenderView off = smView(camera, instances);
    off.shadowsEnabled = false;
    CHECK_FALSE(forward->renderShadowMap(off).valid);
    // 3. the light does not cast
    engine::render::RenderView noCast = smView(camera, instances);
    noCast.directional.castsShadows = false;
    CHECK_FALSE(forward->renderShadowMap(noCast).valid);
    // 4. there is no directional light at all (intensity 0 is the sentinel, lighting.hpp's D6)
    engine::render::RenderView dark = smView(camera, instances);
    dark.directional.intensity = 0.0F;
    CHECK_FALSE(forward->renderShadowMap(dark).valid);

    // NOTHING WAS ACQUIRED AND NOTHING SUBMITTED, which is the half no other observable can see: the
    // counter increments at the ACQUISITION, so a version that acquired a command buffer and then
    // early-returned would read 4 here (and would leak four SDL command buffers into ~Device).
    CHECK(forward->shadowPassCount() == 0);
    CHECK(forward->lastFrameShadowDrawn() == 0);
    CHECK(forward->lastFrameShadowCulled() == 0);
    CHECK_FALSE(forward->hasWarnedShadowFit());  // none of the four is a FAILED fit

    // 5. an INVALID FIT is the fifth condition, and it is the one that DOES warn.
    engine::render::RenderView broken = smView(camera, instances);
    broken.directional.direction = Vec3{};  // zero-length sun
    CHECK_FALSE(forward->renderShadowMap(broken).valid);
    CHECK(forward->hasWarnedShadowFit());
    CHECK(forward->shadowPassCount() == 0);  // still nothing acquired: the fit runs BEFORE the acquire

    // 6. the positive control -- the same view with a real sun DOES acquire exactly one.
    CHECK(forward->renderShadowMap(smView(camera, instances)).valid);
    CHECK(forward->shadowPassCount() == 1);
}

TEST_CASE("render shadow: the returned ShadowView carries the light matrix, the texel step and both biases (SM5)") {
    AERO_SHADOW_TIER1_PREAMBLE(512);
    const engine::render::CameraView camera = smCamera();
    const Mat4 viewProj = camera.proj * camera.view;
    const std::array<MeshInstance, 1> instances{primitiveInstance(PrimitiveId::Cube, Mat4::identity(), viewProj)};
    engine::render::RenderView view = smView(camera, instances);
    // Values chosen MUTUALLY DISTINCT, so a swapped pair cannot land on the value expected.
    view.directional.shadowBias = 0.0037F;
    view.directional.shadowNormalBias = 0.041F;

    const ShadowView shadow = forward->renderShadowMap(view);
    REQUIRE(shadow.valid);
    CHECK(shadow.constantBias == 0.0037F);
    CHECK(shadow.normalBias == 0.041F);
    // texelSize is 1 / RESOLUTION -- a UV step -- and NOT the fit's world texel size. The two are
    // different quantities with similar names, and 1/512 is nowhere near the world value here.
    CHECK(shadow.texelSize == 1.0F / 512.0F);
    // The matrix is the fit's, not the identity: the sphere centre lands at the map's centre.
    CHECK_FALSE(shadow.lightViewProj == Mat4::identity());
    // ...and a second call with the SAME view returns a bit-identical matrix, which is what makes
    // "no cached state" observable: nothing accumulated between the two.
    const ShadowView again = forward->renderShadowMap(view);
    REQUIRE(again.valid);
    CHECK(again.lightViewProj == shadow.lightViewProj);
    CHECK(forward->shadowPassCount() == 2);
}

TEST_CASE("render shadow: the light block mirrors ShadowView, and an invalid one disables in the shader (SM6)") {
    // The one arm of the shadow path that is pure and could have lived at tier 0 -- it is here
    // because it needs a real ShadowView, which needs a renderer.
    AERO_SHADOW_TIER1_PREAMBLE(512);
    const engine::render::CameraView camera = smCamera();
    const Mat4 viewProj = camera.proj * camera.view;
    const std::array<MeshInstance, 1> instances{primitiveInstance(PrimitiveId::Cube, Mat4::identity(), viewProj)};
    engine::render::RenderView view = smView(camera, instances);
    view.directional.shadowBias = 0.0037F;
    view.directional.shadowNormalBias = 0.041F;

    view.shadow = forward->renderShadowMap(view);
    REQUIRE(view.shadow.valid);
    const engine::render::detail::GpuLightBlock lit = engine::render::detail::packLights(view);
    CHECK(lit.lightViewProj == view.shadow.lightViewProj);
    CHECK(lit.shadowParams.x == view.shadow.texelSize);
    CHECK(lit.shadowParams.y == 0.0037F);
    CHECK(lit.shadowParams.z == 0.041F);
    CHECK(lit.shadowParams.w == 1.0F);

    // The DEFAULT view -- a caller who never called renderShadowMap -- writes an identity matrix and
    // w == 0, which is the ONLY thing that turns shadowing off in the shader.
    view.shadow = ShadowView{};
    const engine::render::detail::GpuLightBlock unlit = engine::render::detail::packLights(view);
    CHECK(unlit.lightViewProj == Mat4::identity());
    CHECK(unlit.shadowParams == Vec4{});
    CHECK(unlit.shadowParams.w == 0.0F);
    // ...and nothing ahead of the tail moved, which is what "appended" means.
    CHECK(unlit.eyePosition == lit.eyePosition);
    CHECK(unlit.ambient == lit.ambient);
}

TEST_CASE("render shadow: every caster inside the light frustum is drawn into the map (SM7)") {
    AERO_SHADOW_TIER1_PREAMBLE(512);
    const engine::render::CameraView camera = smCamera();
    const Mat4 viewProj = camera.proj * camera.view;
    const std::array<MeshInstance, 3> instances{
        primitiveInstance(PrimitiveId::Cube, Mat4::identity(), viewProj),
        primitiveInstance(PrimitiveId::Sphere, engine::translation(Vec3{1.5F, 0.0F, 0.0F}), viewProj),
        primitiveInstance(PrimitiveId::Plane, engine::translation(Vec3{0.0F, -1.0F, 0.0F}), viewProj)};

    REQUIRE(forward->renderShadowMap(smView(camera, instances)).valid);
    CHECK(forward->lastFrameShadowDrawn() == 3);
    CHECK(forward->lastFrameShadowCulled() == 0);
    CHECK(forward->shadowPassCount() == 1);
    // PER-FRAME, not cumulative: an identical second call reads the same numbers, not double.
    REQUIRE(forward->renderShadowMap(smView(camera, instances)).valid);
    CHECK(forward->lastFrameShadowDrawn() == 3);
    CHECK(forward->lastFrameShadowCulled() == 0);
    CHECK(forward->shadowPassCount() == 2);
}

TEST_CASE("render shadow: a caster outside the LIGHT frustum is culled, and cullingEnabled is not consulted (SM8)") {
    AERO_SHADOW_TIER1_PREAMBLE(512);
    const engine::render::CameraView camera = smCamera();
    const Mat4 viewProj = camera.proj * camera.view;
    // The fit follows the CAMERA's frustum slice, so "outside the light frustum" here means far
    // outside the camera's reach as well -- 500 units up the +X axis, which is many times the fit
    // radius for this 40-unit shadowDistance.
    constexpr Mat4 FAR_AWAY = engine::translation(Vec3{500.0F, 0.0F, 0.0F});
    const std::array<MeshInstance, 3> instances{primitiveInstance(PrimitiveId::Cube, Mat4::identity(), viewProj),
                                                primitiveInstance(PrimitiveId::Cube, FAR_AWAY, viewProj),
                                                primitiveInstance(PrimitiveId::Sphere, FAR_AWAY, viewProj)};

    engine::render::RenderView view = smView(camera, instances);
    REQUIRE(forward->renderShadowMap(view).valid);
    CHECK(forward->lastFrameShadowDrawn() == 1);
    CHECK(forward->lastFrameShadowCulled() == 2);

    // THE POINT OF THE CASE (D8): RenderView::cullingEnabled is the CAMERA cull's escape hatch and
    // carries a contract about mvp == viewProj * model. The light cull is unconditional, because an
    // instance outside the light frustum writes to no texel BY DEFINITION. An opted-out view must
    // still cull here.
    view.cullingEnabled = false;
    REQUIRE(forward->renderShadowMap(view).valid);
    CHECK(forward->lastFrameShadowDrawn() == 1);
    CHECK(forward->lastFrameShadowCulled() == 2);
}

TEST_CASE("render shadow: an instance carrying a PALETTE is exempt from the light cull (SM9)") {
    AERO_SHADOW_TIER1_PREAMBLE(512);
    const engine::render::CameraView camera = smCamera();
    const Mat4 viewProj = camera.proj * camera.view;
    constexpr Mat4 FAR_AWAY = engine::translation(Vec3{500.0F, 0.0F, 0.0F});
    // A one-entry identity palette. The built-in primitive path IGNORES a palette entirely, so this
    // witnesses the `!palette.empty()` PREDICATE and nothing about skinning -- exactly as 3.6.1's
    // CD2 does for the camera cull. An instance whose vertices may move under a matrix this renderer
    // never sees has no bind-pose box that bounds where it ends up.
    const std::array<Mat4, 1> palette{Mat4::identity()};
    MeshInstance exempt = primitiveInstance(PrimitiveId::Cube, FAR_AWAY, viewProj);
    exempt.palette = palette;
    const MeshInstance twin = primitiveInstance(PrimitiveId::Cube, FAR_AWAY, viewProj);
    const MeshInstance onScreen = primitiveInstance(PrimitiveId::Cube, Mat4::identity(), viewProj);
    const std::array<MeshInstance, 3> instances{exempt, twin, onScreen};

    REQUIRE(forward->renderShadowMap(smView(camera, instances)).valid);
    CHECK(forward->lastFrameShadowDrawn() == 2);   // the exempt one (far outside, drawn anyway) + onScreen
    CHECK(forward->lastFrameShadowCulled() == 1);  // its palette-free twin, same position

    // ...and it also contributed NOTHING to the CASTER UNION, which is what keeps a skinned
    // instance's bind-pose box from dragging the light eye back. The control is the SAME view with
    // the exempt instance removed -- so the union (and therefore casterBack, and therefore the depth
    // range) is identical by construction, and any difference in the matrix is the exempt instance
    // having been folded in. The twin stays in BOTH sides deliberately: it is what makes the union
    // non-trivial, so this is not two identical trivial fits agreeing.
    const std::array<MeshInstance, 2> control{twin, onScreen};
    const ShadowView withExempt = forward->renderShadowMap(smView(camera, instances));
    const ShadowView withoutIt = forward->renderShadowMap(smView(camera, control));
    REQUIRE(withExempt.valid);
    REQUIRE(withoutIt.valid);
    CHECK(withExempt.lightViewProj == withoutIt.lightViewProj);
}

TEST_CASE("render shadow: a shadowless call resets both counters to zero (SM10)") {
    // ONE sequence, deliberately. The reset has to happen BEFORE the six early returns, and only a
    // call that FOLLOWS a populated one can tell the difference -- two independent cases both pass
    // under the very defect this exists to catch, which is 3.6.1's CD5 lesson verbatim.
    AERO_SHADOW_TIER1_PREAMBLE(512);
    const engine::render::CameraView camera = smCamera();
    const Mat4 viewProj = camera.proj * camera.view;
    constexpr Mat4 FAR_AWAY = engine::translation(Vec3{500.0F, 0.0F, 0.0F});
    const std::array<MeshInstance, 2> instances{primitiveInstance(PrimitiveId::Cube, Mat4::identity(), viewProj),
                                                primitiveInstance(PrimitiveId::Cube, FAR_AWAY, viewProj)};

    REQUIRE(forward->renderShadowMap(smView(camera, instances)).valid);
    REQUIRE(forward->lastFrameShadowDrawn() == 1);
    REQUIRE(forward->lastFrameShadowCulled() == 1);

    engine::render::RenderView disabled = smView(camera, instances);
    disabled.directional.castsShadows = false;
    CHECK_FALSE(forward->renderShadowMap(disabled).valid);
    CHECK(forward->lastFrameShadowDrawn() == 0);
    CHECK(forward->lastFrameShadowCulled() == 0);

    // ...and again through the INVALID-FIT exit, which is a different early return in a different
    // place: a reset placed after the opt-out block but before the fit would pass the arm above and
    // fail this one.
    REQUIRE(forward->renderShadowMap(smView(camera, instances)).valid);
    REQUIRE(forward->lastFrameShadowDrawn() == 1);
    engine::render::RenderView broken = smView(camera, instances);
    broken.directional.shadowDistance = -1.0F;
    CHECK_FALSE(forward->renderShadowMap(broken).valid);
    CHECK(forward->lastFrameShadowDrawn() == 0);
    CHECK(forward->lastFrameShadowCulled() == 0);
}

TEST_CASE("render shadow: hasWarnedShadowFit latches across repeated invalid fits (SM11)") {
    AERO_SHADOW_TIER1_PREAMBLE(512);
    const engine::render::CameraView camera = smCamera();
    const std::array<MeshInstance, 1> instances{
        primitiveInstance(PrimitiveId::Cube, Mat4::identity(), camera.proj * camera.view)};
    engine::render::RenderView broken = smView(camera, instances);
    broken.directional.direction = Vec3{};

    CHECK_FALSE(forward->hasWarnedShadowFit());
    for (int i = 0; i < 5; ++i) {
        CHECK_FALSE(forward->renderShadowMap(broken).valid);
        CHECK(forward->hasWarnedShadowFit());
    }
    // A GOOD frame afterwards does NOT clear it -- it is a renderer-lifetime latch, not a per-frame
    // flag, and a version that reset it would flood the log at 60 Hz on an alternating scene.
    CHECK(forward->renderShadowMap(smView(camera, instances)).valid);
    CHECK(forward->hasWarnedShadowFit());
}

TEST_CASE("render shadow: a Mask material casts a solid silhouette rather than being skipped (SM12)") {
    // D13: a depth-only pass has no UVs, no material bind and cannot discard, so an alpha-masked
    // material casts its FULL silhouette. That is the behaviour, and it is what this asserts. The
    // "warns exactly once" half rides the warnOnce idiom every sibling latch here uses and has no
    // accessor by decision (the plan's section 0.8 records the limit rather than adding a sixth
    // observable whose only consumer would be this line).
    AERO_SHADOW_TIER1_PREAMBLE(512);
    const engine::render::MaterialHandle masked =
        forward->createMaterial({.baseColorFactor = Vec4{1.0F, 1.0F, 1.0F, 1.0F},
                                 .alpha = engine::render::MaterialAlpha::Mask,
                                 .alphaCutoff = 0.5F},
                                {});
    REQUIRE(masked.valid());
    const engine::render::CameraView camera = smCamera();
    MeshInstance instance = primitiveInstance(PrimitiveId::Cube, Mat4::identity(), camera.proj * camera.view);
    instance.material = masked;
    const std::array<MeshInstance, 1> instances{instance};

    REQUIRE(forward->renderShadowMap(smView(camera, instances)).valid);
    CHECK(forward->lastFrameShadowDrawn() == 1);  // DRAWN, not skipped
    CHECK(forward->lastFrameShadowCulled() == 0);
    // A second pass still draws it -- the latch changes the LOG, never the picture.
    REQUIRE(forward->renderShadowMap(smView(camera, instances)).valid);
    CHECK(forward->lastFrameShadowDrawn() == 1);
    // ...and the MAIN loop's own Blend latch is untouched by any of this: Mask is not Blend.
    CHECK_FALSE(forward->hasWarnedBlendOpaque());
}

TEST_CASE("render shadow: renderShadowMap then draw records cleanly with the shadow slot bound (SM13)") {
    // THE integration case: a depth-only pass on its own command buffer, submitted, then a colour
    // pass on the frame's command buffer that SAMPLES the result -- the first time in this project's
    // history that one submission's depth output is read by a later one. No pixel assertions (this
    // suite records draws without asserting their output, the tree's own posture); the assertion is
    // that both passes record and submit with no backend validation error, which on Metal means the
    // depth2d<float> binding at slot 5 was type-correct.
    AERO_SHADOW_TIER1_PREAMBLE(512);
    const engine::render::CameraView camera = smCamera();
    const Mat4 viewProj = camera.proj * camera.view;
    const std::array<MeshInstance, 3> instances{
        primitiveInstance(PrimitiveId::Cube, engine::translation(Vec3{0.0F, 1.0F, 0.0F}), viewProj),
        primitiveInstance(PrimitiveId::Sphere, engine::translation(Vec3{2.0F, 1.0F, 0.0F}), viewProj),
        primitiveInstance(PrimitiveId::Plane, engine::scaling(Vec3{10.0F, 1.0F, 10.0F}), viewProj)};

    engine::render::RenderView view = smView(camera, instances);
    view.shadow = forward->renderShadowMap(view);
    REQUIRE(view.shadow.valid);
    CHECK(forward->shadowPassCount() == 1);

    std::optional<engine::render::Frame> open = target->beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(open.has_value());
    forward->draw(*open, view);
    CHECK(target->endFrame(std::move(*open)));
    CHECK(forward->lastFrameDrawn() == 3);
    CHECK(forward->lastFrameShadowDrawn() == 3);

    // ...and the SAME sequence with shadows off, which is the arm that proves slot 5 is bound
    // unconditionally: with shadowsEnabled false nothing is recorded into the map, but the fragment
    // stage still declares six samplers and the draw must still record.
    engine::render::RenderView unshadowed = smView(camera, instances);
    unshadowed.shadowsEnabled = false;
    unshadowed.shadow = forward->renderShadowMap(unshadowed);
    CHECK_FALSE(unshadowed.shadow.valid);
    std::optional<engine::render::Frame> second = target->beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(second.has_value());
    forward->draw(*second, unshadowed);
    CHECK(target->endFrame(std::move(*second)));
    CHECK(forward->lastFrameDrawn() == 3);
}

TEST_CASE("render shadow: the shared resolver drops the same instances from both loops (SM14)") {
    AERO_SHADOW_TIER1_PREAMBLE(512);
    // A cooked triangle at the origin, registered twice so one handle can be made STALE -- the
    // render_culling_test.cpp CD3 shape, cooked IN MEMORY (3.3.3 makes the cook deterministic
    // cross-lane, so this is as stable as a golden and commits no fixture).
    constexpr std::array<Vec3, 3> TRI{Vec3{-0.5F, -0.5F, 0.0F}, Vec3{0.5F, -0.5F, 0.0F}, Vec3{0.0F, 0.5F, 0.0F}};
    constexpr std::array<std::uint32_t, 3> TRI_INDICES{0, 1, 2};
    engine::assets::MeshCookPrimitive primitive;
    primitive.positions = TRI;
    primitive.indices = TRI_INDICES;
    const std::array<engine::assets::MeshCookPrimitive, 1> primitives{primitive};
    engine::assets::MeshCookResult cooked = engine::assets::cookMesh({.sourceGuid = {}, .primitives = primitives});
    REQUIRE(cooked.status == engine::assets::MeshCookStatus::Ok);
    const std::vector<std::byte> bytes = std::move(cooked.bytes);  // MUST outlive the CookedMesh
    const engine::assets::CookedMeshParseResult parse = engine::assets::parseCookedMesh(bytes);
    REQUIRE(parse.status == engine::assets::CookedMeshStatus::Ok);
    const engine::render::MeshHandle live = forward->createMesh(parse.mesh);
    REQUIRE(live.valid());
    const engine::render::MeshHandle stale = forward->createMesh(parse.mesh);
    REQUIRE(stale.valid());
    forward->destroyMesh(stale);

    const engine::render::CameraView camera = smCamera();
    const Mat4 viewProj = camera.proj * camera.view;
    const auto meshInstance = [&](engine::render::MeshHandle mesh, std::uint32_t submesh) {
        MeshInstance instance;
        instance.mesh = mesh;
        instance.submesh = submesh;
        instance.model = Mat4::identity();
        instance.normalMatrix = Mat4::identity();
        instance.mvp = viewProj;
        return instance;
    };
    // An over-cap palette: MAX_SKINNING_JOINTS + 1 entries. The section carries no skin stream, so
    // this ALSO exercises the stray-palette arm -- which WARNs and does NOT skip -- and therefore
    // proves the resolver kept the two apart.
    const std::vector<Mat4> overCap(engine::render::MAX_SKINNING_JOINTS + 1, Mat4::identity());
    MeshInstance capped = meshInstance(live, 0);
    capped.palette = overCap;
    const std::array<MeshInstance, 4> instances{meshInstance(live, 0),   // draws
                                                meshInstance(stale, 0),  // ARM 2a: neither bucket
                                                meshInstance(live, 7),   // ARM 2b: neither bucket
                                                capped};                 // stray palette: DRAWS

    // The SHADOW pass first, deliberately: if the resolver logged, this is where a diagnostic would
    // be consumed before draw() could own it.
    REQUIRE(forward->renderShadowMap(smView(camera, instances)).valid);
    // Two drawn (the plain one and the stray-palette one, which is not skipped), two dropped by the
    // resolver into NEITHER bucket. THE DOCUMENTED NON-SUM.
    CHECK(forward->lastFrameShadowDrawn() == 2);
    CHECK(forward->lastFrameShadowCulled() == 0);
    CHECK(forward->lastFrameShadowDrawn() + forward->lastFrameShadowCulled() == 2);
    CHECK(instances.size() == 4);
    // The shadow pass fired NONE of draw()'s latches.
    CHECK_FALSE(forward->hasWarnedSkinningCap());

    engine::render::RenderView view = smView(camera, instances);
    view.shadow = forward->renderShadowMap(view);
    std::optional<engine::render::Frame> open = target->beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(open.has_value());
    forward->draw(*open, view);
    CHECK(target->endFrame(std::move(*open)));
    // draw() reaches the SAME two, and its own latch fires there -- unaffected by the shadow pass
    // having run twice already.
    CHECK(forward->lastFrameDrawn() == 2);
    CHECK(forward->lastFrameCulled() == 0);
}

TEST_CASE("render shadow: a pipeline with neither a colour target nor a depth target is refused (SM15)") {
    // SH29's ONLY witness, and it has to live here rather than beside rhi_device_test.cpp's other
    // structural-refusal cases. That file deliberately has no shader artifacts, so its handles are
    // never valid and createGraphicsPipeline returns an invalid handle for BOTH reasons -- the
    // structural refusal and the later shader-handle refusal -- which cannot discriminate.
    //
    // With REAL shaders it discriminates cleanly, and the reason is SDL's own validation: the only
    // "no targets at all" refusal in SDL_gpu.c is inside the enable_alpha_to_coverage branch (:1084),
    // which this engine never sets. So nothing downstream refuses this shape, and deleting our own
    // `iff` arm makes it SUCCEED.
    AERO_SHADOW_TIER1_PREAMBLE(512);
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    const engine::rhi::ShaderHandle vs = engine::rhi::loadShader(*device, vfs, "res://shadow.vert");
    const engine::rhi::ShaderHandle fs = engine::rhi::loadShader(*device, vfs, "res://shadow.frag");
    REQUIRE(vs.valid());
    REQUIRE(fs.valid());

    const engine::rhi::VertexBufferLayout layout{.slot = 0, .pitch = sizeof(engine::render::MeshVertex)};
    const std::array<engine::rhi::VertexAttribute, 1> attrs{
        {{.location = 0, .bufferSlot = 0, .format = engine::rhi::VertexFormat::Float3, .offset = 0}}};

    // THE POSITIVE CONTROL FIRST, so the refusal below is not passing for the wrong reason: the SAME
    // desc WITH a depth format is the depth-only shape the widening exists for, and it must succeed.
    engine::rhi::GraphicsPipelineDesc desc{.vertexShader = vs,
                                           .fragmentShader = fs,
                                           .vertexBuffers = std::span{&layout, 1},
                                           .vertexAttributes = attrs,
                                           .colorTargets = {},
                                           .depthStencilFormat = engine::rhi::TextureFormat::D32Float};
    const engine::rhi::GraphicsPipelineHandle depthOnly = device->createGraphicsPipeline(desc);
    CHECK(depthOnly.valid());
    if (depthOnly.valid()) {
        device->destroyGraphicsPipeline(depthOnly);
    }

    // ...and NEITHER target is still refused, by US. Nothing downstream would refuse it.
    desc.depthStencilFormat = engine::rhi::TextureFormat::Invalid;
    const engine::rhi::GraphicsPipelineHandle neither = device->createGraphicsPipeline(desc);
    CHECK_FALSE(neither.valid());
    if (neither.valid()) {
        device->destroyGraphicsPipeline(neither);  // never reached with the guard in place
    }

    device->destroyShader(vs);
    device->destroyShader(fs);
}

    #undef AERO_SHADOW_TIER1_PREAMBLE

#endif  // AERO_SHADER_TOOLS_ENABLED

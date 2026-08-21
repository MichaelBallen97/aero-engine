// tests/render_culling_test.cpp -- task 3.6.1: frustum culling (FC1-FC24, CD1-CD10). A TU of
// aero_tests, which supplies main() from test_main.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier 0 (no GPU, every lane, FC*): the [0,1]-depth Gribb-Hartmann extraction pinned against
// HAND-DERIVED plane literals, the conservative (Arvo) box transform, and the six-plane visibility
// test including both of its special cases. The reference projection is built TWICE and compared
// against the SAME literals -- once as column literals written out in this file, once through
// engine::perspective -- so the production input and its hand-built twin are each pinned to the
// numbers rather than to each other. A function compared against itself witnesses nothing.
//
// Tier 1 (a real Device, NO window -- RenderTarget supplies the formats, gated by AERO_SKIP_OR_FAIL,
// CD*): the cull inside ForwardRenderer::draw -- the per-frame counters, the exemptions, the
// placement ahead of material resolution, and the degenerate-projection latch.
//
// <ostream> is included preventively: MSVC alone needs the complete type to stringify a string_view
// inside a doctest CHECK (the four-time trap in .claude/rules/ci-portability.md).
//
// Nothing here is named lowercase near/far: <windows.h> #defines both as empty macros. The
// FrustumPlane::Near/Far ENUMERATORS are capitalised and therefore safe.

#include <aero/assets/cooked_mesh.hpp>
#include <aero/core/math.hpp>
#include <aero/render/culling.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <ostream>

using engine::Mat4;
using engine::Vec3;
using engine::Vec4;
using engine::render::Aabb;
using engine::render::extractFrustum;
using engine::render::Frustum;
using engine::render::FrustumPlane;
using engine::render::isVisible;
using engine::render::Plane;
using engine::render::signedDistance;
using engine::render::toAabb;
using engine::render::transformAabb;

namespace {

constexpr float INF = std::numeric_limits<float>::infinity();
constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();

// ---- the reference projection, worked once -----------------------------------------------------
// perspectiveRH_ZO(60 deg, aspect 1, zNear 0.1, zFar 100), view identity. GLM's own formulas:
//   [0][0] = 1 / (aspect * tan(fovY/2))   [1][1] = 1 / tan(fovY/2)
//   [2][2] = zFar / (zNear - zFar)        [2][3] = -1        [3][2] = -(zFar*zNear)/(zFar-zNear)
// Written out as COLUMN LITERALS so the extraction is pinned against ADR-005's clip-volume
// inequality (0 <= z <= w), not against whatever the backend happens to produce.
// sqrt(3) comes from <numbers> rather than a spelled literal: clang-tidy's
// modernize-use-std-numbers rejects the literal form outright and CI runs
// --warnings-as-errors='*' (constants.hpp carries the same note about PI).
constexpr float REF_M00 = std::numbers::sqrt3_v<float>;  // 1 / tan(30 deg) == sqrt(3)
constexpr float REF_M22 = -1.001001F;                    // 100 / (0.1 - 100)
constexpr float REF_M32 = -0.1001001F;                   // -(100 * 0.1) / (100 - 0.1)
constexpr float REF_COS = 0.8660254F;                    // sqrt(3) / 2 -- the side planes' normal component

[[nodiscard]] Mat4 referenceProjection() {
    Mat4 m = Mat4::zero();
    m.columns[0] = Vec4{REF_M00, 0.0F, 0.0F, 0.0F};
    m.columns[1] = Vec4{0.0F, REF_M00, 0.0F, 0.0F};
    m.columns[2] = Vec4{0.0F, 0.0F, REF_M22, -1.0F};
    m.columns[3] = Vec4{0.0F, 0.0F, REF_M32, 0.0F};
    return m;
}

// The same projection through the production path -- engine::perspective, i.e. glm::perspectiveRH_ZO.
[[nodiscard]] Mat4 backendProjection() { return engine::perspective(engine::radians(60.0F), 1.0F, 0.1F, 100.0F); }

// The six planes referenceProjection() must extract to, hand-derived in the plan's own table:
//   left = r3+r0  right = r3-r0  bottom = r3+r1  top = r3-r1  NEAR = r2 ALONE  far = r3-r2
// with r_i the i-th ROW of the column-major matrix. Near-inside is -z - 0.1 >= 0 (the near plane at
// z = -0.1, -Z forward); far-inside is z + 100 >= 0.
[[nodiscard]] Frustum literalFrustum() {
    Frustum f{};
    f.planes[static_cast<std::size_t>(FrustumPlane::Left)] = Plane{Vec3{REF_COS, 0.0F, -0.5F}, 0.0F};
    f.planes[static_cast<std::size_t>(FrustumPlane::Right)] = Plane{Vec3{-REF_COS, 0.0F, -0.5F}, 0.0F};
    f.planes[static_cast<std::size_t>(FrustumPlane::Bottom)] = Plane{Vec3{0.0F, REF_COS, -0.5F}, 0.0F};
    f.planes[static_cast<std::size_t>(FrustumPlane::Top)] = Plane{Vec3{0.0F, -REF_COS, -0.5F}, 0.0F};
    f.planes[static_cast<std::size_t>(FrustumPlane::Near)] = Plane{Vec3{0.0F, 0.0F, -1.0F}, -0.1F};
    f.planes[static_cast<std::size_t>(FrustumPlane::Far)] = Plane{Vec3{0.0F, 0.0F, 1.0F}, 100.0F};
    return f;
}

// The EXACT-ARITHMETIC frustum: six axis-aligned planes bounding the cube [-1, 1]^3, so every
// fixture against it is integer-valued float arithmetic and tangency (s + r == 0) is constructible
// without rounding.
[[nodiscard]] Frustum boxFrustum() {
    Frustum f{};
    f.planes[static_cast<std::size_t>(FrustumPlane::Left)] = Plane{Vec3{1.0F, 0.0F, 0.0F}, 1.0F};
    f.planes[static_cast<std::size_t>(FrustumPlane::Right)] = Plane{Vec3{-1.0F, 0.0F, 0.0F}, 1.0F};
    f.planes[static_cast<std::size_t>(FrustumPlane::Bottom)] = Plane{Vec3{0.0F, 1.0F, 0.0F}, 1.0F};
    f.planes[static_cast<std::size_t>(FrustumPlane::Top)] = Plane{Vec3{0.0F, -1.0F, 0.0F}, 1.0F};
    f.planes[static_cast<std::size_t>(FrustumPlane::Near)] = Plane{Vec3{0.0F, 0.0F, 1.0F}, 1.0F};
    f.planes[static_cast<std::size_t>(FrustumPlane::Far)] = Plane{Vec3{0.0F, 0.0F, -1.0F}, 1.0F};
    return f;
}

[[nodiscard]] Aabb boxAt(Vec3 center, Vec3 halfExtent) { return Aabb{center - halfExtent, center + halfExtent}; }

[[nodiscard]] Aabb boxAt(Vec3 center, float halfExtent) {
    return boxAt(center, Vec3{halfExtent, halfExtent, halfExtent});
}

// The one quantity here with real cancellation error is the FAR plane's d: it is 0.1001001 divided
// by 0.001001, and that denominator is the difference of two floats a thousand times its own size,
// so ~3 significant digits are gone before the division ever happens. Everything else is exact to
// ~1e-7. Hence the tolerance on d scales with |d| -- FC6 owns the tight, relative pin.
void checkPlane(const Plane& actual, Vec3 normal, float d, float eps) {
    CHECK(engine::approxEquals(actual.normal, normal, eps));
    CHECK(std::abs(actual.d - d) <= eps * std::max(1.0F, std::abs(d)));
}

void checkAgainstLiterals(const Frustum& actual) {
    constexpr float EPS = 1.0e-4F;
    const Frustum expected = literalFrustum();
    for (const FrustumPlane which : {FrustumPlane::Left, FrustumPlane::Right, FrustumPlane::Bottom, FrustumPlane::Top,
                                     FrustumPlane::Near, FrustumPlane::Far}) {
        const Plane& want = expected.plane(which);
        checkPlane(actual.plane(which), want.normal, want.d, EPS);
    }
}

// The ten-box battery FC7 runs both halves over. Deliberately AWAY from the far plane except for
// the one box that is meant to be behind it -- the far plane's d carries the cancellation error
// above, and a battery box straddling it would compare two legitimately different numbers.
[[nodiscard]] std::array<Aabb, 10> visibilityBattery() {
    return {boxAt(Vec3{0.0F, 0.0F, -5.0F}, 0.5F),         // 0: comfortably inside
            boxAt(Vec3{0.0F, 0.0F, -0.10005F}, 1.0e-5F),  // 1: THE BOUNDARY BOX -- flips under seed C7
            boxAt(Vec3{0.0F, 0.0F, 5.0F}, 0.5F),          // 2: behind the eye
            boxAt(Vec3{-50.0F, 0.0F, -5.0F}, 0.5F),       // 3: past Left
            boxAt(Vec3{50.0F, 0.0F, -5.0F}, 0.5F),        // 4: past Right
            boxAt(Vec3{0.0F, 50.0F, -5.0F}, 0.5F),        // 5: past Top
            boxAt(Vec3{0.0F, -50.0F, -5.0F}, 0.5F),       // 6: past Bottom
            boxAt(Vec3{-2.8F, 0.0F, -5.0F}, 1.0F),        // 7: straddling Left
            boxAt(Vec3{0.0F, 0.0F, 0.0F}, 100.0F),        // 8: containing the whole frustum
            boxAt(Vec3{0.0F, 0.0F, -500.0F}, 1.0F)};      // 9: past Far
}

}  // namespace

// ================================================================================================
// Tier 0 -- the pure vocabulary. No GPU, every configuration, every lane.
// ================================================================================================

TEST_CASE("render culling: Aabb::valid()'s two halves reject different things (FC1)") {
    CHECK(Aabb{Vec3{-1.0F, -2.0F, -3.0F}, Vec3{3.0F, 2.0F, 1.0F}}.valid());
    // A POINT box is valid: a single-vertex submesh and a zero-size primitive are both legal, and
    // min <= max holds at equality.
    CHECK(Aabb{Vec3{1.0F, 2.0F, 3.0F}, Vec3{1.0F, 2.0F, 3.0F}}.valid());
    // Flat in one axis -- the plane primitive's own shape.
    CHECK(Aabb{Vec3{-0.5F, 0.0F, -0.5F}, Vec3{0.5F, 0.0F, 0.5F}}.valid());

    CHECK_FALSE(Aabb{Vec3{1.0F, 0.0F, 0.0F}, Vec3{-1.0F, 1.0F, 1.0F}}.valid());  // min > max on x

    // These two are rejected by the ORDERING half alone: NaN <= x is false in both directions, and
    // the cook's inverted sentinel has min > max on every axis. Seed C14 (drop the finiteness half)
    // leaves BOTH of them green, which is exactly why the arm below exists.
    CHECK_FALSE(Aabb{Vec3{NAN_F, 0.0F, 0.0F}, Vec3{1.0F, 1.0F, 1.0F}}.valid());
    CHECK_FALSE(Aabb{Vec3{INF, INF, INF}, Vec3{-INF, -INF, -INF}}.valid());

    // THE FINITENESS HALF'S UNIQUE CATCH: an ORDERED but infinite box. min <= max holds on every
    // axis, so ordering passes it -- and center() is inf - inf == NaN, which would poison every
    // plane test downstream.
    CHECK_FALSE(Aabb{Vec3{-INF, 0.0F, 0.0F}, Vec3{INF, 1.0F, 1.0F}}.valid());
    CHECK_FALSE(Aabb{Vec3{0.0F, 0.0F, 0.0F}, Vec3{INF, 1.0F, 1.0F}}.valid());
}

TEST_CASE("render culling: center() and halfExtent() are the two halves of a box (FC2)") {
    const Aabb box{Vec3{-1.0F, -2.0F, -3.0F}, Vec3{3.0F, 2.0F, 1.0F}};
    CHECK(box.center() == Vec3{1.0F, 0.0F, -1.0F});
    CHECK(box.halfExtent() == Vec3{2.0F, 2.0F, 2.0F});

    const Aabb point{Vec3{4.0F, 5.0F, 6.0F}, Vec3{4.0F, 5.0F, 6.0F}};
    CHECK(point.center() == Vec3{4.0F, 5.0F, 6.0F});
    CHECK(point.halfExtent() == Vec3{0.0F, 0.0F, 0.0F});
}

TEST_CASE("render culling: toAabb copies a CookedBounds corner for corner (FC3)") {
    // Six MUTUALLY DISTINCT components: a transposed or aliased corner cannot land on the expected
    // value by coincidence.
    engine::assets::CookedBounds bounds;
    bounds.min = Vec3{-1.5F, -2.5F, -3.5F};
    bounds.max = Vec3{4.5F, 5.5F, 6.5F};

    const Aabb box = toAabb(bounds);
    CHECK(box.min == bounds.min);
    CHECK(box.max == bounds.max);
    CHECK(box.valid());
}

TEST_CASE("render culling: the [0,1] extraction matches the hand-derived planes, both ways (FC4)") {
    // (a) the hand-built column literals -- pinned to ADR-005's clip volume, never to the backend.
    checkAgainstLiterals(extractFrustum(referenceProjection()));
    // (b) the PRODUCTION input, engine::perspective, against the SAME literals. Comparing (a) to
    // (b) instead would compare two implementations of the same idea and witness nothing.
    checkAgainstLiterals(extractFrustum(backendProjection()));

    CHECK(extractFrustum(referenceProjection()).valid());
    CHECK(extractFrustum(backendProjection()).valid());
}

TEST_CASE("render culling: the NEAR plane is row 2 ALONE, at z = -zNear (FC5)") {
    // The whole [0,1]-vs-[-1,1] clip-volume decision lives in this one plane. GL's r3 + r2 form
    // would put it at d ~ -0.05 -- half the distance -- and cull geometry in front of the camera.
    const Frustum view = extractFrustum(backendProjection());
    const Plane& nearPlane = view.plane(FrustumPlane::Near);
    CHECK(engine::approxEquals(nearPlane.normal, Vec3{0.0F, 0.0F, -1.0F}, 1.0e-4F));
    CHECK(std::abs(nearPlane.d - (-0.1F)) <= 1.0e-4F);
    // Inside is -z - 0.1 >= 0, i.e. z <= -0.1 with -Z forward.
    CHECK(signedDistance(nearPlane, Vec3{0.0F, 0.0F, -1.0F}) > 0.0F);
    CHECK(signedDistance(nearPlane, Vec3{0.0F, 0.0F, 0.0F}) < 0.0F);
}

TEST_CASE("render culling: the FAR plane is r3 - r2, at z = -zFar (FC6)") {
    const Frustum view = extractFrustum(backendProjection());
    const Plane& farPlane = view.plane(FrustumPlane::Far);
    CHECK(engine::approxEquals(farPlane.normal, Vec3{0.0F, 0.0F, 1.0F}, 1.0e-4F));
    // RELATIVE: d is 0.1001001 / 0.001001 and the denominator is a catastrophic cancellation.
    CHECK(std::abs(farPlane.d - 100.0F) / 100.0F < 1.0e-5F);
    CHECK(signedDistance(farPlane, Vec3{0.0F, 0.0F, -50.0F}) > 0.0F);
    CHECK(signedDistance(farPlane, Vec3{0.0F, 0.0F, -150.0F}) < 0.0F);
}

TEST_CASE("render culling: visibility is scale-invariant, and the extraction normalises d too (FC7)") {
    const std::array<Aabb, 10> battery = visibilityBattery();
    const Frustum literal = literalFrustum();

    // (a) s and r scale by the SAME positive factor, so sign(s + r) is invariant. Each plane gets a
    // DISTINCT k, so a per-plane scale bug cannot cancel out.
    constexpr std::array<float, 6> SCALES{0.25F, 4.0F, 1.0F, 100.0F, 0.001F, 7.5F};
    Frustum scaled = literal;
    for (std::size_t i = 0; i < scaled.planes.size(); ++i) {
        scaled.planes[i].normal = scaled.planes[i].normal * SCALES[i];
        scaled.planes[i].d = scaled.planes[i].d * SCALES[i];
    }
    for (const Aabb& box : battery) {
        CHECK(isVisible(scaled, box) == isVisible(literal, box));
    }

    // (b) the extraction agrees with the literals on the whole battery -- INCLUDING battery[1], the
    // boundary box straddling the near plane by 5e-5. An extraction that normalises the normal but
    // not d leaves the near plane at d = -0.1001001, a 1-per-mille shift, and battery[1] flips from
    // inside to outside. Nothing coarser can see that.
    const Frustum extracted = extractFrustum(backendProjection());
    for (const Aabb& box : battery) {
        CHECK(isVisible(extracted, box) == isVisible(literal, box));
    }
    CHECK(isVisible(extracted, battery[1]));
}

TEST_CASE("render culling: a box fully inside is visible under both frustums (FC8)") {
    CHECK(isVisible(boxFrustum(), boxAt(Vec3{0.0F, 0.0F, 0.0F}, 0.5F)));
    CHECK(isVisible(boxFrustum(), boxAt(Vec3{0.5F, -0.25F, 0.1F}, 0.25F)));
    CHECK(isVisible(extractFrustum(backendProjection()), boxAt(Vec3{0.0F, 0.0F, -5.0F}, 0.5F)));
    CHECK(isVisible(extractFrustum(backendProjection()), boxAt(Vec3{1.0F, -0.5F, -10.0F}, 1.0F)));
}

TEST_CASE("render culling: each of the six planes rejects on its own side, and only its own (FC9)") {
    const Frustum box = boxFrustum();
    // A unit box (half-extent 0.5) pushed 3 units out along each axis, one plane at a time.
    struct Arm {
        FrustumPlane plane;
        Vec3 center;
    };
    const std::array<Arm, 6> boxArms{
        Arm{FrustumPlane::Left, Vec3{-3.0F, 0.0F, 0.0F}},   Arm{FrustumPlane::Right, Vec3{3.0F, 0.0F, 0.0F}},
        Arm{FrustumPlane::Bottom, Vec3{0.0F, -3.0F, 0.0F}}, Arm{FrustumPlane::Top, Vec3{0.0F, 3.0F, 0.0F}},
        Arm{FrustumPlane::Near, Vec3{0.0F, 0.0F, -3.0F}},   Arm{FrustumPlane::Far, Vec3{0.0F, 0.0F, 3.0F}}};
    for (const Arm& arm : boxArms) {
        CHECK_FALSE(isVisible(box, boxAt(arm.center, 0.5F)));
        CHECK(signedDistance(box.plane(arm.plane), arm.center) < 0.0F);
    }

    // The same, named plane by plane, on the REAL extracted frustum -- which is what makes this a
    // witness for a swapped left/right or bottom/top combination in the extraction. The box
    // frustum's own six planes are symmetric under those swaps and cannot see them.
    const Frustum view = extractFrustum(backendProjection());
    const std::array<Arm, 6> viewArms{
        Arm{FrustumPlane::Left, Vec3{-10.0F, 0.0F, -5.0F}},   Arm{FrustumPlane::Right, Vec3{10.0F, 0.0F, -5.0F}},
        Arm{FrustumPlane::Bottom, Vec3{0.0F, -10.0F, -5.0F}}, Arm{FrustumPlane::Top, Vec3{0.0F, 10.0F, -5.0F}},
        Arm{FrustumPlane::Near, Vec3{0.0F, 0.0F, -0.05F}},    Arm{FrustumPlane::Far, Vec3{0.0F, 0.0F, -200.0F}}};
    for (const Arm& arm : viewArms) {
        // EXACTLY ONE plane rejects each point: the named one. A swapped pair moves the negative
        // signed distance into the other slot, and the loop below reddens on both arms.
        CHECK(signedDistance(view.plane(arm.plane), arm.center) < 0.0F);
        int negatives = 0;
        for (const Plane& plane : view.planes) {
            if (signedDistance(plane, arm.center) < 0.0F) {
                ++negatives;
            }
        }
        CHECK(negatives == 1);
        CHECK_FALSE(isVisible(view, boxAt(arm.center, 0.01F)));
    }
}

TEST_CASE("render culling: a box straddling a plane is VISIBLE, not culled (FC10)") {
    const Frustum box = boxFrustum();
    // Centre ON the Left plane, half-extent 0.5: s == 0, r == 0.5, so s + r == 0.5 > 0. The `s - r`
    // form gives -0.5 and culls half the geometry at every screen edge.
    CHECK(isVisible(box, boxAt(Vec3{-1.0F, 0.0F, 0.0F}, 0.5F)));
    CHECK(isVisible(box, boxAt(Vec3{0.0F, 0.0F, -1.0F}, 0.5F)));  // straddling Near
    CHECK(isVisible(box, boxAt(Vec3{1.0F, 1.0F, 1.0F}, 0.5F)));   // straddling three at once

    const Frustum view = extractFrustum(backendProjection());
    CHECK(isVisible(view, boxAt(Vec3{-2.887F, 0.0F, -5.0F}, 1.0F)));  // straddling Left
    CHECK(isVisible(view, boxAt(Vec3{0.0F, 0.0F, -0.1F}, 0.5F)));     // straddling Near
}

TEST_CASE("render culling: a box CONTAINING the eye is visible (FC11)") {
    // No corner is inside the frustum and the centre is behind the near plane, yet the box overlaps
    // the volume. The r term is what makes this come out right.
    const Frustum view = extractFrustum(backendProjection());
    CHECK(isVisible(view, boxAt(Vec3{0.0F, 0.0F, 0.0F}, 1.0F)));
    CHECK(isVisible(view, boxAt(Vec3{0.0F, 0.0F, 0.0F}, 0.5F)));
}

TEST_CASE("render culling: the plane primitive's FLAT box is a first-class citizen (FC12)") {
    // The plane primitive is flat in Y since task 3.1.5 deleted LOCAL_MESH_HALF_EXTENT. A flat box
    // must survive the transform with he.y EXACTLY zero and still test correctly.
    const Aabb flat{Vec3{-0.5F, 0.0F, -0.5F}, Vec3{0.5F, 0.0F, 0.5F}};
    CHECK(flat.valid());
    CHECK(flat.halfExtent().y == 0.0F);

    const Frustum view = extractFrustum(backendProjection());
    const Aabb pushed = transformAabb(engine::translation(Vec3{0.0F, 0.0F, -5.0F}), flat);
    CHECK(pushed.halfExtent().y == 0.0F);
    CHECK(pushed.center() == Vec3{0.0F, 0.0F, -5.0F});
    CHECK(isVisible(view, pushed));

    const Aabb dropped = transformAabb(engine::translation(Vec3{0.0F, -50.0F, -5.0F}), flat);
    CHECK(dropped.halfExtent().y == 0.0F);
    CHECK_FALSE(isVisible(view, dropped));
}

TEST_CASE("render culling: exact tangency (s + r == 0) counts as VISIBLE (FC13)") {
    // Integer-valued float arithmetic on the box frustum, so this is exact rather than nearly-exact:
    // centre x = 2, half-extent 1, against Right = {(-1,0,0), 1} gives s = -1 and r = 1.
    const Frustum box = boxFrustum();
    const Aabb tangent = boxAt(Vec3{2.0F, 0.0F, 0.0F}, 1.0F);
    const Plane& right = box.plane(FrustumPlane::Right);
    const float s = signedDistance(right, tangent.center());
    const Vec3 e = tangent.halfExtent();
    const float r = std::abs(right.normal.x) * e.x + std::abs(right.normal.y) * e.y + std::abs(right.normal.z) * e.z;
    CHECK(s == -1.0F);
    CHECK(r == 1.0F);
    CHECK(s + r == 0.0F);
    CHECK(isVisible(box, tangent));  // `<= 0` here would blink every object out one frame early
}

TEST_CASE("render culling: an INVALID box is not visible under a valid frustum (FC14)") {
    const Frustum box = boxFrustum();
    // The cook's inverted sentinel: an empty submesh has nothing to draw.
    CHECK_FALSE(isVisible(box, Aabb{Vec3{INF, INF, INF}, Vec3{-INF, -INF, -INF}}));
    // The ordered-infinite box, whose centre is NaN.
    CHECK_FALSE(isVisible(box, Aabb{Vec3{-INF, 0.0F, 0.0F}, Vec3{INF, 1.0F, 1.0F}}));
    CHECK_FALSE(isVisible(box, Aabb{Vec3{NAN_F, 0.0F, 0.0F}, Vec3{1.0F, 1.0F, 1.0F}}));
    CHECK_FALSE(isVisible(box, Aabb{Vec3{1.0F, 0.0F, 0.0F}, Vec3{-1.0F, 1.0F, 1.0F}}));
}

TEST_CASE("render culling: an INVALID frustum draws EVERYTHING, and wins over an invalid box (FC15)") {
    // Both frustums below would cull every box if the guard were removed -- that is the point.
    // A zero-length normal with a NEGATIVE d gives s + r == -1 < 0 on every plane; the infinite-d
    // one gives -inf. An all-ZERO frustum would give s + r == 0 and pass anyway, so it proves
    // nothing and is deliberately not used here.
    Frustum zeroNormals{};
    zeroNormals.planes.fill(Plane{Vec3{0.0F, 0.0F, 0.0F}, -1.0F});
    CHECK_FALSE(zeroNormals.valid());

    Frustum infiniteD{};
    infiniteD.planes.fill(Plane{Vec3{1.0F, 0.0F, 0.0F}, -INF});
    CHECK_FALSE(infiniteD.valid());

    const Aabb ordinary = boxAt(Vec3{0.0F, 0.0F, -5.0F}, 0.5F);
    const Aabb faraway = boxAt(Vec3{1000.0F, 1000.0F, 1000.0F}, 0.5F);
    const Aabb sentinel{Vec3{INF, INF, INF}, Vec3{-INF, -INF, -INF}};

    for (const Frustum& degenerate : {zeroNormals, infiniteD}) {
        CHECK(isVisible(degenerate, ordinary));
        CHECK(isVisible(degenerate, faraway));
        // PRECEDENCE: the frustum is checked FIRST, so an invalid box is visible too. Culling to
        // black on a degenerate projection is the one failure mode this feature must never have.
        CHECK(isVisible(degenerate, sentinel));
    }
}

TEST_CASE("render culling: Frustum::valid() rejects every degenerate projection (FC16)") {
    CHECK_FALSE(extractFrustum(Mat4::zero()).valid());

    Mat4 nanColumn = referenceProjection();
    nanColumn.columns[1] = Vec4{0.0F, NAN_F, 0.0F, 0.0F};
    CHECK_FALSE(extractFrustum(nanColumn).valid());

    // zNear == zFar and aspect == 0, as COLUMN LITERALS. engine::perspective asserts both
    // preconditions (glm_backend.cpp), so calling it with these values aborts a Debug build rather
    // than producing the degenerate matrix -- these are exactly the matrices GLM's formulas yield.
    Mat4 zeroDepthRange = Mat4::zero();  // zFar/(zNear-zFar) = 0.1/0 = +inf; -(zFar*zNear)/0 = -inf
    zeroDepthRange.columns[0] = Vec4{REF_M00, 0.0F, 0.0F, 0.0F};
    zeroDepthRange.columns[1] = Vec4{0.0F, REF_M00, 0.0F, 0.0F};
    zeroDepthRange.columns[2] = Vec4{0.0F, 0.0F, INF, -1.0F};
    zeroDepthRange.columns[3] = Vec4{0.0F, 0.0F, -INF, 0.0F};
    CHECK_FALSE(extractFrustum(zeroDepthRange).valid());

    Mat4 zeroAspect = referenceProjection();  // 1/(aspect * tanHalfFovY) = 1/0 = +inf
    zeroAspect.columns[0] = Vec4{INF, 0.0F, 0.0F, 0.0F};
    CHECK_FALSE(extractFrustum(zeroAspect).valid());

    // Finite normals with an INFINITE d -- reachable from an infinite translation column. Every
    // s = dot(n, c) + d is then +-inf or NaN and `s + r < 0` is false for NaN, so without the d
    // half of the check the draw would degrade to all-visible SILENTLY, skipping the WARN.
    Frustum finiteNormalsInfiniteD = literalFrustum();
    finiteNormalsInfiniteD.planes[0].d = INF;
    CHECK_FALSE(finiteNormalsInfiniteD.valid());
    finiteNormalsInfiniteD.planes[0].d = NAN_F;
    CHECK_FALSE(finiteNormalsInfiniteD.valid());

    // ...and a normal shorter than EPSILON is rejected on its own.
    Frustum shortNormal = literalFrustum();
    shortNormal.planes[3].normal = Vec3{0.0F, 1.0e-9F, 0.0F};
    CHECK_FALSE(shortNormal.valid());

    CHECK(literalFrustum().valid());
    CHECK(extractFrustum(referenceProjection()).valid());
}

TEST_CASE("render culling: transformAabb under identity is the identity (FC17)") {
    const Aabb box{Vec3{-1.0F, -2.0F, -3.0F}, Vec3{3.0F, 2.0F, 1.0F}};
    const Aabb out = transformAabb(Mat4::identity(), box);
    CHECK(out.min == box.min);
    CHECK(out.max == box.max);
}

TEST_CASE("render culling: translation shifts both corners exactly (FC18)") {
    // The centre must go through transformPoint (w = 1). transformDirection (w = 0) drops the
    // translation entirely and every instance culls as though it sat at the origin.
    const Aabb box{Vec3{-1.0F, -2.0F, -3.0F}, Vec3{3.0F, 2.0F, 1.0F}};
    const Vec3 t{10.0F, -20.0F, 30.0F};
    const Aabb out = transformAabb(engine::translation(t), box);
    CHECK(out.min == box.min + t);
    CHECK(out.max == box.max + t);
    CHECK(out.halfExtent() == box.halfExtent());
}

TEST_CASE("render culling: scale multiplies the half-extent per axis (FC19)") {
    const Aabb box{Vec3{-1.0F, -1.0F, -1.0F}, Vec3{1.0F, 1.0F, 1.0F}};

    const Aabb uniform = transformAabb(engine::scaling(Vec3{2.0F, 2.0F, 2.0F}), box);
    CHECK(uniform.halfExtent() == Vec3{2.0F, 2.0F, 2.0F});
    CHECK(uniform.center() == Vec3{0.0F, 0.0F, 0.0F});

    const Aabb nonUniform = transformAabb(engine::scaling(Vec3{2.0F, 3.0F, 4.0F}), box);
    CHECK(nonUniform.halfExtent() == Vec3{2.0F, 3.0F, 4.0F});

    // Scale applies to the CENTRE too, not just the extent.
    const Aabb offCentre{Vec3{1.0F, 1.0F, 1.0F}, Vec3{3.0F, 3.0F, 3.0F}};
    const Aabb scaled = transformAabb(engine::scaling(Vec3{2.0F, 2.0F, 2.0F}), offCentre);
    CHECK(scaled.center() == Vec3{4.0F, 4.0F, 4.0F});
    CHECK(scaled.halfExtent() == Vec3{2.0F, 2.0F, 2.0F});
}

TEST_CASE("render culling: rotation grows the box CONSERVATIVELY (FC20)") {
    // 45 degrees about Y on the unit cube: the AABB around the rotated OBB has half-extent
    // (sqrt(2), 1, sqrt(2)) -- bigger than the original, which is the whole point of a conservative
    // test. Transforming the two CORNERS instead of the eight leaves z at 0 and culls the object
    // from the side.
    const Aabb box{Vec3{-1.0F, -1.0F, -1.0F}, Vec3{1.0F, 1.0F, 1.0F}};
    const Mat4 rotate =
        engine::compose({.translation = Vec3{},
                         .rotation = engine::fromAxisAngle(Vec3{0.0F, 1.0F, 0.0F}, engine::radians(45.0F)),
                         .scale = Vec3::one()});
    const Aabb out = transformAabb(rotate, box);
    constexpr float ROOT2 = std::numbers::sqrt2_v<float>;
    CHECK(engine::approxEquals(out.halfExtent(), Vec3{ROOT2, 1.0F, ROOT2}, 1.0e-5F));
    CHECK(engine::approxEquals(out.center(), Vec3{0.0F, 0.0F, 0.0F}, 1.0e-5F));
    CHECK(out.valid());
}

TEST_CASE("render culling: a MIRROR transforms correctly, not inside out (FC21)") {
    // The absolute value in the Arvo form is what makes this work. Without it the half-extent goes
    // negative, min ends up above max, and the box is INVALID -- which isVisible then reads as
    // "nothing to draw", so a mirrored instance vanishes while it is on screen.
    const Aabb box{Vec3{-1.0F, -2.0F, -3.0F}, Vec3{3.0F, 2.0F, 1.0F}};
    const Aabb mirrored = transformAabb(engine::scaling(Vec3{-1.0F, 1.0F, 1.0F}), box);

    CHECK(mirrored.valid());
    // Exactly the reflection of the original corners, re-ordered into min/max.
    CHECK(mirrored.min == Vec3{-3.0F, -2.0F, -3.0F});
    CHECK(mirrored.max == Vec3{1.0F, 2.0F, 1.0F});
    // ...and the same size as the unmirrored twin.
    const Aabb twin = transformAabb(engine::scaling(Vec3{1.0F, 1.0F, 1.0F}), box);
    CHECK(mirrored.halfExtent() == twin.halfExtent());

    // All three axes mirrored at once, and a mirror combined with a translation.
    const Aabb flipped = transformAabb(engine::scaling(Vec3{-1.0F, -1.0F, -1.0F}), box);
    CHECK(flipped.valid());
    CHECK(flipped.halfExtent() == twin.halfExtent());
    CHECK(flipped.center() == Vec3{-1.0F, 0.0F, 1.0F});
}

TEST_CASE("render culling: a SHEAR grows the box along the sheared axis (FC22)") {
    // x' = x + s*y, written as column 1's x component. he'.x = |1|*e.x + |s|*e.y; the other two
    // axes are untouched.
    constexpr float SHEAR = 3.0F;
    Mat4 shear = Mat4::identity();
    shear.columns[1] = Vec4{SHEAR, 1.0F, 0.0F, 0.0F};

    const Aabb box{Vec3{-1.0F, -2.0F, -4.0F}, Vec3{1.0F, 2.0F, 4.0F}};  // e = (1, 2, 4)
    const Aabb out = transformAabb(shear, box);
    CHECK(out.halfExtent() == Vec3{1.0F + (SHEAR * 2.0F), 2.0F, 4.0F});
    CHECK(out.center() == Vec3{0.0F, 0.0F, 0.0F});
    CHECK(out.valid());
}

TEST_CASE("render culling: an invalid box transforms to an invalid box (FC23)") {
    // The predicate PROPAGATES -- transformAabb never fabricates corners from a NaN centre.
    const Aabb sentinel{Vec3{INF, INF, INF}, Vec3{-INF, -INF, -INF}};
    const Aabb ordered{Vec3{-INF, 0.0F, 0.0F}, Vec3{INF, 1.0F, 1.0F}};
    const Aabb nanCorner{Vec3{NAN_F, 0.0F, 0.0F}, Vec3{1.0F, 1.0F, 1.0F}};

    for (const Aabb& bad : {sentinel, ordered, nanCorner}) {
        CHECK_FALSE(transformAabb(Mat4::identity(), bad).valid());
        CHECK_FALSE(transformAabb(engine::translation(Vec3{1.0F, 2.0F, 3.0F}), bad).valid());
        CHECK_FALSE(transformAabb(engine::scaling(Vec3{2.0F, 2.0F, 2.0F}), bad).valid());
    }
}

TEST_CASE("render culling: signedDistance's sign convention, and the plane SLOT order (FC24)") {
    const Plane plane{Vec3{0.0F, 1.0F, 0.0F}, -2.0F};  // inside <=> y >= 2
    CHECK(signedDistance(plane, Vec3{0.0F, 5.0F, 0.0F}) == 3.0F);
    CHECK(signedDistance(plane, Vec3{0.0F, 2.0F, 0.0F}) == 0.0F);    // ON the plane
    CHECK(signedDistance(plane, Vec3{0.0F, -1.0F, 0.0F}) == -3.0F);  // outside
    CHECK(signedDistance(plane, Vec3{9.0F, 5.0F, -9.0F}) == 3.0F);   // the other axes do not matter

    // Six DISTINGUISHABLE planes, so plane(which) returning the wrong slot cannot land on the right
    // answer. Tests name planes; they never count.
    Frustum f{};
    for (std::size_t i = 0; i < f.planes.size(); ++i) {
        f.planes[i] = Plane{Vec3{0.0F, 0.0F, 1.0F}, static_cast<float>(i) + 1.0F};
    }
    CHECK(f.plane(FrustumPlane::Left).d == 1.0F);
    CHECK(f.plane(FrustumPlane::Right).d == 2.0F);
    CHECK(f.plane(FrustumPlane::Bottom).d == 3.0F);
    CHECK(f.plane(FrustumPlane::Top).d == 4.0F);
    CHECK(f.plane(FrustumPlane::Near).d == 5.0F);
    CHECK(f.plane(FrustumPlane::Far).d == 6.0F);
    CHECK(f.planes.size() == 6);
}

// ================================================================================================
// Tier 1 -- a real Device, no window. The cull inside ForwardRenderer::draw.
//
// Gated on AERO_SHADER_TOOLS_ENABLED for the reason render_skinning_test.cpp's own tier-1 block is:
// a ForwardRenderer loads its shaders from build/<preset>/shaders, which only exists when the
// shader toolchain is built. The FC battery above runs in every configuration.
//
// EVERY fixture position below was MEASURED against the real extraction rather than derived by
// hand, and each case names the worst-plane slack it sits on, so a reader can see it is not a
// coincidence one lane could lose.
// ================================================================================================

#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/assets/mesh_cook.hpp>
    #include <aero/core/vfs.hpp>
    #include <aero/platform/platform.hpp>
    #include <aero/render/render.hpp>
    #include <aero/rhi/rhi.hpp>

    #include "rhi_test_support.hpp"

    #include <cstdint>
    #include <memory>
    #include <optional>
    #include <span>
    #include <utility>
    #include <vector>

using engine::render::MeshInstance;
using engine::render::PrimitiveId;

namespace {

// A ForwardRenderer needs a colour and a depth format, which a RenderTarget supplies without a
// window (the render_material_test.cpp / render_skinning_test.cpp pattern).
[[nodiscard]] std::optional<engine::render::ForwardRenderer> makeForwardRenderer(
    engine::rhi::Device& device, const engine::render::RenderTarget& target) {
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    return engine::render::ForwardRenderer::create(
        device, vfs, {.colorFormat = target.colorFormat(), .depthFormat = target.depthFormat()});
}

// The same camera render_skinning_test.cpp's SN battery uses, with eyePosition set to the SAME point
// the view matrix was built from.
[[nodiscard]] engine::render::CameraView makeCamera() {
    const Vec3 eye{0.0F, 1.5F, 3.0F};
    return {engine::lookAt(eye, Vec3{}, Vec3{0.0F, 1.0F, 0.0F}),
            engine::perspective(engine::radians(60.0F), 1.0F, 0.1F, 100.0F), eye};
}

// One built-in primitive instance with model/normalMatrix/mvp set CONSISTENTLY -- mvp == viewProj *
// model, which is the contract RenderView::cullingEnabled documents and the whole reason the cull
// may read `model` while the shader reads `mvp`.
[[nodiscard]] MeshInstance primitiveInstance(PrimitiveId primitive, const Mat4& model, const Mat4& viewProj) {
    MeshInstance instance;
    instance.primitive = primitive;
    instance.model = model;
    instance.normalMatrix = Mat4::identity();
    instance.mvp = viewProj * model;
    return instance;
}

// Behind the eye for makeCamera(): worst-plane slack -40.8, so nothing rounds it back on screen.
constexpr Mat4 OFFSCREEN = engine::translation(Vec3{0.0F, 0.0F, 50.0F});
// On screen, far down the view axis: slack +5.8. Its role is spelled out in CD1.
constexpr Mat4 DISTANT_ON_SCREEN = engine::translation(Vec3{0.0F, 0.0F, -60.0F});
// A mirror about X. The cube's box is symmetric, so the WORLD box is unchanged -- what this fixture
// asserts is that transformAabb's abs keeps it VALID rather than inside out.
constexpr Mat4 MIRROR_X = engine::scaling(Vec3{-1.0F, 1.0F, 1.0F});

// The seven-instance view CD1/CD4/CD5 share.
[[nodiscard]] std::vector<MeshInstance> cullMixView(const Mat4& viewProj) {
    return {primitiveInstance(PrimitiveId::Cube, Mat4::identity(), viewProj),
            primitiveInstance(PrimitiveId::Sphere, Mat4::identity(), viewProj),
            primitiveInstance(PrimitiveId::Plane, Mat4::identity(), viewProj),
            primitiveInstance(PrimitiveId::Cube, MIRROR_X, viewProj),
            primitiveInstance(PrimitiveId::Cube, DISTANT_ON_SCREEN, viewProj),
            primitiveInstance(PrimitiveId::Cube, OFFSCREEN, viewProj),
            primitiveInstance(PrimitiveId::Sphere, OFFSCREEN, viewProj)};
}

// A triangle whose box is (-0.5,-0.5,0)..(0.5,0.5,0): on screen at the origin with slack +1.95, and
// culled at OFFSCREEN with slack -41.2.
constexpr std::array<Vec3, 3> TRI_AT_ORIGIN{Vec3{-0.5F, -0.5F, 0.0F}, Vec3{0.5F, -0.5F, 0.0F}, Vec3{0.0F, 0.5F, 0.0F}};
// The same triangle pushed to x ~ 100: off screen on its OWN box (slack -84.4), while the MODEL box
// spanning both triangles is on screen (+1.95). That gap is what CD8 turns into an assertion.
constexpr std::array<Vec3, 3> TRI_FAR_OUT{Vec3{99.5F, -0.5F, 0.0F}, Vec3{100.5F, -0.5F, 0.0F},
                                          Vec3{100.0F, 0.5F, 0.0F}};
constexpr std::array<std::uint32_t, 3> TRI_INDICES{0, 1, 2};

[[nodiscard]] engine::assets::MeshCookPrimitive triangleAt(std::span<const Vec3> positions,
                                                           std::uint32_t primitiveIndex) {
    engine::assets::MeshCookPrimitive primitive;
    primitive.sourceMeshIndex = 0;
    primitive.sourcePrimitiveIndex = primitiveIndex;
    primitive.positions = positions;
    primitive.indices = TRI_INDICES;
    return primitive;
}

// Cooked IN MEMORY: 3.3.3 makes the cook deterministic cross-lane, so this is as stable as a golden
// and commits no fixture. The returned bytes must OUTLIVE every CookedMesh parsed from them.
[[nodiscard]] std::vector<std::byte> cookTriangles(std::span<const engine::assets::MeshCookPrimitive> primitives) {
    engine::assets::MeshCookResult result = engine::assets::cookMesh({.sourceGuid = {}, .primitives = primitives});
    REQUIRE(result.status == engine::assets::MeshCookStatus::Ok);
    REQUIRE_FALSE(result.bytes.empty());
    return std::move(result.bytes);
}

// A registered-mesh instance, with mvp consistent with model.
[[nodiscard]] MeshInstance meshInstance(engine::render::MeshHandle mesh, std::uint32_t submesh, const Mat4& model,
                                        const Mat4& viewProj) {
    MeshInstance instance;
    instance.mesh = mesh;
    instance.submesh = submesh;
    instance.model = model;
    instance.normalMatrix = Mat4::identity();
    instance.mvp = viewProj * model;
    return instance;
}

}  // namespace

    // The tier-1 preamble, written out per case exactly as render_skinning_test.cpp does it:
    // AERO_SKIP_OR_FAIL returns from the enclosing function, so it cannot live in a helper.
    #define AERO_CULL_TIER1_PREAMBLE()                                         \
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
        auto forward = makeForwardRenderer(*device, *target);                  \
        REQUIRE(forward.has_value())

namespace {

// Records one view into one frame. Every CD case draws through this, so "the counters are read
// AFTER endFrame" is stated once rather than seven times.
void drawOnce(engine::render::RenderTarget& target, engine::render::ForwardRenderer& forward,
              const engine::render::RenderView& view) {
    std::optional<engine::render::Frame> open = target.beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(open.has_value());
    forward.draw(*open, view);
    CHECK(target.endFrame(std::move(*open)));
}

}  // namespace

TEST_CASE("render culling: off-screen instances are counted culled, on-screen ones drawn (CD1)") {
    AERO_CULL_TIER1_PREAMBLE();

    const engine::render::CameraView camera = makeCamera();
    const Mat4 viewProj = camera.proj * camera.view;
    const std::vector<MeshInstance> instances = cullMixView(viewProj);

    engine::render::RenderView view;
    view.camera = camera;
    view.directional = {.direction = Vec3{-0.5F, -1.0F, -0.3F}, .color = Vec3::one(), .intensity = 2.0F};
    view.instances = instances;

    drawOnce(*target, *forward, view);
    // Five on screen (three at the origin, the mirrored cube, the distant one), two behind the eye.
    CHECK(forward->lastFrameDrawn() == 5);
    CHECK(forward->lastFrameCulled() == 2);
    CHECK(forward->lastFrameDrawn() + forward->lastFrameCulled() == instances.size());

    // PER-FRAME, not cumulative: an identical second draw reads the same numbers, not double.
    drawOnce(*target, *forward, view);
    CHECK(forward->lastFrameDrawn() == 5);
    CHECK(forward->lastFrameCulled() == 2);

    // THE MIRRORED INSTANCE is a correctness assertion, not a placement one: with the abs dropped
    // from transformAabb its world box goes inside out, Aabb::valid() rejects it and the cull reads
    // that as "nothing to draw", so an on-screen mirrored object disappears. It is NOT a witness for
    // extracting the frustum per instance -- measured: extraction from a negative-determinant mvp
    // yields the SAME half-spaces in that instance's own space, so the local-space form agrees with
    // this one on every mirrored fixture. DISTANT_ON_SCREEN is the witness for that: extracting from
    // mvp while still testing the WORLD box applies the model transform twice, which pushes a cube
    // 60 units out to 120 and past the far plane (slack +5.8 correct, -10.0 seeded).
}

TEST_CASE("render culling: an instance carrying a PALETTE is exempt from the cull (CD2)") {
    AERO_CULL_TIER1_PREAMBLE();

    const engine::render::CameraView camera = makeCamera();
    const Mat4 viewProj = camera.proj * camera.view;
    // A one-entry identity palette. ARM 1 (the built-in primitive path) ignores a palette entirely,
    // so this witnesses the `!palette.empty()` PREDICATE and nothing about skinning: an instance
    // whose vertices may move under a palette this renderer never sees has no bind-pose box that
    // bounds where it ends up, so it is never culled.
    const std::array<Mat4, 1> palette{Mat4::identity()};

    MeshInstance exempt = primitiveInstance(PrimitiveId::Cube, OFFSCREEN, viewProj);
    exempt.palette = palette;
    const std::array<MeshInstance, 2> instances{exempt, primitiveInstance(PrimitiveId::Cube, OFFSCREEN, viewProj)};

    engine::render::RenderView view;
    view.camera = camera;
    view.instances = instances;
    drawOnce(*target, *forward, view);

    CHECK(forward->lastFrameDrawn() == 1);    // the exempt one, off screen and drawn anyway
    CHECK(forward->lastFrameCulled() == 1);   // its palette-free twin, same position
    CHECK(forward->skinnedDrawCount() == 0);  // no registered mesh, so no skinned ARM was taken
}

TEST_CASE("render culling: drawn + culled need NOT sum -- a skipped instance is in neither (CD3)") {
    AERO_CULL_TIER1_PREAMBLE();

    const std::vector<std::byte> bytes = cookTriangles(std::array{triangleAt(TRI_AT_ORIGIN, 0)});
    const engine::assets::CookedMeshParseResult parse = engine::assets::parseCookedMesh(bytes);
    REQUIRE(parse.status == engine::assets::CookedMeshStatus::Ok);
    const engine::render::MeshHandle live = forward->createMesh(parse.mesh);
    REQUIRE(live.valid());
    const engine::render::MeshHandle stale = forward->createMesh(parse.mesh);
    REQUIRE(stale.valid());
    forward->destroyMesh(stale);

    const engine::render::CameraView camera = makeCamera();
    const Mat4 viewProj = camera.proj * camera.view;
    const std::array<MeshInstance, 3> instances{
        meshInstance(live, 0, Mat4::identity(), viewProj),  // on screen, slack +1.95
        meshInstance(live, 0, OFFSCREEN, viewProj),         // off screen, slack -41.2
        meshInstance(stale, 0, OFFSCREEN, viewProj)};       // resolves to NOTHING -- neither bucket

    engine::render::RenderView view;
    view.camera = camera;
    view.instances = instances;
    drawOnce(*target, *forward, view);

    CHECK(forward->lastFrameDrawn() == 1);
    CHECK(forward->lastFrameCulled() == 1);
    // THE DOCUMENTED NON-SUM. A stale handle cannot be culled, because nothing is known about where
    // it is: returning a default Aabb{} instead of "cannot prove anything" would put a point box at
    // the instance's origin and cull it (slack -41.5 here), silently consuming the stale-handle WARN
    // the arm below owes. Counting at the cull's keep-decision instead of at drawIndexed would count
    // it as DRAWN. Both wrong answers are 2, and this is the assertion that says so.
    CHECK(forward->lastFrameDrawn() + forward->lastFrameCulled() == 2);
    CHECK(instances.size() == 3);
}

TEST_CASE("render culling: cullingEnabled = false draws everything and culls nothing (CD4)") {
    AERO_CULL_TIER1_PREAMBLE();

    const engine::render::CameraView camera = makeCamera();
    const std::vector<MeshInstance> instances = cullMixView(camera.proj * camera.view);

    engine::render::RenderView view;
    view.camera = camera;
    view.instances = instances;
    view.cullingEnabled = false;
    drawOnce(*target, *forward, view);

    CHECK(forward->lastFrameDrawn() == 7);
    CHECK(forward->lastFrameCulled() == 0);
    CHECK(forward->lastFrameDrawn() == instances.size());
}

TEST_CASE("render culling: a no-camera frame resets both counters to zero (CD5)") {
    AERO_CULL_TIER1_PREAMBLE();

    const engine::render::CameraView camera = makeCamera();
    const std::vector<MeshInstance> instances = cullMixView(camera.proj * camera.view);

    engine::render::RenderView populated;
    populated.camera = camera;
    populated.instances = instances;
    drawOnce(*target, *forward, populated);
    REQUIRE(forward->lastFrameDrawn() == 5);
    REQUIRE(forward->lastFrameCulled() == 2);

    // ONE sequence, not two independent cases: the reset has to happen BEFORE the !hasCamera early
    // return, and only a draw that follows a populated one can tell the difference.
    engine::render::RenderView blank;
    blank.hasCamera = false;
    blank.instances = instances;
    drawOnce(*target, *forward, blank);
    CHECK(forward->lastFrameDrawn() == 0);
    CHECK(forward->lastFrameCulled() == 0);
}

TEST_CASE("render culling: a culled instance pushes no material block (CD6)") {
    AERO_CULL_TIER1_PREAMBLE();

    const engine::render::MaterialHandle opaque =
        forward->createMaterial({.baseColorFactor = engine::Vec4{1.0F, 1.0F, 1.0F, 1.0F}, .doubleSided = false}, {});
    const engine::render::MaterialHandle twoSided =
        forward->createMaterial({.baseColorFactor = engine::Vec4{1.0F, 0.0F, 0.0F, 1.0F}, .doubleSided = true}, {});
    REQUIRE(opaque.valid());
    REQUIRE(twoSided.valid());

    const engine::render::CameraView camera = makeCamera();
    const Mat4 viewProj = camera.proj * camera.view;
    // Eight instances alternating material, with EVERY two-sided one off screen: with the cull ahead
    // of material resolution the loop sees four identical on-screen instances in a row.
    std::vector<MeshInstance> instances;
    for (int i = 0; i < 8; ++i) {
        const bool offScreen = (i % 2) == 1;
        MeshInstance instance =
            primitiveInstance(PrimitiveId::Cube, offScreen ? OFFSCREEN : Mat4::identity(), viewProj);
        instance.material = offScreen ? twoSided : opaque;
        instances.push_back(instance);
    }

    engine::render::RenderView view;
    view.camera = camera;
    view.instances = instances;

    const std::size_t materialBefore = forward->materialBindCount();
    const std::size_t pipelineBefore = forward->pipelineBindCount();
    drawOnce(*target, *forward, view);
    const std::size_t materialWithCulling = forward->materialBindCount() - materialBefore;
    const std::size_t pipelineWithCulling = forward->pipelineBindCount() - pipelineBefore;

    view.cullingEnabled = false;
    const std::size_t materialMid = forward->materialBindCount();
    const std::size_t pipelineMid = forward->pipelineBindCount();
    drawOnce(*target, *forward, view);
    const std::size_t materialWithout = forward->materialBindCount() - materialMid;
    const std::size_t pipelineWithout = forward->pipelineBindCount() - pipelineMid;

    // THE PLACEMENT ASSERTION. Pipeline binds happen INSIDE the draw arms, downstream of both
    // candidate cull placements, so they cannot see whether the cull sits before or after material
    // resolution -- the material-block count is the only observable that can, which is the whole
    // reason materialBindCount() exists.
    CHECK(materialWithCulling == 1);
    CHECK(materialWithout == 8);
    CHECK(pipelineWithCulling == 0);
    CHECK(pipelineWithout == 7);
    // ...and the spec's own inequality, which is true and worth keeping: culling really does reduce
    // rebinds for an alternating view, because culling-off draws the alternating instances.
    CHECK(pipelineWithCulling < pipelineWithout);
    CHECK(materialWithCulling < materialWithout);
}

TEST_CASE("render culling: the primitive boxes are FOLDED, so the flat plane culls where a cube does not (CD7)") {
    AERO_CULL_TIER1_PREAMBLE();

    const engine::render::CameraView camera = makeCamera();
    const Mat4 viewProj = camera.proj * camera.view;
    // MEASURED: at (0, 2.25, -5) the plane's flat box misses the Top plane by 0.239 while the cube's
    // half-extent of 0.5 in Y clears it by 0.260. A hard-coded [-0.5, 0.5]^3 table for all three
    // primitives -- or a fold that reads the wrong geometry for the plane slot -- makes the plane
    // visible here, and nothing else in this suite can tell the difference.
    const Mat4 justAboveTop = engine::translation(Vec3{0.0F, 2.25F, -5.0F});
    const std::array<MeshInstance, 1> planeOnly{primitiveInstance(PrimitiveId::Plane, justAboveTop, viewProj)};

    engine::render::RenderView view;
    view.camera = camera;
    view.instances = planeOnly;
    drawOnce(*target, *forward, view);
    CHECK(forward->lastFrameDrawn() == 0);
    CHECK(forward->lastFrameCulled() == 1);

    const std::array<MeshInstance, 1> cubeOnly{primitiveInstance(PrimitiveId::Cube, justAboveTop, viewProj)};
    view.instances = cubeOnly;
    drawOnce(*target, *forward, view);
    CHECK(forward->lastFrameDrawn() == 1);
    CHECK(forward->lastFrameCulled() == 0);
}

TEST_CASE("render culling: each submesh culls on its OWN box, never the model's (CD8)") {
    AERO_CULL_TIER1_PREAMBLE();

    // Two primitives with the same attribute mask cook into ONE section with TWO submeshes. A is at
    // the origin (slack +1.95), B is at x ~ 100 (slack -84.4), and the MODEL box spanning both is on
    // screen (+1.95) -- so copying the model box into every submesh makes B undulling forever.
    const std::array<engine::assets::MeshCookPrimitive, 2> primitives{triangleAt(TRI_AT_ORIGIN, 0),
                                                                      triangleAt(TRI_FAR_OUT, 1)};
    const std::vector<std::byte> bytes = cookTriangles(primitives);
    const engine::assets::CookedMeshParseResult parse = engine::assets::parseCookedMesh(bytes);
    REQUIRE(parse.status == engine::assets::CookedMeshStatus::Ok);
    REQUIRE(parse.mesh.submeshes.size() == 2);
    const engine::render::MeshHandle mesh = forward->createMesh(parse.mesh);
    REQUIRE(mesh.valid());
    REQUIRE(forward->meshSubmeshCount(mesh) == 2);

    const engine::render::CameraView camera = makeCamera();
    const Mat4 viewProj = camera.proj * camera.view;

    engine::render::RenderView view;
    view.camera = camera;
    const std::array<MeshInstance, 1> farSubmesh{meshInstance(mesh, 1, Mat4::identity(), viewProj)};
    view.instances = farSubmesh;
    drawOnce(*target, *forward, view);
    CHECK(forward->lastFrameDrawn() == 0);
    CHECK(forward->lastFrameCulled() == 1);

    const std::array<MeshInstance, 1> nearSubmesh{meshInstance(mesh, 0, Mat4::identity(), viewProj)};
    view.instances = nearSubmesh;
    drawOnce(*target, *forward, view);
    CHECK(forward->lastFrameDrawn() == 1);
    CHECK(forward->lastFrameCulled() == 0);
}

TEST_CASE("render culling: a submesh carrying the empty sentinel is culled (CD9)") {
    AERO_CULL_TIER1_PREAMBLE();

    const std::vector<std::byte> bytes = cookTriangles(std::array{triangleAt(TRI_AT_ORIGIN, 0)});
    engine::assets::CookedMeshParseResult parse = engine::assets::parseCookedMesh(bytes);
    REQUIRE(parse.status == engine::assets::CookedMeshStatus::Ok);
    REQUIRE(parse.mesh.submeshes.size() == 1);
    // PRODUCTION-UNREACHABLE THROUGH THE COOK, TEST-REACHABLE BY CONSTRUCTION -- and it must never be
    // read as live cover for the cook. mesh_cook.cpp drops a primitive with no positions whole, and a
    // file with no submeshes stores a POINT BOX at the origin rather than this sentinel; what can
    // carry it is a hand-built or corrupt .aeromesh, which parseCookedMesh does not validate bounds
    // for. So the box is edited in memory, exactly as such a file would present it.
    parse.mesh.submeshes[0].bounds.min = Vec3{INF, INF, INF};
    parse.mesh.submeshes[0].bounds.max = Vec3{-INF, -INF, -INF};

    const engine::render::MeshHandle mesh = forward->createMesh(parse.mesh);
    REQUIRE(mesh.valid());

    const engine::render::CameraView camera = makeCamera();
    const std::array<MeshInstance, 1> instances{meshInstance(mesh, 0, Mat4::identity(), camera.proj * camera.view)};

    engine::render::RenderView view;
    view.camera = camera;
    view.instances = instances;
    drawOnce(*target, *forward, view);

    CHECK(forward->lastFrameDrawn() == 0);
    CHECK(forward->lastFrameCulled() == 1);
}

TEST_CASE("render culling: a degenerate projection draws EVERYTHING and warns once (CD10)") {
    AERO_CULL_TIER1_PREAMBLE();

    CHECK_FALSE(forward->hasWarnedDegenerateFrustum());

    engine::render::CameraView camera = makeCamera();
    camera.proj = Mat4::zero();  // every extracted plane collapses; Frustum::valid() is false
    const std::vector<MeshInstance> instances = cullMixView(camera.proj * camera.view);

    engine::render::RenderView view;
    view.camera = camera;
    view.instances = instances;
    drawOnce(*target, *forward, view);

    // ALL of them, including the two that would be culled under a usable frustum. Culling to black
    // on a degenerate projection is the one failure mode this feature must never have.
    CHECK(forward->lastFrameDrawn() == 7);
    CHECK(forward->lastFrameCulled() == 0);
    CHECK(forward->hasWarnedDegenerateFrustum());

    // LATCHED: the second degenerate draw behaves identically and does not un-latch.
    drawOnce(*target, *forward, view);
    CHECK(forward->lastFrameDrawn() == 7);
    CHECK(forward->lastFrameCulled() == 0);
    CHECK(forward->hasWarnedDegenerateFrustum());
}

    #undef AERO_CULL_TIER1_PREAMBLE

#endif  // AERO_SHADER_TOOLS_ENABLED

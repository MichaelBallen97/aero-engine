// Aero Engine — math surface tests (task 0.2.2, ADR-005).
//
// D18: these tests deliberately do NOT include GLM. /tests sits outside core/math, so including
// GLM here would be the exact violation .github/scripts/check-math-boundary.sh (task 0.2.3) exists
// to catch — and cross-validating our math against the very backend that implements it would be
// partly circular. Every expectation below is a hand-computed literal or an algebraic identity.
//
// AC-9(i): aero_tests links aero::core but NOT glm::glm — that omission is intentional style, but
// it is NOT a compile-time guarantee that no public math header pulls GLM in. aero_tests also
// links doctest::doctest, which (via vcpkg's shared per-triplet include/ directory) exports GLM's
// headers to this TU regardless of what aero_core links; a probe TU with aero_tests' exact flags
// plus `#include <glm/vec3.hpp>` compiles clean here. The real compile-time boundary holds only
// for engine-layer targets that link no vcpkg package directly (verified with a throwaway probe
// linking only aero::core — see docs/02-adrs.md's ADR-005 implementation note and risk R12 in
// docs/08-risks.md). Inside tests/, .github/scripts/check-math-boundary.sh (task 0.2.3, run by
// the `lint` job) is the enforcement.
#include <aero/core/math.hpp>

#include <doctest/doctest.h>

#include <limits>
#include <type_traits>

// docs/04 forbids `using namespace` in HEADERS; this is a test TU. Free functions live in
// engine:: too (D12), so ADL would find dot(a, b) unqualified regardless.
using namespace engine;

namespace {
// EPSILON is tuned for a handful of accumulated float32 ops. Matrix inversion, quat_cast and the
// projections accumulate more, so they get an explicit looser tolerance (E11).
constexpr float MATRIX_EPSILON = 1e-4f;
}  // namespace

TEST_CASE("math: layout and trait invariants (AC-11)") {
    // Mirrors the headers' static_asserts as executable documentation of WHY they matter: data()
    // (the GPU upload path, D8) and ADR-004 serialization both rest on them.
    static_assert(std::is_trivially_copyable_v<Vec2>);
    static_assert(std::is_trivially_copyable_v<Vec3>);
    static_assert(std::is_trivially_copyable_v<Vec4>);
    static_assert(std::is_trivially_copyable_v<Mat3>);
    static_assert(std::is_trivially_copyable_v<Mat4>);
    static_assert(std::is_trivially_copyable_v<Quat>);

    static_assert(std::is_standard_layout_v<Mat3>);
    static_assert(std::is_standard_layout_v<Mat4>);

    static_assert(sizeof(Vec2) == 8);
    static_assert(sizeof(Vec3) == 12);
    static_assert(sizeof(Vec4) == 16);
    static_assert(sizeof(Mat3) == 36);
    static_assert(sizeof(Mat4) == 64);
    static_assert(sizeof(Quat) == 16);

    // D14 keeps the types AGGREGATES (no user-declared ctors) so designated initializers and
    // is_trivially_copyable both survive — the latter is what ADR-004 and data() need.
    static_assert(std::is_aggregate_v<Vec3>);
    static_assert(std::is_aggregate_v<Mat4>);
    static_assert(std::is_aggregate_v<Quat>);

    // E4: data() promises 16 contiguous floats in COLUMN-MAJOR order — what the GPU upload path
    // (0.4.x/0.5.x) memcpy's with no transpose (D8). The static_asserts prove the SIZE; this
    // proves the ORDER, and that std::array (Deviation #1) did not change it.
    const Mat4 m = translation(Vec3{1.0f, 2.0f, 3.0f});
    const float* p = m.data();
    CHECK(p[0] == doctest::Approx(1.0f));   // columns[0].x
    CHECK(p[5] == doctest::Approx(1.0f));   // columns[1].y
    CHECK(p[12] == doctest::Approx(1.0f));  // columns[3].x — translation lives in column 3
    CHECK(p[13] == doctest::Approx(2.0f));
    CHECK(p[14] == doctest::Approx(3.0f));
    CHECK(p[15] == doctest::Approx(1.0f));
}

TEST_CASE("math: default-constructed values are each type's identity element (D14)") {
    // A zero Mat4 would silently collapse all geometry to a point — identity is the only sane
    // default for a transform. Costs a dead store the optimizer removes.
    const Vec3 v;
    CHECK(v == Vec3::zero());
    const Vec2 v2;
    CHECK(v2 == Vec2::zero());
    const Vec4 v4;
    CHECK(v4 == Vec4::zero());
    const Quat q;
    CHECK(q == Quat::identity());
    const Mat3 m3;
    CHECK(m3 == Mat3::identity());
    const Mat4 m4;
    CHECK(m4 == Mat4::identity());
    CHECK(Quat::identity().w == doctest::Approx(1.0f));  // D7: {x,y,z,w} — w is LAST, and it is 1
}

TEST_CASE("math: angle helpers and constants (D6 — radians everywhere)") {
    CHECK(radians(180.0f) == doctest::Approx(PI));
    CHECK(degrees(PI) == doctest::Approx(180.0f));
    CHECK(radians(degrees(1.234f)) == doctest::Approx(1.234f));
    CHECK(TWO_PI == doctest::Approx(2.0f * PI));
    CHECK(HALF_PI == doctest::Approx(PI * 0.5f));
    CHECK(approxEquals(1.0f, 1.0f + (EPSILON * 0.5f)));
    CHECK_FALSE(approxEquals(1.0f, 1.1f));

    // Compile-time usable (E6: no sqrt involved).
    static_assert(radians(180.0f) > 3.14f);
    static_assert(approxEquals(degrees(PI), 180.0f, 1e-3f));
}

TEST_CASE("math: vector algebra against hand-computed literals") {
    const Vec3 a{1.0f, 2.0f, 3.0f};
    const Vec3 b{4.0f, 5.0f, 6.0f};

    CHECK(approxEquals(a + b, Vec3{5.0f, 7.0f, 9.0f}));
    CHECK(approxEquals(b - a, Vec3{3.0f, 3.0f, 3.0f}));
    CHECK(approxEquals(-a, Vec3{-1.0f, -2.0f, -3.0f}));
    CHECK(approxEquals(a * 2.0f, Vec3{2.0f, 4.0f, 6.0f}));
    CHECK(approxEquals(2.0f * a, Vec3{2.0f, 4.0f, 6.0f}));
    CHECK(approxEquals(a * b, Vec3{4.0f, 10.0f, 18.0f}));  // componentwise — not dot, not cross
    CHECK(approxEquals(b / a, Vec3{4.0f, 2.5f, 2.0f}));    // componentwise divide
    CHECK(approxEquals(a / 2.0f, Vec3{0.5f, 1.0f, 1.5f}));
    CHECK(dot(a, b) == doctest::Approx(32.0f));  // 4 + 10 + 18
    CHECK(length(Vec3{3.0f, 4.0f, 0.0f}) == doctest::Approx(5.0f));
    CHECK(lengthSquared(Vec3{3.0f, 4.0f, 0.0f}) == doctest::Approx(25.0f));
    CHECK(distance(Vec3{1.0f, 0.0f, 0.0f}, Vec3{4.0f, 4.0f, 0.0f}) == doctest::Approx(5.0f));
    CHECK(approxEquals(lerp(a, b, 0.5f), Vec3{2.5f, 3.5f, 4.5f}));
    CHECK(approxEquals(lerp(a, b, 0.0f), a));
    CHECK(approxEquals(lerp(a, b, 1.0f), b));

    Vec3 acc = a;
    acc += b;
    CHECK(approxEquals(acc, Vec3{5.0f, 7.0f, 9.0f}));
    acc -= b;
    CHECK(approxEquals(acc, a));
    acc *= 2.0f;
    CHECK(approxEquals(acc, Vec3{2.0f, 4.0f, 6.0f}));
    acc /= 2.0f;
    CHECK(approxEquals(acc, a));
}

TEST_CASE("math: the cross product is RIGHT-HANDED (AC-7, D3)") {
    // THE handedness statement. RH/Y-up/-Z-forward is glTF's convention — the engine's canonical
    // asset format — so imported meshes need no flip anywhere in the pipeline (D3). If anyone
    // ever swaps this to left-handed, THIS is the test that fires.
    CHECK(approxEquals(cross(Vec3::unitX(), Vec3::unitY()), Vec3::unitZ()));
    CHECK(approxEquals(cross(Vec3::unitY(), Vec3::unitZ()), Vec3::unitX()));
    CHECK(approxEquals(cross(Vec3::unitZ(), Vec3::unitX()), Vec3::unitY()));

    const Vec3 a{1.0f, 2.0f, 3.0f};
    const Vec3 b{4.0f, 5.0f, 6.0f};
    CHECK(approxEquals(cross(a, b), Vec3{-3.0f, 6.0f, -3.0f}));  // hand-computed
    CHECK(approxEquals(cross(a, b), -cross(b, a)));              // anticommutative
    CHECK(dot(cross(a, b), a) == doctest::Approx(0.0f));         // orthogonal to both inputs
    CHECK(dot(cross(a, b), b) == doctest::Approx(0.0f));
}

TEST_CASE("math: normalize and normalizeOrZero (D15)") {
    const Vec3 v{3.0f, 4.0f, 0.0f};
    CHECK(length(normalize(v)) == doctest::Approx(1.0f));
    CHECK(approxEquals(normalize(v), Vec3{0.6f, 0.8f, 0.0f}));
    CHECK(approxEquals(normalizeOrZero(v), normalize(v)));

    // The safe variant branches; the unsafe one does not (it is the hottest op in the engine).
    CHECK(approxEquals(normalizeOrZero(Vec3::zero()), Vec3::zero()));

    // E7 — DELIBERATELY NOT TESTED: normalize(Vec3::zero()) fires an assert() in Debug, and
    // doctest has no death tests. An accepted, recorded gap — not an oversight. Callers whose
    // input may be zero must use normalizeOrZero, which IS tested above.
}

TEST_CASE("math: Vec2 and Vec4 mirror Vec3's surface") {
    const Vec2 a{3.0f, 4.0f};
    CHECK(length(a) == doctest::Approx(5.0f));
    CHECK(dot(a, Vec2{1.0f, 0.0f}) == doctest::Approx(3.0f));
    CHECK(approxEquals(normalize(a), Vec2{0.6f, 0.8f}));
    CHECK(approxEquals(a + Vec2{1.0f, 1.0f}, Vec2{4.0f, 5.0f}));
    CHECK(approxEquals(lerp(Vec2::zero(), a, 0.5f), Vec2{1.5f, 2.0f}));

    const Vec4 b{1.0f, 2.0f, 3.0f, 4.0f};
    CHECK(dot(b, b) == doctest::Approx(30.0f));
    CHECK(approxEquals(b * 2.0f, Vec4{2.0f, 4.0f, 6.0f, 8.0f}));
    CHECK(length(normalize(b)) == doctest::Approx(1.0f));

    // Size-changing conversions are EXPLICIT ONLY (D10) — no implicit widening or narrowing, so a
    // Vec3 can never silently become a Vec4 with a fabricated w.
    CHECK(approxEquals(xyz(b), Vec3{1.0f, 2.0f, 3.0f}));
    CHECK(approxEquals(toVec4(Vec3{1.0f, 2.0f, 3.0f}, 4.0f), b));
    CHECK(approxEquals(xyz(toVec4(Vec3{1.0f, 2.0f, 3.0f}, 9.0f)), Vec3{1.0f, 2.0f, 3.0f}));
}

TEST_CASE("math: Vec2 and Vec4 carry AC-4's FULL op list, not just Vec3's") {
    // AC-4 enumerates its op list "for vectors" — all three of them, and epic 0.2's DoD wants every
    // core utility unit-tested. Vec3's instantiation is covered above; these are the Vec2/Vec4 ones.
    // Verified by llvm-cov: without this case, 24 public Vec2/Vec4 functions have ZERO coverage.
    CHECK(approxEquals(Vec2::one(), Vec2{1.0f, 1.0f}));
    CHECK(approxEquals(Vec2::unitX(), Vec2{1.0f, 0.0f}));
    CHECK(approxEquals(Vec2::unitY(), Vec2{0.0f, 1.0f}));

    const Vec2 a{1.0f, 2.0f};
    const Vec2 b{4.0f, 8.0f};
    CHECK(approxEquals(-a, Vec2{-1.0f, -2.0f}));
    CHECK(approxEquals(b - a, Vec2{3.0f, 6.0f}));
    CHECK(approxEquals(a * b, Vec2{4.0f, 16.0f}));  // componentwise
    CHECK(approxEquals(b / a, Vec2{4.0f, 4.0f}));   // componentwise
    CHECK(approxEquals(2.0f * a, Vec2{2.0f, 4.0f}));
    CHECK(approxEquals(b / 2.0f, Vec2{2.0f, 4.0f}));
    CHECK(lengthSquared(Vec2{3.0f, 4.0f}) == doctest::Approx(25.0f));
    CHECK(distance(Vec2{1.0f, 0.0f}, Vec2{4.0f, 4.0f}) == doctest::Approx(5.0f));
    CHECK(approxEquals(normalizeOrZero(Vec2{3.0f, 4.0f}), Vec2{0.6f, 0.8f}));
    CHECK(approxEquals(normalizeOrZero(Vec2::zero()), Vec2::zero()));  // D15's safe variant

    Vec2 acc2 = a;
    acc2 += b;
    CHECK(approxEquals(acc2, Vec2{5.0f, 10.0f}));
    acc2 -= b;
    CHECK(approxEquals(acc2, a));
    acc2 *= 2.0f;
    CHECK(approxEquals(acc2, Vec2{2.0f, 4.0f}));
    acc2 /= 2.0f;
    CHECK(approxEquals(acc2, a));

    CHECK(approxEquals(Vec4::one(), Vec4{1.0f, 1.0f, 1.0f, 1.0f}));
    CHECK(approxEquals(Vec4::unitX(), Vec4{1.0f, 0.0f, 0.0f, 0.0f}));
    CHECK(approxEquals(Vec4::unitY(), Vec4{0.0f, 1.0f, 0.0f, 0.0f}));
    CHECK(approxEquals(Vec4::unitZ(), Vec4{0.0f, 0.0f, 1.0f, 0.0f}));
    CHECK(approxEquals(Vec4::unitW(), Vec4{0.0f, 0.0f, 0.0f, 1.0f}));

    const Vec4 c{1.0f, 2.0f, 3.0f, 4.0f};
    const Vec4 d{2.0f, 4.0f, 6.0f, 8.0f};
    CHECK(approxEquals(-c, Vec4{-1.0f, -2.0f, -3.0f, -4.0f}));
    CHECK(approxEquals(c + c, d));
    CHECK(approxEquals(d - c, c));
    CHECK(approxEquals(c * d, Vec4{2.0f, 8.0f, 18.0f, 32.0f}));  // componentwise
    CHECK(approxEquals(d / c, Vec4{2.0f, 2.0f, 2.0f, 2.0f}));    // componentwise
    CHECK(approxEquals(2.0f * c, d));
    CHECK(approxEquals(d / 2.0f, c));
    CHECK(approxEquals(lerp(c, d, 0.5f), Vec4{1.5f, 3.0f, 4.5f, 6.0f}));
    CHECK(lengthSquared(c) == doctest::Approx(30.0f));
    CHECK(distance(Vec4::zero(), Vec4{1.0f, 2.0f, 2.0f, 4.0f}) == doctest::Approx(5.0f));
    CHECK(length(normalizeOrZero(c)) == doctest::Approx(1.0f));
    CHECK(approxEquals(normalizeOrZero(Vec4::zero()), Vec4::zero()));  // D15's safe variant

    Vec4 acc4 = c;
    acc4 += c;
    CHECK(approxEquals(acc4, d));
    acc4 -= c;
    CHECK(approxEquals(acc4, c));
    acc4 *= 2.0f;
    CHECK(approxEquals(acc4, d));
    acc4 /= 2.0f;
    CHECK(approxEquals(acc4, c));
}

TEST_CASE("math: Mat3::zero() and Mat4::zero() are genuinely all-zero (D13/D14)") {
    // D14 makes the DEFAULT identity, so zero() is the explicit opt-in — and the easy one to get
    // wrong, because `return {};` here would silently hand back IDENTITY and look plausible.
    // Nothing else in this file exercises it (verified by llvm-cov).
    const Mat4 z4 = Mat4::zero();
    CHECK(approxEquals(z4.columns[0], Vec4::zero()));
    CHECK(approxEquals(z4.columns[1], Vec4::zero()));
    CHECK(approxEquals(z4.columns[2], Vec4::zero()));
    CHECK(approxEquals(z4.columns[3], Vec4::zero()));
    CHECK_FALSE(z4 == Mat4::identity());
    CHECK(determinant(z4) == doctest::Approx(0.0f));  // degenerate: collapses all geometry
    CHECK(approxEquals(transformDirection(z4, Vec3::one()), Vec3::zero()));

    const Mat3 z3 = Mat3::zero();
    CHECK(approxEquals(z3.columns[0], Vec3::zero()));
    CHECK(approxEquals(z3.columns[1], Vec3::zero()));
    CHECK(approxEquals(z3.columns[2], Vec3::zero()));
    CHECK_FALSE(z3 == Mat3::identity());
    CHECK(determinant(z3) == doctest::Approx(0.0f));
    CHECK(approxEquals(z3 * Vec3::one(), Vec3::zero()));
}

TEST_CASE("math: Mat4 algebra and round-trips (AC-5)") {
    const Mat4 t = translation(Vec3{1.0f, 2.0f, 3.0f});
    const Mat4 s = scaling(Vec3{2.0f, 3.0f, 4.0f});
    const Mat4 i = Mat4::identity();

    CHECK(approxEquals(t * i, t));
    CHECK(approxEquals(i * t, t));
    CHECK(approxEquals((t * s) * i, t * (s * i)));    // associativity
    CHECK(approxEquals(transpose(transpose(t)), t));  // involution round-trip

    // Round-trip: M * inverse(M) == I. Inversion accumulates error -> looser tolerance (E11).
    const Mat4 m = t * s;
    CHECK(approxEquals(m * inverse(m), Mat4::identity(), MATRIX_EPSILON));
    CHECK(approxEquals(inverse(m) * m, Mat4::identity(), MATRIX_EPSILON));

    CHECK(determinant(Mat4::identity()) == doctest::Approx(1.0f));
    CHECK(determinant(s) == doctest::Approx(24.0f));  // 2 * 3 * 4

    // transformPoint (w=1) and transformDirection (w=0) differ by EXACTLY the translation.
    const Vec3 p{5.0f, 6.0f, 7.0f};
    CHECK(approxEquals(transformPoint(t, p), Vec3{6.0f, 8.0f, 10.0f}));
    CHECK(approxEquals(transformDirection(t, p), p));  // a direction is never translated
    CHECK(approxEquals(transformPoint(t, p) - transformDirection(t, p), Vec3{1.0f, 2.0f, 3.0f}));

    // D8: Model = T * R * S applies S FIRST. Scale then translate, not the reverse.
    CHECK(approxEquals(transformPoint(t * s, Vec3::one()), Vec3{3.0f, 5.0f, 7.0f}));

    // toMat3 lifts out the rotation/scale block and drops the translation.
    CHECK(approxEquals(toMat3(t), Mat3::identity()));
}

TEST_CASE("math: Mat3 algebra (the rotation/scale block, no translation)") {
    const Mat3 i = Mat3::identity();
    const Quat q = fromAxisAngle(Vec3::unitZ(), HALF_PI);
    const Mat3 r = toMat3(q);

    CHECK(approxEquals(r * i, r));
    CHECK(approxEquals(transpose(transpose(r)), r));
    CHECK(determinant(i) == doctest::Approx(1.0f));
    CHECK(determinant(r) == doctest::Approx(1.0f));  // a pure rotation preserves volume
    CHECK(approxEquals(r * inverse(r), Mat3::identity(), MATRIX_EPSILON));

    // A rotation matrix is orthonormal: its inverse IS its transpose.
    CHECK(approxEquals(inverse(r), transpose(r), MATRIX_EPSILON));

    // +90 deg about +Z takes +X to +Y — right-handed (D3).
    CHECK(approxEquals(r * Vec3::unitX(), Vec3::unitY(), MATRIX_EPSILON));

    // AC-11: 9 contiguous floats, column-major.
    Mat3 s;
    s.columns[0] = Vec3{1.0f, 2.0f, 3.0f};
    s.columns[1] = Vec3{4.0f, 5.0f, 6.0f};
    s.columns[2] = Vec3{7.0f, 8.0f, 9.0f};
    CHECK(s.data()[0] == doctest::Approx(1.0f));
    CHECK(s.data()[3] == doctest::Approx(4.0f));  // columns[1].x
    CHECK(s.data()[8] == doctest::Approx(9.0f));  // columns[2].z
}

TEST_CASE("math: quaternion algebra and rotation sense") {
    const Quat i = Quat::identity();
    const Quat q = fromAxisAngle(Vec3::unitY(), HALF_PI);

    CHECK(approxEquals(q * i, q));
    CHECK(approxEquals(i * q, q));
    CHECK(length(q) == doctest::Approx(1.0f));  // fromAxisAngle already yields a unit quat
    CHECK(length(normalize(q)) == doctest::Approx(1.0f));
    CHECK(approxEquals(normalizeOrIdentity(Quat{0.0f, 0.0f, 0.0f, 0.0f}), Quat::identity()));
    CHECK(dot(i, i) == doctest::Approx(1.0f));

    // RIGHT-HANDED rotation sense (D3): +90 deg about +Y takes +X to -Z. Hand-derived.
    CHECK(approxEquals(q * Vec3::unitX(), -Vec3::unitZ()));

    // Rotate, then rotate back by the inverse -> identity (AC-5).
    const Vec3 v{1.0f, 2.0f, 3.0f};
    CHECK(approxEquals(inverse(q) * (q * v), v));
    CHECK(approxEquals(conjugate(q) * (q * v), v));  // == inverse for a UNIT quat
    CHECK(approxEquals(q * conjugate(q), Quat::identity()));
}

TEST_CASE("math: Hamilton product operand order matches D8's convention (a*b applies b first)") {
    // quat.hpp:37-38: "(a * b) applies b first, then a" — the same right-to-left convention as
    // Mat4's operator* (D8). Every assertion in the previous test case is ORDER-SYMMETRIC
    // (q*i==q, i*q==q, q*conjugate(q)==identity all hold under either operand order), so a
    // transposed operator* (returning b*a) would leave the whole suite green. Two NON-COMMUTING
    // rotations are required to actually pin the order — this is the quat-path analogue of the
    // Mat4 operand-order proof already covered above (transformPoint(t*s, ...)).
    const Quat a = fromAxisAngle(Vec3::unitX(), HALF_PI);
    const Quat b = fromAxisAngle(Vec3::unitY(), HALF_PI);
    const Vec3 v = Vec3::unitZ();

    // (a * b) applies b first, then a -- so (a * b) * v must equal a * (b * v), NOT b * (a * v).
    CHECK(approxEquals((a * b) * v, a * (b * v)));
    CHECK_FALSE(approxEquals((a * b) * v, b * (a * v)));  // the two orders genuinely differ
}

TEST_CASE("math: quaternion <-> matrix round-trip handles double cover (AC-5, E8)") {
    const Quat q = normalize(Quat{0.2f, 0.3f, 0.4f, 0.5f});

    // E8 — DOUBLE COVER: q and -q encode the SAME rotation, so quat_cast may legitimately hand
    // back -q. approxEquals(Quat, Quat) compares BOTH signs; a naive componentwise compare here
    // would be a flaky test that "fails" on correct code.
    CHECK(approxEquals(toQuat(toMat3(q)), q, MATRIX_EPSILON));

    // The ROTATION is what must round-trip — sign-independent, so this is the stronger check.
    const Vec3 v{1.0f, 2.0f, 3.0f};
    CHECK(approxEquals(toQuat(toMat3(q)) * v, q * v, MATRIX_EPSILON));

    // toMat4(q) is toMat3(q) embedded in the upper-left 3x3, with no translation.
    CHECK(approxEquals(toMat3(toMat4(q)), toMat3(q), MATRIX_EPSILON));
    CHECK(approxEquals(transformPoint(toMat4(q), v), q * v, MATRIX_EPSILON));
}

TEST_CASE("math: approxEquals(Quat, Quat) accepts +/-q, and is not a rubber stamp (E8)") {
    // E8 is load-bearing, and until this case existed NOTHING proved it: every other Quat
    // comparison in this file happens to pass via the same-SIGN branch (GLM's quat_cast returns +q
    // for the value used above), so `return same || negated;` could have been cut down to
    // `return same;` with the whole suite still green — and E8's protection would have silently
    // evaporated, leaving a future test to "fail" on correct code.
    const Quat q = normalize(Quat{0.2f, 0.3f, 0.4f, 0.5f});
    const Quat negQ{-q.x, -q.y, -q.z, -q.w};

    CHECK(approxEquals(q, negQ));  // q and -q are the SAME rotation — double cover
    CHECK(approxEquals(negQ, q));  // and the relation is symmetric

    // Proof they really are the same rotation, established independently of approxEquals itself.
    const Vec3 v{1.0f, 2.0f, 3.0f};
    CHECK(approxEquals(negQ * v, q * v, MATRIX_EPSILON));

    // ...but it must not accept everything: genuinely different rotations still compare unequal
    // under BOTH signs, or the double-cover tolerance would be hiding real regressions.
    CHECK_FALSE(approxEquals(fromAxisAngle(Vec3::unitY(), HALF_PI), fromAxisAngle(Vec3::unitX(), HALF_PI)));
    CHECK_FALSE(approxEquals(q, Quat::identity()));
}

TEST_CASE("math: slerp endpoints and degenerate cases") {
    const Quat a = Quat::identity();
    const Quat b = fromAxisAngle(Vec3::unitY(), HALF_PI);

    CHECK(approxEquals(slerp(a, b, 0.0f), a));
    CHECK(approxEquals(slerp(a, b, 1.0f), b));
    CHECK(approxEquals(slerp(a, a, 0.5f), a));
    CHECK(length(slerp(a, b, 0.3f)) == doctest::Approx(1.0f));

    // Halfway along the arc is HALF the angle — constant angular velocity, slerp's whole point.
    CHECK(approxEquals(slerp(a, b, 0.5f), fromAxisAngle(Vec3::unitY(), HALF_PI * 0.5f), MATRIX_EPSILON));

    // lerp is the cheap approximation: same endpoints, unit output, different middle.
    CHECK(approxEquals(lerp(a, b, 0.0f), a));
    CHECK(approxEquals(lerp(a, b, 1.0f), b));
    CHECK(length(lerp(a, b, 0.3f)) == doctest::Approx(1.0f));

    // E8 double-cover / shorter-arc branch: dot(a, b) here is +0.7071 (both endpoints), so
    // `dot(a, b) < 0.0f` in lerp()'s body never fires for any assertion above — deleting that
    // branch would leave the suite green. -b is the SAME rotation as b (double cover), so
    // lerp(a, -b, t) must trace the SAME interpolated rotation as lerp(a, b, t): the branch picks
    // the shorter of the two arcs (to -b vs to b) instead of blindly lerping toward whichever sign
    // was passed in. Compare via rotated vectors, not raw quaternion components, since the
    // intermediate lerp() *output* quaternion itself is allowed to differ in sign.
    const Quat negB{-b.x, -b.y, -b.z, -b.w};
    const Vec3 v = Vec3::unitX();
    CHECK(approxEquals(lerp(a, negB, 0.5f) * v, lerp(a, b, 0.5f) * v));
}

TEST_CASE("math: compose -> decompose -> compose round-trip (AC-5, the deliverable's own words)") {
    Trs trs;
    trs.translation = Vec3{1.0f, -2.0f, 3.0f};
    trs.rotation = fromAxisAngle(normalize(Vec3{1.0f, 2.0f, 3.0f}), radians(35.0f));
    trs.scale = Vec3{2.0f, 3.0f, 4.0f};

    const Mat4 m = compose(trs);

    Trs out;
    REQUIRE(decompose(m, out));
    CHECK(approxEquals(out.translation, trs.translation, MATRIX_EPSILON));
    CHECK(approxEquals(out.scale, trs.scale, MATRIX_EPSILON));
    CHECK(approxEquals(out.rotation, trs.rotation, MATRIX_EPSILON));  // E8: +/-q both accepted

    // The full round-trip: compose(decompose(M)) == M.
    CHECK(approxEquals(compose(out), m, MATRIX_EPSILON));

    // The identity TRS composes to the identity matrix.
    const Trs identityTrs;
    CHECK(approxEquals(compose(identityTrs), Mat4::identity()));
}

TEST_CASE("math: decompose handles mirrored (negative-scale) matrices (AC-5, D9)") {
    // Covers decompose()'s negative-determinant branch — the `determinant(m) < 0 ? -sxRaw : sxRaw`
    // ternary, which had ZERO coverage (verified by llvm-cov) because every other decompose test
    // uses positive scale. Without the sign flip the normalized 3x3 is IMPROPER (det -1) and
    // quat_cast hands back a garbage rotation. The Phase-2 inspector hits this the first time a
    // user types a negative number into a scale field.
    Trs trs;
    trs.translation = Vec3{1.0f, -2.0f, 3.0f};
    trs.rotation = fromAxisAngle(normalize(Vec3{1.0f, 2.0f, 3.0f}), radians(35.0f));
    trs.scale = Vec3{-2.0f, 3.0f, 4.0f};  // X mirrored — the axis D9's convention flips

    const Mat4 m = compose(trs);
    REQUIRE(determinant(m) < 0.0f);  // an odd number of mirrored axes

    Trs out;
    REQUIRE(decompose(m, out));

    // D9 puts the flip on X — Aero's OWN convention, not glm::decompose's: glm's implementation
    // (glm/gtx/matrix_decompose.inl:130-137) negates ALL THREE scale components on det<0, so for
    // this exact input glm would report scale (-2,-3,-4) where Aero reports (-2,+3,+4). Both are
    // legitimate independent choices (either makes the normalized 3x3 proper, det +1); this
    // engine picks "flip X only" and this test is what pins that choice — do not "align with
    // glm::decompose" on the strength of a comment, it would break this assertion.
    // An X-mirrored input round-trips its COMPONENTS exactly — sign included.
    CHECK(out.scale.x < 0.0f);
    CHECK(approxEquals(out.translation, trs.translation, MATRIX_EPSILON));
    CHECK(approxEquals(out.scale, trs.scale, MATRIX_EPSILON));
    CHECK(approxEquals(out.rotation, trs.rotation, MATRIX_EPSILON));
    CHECK(approxEquals(compose(out), m, MATRIX_EPSILON));

    // A mirror on a DIFFERENT axis is not uniquely recoverable — TRS cannot distinguish "flip Y"
    // from "flip X and rotate", and the convention always reports the flip on X. So the components
    // legitimately differ; what MUST hold is that the MATRIX round-trips and the handedness
    // (determinant sign) survives. Asserting component equality here would be asserting a falsehood.
    Trs yMirrored = trs;
    yMirrored.scale = Vec3{2.0f, -3.0f, 4.0f};
    const Mat4 my = compose(yMirrored);
    REQUIRE(determinant(my) < 0.0f);

    Trs outY;
    REQUIRE(decompose(my, outY));
    CHECK(approxEquals(compose(outY), my, MATRIX_EPSILON));
    CHECK(determinant(compose(outY)) < 0.0f);
}

TEST_CASE("math: decompose reports failure and leaves `out` untouched (D9)") {
    // docs/04: no exceptions across the public API — an explicit status, never a throw.
    // Each of the three axes is exercised independently — a collapsed axis is unrecoverable
    // regardless of which column it lives in, and this guarantees no axis-specific bug hides
    // behind the other two being well-formed.
    const Mat4 collapsedX = scaling(Vec3{0.0f, 1.0f, 1.0f});
    const Mat4 collapsedY = scaling(Vec3{1.0f, 0.0f, 1.0f});
    const Mat4 collapsedZ = scaling(Vec3{1.0f, 1.0f, 0.0f});

    for (const Mat4& degenerate : {collapsedX, collapsedY, collapsedZ}) {
        Trs out;
        out.translation = Vec3{9.0f, 9.0f, 9.0f};  // sentinel: must survive the failed call
        const Trs before = out;

        CHECK_FALSE(decompose(degenerate, out));
        CHECK(approxEquals(out.translation, before.translation));
        CHECK(approxEquals(out.scale, before.scale));
        CHECK(approxEquals(out.rotation, before.rotation));
    }
}

TEST_CASE("math: decompose rejects NaN/Inf columns instead of reporting false success") {
    // REAL BUG this test guards against: for a NaN matrix, every ORDERED comparison against
    // EPSILON is false, so a `<=` guard (`sxRaw <= EPSILON`) does NOT catch it — NaN silently
    // passes as "not degenerate". determinant(m) < 0.0f is then also false for NaN, so the
    // negative-determinant branch is skipped, sx becomes NaN, and decompose() used to return
    // TRUE with a NaN rotation/translation written into `out` — reported as SUCCESS with garbage
    // data, exactly the "guess instead of report" failure D9's own comment disclaims. The fix is
    // the negated guard `!(sxRaw > EPSILON)`, which IS true for NaN (every comparison with NaN,
    // including `>`, is false, so its negation is true) and correctly rejects it.
    //
    // Inf needs a SEPARATE check: `Inf > EPSILON` is true (unlike NaN comparisons), so the
    // ordered-comparison guard alone lets an infinite-length column through as "not degenerate".
    // The division that follows (e.g. c1 / sy where sy == Inf) then computes Inf / Inf
    // component-wise, which IS NaN by IEEE 754 — corrupting the rotation the same way the NaN
    // case would have. decompose() therefore also requires `std::isfinite()` on each axis length.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    Mat4 nanMatrix = Mat4::identity();
    nanMatrix.columns[0] = Vec4{nan, 0.0f, 0.0f, 0.0f};  // NaN in the X-scale column

    Mat4 infMatrix = Mat4::identity();
    infMatrix.columns[1] = Vec4{0.0f, inf, 0.0f, 0.0f};  // Inf in the Y-scale column

    for (const Mat4& bad : {nanMatrix, infMatrix}) {
        Trs out;
        out.translation = Vec3{9.0f, 9.0f, 9.0f};  // sentinel: must survive the failed call
        out.scale = Vec3{9.0f, 9.0f, 9.0f};
        const Trs before = out;

        CHECK_FALSE(decompose(bad, out));
        CHECK(approxEquals(out.translation, before.translation));
        CHECK(approxEquals(out.scale, before.scale));
        CHECK(approxEquals(out.rotation, before.rotation));
    }
}

TEST_CASE("math: perspective maps near->0 and far->1 in NDC (AC-6, D4 — THE depth convention)") {
    // THE executable statement of D4, and the money test of this task. SDL_GPU's header
    // (SDL_gpu.h, "Coordinate System") documents: "The lower-left corner has an x,y coordinate of
    // (-1.0, -1.0). The upper-right corner is (1.0, 1.0). Z values range from [0.0, 1.0] where 0
    // is the near plane." The RHI is SDL_GPU (docs/03, "sacred"), so the depth range is decided
    // FOR us by the layer below — hence perspectiveRH_ZO, never the [-1,1] GL convention. If
    // anyone ever swaps in a [-1,1] projection, THIS test fires.
    //
    // Note what is NOT here: no proj[1][1] *= -1 Vulkan Y-flip. SDL's NDC is Y-UP and SDL
    // converts Vulkan's Y-down NDC internally ("SDL will automatically convert the coordinate
    // system behind the scenes"), so a flip here would DOUBLE-flip the image. Never add one.
    //
    // NEAR_PLANE/FAR_PLANE, not NEAR/FAR: the latter are Windows SDK macros.
    constexpr float NEAR_PLANE = 0.1f;
    constexpr float FAR_PLANE = 100.0f;
    const Mat4 p = perspective(radians(60.0f), 16.0f / 9.0f, NEAR_PLANE, FAR_PLANE);

    // D3: right-handed view space, -Z forward -> points in front of the camera have NEGATIVE z.
    const Vec4 nearClip = p * Vec4{0.0f, 0.0f, -NEAR_PLANE, 1.0f};
    const Vec4 farClip = p * Vec4{0.0f, 0.0f, -FAR_PLANE, 1.0f};

    // w == -z_view -> w > 0 in front of the camera, so the perspective divide is well-defined.
    CHECK(nearClip.w == doctest::Approx(NEAR_PLANE));
    CHECK(farClip.w == doctest::Approx(FAR_PLANE));

    CHECK(nearClip.z / nearClip.w == doctest::Approx(0.0f));  // near plane -> NDC z = 0
    CHECK(farClip.z / farClip.w == doctest::Approx(1.0f));    // far  plane -> NDC z = 1

    // Depth increases monotonically away from the camera (what the depth buffer relies on).
    const Vec4 midClip = p * Vec4{0.0f, 0.0f, -10.0f, 1.0f};
    CHECK(midClip.z / midClip.w > 0.0f);
    CHECK(midClip.z / midClip.w < 1.0f);
}

TEST_CASE("math: ortho maps near->0 and far->1 in NDC (AC-6, D4)") {
    constexpr float NEAR_PLANE = 0.1f;
    constexpr float FAR_PLANE = 100.0f;
    const Mat4 p = ortho(-1.0f, 1.0f, -1.0f, 1.0f, NEAR_PLANE, FAR_PLANE);

    const Vec4 nearClip = p * Vec4{0.0f, 0.0f, -NEAR_PLANE, 1.0f};
    const Vec4 farClip = p * Vec4{0.0f, 0.0f, -FAR_PLANE, 1.0f};

    CHECK(nearClip.w == doctest::Approx(1.0f));  // orthographic: no divide
    CHECK(farClip.w == doctest::Approx(1.0f));
    CHECK(nearClip.z == doctest::Approx(0.0f));
    CHECK(farClip.z == doctest::Approx(1.0f));

    // X/Y still map to [-1,1] (SDL_gpu.h: lower-left (-1,-1), upper-right (1,1)).
    const Vec4 corner = p * Vec4{1.0f, 1.0f, -NEAR_PLANE, 1.0f};
    CHECK(corner.x == doctest::Approx(1.0f));
    CHECK(corner.y == doctest::Approx(1.0f));
}

TEST_CASE("math: lookAt puts -Z forward in view space (AC-7, D3)") {
    const Vec3 eye{0.0f, 0.0f, 5.0f};
    const Mat4 view = lookAt(eye, Vec3::zero(), Vec3::unitY());

    // The camera sits at +5 on Z looking at the origin, so the origin must land at view-space
    // z = -5: -Z IS FORWARD (D3, glTF's convention). A left-handed lookAt would give +5.
    CHECK(approxEquals(transformPoint(view, Vec3::zero()), Vec3{0.0f, 0.0f, -5.0f}, MATRIX_EPSILON));

    // The eye maps to the view-space origin.
    CHECK(approxEquals(transformPoint(view, eye), Vec3::zero(), MATRIX_EPSILON));

    // A rigid transform: det == +1 (no scale, no mirror), and it round-trips with its inverse.
    CHECK(determinant(view) == doctest::Approx(1.0f));
    CHECK(approxEquals(view * inverse(view), Mat4::identity(), MATRIX_EPSILON));

    // Distances are preserved.
    CHECK(distance(transformPoint(view, Vec3::zero()), transformPoint(view, Vec3::unitX())) == doctest::Approx(1.0f));
}

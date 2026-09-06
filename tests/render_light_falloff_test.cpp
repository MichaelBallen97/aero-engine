// tests/render_light_falloff_test.cpp -- task E.2.2: the punctual-light falloff vocabulary. Tier-0
// throughout: no GPU, no files, no randomness. PF1 asserts <aero/render/render.hpp> ALONE carries
// the vocabulary, so this file must never include light_falloff.hpp or lighting.hpp directly, and it
// never reaches engine/render/src -- the packer cases live in render_material_test.cpp for that
// reason (E.2.1's HE1 / sky_pack.hpp split, in its second instance).
#include <aero/render/render.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>  // std::memcpy for the bit helper -- MSVC's STL supplies no <cstring> transitively
#include <fstream>
#include <limits>
#include <ostream>  // a CHECK on a std::string_view needs this or MSVC ALONE fails (the 0.4.1 trap)
#include <string>
#include <string_view>

namespace {

namespace rd = engine::render;

// The umbrella header's own source, reached through AERO_SHADERS_SRC_DIR -- the ONE route into the
// source tree aero_tests has, defined UNCONDITIONALLY, which is what lets PF1's source-text arm ride
// both reduced configurations exactly as HE1 does.
constexpr std::string_view RENDER_UMBRELLA_PATH =
    AERO_SHADERS_SRC_DIR "/../engine/render/include/aero/render/render.hpp";

// COMMENT-STRIPPED, so a token that appears only in a comment cannot satisfy a source-text arm --
// and it is what makes PF1 refuse a COMMENTED-OUT include, which a compile cannot distinguish from
// a missing one in a header some other TU happens to pull in.
[[nodiscard]] std::string strippedSourceAt(std::string_view absolutePath) {
    std::ifstream file{std::string{absolutePath}};
    std::string out;
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t comment = line.find("//");
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        out += line;
        out += '\n';
    }
    return out;
}

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// The float's bit pattern, for the arms that say BIT-exact rather than equal: `==` alone cannot
// tell +0.0 from -0.0, and "exactly 0" below is a claim about bits (render_sky_test.cpp's own).
[[nodiscard]] std::uint32_t bitsOf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
constexpr std::uint32_t POSITIVE_ZERO_BITS = 0x00000000U;

// doctest decomposes and prints BOTH sides; without this every SpotCone assertion prints {?} == {?}
// on the one run that matters. An operator<<, NEVER a toString -- DOCTEST_STRINGIFY expands to an
// UNQUALIFIED toString(...), which ADL finds and which then hard-errors inside doctest.h. At
// namespace scope, not inside a case: [class.friend]/6 forbids defining a friend in a local class.
std::ostream& operator<<(std::ostream& out, const rd::SpotCone& cone) {
    out << "SpotCone{scale " << cone.angleScale << ", offset " << cone.angleOffset << '}';
    return out;
}

}  // namespace

TEST_CASE("render light falloff: the umbrella header alone carries the punctual vocabulary (PF1)") {
    // THE NAMING ARM IS A SMOKE TEST OF SPELLING, not of the include: everything below is
    // transitively satisfiable through any other render header that happens to pull light_falloff.hpp
    // in. The SOURCE-TEXT arm underneath is what carries the claim (HE1 records exactly this split).
    [[maybe_unused]] const std::uint32_t maxSpots = rd::MAX_SPOT_LIGHTS;
    [[maybe_unused]] const rd::SpotLightData spotData{};
    [[maybe_unused]] const rd::SpotCone cone = rd::resolveSpotCone(0.3F, 0.5F);
    [[maybe_unused]] const float distance = rd::punctualDistanceAttenuation(1.0F, 10.0F);
    [[maybe_unused]] const float angle = rd::spotConeAttenuation(1.0F, cone);
    [[maybe_unused]] const float minDistanceSq = rd::PUNCTUAL_MIN_DISTANCE_SQ;
    [[maybe_unused]] const float minRange = rd::PUNCTUAL_MIN_RANGE;
    [[maybe_unused]] const float minCosDelta = rd::SPOT_CONE_MIN_COS_DELTA;

    const std::string umbrella = strippedSourceAt(RENDER_UMBRELLA_PATH);
    REQUIRE_FALSE(umbrella.empty());
    CHECK(contains(umbrella, "#include <aero/render/light_falloff.hpp>"));
    // ANTI-VACUITY: a reader that matched everything would satisfy the arm above for free.
    CHECK_FALSE(contains(umbrella, "#include <aero/render/does_not_exist.hpp>"));
}

TEST_CASE("render light falloff: the three floors and both budgets are pinned by value (PF2)") {
    // Tuning constants (D10). A drift is loud here rather than silent in a picture.
    CHECK(rd::MAX_SPOT_LIGHTS == 8U);
    CHECK(rd::MAX_POINT_LIGHTS == 8U);  // unmoved by task E.2.2
    CHECK(rd::PUNCTUAL_MIN_DISTANCE_SQ == 1e-4F);
    CHECK(rd::PUNCTUAL_MIN_RANGE == 1e-4F);
    CHECK(rd::SPOT_CONE_MIN_COS_DELTA == 1e-4F);
}

TEST_CASE("render light falloff: the window is exactly 1 at the light and exactly 0 from range out (PF3)") {
    // window(0) is EXACTLY 1: 1.0F - 0.0F is 1 under any contraction and saturate(1) returns the
    // literal 1.0F, so the whole result is the SAME IEEE division on both sides of this ==.
    CHECK(rd::punctualDistanceAttenuation(0.0F, 10.0F) == 1.0F / rd::PUNCTUAL_MIN_DISTANCE_SQ);

    CHECK(rd::punctualDistanceAttenuation(100.0F, 10.0F) == 0.0F);
    CHECK(bitsOf(rd::punctualDistanceAttenuation(100.0F, 10.0F)) == POSITIVE_ZERO_BITS);

    // 64 samples strictly beyond the cutoff. The BIT form is what makes "exactly nothing" a claim
    // rather than a tolerance -- LP4 reads the same +0.0 back out of a texel.
    const float range = 10.0F;
    for (int k = 1; k <= 64; ++k) {
        const float distanceSq = range * range * (1.0F + (static_cast<float>(k) / 16.0F));
        CAPTURE(k);
        CHECK(bitsOf(rd::punctualDistanceAttenuation(distanceSq, range)) == POSITIVE_ZERO_BITS);
    }
}

TEST_CASE("render light falloff: intensity means the irradiance at one world unit (PF4)") {
    // At range 1000 the window's deviation from 1 is 2*d^4/1e12 <= 8.2e-9 for every d below, so
    // atten * d^2 is the inverse-square law itself. A BARE 1/d^2 passes this arm too -- PF6 is what
    // separates the two laws.
    for (const float d : {0.5F, 1.0F, 2.0F, 4.0F, 8.0F}) {
        CAPTURE(d);
        const float atten = rd::punctualDistanceAttenuation(d * d, 1000.0F);
        CHECK(static_cast<double>(atten * d * d) == doctest::Approx(1.0).epsilon(1e-5));
    }
}

TEST_CASE("render light falloff: the window is non-increasing and vanishes smoothly at range (PF5)") {
    const float range = 10.0F;
    float previous = std::numeric_limits<float>::infinity();
    float lastBelowRange = -1.0F;
    for (int k = 0; k <= 255; ++k) {
        const float d = static_cast<float>(k) * 1.5F * range / 255.0F;
        const float atten = rd::punctualDistanceAttenuation(d * d, range);
        CAPTURE(k);
        CAPTURE(d);
        // NON-INCREASING, ties allowed: a strict-decrease assertion would fail on the legitimate
        // run of exact zeros beyond the cutoff.
        CHECK(atten <= previous);
        previous = atten;
        if (d < range) {
            lastBelowRange = atten;
        }
    }
    // NO RING at the edge: the window's derivative vanishes at range, so the last sample strictly
    // inside it (d = 9.941) is already three orders below 1/r^2. Measured 5.50e-06.
    REQUIRE(lastBelowRange >= 0.0F);
    CHECK(lastBelowRange < 1e-3F / (range * range));
}

TEST_CASE("render light falloff: the D3 table by value, which the two look-alike laws fail (PF6)") {
    // r = 10. The SHAPE by value, so a bare 1/d^2 (0.0156 at d = 8) and the pre-E.2.2 squared
    // linear ramp (1 - d/r)^2 (0.0400 at d = 8) both redden. doctest::Approx(v).epsilon(0) NEVER
    // matches (its comparison is `< 0`), so the tolerance is an explicit absolute one with the
    // value inside the assertion.
    const float range = 10.0F;
    struct Sample {
        float d;
        float want;
    };
    constexpr std::array<Sample, 6> TABLE{
        {{1.0F, 0.9998F}, {2.0F, 0.2492F}, {3.0F, 0.1093F}, {5.0F, 0.0352F}, {8.0F, 0.0054F}, {9.0F, 0.0015F}}};
    for (const Sample& sample : TABLE) {
        CAPTURE(sample.d);
        CAPTURE(sample.want);
        const float got = rd::punctualDistanceAttenuation(sample.d * sample.d, range);
        CAPTURE(got);
        CHECK(std::fabs(got - sample.want) < 1e-4F);
    }
}

TEST_CASE("render light falloff: the distance term is total on every input (PF7)") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // A NaN range takes the floor, so it is INDISTINGUISHABLE from range == PUNCTUAL_MIN_RANGE --
    // asserted as bit equality, not as "finite".
    for (const float distanceSq : {0.0F, 1e-6F, 1.0F, 100.0F}) {
        CAPTURE(distanceSq);
        CHECK(bitsOf(rd::punctualDistanceAttenuation(distanceSq, nan)) ==
              bitsOf(rd::punctualDistanceAttenuation(distanceSq, rd::PUNCTUAL_MIN_RANGE)));
    }

    // A non-finite DISTANCE yields +0.0, through saturate's comparison chain.
    CHECK(bitsOf(rd::punctualDistanceAttenuation(nan, 10.0F)) == POSITIVE_ZERO_BITS);
    CHECK(bitsOf(rd::punctualDistanceAttenuation(inf, 10.0F)) == POSITIVE_ZERO_BITS);
    CHECK(bitsOf(rd::punctualDistanceAttenuation(-inf, 10.0F)) == POSITIVE_ZERO_BITS);

    // range <= 0 contributes NOTHING BEYOND 1e-4 WORLD UNITS -- which is not the same sentence as
    // "nothing for every d^2 > 0". At range 0 the floor gives r = 1e-4, so f = d^2 / 1e-8 and the
    // window is exactly 0 once f >= 1, i.e. d^2 >= 1e-8. Every sample below has f >= 100.
    for (const float range : {0.0F, -5.0F}) {
        CAPTURE(range);
        for (const float distanceSq : {1e-6F, 1e-4F, 1e-2F, 1.0F, 100.0F}) {
            CAPTURE(distanceSq);
            CHECK(bitsOf(rd::punctualDistanceAttenuation(distanceSq, range)) == POSITIVE_ZERO_BITS);
        }
        CHECK(rd::punctualDistanceAttenuation(0.0F, range) == 1.0F / rd::PUNCTUAL_MIN_DISTANCE_SQ);
    }

    // THE ANTI-VACUITY OF THE SENTENCE ABOVE: below d^2 = 1e-8 the floor is a FLOOR, not a switch --
    // the window is still a window and the divisor is floored, so the result is large, finite and
    // positive. Compared against the .cpp's own expression rather than a decimal (it is 9801.001).
    const float f = 1e-9F / 1e-8F;
    const float raw = 1.0F - (f * f);
    const float window = (raw > 0.0F) ? ((raw < 1.0F) ? raw : 1.0F) : 0.0F;
    const float floored = rd::punctualDistanceAttenuation(1e-9F, 0.0F);
    CHECK(std::isfinite(floored));
    CHECK(floored > 0.0F);
    CHECK(floored == (window * window) / 1e-4F);
}

TEST_CASE("render light falloff: the cone pair is the resolver's formula, bit for bit (PF8)") {
    // `const float` so std::cos(float) is the overload the .cpp calls. This is a FORMULA PIN and is
    // stated as one: for the defaults the .cpp's atLeast returns its first argument unchanged, so
    // both sides are the same two IEEE operations.
    const float inner = engine::radians(20.0F);
    const float outer = engine::radians(30.0F);
    const rd::SpotCone cone = rd::resolveSpotCone(inner, outer);
    const float scale = 1.0F / (std::cos(inner) - std::cos(outer));
    CHECK(cone.angleScale == scale);
    CHECK(cone.angleOffset == -std::cos(outer) * scale);

    // ...and by NUMBER, which is what catches a swapped pair once the formula above is rewritten to
    // match the swap. Measured 13.5745573 / -11.7559109.
    CHECK(std::fabs(cone.angleScale - 13.5746F) < 1e-3F);
    CHECK(std::fabs(cone.angleOffset - (-11.7559F)) < 1e-3F);
}

TEST_CASE("render light falloff: the cone is exactly 1 inside, exactly 0 outside, squared between (PF9)") {
    // The +-1e-3 offsets are deliberate: AT the exact edge the residual of (cosAngle * scale +
    // offset) is FMA-dependent (either compiler may contract), so an "exactly 0 / exactly 1" arm
    // sampled there would be a lane-dependent coin flip.
    struct Pair {
        float innerDeg;
        float outerDeg;
    };
    constexpr std::array<Pair, 10> PAIRS{{{5.0F, 10.0F},
                                          {10.0F, 20.0F},
                                          {15.0F, 30.0F},
                                          {20.0F, 30.0F},
                                          {20.0F, 45.0F},
                                          {30.0F, 60.0F},
                                          {45.0F, 60.0F},
                                          {10.0F, 80.0F},
                                          {60.0F, 85.0F},
                                          {2.0F, 89.0F}}};
    for (const Pair& pair : PAIRS) {
        CAPTURE(pair.innerDeg);
        CAPTURE(pair.outerDeg);
        const float inner = engine::radians(pair.innerDeg);
        const float outer = engine::radians(pair.outerDeg);
        const rd::SpotCone cone = rd::resolveSpotCone(inner, outer);
        const float cosInner = std::cos(inner);
        const float cosOuter = std::cos(outer);

        CHECK(rd::spotConeAttenuation(1.0F, cone) == 1.0F);                                    // the axis
        CHECK(bitsOf(rd::spotConeAttenuation(cosOuter - 1e-3F, cone)) == POSITIVE_ZERO_BITS);  // outside
        CHECK(rd::spotConeAttenuation(cosInner + 1e-3F, cone) == 1.0F);                        // inside

        // THE SQUARE, not the blend: at the cosine midpoint t is 0.5 and the result is 0.25. An
        // UNSQUARED t reads 0.5 here. Measured worst deviation across the ten pairs: 2.74e-06.
        const float midpoint = rd::spotConeAttenuation((cosInner + cosOuter) * 0.5F, cone);
        CAPTURE(midpoint);
        CHECK(std::fabs(midpoint - 0.25F) < 1e-5F);

        // Non-decreasing in cosAngle across the whole domain.
        float previous = -1.0F;
        for (int k = 0; k <= 127; ++k) {
            const float cosAngle = -1.0F + (2.0F * static_cast<float>(k) / 127.0F);
            const float got = rd::spotConeAttenuation(cosAngle, cone);
            CAPTURE(k);
            CHECK(got >= previous);
            previous = got;
        }

        // THE EDGE IS SOFT for every pair here -- none takes the delta floor. This is the arm a
        // swapped inner/outer pair reddens, because a swap folds the pair into a hard edge.
        CHECK(cone.angleScale < 1.0F / rd::SPOT_CONE_MIN_COS_DELTA);
    }
}

TEST_CASE("render light falloff: the degenerate cones are hard edges, never errors (PF10)") {
    SUBCASE("inner == outer, and inner > outer, both take the delta floor") {
        const rd::SpotCone equal = rd::resolveSpotCone(engine::radians(15.0F), engine::radians(15.0F));
        const rd::SpotCone inverted = rd::resolveSpotCone(engine::radians(30.0F), engine::radians(20.0F));
        CHECK(equal.angleScale == 1.0F / rd::SPOT_CONE_MIN_COS_DELTA);
        CHECK(inverted.angleScale == 1.0F / rd::SPOT_CONE_MIN_COS_DELTA);

        // A hard edge NARROWER THAN 4e-4 of cosine, sampled 2e-4 either side and NEVER AT cosOuter,
        // where the FMA residual would decide the answer.
        const float cosOuterEqual = std::cos(engine::radians(15.0F));
        CHECK(rd::spotConeAttenuation(cosOuterEqual + 2e-4F, equal) == 1.0F);
        CHECK(bitsOf(rd::spotConeAttenuation(cosOuterEqual - 2e-4F, equal)) == POSITIVE_ZERO_BITS);

        const float cosOuterInverted = std::cos(engine::radians(20.0F));
        CHECK(rd::spotConeAttenuation(cosOuterInverted + 2e-4F, inverted) == 1.0F);
        CHECK(bitsOf(rd::spotConeAttenuation(cosOuterInverted - 2e-4F, inverted)) == POSITIVE_ZERO_BITS);
    }

    SUBCASE("outer == 0 is a cone with no interior -- dark even on the axis") {
        for (const float inner : {0.0F, 0.2F}) {
            CAPTURE(inner);
            const float cosAngleOnAxis = 1.0F;
            CHECK(bitsOf(rd::spotConeAttenuation(cosAngleOnAxis, rd::resolveSpotCone(inner, 0.0F))) ==
                  POSITIVE_ZERO_BITS);
        }
    }

    SUBCASE("outer == HALF_PI is the hemisphere in front of the light") {
        // engine::HALF_PI and the AERO_RANGE literal 1.5707964f are provably ONE number -- asserted
        // through bitsOf so the component's bound and the constant cannot drift apart silently.
        CHECK(bitsOf(engine::HALF_PI) == bitsOf(1.5707964F));

        const rd::SpotCone hemisphere = rd::resolveSpotCone(engine::radians(20.0F), engine::HALF_PI);
        CHECK(rd::spotConeAttenuation(1.0F, hemisphere) == 1.0F);
        // SAMPLED AT -1e-3, ON PURPOSE: std::cos(HALF_PI) is -4.37e-8 in float, not 0, so at
        // cosAngle == 0 exactly the result is ~2.16e-15 rather than zero. The claim is about the
        // hemisphere BEHIND the light, and -1e-3 is unambiguously behind it.
        CHECK(bitsOf(rd::spotConeAttenuation(-1e-3F, hemisphere)) == POSITIVE_ZERO_BITS);
    }
}

TEST_CASE("render light falloff: a non-finite cone angle switches the light off, never on (PF11)") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // THE NaN-INNER ARM IS THE ONE THAT CARRIES THE CLAIM. Without the .cpp's explicit finiteness
    // arm the NaN cosine DIFFERENCE takes SPOT_CONE_MIN_COS_DELTA and produces a perfectly finite
    // hard-edged cone at `outer` reading 1.0 on the axis -- a plausible wrong picture, not a NaN.
    // The NaN-OUTER arm below yields {NaN, NaN} without the arm and reaches 0 through saturate
    // anyway, so it would stay green: it is NOT the witness, and this comment says so.
    struct Pair {
        float inner;
        float outer;
    };
    const std::array<Pair, 4> pairs{{{nan, 0.5F}, {0.3F, nan}, {inf, 0.5F}, {0.3F, -inf}}};
    for (const Pair& pair : pairs) {
        CAPTURE(pair.inner);
        CAPTURE(pair.outer);
        const rd::SpotCone cone = rd::resolveSpotCone(pair.inner, pair.outer);
        CHECK(cone == rd::SpotCone{0.0F, 0.0F});
        CHECK(std::isfinite(cone.angleScale));
        CHECK(std::isfinite(cone.angleOffset));
        for (const float cosAngle : {1.0F, 0.9F, 0.5F, 0.0F, -0.5F, -1.0F, nan}) {
            CAPTURE(cosAngle);
            CHECK(bitsOf(rd::spotConeAttenuation(cosAngle, cone)) == POSITIVE_ZERO_BITS);
        }
    }
}

TEST_CASE("render light falloff: SpotLightData's defaults and RenderView's spot span (PF12)") {
    const rd::SpotLightData data{};
    CHECK(data.position.x == 0.0F);
    CHECK(data.position.y == 0.0F);
    CHECK(data.position.z == 0.0F);
    // -Z rather than zero, so a hand-built view that never assigns it aims somewhere instead of
    // handing the shader a zero vector to normalise.
    CHECK(data.direction.x == 0.0F);
    CHECK(data.direction.y == 0.0F);
    CHECK(data.direction.z == -1.0F);
    CHECK(data.color.x == 1.0F);
    CHECK(data.color.y == 1.0F);
    CHECK(data.color.z == 1.0F);
    CHECK(data.intensity == 1.0F);
    CHECK(data.range == 10.0F);
    CHECK(data.innerConeRadians == engine::radians(20.0F));
    CHECK(data.outerConeRadians == engine::radians(30.0F));
    // ...and BY NUMBER, so a swapped inner/outer pair reddens here and not only in the witness.
    CHECK(std::fabs(data.innerConeRadians - 0.34906585F) < 1e-7F);
    CHECK(std::fabs(data.outerConeRadians - 0.52359878F) < 1e-7F);

    const rd::RenderView view{};
    CHECK(view.spots.empty());
    CHECK(view.spotsTruncated == false);
    // The RenderViewScratch arm is NOT here -- that type is engine::scene_render's and this TU
    // includes only the render umbrella. It lives in scene_render_test.cpp's witness case.

    // The two budgets are equal TODAY. Stated so that making them differ is a deliberate act.
    CHECK(rd::MAX_SPOT_LIGHTS == rd::MAX_POINT_LIGHTS);
}

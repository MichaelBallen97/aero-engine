// tests/render_environment_test.cpp — task E.2.1: the pure environment vocabulary, entirely without
// a GPU. EVERY CASE HERE IS UNGATED. There is no device, no window, no shader toolchain and no #if
// of any kind in this file -- modes, clamps, resolvers and the two curves are arithmetic, so
// everything about them is assertable without one, which is the point of putting them in
// engine/render rather than in the editor.
//
// <aero/render/environment.hpp> IS DELIBERATELY NOT INCLUDED, and neither is
// engine/render/src/sky_pack.hpp. HE1's whole claim is that the UMBRELLA carries the vocabulary, and
// with the direct include present that case passes on a seeded umbrella and proves nothing (the
// GR1 / DD23 pattern). The packer cases live in render_sky_test.cpp for exactly the same reason: its
// relative include of the src-private packer header would pull environment.hpp in transitively and
// make HE1 vacuous here.
//
// WHY EVERY VECTOR ASSERTION IS PER CHANNEL. doctest stringifies through an UNQUALIFIED `os << v`,
// which reaches engine::Vec3 only by ADL -- and ADL's associated namespace for engine::Vec3 is
// `engine`, never this file's anonymous namespace. A streamer declared here would therefore compile,
// never be found, and every Vec3 assertion would still print `{?} == {?}` on the one run that
// matters. Asserting `.x`, `.y` and `.z` separately prints both sides as floats instead, which is
// what the tree's other Vec3 cases (GR2 and the render_material battery) already do.
#include <aero/render/render.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <numbers>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using engine::Vec3;
namespace rd = engine::render;

// The umbrella header's own source, reached through AERO_SHADERS_SRC_DIR -- the ONE route into the
// source tree aero_tests already has (tests/CMakeLists.txt:147), defined UNCONDITIONALLY rather than
// under the AERO_SHADER_TOOLS gate, which is what lets a source-text pin ride the shader-tools-OFF
// configuration exactly as TM29, JP14 and GR1 do.
constexpr std::string_view RENDER_UMBRELLA_PATH =
    AERO_SHADERS_SRC_DIR "/../engine/render/include/aero/render/render.hpp";

// COMMENT-STRIPPED, so a token that appears only in a comment cannot satisfy a source-text arm --
// and it is what makes HE1 refuse a COMMENTED-OUT include, which a compile cannot distinguish from
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

// task E.2.1 (HE16): "exactly once", not merely "present". A second copy of a transcribed constant
// is the same defect as a missing one -- a reader who edits the first and not the second leaves the
// two lanes disagreeing -- and `contains` alone cannot tell the two apart.
[[nodiscard]] std::size_t countOccurrences(const std::string& haystack, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

// The float's bit pattern, for the arms that say BIT-exact rather than equal: `==` alone cannot tell
// +0.0 from -0.0, and "Solid reproduces a clear bit for bit" is precisely a claim about bits.
[[nodiscard]] std::uint32_t bitsOf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// A ULP DISTANCE, not an epsilon. Two SAME-SIGN FINITE floats differ by N ulps iff their bit
// patterns, read as uint32, differ by N -- which is the only form in which HE9's and HE12's bounds
// mean anything, because those quantities are DERIVED (three roundings separate mid +/- halfDelta
// from an endpoint) rather than exact. Every value it is called with here is finite and positive.
// A doctest::Approx with epsilon(0.0) would NOT do instead: its comparison is `< 0`, so it NEVER
// matches, and it prints `1 == 1` on failure.
[[nodiscard]] std::uint32_t ulpDistance(float a, float b) {
    const std::uint32_t ba = bitsOf(a);
    const std::uint32_t bb = bitsOf(b);
    return ba >= bb ? ba - bb : bb - ba;
}

// Seeded directions over the whole sphere. THE FLOATS ARE DERIVED ARITHMETICALLY FROM std::mt19937's
// RAW OUTPUT -- std::uniform_real_distribution is NOT portable across standard libraries (E.1.5's
// rule), so otherwise the three CI lanes would sample three different thousands and "1000 seeded"
// would mean nothing. The 24-bit shift and the 2^-24 scale are both exact, so every lane draws the
// same floats from the same seed.
// Unit only to within the sqrt's rounding, and NOTHING here depends on that: the two cases that use
// this set are about a ZERO delta, which is exact for any direction whatsoever.
[[nodiscard]] std::vector<Vec3> seededUnitDirections(std::uint32_t seed, std::size_t count) {
    std::mt19937 rng{seed};
    std::vector<Vec3> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float u = static_cast<float>(rng() >> 8U) * (1.0F / 16777216.0F);  // [0, 1)
        const float v = static_cast<float>(rng() >> 8U) * (1.0F / 16777216.0F);  // [0, 1)
        const float y = (2.0F * u) - 1.0F;                                       // [-1, 1)
        const float radius = std::sqrt(1.0F - (y * y) > 0.0F ? 1.0F - (y * y) : 0.0F);
        const float phi = 2.0F * std::numbers::pi_v<float> * v;
        out.push_back(Vec3{radius * std::cos(phi), y, radius * std::sin(phi)});
    }
    return out;
}

// wSky and wGround recovered THROUGH THE PUBLIC EVALUATOR rather than restated here: with horizon
// zero and one unit delta, skyRadiance returns that weight EXACTLY on every channel
// (0 + 1*w + 0*w' == w). Asserting the curve's shape through the function that ships it is the
// EFFECT; a second copy of `1 - pow(1 - t, k)` in this file would be the INTENTION.
[[nodiscard]] float unitCircleX(float y) {
    const float squared = 1.0F - (y * y);
    return std::sqrt(squared > 0.0F ? squared : 0.0F);
}

[[nodiscard]] float skyWeightAt(float y) {
    const rd::SkyGradient probe{.horizon = Vec3{}, .skyDelta = Vec3::one(), .groundDelta = Vec3{}};
    return rd::skyRadiance(probe, Vec3{unitCircleX(y), y, 0.0F}).x;
}

[[nodiscard]] float groundWeightAt(float below) {
    const rd::SkyGradient probe{.horizon = Vec3{}, .skyDelta = Vec3{}, .groundDelta = Vec3::one()};
    return rd::skyRadiance(probe, Vec3{unitCircleX(below), -below, 0.0F}).x;
}

// task E.2.1 (HE17): "does this type have a member named `ambient`?", answered at COMPILE time.
// A requires-expression only SUBSTITUTES -- and therefore only yields `false` instead of a hard
// error -- when the type it names is DEPENDENT. Written inline as `requires(rd::RenderView v) {
// v.ambient; }` it is a hard compile error ("no member named 'ambient' in
// 'engine::render::RenderView'"), MEASURED, because a member access on a non-dependent type is not
// in the immediate context of any substitution. Hence the concept, and hence the positive control
// beside HE17's assertion: a concept that detects NOTHING would make that assertion vacuously true.
template <typename T>
concept HasAmbientMember = requires(T v) { v.ambient; };

struct AmbientProbe {
    int ambient = 0;
};

}  // namespace

TEST_CASE("render environment: the umbrella header carries environment.hpp (HE1)") {
    // TWO ARMS, AND THE SOURCE-TEXT ONE IS THE ONLY COVER -- the naming arm below is ALREADY
    // vacuous, and it was already vacuous the day it was written. It is a compile failure only while
    // nothing else this TU includes reaches environment.hpp, and lighting.hpp includes it directly
    // (it needs EnvironmentData for RenderView::environment, which is legitimate and must NOT be
    // undone) while render.hpp includes lighting.hpp -- as it also does forward_renderer.hpp,
    // shadow.hpp and sky_pass.hpp, each of which reaches lighting.hpp in turn. Delete render.hpp's
    // OWN environment include and all ten declarations below still compile. That is exactly the way
    // a naming arm weakens -- silently, from a sibling header, with no test moving -- and it is why
    // the source-text arm carries the claim: it reads render.hpp's own bytes, it does not weaken
    // that way, and it additionally refuses a COMMENTED-OUT include, which no compile can
    // distinguish from a present one. The naming arm stays as a smoke test of the vocabulary's
    // spelling, which is all it can be.
    const std::string umbrella = strippedSourceAt(RENDER_UMBRELLA_PATH);
    REQUIRE_FALSE(umbrella.empty());  // non-vacuity: the path resolved and the file was read
    CHECK(contains(umbrella, "#include <aero/render/environment.hpp>"));
    // ...and the search can say NO, so a reader that matched everything could not fake the line above.
    CHECK_FALSE(contains(umbrella, "#include <aero/render/does_not_exist.hpp>"));

    // The naming arm: <aero/render/render.hpp> alone provides the whole vocabulary -- TRANSITIVELY,
    // which is the whole of what it can say. It still catches a RENAME or a signature change.
    [[maybe_unused]] const rd::EnvironmentData environment{};
    [[maybe_unused]] const rd::SkyGradient gradient{};
    [[maybe_unused]] const rd::HemisphereAmbient ambient{};
    [[maybe_unused]] const rd::BackgroundMode background = rd::BackgroundMode::Sky;
    [[maybe_unused]] const rd::AmbientMode ambientMode = rd::AmbientMode::Hemisphere;
    [[maybe_unused]] const rd::SkyGradient resolvedGradient = rd::resolveSkyGradient(environment);
    [[maybe_unused]] const rd::HemisphereAmbient resolvedAmbient = rd::resolveAmbient(environment);
    [[maybe_unused]] const Vec3 radiance = rd::skyRadiance(gradient, Vec3::unitY());
    [[maybe_unused]] const Vec3 irradiance = rd::ambientIrradiance(ambient, Vec3::unitY());
    CHECK(rd::BACKGROUND_MODE_COUNT > 0U);
}

TEST_CASE("render environment: EnvironmentData's defaults are exactly the documented eight (HE2)") {
    const rd::EnvironmentData env{};
    CHECK((env.backgroundMode == rd::BackgroundMode::Sky));
    CHECK(env.skyColor.x == 0.16F);
    CHECK(env.skyColor.y == 0.26F);
    CHECK(env.skyColor.z == 0.48F);
    CHECK(env.horizonColor.x == 0.52F);
    CHECK(env.horizonColor.y == 0.58F);
    CHECK(env.horizonColor.z == 0.68F);
    CHECK(env.groundColor.x == 0.10F);
    CHECK(env.groundColor.y == 0.09F);
    CHECK(env.groundColor.z == 0.085F);
    CHECK(env.solidColor.x == 0.06F);
    CHECK(env.solidColor.y == 0.06F);
    CHECK(env.solidColor.z == 0.07F);
    CHECK((env.ambientMode == rd::AmbientMode::Hemisphere));
    CHECK(env.ambientColor.x == 0.03F);
    CHECK(env.ambientColor.y == 0.03F);
    CHECK(env.ambientColor.z == 0.03F);
    CHECK(env.ambientIntensity == 0.5F);

    // The default-constructed value equals itself field for field -- the defaulted operator== is a
    // real comparison and not a stub, and one changed field breaks it.
    CHECK(env == rd::EnvironmentData{});
    rd::EnvironmentData moved = env;
    moved.ambientIntensity = 0.25F;
    CHECK_FALSE(moved == env);

    // The selectors are a WIRE FORMAT: engine::Environment stores them as uint32 and docs/09's
    // payload names the integers, so the enumerator values are pinned rather than incidental.
    CHECK(rd::BACKGROUND_MODE_COUNT == 2U);
    CHECK(rd::AMBIENT_MODE_COUNT == 2U);
    CHECK(static_cast<std::uint32_t>(rd::BackgroundMode::Sky) == 0U);
    CHECK(static_cast<std::uint32_t>(rd::BackgroundMode::Solid) == 1U);
    CHECK(static_cast<std::uint32_t>(rd::AmbientMode::Hemisphere) == 0U);
    CHECK(static_cast<std::uint32_t>(rd::AmbientMode::Flat) == 1U);
}

TEST_CASE("render environment: both clamps are total and land on the default enumerator (HE3)") {
    // clampPrimitive's rule verbatim: anything at or past Count is the DEFAULT, not the last valid
    // enumerator and not an error. Double parentheses on every scoped-enum comparison (the doctest
    // decomposition rule), plus the raw integer beside it so a failure prints numbers rather than
    // `CHECK( true )`.
    CHECK((rd::clampBackgroundMode(0U) == rd::BackgroundMode::Sky));
    CHECK((rd::clampBackgroundMode(1U) == rd::BackgroundMode::Solid));
    CHECK((rd::clampBackgroundMode(2U) == rd::BackgroundMode::Sky));
    CHECK((rd::clampBackgroundMode(7U) == rd::BackgroundMode::Sky));
    CHECK((rd::clampBackgroundMode(std::numeric_limits<std::uint32_t>::max()) == rd::BackgroundMode::Sky));
    CHECK(static_cast<std::uint32_t>(rd::clampBackgroundMode(0U)) == 0U);
    CHECK(static_cast<std::uint32_t>(rd::clampBackgroundMode(1U)) == 1U);  // NOT a constant function
    CHECK(static_cast<std::uint32_t>(rd::clampBackgroundMode(2U)) == 0U);
    CHECK(static_cast<std::uint32_t>(rd::clampBackgroundMode(std::numeric_limits<std::uint32_t>::max())) == 0U);

    CHECK((rd::clampAmbientMode(0U) == rd::AmbientMode::Hemisphere));
    CHECK((rd::clampAmbientMode(1U) == rd::AmbientMode::Flat));
    CHECK((rd::clampAmbientMode(2U) == rd::AmbientMode::Hemisphere));
    CHECK((rd::clampAmbientMode(7U) == rd::AmbientMode::Hemisphere));
    CHECK((rd::clampAmbientMode(std::numeric_limits<std::uint32_t>::max()) == rd::AmbientMode::Hemisphere));
    CHECK(static_cast<std::uint32_t>(rd::clampAmbientMode(0U)) == 0U);
    CHECK(static_cast<std::uint32_t>(rd::clampAmbientMode(1U)) == 1U);  // NOT a constant function
    CHECK(static_cast<std::uint32_t>(rd::clampAmbientMode(2U)) == 0U);
    CHECK(static_cast<std::uint32_t>(rd::clampAmbientMode(std::numeric_limits<std::uint32_t>::max())) == 0U);
}

TEST_CASE("render environment: Sky mode resolves to the horizon and two exact differences (HE4)") {
    // DYADIC colours, so every expected value below is a LITERAL rather than a second evaluation of
    // the formula under test -- E.1.2's GR8 lesson: both sides from one source asserts nothing.
    rd::EnvironmentData env{};
    env.backgroundMode = rd::BackgroundMode::Sky;
    env.horizonColor = Vec3{0.25F, 0.125F, 0.0625F};
    env.skyColor = Vec3{0.75F, 0.375F, 0.1875F};
    env.groundColor = Vec3{0.125F, 0.0625F, 0.03125F};

    const rd::SkyGradient g = rd::resolveSkyGradient(env);
    CHECK(g.horizon.x == 0.25F);
    CHECK(g.horizon.y == 0.125F);
    CHECK(g.horizon.z == 0.0625F);
    CHECK(g.skyDelta.x == 0.5F);
    CHECK(g.skyDelta.y == 0.25F);
    CHECK(g.skyDelta.z == 0.125F);
    CHECK(g.groundDelta.x == -0.125F);
    CHECK(g.groundDelta.y == -0.0625F);
    CHECK(g.groundDelta.z == -0.03125F);
}

TEST_CASE("render environment: Solid mode resolves to two EXACTLY zero deltas (HE5)") {
    // The three gradient colours are MUTUALLY DISTINCT GARBAGE. That is the anti-vacuity: it is what
    // proves Solid mode does not read them, and it is why the delta arms below are a statement about
    // the resolver rather than about the fixture happening to be zero.
    rd::EnvironmentData env{};
    env.backgroundMode = rd::BackgroundMode::Solid;
    env.solidColor = Vec3{0.4F, 0.2F, 0.1F};
    env.skyColor = Vec3{7.0F, 8.0F, 9.0F};
    env.horizonColor = Vec3{-3.0F, -4.0F, -5.0F};
    env.groundColor = Vec3{123.0F, 456.0F, 789.0F};

    const rd::SkyGradient g = rd::resolveSkyGradient(env);
    CHECK(g.horizon.x == 0.4F);
    CHECK(g.horizon.y == 0.2F);
    CHECK(g.horizon.z == 0.1F);
    // BIT-exact, not merely equal: "Solid reproduces a clear bit for bit" is a claim about bits, and
    // `== 0.0F` cannot tell +0.0 from -0.0. No channel of this fixture is -0.0 and none may become
    // one -- `x + (-0.0) * w` is not `x` when x is itself a zero.
    CHECK(bitsOf(g.skyDelta.x) == bitsOf(0.0F));
    CHECK(bitsOf(g.skyDelta.y) == bitsOf(0.0F));
    CHECK(bitsOf(g.skyDelta.z) == bitsOf(0.0F));
    CHECK(bitsOf(g.groundDelta.x) == bitsOf(0.0F));
    CHECK(bitsOf(g.groundDelta.y) == bitsOf(0.0F));
    CHECK(bitsOf(g.groundDelta.z) == bitsOf(0.0F));
}

TEST_CASE("render environment: Hemisphere ambient is the exact mid and half-difference (HE6)") {
    // Dyadic again, so mid and halfDelta below are LITERALS. sky = {0.5, 0.25, 0.125},
    // ground = {0.25, 0.125, 0.0625}, I = 0.5:
    //   mid       = (sky + ground) * 0.5 * I = {0.75, 0.375, 0.1875} * 0.25 = {0.1875, 0.09375, 0.046875}
    //   halfDelta = (sky - ground) * 0.5 * I = {0.25, 0.125, 0.0625} * 0.25 = {0.0625, 0.03125, 0.015625}
    // Every step is exact in binary32, so the ordering of the two multiplications cannot matter.
    rd::EnvironmentData env{};
    env.ambientMode = rd::AmbientMode::Hemisphere;
    env.skyColor = Vec3{0.5F, 0.25F, 0.125F};
    env.groundColor = Vec3{0.25F, 0.125F, 0.0625F};
    env.ambientIntensity = 0.5F;
    // GARBAGE in the two fields Hemisphere mode must not read -- the anti-vacuity.
    env.horizonColor = Vec3{99.0F, -99.0F, 42.0F};
    env.ambientColor = Vec3{13.0F, 17.0F, 19.0F};

    const rd::HemisphereAmbient a = rd::resolveAmbient(env);
    CHECK(a.mid.x == 0.1875F);
    CHECK(a.mid.y == 0.09375F);
    CHECK(a.mid.z == 0.046875F);
    CHECK(a.halfDelta.x == 0.0625F);
    CHECK(a.halfDelta.y == 0.03125F);
    CHECK(a.halfDelta.z == 0.015625F);
}

TEST_CASE("render environment: Flat ambient is the scaled colour and an EXACTLY zero delta (HE7)") {
    // ambientColor = {0.5, 0.25, 0.125} at I = 0.25 -> {0.125, 0.0625, 0.03125}, all dyadic.
    rd::EnvironmentData env{};
    env.ambientMode = rd::AmbientMode::Flat;
    env.ambientColor = Vec3{0.5F, 0.25F, 0.125F};
    env.ambientIntensity = 0.25F;
    // GARBAGE in the three fields Flat mode must not read.
    env.skyColor = Vec3{7.0F, 8.0F, 9.0F};
    env.groundColor = Vec3{123.0F, 456.0F, 789.0F};
    env.horizonColor = Vec3{-3.0F, -4.0F, -5.0F};

    const rd::HemisphereAmbient a = rd::resolveAmbient(env);
    CHECK(a.mid.x == 0.125F);
    CHECK(a.mid.y == 0.0625F);
    CHECK(a.mid.z == 0.03125F);
    CHECK(bitsOf(a.halfDelta.x) == bitsOf(0.0F));
    CHECK(bitsOf(a.halfDelta.y) == bitsOf(0.0F));
    CHECK(bitsOf(a.halfDelta.z) == bitsOf(0.0F));
}

TEST_CASE("render environment: a Flat ambient is bit-exactly mid on every normal (HE8)") {
    // THE WHOLE POINT OF THE DELTA PACKING, in assertable form: `mid + 0 * N.y` is EXACTLY mid, so a
    // Flat ambient reproduces today's constant byte for byte on every backend. RELAXING THIS TO
    // Approx MAKES SABOTAGE SEED 1 PASS -- the seed that replaces the scaled delta with a lerp,
    // which agrees to within an ulp and is a DIFFERENT function.
    rd::EnvironmentData env{};
    env.ambientMode = rd::AmbientMode::Flat;
    env.ambientColor = Vec3{0.31F, 0.47F, 0.83F};
    env.ambientIntensity = 0.73F;
    const rd::HemisphereAmbient flat = rd::resolveAmbient(env);

    const std::vector<Vec3> normals = seededUnitDirections(20260905U, 1000);
    REQUIRE(normals.size() == 1000);
    float lowest = 1.0F;
    float highest = -1.0F;
    std::size_t exact = 0;
    for (const Vec3 n : normals) {
        lowest = n.y < lowest ? n.y : lowest;
        highest = n.y > highest ? n.y : highest;
        const Vec3 got = rd::ambientIrradiance(flat, n);
        if (bitsOf(got.x) == bitsOf(flat.mid.x) && bitsOf(got.y) == bitsOf(flat.mid.y) &&
            bitsOf(got.z) == bitsOf(flat.mid.z)) {
            ++exact;
        }
    }
    CHECK(exact == 1000);
    // ANTI-VACUITY: the sample set really spans both hemispheres, so `N.y` is not quietly zero
    // throughout -- a set clustered at the horizon would make the claim above true for free.
    CHECK(lowest < -0.5F);
    CHECK(highest > 0.5F);
}

TEST_CASE("render environment: the hemisphere's endpoints and its monotonicity in N.y (HE9)") {
    // NON-dyadic colours on purpose: the endpoint arms below are about ROUNDING, and a fixture whose
    // arithmetic happens to be exact would make them vacuous.
    rd::EnvironmentData env{};
    env.ambientMode = rd::AmbientMode::Hemisphere;
    env.skyColor = Vec3{0.6F, 0.5F, 0.4F};
    env.groundColor = Vec3{0.1F, 0.08F, 0.05F};
    env.ambientIntensity = 0.7F;
    const rd::HemisphereAmbient a = rd::resolveAmbient(env);

    // N.y == 0 -> EXACTLY mid: halfDelta * 0 is zero and x + 0 is x, on every implementation.
    const Vec3 atHorizon = rd::ambientIrradiance(a, Vec3{1.0F, 0.0F, 0.0F});
    CHECK(bitsOf(atHorizon.x) == bitsOf(a.mid.x));
    CHECK(bitsOf(atHorizon.y) == bitsOf(a.mid.y));
    CHECK(bitsOf(atHorizon.z) == bitsOf(a.mid.z));

    // THREE ROUNDINGS separate mid + halfDelta from sky * I: the sum, the halved difference and the
    // intensity scale are each rounded, and the two are the same real number, not the same float.
    // The bound is stated in ULPS -- never widened to an Approx -- and it is DERIVED rather than
    // copied off this lane's run, because a bound pinned to one lane's rounding is a cross-lane
    // flake. mid and halfDelta live around 0.245 and 0.175 (ulp 2^-26) while the nadir they produce
    // is 0.07 (ulp 2^-27), so two roundings each side plus the difference's own can reach ~10 ulps
    // AT THE ENDPOINT'S SCALE, and ambientIrradiance's `mid + halfDelta * y` is an FMA-contraction
    // site that moves it again. 16 covers that; MEASURED WORST HERE IS 3 (nadir, channel x), and
    // even 16 ulps is a relative 1e-6 -- orders of magnitude below any structural defect this case
    // could be wrong about.
    constexpr std::uint32_t ENDPOINT_ULPS = 16;
    const Vec3 zenith = rd::ambientIrradiance(a, Vec3{0.0F, 1.0F, 0.0F});
    CHECK(ulpDistance(zenith.x, 0.6F * 0.7F) <= ENDPOINT_ULPS);
    CHECK(ulpDistance(zenith.y, 0.5F * 0.7F) <= ENDPOINT_ULPS);
    CHECK(ulpDistance(zenith.z, 0.4F * 0.7F) <= ENDPOINT_ULPS);
    const Vec3 nadir = rd::ambientIrradiance(a, Vec3{0.0F, -1.0F, 0.0F});
    CHECK(ulpDistance(nadir.x, 0.1F * 0.7F) <= ENDPOINT_ULPS);
    CHECK(ulpDistance(nadir.y, 0.08F * 0.7F) <= ENDPOINT_ULPS);
    CHECK(ulpDistance(nadir.z, 0.05F * 0.7F) <= ENDPOINT_ULPS);

    // Monotone in N.y on every channel, because sky > ground on every channel.
    std::size_t nonDecreasing = 0;
    Vec3 previous = rd::ambientIrradiance(a, Vec3{0.0F, -1.0F, 0.0F});
    for (int i = 1; i < 64; ++i) {
        const float y = -1.0F + (2.0F * static_cast<float>(i) / 63.0F);
        const Vec3 current = rd::ambientIrradiance(a, Vec3{0.0F, y, 0.0F});
        if (current.x >= previous.x && current.y >= previous.y && current.z >= previous.z) {
            ++nonDecreasing;
        }
        previous = current;
    }
    CHECK(nonDecreasing == 63);
    // ...and it really RISES: a constant would satisfy every step above.
    CHECK(zenith.x > nadir.x);
    CHECK(zenith.y > nadir.y);
    CHECK(zenith.z > nadir.z);
}

TEST_CASE("render environment: a Solid sky is bit-exactly its colour in every direction (HE10)") {
    // The background half of HE8's claim: `horizon + 0 * wSky + 0 * wGround` is EXACTLY horizon, so
    // Solid mode reproduces today's CLEAR byte for byte whatever the weights evaluate to. No channel
    // of this fixture is -0.0, which is the one value the delta rule does not cover (a -0.0 channel
    // would come back +0.0 -- numerically equal, bit-different).
    rd::EnvironmentData env{};
    env.backgroundMode = rd::BackgroundMode::Solid;
    env.solidColor = Vec3{0.06F, 0.06F, 0.07F};
    env.skyColor = Vec3{7.0F, 8.0F, 9.0F};
    env.horizonColor = Vec3{-3.0F, -4.0F, -5.0F};
    env.groundColor = Vec3{123.0F, 456.0F, 789.0F};
    const rd::SkyGradient solid = rd::resolveSkyGradient(env);

    const std::vector<Vec3> directions = seededUnitDirections(1957U, 1000);
    REQUIRE(directions.size() == 1000);
    float lowest = 1.0F;
    float highest = -1.0F;
    std::size_t exact = 0;
    for (const Vec3 d : directions) {
        lowest = d.y < lowest ? d.y : lowest;
        highest = d.y > highest ? d.y : highest;
        const Vec3 got = rd::skyRadiance(solid, d);
        if (bitsOf(got.x) == bitsOf(0.06F) && bitsOf(got.y) == bitsOf(0.06F) && bitsOf(got.z) == bitsOf(0.07F)) {
            ++exact;
        }
    }
    CHECK(exact == 1000);
    // ANTI-VACUITY: both weights genuinely vary over this set -- the samples reach both poles.
    CHECK(lowest < -0.5F);
    CHECK(highest > 0.5F);
}

TEST_CASE("render environment: the two halves meet at the horizon, continuously (HE11)") {
    // sky and ground STRADDLE horizon on channel x (0.8 > 0.5 > 0.2) and straddle it the other way
    // on channel y, which is what makes the two-sided arm below a statement about direction rather
    // than about magnitude.
    rd::EnvironmentData env{};
    env.backgroundMode = rd::BackgroundMode::Sky;
    env.horizonColor = Vec3{0.5F, 0.5F, 0.5F};
    env.skyColor = Vec3{0.8F, 0.2F, 0.5F};
    env.groundColor = Vec3{0.2F, 0.8F, 0.5F};
    const rd::SkyGradient g = rd::resolveSkyGradient(env);

    // EXACTLY horizon on the horizon: saturate(0) is 0, pow(1, k) is 1, so both weights are exactly
    // zero and both deltas contribute exactly nothing.
    const Vec3 onHorizon = rd::skyRadiance(g, Vec3{1.0F, 0.0F, 0.0F});
    CHECK(bitsOf(onHorizon.x) == bitsOf(0.5F));
    CHECK(bitsOf(onHorizon.y) == bitsOf(0.5F));
    CHECK(bitsOf(onHorizon.z) == bitsOf(0.5F));

    // A HAIR above and a hair below. wSky(1e-4) ~= 4e-4 and wGround(1e-4) ~= 8e-4; the largest delta
    // magnitude here is 0.3, so neither side can move further than 2.4e-4 from the horizon colour.
    // The bound is stated at 5e-4 with that derivation, not guessed.
    constexpr float NEAR_HORIZON_BOUND = 5.0e-4F;
    const Vec3 justAbove = rd::skyRadiance(g, Vec3{1.0F, 1.0e-4F, 0.0F});
    const Vec3 justBelow = rd::skyRadiance(g, Vec3{1.0F, -1.0e-4F, 0.0F});
    CHECK(std::fabs(justAbove.x - 0.5F) < NEAR_HORIZON_BOUND);
    CHECK(std::fabs(justBelow.x - 0.5F) < NEAR_HORIZON_BOUND);
    CHECK(std::fabs(justAbove.y - 0.5F) < NEAR_HORIZON_BOUND);
    CHECK(std::fabs(justBelow.y - 0.5F) < NEAR_HORIZON_BOUND);
    // OPPOSITE SIDES, strictly: above the horizon channel x moves toward the sky's 0.8 and channel y
    // toward the sky's 0.2; below, each moves the other way. THAT is what "continuous, not constant"
    // means here -- a function that returned the horizon colour everywhere would pass the bounds
    // above and fails these four.
    CHECK(justAbove.x > 0.5F);
    CHECK(justBelow.x < 0.5F);
    CHECK(justAbove.y < 0.5F);
    CHECK(justBelow.y > 0.5F);
    // The straddled channel z is EQUAL on both endpoints, so it stays at the horizon exactly.
    CHECK(bitsOf(justAbove.z) == bitsOf(0.5F));
    CHECK(bitsOf(justBelow.z) == bitsOf(0.5F));
}

TEST_CASE("render environment: the zenith and the nadir reach their endpoints (HE12)") {
    // THE DEFAULTS, because this case is about the shipped picture's own rounding.
    const rd::EnvironmentData env{};
    const rd::SkyGradient g = rd::resolveSkyGradient(env);

    // EXACT, and exactly this form: saturate(1) is 1, pow(0, k) is 0 for k > 0, so wSky is exactly 1
    // and wGround exactly 0 -- the zenith is horizon + skyDelta, evaluated once.
    const Vec3 zenith = rd::skyRadiance(g, Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 expectedZenith = g.horizon + g.skyDelta;
    CHECK(bitsOf(zenith.x) == bitsOf(expectedZenith.x));
    CHECK(bitsOf(zenith.y) == bitsOf(expectedZenith.y));
    CHECK(bitsOf(zenith.z) == bitsOf(expectedZenith.z));
    // ...and it is NOT bit-equal to skyColor, deliberately: E.1.3's `a + (b - a)` is not `b`. Two
    // roundings separate horizon + (sky - horizon) from sky, so the claim is an ULP BOUND. The
    // inequality itself is NOT asserted -- whether the two roundings happen to cancel is a
    // per-lane accident, and a case that pinned it would be a cross-lane flake.
    // THE BOUND IS DERIVED, not copied off this lane. The worst pairing in the DEFAULTS is the
    // nadir's z: horizon 0.68 and groundDelta -0.595 both carry an ulp of 2^-24 while the 0.085 they
    // produce carries 2^-27, so the difference's own rounding alone is up to 4 ulps AT THE
    // ENDPOINT'S SCALE, plus the sum's half. 8 covers it; MEASURED HERE: zenith 0/0/0, nadir 1/0/3.
    constexpr std::uint32_t ENDPOINT_ULPS = 8;
    CHECK(ulpDistance(zenith.x, env.skyColor.x) <= ENDPOINT_ULPS);
    CHECK(ulpDistance(zenith.y, env.skyColor.y) <= ENDPOINT_ULPS);
    CHECK(ulpDistance(zenith.z, env.skyColor.z) <= ENDPOINT_ULPS);

    const Vec3 nadir = rd::skyRadiance(g, Vec3{0.0F, -1.0F, 0.0F});
    const Vec3 expectedNadir = g.horizon + g.groundDelta;
    CHECK(bitsOf(nadir.x) == bitsOf(expectedNadir.x));
    CHECK(bitsOf(nadir.y) == bitsOf(expectedNadir.y));
    CHECK(bitsOf(nadir.z) == bitsOf(expectedNadir.z));
    CHECK(ulpDistance(nadir.x, env.groundColor.x) <= ENDPOINT_ULPS);
    CHECK(ulpDistance(nadir.y, env.groundColor.y) <= ENDPOINT_ULPS);
    CHECK(ulpDistance(nadir.z, env.groundColor.z) <= ENDPOINT_ULPS);
}

TEST_CASE("render environment: both curves rise monotonically and ground is the steeper (HE13)") {
    // Both weights are read back THROUGH skyRadiance, never recomputed here -- the EFFECT, not the
    // INTENTION. skyWeightAt / groundWeightAt use a zero horizon and a single unit delta, so the
    // value they return IS the weight, exactly.
    CHECK(skyWeightAt(0.0F) == 0.0F);
    CHECK(groundWeightAt(0.0F) == 0.0F);
    CHECK(skyWeightAt(1.0F) == 1.0F);
    CHECK(groundWeightAt(1.0F) == 1.0F);

    std::size_t skyNonDecreasing = 0;
    std::size_t groundNonDecreasing = 0;
    std::size_t groundAtLeastSky = 0;
    float previousSky = skyWeightAt(0.0F);
    float previousGround = groundWeightAt(0.0F);
    for (int i = 1; i < 64; ++i) {
        const float v = static_cast<float>(i) / 63.0F;
        const float currentSky = skyWeightAt(v);
        const float currentGround = groundWeightAt(v);
        if (currentSky >= previousSky) {
            ++skyNonDecreasing;
        }
        if (currentGround >= previousGround) {
            ++groundNonDecreasing;
        }
        if (currentGround >= currentSky) {
            ++groundAtLeastSky;
        }
        previousSky = currentSky;
        previousGround = currentGround;
    }
    CHECK(skyNonDecreasing == 63);
    CHECK(groundNonDecreasing == 63);
    CHECK(groundAtLeastSky == 63);
    // STRICTLY steeper at an interior sample -- without this the ordering arm is satisfied by the
    // same curve twice, which is exactly the mistake a copy-pasted power would make. At v = 0.5 the
    // two are 1 - 0.5^4 = 0.9375 and 1 - 0.5^8 = 0.99609375.
    CHECK(groundWeightAt(0.5F) > skyWeightAt(0.5F));
    CHECK(skyWeightAt(0.5F) == doctest::Approx(0.9375).epsilon(1.0e-5));
    CHECK(groundWeightAt(0.5F) == doctest::Approx(0.99609375).epsilon(1.0e-5));
}

TEST_CASE("render environment: skyRadiance is total, and scale-invariant by a power of two (HE14)") {
    const rd::EnvironmentData env{};
    const rd::SkyGradient g = rd::resolveSkyGradient(env);

    // A ZERO-LENGTH direction yields the horizon, never a NaN: the length guard sends y to 0, so
    // both weights are 0 and both deltas contribute nothing.
    // A DECLARED HOLE, MEASURED RATHER THAN ASSUMED. This arm asserts the RESULT, and the result
    // survives the guard's removal: with `direction.y / len` unguarded, 0/0 is NaN, and the saturate
    // below is a comparison chain that sends a NaN to 0 on BOTH weights -- so the horizon comes back
    // anyway. Verified by deleting the guard and re-running: all 15 cases still pass. What the guard
    // uniquely buys is not raising FE_INVALID, which nothing in this tree observes. Do not read this
    // arm as cover for the guard; it is cover for the CONTRACT.
    const Vec3 fromZero = rd::skyRadiance(g, Vec3{});
    CHECK(bitsOf(fromZero.x) == bitsOf(g.horizon.x));
    CHECK(bitsOf(fromZero.y) == bitsOf(g.horizon.y));
    CHECK(bitsOf(fromZero.z) == bitsOf(g.horizon.z));

    // A NON-FINITE direction likewise. The saturate is a comparison chain rather than std::clamp,
    // which returns NaN for a NaN input on libc++ -- that is what keeps a NaN out of the picture
    // instead of propagating it into a pixel.
    constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
    constexpr float INF_F = std::numeric_limits<float>::infinity();
    for (const Vec3 hostile : {Vec3{NAN_F, 0.0F, 0.0F}, Vec3{0.0F, NAN_F, 0.0F}, Vec3{INF_F, 0.0F, 0.0F},
                               Vec3{0.0F, INF_F, 0.0F}, Vec3{0.0F, -INF_F, 0.0F}}) {
        const Vec3 got = rd::skyRadiance(g, hostile);
        CHECK(bitsOf(got.x) == bitsOf(g.horizon.x));
        CHECK(bitsOf(got.y) == bitsOf(g.horizon.y));
        CHECK(bitsOf(got.z) == bitsOf(g.horizon.z));
    }

    // A POWER-OF-TWO scale commutes with every rounding in the normalise: each squared term scales by
    // 16 exactly, the sqrt by 4 exactly, and y / len is then the identical quotient. A factor of 3
    // would agree only to within an ulp, and THIS CASE DELIBERATELY DOES NOT ASSERT THAT -- the
    // power of two is a choice, stated as one.
    const std::vector<Vec3> directions = seededUnitDirections(4242U, 256);
    std::size_t identical = 0;
    for (const Vec3 d : directions) {
        const Vec3 plain = rd::skyRadiance(g, d);
        const Vec3 scaled = rd::skyRadiance(g, Vec3{4.0F * d.x, 4.0F * d.y, 4.0F * d.z});
        if (bitsOf(plain.x) == bitsOf(scaled.x) && bitsOf(plain.y) == bitsOf(scaled.y) &&
            bitsOf(plain.z) == bitsOf(scaled.z)) {
            ++identical;
        }
    }
    CHECK(identical == 256);
    // ANTI-VACUITY: the set is not all one colour -- the gradient really varies across it, so the
    // agreement above is between two evaluations that both moved.
    CHECK(rd::skyRadiance(g, Vec3{0.0F, 1.0F, 0.0F}).x != rd::skyRadiance(g, Vec3{0.0F, -1.0F, 0.0F}).x);
}

TEST_CASE("render environment: the two curve powers are the numbers the shader transcribes (HE15)") {
    // TUNING CONSTANTS (D8): this pins the VALUES so the HLSL's bare literals have something to be
    // compared against (HE16, task E.2.1 commit 7). Nothing here claims they are the RIGHT numbers.
    CHECK(rd::SKY_CURVE_POWER == 4.0F);
    CHECK(rd::GROUND_CURVE_POWER == 8.0F);
    CHECK(rd::GROUND_CURVE_POWER > rd::SKY_CURVE_POWER);
}

TEST_CASE("render environment: the three shaders transcribe this header, and carry no mode (HE16)") {
    // The TM29 / SO9 shape: the HLSL is a SECOND COPY of arithmetic no case in this file can execute,
    // so the pin reads the source with comments stripped and asserts needles PRESENT *and* needles
    // ABSENT. The absent half is what makes it non-decorative: a `lerp` that happens to agree on
    // this hardware, a mode selector that reintroduces a branch, or an eye-relative ray would each
    // leave every present-needle arm green.
    const std::string skyFrag = strippedSourceAt(AERO_SHADERS_SRC_DIR "/sky.frag.hlsl");
    const std::string skyVert = strippedSourceAt(AERO_SHADERS_SRC_DIR "/sky.vert.hlsl");
    const std::string sceneFrag = strippedSourceAt(AERO_SHADERS_SRC_DIR "/scene.frag.hlsl");
    // ANTI-VACUITY FIRST: a mistyped path yields an empty string from which every "absent" arm below
    // passes and every "present" arm fails in a way that reads as a shader edit.
    REQUIRE(skyFrag.size() > 200);
    REQUIRE(skyVert.size() > 200);
    REQUIRE(sceneFrag.size() > 200);

    SUBCASE("sky.frag.hlsl transcribes the two curve powers, once each") {
        // The bare literals HE15 pins on the C++ side. `4.0` and `8.0` are spelled INSIDE the pow so
        // a stray 4.0 elsewhere in the file cannot satisfy the arm.
        CHECK(countOccurrences(skyFrag, "pow(1.0 - t, 4.0)") == 1);
        CHECK(countOccurrences(skyFrag, "pow(1.0 - b, 8.0)") == 1);
        CHECK(countOccurrences(skyFrag, "uHorizon + uSkyDelta * wSky + uGroundDelta * wGround") == 1);
    }

    SUBCASE("sky.frag.hlsl has NO lerp, NO mode and NO branch (INV-1)") {
        // THE WHOLE REASON THE PIN EXISTS. DXC maps HLSL `lerp` to SPIR-V's FMix, specified as
        // x*(1-a) + y*a -- a different number from x when x == y, which would break the Solid
        // bit-exactness the entire design rests on.
        CHECK_FALSE(contains(skyFrag, "lerp("));
        // The modes live on the CPU. A selector here means a second source for the mode decision and
        // a branch the GPU has to predict; there is no variant key and nothing to keep in sync.
        CHECK_FALSE(contains(skyFrag, "Mode"));
        CHECK_FALSE(contains(skyFrag, "if ("));
    }

    SUBCASE("sky.vert.hlsl builds the ray from far - near, and never from the eye") {
        CHECK(countOccurrences(skyVert, "farP.xyz / farP.w - nearP.xyz / nearP.w") == 1);
        // CameraView::eyePosition is WRONG under a parallel projection (as it is in Unity), so the
        // ray must never be eye-relative. The light block's own uEyePosition is a fragment-stage
        // field of a different cbuffer entirely; naming it here would be the defect.
        CHECK_FALSE(contains(skyVert, "uEyePosition"));
    }

    SUBCASE("scene.frag.hlsl shades with the hemisphere pair and no scaled constant") {
        CHECK(countOccurrences(sceneFrag, "uAmbientMid + uAmbientHalfDelta * N.y") == 1);
        // The pre-E.2.1 term was `uAmbient * <something>`. Its absence is what says the hemisphere
        // REPLACED it rather than being added beside it.
        CHECK_FALSE(contains(sceneFrag, "uAmbient *"));
    }
}

TEST_CASE("render environment: RenderView carries an EnvironmentData and no ambient (HE17)") {
    // The DEFAULT view is the engine's default sky and shade -- which is what makes "a scene without
    // an Environment looks like a scene with a fresh one" a property of the VIEW, not of the bridge.
    const rd::RenderView view;
    CHECK(view.environment == rd::EnvironmentData{});
    CHECK(view.environmentCount == 0U);
    // ...and `ambient` is GONE, asserted at COMPILE time rather than by grep: a requires-expression
    // naming the member is false iff the member is absent, so this static_assert is D9's closure. A
    // field re-added "for compatibility" fails HERE, in every configuration, on every lane.
    static_assert(!HasAmbientMember<rd::RenderView>,
                  "task E.2.1: RenderView::ambient was REMOVED, not deprecated -- a hand-built view "
                  "must not be able to silently take the old constant (D9).");
    // ANTI-VACUITY: the detector really does detect. Without this, a mis-spelled member in the
    // concept above would make the assertion hold for every type in the language.
    static_assert(HasAmbientMember<AmbientProbe>, "the HE17 detector must be able to see a member");
}

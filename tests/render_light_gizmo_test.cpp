// tests/render_light_gizmo_test.cpp — task E.2.3: the three light-gizmo emitters, entirely without a
// GPU.
//
// EVERY CASE HERE IS UNGATED. There is no device, no window, no shader toolchain and NO #if OF ANY
// KIND in this file -- the emitters are pure arithmetic, so everything about them is assertable
// without one, which is the point of putting them in engine/render rather than in the editor. The
// two GPU pixel cases this task ships live in render_debug_draw_test.cpp as DG19/DG20, at the bottom
// of the gate that already carries DG6's harness; that is E.1.2's own precedent, and it is what keeps
// this file free of the sanctioned #if pair.
//
// <aero/render/light_gizmo.hpp> IS DELIBERATELY NOT INCLUDED. GZ1's whole claim is that the UMBRELLA
// carries it, and with both includes present that case passes on a seeded umbrella and proves
// nothing -- the GR1/DD23 pattern, restated because it is the kind of thing that gets "tidied" away.
#include <aero/render/render.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <ostream>  // MSVC: any CHECK on a string_view needs this (the 0.4.1 trap)
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using engine::Vec3;
using engine::Vec4;
namespace rd = engine::render;

constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
constexpr float INF_F = std::numeric_limits<float>::infinity();

// The one epsilon this file uses for a quantity built from cos/sin at unit scale. It is NAMED so an
// assertion carries it rather than hiding it: the emitters reach sin and cos, neither of which
// IEEE-754 requires to be correctly rounded, so a vertex read back off the batch is good to about
// this and no better. Assertions at a larger scale state their own bound relative to it.
constexpr float UNIT_EPS = 1e-5F;

// The umbrella header and the emitter's own source, reached through AERO_SHADERS_SRC_DIR -- the ONE
// route into the source tree aero_tests already has (tests/CMakeLists.txt, target-wide). NOT a new
// compile definition.
constexpr std::string_view RENDER_UMBRELLA_PATH =
    AERO_SHADERS_SRC_DIR "/../engine/render/include/aero/render/render.hpp";
constexpr std::string_view LIGHT_GIZMO_SOURCE_PATH = AERO_SHADERS_SRC_DIR "/../engine/render/src/light_gizmo.cpp";

// COMMENT-STRIPPED, so a token that appears only in a comment cannot satisfy GZ2 -- and this file's
// own subject names the banned functions in the sentence saying they are absent, which is exactly
// the trap this strips away.
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

// A Vec3 as its three RAW bit patterns. BIT equality, not float equality: `==` on floats calls +0.0F
// and -0.0F equal, and GZ7/GZ13's whole claim is that a ray endpoint and a circle vertex are the SAME
// COMPUTATION rather than two that agree by rounding.
//
// It lives HERE, at file scope, because a friend may not be DEFINED inside a local class
// ([class.friend]/6), so a type declared in a TEST_CASE body cannot carry the operator<< below -- and
// without it CHECK(a == b) prints `CHECK( true )` on a FAILURE as well as a pass.
struct Bits3 {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;
    [[nodiscard]] bool operator==(const Bits3&) const = default;
};

std::ostream& operator<<(std::ostream& out, const Bits3& value) {
    out << "bits3(0x" << std::hex << value.x << ", 0x" << value.y << ", 0x" << value.z << ")" << std::dec;
    return out;
}

[[nodiscard]] Bits3 bitsOf(Vec3 v) {
    Bits3 bits{};
    std::memcpy(&bits.x, &v.x, sizeof(float));
    std::memcpy(&bits.y, &v.y, sizeof(float));
    std::memcpy(&bits.z, &v.z, sizeof(float));
    return bits;
}

// A batch big enough that nothing is ever dropped, so a count is the count EMITTED rather than the
// count that happened to fit. GZ16 is the one case that deliberately uses a small one.
[[nodiscard]] rd::DebugDrawBatch roomyBatch() { return rd::DebugDrawBatch{rd::DebugDrawBudget{}}; }

// Every vertex the emitter pushed into `depth`, as a COPY -- so a case can scan it without holding a
// span into a batch that is about to be cleared.
[[nodiscard]] std::vector<rd::DebugLineVertex> verticesOf(const rd::DebugDrawBatch& batch, rd::DebugDepth depth) {
    const std::span<const rd::DebugLineVertex> v = batch.lineVertices(depth);
    return std::vector<rd::DebugLineVertex>{v.begin(), v.end()};
}

// How far along `axis` a point sits from `origin`, and how far off that axis. Both READ OFF the
// emitted vertex, which is what makes "the cap sits at range*cos(outer) with radius range*sin(outer)"
// a statement about the batch rather than about the parameters.
[[nodiscard]] float axialDistance(Vec3 point, Vec3 origin, Vec3 axis) { return engine::dot(point - origin, axis); }

[[nodiscard]] float perpRadius(Vec3 point, Vec3 origin, Vec3 axis) {
    const Vec3 d = point - origin;
    return engine::length(d - (axis * engine::dot(d, axis)));
}

// The number of samples across [cos(outer), 1] whose cone attenuation is STRICTLY between 0 and 1 --
// i.e. how wide the graded band is, measured through the falloff vocabulary itself. A folded cone
// (inner >= outer) has a band 1e-4 wide in cosine, which this grid's step of ~1/64 of the interval
// cannot straddle more than once; a real cone has a band far wider than the step.
[[nodiscard]] int gradedSampleCount(float innerRadians, float outerRadians) {
    const rd::SpotCone cone = rd::resolveSpotCone(innerRadians, outerRadians);
    const float lo = std::cos(outerRadians);
    constexpr int SAMPLES = 64;
    int graded = 0;
    for (int i = 0; i <= SAMPLES; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(SAMPLES);
        const float cosAngle = lo + ((1.0F - lo) * t);
        const float attenuation = rd::spotConeAttenuation(cosAngle, cone);
        if (attenuation > 0.0F && attenuation < 1.0F) {
            ++graded;
        }
    }
    return graded;
}

}  // namespace

TEST_CASE("render light gizmo: the umbrella header carries light_gizmo.hpp (GZ1)") {
    // TWO ARMS, and only one of them is real -- GR1's honesty, restated. The NAMING arm below is NOT
    // a compile failure on its own: this TU includes render.hpp, which includes debug_draw.hpp, and
    // light_gizmo.hpp includes debug_draw.hpp too -- but NOTHING ELSE in aero_tests includes
    // light_gizmo.hpp, so deleting the umbrella's line WOULD break this TU. That makes the naming arm
    // genuine here. The SOURCE-TEXT arm is kept anyway because it also refuses a COMMENTED-OUT
    // include, which a compile cannot distinguish from a missing one.
    const std::string umbrella = strippedSourceAt(RENDER_UMBRELLA_PATH);
    REQUIRE_FALSE(umbrella.empty());  // non-vacuity: the path resolved and the file was read
    CHECK(contains(umbrella, "#include <aero/render/light_gizmo.hpp>"));
    // ...and the search can say NO, so a reader that matched everything could not fake the line above.
    CHECK_FALSE(contains(umbrella, "#include <aero/render/does_not_exist.hpp>"));

    [[maybe_unused]] const rd::LightGizmoStyle style{};
    [[maybe_unused]] const rd::DirectionalGizmoParams directional{};
    [[maybe_unused]] const rd::PointGizmoParams point{};
    [[maybe_unused]] const rd::SpotGizmoParams spot{};
    CHECK(rd::LIGHT_GIZMO_MAX_LINES_PER_ENTITY > 0U);
}

TEST_CASE("render light gizmo: the emitter reaches no rounding libm function but sin/cos/sqrt (GZ2)") {
    // THE HONESTY OF THE HEADER'S OWN LIBM LIST, pinned as source text, plus the STRUCTURAL half of
    // "the spot MIRRORS the cone fold rather than reading it": light_gizmo.cpp must not include the
    // falloff header or call its resolver, because a resolved cone carries no bit saying its delta
    // was floored and there is therefore nothing to carry. GZ8 is what ties the two behaviourally.
    const std::string source = strippedSourceAt(LIGHT_GIZMO_SOURCE_PATH);
    REQUIRE_FALSE(source.empty());  // non-vacuity, first direction: the path resolved

    // (a) the banned rounding functions
    CHECK_FALSE(contains(source, "std::pow"));
    CHECK_FALSE(contains(source, "std::log"));
    CHECK_FALSE(contains(source, "std::exp"));
    CHECK_FALSE(contains(source, "std::tan"));
    CHECK_FALSE(contains(source, "std::atan"));
    CHECK_FALSE(contains(source, "std::asin"));
    CHECK_FALSE(contains(source, "std::acos"));

    // (b) the falloff coupling, in both spellings
    CHECK_FALSE(contains(source, "light_falloff"));
    CHECK_FALSE(contains(source, "resolveSpotCone"));

    // (c) non-vacuity, second direction: the ADMITTED ones ARE present, so a reader that matched
    //     nothing could not fake the nine lines above.
    CHECK(contains(source, "std::cos"));
    CHECK(contains(source, "std::sin"));
    CHECK(contains(source, "std::isfinite"));

    // (d) ...and the search can say NO to something that is genuinely absent.
    CHECK_FALSE(contains(source, "std::this_is_not_a_function"));
}

TEST_CASE("render light gizmo: the style's defaults, the derived cap and both strides (GZ3)") {
    const rd::LightGizmoStyle style{};

    // THE TWO BEHAVIOURAL DEFAULTS, as RELATIONSHIPS. `segments` is the named constant rather than
    // its value, and Overlay is the header's own promise: a light gizmo is meant to be seen through
    // the wall the light is behind.
    CHECK(style.segments == rd::LIGHT_GIZMO_CIRCLE_SEGMENTS);
    CHECK((style.depth == rd::DebugDepth::Overlay));

    // THE TWO TUNING MAGNITUDES ARE BANDS, NOT VALUES (D14): retuning either on the validation page
    // must redden nothing. innerAlphaScale is documented as a multiple of color.w that DIMS.
    CHECK(style.innerAlphaScale > 0.0F);
    CHECK(style.innerAlphaScale < 1.0F);
    CHECK(style.color.w > 0.0F);
    CHECK(style.color.w <= 1.0F);

    SUBCASE("operator== distinguishes every field, so an added one cannot be silently ignored") {
        CHECK(style == rd::LightGizmoStyle{});
        rd::LightGizmoStyle mutated = style;
        mutated.color.x += 0.25F;
        CHECK_FALSE(mutated == style);
        mutated = style;
        mutated.color.w *= 0.5F;
        CHECK_FALSE(mutated == style);
        mutated = style;
        mutated.innerAlphaScale *= 0.5F;
        CHECK_FALSE(mutated == style);
        mutated = style;
        mutated.segments += 1U;
        CHECK_FALSE(mutated == style);
        mutated = style;
        mutated.depth = rd::DebugDepth::Tested;
        CHECK_FALSE(mutated == style);
    }

    SUBCASE("the derived per-entity cap, and the two strides, as RUNTIME checks") {
        // The static_asserts in the header are compile-time; these fail LOUDLY at run time instead of
        // silently changing which vertices the rays land on if a retune ever breaks divisibility.
        CHECK(rd::LIGHT_GIZMO_MAX_LINES_PER_ENTITY == 3U * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS);
        CHECK(rd::LIGHT_GIZMO_CIRCLE_SEGMENTS % rd::LIGHT_GIZMO_SPOT_RIM_RAYS == 0U);
        CHECK(rd::LIGHT_GIZMO_CIRCLE_SEGMENTS % rd::LIGHT_GIZMO_DIRECTIONAL_RAYS == 0U);
        CHECK(rd::LIGHT_GIZMO_CIRCLE_SEGMENTS >= rd::MIN_CIRCLE_SEGMENTS);
        CHECK(rd::LIGHT_GIZMO_CIRCLE_SEGMENTS <= rd::MAX_CIRCLE_SEGMENTS);
    }
}

TEST_CASE("render light gizmo: a point light is a sphere of three circles at `range` (GZ4)") {
    rd::DebugDrawBatch batch = roomyBatch();
    const Vec3 center{2.0F, -1.0F, 0.5F};
    constexpr float RANGE = 7.0F;
    const std::uint32_t emitted = rd::emitPointLightGizmo(batch, {.center = center, .range = RANGE});

    CHECK(emitted == 3U * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS);
    CHECK(batch.lineCount() == emitted);
    CHECK(batch.rejectedLines() == 0U);
    CHECK(batch.droppedLines() == 0U);
    // The STYLE's bucket, and only it -- the default is Overlay.
    CHECK(batch.lineCount(rd::DebugDepth::Overlay) == emitted);
    CHECK(batch.lineCount(rd::DebugDepth::Tested) == 0U);

    // EVERY vertex is on the sphere, read off the batch rather than recomputed from the parameters.
    // 1e-4 at radius 7 is 1.4e-5 relative -- three chained cos/sin plus a translate, DD15's own bound.
    constexpr float EPS = 1e-4F;
    for (const rd::DebugLineVertex& v : verticesOf(batch, rd::DebugDepth::Overlay)) {
        CHECK(std::abs(engine::length(v.position - center) - RANGE) <= EPS);
    }
}

TEST_CASE("render light gizmo: a point light with a bad range or centre emits NOTHING (GZ5)") {
    // "Emitted nothing" and "rejected one" are DIFFERENT claims, and a wireCircle reached with a bad
    // radius would satisfy only the first. Both are asserted on every arm.
    const std::array<float, 4> badRanges{{0.0F, -1.0F, NAN_F, INF_F}};
    for (const float range : badRanges) {
        CAPTURE(range);
        rd::DebugDrawBatch batch = roomyBatch();
        CHECK(rd::emitPointLightGizmo(batch, {.center = Vec3{1.0F, 2.0F, 3.0F}, .range = range}) == 0U);
        CHECK(batch.lineCount() == 0U);
        CHECK(batch.rejectedLines() == 0U);
        CHECK(batch.droppedLines() == 0U);
        CHECK(batch.empty());
    }

    SUBCASE("...and a non-finite centre, with a range that is perfectly good") {
        const std::array<Vec3, 3> badCentres{{
            Vec3{NAN_F, 0.0F, 0.0F},
            Vec3{0.0F, INF_F, 0.0F},
            Vec3{0.0F, 0.0F, -INF_F},
        }};
        for (const Vec3 center : badCentres) {
            rd::DebugDrawBatch batch = roomyBatch();
            CHECK(rd::emitPointLightGizmo(batch, {.center = center, .range = 5.0F}) == 0U);
            CHECK(batch.lineCount() == 0U);
            CHECK(batch.rejectedLines() == 0U);
            CHECK(batch.empty());
        }
    }

    SUBCASE("anti-vacuity: the SAME centre with a good range does emit") {
        rd::DebugDrawBatch batch = roomyBatch();
        CHECK(rd::emitPointLightGizmo(batch, {.center = Vec3{1.0F, 2.0F, 3.0F}, .range = 5.0F}) > 0U);
    }
}

TEST_CASE("render light gizmo: the spot's rim sits ON the range sphere (GZ6)") {
    // THE DESIGN CLAIM. The outer circle is at axial range*cos(outer) with radius range*sin(outer), so
    // every rim point is EXACTLY `range` from the apex whatever `range` is -- a relationship, never a
    // magnitude. The rejected flat form (a circle of radius range*tan(outer) at axial range) puts the
    // rim at range/cos(outer) instead, which this case reads as a rim ray 15% too long at 30 degrees.
    rd::DebugDrawBatch batch = roomyBatch();
    const Vec3 apex{1.0F, 2.0F, -3.0F};
    const Vec3 direction{0.3F, -0.9F, 0.4F};
    const Vec3 axis = engine::normalize(direction);
    constexpr float RANGE = 10.0F;
    const float outer = engine::radians(30.0F);
    const std::uint32_t emitted = rd::emitSpotLightGizmo(
        batch, {.apex = apex, .direction = direction, .range = RANGE, .outerConeRadians = outer});

    // ONE cap (inner defaults to 0, which is not < outer... it IS: 0 < 30 degrees), so TWO caps and
    // four rays. The inner cap has radius range*sin(0) == 0 and therefore emits no circle at all.
    CHECK(emitted == rd::LIGHT_GIZMO_CIRCLE_SEGMENTS + rd::LIGHT_GIZMO_SPOT_RIM_RAYS);
    CHECK(batch.rejectedLines() == 0U);

    const std::vector<rd::DebugLineVertex> v = verticesOf(batch, rd::DebugDepth::Overlay);
    REQUIRE(v.size() == 2U * emitted);

    // 1e-3 at range 10 is 1e-4 relative: cos/sin, a normalize, a cross product and a translate.
    constexpr float EPS = 1e-3F;
    const std::size_t circleVertexCount = std::size_t{2} * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS;
    for (std::size_t i = 0; i < circleVertexCount; ++i) {
        CHECK(std::abs(axialDistance(v[i].position, apex, axis) - (RANGE * std::cos(outer))) <= EPS);
        CHECK(std::abs(perpRadius(v[i].position, apex, axis) - (RANGE * std::sin(outer))) <= EPS);
        CHECK(std::abs(engine::length(v[i].position - apex) - RANGE) <= EPS);
    }

    // THE RIM RAYS: each is (apex, rim) and each is exactly `range` long.
    for (std::size_t k = 0; k < rd::LIGHT_GIZMO_SPOT_RIM_RAYS; ++k) {
        const Vec3 start = v[circleVertexCount + (k * 2U)].position;
        const Vec3 end = v[circleVertexCount + (k * 2U) + 1U].position;
        CHECK(engine::length(start - apex) <= EPS);  // every ray STARTS at the apex
        CHECK(std::abs(engine::length(end - apex) - RANGE) <= EPS);
    }
}

TEST_CASE("render light gizmo: the spot's rim rays land on EMITTED circle vertices (GZ7)") {
    // THE CASE THAT EXISTS FOR THE EXPOSED BASIS. A ray computed from a locally-rebuilt basis would
    // land on the circle to within an ulp and this comparison would go red -- which is the whole
    // reason debugCircleBasis was promoted rather than reproduced.
    //
    // THE POSE IS CHOSEN, NOT ARBITRARY. wireCircle emits vertex 0 as `center + u*radius` while every
    // later vertex takes the general cos/sin form, so the k == 0 ray adds `v * 0.0F` on top: exact
    // for every component EXCEPT one that is exactly -0.0F, where -0.0 + 0.0 is +0.0 -- equal,
    // bit-different. This apex/direction pair puts no component of the cap centre plus u*radius at a
    // signed zero, and the arm below measures that rather than assuming it.
    rd::DebugDrawBatch batch = roomyBatch();
    const Vec3 apex{1.5F, 0.75F, -2.25F};
    const Vec3 direction{0.2F, 0.5F, -0.84F};
    constexpr float RANGE = 6.0F;
    const float outer = engine::radians(35.0F);
    const std::uint32_t emitted = rd::emitSpotLightGizmo(
        batch, {.apex = apex, .direction = direction, .range = RANGE, .outerConeRadians = outer});
    REQUIRE(emitted == rd::LIGHT_GIZMO_CIRCLE_SEGMENTS + rd::LIGHT_GIZMO_SPOT_RIM_RAYS);

    const std::vector<rd::DebugLineVertex> v = verticesOf(batch, rd::DebugDepth::Overlay);
    const std::size_t circleVertexCount = std::size_t{2} * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS;
    const std::uint32_t stride = rd::LIGHT_GIZMO_CIRCLE_SEGMENTS / rd::LIGHT_GIZMO_SPOT_RIM_RAYS;

    // No component of vertex 0 is a signed zero, which is what makes the k == 0 arm below a bit
    // claim rather than a coincidence.
    const Bits3 vertexZero = bitsOf(v[0].position);
    CHECK(vertexZero.x != 0x00000000U);
    CHECK(vertexZero.x != 0x80000000U);
    CHECK(vertexZero.y != 0x00000000U);
    CHECK(vertexZero.y != 0x80000000U);
    CHECK(vertexZero.z != 0x00000000U);
    CHECK(vertexZero.z != 0x80000000U);

    for (std::uint32_t k = 0; k < rd::LIGHT_GIZMO_SPOT_RIM_RAYS; ++k) {
        CAPTURE(k);
        const Vec3 rim = v[circleVertexCount + (std::size_t{k} * 2U) + 1U].position;
        // wireCircle pushes vertex 0 at index 0 and vertex i (i = 1..n) at index 2*i - 1. The CLOSING
        // vertex at i == n is NOT vertex 0: the angle there is TWO_PI exactly and std::sin(TWO_PI_f)
        // is about -1.75e-7, so matching k == 0 against the last vertex would match the wrong one.
        const std::uint32_t i = k * stride;
        const std::size_t index = i == 0U ? 0U : (2U * static_cast<std::size_t>(i)) - 1U;
        CHECK(bitsOf(rim) == bitsOf(v[index].position));
    }

    SUBCASE("anti-vacuity: a rim ray is NOT bit-equal to the circle's CLOSING vertex") {
        const Vec3 rimZero = v[circleVertexCount + 1U].position;
        CHECK_FALSE(bitsOf(rimZero) == bitsOf(v[circleVertexCount - 1U].position));
    }
}

TEST_CASE("render light gizmo: the inner cap is drawn iff the cone fold says the edge is soft (GZ8)") {
    // THE TIE, ASSERTED ACROSS THE TWO SPELLINGS OF ONE DECISION. The emitter mirrors the falloff
    // vocabulary's fold of inner >= outer into a hard edge; it cannot READ it, because a resolved
    // cone carries no bit saying its delta was floored. So the two are driven with the same angles
    // and compared -- AX1's shape, and a real pin: seeding `inner <= outer` in the emitter reddens it.
    const float outer = engine::radians(30.0F);
    constexpr float RANGE = 8.0F;
    const std::array<float, 3> inners{{engine::radians(20.0F), engine::radians(30.0F), engine::radians(40.0F)}};
    const std::array<bool, 3> expectSoft{{true, false, false}};

    for (std::size_t arm = 0; arm < inners.size(); ++arm) {
        CAPTURE(arm);
        rd::DebugDrawBatch batch = roomyBatch();
        const std::uint32_t emitted = rd::emitSpotLightGizmo(batch, {.apex = Vec3::zero(),
                                                                     .direction = Vec3{0.0F, 0.0F, -1.0F},
                                                                     .range = RANGE,
                                                                     .innerConeRadians = inners[arm],
                                                                     .outerConeRadians = outer});

        // THE CAPS, IDENTIFIED BY RADIUS RATHER THAN BY ORDINAL. Two distinct perpendicular radii
        // means two caps; one means one. `outer` is the surviving cap in every arm, and which one it
        // is is read off the geometry rather than inferred from the push order.
        const std::vector<rd::DebugLineVertex> v = verticesOf(batch, rd::DebugDepth::Overlay);
        const Vec3 axis{0.0F, 0.0F, -1.0F};
        const float outerRadius = RANGE * std::sin(outer);
        const float innerRadius = RANGE * std::sin(inners[arm]);
        int onOuter = 0;
        int onInner = 0;
        for (const rd::DebugLineVertex& vertex : v) {
            const float r = perpRadius(vertex.position, Vec3::zero(), axis);
            if (std::abs(r - outerRadius) <= 1e-3F) {
                ++onOuter;
            } else if (std::abs(r - innerRadius) <= 1e-3F) {
                ++onInner;
            }
        }
        const bool twoCaps = onInner > 0;
        CHECK(onOuter > 0);  // the OUTER cap is always drawn

        // ...and the falloff's own answer, over the same two angles.
        const int graded = gradedSampleCount(inners[arm], outer);
        CAPTURE(graded);
        CAPTURE(onInner);
        CHECK(twoCaps == expectSoft[arm]);
        CHECK((graded >= 10) == expectSoft[arm]);
        CHECK(twoCaps == (graded >= 10));  // THE TIE: one decision, two consumers
        CHECK(emitted == (twoCaps ? 2U * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS : rd::LIGHT_GIZMO_CIRCLE_SEGMENTS) +
                             rd::LIGHT_GIZMO_SPOT_RIM_RAYS);
    }
}

TEST_CASE("render light gizmo: a spot at outer == HALF_PI is finite and drawn (GZ9)") {
    // THE CASE THAT FAILS ON THE REJECTED FLAT FORM. The component clamps outerConeRadians to exactly
    // HALF_PI, and range*tan(HALF_PI) is infinite -- one wireCircle rejection and no cone at a value
    // the Inspector slider can reach. On the range sphere it is a great circle THROUGH the apex.
    // HALF_PI is spelled as the constant, never as a decimal.
    rd::DebugDrawBatch batch = roomyBatch();
    constexpr float RANGE = 4.0F;
    const Vec3 axis{0.0F, 0.0F, -1.0F};
    const std::uint32_t emitted = rd::emitSpotLightGizmo(
        batch, {.apex = Vec3::zero(), .direction = axis, .range = RANGE, .outerConeRadians = engine::HALF_PI});
    CHECK(emitted == rd::LIGHT_GIZMO_CIRCLE_SEGMENTS + rd::LIGHT_GIZMO_SPOT_RIM_RAYS);
    CHECK(batch.rejectedLines() == 0U);

    const std::vector<rd::DebugLineVertex> v = verticesOf(batch, rd::DebugDepth::Overlay);
    const std::size_t circleVertexCount = std::size_t{2} * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS;
    // std::cos(HALF_PI as a float) is about -4.37e-8, not zero, so the cap centre is at the apex to
    // an epsilon and NOT exactly. An exact assertion here would be wrong on every lane.
    constexpr float EPS = 1e-4F;
    for (std::size_t i = 0; i < circleVertexCount; ++i) {
        CHECK(std::abs(axialDistance(v[i].position, Vec3::zero(), axis)) <= EPS);
        CHECK(std::abs(perpRadius(v[i].position, Vec3::zero(), axis) - RANGE) <= EPS);
        CHECK(std::isfinite(engine::length(v[i].position)));
    }
    for (const rd::DebugLineVertex& vertex : v) {
        CHECK(std::isfinite(vertex.position.x));
        CHECK(std::isfinite(vertex.position.y));
        CHECK(std::isfinite(vertex.position.z));
    }
}

TEST_CASE("render light gizmo: a spot at outer == 0 is one axis ray, not a rejection (GZ10)") {
    // A cone with no aperture is a ray. Emitting nothing at all would leave a light with no gizmo,
    // and calling wireCircle with radius 0 would be ONE rejection and no picture -- both are wrong
    // pictures for a legal component value.
    rd::DebugDrawBatch batch = roomyBatch();
    constexpr float RANGE = 3.5F;
    const Vec3 apex{0.5F, 1.0F, 2.0F};
    const Vec3 direction{0.0F, -1.0F, 0.0F};
    const rd::SpotGizmoParams params{.apex = apex, .direction = direction, .range = RANGE, .outerConeRadians = 0.0F};
    const std::uint32_t emitted = rd::emitSpotLightGizmo(batch, params);

    CHECK(emitted == 1U);
    CHECK(batch.lineCount() == 1U);
    CHECK(batch.rejectedLines() == 0U);
    const std::vector<rd::DebugLineVertex> v = verticesOf(batch, rd::DebugDepth::Overlay);
    REQUIRE(v.size() == 2U);
    CHECK(engine::length(v[0].position - apex) <= UNIT_EPS);
    CHECK(std::abs(engine::length(v[1].position - apex) - RANGE) <= 1e-4F);
    // ...and it points down the axis, not somewhere else.
    CHECK(std::abs(axialDistance(v[1].position, apex, engine::normalize(direction)) - RANGE) <= 1e-4F);
}

TEST_CASE("render light gizmo: every degenerate spot input emits NOTHING and rejects NOTHING (GZ11)") {
    const rd::SpotGizmoParams good{.apex = Vec3{1.0F, 1.0F, 1.0F},
                                   .direction = Vec3{0.0F, 0.0F, -1.0F},
                                   .range = 5.0F,
                                   .innerConeRadians = engine::radians(10.0F),
                                   .outerConeRadians = engine::radians(20.0F)};

    std::vector<rd::SpotGizmoParams> bad;
    for (const float value : {NAN_F, INF_F, -INF_F}) {
        rd::SpotGizmoParams p = good;
        p.innerConeRadians = value;
        bad.push_back(p);
        p = good;
        p.outerConeRadians = value;
        bad.push_back(p);
        p = good;
        p.range = value;
        bad.push_back(p);
        p = good;
        p.apex.y = value;
        bad.push_back(p);
        p = good;
        p.direction.z = value;
        bad.push_back(p);
    }
    {
        rd::SpotGizmoParams p = good;
        p.direction = Vec3::zero();  // a zero direction is not a bad push either
        bad.push_back(p);
        p = good;
        p.range = 0.0F;
        bad.push_back(p);
        p = good;
        p.range = -2.0F;
        bad.push_back(p);
    }

    for (const rd::SpotGizmoParams& params : bad) {
        rd::DebugDrawBatch batch = roomyBatch();
        CHECK(rd::emitSpotLightGizmo(batch, params) == 0U);
        CHECK(batch.lineCount() == 0U);
        CHECK(batch.rejectedLines() == 0U);
        CHECK(batch.droppedLines() == 0U);
        CHECK(batch.empty());
    }

    SUBCASE("anti-vacuity: the unmodified params DO emit") {
        rd::DebugDrawBatch batch = roomyBatch();
        CHECK(rd::emitSpotLightGizmo(batch, good) > 0U);
    }
}

TEST_CASE("render light gizmo: a directional light is a disc plus parallel rays (GZ12)") {
    rd::DebugDrawBatch batch = roomyBatch();
    const Vec3 origin{-2.0F, 3.0F, 1.0F};
    const Vec3 direction{0.4F, -0.6F, 0.7F};
    const Vec3 axis = engine::normalize(direction);
    const std::uint32_t emitted = rd::emitDirectionalLightGizmo(batch, {.origin = origin, .direction = direction});

    CHECK(emitted == rd::LIGHT_GIZMO_CIRCLE_SEGMENTS + rd::LIGHT_GIZMO_DIRECTIONAL_RAYS);
    CHECK(batch.rejectedLines() == 0U);

    const std::vector<rd::DebugLineVertex> v = verticesOf(batch, rd::DebugDepth::Overlay);
    const std::size_t discVertexCount = std::size_t{2} * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS;
    constexpr float EPS = 1e-5F;
    for (std::size_t i = 0; i < discVertexCount; ++i) {
        // The disc is PLANAR through the origin, perpendicular to the aim...
        CHECK(std::abs(axialDistance(v[i].position, origin, axis)) <= EPS);
        // ...and on the disc's own radius, which is a NAMED constant rather than a literal.
        CHECK(std::abs(perpRadius(v[i].position, origin, axis) - rd::LIGHT_GIZMO_DIRECTIONAL_RADIUS) <= EPS);
    }
    for (std::size_t k = 0; k < rd::LIGHT_GIZMO_DIRECTIONAL_RAYS; ++k) {
        const Vec3 start = v[discVertexCount + (k * 2U)].position;
        const Vec3 end = v[discVertexCount + (k * 2U) + 1U].position;
        const Vec3 ray = end - start;
        CHECK(std::abs(engine::length(ray) - rd::LIGHT_GIZMO_DIRECTIONAL_LENGTH) <= EPS);
        // PARALLEL to the aim: the cross product vanishes, which is a claim about direction and not
        // merely about length.
        CHECK(engine::length(engine::cross(ray, axis)) <= EPS);
    }
}

TEST_CASE("render light gizmo: the directional rays START on emitted disc vertices (GZ13)") {
    // GZ7's claim for the second emitter, and the same reason: a locally-rebuilt basis lands on the
    // disc to within an ulp and this comparison goes red.
    rd::DebugDrawBatch batch = roomyBatch();
    const Vec3 origin{0.375F, -1.25F, 2.5F};
    const Vec3 direction{0.1F, 0.95F, -0.3F};
    const std::uint32_t emitted = rd::emitDirectionalLightGizmo(batch, {.origin = origin, .direction = direction});
    REQUIRE(emitted == rd::LIGHT_GIZMO_CIRCLE_SEGMENTS + rd::LIGHT_GIZMO_DIRECTIONAL_RAYS);

    const std::vector<rd::DebugLineVertex> v = verticesOf(batch, rd::DebugDepth::Overlay);
    const std::size_t discVertexCount = std::size_t{2} * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS;
    const std::uint32_t stride = rd::LIGHT_GIZMO_CIRCLE_SEGMENTS / rd::LIGHT_GIZMO_DIRECTIONAL_RAYS;
    for (std::uint32_t k = 0; k < rd::LIGHT_GIZMO_DIRECTIONAL_RAYS; ++k) {
        CAPTURE(k);
        const Vec3 start = v[discVertexCount + (static_cast<std::size_t>(k) * 2U)].position;
        const std::uint32_t i = k * stride;
        const std::size_t index = i == 0U ? 0U : (2U * static_cast<std::size_t>(i)) - 1U;
        CHECK(bitsOf(start) == bitsOf(v[index].position));
    }

    SUBCASE("totality: a zero, NaN or infinite direction or origin emits nothing and rejects nothing") {
        const std::array<rd::DirectionalGizmoParams, 6> bad{{
            {.origin = origin, .direction = Vec3::zero()},
            {.origin = origin, .direction = Vec3{NAN_F, 0.0F, 0.0F}},
            {.origin = origin, .direction = Vec3{INF_F, 0.0F, 0.0F}},
            {.origin = Vec3{NAN_F, 0.0F, 0.0F}, .direction = direction},
            {.origin = Vec3{0.0F, -INF_F, 0.0F}, .direction = direction},
            {.origin = origin, .direction = Vec3{0.0F, 0.0F, -1e-9F}},  // too short to normalize
        }};
        for (const rd::DirectionalGizmoParams& params : bad) {
            rd::DebugDrawBatch empty = roomyBatch();
            CHECK(rd::emitDirectionalLightGizmo(empty, params) == 0U);
            CHECK(empty.lineCount() == 0U);
            CHECK(empty.rejectedLines() == 0U);
            CHECK(empty.empty());
        }
    }
}

TEST_CASE("render light gizmo: every emitter honours style.depth, and the default is Overlay (GZ14)") {
    // The default is asserted WHERE IT IS USED: a gizmo drawn into the Tested bucket is hidden by the
    // wall the light is behind, which is the opposite of the editor convention this task exists for.
    const std::array<rd::DebugDepth, 2> depths{{rd::DebugDepth::Tested, rd::DebugDepth::Overlay}};
    for (const rd::DebugDepth depth : depths) {
        const bool tested = depth == rd::DebugDepth::Tested;
        const rd::DebugDepth other = tested ? rd::DebugDepth::Overlay : rd::DebugDepth::Tested;
        rd::LightGizmoStyle style{};
        style.depth = depth;

        rd::DebugDrawBatch point = roomyBatch();
        CHECK(rd::emitPointLightGizmo(point, {.range = 4.0F, .style = style}) > 0U);
        CHECK(point.lineCount(depth) == point.lineCount());
        CHECK(point.lineCount(other) == 0U);

        rd::DebugDrawBatch spot = roomyBatch();
        CHECK(rd::emitSpotLightGizmo(spot, {.range = 4.0F,
                                            .innerConeRadians = engine::radians(10.0F),
                                            .outerConeRadians = engine::radians(25.0F),
                                            .style = style}) > 0U);
        CHECK(spot.lineCount(depth) == spot.lineCount());
        CHECK(spot.lineCount(other) == 0U);

        rd::DebugDrawBatch directional = roomyBatch();
        CHECK(rd::emitDirectionalLightGizmo(directional, {.style = style}) > 0U);
        CHECK(directional.lineCount(depth) == directional.lineCount());
        CHECK(directional.lineCount(other) == 0U);
    }

    SUBCASE("the DEFAULT style lands in Overlay") {
        rd::DebugDrawBatch batch = roomyBatch();
        CHECK(rd::emitPointLightGizmo(batch, {.range = 4.0F}) > 0U);
        CHECK(batch.lineCount(rd::DebugDepth::Overlay) == batch.lineCount());
        CHECK(batch.lineCount(rd::DebugDepth::Tested) == 0U);
    }
}

TEST_CASE("render light gizmo: style.color reaches every line, and the inner cap is dimmer (GZ15)") {
    rd::LightGizmoStyle style{};
    style.color = Vec4{0.25F, 0.5F, 0.75F, 0.8F};
    const std::uint32_t expected = rd::packDebugColor(style.color);

    rd::DebugDrawBatch point = roomyBatch();
    CHECK(rd::emitPointLightGizmo(point, {.range = 4.0F, .style = style}) > 0U);
    for (const rd::DebugLineVertex& v : verticesOf(point, rd::DebugDepth::Overlay)) {
        CHECK(v.rgba == expected);
    }

    rd::DebugDrawBatch directional = roomyBatch();
    CHECK(rd::emitDirectionalLightGizmo(directional, {.style = style}) > 0U);
    for (const rd::DebugLineVertex& v : verticesOf(directional, rd::DebugDepth::Overlay)) {
        CHECK(v.rgba == expected);
    }

    SUBCASE("the spot's INNER cap alpha is STRICTLY BELOW the outer's -- a band, never a magnitude") {
        rd::DebugDrawBatch spot = roomyBatch();
        REQUIRE(style.innerAlphaScale < 1.0F);
        CHECK(rd::emitSpotLightGizmo(spot, {.range = 8.0F,
                                            .innerConeRadians = engine::radians(15.0F),
                                            .outerConeRadians = engine::radians(35.0F),
                                            .style = style}) > 0U);
        const float outerAlpha = rd::unpackDebugColor(expected).w;
        int dimmer = 0;
        int atFull = 0;
        for (const rd::DebugLineVertex& v : verticesOf(spot, rd::DebugDepth::Overlay)) {
            const float alpha = rd::unpackDebugColor(v.rgba).w;
            if (alpha < outerAlpha) {
                ++dimmer;
            } else {
                CHECK(v.rgba == expected);
                ++atFull;
            }
        }
        // BOTH arms non-empty, so neither is vacuous: the inner cap really is dimmer AND the outer
        // cap and the rays really are at the full colour.
        CHECK(dimmer == static_cast<int>(2U * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS));
        CHECK(atFull ==
              static_cast<int>((2U * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS) + (2U * rd::LIGHT_GIZMO_SPOT_RIM_RAYS)));
    }
}

TEST_CASE("render light gizmo: at the budget the emitter returns what the batch ACCEPTED (GZ16)") {
    // READ OFF THE BATCH, never a running sum of what the emitter MEANT to push -- E.1.2's GR8 lesson
    // at the source. A wireSphere that straddles the budget is drawn up to the segment the budget
    // ended on: partial, deterministic, and counted, which is E.1.1's stated overflow policy asserted
    // here for the first time from a CONSUMER.
    constexpr std::uint32_t CAP = 10U;
    rd::DebugDrawBatch batch{{.maxLines = CAP}};
    const std::uint32_t wanted = 3U * rd::LIGHT_GIZMO_CIRCLE_SEGMENTS;
    const std::uint32_t emitted = rd::emitPointLightGizmo(batch, {.center = Vec3::zero(), .range = 2.0F});

    CHECK(emitted == CAP);
    CHECK(batch.lineCount() == CAP);
    CHECK(batch.droppedLines() == wanted - CAP);
    CHECK(batch.rejectedLines() == 0U);

    SUBCASE("a SECOND emit into the same full batch accepts nothing and returns zero") {
        const std::uint32_t again = rd::emitPointLightGizmo(batch, {.center = Vec3::one(), .range = 2.0F});
        CHECK(again == 0U);
        CHECK(batch.lineCount() == CAP);
        CHECK(batch.droppedLines() == (2U * wanted) - CAP);
    }
}

TEST_CASE("render light gizmo: no emitter outruns its line bound over an adversarial sweep (GZ17)") {
    // MEASURED OFF THE BATCH, because E.1.2 proved a clamp that bounds a LOOP does not necessarily
    // bound a COUNT. Two arms, and they claim different things:
    //   (a) at the DEFAULT style -- the only style anything in this tree constructs, and the one the
    //       editor's line budget rests on -- every emitter stays at or below the derived per-entity
    //       cap over every range, angle and direction below;
    //   (b) at an ARBITRARY style.segments the cap SCALES, so the bound is the max of the three
    //       per-emitter formulas over the CLAMPED n. The stronger claim -- that (a)'s constant bounds
    //       (b) too -- is simply false: at n = 256 a point light is 768 lines by construction.
    const std::array<float, 5> ranges{{1e-6F, 1e-3F, 1.0F, 1e3F, 1e6F}};
    const std::array<Vec3, 4> directions{{
        Vec3{0.0F, 0.0F, -1.0F},
        Vec3{1.0F, 1.0F, 1.0F},
        Vec3{0.0F, 0.8999F, 0.4359F},  // both sides of debugCircleBasis' 0.9F helper-axis switch
        Vec3{0.0F, 0.9001F, 0.4358F},
    }};
    const std::array<std::uint32_t, 6> segmentCounts{{0U, 1U, 3U, 32U, 256U, 10000U}};
    constexpr int ANGLE_STEPS = 16;

    rd::DebugDrawBatch batch = roomyBatch();
    std::uint32_t worstDefault = 0;
    std::uint32_t sweptConfigurations = 0;
    for (const std::uint32_t segments : segmentCounts) {
        rd::LightGizmoStyle style{};
        style.segments = segments;
        const std::uint32_t n = std::clamp(segments, rd::MIN_CIRCLE_SEGMENTS, rd::MAX_CIRCLE_SEGMENTS);
        const std::uint32_t generalBound =
            std::max({3U * n, n + rd::LIGHT_GIZMO_DIRECTIONAL_RAYS, (2U * n) + rd::LIGHT_GIZMO_SPOT_RIM_RAYS});
        const bool isDefault = segments == rd::LIGHT_GIZMO_CIRCLE_SEGMENTS;

        for (const Vec3 direction : directions) {
            for (const float range : ranges) {
                batch.clear();
                const std::uint32_t point =
                    rd::emitPointLightGizmo(batch, {.center = Vec3::one(), .range = range, .style = style});
                batch.clear();
                const std::uint32_t directional = rd::emitDirectionalLightGizmo(
                    batch, {.origin = Vec3::one(), .direction = direction, .style = style});
                CHECK(point <= generalBound);
                CHECK(directional <= generalBound);
                CHECK(batch.rejectedLines() == 0U);
                if (isDefault) {
                    worstDefault = std::max({worstDefault, point, directional});
                    CHECK(point <= rd::LIGHT_GIZMO_MAX_LINES_PER_ENTITY);
                    CHECK(directional <= rd::LIGHT_GIZMO_MAX_LINES_PER_ENTITY);
                }

                for (int step = 0; step <= ANGLE_STEPS; ++step) {
                    const float fraction = static_cast<float>(step) / static_cast<float>(ANGLE_STEPS);
                    const float outer = engine::HALF_PI * fraction;
                    batch.clear();
                    const std::uint32_t spot = rd::emitSpotLightGizmo(batch, {.apex = Vec3::one(),
                                                                              .direction = direction,
                                                                              .range = range,
                                                                              .innerConeRadians = outer * 0.5F,
                                                                              .outerConeRadians = outer,
                                                                              .style = style});
                    CHECK(spot <= generalBound);
                    CHECK(batch.rejectedLines() == 0U);
                    if (isDefault) {
                        worstDefault = std::max(worstDefault, spot);
                        CHECK(spot <= rd::LIGHT_GIZMO_MAX_LINES_PER_ENTITY);
                    }
                    ++sweptConfigurations;
                }
            }
        }
    }
    // ANTI-VACUITY: the sweep really ran, and at the default style it really reached the cap rather
    // than staying trivially far below it -- otherwise "no emitter outruns the bound" would be a
    // statement about an emitter that emitted nothing.
    const std::size_t expectedConfigurations =
        segmentCounts.size() * directions.size() * ranges.size() * static_cast<std::size_t>(ANGLE_STEPS + 1);
    CHECK(sweptConfigurations == static_cast<std::uint32_t>(expectedConfigurations));
    CHECK(worstDefault == rd::LIGHT_GIZMO_MAX_LINES_PER_ENTITY);
}

// tests/render_debug_draw_test.cpp — task E.1.1: the debug-draw vocabulary, the pure batch, the
// packers, the shader source pin (DD1-DD26, every configuration), and the GPU DebugDraw (DG1-DG16,
// gated).
//
// The whole DD battery runs with NO device and NO shader toolchain -- that is the point of the pure
// split: everything assertable without a GPU is asserted without one.

// <aero/render/debug_draw.hpp> IS DELIBERATELY NOT INCLUDED HERE. DD23's whole claim is that the
// UMBRELLA carries it, and with both includes present that case passes on a seeded umbrella and
// proves nothing. Everything below is reached through render.hpp alone.
#include <aero/render/render.hpp>

#include "../engine/render/src/debug_draw_pack.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <ostream>  // MSVC: any CHECK on a string_view needs this (the 0.4.1 trap)
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using engine::Mat4;
using engine::Vec2;
using engine::Vec3;
using engine::Vec4;
namespace rd = engine::render;

constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
constexpr float INF_F = std::numeric_limits<float>::infinity();

// The four bytes of a packed colour, IN MEMORY ORDER -- which is the property the vertex format
// depends on, and is NOT the same claim as an integer comparison.
[[nodiscard]] std::array<std::uint8_t, 4> colorBytes(std::uint32_t packed) {
    std::array<std::uint8_t, 4> bytes{};
    std::memcpy(bytes.data(), &packed, sizeof(packed));
    return bytes;
}

// Comment-stripped shader source, the JP14/TM29 pattern, reached through AERO_SHADERS_SRC_DIR (the
// SOURCE tree, distinct from AERO_SHADERS_DIR's build output). UNGATED: the HLSL text exists whether
// or not AERO_SHADER_TOOLS built it.
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

[[nodiscard]] std::string strippedShaderSource(std::string_view name) {
    return strippedSourceAt(std::string{AERO_SHADERS_SRC_DIR} + "/" + std::string{name});
}

// The umbrella header's own path, derived from the SOURCE tree's shaders directory -- the one route
// into the source tree this TU already has (DD26 reads the HLSL through it). Deliberately NOT a new
// compile definition: tests/CMakeLists.txt is held to the two source lines this task added.
constexpr std::string_view RENDER_UMBRELLA_PATH =
    AERO_SHADERS_SRC_DIR "/../engine/render/include/aero/render/render.hpp";

[[nodiscard]] bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("render debug draw: the two vertex layouts are pinned to the pipeline's literals (DD1)") {
    // The pipeline descriptors write 16/36 and the offsets 0/12 and 0/12/20/28/32 as BARE LITERALS.
    // These are the only thing tying them to the C++ types; a field inserted in either struct moves
    // an offset that no compiler and no backend would complain about, and the picture would be
    // garbage on every lane at once.
    CHECK(sizeof(rd::DebugLineVertex) == 16U);
    CHECK(offsetof(rd::DebugLineVertex, position) == 0U);
    CHECK(offsetof(rd::DebugLineVertex, rgba) == 12U);

    CHECK(sizeof(rd::DebugBillboardVertex) == 36U);
    CHECK(offsetof(rd::DebugBillboardVertex, center) == 0U);
    CHECK(offsetof(rd::DebugBillboardVertex, corner) == 12U);
    CHECK(offsetof(rd::DebugBillboardVertex, uv) == 20U);
    CHECK(offsetof(rd::DebugBillboardVertex, rgba) == 28U);
    CHECK(offsetof(rd::DebugBillboardVertex, sizePx) == 32U);

    // Standard layout is what licenses offsetof above; trivially COPYABLE is what licenses the
    // staging memcpy in flush(). NOT trivially DEFAULT-constructible -- Vec3 carries NSDMIs, so that
    // is false and asserting it would redden on a correct tree (the AssetDragPayload lesson).
    CHECK(std::is_standard_layout_v<rd::DebugLineVertex>);
    CHECK(std::is_trivially_copyable_v<rd::DebugLineVertex>);
    CHECK(std::is_standard_layout_v<rd::DebugBillboardVertex>);
    CHECK(std::is_trivially_copyable_v<rd::DebugBillboardVertex>);

    // ZERO TAIL PADDING is what makes the staging copy deterministic: 12+4 = 16 and
    // 12+8+8+4+4 = 36, both already a multiple of alignof (4). Asserted rather than assumed.
    CHECK(alignof(rd::DebugLineVertex) == 4U);
    CHECK(alignof(rd::DebugBillboardVertex) == 4U);
}

TEST_CASE("render debug draw: packDebugColor writes r,g,b,a in MEMORY order (DD2)") {
    CHECK(rd::packDebugColor(Vec4{1.0F, 1.0F, 1.0F, 1.0F}) == 0xFFFFFFFFU);
    CHECK(rd::packDebugColor(Vec4{0.0F, 0.0F, 0.0F, 0.0F}) == 0x00000000U);

    // THE MEMORY ORDER, which is what UByte4Norm actually reads -- an integer comparison alone would
    // pass a seeded a,b,g,r packer on a big-endian lane and says nothing about byte layout anywhere.
    const std::array<std::uint8_t, 4> red = colorBytes(rd::packDebugColor(Vec4{1.0F, 0.0F, 0.0F, 1.0F}));
    CHECK(red[0] == 255U);  // r
    CHECK(red[1] == 0U);    // g
    CHECK(red[2] == 0U);    // b
    CHECK(red[3] == 255U);  // a
    const std::array<std::uint8_t, 4> mix = colorBytes(rd::packDebugColor(Vec4{0.0F, 1.0F, 0.0F, 0.0F}));
    CHECK(mix[0] == 0U);
    CHECK(mix[1] == 255U);
    CHECK(mix[2] == 0U);
    CHECK(mix[3] == 0U);
}

TEST_CASE("render debug draw: packDebugColor is TOTAL over every float (DD3)") {
    // Seed S18's witness. Without the non-finite arm, std::clamp(NaN, 0, 1) returns NaN on libc++
    // and the narrowing cast is UB that UBSan traps on both Debug lanes -- so this case fails as an
    // ABORT there, not as a wrong number, which is the loudest possible failure and the right one.
    SUBCASE("NaN floors to 0 in every channel") {
        CHECK(colorBytes(rd::packDebugColor(Vec4{NAN_F, 1.0F, 1.0F, 1.0F}))[0] == 0U);
        CHECK(colorBytes(rd::packDebugColor(Vec4{1.0F, NAN_F, 1.0F, 1.0F}))[1] == 0U);
        CHECK(colorBytes(rd::packDebugColor(Vec4{1.0F, 1.0F, NAN_F, 1.0F}))[2] == 0U);
        CHECK(colorBytes(rd::packDebugColor(Vec4{1.0F, 1.0F, 1.0F, NAN_F}))[3] == 0U);
    }
    SUBCASE("the infinities clamp rather than floor") {
        CHECK(colorBytes(rd::packDebugColor(Vec4{INF_F, 0.0F, 0.0F, 0.0F}))[0] == 255U);
        CHECK(colorBytes(rd::packDebugColor(Vec4{-INF_F, 1.0F, 1.0F, 1.0F}))[0] == 0U);
    }
    SUBCASE("out-of-range finite values clamp") {
        CHECK(colorBytes(rd::packDebugColor(Vec4{1.5F, -0.2F, 0.0F, 0.0F}))[0] == 255U);
        CHECK(colorBytes(rd::packDebugColor(Vec4{1.5F, -0.2F, 0.0F, 0.0F}))[1] == 0U);
    }
    SUBCASE("rounding is round-half-away-from-zero, as std::lround specifies") {
        // 0.5 * 255 = 127.5 -> 128; 0.501 * 255 = 127.755 -> 128; 0.499 * 255 = 127.245 -> 127.
        CHECK(colorBytes(rd::packDebugColor(Vec4{0.5F, 0.0F, 0.0F, 0.0F}))[0] == 128U);
        CHECK(colorBytes(rd::packDebugColor(Vec4{0.501F, 0.0F, 0.0F, 0.0F}))[0] == 128U);
        CHECK(colorBytes(rd::packDebugColor(Vec4{0.499F, 0.0F, 0.0F, 0.0F}))[0] == 127U);
    }
}

TEST_CASE("render debug draw: unpack(pack(k/255)) is EXACT for all 256 values (DD4)") {
    // EXACT, not within an epsilon: k/255 * 255 is exactly k for every k in [0, 255] under fp32
    // (both are exactly representable and the product is exact), and lround of an integer is that
    // integer. If this ever needs a tolerance, the packer changed.
    for (int k = 0; k <= 255; ++k) {
        const float v = static_cast<float>(k) / 255.0F;
        const Vec4 out = rd::unpackDebugColor(rd::packDebugColor(Vec4{v, v, v, v}));
        CHECK(out.x == v);
        CHECK(out.y == v);
        CHECK(out.z == v);
        CHECK(out.w == v);
    }
}

TEST_CASE("render debug draw: the budget's defaults, ceilings and clamp (DD5)") {
    const rd::DebugDrawBudget defaults{};
    CHECK(defaults.maxLines == 32768U);
    CHECK(defaults.maxBillboards == 4096U);
    CHECK(rd::DEBUG_DRAW_MAX_LINES_CEILING == 1048576U);
    CHECK(rd::DEBUG_DRAW_MAX_BILLBOARDS_CEILING == 65536U);

    CHECK((rd::clampDebugDrawBudget({0U, 0U}) == rd::DebugDrawBudget{1U, 1U}));
    CHECK((
        rd::clampDebugDrawBudget({rd::DEBUG_DRAW_MAX_LINES_CEILING + 1U, rd::DEBUG_DRAW_MAX_BILLBOARDS_CEILING + 1U}) ==
        rd::DebugDrawBudget{rd::DEBUG_DRAW_MAX_LINES_CEILING, rd::DEBUG_DRAW_MAX_BILLBOARDS_CEILING}));
    CHECK((rd::clampDebugDrawBudget(defaults) == defaults));  // an in-range value is UNCHANGED
}

TEST_CASE("render debug draw: debugDepthLabel is total, distinct and never empty (DD6)") {
    CHECK(rd::DEBUG_DEPTH_COUNT == 2U);
    CHECK(rd::debugDepthLabel(rd::DebugDepth::Tested) == "Tested");
    CHECK(rd::debugDepthLabel(rd::DebugDepth::Overlay) == "Overlay");
    CHECK_FALSE(rd::debugDepthLabel(rd::DebugDepth::Tested).empty());
    CHECK_FALSE(rd::debugDepthLabel(rd::DebugDepth::Overlay).empty());
    CHECK(rd::debugDepthLabel(rd::DebugDepth::Tested) != rd::debugDepthLabel(rd::DebugDepth::Overlay));
    // Total over an out-of-range value too -- the switch has no default:, so the fallthrough return
    // is what answers, and it must not be empty either.
    CHECK_FALSE(rd::debugDepthLabel(static_cast<rd::DebugDepth>(200)).empty());
}

TEST_CASE("render debug draw: a fresh batch is empty and one line lands as two vertices (DD7)") {
    rd::DebugDrawBatch batch{{}};
    CHECK(batch.empty());
    CHECK(batch.lineCount() == 0U);
    CHECK(batch.billboardCount() == 0U);
    CHECK(batch.droppedLines() == 0U);
    CHECK(batch.rejectedLines() == 0U);

    CHECK(batch.line(Vec3{1.0F, 2.0F, 3.0F}, Vec3{4.0F, 5.0F, 6.0F}, Vec4{0.0F, 1.0F, 0.0F, 1.0F}));
    CHECK_FALSE(batch.empty());
    CHECK(batch.lineCount() == 1U);
    CHECK(batch.lineCount(rd::DebugDepth::Tested) == 1U);
    CHECK(batch.lineCount(rd::DebugDepth::Overlay) == 0U);

    const std::span<const rd::DebugLineVertex> vertices = batch.lineVertices(rd::DebugDepth::Tested);
    REQUIRE(vertices.size() == 2U);
    CHECK(vertices[0].position.x == 1.0F);
    CHECK(vertices[0].position.y == 2.0F);
    CHECK(vertices[0].position.z == 3.0F);
    CHECK(vertices[1].position.x == 4.0F);
    CHECK(vertices[1].position.z == 6.0F);
    // BOTH vertices carry the packed colour: a line is one colour, and the shader reads it per vertex.
    CHECK(vertices[0].rgba == rd::packDebugColor(Vec4{0.0F, 1.0F, 0.0F, 1.0F}));
    CHECK(vertices[1].rgba == vertices[0].rgba);
    CHECK(batch.lineVertices(rd::DebugDepth::Overlay).empty());
}

TEST_CASE("render debug draw: an Overlay line lands in the Overlay bucket ONLY (DD8)") {
    rd::DebugDrawBatch batch{{}};
    CHECK(batch.line(Vec3{}, Vec3{1.0F, 0.0F, 0.0F}, Vec4{1.0F, 1.0F, 1.0F, 1.0F}, rd::DebugDepth::Overlay));
    CHECK(batch.lineCount(rd::DebugDepth::Overlay) == 1U);
    CHECK(batch.lineCount(rd::DebugDepth::Tested) == 0U);
    CHECK(batch.lineCount() == 1U);
    CHECK(batch.lineVertices(rd::DebugDepth::Tested).empty());
    CHECK(batch.lineVertices(rd::DebugDepth::Overlay).size() == 2U);
}

TEST_CASE("render debug draw: overflow keeps the FIRST maxLines and counts the rest (DD9)") {
    rd::DebugDrawBatch batch{{.maxLines = 3U, .maxBillboards = 4096U}};
    // Distinct x per line, so "which three survived" is an assertion rather than a count.
    for (int i = 0; i < 5; ++i) {
        const auto x = static_cast<float>(i);
        const bool accepted = batch.line(Vec3{x, 0.0F, 0.0F}, Vec3{x, 1.0F, 0.0F}, Vec4{1.0F, 1.0F, 1.0F, 1.0F});
        CHECK(accepted == (i < 3));
    }
    CHECK(batch.lineCount() == 3U);
    CHECK(batch.droppedLines() == 2U);
    CHECK(batch.rejectedLines() == 0U);  // a budget refusal is NEVER a rejection

    const std::span<const rd::DebugLineVertex> vertices = batch.lineVertices(rd::DebugDepth::Tested);
    REQUIRE(vertices.size() == 6U);
    CHECK(vertices[0].position.x == 0.0F);
    CHECK(vertices[2].position.x == 1.0F);
    CHECK(vertices[4].position.x == 2.0F);  // NOT 3 or 4 -- seed S22's witness
}

TEST_CASE("render debug draw: a non-finite endpoint is REJECTED, a zero-length line is not (DD10)") {
    rd::DebugDrawBatch batch{{}};
    const Vec4 white{1.0F, 1.0F, 1.0F, 1.0F};

    CHECK_FALSE(batch.line(Vec3{NAN_F, 0.0F, 0.0F}, Vec3{1.0F, 0.0F, 0.0F}, white));
    CHECK_FALSE(batch.line(Vec3{0.0F, 0.0F, 0.0F}, Vec3{INF_F, 0.0F, 0.0F}, white));
    CHECK_FALSE(batch.line(Vec3{0.0F, -INF_F, 0.0F}, Vec3{1.0F, 0.0F, 0.0F}, white));
    CHECK(batch.rejectedLines() == 3U);
    CHECK(batch.droppedLines() == 0U);  // UNMOVED: a rejection is not a drop
    CHECK(batch.lineCount() == 0U);     // NOTHING was pushed
    CHECK(batch.empty());

    // A ZERO-LENGTH LINE IS ACCEPTED. It is a legal degenerate: it draws nothing under the
    // diamond-exit rule and possibly one pixel under Bresenham, which is backend-defined and stated
    // rather than refused -- refusing it would make wireBox on a flat box behave surprisingly.
    CHECK(batch.line(Vec3{2.0F, 2.0F, 2.0F}, Vec3{2.0F, 2.0F, 2.0F}, white));
    CHECK(batch.lineCount() == 1U);
    CHECK(batch.rejectedLines() == 3U);  // still 3

    // A NaN COLOUR CHANNEL IS NOT A REJECTION -- packDebugColor is total, which is why it is total.
    CHECK(batch.line(Vec3{}, Vec3{1.0F, 0.0F, 0.0F}, Vec4{NAN_F, 1.0F, 1.0F, 1.0F}));
    CHECK(batch.lineCount() == 2U);
    CHECK(batch.rejectedLines() == 3U);
}

TEST_CASE("render debug draw: lines(span) pushes pairwise through the same gate (DD11)") {
    const Vec4 white{1.0F, 1.0F, 1.0F, 1.0F};
    const std::uint32_t packed = rd::packDebugColor(white);

    SUBCASE("six vertices are three lines") {
        rd::DebugDrawBatch batch{{}};
        std::array<rd::DebugLineVertex, 6> vertices{};
        for (std::size_t i = 0; i < 6U; ++i) {
            vertices[i] = rd::DebugLineVertex{.position = Vec3{static_cast<float>(i), 0.0F, 0.0F}, .rgba = packed};
        }
        CHECK(batch.lines(vertices) == 3U);
        CHECK(batch.lineCount() == 3U);
        CHECK(batch.rejectedLines() == 0U);
        // The rgba is taken VERBATIM -- this overload's vertices are already packed.
        CHECK(batch.lineVertices(rd::DebugDepth::Tested)[0].rgba == packed);
    }
    SUBCASE("an odd count pushes floor(n/2) and counts ONE rejection") {
        rd::DebugDrawBatch batch{{}};
        std::array<rd::DebugLineVertex, 5> vertices{};
        CHECK(batch.lines(vertices) == 2U);
        CHECK(batch.lineCount() == 2U);
        CHECK(batch.rejectedLines() == 1U);  // exactly one, for the whole trailing vertex
    }
    SUBCASE("the budget gate applies") {
        rd::DebugDrawBatch batch{{.maxLines = 3U, .maxBillboards = 4096U}};
        std::array<rd::DebugLineVertex, 8> vertices{};
        CHECK(batch.lines(vertices) == 3U);
        CHECK(batch.lineCount() == 3U);
        CHECK(batch.droppedLines() == 1U);  // the fourth pair
        CHECK(batch.rejectedLines() == 0U);
    }
}

TEST_CASE("render debug draw: wireBox pushes 12 edges over the 8 corners (DD12)") {
    // ASSERTS THE PROPERTY, NEVER THE TABLE. engine/render owns its own corner/edge derivation
    // (it may not include the editor's), so pinning a table here would only prove the table equals
    // itself. What matters is: 12 segments, every one differing in EXACTLY ONE axis, all 8 corners
    // reached, and the model matrix applied to every corner.
    const Vec4 white{1.0F, 1.0F, 1.0F, 1.0F};

    SUBCASE("identity model, the unit box") {
        rd::DebugDrawBatch batch{{}};
        batch.wireBox(Mat4::identity(), Vec3{-1.0F, -1.0F, -1.0F}, Vec3{1.0F, 1.0F, 1.0F}, white);
        CHECK(batch.lineCount() == 12U);
        CHECK(batch.rejectedLines() == 0U);
        CHECK(batch.droppedLines() == 0U);

        const std::span<const rd::DebugLineVertex> v = batch.lineVertices(rd::DebugDepth::Tested);
        REQUIRE(v.size() == 24U);
        std::array<int, 8> cornerHits{};
        for (std::size_t e = 0; e < 12U; ++e) {
            const Vec3 a = v[e * 2U].position;
            const Vec3 b = v[(e * 2U) + 1U].position;
            // every component is -1 or +1, exactly
            for (const float c : {a.x, a.y, a.z, b.x, b.y, b.z}) {
                CHECK((c == -1.0F || c == 1.0F));
            }
            // exactly one axis differs
            const int differing =
                static_cast<int>(a.x != b.x) + static_cast<int>(a.y != b.y) + static_cast<int>(a.z != b.z);
            CHECK(differing == 1);
            const auto index = [](Vec3 p) {
                return static_cast<std::size_t>((p.x > 0.0F ? 1 : 0) | (p.y > 0.0F ? 2 : 0) | (p.z > 0.0F ? 4 : 0));
            };
            ++cornerHits[index(a)];
            ++cornerHits[index(b)];
        }
        // Each of the 8 corners meets exactly 3 edges, so each is hit exactly 3 times. That is a
        // stronger statement than "all 8 appear" and it is what a duplicated edge would break.
        for (const int hits : cornerHits) {
            CHECK(hits == 3);
        }
    }
    SUBCASE("a translated model shifts every corner") {
        rd::DebugDrawBatch batch{{}};
        batch.wireBox(engine::translation(Vec3{1.0F, 2.0F, 3.0F}), Vec3{-1.0F, -1.0F, -1.0F}, Vec3{1.0F, 1.0F, 1.0F},
                      white);
        REQUIRE(batch.lineCount() == 12U);
        for (const rd::DebugLineVertex& vertex : batch.lineVertices(rd::DebugDepth::Tested)) {
            CHECK((vertex.position.x == 0.0F || vertex.position.x == 2.0F));
            CHECK((vertex.position.y == 1.0F || vertex.position.y == 3.0F));
            CHECK((vertex.position.z == 2.0F || vertex.position.z == 4.0F));
        }
    }
}

TEST_CASE("render debug draw: wireCircle's chords are closed, planar and on the radius (DD13)") {
    rd::DebugDrawBatch batch{{}};
    batch.wireCircle(Vec3{}, Vec3{0.0F, 1.0F, 0.0F}, 2.0F, Vec4{1.0F, 1.0F, 1.0F, 1.0F}, 4U);
    REQUIRE(batch.lineCount() == 4U);
    const std::span<const rd::DebugLineVertex> v = batch.lineVertices(rd::DebugDepth::Tested);
    REQUIRE(v.size() == 8U);

    // TOLERANCE STATED WITH THE ASSERTION: cos/sin are not exact, so 1e-5 relative to a radius of 2
    // is the epsilon, and it is part of the claim rather than a fudge applied afterwards.
    constexpr float EPS = 1e-5F;
    for (const rd::DebugLineVertex& vertex : v) {
        CHECK(std::abs(vertex.position.y) <= EPS);  // planar: normal is +Y
        const float r = std::sqrt((vertex.position.x * vertex.position.x) + (vertex.position.z * vertex.position.z));
        CHECK(std::abs(r - 2.0F) <= EPS * 2.0F);  // on the radius
    }
    // CONSECUTIVE and CLOSED: chord k ends where chord k+1 begins, and chord 3 ends at chord 0's start.
    for (std::size_t e = 0; e < 4U; ++e) {
        const Vec3 end = v[(e * 2U) + 1U].position;
        const Vec3 nextStart = v[((e + 1U) % 4U) * 2U].position;
        CHECK(std::abs(end.x - nextStart.x) <= EPS * 2.0F);
        CHECK(std::abs(end.z - nextStart.z) <= EPS * 2.0F);
    }

    SUBCASE("segments clamp to [3, 256]") {
        rd::DebugDrawBatch low{{}};
        low.wireCircle(Vec3{}, Vec3{0.0F, 1.0F, 0.0F}, 1.0F, Vec4{1, 1, 1, 1}, 2U);
        CHECK(low.lineCount() == 3U);
        rd::DebugDrawBatch high{{}};
        high.wireCircle(Vec3{}, Vec3{0.0F, 1.0F, 0.0F}, 1.0F, Vec4{1, 1, 1, 1}, 1000U);
        CHECK(high.lineCount() == 256U);
    }
}

TEST_CASE("render debug draw: wireCircle rejects five degenerate inputs and pushes nothing (DD14)") {
    const Vec4 white{1.0F, 1.0F, 1.0F, 1.0F};
    const std::array<std::pair<Vec3, float>, 5> bad{{
        {Vec3{0.0F, 0.0F, 0.0F}, 1.0F},   // a zero normal
        {Vec3{NAN_F, 0.0F, 0.0F}, 1.0F},  // a NaN normal
        {Vec3{0.0F, 1.0F, 0.0F}, 0.0F},   // radius 0
        {Vec3{0.0F, 1.0F, 0.0F}, -1.0F},  // a negative radius
        {Vec3{0.0F, 1.0F, 0.0F}, NAN_F},  // a NaN radius
    }};
    for (const auto& [normal, radius] : bad) {
        rd::DebugDrawBatch batch{{}};
        batch.wireCircle(Vec3{}, normal, radius, white);
        CHECK(batch.lineCount() == 0U);
        CHECK(batch.rejectedLines() == 1U);  // ONE rejection each, not one per would-be segment
        CHECK(batch.droppedLines() == 0U);
        CHECK(batch.empty());
    }
}

TEST_CASE("render debug draw: wireSphere is three circles on the radius (DD15)") {
    rd::DebugDrawBatch batch{{}};
    constexpr std::uint32_t SEGMENTS = 16U;
    const Vec3 center{1.0F, -2.0F, 0.5F};
    batch.wireSphere(center, 3.0F, Vec4{1.0F, 1.0F, 1.0F, 1.0F}, SEGMENTS);
    CHECK(batch.lineCount() == 3U * SEGMENTS);

    constexpr float EPS = 1e-4F;  // three chained cos/sin plus a translate; 1e-4 at radius 3
    for (const rd::DebugLineVertex& v : batch.lineVertices(rd::DebugDepth::Tested)) {
        const Vec3 d{v.position.x - center.x, v.position.y - center.y, v.position.z - center.z};
        CHECK(std::abs(std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z)) - 3.0F) <= EPS);
    }
}

TEST_CASE("render debug draw: billboard accepts, drops at the budget and rejects four ways (DD16)") {
    const Vec4 white{1.0F, 1.0F, 1.0F, 1.0F};
    SUBCASE("the budget") {
        rd::DebugDrawBatch batch{{.maxLines = 32768U, .maxBillboards = 2U}};
        CHECK(batch.billboard(Vec3{0.0F, 0.0F, 0.0F}, 16.0F, white));
        CHECK(batch.billboard(Vec3{1.0F, 0.0F, 0.0F}, 16.0F, white));
        CHECK_FALSE(batch.billboard(Vec3{2.0F, 0.0F, 0.0F}, 16.0F, white));
        CHECK(batch.billboardCount() == 2U);
        CHECK(batch.droppedBillboards() == 1U);
        CHECK(batch.rejectedBillboards() == 0U);
        // First-pushed-wins, pinned by position rather than by count alone.
        CHECK(batch.billboards(rd::DebugDepth::Tested)[0].center.x == 0.0F);
        CHECK(batch.billboards(rd::DebugDepth::Tested)[1].center.x == 1.0F);
    }
    SUBCASE("the four rejections") {
        rd::DebugDrawBatch batch{{}};
        CHECK_FALSE(batch.billboard(Vec3{NAN_F, 0.0F, 0.0F}, 16.0F, white));
        CHECK_FALSE(batch.billboard(Vec3{}, 0.0F, white));
        CHECK_FALSE(batch.billboard(Vec3{}, -4.0F, white));
        CHECK_FALSE(batch.billboard(Vec3{}, NAN_F, white));
        CHECK_FALSE(batch.billboard(Vec3{}, 16.0F, white, Vec2{NAN_F, 0.0F}, Vec2{1.0F, 1.0F}));
        CHECK_FALSE(batch.billboard(Vec3{}, 16.0F, white, Vec2{0.0F, 0.0F}, Vec2{1.0F, INF_F}));
        CHECK(batch.rejectedBillboards() == 6U);
        CHECK(batch.droppedBillboards() == 0U);
        CHECK(batch.billboardCount() == 0U);
    }
}

TEST_CASE("render debug draw: expandBillboard's six vertices and its top-is-top UV rule (DD17)") {
    // Seed S21's ONLY automated witness. The picture half of that seed (a mirrored atlas cell whose
    // CENTRE still reads the same colour) is declared on the validation page, row 6.
    const rd::DebugBillboard record{.center = Vec3{7.0F, 8.0F, 9.0F},
                                    .sizePx = 24.0F,
                                    .uvMin = Vec2{0.25F, 0.125F},
                                    .uvMax = Vec2{0.75F, 0.625F},
                                    .rgba = 0x11223344U};
    std::array<rd::DebugBillboardVertex, 6> out{};
    rd::expandBillboard(record, out);

    for (const rd::DebugBillboardVertex& v : out) {
        // The record's own fields, VERBATIM, on all six.
        CHECK(v.center.x == 7.0F);
        CHECK(v.center.y == 8.0F);
        CHECK(v.center.z == 9.0F);
        CHECK(v.rgba == 0x11223344U);
        CHECK(v.sizePx == 24.0F);
        // Corners are exactly +/-0.5 on both axes -- the shader multiplies by sizePx, so a corner of
        // +/-1 would draw a sprite twice the requested width with nothing complaining.
        CHECK((v.corner.x == -0.5F || v.corner.x == 0.5F));
        CHECK((v.corner.y == -0.5F || v.corner.y == 0.5F));
        // TOP IS TOP: corner.y == +0.5 is the top of the sprite on screen and carries uvMin.y.
        CHECK(v.uv.y == (v.corner.y > 0.0F ? record.uvMin.y : record.uvMax.y));
        CHECK(v.uv.x == (v.corner.x < 0.0F ? record.uvMin.x : record.uvMax.x));
    }

    // Two triangles over four distinct corners: the shared diagonal appears twice, the other two once.
    int topLeft = 0;
    int topRight = 0;
    int bottomRight = 0;
    int bottomLeft = 0;
    for (const rd::DebugBillboardVertex& v : out) {
        if (v.corner.x < 0.0F && v.corner.y > 0.0F) {
            ++topLeft;
        }
        if (v.corner.x > 0.0F && v.corner.y > 0.0F) {
            ++topRight;
        }
        if (v.corner.x > 0.0F && v.corner.y < 0.0F) {
            ++bottomRight;
        }
        if (v.corner.x < 0.0F && v.corner.y < 0.0F) {
            ++bottomLeft;
        }
    }
    CHECK(topLeft == 2);      // the shared diagonal
    CHECK(bottomRight == 2);  //   "
    CHECK(topRight == 1);
    CHECK(bottomLeft == 1);
    CHECK(topLeft + topRight + bottomRight + bottomLeft == 6);
}

TEST_CASE("render debug draw: clear() empties everything and RETAINS capacity (DD18)") {
    // INV-1's witness. There is no capacity() accessor on the batch -- deliberately, it would be an
    // accessor that exists only for a test -- so the property is asserted by EFFECT: fill the batch
    // to its budget, clear it, fill it again, and observe that the SAME number is accepted with no
    // drop. A clear that shrank would still accept them (vector regrows), so the discriminating half
    // is the SPAN'S DATA POINTER, which a reallocation moves.
    rd::DebugDrawBatch batch{{.maxLines = 8U, .maxBillboards = 4U}};
    for (int i = 0; i < 8; ++i) {
        CHECK(batch.line(Vec3{static_cast<float>(i), 0.0F, 0.0F}, Vec3{}, Vec4{1, 1, 1, 1}));
    }
    const void* const before = batch.lineVertices(rd::DebugDepth::Tested).data();
    CHECK(batch.lineCount() == 8U);

    batch.clear();
    CHECK(batch.empty());
    CHECK(batch.lineCount() == 0U);
    CHECK(batch.droppedLines() == 0U);
    CHECK(batch.rejectedLines() == 0U);
    CHECK(batch.billboardCount() == 0U);
    CHECK(batch.droppedBillboards() == 0U);
    CHECK(batch.rejectedBillboards() == 0U);

    for (int i = 0; i < 8; ++i) {
        CHECK(batch.line(Vec3{static_cast<float>(i), 0.0F, 0.0F}, Vec3{}, Vec4{1, 1, 1, 1}));
    }
    CHECK(batch.droppedLines() == 0U);
    // SAME STORAGE: no reallocation happened across the clear-and-refill.
    CHECK(batch.lineVertices(rd::DebugDepth::Tested).data() == before);
}

TEST_CASE("render debug draw: a batch built from an out-of-range budget reports the clamped one (DD19)") {
    const rd::DebugDrawBatch zero{{.maxLines = 0U, .maxBillboards = 0U}};
    CHECK(zero.budget().maxLines == 1U);
    CHECK(zero.budget().maxBillboards == 1U);
    const rd::DebugDrawBatch huge{{.maxLines = rd::DEBUG_DRAW_MAX_LINES_CEILING + 5U,
                                   .maxBillboards = rd::DEBUG_DRAW_MAX_BILLBOARDS_CEILING + 5U}};
    CHECK(huge.budget().maxLines == rd::DEBUG_DRAW_MAX_LINES_CEILING);
    CHECK(huge.budget().maxBillboards == rd::DEBUG_DRAW_MAX_BILLBOARDS_CEILING);
    // ...and the clamped budget is what the GATE uses: one push past 1 is dropped, not accepted.
    rd::DebugDrawBatch one{{.maxLines = 0U, .maxBillboards = 0U}};
    CHECK(one.line(Vec3{}, Vec3{1, 0, 0}, Vec4{1, 1, 1, 1}));
    CHECK_FALSE(one.line(Vec3{}, Vec3{1, 0, 0}, Vec4{1, 1, 1, 1}));
    CHECK(one.droppedLines() == 1U);
}

TEST_CASE("render debug draw: packDebugLineView writes viewProj column-major at offset 0 (DD20)") {
    // Sixteen DISTINCT entries, so a transpose is visible rather than symmetric.
    Mat4 m{};
    float* const raw = m.data();  // Mat4 is 16 contiguous floats, column-major (mat4.hpp)
    for (int i = 0; i < 16; ++i) {
        raw[i] = static_cast<float>(i) + 0.5F;
    }
    const auto block = rd::packDebugLineView(m);
    CHECK(block.size() == 64U);
    CHECK(rd::DEBUG_LINE_VERTEX_UNIFORM_BYTES == 64U);
    std::array<float, 16> readBack{};
    std::memcpy(readBack.data(), block.data(), 64U);
    for (int i = 0; i < 16; ++i) {
        CHECK(readBack[static_cast<std::size_t>(i)] == static_cast<float>(i) + 0.5F);  // NO TRANSPOSE
    }
}

TEST_CASE("render debug draw: packDebugBillboardView writes the matrix, the extent, and ZERO padding (DD21)") {
    Mat4 m{};
    float* const raw = m.data();
    for (int i = 0; i < 16; ++i) {
        raw[i] = static_cast<float>(i) + 0.5F;
    }
    const auto block = rd::packDebugBillboardView(m, engine::rhi::Extent2D{256U, 192U});
    CHECK(block.size() == 80U);
    CHECK(rd::DEBUG_BILLBOARD_VERTEX_UNIFORM_BYTES == 80U);

    std::array<float, 16> matrix{};
    std::memcpy(matrix.data(), block.data(), 64U);
    for (int i = 0; i < 16; ++i) {
        CHECK(matrix[static_cast<std::size_t>(i)] == static_cast<float>(i) + 0.5F);
    }
    std::array<float, 2> extent{};
    std::memcpy(extent.data(), block.data() + 64U, 8U);
    CHECK(extent[0] == 256.0F);
    CHECK(extent[1] == 192.0F);
    // PADDING IS ZERO. An uninitialised padding byte is exactly the class of defect that is invisible
    // everywhere else and reproduces differently per lane. Bytes 72..79 are the shader's `float2 _pad0`.
    for (std::size_t i = 72; i < 80U; ++i) {
        CHECK(block[i] == std::byte{0});
    }
}

TEST_CASE("render debug draw: DebugDrawConfig's defaults are what create() is called with (DD22)") {
    const rd::DebugDrawConfig config{};
    CHECK((config.colorFormat == engine::rhi::TextureFormat::Invalid));  // double parens: scoped enum
    CHECK((config.depthFormat == engine::rhi::TextureFormat::Invalid));
    CHECK(config.lineVertexShaderPath == "res://debug_line.vert");
    CHECK(config.lineFragmentShaderPath == "res://debug_line.frag");
    CHECK(config.billboardVertexShaderPath == "res://debug_billboard.vert");
    CHECK(config.billboardFragmentShaderPath == "res://debug_billboard.frag");
    CHECK((config.budget == rd::DebugDrawBudget{}));
    CHECK(config.budget.maxLines == 32768U);
    CHECK(config.budget.maxBillboards == 4096U);
}

TEST_CASE("render debug draw: the umbrella header carries debug_draw.hpp (DD23)") {
    // TWO ARMS, AND ONLY ONE OF THEM IS A COMPILE FAILURE. Saying which is which is the whole point:
    // TM28 ships the same shape with half of it partial and records itself as "a PARTIAL pin rather
    // than claimed as a full one", and a half that is not a compile failure is worth nothing if it is
    // claimed as one.
    //
    // (a) THE NAMING ARM BELOW IS **NOT** A GENUINE COMPILE FAILURE HERE, and that was MEASURED
    //     rather than assumed: this TU deliberately does not include <aero/render/debug_draw.hpp>,
    //     but it does include "../engine/render/src/debug_draw_pack.hpp" for DD20/DD21, and that
    //     header includes debug_draw.hpp DIRECTLY -- so deleting the umbrella's line leaves the whole
    //     aero_tests target building clean. The arm still asserts something real (the four public
    //     names exist and are usable through the umbrella's transitive closure); it does not assert
    //     the umbrella's own role in supplying them.
    // (b) THE SOURCE-TEXT ARM IS THE REAL PIN, and it is stronger than the compile failure would have
    //     been: it reads render.hpp's own COMMENT-STRIPPED text, so a commented-out include does not
    //     satisfy it either. A missing umbrella entry is otherwise invisible to every consumer that
    //     includes the narrow header directly.
    const std::string umbrella = strippedSourceAt(RENDER_UMBRELLA_PATH);
    REQUIRE_FALSE(umbrella.empty());  // non-vacuity: the path really resolved and the file was read
    CHECK(contains(umbrella, "#include <aero/render/debug_draw.hpp>"));
    // ...and the search can say NO, so a reader that matched everything could not fake the line above.
    CHECK_FALSE(contains(umbrella, "#include <aero/render/does_not_exist.hpp>"));

    [[maybe_unused]] const engine::render::DebugDepth depth = engine::render::DebugDepth::Tested;
    const engine::render::DebugDrawBatch batch{engine::render::DebugDrawBudget{}};
    [[maybe_unused]] const engine::render::DebugDrawConfig config{};
    engine::render::DebugDraw* const gpu = nullptr;
    CHECK(gpu == nullptr);
    CHECK(batch.empty());
}

TEST_CASE("render debug draw: wireBox fills a 12-line remainder exactly and the 13th drops (DD24)") {
    // The helpers push ONE SEGMENT AT A TIME through the same gate, which is what makes a partial
    // wireBox/wireSphere deterministic rather than all-or-nothing.
    rd::DebugDrawBatch batch{{.maxLines = 13U, .maxBillboards = 4096U}};
    CHECK(batch.line(Vec3{}, Vec3{1, 0, 0}, Vec4{1, 1, 1, 1}));  // consume one
    batch.wireBox(Mat4::identity(), Vec3{-1, -1, -1}, Vec3{1, 1, 1}, Vec4{1, 1, 1, 1});
    CHECK(batch.lineCount() == 13U);
    CHECK(batch.droppedLines() == 0U);
    CHECK_FALSE(batch.line(Vec3{}, Vec3{1, 0, 0}, Vec4{1, 1, 1, 1}));
    CHECK(batch.droppedLines() == 1U);

    SUBCASE("a wireBox that STRADDLES the budget is drawn up to the segment it ended on") {
        rd::DebugDrawBatch tight{{.maxLines = 5U, .maxBillboards = 4096U}};
        tight.wireBox(Mat4::identity(), Vec3{-1, -1, -1}, Vec3{1, 1, 1}, Vec4{1, 1, 1, 1});
        CHECK(tight.lineCount() == 5U);
        CHECK(tight.droppedLines() == 7U);  // 12 - 5, counted per refused segment
    }
}

TEST_CASE("render debug draw: the two depth buckets share ONE budget (DD25)") {
    rd::DebugDrawBatch batch{{.maxLines = 4U, .maxBillboards = 4096U}};
    CHECK(batch.line(Vec3{}, Vec3{1, 0, 0}, Vec4{1, 1, 1, 1}, rd::DebugDepth::Tested));
    CHECK(batch.line(Vec3{}, Vec3{1, 0, 0}, Vec4{1, 1, 1, 1}, rd::DebugDepth::Tested));
    CHECK(batch.line(Vec3{}, Vec3{1, 0, 0}, Vec4{1, 1, 1, 1}, rd::DebugDepth::Overlay));
    CHECK(batch.line(Vec3{}, Vec3{1, 0, 0}, Vec4{1, 1, 1, 1}, rd::DebugDepth::Overlay));
    CHECK(batch.lineCount() == 4U);
    CHECK(batch.lineCount(rd::DebugDepth::Tested) == 2U);
    CHECK(batch.lineCount(rd::DebugDepth::Overlay) == 2U);
    // A fifth of EITHER depth drops -- the budget is the batch's, not the bucket's.
    CHECK_FALSE(batch.line(Vec3{}, Vec3{1, 0, 0}, Vec4{1, 1, 1, 1}, rd::DebugDepth::Tested));
    CHECK_FALSE(batch.line(Vec3{}, Vec3{1, 0, 0}, Vec4{1, 1, 1, 1}, rd::DebugDepth::Overlay));
    CHECK(batch.droppedLines() == 2U);
}

TEST_CASE("render debug draw: the HLSL transcribes the C++ contract, pinned as source text (DD26)") {
    // There is no behaviour to observe: the HLSL and the C++ never see each other's tokens, and
    // EVERY mismatched variant compiles, cooks, submits and draws. The JP14/TM29 pattern, reached
    // through AERO_SHADERS_SRC_DIR (the SOURCE tree). UNGATED -- the text exists whether or not
    // AERO_SHADER_TOOLS built it.
    const std::string lineVert = strippedShaderSource("debug_line.vert.hlsl");
    const std::string billboardVert = strippedShaderSource("debug_billboard.vert.hlsl");
    const std::string billboardFrag = strippedShaderSource("debug_billboard.frag.hlsl");

    // (a) NON-VACUITY, first direction: the scan found something at all. Without this a path typo
    //     would make every "contains" below fail with a message naming the needle, not the path.
    REQUIRE_FALSE(lineVert.empty());
    REQUIRE_FALSE(billboardVert.empty());
    REQUIRE_FALSE(billboardFrag.empty());

    // (b) NON-VACUITY, second direction: a deliberately-absent needle IS absent, proving the search
    //     can say "no". uModel is a uniform no debug shader has -- there is no per-line transform.
    CHECK_FALSE(contains(lineVert, "uModel"));
    CHECK_FALSE(contains(billboardVert, "uModel"));

    SUBCASE("the line stage's uniform and its binding space") {
        CHECK(contains(lineVert, "uViewProj"));
        CHECK(contains(lineVert, "register(b0, space1)"));
        // Both attributes are consumed. A stage declaring only TEXCOORD0 would still create a
        // pipeline (a layout may describe attributes the shader ignores) and draw black lines.
        CHECK(contains(lineVert, "TEXCOORD0"));
        CHECK(contains(lineVert, "TEXCOORD1"));
    }
    SUBCASE("the billboard stage declares ALL FIVE inputs, in table order") {
        // F15's Metal rule: SPIRV-Cross numbers MSL [[attribute(n)]] by DECLARATION ORDER, so the
        // five must appear, and appear in this order, or Metal binds the wrong data.
        const std::size_t t0 = billboardVert.find("TEXCOORD0");
        const std::size_t t1 = billboardVert.find("TEXCOORD1");
        const std::size_t t2 = billboardVert.find("TEXCOORD2");
        const std::size_t t3 = billboardVert.find("TEXCOORD3");
        const std::size_t t4 = billboardVert.find("TEXCOORD4");
        REQUIRE(t0 != std::string::npos);
        REQUIRE(t4 != std::string::npos);
        CHECK(t0 < t1);
        CHECK(t1 < t2);
        CHECK(t2 < t3);
        CHECK(t3 < t4);
    }
    SUBCASE("THE SIZING LINE -- the whole point of the file") {
        // Seeds S19 (drop the clip.w) and S20 (1.0 instead of 2.0) both land on ONE statement, so
        // the pin is on that statement rather than on the file: the three tokens must appear
        // TOGETHER, on the same line, or the claim is satisfied by three unrelated occurrences.
        std::istringstream stream{billboardVert};
        std::string line;
        bool sawSizingLine = false;
        while (std::getline(stream, line)) {
            if (line.find("input.sizePx") != std::string::npos) {
                // On the SAME statement: the viewport uniform, the w multiply, and the literal 2.0.
                CHECK(line.find("uViewportPx") != std::string::npos);
                CHECK(line.find("clip.w") != std::string::npos);
                CHECK(line.find("2.0") != std::string::npos);
                CHECK(line.find("input.corner") != std::string::npos);
                sawSizingLine = true;
            }
        }
        CHECK(sawSizingLine);  // anti-vacuity: the loop really found the statement
    }
    SUBCASE("the billboard fragment stage's sampler binding") {
        CHECK(contains(billboardFrag, "register(t0, space2)"));
        CHECK(contains(billboardFrag, "register(s0, space2)"));
        // The vertex colour MULTIPLIES the sample -- a stage returning the sample alone would make
        // every untextured billboard white and every colour argument silently inert.
        CHECK(contains(billboardFrag, "* color"));
    }
}

// ================================================================================================
// Tier 1 -- a real Device, no window. The four pipelines, the flush, and the tree's first assertions
// about the COLOUR OF A PIXEL A PIPELINE PRODUCED.
//
// Gated on AERO_SHADER_TOOLS_ENABLED for the reason render_tonemap_test.cpp's own tier-1 block is:
// DebugDraw::create loads four shaders from build/<preset>/shaders, which only exists when the
// shader toolchain is built. The whole DD battery above runs in every configuration.
//
// THE FRAME OF REFERENCE, and every DG case rests on it. A 256x192 RGBA8Unorm RenderTarget WITH
// DEPTH, quantum 1, cleared to opaque black, drawn through an IDENTITY CameraView (view = proj =
// identity), so world coordinates ARE NDC: a point (x, y, z) lands at column (x+1)/2 * 256 and row
// (1-y)/2 * 192, and row 0 is the TOP (fullscreen.vert.hlsl:22-23's own convention). Line cases aim
// at PIXEL CENTRES -- row r is NDC y = 1 - (2r+1)/192 -- and assert INTERIOR pixels only, never an
// endpoint, because a line through pixel centres is covered under both the diamond-exit rule and
// Bresenham while a line on a pixel BOUNDARY is the one input the two rules disagree about.
// ================================================================================================

#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/core/log.hpp>
    #include <aero/core/vfs.hpp>
    #include <aero/platform/platform.hpp>
    #include <aero/rhi/rhi.hpp>

    #include "rhi_test_support.hpp"

    #include <memory>
    #include <optional>
    #include <vector>

namespace {

constexpr std::uint32_t DG_W = 256;
constexpr std::uint32_t DG_H = 192;

// NDC y of the CENTRE of pixel row r.
[[nodiscard]] constexpr float ndcYForRow(std::uint32_t row) {
    return 1.0F - (((2.0F * static_cast<float>(row)) + 1.0F) / static_cast<float>(DG_H));
}

struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
    [[nodiscard]] bool operator==(const Rgba&) const = default;
};

// WITHOUT THIS every pixel assertion below would have to be written CHECK((a == b)) and would print
// `CHECK( true )` -- which reads a green tick and tells a failure nothing at all. With it, doctest
// decomposes the comparison and prints BOTH SIDES, so the numbers a lane actually produced are in
// the log whether the case passes or fails. It is an operator<<, NOT a toString: an engine-side
// toString on a public header is the ADL trap that hard-errors inside doctest.h.
std::ostream& operator<<(std::ostream& out, const Rgba& value) {
    out << "rgba(" << static_cast<int>(value.r) << ", " << static_cast<int>(value.g) << ", "
        << static_cast<int>(value.b) << ", " << static_cast<int>(value.a) << ")";
    return out;
}

[[nodiscard]] Rgba texelAt(const std::vector<std::byte>& pixels, std::uint32_t row, std::uint32_t column) {
    const std::size_t base = ((static_cast<std::size_t>(row) * DG_W) + column) * 4U;
    return Rgba{static_cast<std::uint8_t>(pixels[base]), static_cast<std::uint8_t>(pixels[base + 1U]),
                static_cast<std::uint8_t>(pixels[base + 2U]), static_cast<std::uint8_t>(pixels[base + 3U])};
}

// The IDENTITY camera: world == NDC. eyePosition is BEHIND the near plane rather than at the origin
// -- no debug shader reads it, but DG6's ForwardRenderer does (V = normalize(eye - worldPos)), and
// an eye ON a drawn surface would make that normalize a 0/0.
[[nodiscard]] engine::render::CameraView identityCamera() {
    return engine::render::CameraView{
        .view = Mat4::identity(), .proj = Mat4::identity(), .eyePosition = Vec3{0.0F, 0.0F, -1.0F}};
}

// beginFrame -> flush -> endFrame -> readback, in one call, so no case can forget the endFrame.
[[nodiscard]] std::vector<std::byte> flushAndRead(engine::rhi::Device& device, engine::render::RenderTarget& target,
                                                  engine::render::DebugDraw& draw,
                                                  const engine::render::CameraView& camera) {
    std::optional<engine::render::Frame> frame = target.beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(frame.has_value());
    draw.flush(*frame, camera);
    REQUIRE(target.endFrame(std::move(*frame)));
    std::vector<std::byte> pixels(static_cast<std::size_t>(DG_W) * DG_H * 4U, std::byte{0xAB});
    REQUIRE(device.readbackTexture(target.colorTexture(), 0, pixels));
    return pixels;
}

// The RAII log-capture guard, copied verbatim from render_tonemap_test.cpp's PP4 -- its destructor
// detaches, which is what a code-review round required there after the bare form was found to be a
// latent use-after-free (log.hpp:92-101: detaching does NOT guarantee the captured state may be
// destroyed).
struct LogCallbackGuard {
    ~LogCallbackGuard() { engine::setLogCallback({}); }
    LogCallbackGuard() = default;
    LogCallbackGuard(const LogCallbackGuard&) = delete;
    LogCallbackGuard& operator=(const LogCallbackGuard&) = delete;
    LogCallbackGuard(LogCallbackGuard&&) = delete;
    LogCallbackGuard& operator=(LogCallbackGuard&&) = delete;
};

// render_tonemap_test.cpp's SingleShaderBackend under a distinct name. DG2 needs the arm where SOME
// shaders load and others do not: with nothing mounted all four handles are invalid and destroying
// an invalid handle is a documented no-op, so an empty mount CANNOT see a create() that forgot to
// release the shaders it DID make.
class SingleShaderPrefixBackend final : public engine::FileSystemBackend {
public:
    SingleShaderPrefixBackend(std::string_view rootDirectory, std::string_view allowedPrefix)
        : inner(rootDirectory), prefix(allowedPrefix) {}

    [[nodiscard]] bool exists(std::string_view relPath) const override {
        return admits(relPath) && inner.exists(relPath);
    }
    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view relPath) const override {
        return admits(relPath) ? inner.fileSize(relPath) : std::nullopt;
    }
    [[nodiscard]] std::optional<engine::ByteBuffer> readFile(std::string_view relPath) const override {
        return admits(relPath) ? inner.readFile(relPath) : std::nullopt;
    }

private:
    [[nodiscard]] bool admits(std::string_view relPath) const { return relPath.starts_with(prefix); }

    engine::DirectoryBackend inner;
    std::string prefix;
};

// The longest run of non-black texels through (row, centreColumn), along the row and down the
// column. The predicate is "any colour channel is lit" rather than "== white", so a partially
// covered edge texel still counts and the +/-1 tolerance stays a rasterisation allowance rather
// than a hiding place for a real error.
[[nodiscard]] bool litAt(const std::vector<std::byte>& pixels, std::uint32_t row, std::uint32_t column) {
    const Rgba texel = texelAt(pixels, row, column);
    return texel.r != 0U || texel.g != 0U || texel.b != 0U;
}

[[nodiscard]] int horizontalRun(const std::vector<std::byte>& pixels, std::uint32_t row, std::uint32_t centre) {
    if (!litAt(pixels, row, centre)) {
        return 0;
    }
    std::uint32_t left = centre;
    while (left > 0U && litAt(pixels, row, left - 1U)) {
        --left;
    }
    std::uint32_t right = centre;
    while (right + 1U < DG_W && litAt(pixels, row, right + 1U)) {
        ++right;
    }
    return static_cast<int>((right - left) + 1U);
}

[[nodiscard]] int verticalRun(const std::vector<std::byte>& pixels, std::uint32_t centre, std::uint32_t column) {
    if (!litAt(pixels, centre, column)) {
        return 0;
    }
    std::uint32_t top = centre;
    while (top > 0U && litAt(pixels, top - 1U, column)) {
        --top;
    }
    std::uint32_t bottom = centre;
    while (bottom + 1U < DG_H && litAt(pixels, bottom + 1U, column)) {
        ++bottom;
    }
    return static_cast<int>((bottom - top) + 1U);
}

// Where a world point lands, computed through the SAME viewProj the flush uses, so DG8's two
// billboard centres are measured rather than guessed.
struct PixelAt {
    std::uint32_t row = 0;
    std::uint32_t column = 0;
};

[[nodiscard]] PixelAt projectToPixel(const engine::render::CameraView& camera, Vec3 world) {
    const engine::Vec4 clip = (camera.proj * camera.view) * engine::Vec4{world.x, world.y, world.z, 1.0F};
    const float ndcX = clip.x / clip.w;
    const float ndcY = clip.y / clip.w;
    return PixelAt{static_cast<std::uint32_t>((0.5F - (ndcY * 0.5F)) * static_cast<float>(DG_H)),
                   static_cast<std::uint32_t>(((ndcX * 0.5F) + 0.5F) * static_cast<float>(DG_W))};
}

}  // namespace

    // The tier-1 preamble, written out per case exactly as render_tonemap_test.cpp does it:
    // AERO_SKIP_OR_FAIL returns from the enclosing function, so it cannot live in a helper.
    // NOTE `.depth = true` -- the ONE field that differs from the tonemap twin, and the reason it
    // differs is the whole task: without depth there is nothing for a Tested line to be tested against.
    #define AERO_DG_PREAMBLE()                                                                     \
        const engine::platform::Context ctx{{.headless = false}};                                  \
        if (!ctx.valid()) {                                                                        \
            AERO_SKIP_OR_FAIL("no real video driver available");                                   \
        }                                                                                          \
        auto device = engine::rhi::Device::create();                                               \
        if (!device.has_value()) {                                                                 \
            AERO_SKIP_OR_FAIL("no GPU device available");                                          \
        }                                                                                          \
        engine::VirtualFileSystem vfs;                                                             \
        vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));                   \
        auto target = engine::render::RenderTarget::create(                                        \
            *device, {DG_W, DG_H},                                                                 \
            {.colorFormat = engine::rhi::TextureFormat::RGBA8Unorm, .depth = true, .quantum = 1}); \
        REQUIRE(target.has_value())

TEST_CASE("render debug draw: create succeeds against both format pairs the tree uses (DG1)") {
    AERO_DG_PREAMBLE();
    auto draw = engine::render::DebugDraw::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(draw.has_value());
    CHECK(draw->budget().maxLines == 32768U);
    CHECK(draw->budget().maxBillboards == 4096U);
    CHECK(draw->flushCount() == 0U);
    CHECK(draw->uploadCount() == 0U);
    CHECK(draw->lastFrameLines() == 0U);
    CHECK(draw->lastFrameDrawCalls() == 0U);
    CHECK_FALSE(draw->hasWarnedBudget());
    CHECK_FALSE(draw->hasWarnedUploadFailure());
    CHECK_FALSE(draw->hasBillboardTexture());
    CHECK(draw->batch().empty());

    SUBCASE("and against the EDITOR's real pair, RGBA16Float + depth") {
        // The pair the viewport actually builds against (post->sceneColorFormat() is RGBA16Float
        // since 3.6.3). DG16 goes further and draws into one; this arm is the create half.
        auto hdr = engine::render::RenderTarget::create(
            *device, {DG_W, DG_H},
            {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
        REQUIRE(hdr.has_value());
        auto hdrDraw = engine::render::DebugDraw::create(
            *device, vfs, {.colorFormat = hdr->colorFormat(), .depthFormat = hdr->depthFormat()});
        CHECK(hdrDraw.has_value());
    }
}

TEST_CASE("render debug draw: create refuses four ways and leaks NOTHING (DG2)") {
    AERO_DG_PREAMBLE();
    const engine::rhi::TextureFormat colorFormat = target->colorFormat();
    const engine::rhi::TextureFormat depthFormat = target->depthFormat();

    CHECK_FALSE(engine::render::DebugDraw::create(
                    *device, vfs, {.colorFormat = engine::rhi::TextureFormat::Invalid, .depthFormat = depthFormat})
                    .has_value());
    CHECK_FALSE(
        engine::render::DebugDraw::create(*device, vfs, {.colorFormat = depthFormat, .depthFormat = depthFormat})
            .has_value());  // a DEPTH format as the colour target
    CHECK_FALSE(engine::render::DebugDraw::create(
                    *device, vfs, {.colorFormat = colorFormat, .depthFormat = engine::rhi::TextureFormat::Invalid})
                    .has_value());
    const engine::VirtualFileSystem emptyVfs;
    CHECK_FALSE(
        engine::render::DebugDraw::create(*device, emptyVfs, {.colorFormat = colorFormat, .depthFormat = depthFormat})
            .has_value());

    SUBCASE("a PARTIAL VFS -- two shaders load, two do not -- and NOTHING leaks") {
        // The arm that matters, and the reason an empty VFS cannot replace it: with nothing mounted
        // all four handles are invalid, and destroying an invalid handle is a documented no-op, so
        // an empty mount CANNOT see a create() that forgot to release the shaders that DID load.
        // ScopedShader is what makes that unspellable; this is its witness.
        std::vector<std::string> warnings;
        const LogCallbackGuard detachOnExit;
        engine::setLogCallback([&warnings](const engine::LogRecord& record) {
            if (record.level >= engine::LogLevel::Warn) {
                warnings.emplace_back(record.message);
            }
        });
        {
            engine::VirtualFileSystem partialVfs;
            partialVfs.mount(std::make_unique<SingleShaderPrefixBackend>(AERO_SHADERS_DIR, "debug_line"));
            CHECK(partialVfs.exists("res://debug_line.vert.json"));
            CHECK_FALSE(partialVfs.exists("res://debug_billboard.vert.json"));
            CHECK_FALSE(engine::render::DebugDraw::create(*device, partialVfs,
                                                          {.colorFormat = colorFormat, .depthFormat = depthFormat})
                            .has_value());
        }
        target.reset();  // holds a Device* and must die FIRST
        device.reset();  // ...then ~Device is what WOULD report a leak
        engine::setLogCallback({});
        // The exact wording comes from sdl_gpu_backend.cpp's ~Impl: "rhi: ~Device releasing N
        // leaked <kind>(s)". A leaked shader is the one this arm exists for; the other four are
        // asserted too because create() builds them in the same function.
        for (const std::string& message : warnings) {
            CHECK(message.find("leaked shader") == std::string::npos);
            CHECK(message.find("leaked graphics pipeline") == std::string::npos);
            CHECK(message.find("leaked texture") == std::string::npos);
            CHECK(message.find("leaked buffer") == std::string::npos);
            CHECK(message.find("leaked sampler") == std::string::npos);
        }
        return;  // `device` is reset; nothing below may run
    }
}

TEST_CASE("render debug draw: an out-of-range budget clamps with ONE WARN and is what gets allocated (DG3)") {
    AERO_DG_PREAMBLE();
    std::vector<std::string> warnings;
    const LogCallbackGuard detachOnExit;
    engine::setLogCallback([&warnings](const engine::LogRecord& record) {
        if (record.level >= engine::LogLevel::Warn) {
            warnings.emplace_back(record.message);
        }
    });
    auto draw = engine::render::DebugDraw::create(*device, vfs,
                                                  {.colorFormat = target->colorFormat(),
                                                   .depthFormat = target->depthFormat(),
                                                   .budget = {.maxLines = 0U, .maxBillboards = 0U}});
    engine::setLogCallback({});
    REQUIRE(draw.has_value());
    CHECK(draw->budget().maxLines == 1U);
    CHECK(draw->budget().maxBillboards == 1U);
    // EXACTLY ONE clamp WARN, naming BOTH numbers -- not one per field, not one per frame.
    int clampWarnings = 0;
    for (const std::string& message : warnings) {
        if (message.find("budget clamped") != std::string::npos) {
            ++clampWarnings;
            CHECK(message.find("requested 0") != std::string::npos);
            CHECK(message.find("allocated 1") != std::string::npos);
        }
    }
    CHECK(clampWarnings == 1);
    // ...and the ALLOCATED budget is what the gate uses.
    CHECK(draw->batch().line(Vec3{}, Vec3{0.5F, 0.0F, 0.0F}, Vec4{1, 1, 1, 1}));
    CHECK_FALSE(draw->batch().line(Vec3{}, Vec3{0.5F, 0.0F, 0.0F}, Vec4{1, 1, 1, 1}));
}

TEST_CASE("render debug draw: an EMPTY flush acquires nothing, uploads nothing and draws nothing (DG4)") {
    AERO_DG_PREAMBLE();
    auto draw = engine::render::DebugDraw::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(draw.has_value());

    // INV-3, and it is what makes "the editor's picture is byte-identical" a MEASURED fact.
    const std::vector<std::byte> pixels = flushAndRead(*device, *target, *draw, identityCamera());
    CHECK(draw->flushCount() == 1U);
    CHECK(draw->uploadCount() == 0U);  // NO command buffer was acquired
    CHECK(draw->lastFrameDrawCalls() == 0U);
    CHECK(draw->lastFrameLines() == 0U);
    CHECK(draw->lastFrameBillboards() == 0U);
    CHECK(draw->lastFrameDroppedLines() == 0U);
    CHECK(draw->lastFrameRejectedLines() == 0U);
    CHECK_FALSE(draw->hasWarnedUploadFailure());
    // ...and the picture is the clear colour everywhere sampled.
    for (const auto& [row, column] : std::array<std::pair<std::uint32_t, std::uint32_t>, 5>{
             {{0U, 0U}, {60U, 128U}, {96U, 128U}, {131U, 128U}, {191U, 255U}}}) {
        CHECK(texelAt(pixels, row, column) == Rgba{0U, 0U, 0U, 255U});
    }
}

TEST_CASE("render debug draw: an Overlay line lands on its row, and the image is UPRIGHT (DG5)") {
    AERO_DG_PREAMBLE();
    auto draw = engine::render::DebugDraw::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(draw.has_value());

    // Row 60, upper half. Aimed at the row's PIXEL CENTRE so both rasterisation rules agree, and the
    // segment spans x in [-0.9, +0.9] so every column asserted below is INTERIOR.
    constexpr std::uint32_t ROW = 60U;
    const float y = ndcYForRow(ROW);
    CHECK(draw->batch().line(Vec3{-0.9F, y, 0.5F}, Vec3{0.9F, y, 0.5F}, Vec4{0.0F, 1.0F, 0.0F, 1.0F},
                             engine::render::DebugDepth::Overlay));
    const std::vector<std::byte> pixels = flushAndRead(*device, *target, *draw, identityCamera());

    CHECK(draw->lastFrameLines() == 1U);
    CHECK(draw->uploadCount() == 1U);
    CHECK(draw->lastFrameDrawCalls() == 1U);

    // ON the row: exactly green, at five interior columns. Not "greenish" -- EXACT, because the
    // fragment stage is a passthrough and the target is unorm, so 1.0 encodes to 255 with no curve.
    for (const std::uint32_t column : {32U, 64U, 128U, 192U, 224U}) {
        CHECK(texelAt(pixels, ROW, column) == Rgba{0U, 255U, 0U, 255U});
    }
    // OFF the row, four ways, each a different failure it would catch:
    CHECK(texelAt(pixels, ROW + 8U, 128U) == Rgba{0U, 0U, 0U, 255U});  // a fat/offset line
    CHECK(texelAt(pixels, ROW - 8U, 128U) == Rgba{0U, 0U, 0U, 255U});  //   "
    CHECK(texelAt(pixels, ROW, 4U) == Rgba{0U, 0U, 0U, 255U});         // past the segment's end
    // THE MIRROR ROW. A vertically flipped readback, or a flipped pipeline, would light THIS row
    // instead of ROW -- and nothing else in this file would notice. 191 - 60 = 131.
    CHECK(texelAt(pixels, 131U, 128U) == Rgba{0U, 0U, 0U, 255U});
}

TEST_CASE("render debug draw: a Tested line is hidden by geometry and an Overlay one is not (DG6)") {
    AERO_DG_PREAMBLE();
    auto draw = engine::render::DebugDraw::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(draw.has_value());
    auto forward = engine::render::ForwardRenderer::create(
        *device, vfs,
        {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat(), .shadowMapResolution = 0});
    REQUIRE(forward.has_value());

    // ONE Cube primitive, flat red, under the identity camera. Everything that could vary is pinned
    // OFF so the cube's colour is a constant this case can assert EXACTLY: shadows off, culling off,
    // directional intensity 0 (with a UNIT direction, so nothing normalizes a zero vector),
    // ambient = {1,1,1}, the default material (white dielectric, metallic 0, emissive black). The
    // fragment then reduces to ambient * baseColor * instanceColor = (1, 0, 0).
    //
    // model puts the cube at x, y in [-0.5, +0.5] and z in [0, 0.9]. The DEPTH span is what matters
    // and why 0.9 rather than 1: whichever of the two z-facing quads survives back-face culling
    // writes a depth <= 0.9, so a Tested line at z = 0.99 is behind it either way and the case does
    // not rest on which face wins.
    const Mat4 model = engine::translation(Vec3{0.0F, 0.0F, 0.45F}) * engine::scaling(Vec3{1.0F, 1.0F, 0.9F});
    engine::render::MeshInstance instance{};
    instance.primitive = engine::render::PrimitiveId::Cube;
    instance.model = model;
    instance.mvp = model;  // viewProj is the identity, so mvp == model (the cullingEnabled contract)
    instance.normalMatrix = Mat4::identity();
    instance.color = Vec3{1.0F, 0.0F, 0.0F};

    const engine::render::CameraView camera = identityCamera();
    engine::render::RenderView view;
    view.camera = camera;
    view.instances = std::span{&instance, 1};
    view.ambient = Vec3::one();
    view.directional = {.direction = Vec3{0.0F, -1.0F, 0.0F}, .color = Vec3::one(), .intensity = 0.0F};
    view.cullingEnabled = false;
    view.shadowsEnabled = false;

    constexpr std::uint32_t ROW = 60U;
    const float y = ndcYForRow(ROW);
    // Column 128 is NDC x ~ +0.004, INSIDE the cube's |x| <= 0.5 face; column 240 is ~ +0.877,
    // outside it. Row 60 is NDC y ~ +0.370, inside |y| <= 0.5.
    const auto renderOnce = [&](const std::vector<std::pair<Vec4, engine::render::DebugDepth>>& lines, float lineZ,
                                float xMin, float xMax) {
        std::optional<engine::render::Frame> frame = target->beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
        REQUIRE(frame.has_value());
        forward->draw(*frame, view);
        for (const auto& [colour, depthMode] : lines) {
            draw->batch().line(Vec3{xMin, y, lineZ}, Vec3{xMax, y, lineZ}, colour, depthMode);
        }
        draw->flush(*frame, camera);
        REQUIRE(target->endFrame(std::move(*frame)));
        std::vector<std::byte> pixels(static_cast<std::size_t>(DG_W) * DG_H * 4U, std::byte{0xAB});
        REQUIRE(device->readbackTexture(target->colorTexture(), 0, pixels));
        return pixels;
    };

    SUBCASE("Tested: hidden where the cube is, visible where it is not") {
        const std::vector<std::byte> pixels =
            renderOnce({{Vec4{0.0F, 1.0F, 0.0F, 1.0F}, engine::render::DebugDepth::Tested}}, 0.99F, -0.9F, 0.9F);
        CHECK(texelAt(pixels, ROW, 128U) == Rgba{255U, 0U, 0U, 255U});  // HIDDEN: the cube wins
        CHECK(texelAt(pixels, ROW, 240U) == Rgba{0U, 255U, 0U, 255U});  // VISIBLE: outside the cube
    }
    SUBCASE("Overlay: visible THROUGH the cube") {
        const std::vector<std::byte> pixels =
            renderOnce({{Vec4{0.0F, 1.0F, 0.0F, 1.0F}, engine::render::DebugDepth::Overlay}}, 0.99F, -0.9F, 0.9F);
        CHECK(texelAt(pixels, ROW, 128U) == Rgba{0U, 255U, 0U, 255U});  // the cube does not hide it
        CHECK(texelAt(pixels, ROW, 240U) == Rgba{0U, 255U, 0U, 255U});
    }
    SUBCASE("Tested and Overlay coincident: the Overlay colour wins") {
        // Tested draws FIRST, Overlay SECOND, per primitive -- overlay content is meant to be seen
        // over everything, INCLUDING tested lines, and drawing it last is what makes that true.
        // Both lines sit at the same z and OUTSIDE the cube (x in [0.6, 0.95]), so the depth test
        // does not decide the outcome. With the order inverted, the blue Tested line is drawn last,
        // passes LessOrEqual at equal depth, and overwrites.
        const std::vector<std::byte> pixels =
            renderOnce({{Vec4{0.0F, 0.0F, 1.0F, 1.0F}, engine::render::DebugDepth::Tested},
                        {Vec4{0.0F, 1.0F, 0.0F, 1.0F}, engine::render::DebugDepth::Overlay}},
                       0.99F, 0.6F, 0.95F);
        CHECK(texelAt(pixels, ROW, 224U) == Rgba{0U, 255U, 0U, 255U});
        CHECK(draw->lastFrameDrawCalls() == 2U);  // one per bucket, both reached
    }
}

TEST_CASE("render debug draw: a half-alpha white line reads mid-grey and the alpha stays 255 (DG7)") {
    AERO_DG_PREAMBLE();
    auto draw = engine::render::DebugDraw::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(draw.has_value());
    constexpr std::uint32_t ROW = 60U;
    const float y = ndcYForRow(ROW);
    draw->batch().line(Vec3{-0.9F, y, 0.5F}, Vec3{0.9F, y, 0.5F}, Vec4{1.0F, 1.0F, 1.0F, 0.5F},
                       engine::render::DebugDepth::Overlay);
    const std::vector<std::byte> pixels = flushAndRead(*device, *target, *draw, identityCamera());

    for (const std::uint32_t column : {64U, 128U, 192U}) {
        const Rgba texel = texelAt(pixels, ROW, column);
        // COLOUR: within +/-1 of 127.5, and the +/-1 IS the assertion. packDebugColor(0.5) is
        // lround(127.5) = 128, so the source alpha on the wire is 128/255 = 0.50196 and the blend is
        // 1.0*0.50196 + 0.0*0.49804 = 0.50196 -> 128. Measured 128 on Metal; 127 is allowed because
        // UNORM rounding at almost exactly one half is backend-defined. 255 (blending off) is not.
        CHECK(texel.r >= 127U);
        CHECK(texel.r <= 128U);
        CHECK(texel.g == texel.r);
        CHECK(texel.b == texel.r);
        // ALPHA: EXACTLY 255, no tolerance. INV-6: the clear alpha is 1, so
        // dstA*(1-srcA) + srcA*1 = 1*(1-0.5) + 0.5*1 = 1. A srcAlphaFactor of SrcAlpha instead of
        // One would give 1*0.5 + 0.5*0.5 = 0.75 -> 191, which this catches and the colour does not.
        CHECK(texel.a == 255U);
    }
}

TEST_CASE("render debug draw: two billboards at different depths measure the same width (DG8)") {
    AERO_DG_PREAMBLE();
    auto draw = engine::render::DebugDraw::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(draw.has_value());

    // A REAL perspective camera -- the identity camera cannot show this, because with no perspective
    // divide every w is 1 and the clip.w multiply is invisible. fovY is in RADIANS (transform.hpp's
    // D6); degrees here would put the two centres somewhere else entirely.
    const Vec3 eye{0.0F, 0.0F, 5.0F};
    const engine::render::CameraView camera{
        .view = engine::lookAt(eye, Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}),
        .proj = engine::perspective(engine::radians(60.0F), 4.0F / 3.0F, 0.1F, 100.0F),
        .eyePosition = eye};
    // Two 16 px billboards at very different distances: 2 units from the eye and 45. They are
    // separated VERTICALLY so the two horizontal runs can never touch, and their pixel centres are
    // PROJECTED rather than guessed.
    const Vec3 nearCenter{0.0F, 0.5F, 3.0F};
    const Vec3 farCenter{0.0F, -10.0F, -40.0F};
    const PixelAt nearAt = projectToPixel(camera, nearCenter);
    const PixelAt farAt = projectToPixel(camera, farCenter);
    REQUIRE(nearAt.row < DG_H);
    REQUIRE(farAt.row < DG_H);
    REQUIRE(nearAt.column < DG_W);
    REQUIRE(farAt.column < DG_W);
    REQUIRE(farAt.row > nearAt.row + 32U);  // no overlap: the two runs are independent

    CHECK(draw->batch().billboard(nearCenter, 16.0F, Vec4{1.0F, 1.0F, 1.0F, 1.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F},
                                  engine::render::DebugDepth::Overlay));
    CHECK(draw->batch().billboard(farCenter, 16.0F, Vec4{1.0F, 1.0F, 1.0F, 1.0F}, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F},
                                  engine::render::DebugDepth::Overlay));
    const std::vector<std::byte> pixels = flushAndRead(*device, *target, *draw, camera);
    CHECK(draw->lastFrameBillboards() == 2U);

    // BOTH read 16, within +/-1: the +/-1 is stated because a half-pixel-aligned quad edge covers 16
    // or 17 columns depending on where the centre lands, and that is rasterisation, not a defect.
    // Dropping the clip.w makes the FAR one collapse to about 1 px; writing 1.0 where the shader
    // says 2.0 makes BOTH read 8. Neither is within any tolerance of 16.
    const int nearWidth = horizontalRun(pixels, nearAt.row, nearAt.column);
    const int farWidth = horizontalRun(pixels, farAt.row, farAt.column);
    const int nearHeight = verticalRun(pixels, nearAt.row, nearAt.column);
    const int farHeight = verticalRun(pixels, farAt.row, farAt.column);
    CHECK(std::abs(nearWidth - 16) <= 1);
    CHECK(std::abs(farWidth - 16) <= 1);
    CHECK(std::abs(nearHeight - 16) <= 1);
    CHECK(std::abs(farHeight - 16) <= 1);
    CHECK(std::abs(nearWidth - farWidth) <= 1);  // THE CLAIM: the same size at any distance
}

TEST_CASE("render debug draw: a set atlas is sampled, and an invalid handle falls back to white (DG9)") {
    AERO_DG_PREAMBLE();
    auto draw = engine::render::DebugDraw::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(draw.has_value());

    // A 2x2 atlas: red, green / blue, white, uploaded row 0 first (RU10's own convention).
    const engine::rhi::TextureHandle atlas = device->createTexture({.format = engine::rhi::TextureFormat::RGBA8Unorm,
                                                                    .usage = engine::rhi::TextureUsage::Sampler,
                                                                    .width = 2,
                                                                    .height = 2});
    REQUIRE(atlas.valid());
    const std::array<std::uint8_t, 16> texels{255U, 0U, 0U,   255U, 0U,   255U, 0U,   255U,
                                              0U,   0U, 255U, 255U, 255U, 255U, 255U, 255U};
    REQUIRE(device->uploadTexture(atlas, 0, std::as_bytes(std::span{texels})));
    // NEAREST, so the centre of a cell samples exactly that cell with no bleed from its neighbours.
    const engine::rhi::SamplerHandle sampler =
        device->createSampler({.minFilter = engine::rhi::Filter::Nearest,
                               .magFilter = engine::rhi::Filter::Nearest,
                               .mipmapMode = engine::rhi::MipmapMode::Nearest,
                               .addressU = engine::rhi::AddressMode::ClampToEdge,
                               .addressV = engine::rhi::AddressMode::ClampToEdge,
                               .addressW = engine::rhi::AddressMode::ClampToEdge});
    REQUIRE(sampler.valid());

    SUBCASE("the atlas's top-right cell is GREEN") {
        draw->setBillboardTexture(atlas, sampler);
        CHECK(draw->hasBillboardTexture());
        // uvMin = {0.5, 0}, uvMax = {1, 0.5} selects the TOP-RIGHT texel, which is green -- and it is
        // green only because uvMin.y is the TOP (DD17's rule) applied consistently in the shader.
        draw->batch().billboard(Vec3{0.0F, 0.0F, 0.5F}, 32.0F, Vec4{1.0F, 1.0F, 1.0F, 1.0F}, Vec2{0.5F, 0.0F},
                                Vec2{1.0F, 0.5F}, engine::render::DebugDepth::Overlay);
        const std::vector<std::byte> pixels = flushAndRead(*device, *target, *draw, identityCamera());
        CHECK(texelAt(pixels, DG_H / 2U, DG_W / 2U) == Rgba{0U, 255U, 0U, 255U});
    }
    SUBCASE("an INVALID handle falls back to the built-in 1x1 white") {
        draw->setBillboardTexture({}, {});
        CHECK_FALSE(draw->hasBillboardTexture());
        draw->batch().billboard(Vec3{0.0F, 0.0F, 0.5F}, 32.0F, Vec4{1.0F, 1.0F, 1.0F, 1.0F}, Vec2{0.5F, 0.0F},
                                Vec2{1.0F, 0.5F}, engine::render::DebugDepth::Overlay);
        const std::vector<std::byte> pixels = flushAndRead(*device, *target, *draw, identityCamera());
        // WHITE, not green: the default texel is sampled at every UV, so the rect is irrelevant.
        CHECK(texelAt(pixels, DG_H / 2U, DG_W / 2U) == Rgba{255U, 255U, 255U, 255U});
    }
    device->destroySampler(sampler);
    device->destroyTexture(atlas);  // BORROWED: the caller destroys them, never DebugDraw
}

TEST_CASE("render debug draw: overflow drops the LATER lines and warns exactly once (DG10)") {
    AERO_DG_PREAMBLE();
    auto draw = engine::render::DebugDraw::create(*device, vfs,
                                                  {.colorFormat = target->colorFormat(),
                                                   .depthFormat = target->depthFormat(),
                                                   .budget = {.maxLines = 8U, .maxBillboards = 4096U}});
    REQUIRE(draw.has_value());
    std::vector<std::string> warnings;
    const LogCallbackGuard detachOnExit;
    engine::setLogCallback([&warnings](const engine::LogRecord& record) {
        if (record.level >= engine::LogLevel::Warn) {
            warnings.emplace_back(record.message);
        }
    });

    const auto pushOverflowing = [&draw] {
        for (std::uint32_t row = 40U; row < 48U; ++row) {  // 8 white lines, filling the budget
            draw->batch().line(Vec3{-0.9F, ndcYForRow(row), 0.5F}, Vec3{0.9F, ndcYForRow(row), 0.5F},
                               Vec4{1.0F, 1.0F, 1.0F, 1.0F}, engine::render::DebugDepth::Overlay);
        }
        for (std::uint32_t row = 140U; row < 143U; ++row) {  // 3 green lines, past it
            draw->batch().line(Vec3{-0.9F, ndcYForRow(row), 0.5F}, Vec3{0.9F, ndcYForRow(row), 0.5F},
                               Vec4{0.0F, 1.0F, 0.0F, 1.0F}, engine::render::DebugDepth::Overlay);
        }
    };

    pushOverflowing();
    const std::vector<std::byte> first = flushAndRead(*device, *target, *draw, identityCamera());
    // BY EFFECT: rows 40..47 are WHITE and rows 140..142 are BLACK. A `>` instead of `>=` in the
    // gate lights row 140; a missing counter leaves the picture right and the count wrong, which is
    // why both halves are asserted.
    for (std::uint32_t row = 40U; row < 48U; ++row) {
        CHECK(texelAt(first, row, 128U) == Rgba{255U, 255U, 255U, 255U});
    }
    for (std::uint32_t row = 140U; row < 143U; ++row) {
        CHECK(texelAt(first, row, 128U) == Rgba{0U, 0U, 0U, 255U});
    }
    CHECK(draw->lastFrameDroppedLines() == 3U);
    CHECK(draw->lastFrameLines() == 8U);
    CHECK(draw->hasWarnedBudget());
    const std::size_t afterFirst = warnings.size();
    CHECK(afterFirst >= 1U);

    pushOverflowing();
    (void)flushAndRead(*device, *target, *draw, identityCamera());
    CHECK(draw->lastFrameDroppedLines() == 3U);  // still counted every frame...
    engine::setLogCallback({});
    CHECK(warnings.size() == afterFirst);  // ...and NOT warned again
}

TEST_CASE("render debug draw: a NaN endpoint draws nothing, counts one, and warns NOT AT ALL (DG11)") {
    AERO_DG_PREAMBLE();
    auto draw = engine::render::DebugDraw::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(draw.has_value());
    std::vector<std::string> warnings;
    const LogCallbackGuard detachOnExit;
    engine::setLogCallback([&warnings](const engine::LogRecord& record) {
        if (record.level >= engine::LogLevel::Warn) {
            warnings.emplace_back(record.message);
        }
    });
    constexpr std::uint32_t ROW = 60U;
    draw->batch().line(Vec3{NAN_F, ndcYForRow(ROW), 0.5F}, Vec3{0.9F, ndcYForRow(ROW), 0.5F},
                       Vec4{0.0F, 1.0F, 0.0F, 1.0F}, engine::render::DebugDepth::Overlay);
    const std::vector<std::byte> pixels = flushAndRead(*device, *target, *draw, identityCamera());
    engine::setLogCallback({});

    CHECK(texelAt(pixels, ROW, 128U) == Rgba{0U, 0U, 0U, 255U});  // NOTHING was drawn
    CHECK(draw->lastFrameRejectedLines() == 1U);
    CHECK(draw->lastFrameLines() == 0U);
    CHECK(draw->lastFrameDroppedLines() == 0U);
    CHECK(draw->uploadCount() == 0U);  // the batch is EMPTY after the rejection, so the flush is free
    CHECK(warnings.empty());           // rejections are COUNTED, never warned -- a NaN storm at
                                       // 60 Hz would otherwise be a log flood
}

TEST_CASE("render debug draw: a moved-to instance draws and a moved-from one is silent (DG12)") {
    AERO_DG_PREAMBLE();
    auto source = engine::render::DebugDraw::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(source.has_value());
    std::vector<std::string> warnings;
    const LogCallbackGuard detachOnExit;
    engine::setLogCallback([&warnings](const engine::LogRecord& record) {
        if (record.level >= engine::LogLevel::Warn) {
            warnings.emplace_back(record.message);
        }
    });
    {
        engine::render::DebugDraw moved{std::move(*source)};
        constexpr std::uint32_t ROW = 60U;
        moved.batch().line(Vec3{-0.9F, ndcYForRow(ROW), 0.5F}, Vec3{0.9F, ndcYForRow(ROW), 0.5F},
                           Vec4{0.0F, 1.0F, 0.0F, 1.0F}, engine::render::DebugDepth::Overlay);
        const std::vector<std::byte> pixels = flushAndRead(*device, *target, moved, identityCamera());
        CHECK(texelAt(pixels, ROW, 128U) == Rgba{0U, 255U, 0U, 255U});

        // The moved-FROM one is inert and SILENT: flush moves flushCount and nothing else, and logs
        // nothing at all -- a moved-from accessor in this layer never complains.
        const std::size_t before = warnings.size();
        std::optional<engine::render::Frame> frame = target->beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
        REQUIRE(frame.has_value());
        source->flush(*frame, identityCamera());  // NOLINT(bugprone-use-after-move)
        REQUIRE(target->endFrame(std::move(*frame)));
        CHECK(source->flushCount() == 1U);   // NOLINT(bugprone-use-after-move)
        CHECK(source->uploadCount() == 0U);  // NOLINT(bugprone-use-after-move)
        CHECK(warnings.size() == before);
    }
    // `moved` is destroyed here; a DEFAULTED move would have double-freed eight handles, and the
    // destructor of the moved-FROM `source` (below) would be the second free.
    source.reset();
    target.reset();
    device.reset();
    engine::setLogCallback({});
    for (const std::string& message : warnings) {
        CHECK(message.find("leaked") == std::string::npos);
    }
}

TEST_CASE("render debug draw: ten consecutive frames each show THEIR OWN line (DG13)") {
    AERO_DG_PREAMBLE();
    auto draw = engine::render::DebugDraw::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(draw.has_value());
    // Ten frames back to back with NO waitIdle between them, which is what maximises the overlap a
    // cycle=false would corrupt. The honest caveat is that a driver serialising frames makes the
    // broken form pass too -- the CONTRACT rests on SDL's documented model, and this case is the
    // observation, not the proof.
    for (std::uint32_t k = 0; k < 10U; ++k) {
        const std::uint32_t row = 20U + (10U * k);
        draw->batch().line(Vec3{-0.9F, ndcYForRow(row), 0.5F}, Vec3{0.9F, ndcYForRow(row), 0.5F},
                           Vec4{1.0F, 1.0F, 1.0F, 1.0F}, engine::render::DebugDepth::Overlay);
        const std::vector<std::byte> pixels = flushAndRead(*device, *target, *draw, identityCamera());
        CHECK(texelAt(pixels, row, 128U) == Rgba{255U, 255U, 255U, 255U});  // THIS frame's row
        if (k > 0U) {
            const std::uint32_t previous = 20U + (10U * (k - 1U));
            CHECK(texelAt(pixels, previous, 128U) == Rgba{0U, 0U, 0U, 255U});  // never LAST frame's
        }
        CHECK(draw->lastFrameLines() == 1U);
        CHECK(draw->uploadCount() == static_cast<std::size_t>(k) + 1U);
    }
}

TEST_CASE("render debug draw: two instances on one device do not see each other (DG14)") {
    AERO_DG_PREAMBLE();
    auto a = engine::render::DebugDraw::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    auto second = engine::render::RenderTarget::create(
        *device, {DG_W, DG_H}, {.colorFormat = engine::rhi::TextureFormat::RGBA8Unorm, .depth = true, .quantum = 1});
    REQUIRE(a.has_value());
    REQUIRE(second.has_value());
    auto b = engine::render::DebugDraw::create(
        *device, vfs, {.colorFormat = second->colorFormat(), .depthFormat = second->depthFormat()});
    REQUIRE(b.has_value());

    // Each owns its own vertex buffers; the DEVICE's transfer buffer is SHARED and cycled between
    // them, which is exactly the case SDL's record-time reference count exists to make safe.
    a->batch().line(Vec3{-0.9F, ndcYForRow(40U), 0.5F}, Vec3{0.9F, ndcYForRow(40U), 0.5F}, Vec4{1.0F, 0.0F, 0.0F, 1.0F},
                    engine::render::DebugDepth::Overlay);
    b->batch().line(Vec3{-0.9F, ndcYForRow(140U), 0.5F}, Vec3{0.9F, ndcYForRow(140U), 0.5F},
                    Vec4{0.0F, 0.0F, 1.0F, 1.0F}, engine::render::DebugDepth::Overlay);
    const std::vector<std::byte> pixelsA = flushAndRead(*device, *target, *a, identityCamera());
    const std::vector<std::byte> pixelsB = flushAndRead(*device, *second, *b, identityCamera());

    CHECK(texelAt(pixelsA, 40U, 128U) == Rgba{255U, 0U, 0U, 255U});
    CHECK(texelAt(pixelsA, 140U, 128U) == Rgba{0U, 0U, 0U, 255U});  // NOT b's line
    CHECK(texelAt(pixelsB, 140U, 128U) == Rgba{0U, 0U, 255U, 255U});
    CHECK(texelAt(pixelsB, 40U, 128U) == Rgba{0U, 0U, 0U, 255U});  // NOT a's line
}

TEST_CASE("render debug draw: flush CLEARS the batch, so a second frame draws nothing (DG15)") {
    AERO_DG_PREAMBLE();
    auto draw = engine::render::DebugDraw::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(draw.has_value());
    constexpr std::uint32_t ROW = 60U;
    draw->batch().line(Vec3{-0.9F, ndcYForRow(ROW), 0.5F}, Vec3{0.9F, ndcYForRow(ROW), 0.5F},
                       Vec4{1.0F, 1.0F, 1.0F, 1.0F}, engine::render::DebugDepth::Overlay);
    const std::vector<std::byte> first = flushAndRead(*device, *target, *draw, identityCamera());
    CHECK(texelAt(first, ROW, 128U) == Rgba{255U, 255U, 255U, 255U});
    CHECK(draw->batch().empty());  // drained by the flush itself
    CHECK(draw->uploadCount() == 1U);

    // A flush that did not clear would draw the same line again: the SECOND frame, with NO push.
    const std::vector<std::byte> second = flushAndRead(*device, *target, *draw, identityCamera());
    CHECK(texelAt(second, ROW, 128U) == Rgba{0U, 0U, 0U, 255U});  // BLACK everywhere
    CHECK(draw->lastFrameLines() == 0U);
    CHECK(draw->lastFrameDrawCalls() == 0U);
    CHECK(draw->uploadCount() == 1U);  // UNMOVED: an empty flush acquires nothing
    CHECK(draw->flushCount() == 2U);
}

TEST_CASE("render debug draw: the EDITOR's RGBA16Float pair, proven by bytes (DG16)") {
    AERO_DG_PREAMBLE();
    // The pair the viewport really builds against. "It created" is not the claim -- building against
    // the 8-bit OUTPUT format instead is a draw-time failure, not a create failure, so the create
    // half proves nothing on its own. This draws into one and reads it back.
    if (!device->supportsTextureFormat(engine::rhi::TextureFormat::RGBA16Float,
                                       engine::rhi::TextureUsage::Sampler | engine::rhi::TextureUsage::ColorTarget)) {
        AERO_SKIP_OR_FAIL("device does not support RGBA16Float as a sampleable color target");
    }
    auto hdr = engine::render::RenderTarget::create(
        *device, {DG_W, DG_H}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(hdr.has_value());
    auto draw = engine::render::DebugDraw::create(
        *device, vfs, {.colorFormat = hdr->colorFormat(), .depthFormat = hdr->depthFormat()});
    REQUIRE(draw.has_value());

    constexpr std::uint32_t ROW = 60U;
    draw->batch().line(Vec3{-0.9F, ndcYForRow(ROW), 0.5F}, Vec3{0.9F, ndcYForRow(ROW), 0.5F},
                       Vec4{0.0F, 1.0F, 0.0F, 1.0F}, engine::render::DebugDepth::Overlay);
    std::optional<engine::render::Frame> frame = hdr->beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(frame.has_value());
    draw->flush(*frame, identityCamera());
    REQUIRE(hdr->endFrame(std::move(*frame)));

    std::vector<std::byte> pixels(static_cast<std::size_t>(DG_W) * DG_H * 8U, std::byte{0xAB});
    REQUIRE(device->readbackTexture(hdr->colorTexture(), 0, pixels));
    // RU9's half bit patterns: 0.0 -> 0x0000, 1.0 -> 0x3C00. Exact, no decode.
    const std::size_t base = ((static_cast<std::size_t>(ROW) * DG_W) + 128U) * 8U;
    const auto half = [&pixels](std::size_t at) {
        return static_cast<std::uint16_t>(static_cast<std::uint8_t>(pixels[at]) |
                                          (static_cast<std::uint8_t>(pixels[at + 1U]) << 8U));
    };
    CHECK(half(base + 0U) == 0x0000U);  // r
    CHECK(half(base + 2U) == 0x3C00U);  // g
    CHECK(half(base + 4U) == 0x0000U);  // b
    CHECK(half(base + 6U) == 0x3C00U);  // a -- INV-6 again, on the format the editor really uses
}

#endif  // AERO_SHADER_TOOLS_ENABLED

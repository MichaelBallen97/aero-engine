// tests/render_debug_draw_test.cpp — task E.1.1: the debug-draw vocabulary, the pure batch, the
// packers, the shader source pin (DD1-DD26, every configuration), and the GPU DebugDraw (DG1-DG16,
// gated).
//
// The whole DD battery runs with NO device and NO shader toolchain -- that is the point of the pure
// split: everything assertable without a GPU is asserted without one.

#include <aero/render/debug_draw.hpp>
#include <aero/render/render.hpp>  // the umbrella must carry the new header; DD23 uses it in commit 3

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
[[nodiscard]] std::string strippedShaderSource(std::string_view name) {
    std::ifstream file{std::string{AERO_SHADERS_SRC_DIR} + "/" + std::string{name}};
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

// DD20-DD23 land in commit 3 with packDebugLineView, packDebugBillboardView, DebugDrawConfig and
// DebugDraw; the numbering stays contiguous in this file.

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

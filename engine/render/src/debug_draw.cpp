// engine/render/src/debug_draw.cpp — task E.1.1: the pure batch and the GPU DebugDraw.

#include <aero/render/debug_draw.hpp>

#include <algorithm>  // std::clamp, std::max -- MSVC's STL supplies none of <algorithm> transitively
#include <array>
#include <cmath>  // std::isfinite, std::lround, std::cos, std::sin, std::abs
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::render {
namespace {

// Corner i of the local box: bit 0 selects X, bit 1 Y, bit 2 Z -- min when 0, max when 1. The SAME
// bit order the editor's own aabbCorner uses, restated here rather than shared, because
// engine/render may not include anything under /editor (the golden rule) and a debug batch must not
// grow a dependency on the editor's bounds vocabulary to draw a box.
[[nodiscard]] constexpr Vec3 boxCorner(Vec3 lo, Vec3 hi, std::size_t i) noexcept {
    return Vec3{(i & 1U) != 0U ? hi.x : lo.x, (i & 2U) != 0U ? hi.y : lo.y, (i & 4U) != 0U ? hi.z : lo.z};
}

// The twelve edges, derived from that one bit assignment so the two cannot drift:
//   X edges (i, i^1) for i in {0,2,4,6}; Y edges (i, i^2) for {0,1,4,5}; Z edges (i, i^4) for {0,1,2,3}.
struct BoxEdge {
    std::uint8_t a = 0;
    std::uint8_t b = 0;
};
constexpr std::array<BoxEdge, 12> BOX_EDGES{{
    {0, 1},
    {2, 3},
    {4, 5},
    {6, 7},
    {0, 2},
    {1, 3},
    {4, 6},
    {5, 7},
    {0, 4},
    {1, 5},
    {2, 6},
    {3, 7},
}};

[[nodiscard]] bool finite(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
[[nodiscard]] bool finite(Vec2 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y); }

// TOTAL over every DebugDepth value, including Count and anything cast in from outside: an
// out-of-range depth reads the last bucket rather than walking off the array. The same posture as
// debugDepthLabel's fallthrough -- this vocabulary never traps and never has undefined behaviour.
[[nodiscard]] constexpr std::size_t bucketIndex(DebugDepth depth) noexcept {
    const auto raw = static_cast<std::size_t>(depth);
    return raw < DEBUG_DEPTH_COUNT ? raw : DEBUG_DEPTH_COUNT - 1U;
}

constexpr std::uint32_t MIN_CIRCLE_SEGMENTS = 3;
constexpr std::uint32_t MAX_CIRCLE_SEGMENTS = 256;

}  // namespace

std::string_view debugDepthLabel(DebugDepth depth) noexcept {
    switch (depth) {
        case DebugDepth::Tested:
            return "Tested";
        case DebugDepth::Overlay:
            return "Overlay";
        case DebugDepth::Count:
            break;
    }
    return "Unknown";  // total, distinct, never empty; no default: so a new enumerator is -Wswitch
}

std::uint32_t packDebugColor(Vec4 linearRgba) noexcept {
    // TOTAL. NaN is special-cased FIRST: std::clamp(NaN, 0, 1) returns NaN on libc++, and
    // static_cast<int>(NaN) is UB that UBSan traps. +/-inf need no special case of their own beyond
    // this one -- isfinite is false for NaN and for both infinities, and `NaN > 0` is false, so +inf
    // saturates to 255 while -inf and NaN both floor to 0.
    const auto channel = [](float v) -> std::uint32_t {
        if (!std::isfinite(v)) {
            return v > 0.0F ? 255U : 0U;
        }
        return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
    };
    // Bytes r, g, b, a in MEMORY order. On the three little-endian lanes this project targets that is
    // r | g<<8 | b<<16 | a<<24; DD2 asserts the MEMORY order with a memcpy rather than the integer.
    return channel(linearRgba.x) | (channel(linearRgba.y) << 8U) | (channel(linearRgba.z) << 16U) |
           (channel(linearRgba.w) << 24U);
}

Vec4 unpackDebugColor(std::uint32_t rgba) noexcept {
    // DIVIDE by 255, never multiply by a precomputed 1/255. The two are not the same number:
    // k * fl(1/255) rounds TWICE and is bit-unequal to fl(k / 255) for 126 of the 256 byte values,
    // the first at k = 3 (measured, not reasoned). The reciprocal form would make DD4's exact round
    // trip false while looking right to six significant digits, which is the worst kind of wrong.
    constexpr float SCALE = 255.0F;
    return Vec4{static_cast<float>(rgba & 0xFFU) / SCALE, static_cast<float>((rgba >> 8U) & 0xFFU) / SCALE,
                static_cast<float>((rgba >> 16U) & 0xFFU) / SCALE, static_cast<float>((rgba >> 24U) & 0xFFU) / SCALE};
}

DebugDrawBudget clampDebugDrawBudget(DebugDrawBudget requested) noexcept {
    return DebugDrawBudget{std::clamp(requested.maxLines, 1U, DEBUG_DRAW_MAX_LINES_CEILING),
                           std::clamp(requested.maxBillboards, 1U, DEBUG_DRAW_MAX_BILLBOARDS_CEILING)};
}

void expandBillboard(const DebugBillboard& billboard, std::span<DebugBillboardVertex, 6> out) noexcept {
    // THE CORNER/UV MAPPING IS PART OF THE CONTRACT (D7), and DD17 pins it byte for byte.
    // corner.y == +0.5 is the TOP of the sprite on screen (NDC +y is up) and carries uvMin.y, which
    // is the TOP of the texture (the origin SDL_GPU normalises to). Two triangles, four distinct
    // corners; (-0.5, +0.5) and (+0.5, -0.5) each appear twice, being the shared diagonal.
    // CullMode::None on the pipeline, so the winding is not a fact anyone has to get right.
    constexpr std::array<Vec2, 6> CORNERS{{
        {-0.5F, +0.5F},
        {+0.5F, +0.5F},
        {+0.5F, -0.5F},  // triangle 1: TL, TR, BR
        {-0.5F, +0.5F},
        {+0.5F, -0.5F},
        {-0.5F, -0.5F},  // triangle 2: TL, BR, BL
    }};
    for (std::size_t i = 0; i < 6U; ++i) {
        const Vec2 corner = CORNERS[i];
        out[i] = DebugBillboardVertex{
            .center = billboard.center,
            .corner = corner,
            // u from corner.x: -0.5 -> uvMin.x, +0.5 -> uvMax.x.
            // v from corner.y: +0.5 (TOP) -> uvMin.y, -0.5 (BOTTOM) -> uvMax.y.  <-- the flip
            .uv = Vec2{corner.x < 0.0F ? billboard.uvMin.x : billboard.uvMax.x,
                       corner.y > 0.0F ? billboard.uvMin.y : billboard.uvMax.y},
            .rgba = billboard.rgba,
            .sizePx = billboard.sizePx,
        };
    }
}

DebugDrawBatch::DebugDrawBatch(DebugDrawBudget budget) : budgetValue(clampDebugDrawBudget(budget)) {
    // INV-1: ALLOCATE ONCE, HERE, AND NEVER AGAIN. Each line bucket reserves 2 * maxLines vertices
    // and each billboard bucket maxBillboards records -- the FULL budget per bucket, because the two
    // buckets share one budget and either may hold all of it. A push past the budget is refused, so
    // no vector ever grows past its reservation.
    for (std::vector<DebugLineVertex>& bucket : lineBuckets) {
        bucket.reserve(static_cast<std::size_t>(budgetValue.maxLines) * 2U);
    }
    for (std::vector<DebugBillboard>& bucket : billboardBuckets) {
        bucket.reserve(budgetValue.maxBillboards);
    }
}

void DebugDrawBatch::clear() noexcept {
    for (std::vector<DebugLineVertex>& bucket : lineBuckets) {
        bucket.clear();  // capacity RETAINED -- DD18 asserts it
    }
    for (std::vector<DebugBillboard>& bucket : billboardBuckets) {
        bucket.clear();
    }
    droppedLineCount = 0;
    droppedBillboardCount = 0;
    rejectedLineCount = 0;
    rejectedBillboardCount = 0;
}

bool DebugDrawBatch::line(Vec3 a, Vec3 b, Vec4 color, DebugDepth depth) {
    // REJECTED before DROPPED: a non-finite endpoint was never a legal push, so it must not consume
    // budget and must not be counted as a drop. A NaN colour CHANNEL is NOT a rejection --
    // packDebugColor is total, which is exactly why it is total.
    if (!finite(a) || !finite(b)) {
        ++rejectedLineCount;
        return false;
    }
    if (lineCount() >= budgetValue.maxLines) {
        ++droppedLineCount;
        return false;
    }
    const std::uint32_t packed = packDebugColor(color);
    std::vector<DebugLineVertex>& bucket = lineBuckets[bucketIndex(depth)];
    bucket.push_back(DebugLineVertex{.position = a, .rgba = packed});
    bucket.push_back(DebugLineVertex{.position = b, .rgba = packed});
    return true;
}

std::uint32_t DebugDrawBatch::lines(std::span<const DebugLineVertex> vertices, DebugDepth depth) {
    std::uint32_t accepted = 0;
    const std::size_t pairs = vertices.size() / 2U;
    for (std::size_t i = 0; i < pairs; ++i) {
        const DebugLineVertex& a = vertices[i * 2U];
        const DebugLineVertex& b = vertices[(i * 2U) + 1U];
        if (!finite(a.position) || !finite(b.position)) {
            ++rejectedLineCount;
            continue;
        }
        if (lineCount() >= budgetValue.maxLines) {
            ++droppedLineCount;
            continue;
        }
        std::vector<DebugLineVertex>& bucket = lineBuckets[bucketIndex(depth)];
        bucket.push_back(a);  // ALREADY PACKED: this overload takes the caller's rgba verbatim
        bucket.push_back(b);
        ++accepted;
    }
    if ((vertices.size() % 2U) != 0U) {
        ++rejectedLineCount;  // the odd trailing vertex: dropped, and counted ONCE
    }
    return accepted;
}

void DebugDrawBatch::wireBox(const Mat4& model, Vec3 localMin, Vec3 localMax, Vec4 color, DebugDepth depth) {
    std::array<Vec3, 8> corners{};
    for (std::size_t i = 0; i < 8U; ++i) {
        corners[i] = transformPoint(model, boxCorner(localMin, localMax, i));
    }
    for (const BoxEdge edge : BOX_EDGES) {
        line(corners[edge.a], corners[edge.b], color, depth);  // through the SAME gate, one at a time
    }
}

void DebugDrawBatch::wireCircle(Vec3 center, Vec3 normal, float radius, Vec4 color, std::uint32_t segments,
                                DebugDepth depth) {
    if (!finite(center) || !finite(normal) || !std::isfinite(radius) || radius <= 0.0F) {
        ++rejectedLineCount;
        return;
    }
    const Vec3 axis = normalizeOrZero(normal);  // never normalize(): it ASSERTS on a zero vector
    if (lengthSquared(axis) <= 0.0F) {
        ++rejectedLineCount;
        return;
    }
    // The shadowUpAxis idiom: pick the world axis LEAST parallel to `axis`, then two cross products.
    const Vec3 helper = std::abs(axis.y) < 0.9F ? Vec3::unitY() : Vec3::unitX();
    const Vec3 u = normalizeOrZero(cross(helper, axis));
    const Vec3 v = cross(axis, u);
    const std::uint32_t n = std::clamp(segments, MIN_CIRCLE_SEGMENTS, MAX_CIRCLE_SEGMENTS);
    Vec3 previous = center + (u * radius);
    for (std::uint32_t i = 1; i <= n; ++i) {
        const float t = TWO_PI * static_cast<float>(i) / static_cast<float>(n);
        const Vec3 next = center + (u * (std::cos(t) * radius)) + (v * (std::sin(t) * radius));
        line(previous, next, color, depth);  // closed: the last chord returns to the first point
        previous = next;
    }
}

void DebugDrawBatch::wireSphere(Vec3 center, float radius, Vec4 color, std::uint32_t segments, DebugDepth depth) {
    wireCircle(center, Vec3::unitX(), radius, color, segments, depth);
    wireCircle(center, Vec3::unitY(), radius, color, segments, depth);
    wireCircle(center, Vec3::unitZ(), radius, color, segments, depth);
}

bool DebugDrawBatch::billboard(Vec3 center, float sizePx, Vec4 color, Vec2 uvMin, Vec2 uvMax, DebugDepth depth) {
    if (!finite(center) || !std::isfinite(sizePx) || sizePx <= 0.0F || !finite(uvMin) || !finite(uvMax)) {
        ++rejectedBillboardCount;
        return false;
    }
    if (billboardCount() >= budgetValue.maxBillboards) {
        ++droppedBillboardCount;
        return false;
    }
    billboardBuckets[bucketIndex(depth)].push_back(DebugBillboard{
        .center = center, .sizePx = sizePx, .uvMin = uvMin, .uvMax = uvMax, .rgba = packDebugColor(color)});
    return true;
}

std::span<const DebugLineVertex> DebugDrawBatch::lineVertices(DebugDepth depth) const noexcept {
    return lineBuckets[bucketIndex(depth)];
}

std::span<const DebugBillboard> DebugDrawBatch::billboards(DebugDepth depth) const noexcept {
    return billboardBuckets[bucketIndex(depth)];
}

std::uint32_t DebugDrawBatch::lineCount() const noexcept {
    std::size_t vertices = 0;
    for (const std::vector<DebugLineVertex>& bucket : lineBuckets) {
        vertices += bucket.size();
    }
    return static_cast<std::uint32_t>(vertices / 2U);
}

std::uint32_t DebugDrawBatch::lineCount(DebugDepth depth) const noexcept {
    return static_cast<std::uint32_t>(lineBuckets[bucketIndex(depth)].size() / 2U);
}

std::uint32_t DebugDrawBatch::billboardCount() const noexcept {
    std::size_t records = 0;
    for (const std::vector<DebugBillboard>& bucket : billboardBuckets) {
        records += bucket.size();
    }
    return static_cast<std::uint32_t>(records);
}

std::uint32_t DebugDrawBatch::billboardCount(DebugDepth depth) const noexcept {
    return static_cast<std::uint32_t>(billboardBuckets[bucketIndex(depth)].size());
}

std::uint32_t DebugDrawBatch::droppedLines() const noexcept { return droppedLineCount; }
std::uint32_t DebugDrawBatch::droppedBillboards() const noexcept { return droppedBillboardCount; }
std::uint32_t DebugDrawBatch::rejectedLines() const noexcept { return rejectedLineCount; }
std::uint32_t DebugDrawBatch::rejectedBillboards() const noexcept { return rejectedBillboardCount; }
const DebugDrawBudget& DebugDrawBatch::budget() const noexcept { return budgetValue; }
bool DebugDrawBatch::empty() const noexcept { return lineCount() == 0U && billboardCount() == 0U; }

}  // namespace engine::render

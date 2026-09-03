// engine/render/src/debug_draw.cpp — task E.1.1: the pure batch and the GPU DebugDraw.

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/vfs.hpp>
#include <aero/render/debug_draw.hpp>
#include <aero/render/lighting.hpp>  // CameraView
#include <aero/render/renderer.hpp>  // Frame
#include <aero/rhi/descriptors.hpp>
#include <aero/rhi/device.hpp>
#include <aero/rhi/shader_loader.hpp>

#include "debug_draw_pack.hpp"

#include <algorithm>  // std::clamp, std::max -- MSVC's STL supplies none of <algorithm> transitively
#include <array>
#include <cmath>  // std::isfinite, std::lround, std::cos, std::sin, std::abs
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
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

// A SCOPE-OWNED shader handle, copied verbatim from post_process.cpp's own, comment included,
// because the reason it exists is a sabotage finding rather than taste. create() has several exits
// between loading a shader and no longer needing it, and a destroy written at each of them is that
// many chances to forget one. NOTHING IN THIS TREE CAN WITNESS A LEAKED SHADER on its own: ~Device
// logs a WARN and releases it, so ASan sees no process leak and no assertion moves. So the closure
// is STRUCTURAL: with ownership here, forgetting a destroy is UNSPELLABLE rather than merely
// untested. DG2's log-capture arm is the witness that the release still happens.
//
// The valid() guard also keeps a failed load quiet: destroying an invalid handle is a documented
// no-op, but the backend logs an ERROR for it, and a failure path that reports its own cause should
// not also report a non-problem.
class ScopedShader {
public:
    ScopedShader(rhi::Device& owner, rhi::ShaderHandle shader) noexcept : device(&owner), handle(shader) {}
    ~ScopedShader() {
        if (device != nullptr && handle.valid()) {
            device->destroyShader(handle);
        }
    }
    ScopedShader(const ScopedShader&) = delete;
    ScopedShader& operator=(const ScopedShader&) = delete;
    ScopedShader(ScopedShader&&) = delete;
    ScopedShader& operator=(ScopedShader&&) = delete;

    [[nodiscard]] rhi::ShaderHandle get() const noexcept { return handle; }

private:
    rhi::Device* device;
    rhi::ShaderHandle handle;
};

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

// --- the GPU half -------------------------------------------------------------------------------

DebugDraw::DebugDraw(rhi::Device* owner, DebugDrawBudget allocated) : device(owner), batchValue(allocated) {}

std::optional<DebugDraw> DebugDraw::create(rhi::Device& device, const VirtualFileSystem& shaderVfs,
                                           const DebugDrawConfig& config) {
    AERO_PROFILE_ZONE_NAMED("render::DebugDraw::create");
    // 1. Cheap validation, no GPU object yet.
    if (config.colorFormat == rhi::TextureFormat::Invalid || rhi::isDepthFormat(config.colorFormat)) {
        AERO_LOG_ERROR("render::DebugDraw::create - colorFormat must be a non-depth, non-Invalid format");
        return std::nullopt;
    }
    if (config.depthFormat == rhi::TextureFormat::Invalid) {
        AERO_LOG_ERROR(
            "render::DebugDraw::create - depthFormat is required: the pass carries depth "
            "whether or not a pipeline tests it, and all four pipelines must declare it");
        return std::nullopt;
    }
    // 2. The budget: clamped with ONE WARN naming BOTH numbers (the 3.6.2 shadow-resolution posture:
    //    a typo is not a reason to refuse to start).
    const DebugDrawBudget allocated = clampDebugDrawBudget(config.budget);
    if (!(allocated == config.budget)) {
        AERO_LOG_WARN(
            "render::DebugDraw::create - budget clamped: requested {} lines / {} billboards, "
            "allocated {} / {}",
            config.budget.maxLines, config.budget.maxBillboards, allocated.maxLines, allocated.maxBillboards);
    }

    // 3. The four shaders, ALL SCOPE-OWNED: there is no destroy line on any exit from here, which is
    //    what makes "forgot to release the shader that DID load" unspellable rather than untested.
    const ScopedShader lineVs{device, rhi::loadShader(device, shaderVfs, config.lineVertexShaderPath)};
    const ScopedShader lineFs{device, rhi::loadShader(device, shaderVfs, config.lineFragmentShaderPath)};
    const ScopedShader billboardVs{device, rhi::loadShader(device, shaderVfs, config.billboardVertexShaderPath)};
    const ScopedShader billboardFs{device, rhi::loadShader(device, shaderVfs, config.billboardFragmentShaderPath)};
    if (!lineVs.get().valid() || !lineFs.get().valid() || !billboardVs.get().valid() || !billboardFs.get().valid()) {
        AERO_LOG_ERROR(
            "render::DebugDraw::create - shader load failed (are res://debug_line.* / "
            "res://debug_billboard.* cooked?)");
        return std::nullopt;
    }

    // 4. The four pipelines, from TWO descriptors and two one-field copies -- the cullNoneDesc idiom
    //    ForwardRenderer::create already uses, so a pair cannot drift.
    const rhi::VertexBufferLayout lineLayout{.slot = 0, .pitch = 16};  // sizeof(DebugLineVertex)
    const std::array<rhi::VertexAttribute, 2> lineAttrs{{
        {.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offset = 0},
        // THE TREE'S FIRST UByte4Norm ATTRIBUTE. The input assembler normalises, so the shader sees
        // a float4 in [0, 1] and never a 0..255 integer.
        {.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::UByte4Norm, .offset = 12},
    }};
    // Blending ON, and the ALPHA half is chosen so a target whose alpha is already 1 STAYS at 1 after
    // any number of lines: dstA*(1-srcA) + srcA*1 = 1. 3.6.3's INV-6 kept, at the cost of one field.
    // These are the tree's FIRST blend-enabled engine pipelines.
    const rhi::ColorTargetDesc colorTarget{
        .format = config.colorFormat,
        .blend = {.enableBlend = true,
                  .srcColorFactor = rhi::BlendFactor::SrcAlpha,
                  .dstColorFactor = rhi::BlendFactor::OneMinusSrcAlpha,
                  .colorOp = rhi::BlendOp::Add,
                  .srcAlphaFactor = rhi::BlendFactor::One,
                  .dstAlphaFactor = rhi::BlendFactor::OneMinusSrcAlpha,
                  .alphaOp = rhi::BlendOp::Add,
                  .writeMask = rhi::ColorWriteMask::All},
    };
    const rhi::GraphicsPipelineDesc lineTestedDesc{
        .vertexShader = lineVs.get(),
        .fragmentShader = lineFs.get(),
        .vertexBuffers = std::span{&lineLayout, 1},
        .vertexAttributes = lineAttrs,
        // THE TREE'S FIRST NON-TRIANGLE TOPOLOGY. FillMode::Line is deliberately NOT used anywhere:
        // it is triangle wireframe, whose edges are the mesh's triangulation rather than a shape
        // anyone chose, and Metal's triangleFillMode = lines has its own rasterisation semantics.
        .primitiveType = rhi::PrimitiveType::LineList,
        .rasterizer = {.fillMode = rhi::FillMode::Fill,
                       // NOT the engine's Back convention: a debug line has no winding to speak of
                       // and a billboard quad's winding is an artifact of expandBillboard's table.
                       .cullMode = rhi::CullMode::None,
                       .frontFace = rhi::FrontFace::CounterClockwise},
        // LessOrEqual, DELIBERATELY, against the engine's Less convention (R7): a line drawn exactly
        // on a surface's own vertices -- a wire box whose edges coincide with the cube's -- WINS the
        // comparison where a bit-identical depth would otherwise flicker. It changes nothing for a
        // line behind or in front of geometry. Depth WRITE stays off on all four pipelines, so a
        // debug line can never occlude a mesh or another line (INV-5).
        .depthStencil = {.enableDepthTest = true, .enableDepthWrite = false, .compareOp = rhi::CompareOp::LessOrEqual},
        .colorTargets = std::span{&colorTarget, 1},
        // Non-Invalid on ALL FOUR: the pipeline's depth format must match the pass it records into,
        // and the pass carries depth whether or not this pipeline tests it.
        .depthStencilFormat = config.depthFormat,
    };
    rhi::GraphicsPipelineDesc lineOverlayDesc = lineTestedDesc;
    lineOverlayDesc.depthStencil.enableDepthTest = false;

    const rhi::VertexBufferLayout billboardLayout{.slot = 0, .pitch = 36};  // sizeof(DebugBillboardVertex)
    const std::array<rhi::VertexAttribute, 5> billboardAttrs{{
        {.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offset = 0},
        {.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offset = 12},
        {.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offset = 20},
        {.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::UByte4Norm, .offset = 28},
        {.location = 4, .bufferSlot = 0, .format = rhi::VertexFormat::Float, .offset = 32},
    }};
    rhi::GraphicsPipelineDesc billboardTestedDesc = lineTestedDesc;
    billboardTestedDesc.vertexShader = billboardVs.get();
    billboardTestedDesc.fragmentShader = billboardFs.get();
    billboardTestedDesc.vertexBuffers = std::span{&billboardLayout, 1};
    billboardTestedDesc.vertexAttributes = billboardAttrs;
    billboardTestedDesc.primitiveType = rhi::PrimitiveType::TriangleList;
    rhi::GraphicsPipelineDesc billboardOverlayDesc = billboardTestedDesc;
    billboardOverlayDesc.depthStencil.enableDepthTest = false;

    // 5. Build the instance and adopt everything into it, so ONE destroyAll() cleans every partial
    //    failure below -- there is no per-exit destroy list to forget an entry from. `out` holds a
    //    non-null device from here on, which is what makes ~DebugDraw's destroyAll() reach anything.
    DebugDraw out{&device, allocated};
    out.linePipelines[0] = device.createGraphicsPipeline(lineTestedDesc);
    out.linePipelines[1] = device.createGraphicsPipeline(lineOverlayDesc);
    out.billboardPipelines[0] = device.createGraphicsPipeline(billboardTestedDesc);
    out.billboardPipelines[1] = device.createGraphicsPipeline(billboardOverlayDesc);
    for (const rhi::GraphicsPipelineHandle handle :
         {out.linePipelines[0], out.linePipelines[1], out.billboardPipelines[0], out.billboardPipelines[1]}) {
        if (!handle.valid()) {
            AERO_LOG_ERROR("render::DebugDraw::create - pipeline creation failed");
            return std::nullopt;  // ~DebugDraw -> destroyAll() releases whatever DID get made
        }
    }

    // 6. The two vertex buffers, sized ONCE from the allocated budget and never resized. The
    //    arithmetic is WIDENED FIRST and only then narrowed: at the ceilings these are 32 MiB and
    //    13.5 MiB, which is exactly why the ceilings exist -- neither can reach the RHI's 32-bit
    //    buffer-size limit, and doing the multiply in 32 bits would be the wrap that hides it.
    const std::uint64_t lineBufferBytes = static_cast<std::uint64_t>(allocated.maxLines) * 2U * sizeof(DebugLineVertex);
    const std::uint64_t billboardBufferBytes =
        static_cast<std::uint64_t>(allocated.maxBillboards) * 6U * sizeof(DebugBillboardVertex);
    out.lineBuffer =
        device.createBuffer({.usage = rhi::BufferUsage::Vertex, .size = static_cast<std::uint32_t>(lineBufferBytes)});
    out.billboardBuffer = device.createBuffer(
        {.usage = rhi::BufferUsage::Vertex, .size = static_cast<std::uint32_t>(billboardBufferBytes)});
    if (!out.lineBuffer.valid() || !out.billboardBuffer.valid()) {
        AERO_LOG_ERROR("render::DebugDraw::create - vertex buffer creation failed");
        return std::nullopt;
    }

    // 7. The built-in 1x1 white default and its sampler -- the ForwardRenderer::createDefaults idiom.
    //    They exist so the fragment stage NEVER has an unbound slot (the Metal assertion 3.6.2 D7
    //    records) and so an untextured billboard is a solid quad of its own colour.
    out.defaultTexture = device.createTexture(
        {.format = rhi::TextureFormat::RGBA8Unorm, .usage = rhi::TextureUsage::Sampler, .width = 1, .height = 1});
    const std::array<std::uint8_t, 4> white{255U, 255U, 255U, 255U};
    if (!out.defaultTexture.valid() || !device.uploadTexture(out.defaultTexture, 0, std::as_bytes(std::span{white}))) {
        AERO_LOG_ERROR("render::DebugDraw::create - the built-in 1x1 white texture could not be created");
        return std::nullopt;
    }
    out.defaultSampler = device.createSampler({.minFilter = rhi::Filter::Linear,
                                               .magFilter = rhi::Filter::Linear,
                                               .mipmapMode = rhi::MipmapMode::Nearest,
                                               .addressU = rhi::AddressMode::ClampToEdge,
                                               .addressV = rhi::AddressMode::ClampToEdge,
                                               .addressW = rhi::AddressMode::ClampToEdge});
    if (!out.defaultSampler.valid()) {
        AERO_LOG_ERROR("render::DebugDraw::create - the default sampler could not be created");
        return std::nullopt;
    }

    // 8. The staging mirrors: reserved ONCE, to the same maxima the GPU buffers hold.
    out.lineStaging.reserve(static_cast<std::size_t>(allocated.maxLines) * 2U);
    out.billboardStaging.reserve(static_cast<std::size_t>(allocated.maxBillboards) * 6U);
    return out;
}

// --- special members: PostProcess's C1 shape, because a DEFAULTED move would run destroyAll() on a
// source whose eight handles the destination now also holds -------------------------------------

DebugDraw::DebugDraw(DebugDraw&& other) noexcept
    : device(other.device),
      batchValue(std::move(other.batchValue)),
      linePipelines(other.linePipelines),
      billboardPipelines(other.billboardPipelines),
      lineBuffer(other.lineBuffer),
      billboardBuffer(other.billboardBuffer),
      defaultTexture(other.defaultTexture),
      defaultSampler(other.defaultSampler),
      billboardTexture(other.billboardTexture),
      billboardSampler(other.billboardSampler),
      lineStaging(std::move(other.lineStaging)),
      billboardStaging(std::move(other.billboardStaging)),
      lastLines(other.lastLines),
      lastBillboards(other.lastBillboards),
      lastDroppedLines(other.lastDroppedLines),
      lastDroppedBillboards(other.lastDroppedBillboards),
      lastRejectedLines(other.lastRejectedLines),
      lastRejectedBillboards(other.lastRejectedBillboards),
      lastDrawCalls(other.lastDrawCalls),
      flushes(other.flushes),
      uploads(other.uploads),
      warnedBudget(other.warnedBudget),
      warnedUploadFailure(other.warnedUploadFailure) {
    other.reset();
}

DebugDraw& DebugDraw::operator=(DebugDraw&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    destroyAll();
    device = other.device;
    batchValue = std::move(other.batchValue);
    linePipelines = other.linePipelines;
    billboardPipelines = other.billboardPipelines;
    lineBuffer = other.lineBuffer;
    billboardBuffer = other.billboardBuffer;
    defaultTexture = other.defaultTexture;
    defaultSampler = other.defaultSampler;
    billboardTexture = other.billboardTexture;
    billboardSampler = other.billboardSampler;
    lineStaging = std::move(other.lineStaging);
    billboardStaging = std::move(other.billboardStaging);
    lastLines = other.lastLines;
    lastBillboards = other.lastBillboards;
    lastDroppedLines = other.lastDroppedLines;
    lastDroppedBillboards = other.lastDroppedBillboards;
    lastRejectedLines = other.lastRejectedLines;
    lastRejectedBillboards = other.lastRejectedBillboards;
    lastDrawCalls = other.lastDrawCalls;
    flushes = other.flushes;
    uploads = other.uploads;
    warnedBudget = other.warnedBudget;
    warnedUploadFailure = other.warnedUploadFailure;
    other.reset();
    return *this;
}

DebugDraw::~DebugDraw() { destroyAll(); }

void DebugDraw::destroyAll() noexcept {
    if (device == nullptr) {
        return;  // moved-from: it owns nothing, and reset() already cleared every handle
    }
    // Reverse creation order: sampler -> texture -> buffers -> the four pipelines. The two BORROWED
    // handles are never touched -- the caller made them and the caller destroys them.
    if (defaultSampler.valid()) {
        device->destroySampler(defaultSampler);
    }
    if (defaultTexture.valid()) {
        device->destroyTexture(defaultTexture);
    }
    if (billboardBuffer.valid()) {
        device->destroyBuffer(billboardBuffer);
    }
    if (lineBuffer.valid()) {
        device->destroyBuffer(lineBuffer);
    }
    for (const rhi::GraphicsPipelineHandle pipeline : billboardPipelines) {
        if (pipeline.valid()) {
            device->destroyGraphicsPipeline(pipeline);
        }
    }
    for (const rhi::GraphicsPipelineHandle pipeline : linePipelines) {
        if (pipeline.valid()) {
            device->destroyGraphicsPipeline(pipeline);
        }
    }
    reset();
}

void DebugDraw::reset() noexcept {
    device = nullptr;
    linePipelines = {};
    billboardPipelines = {};
    lineBuffer = {};
    billboardBuffer = {};
    defaultTexture = {};
    defaultSampler = {};
    billboardTexture = {};
    billboardSampler = {};
    lineStaging.clear();
    billboardStaging.clear();
    batchValue.clear();
    lastLines = 0;
    lastBillboards = 0;
    lastDroppedLines = 0;
    lastDroppedBillboards = 0;
    lastRejectedLines = 0;
    lastRejectedBillboards = 0;
    lastDrawCalls = 0;
    flushes = 0;
    uploads = 0;
    warnedBudget = false;
    warnedUploadFailure = false;
}

void DebugDraw::setBillboardTexture(rhi::TextureHandle texture, rhi::SamplerHandle sampler) noexcept {
    billboardTexture = texture;  // BORROWED, both: never destroyed here, never adopted
    billboardSampler = sampler;
}

// A MEMBER FUNCTION rather than a lambda, deliberately: with profiling OFF, AERO_PROFILE_PLOT
// expands to ((void)0) and does NOT evaluate its argument, so a `[this]` capture whose only use was
// inside the macro would be an unused capture in exactly one of the two configurations.
void DebugDraw::plotCounters() const noexcept {
    AERO_PROFILE_PLOT("render.debugLines", static_cast<std::int64_t>(lastLines));
    AERO_PROFILE_PLOT("render.debugBillboards", static_cast<std::int64_t>(lastBillboards));
}

void DebugDraw::flush(Frame& frame, const CameraView& camera) {
    AERO_PROFILE_ZONE_NAMED("render::DebugDraw::flush");
    // RESET BEFORE ANY EARLY RETURN (3.6.1's CD5 lesson): a frame that drew nothing must read zeros
    // rather than the previous frame's numbers.
    lastLines = 0;
    lastBillboards = 0;
    lastDroppedLines = 0;
    lastDroppedBillboards = 0;
    lastRejectedLines = 0;
    lastRejectedBillboards = 0;
    lastDrawCalls = 0;
    ++flushes;
    // plotCounters() is called on EVERY exit including the empty one (3.6.1's own rule), so a Tracy
    // capture shows a continuous zero line rather than a gap that reads as "the renderer stopped".
    if (device == nullptr) {
        plotCounters();
        return;  // moved-from: inert and SILENT, like every moved-from accessor in this layer
    }

    // The batch's own counters, snapshotted before anything can clear them.
    lastDroppedLines = batchValue.droppedLines();
    lastDroppedBillboards = batchValue.droppedBillboards();
    lastRejectedLines = batchValue.rejectedLines();
    lastRejectedBillboards = batchValue.rejectedBillboards();
    if ((lastDroppedLines != 0U || lastDroppedBillboards != 0U) && !warnedBudget) {
        warnedBudget = true;  // LATCHED: overflow is a persistent condition and an unlatched WARN
        AERO_LOG_WARN(        //          is a 60 Hz flood.
            "render::DebugDraw::flush - budget exceeded: dropped {} line(s) and {} billboard(s) this "
            "frame (budget {} / {}); reported once per DebugDraw",
            lastDroppedLines, lastDroppedBillboards, batchValue.budget().maxLines, batchValue.budget().maxBillboards);
    }
    // NOTE: rejections are counted and NEVER warned. A non-finite input is a caller bug the counter
    // reports; warning about it would flood exactly the frames a NaN is propagating.

    const std::uint32_t testedLines = batchValue.lineCount(DebugDepth::Tested);
    const std::uint32_t overlayLines = batchValue.lineCount(DebugDepth::Overlay);
    const std::uint32_t testedBillboards = batchValue.billboardCount(DebugDepth::Tested);
    const std::uint32_t overlayBillboards = batchValue.billboardCount(DebugDepth::Overlay);
    if (batchValue.empty()) {
        // AN EMPTY FLUSH IS FREE: no command buffer, no upload, no bind, no draw. That is what makes
        // "the editor's picture is byte-identical" a measured fact rather than a hope (DG4, I108).
        batchValue.clear();
        plotCounters();
        return;
    }

    // 1. Staging: Tested THEN Overlay, contiguous, so ONE upload and ONE bind serve both buckets and
    //    the two draws differ only in firstVertex.
    lineStaging.clear();
    for (const DebugDepth depth : {DebugDepth::Tested, DebugDepth::Overlay}) {
        const std::span<const DebugLineVertex> src = batchValue.lineVertices(depth);
        lineStaging.insert(lineStaging.end(), src.begin(), src.end());
    }
    billboardStaging.clear();
    for (const DebugDepth depth : {DebugDepth::Tested, DebugDepth::Overlay}) {
        for (const DebugBillboard& record : batchValue.billboards(depth)) {
            const std::size_t at = billboardStaging.size();
            billboardStaging.resize(at + 6U);
            expandBillboard(record, std::span<DebugBillboardVertex, 6>{billboardStaging.data() + at, 6U});
        }
    }

    // 2. The upload, on its OWN command buffer, SUBMITTED before the frame's (D4/F12). The frame's
    //    buffer was acquired by beginFrame/beginScene and is submitted by endFrame/endScene, so this
    //    submit precedes it and the device's ordering guarantee does the rest -- NO EXPLICIT BARRIER
    //    IS NEEDED AND NONE IS AVAILABLE. The shadow-pass pattern, third use.
    const rhi::CommandBufferHandle uploadCmd = device->acquireCommandBuffer();
    if (!uploadCmd.valid()) {
        if (!warnedUploadFailure) {
            warnedUploadFailure = true;
            AERO_LOG_ERROR(
                "render::DebugDraw::flush - could not acquire an upload command buffer; "
                "nothing drawn this frame (reported once per DebugDraw)");
        }
        batchValue.clear();
        plotCounters();
        return;  // uploadCount() UNMOVED -- "acquired nothing" and "acquired and leaked" differ
    }
    ++uploads;  // at the ACQUISITION, the shadowPassCount posture
    bool uploaded = true;
    if (!lineStaging.empty()) {
        uploaded = device->recordBufferUpload(uploadCmd, lineBuffer, std::as_bytes(std::span{lineStaging}));
    }
    if (uploaded && !billboardStaging.empty()) {
        uploaded = device->recordBufferUpload(uploadCmd, billboardBuffer, std::as_bytes(std::span{billboardStaging}));
    }
    // ALWAYS submitted, even when a record failed: an acquired command buffer must be submitted or
    // cancelled, and cancel is the wrong verb for a buffer that may already hold the first copy.
    device->submit(uploadCmd);
    if (!uploaded) {
        if (!warnedUploadFailure) {
            warnedUploadFailure = true;
            AERO_LOG_WARN(
                "render::DebugDraw::flush - a vertex upload failed; nothing drawn this frame "
                "(reported once per DebugDraw)");
        }
        batchValue.clear();
        plotCounters();
        return;  // NOTHING is drawn from a buffer whose contents are undefined
    }

    // 3. The draws, into the caller's ALREADY-OPEN pass.
    const rhi::RenderPassHandle pass = frame.pass();
    const rhi::CommandBufferHandle frameCmd = frame.commandBuffer();
    const Mat4 viewProj = camera.proj * camera.view;  // M * v, right-to-left (forward_renderer.cpp's own)
    if (!lineStaging.empty()) {
        const auto block = packDebugLineView(viewProj);
        device->pushVertexUniforms(frameCmd, 0, block);
        device->bindVertexBuffer(pass, 0, lineBuffer);
        // TESTED BEFORE OVERLAY, per primitive: overlay content is meant to be seen over everything,
        // including tested lines, and drawing it LAST is what makes that true where the two overlap.
        // firstVertex is legal here because NEITHER shader reads SV_VertexID -- the fullscreen
        // triangle does, and its draw starts at 0.
        if (testedLines != 0U) {
            device->bindGraphicsPipeline(pass, linePipelines[0]);
            device->draw(pass, testedLines * 2U, 1, 0);
            ++lastDrawCalls;
        }
        if (overlayLines != 0U) {
            device->bindGraphicsPipeline(pass, linePipelines[1]);
            device->draw(pass, overlayLines * 2U, 1, testedLines * 2U);
            ++lastDrawCalls;
        }
    }
    if (!billboardStaging.empty()) {
        // THE SAME SLOT, PUSHED AGAIN. Pushed uniforms persist on a command buffer until pushed
        // again and are captured PER DRAW (device.hpp's own contract, which ForwardRenderer::draw
        // relies on per instance), so this is one slot, two blocks, no collision.
        const auto block = packDebugBillboardView(viewProj, frame.extent());
        device->pushVertexUniforms(frameCmd, 0, block);
        const rhi::TextureSamplerBinding binding{billboardTexture.valid() ? billboardTexture : defaultTexture,
                                                 billboardSampler.valid() ? billboardSampler : defaultSampler};
        device->bindFragmentSamplers(pass, 0, std::span{&binding, 1});
        device->bindVertexBuffer(pass, 0, billboardBuffer);
        if (testedBillboards != 0U) {
            device->bindGraphicsPipeline(pass, billboardPipelines[0]);
            device->draw(pass, testedBillboards * 6U, 1, 0);
            ++lastDrawCalls;
        }
        if (overlayBillboards != 0U) {
            device->bindGraphicsPipeline(pass, billboardPipelines[1]);
            device->draw(pass, overlayBillboards * 6U, 1, testedBillboards * 6U);
            ++lastDrawCalls;
        }
    }
    lastLines = testedLines + overlayLines;
    lastBillboards = testedBillboards + overlayBillboards;
    batchValue.clear();
    plotCounters();
}

DebugDrawBatch& DebugDraw::batch() noexcept { return batchValue; }
const DebugDrawBatch& DebugDraw::batch() const noexcept { return batchValue; }
const DebugDrawBudget& DebugDraw::budget() const noexcept { return batchValue.budget(); }
std::uint32_t DebugDraw::lastFrameLines() const noexcept { return lastLines; }
std::uint32_t DebugDraw::lastFrameBillboards() const noexcept { return lastBillboards; }
std::uint32_t DebugDraw::lastFrameDroppedLines() const noexcept { return lastDroppedLines; }
std::uint32_t DebugDraw::lastFrameDroppedBillboards() const noexcept { return lastDroppedBillboards; }
std::uint32_t DebugDraw::lastFrameRejectedLines() const noexcept { return lastRejectedLines; }
std::uint32_t DebugDraw::lastFrameRejectedBillboards() const noexcept { return lastRejectedBillboards; }
std::uint32_t DebugDraw::lastFrameDrawCalls() const noexcept { return lastDrawCalls; }
std::size_t DebugDraw::flushCount() const noexcept { return flushes; }
std::size_t DebugDraw::uploadCount() const noexcept { return uploads; }
bool DebugDraw::hasWarnedBudget() const noexcept { return warnedBudget; }
bool DebugDraw::hasWarnedUploadFailure() const noexcept { return warnedUploadFailure; }
bool DebugDraw::hasBillboardTexture() const noexcept { return billboardTexture.valid(); }

}  // namespace engine::render

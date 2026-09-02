#pragma once
// Aero Engine — render::DebugDraw (task E.1.1): the tree's first world-space line renderer.
//
// TWO HALVES IN ONE HEADER, the post_process.hpp shape (tonemapSourceUvMax beside PostProcess):
//   * DebugDrawBatch -- PURE. No rhi type, no logging, no GPU, and NO ALLOCATION after construction.
//     Everything assertable without a device is asserted without one.
//   * DebugDraw      -- the GPU half: four pipelines from two shader pairs, two vertex buffers sized
//     at create(), and a flush() that uploads the batch and records it into a caller's ALREADY-OPEN
//     render pass. It OWNS a batch and hands it out through batch().
// They share every type and a consumer that has one always has the other, which is why they share a
// header rather than a dependency edge.
//
// THE SLOT. flush() records into a pass the caller opened and has NOT ended -- in the editor, the
// HDR scene pass, AFTER ForwardRenderer's draw (so a Tested line is depth-tested against this
// frame's geometry) and BEFORE PostProcess::endScene (so the lines go through the resolve with
// everything else). A Frame is consumed through its three PUBLIC accessors and this header needs no
// friend (renderer.hpp:74-79 forbids befriending anything outside engine::render anyway).
//
// LIFETIME CONTRACTS (forward_renderer.hpp's, one layer over): the rhi::Device passed to create()
// MUST outlive the DebugDraw. Move-only with USER-DEFINED noexcept moves -- a defaulted move would
// double-free four pipelines, two buffers, a texture and a sampler.
//
// ERROR MODEL (docs/04): nothing throws. create() -> nullopt (+ one ERROR naming the cause);
// flush() is void and best-effort, matching ForwardRenderer::draw; the batch returns bool/counts and
// refuses silently (a per-frame WARN at 60 Hz is a flood, so the WARN is latched inside flush()).

#include <aero/core/math.hpp>
#include <aero/rhi/format.hpp>
#include <aero/rhi/handles.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace engine::rhi {
class Device;  // forward-declared, exactly as forward_renderer.hpp / post_process.hpp do
}  // namespace engine::rhi

namespace engine {
class VirtualFileSystem;  // forward-declared: create() takes it by ref; the .cpp includes vfs.hpp
}  // namespace engine

namespace engine::render {

class Frame;        // renderer.hpp; flush() takes it by reference
struct CameraView;  // lighting.hpp

// ---- the vocabulary --------------------------------------------------------------------------

// A per-PRIMITIVE choice, never a per-flush one.
//   Tested  -- depth test ON, depth write OFF, CompareOp::LessOrEqual. A wire box around a cube
//              shows only its near edges. LessOrEqual rather than the engine's Less convention,
//              DELIBERATELY: a line drawn exactly on a surface's own vertices wins the comparison
//              where a bit-identical depth would otherwise flicker, and it changes nothing for a
//              line behind or in front of geometry. The write stays off on BOTH modes, so a debug
//              line can never occlude a mesh or another line.
//   Overlay -- depth test OFF. A selected light's gizmo drawn through a wall, which is the editor
//              convention every 3D tool follows, and what E.2.3's always-visible icons need.
enum class DebugDepth : std::uint8_t { Tested = 0, Overlay = 1, Count = 2 };
inline constexpr std::size_t DEBUG_DEPTH_COUNT = 2;

// NEVER named toString: doctest's DOCTEST_STRINGIFY expands to an UNQUALIFIED toString(...), which
// ADL finds on a public engine header, beats doctest's own template, and makes the decomposer try
// std::string_view + const char* -- a hard compile error on every lane, reported inside doctest.h.
// audioClipLoadStatusLabel is the precedent.
[[nodiscard]] std::string_view debugDepthLabel(DebugDepth depth) noexcept;

// slot 0, pitch 16: Float3 @0, UByte4Norm @12. The pipeline's stride and offsets are pinned against
// these two static_asserts, so a field added here without touching the descriptor fails to compile.
struct DebugLineVertex {
    Vec3 position;
    std::uint32_t rgba = 0xFFFFFFFFU;  // linear RGBA8; bytes r, g, b, a in MEMORY order
};
static_assert(sizeof(DebugLineVertex) == 16);
static_assert(offsetof(DebugLineVertex, rgba) == 12);

// The batch's un-expanded record. Expanded to six DebugBillboardVertex at flush time (D7).
struct DebugBillboard {
    Vec3 center;
    float sizePx = 16.0F;    // FULL width and height, in the frame's pixels, at any distance
    Vec2 uvMin{0.0F, 0.0F};  // uvMin.y is the TOP of the sprite (the texture origin)
    Vec2 uvMax{1.0F, 1.0F};
    std::uint32_t rgba = 0xFFFFFFFFU;
};

// slot 0, pitch 36: Float3 @0, Float2 @12, Float2 @20, UByte4Norm @28, Float @32.
// Public so a tier-0 case can pin the expansion table byte for byte, and so a future instanced path
// and the sample share ONE definition of the corner/UV mapping.
struct DebugBillboardVertex {
    Vec3 center;
    Vec2 corner;  // one of {-0.5, +0.5}^2; +y is the TOP of the sprite (NDC +y is up)
    Vec2 uv;
    std::uint32_t rgba = 0xFFFFFFFFU;
    float sizePx = 16.0F;
};
static_assert(sizeof(DebugBillboardVertex) == 36);
static_assert(offsetof(DebugBillboardVertex, corner) == 12);
static_assert(offsetof(DebugBillboardVertex, uv) == 20);
static_assert(offsetof(DebugBillboardVertex, rgba) == 28);
static_assert(offsetof(DebugBillboardVertex, sizePx) == 32);

// TOTAL. A NON-FINITE channel packs to 0 -- std::clamp(NaN, lo, hi) returns NaN on libc++ and
// narrowing NaN to an integer is UB that UBSan traps (the 3.7.2 rule). Everything else clamps to
// [0, 1] and rounds with std::lround. Colours are LINEAR, not sRGB-encoded: the HDR target is linear
// and 3.6.3's resolve encodes, so a line pushed as (1,1,1,1) reads 232/255 under the default ACES
// operator and 255 under None.
[[nodiscard]] std::uint32_t packDebugColor(Vec4 linearRgba) noexcept;
[[nodiscard]] Vec4 unpackDebugColor(std::uint32_t rgba) noexcept;

// The fixed six-vertex table: two triangles, four distinct corners, +0.5 y carrying uvMin.y.
void expandBillboard(const DebugBillboard& billboard, std::span<DebugBillboardVertex, 6> out) noexcept;

// ---- the budget ------------------------------------------------------------------------------

// Defaults: 1 MiB of line vertices (32768 * 2 * 16) and 864 KiB of expanded billboard vertices
// (4096 * 6 * 36), both allocated ONCE at create() and never resized. E.1.2's grid is a few thousand
// lines at its densest cadence; a point-light sphere is three circles; a spot cone is a few dozen
// segments. An order of magnitude above the first two consumers' worst case.
struct DebugDrawBudget {
    std::uint32_t maxLines = 32768;
    std::uint32_t maxBillboards = 4096;
    bool operator==(const DebugDrawBudget&) const = default;
};

// The ceilings exist so 2 * maxLines * 16 can never approach the RHI's 32-bit buffer-size limit, and
// so the CPU mirrors stay bounded: 1'048'576 lines is 32 MiB, 65'536 billboards is 13.5 MiB.
inline constexpr std::uint32_t DEBUG_DRAW_MAX_LINES_CEILING = 1'048'576;
inline constexpr std::uint32_t DEBUG_DRAW_MAX_BILLBOARDS_CEILING = 65'536;

// PURE and TOTAL: each field into [1, ceiling]. A typo is not a reason to refuse to start (the 3.6.2
// shadow-resolution posture); create() clamps with ONE WARN naming the requested and allocated
// numbers, and budget() then reports what was allocated.
[[nodiscard]] DebugDrawBudget clampDebugDrawBudget(DebugDrawBudget requested) noexcept;

// ---- the batch -------------------------------------------------------------------------------

// PURE: no rhi type, no logging, no GPU. ALLOCATES AT CONSTRUCTION AND NEVER AGAIN (the
// RenderViewScratch rule) -- a push past the budget is DROPPED AND COUNTED, never resized into, so a
// consumer that pushes a million lines every frame costs a million refused pushes and one latched
// WARN rather than a million-entry vector.
//
// THE OVERFLOW POLICY, stateable precisely because refusal happens at PUSH time: the first maxLines
// lines pushed are the ones drawn; everything after is dropped, IN PUSH ORDER, with no reordering
// and no priority. The two depth buckets share ONE budget. The helpers (wireBox, wireCircle,
// wireSphere) push their segments one by one through the same gate, so a wireSphere that straddles
// the budget is drawn up to the segment the budget ended on -- partial, deterministic, and counted.
//
// TWO KINDS OF REFUSAL, counted separately and never conflated:
//   dropped*  -- the budget was full. The push was legal; there was no room.
//   rejected* -- the input was not finite (or a radius/size was not positive). The push was never
//                legal. A NaN colour CHANNEL is not a rejection -- packDebugColor is total.
class DebugDrawBatch {
public:
    explicit DebugDrawBatch(DebugDrawBudget budget);  // clamps again, so a hand-built one is safe

    void clear() noexcept;  // empties every bucket, zeroes the counters, KEEPS capacity

    bool line(Vec3 a, Vec3 b, Vec4 color, DebugDepth depth = DebugDepth::Tested);
    // An EVEN count of pre-packed vertices, pushed pairwise through the same gate. An odd count
    // drops the last vertex and counts ONE rejection. Returns the number of LINES accepted.
    std::uint32_t lines(std::span<const DebugLineVertex> vertices, DebugDepth depth = DebugDepth::Tested);
    // The eight corners through transformPoint(model, .), the twelve edges through line(). No Aabb
    // in the signature, so both the render and the editor Aabb types can call it with two vectors.
    void wireBox(const Mat4& model, Vec3 localMin, Vec3 localMax, Vec4 color, DebugDepth depth = DebugDepth::Tested);
    // `segments` clamped to [3, 256]. A normal that normalizeOrZero's to zero, or a non-finite or
    // non-positive radius, is ONE rejection and no push.
    void wireCircle(Vec3 center, Vec3 normal, float radius, Vec4 color, std::uint32_t segments = 32,
                    DebugDepth depth = DebugDepth::Tested);
    void wireSphere(Vec3 center, float radius, Vec4 color, std::uint32_t segments = 32,
                    DebugDepth depth = DebugDepth::Tested);
    bool billboard(Vec3 center, float sizePx, Vec4 color, Vec2 uvMin = {0.0F, 0.0F}, Vec2 uvMax = {1.0F, 1.0F},
                   DebugDepth depth = DebugDepth::Tested);

    [[nodiscard]] std::span<const DebugLineVertex> lineVertices(DebugDepth depth) const noexcept;
    [[nodiscard]] std::span<const DebugBillboard> billboards(DebugDepth depth) const noexcept;
    [[nodiscard]] std::uint32_t lineCount() const noexcept;  // both buckets
    [[nodiscard]] std::uint32_t lineCount(DebugDepth depth) const noexcept;
    [[nodiscard]] std::uint32_t billboardCount() const noexcept;
    [[nodiscard]] std::uint32_t billboardCount(DebugDepth depth) const noexcept;
    [[nodiscard]] std::uint32_t droppedLines() const noexcept;  // budget refusals since clear()
    [[nodiscard]] std::uint32_t droppedBillboards() const noexcept;
    [[nodiscard]] std::uint32_t rejectedLines() const noexcept;  // non-finite refusals since clear()
    [[nodiscard]] std::uint32_t rejectedBillboards() const noexcept;
    [[nodiscard]] const DebugDrawBudget& budget() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    // Members are plain camelBack with no trailing underscore; where a name would collide with an
    // accessor the MEMBER takes a distinct name (budgetValue/budget(), the RenderTarget precedent).
    DebugDrawBudget budgetValue;
    std::array<std::vector<DebugLineVertex>, DEBUG_DEPTH_COUNT> lineBuckets;
    std::array<std::vector<DebugBillboard>, DEBUG_DEPTH_COUNT> billboardBuckets;
    std::uint32_t droppedLineCount = 0;
    std::uint32_t droppedBillboardCount = 0;
    std::uint32_t rejectedLineCount = 0;
    std::uint32_t rejectedBillboardCount = 0;
};

// ---- the GPU half ----------------------------------------------------------------------------

struct DebugDrawConfig {
    // REQUIRED: the format of the colour target of the pass flush() records into. Invalid or a depth
    // format FAILS create().
    rhi::TextureFormat colorFormat = rhi::TextureFormat::Invalid;
    // REQUIRED, != Invalid: the pass carries depth whether or not the pipeline TESTS it -- the
    // pipeline's depth format must match the pass it records into (the GraphicsPipelineDesc
    // sentinel), and both Overlay pipelines declare it too.
    rhi::TextureFormat depthFormat = rhi::TextureFormat::Invalid;
    DebugDrawBudget budget{};
    // Extension-less res:// VFS paths. READ ONLY INSIDE create() and never after -- VERIFIED, not
    // promised: the config is not stored at all, so there is no view left to dangle (the 3.6.3
    // PostProcessConfig precision, which a code-review round required there, taken one step further).
    std::string_view lineVertexShaderPath = "res://debug_line.vert";
    std::string_view lineFragmentShaderPath = "res://debug_line.frag";
    std::string_view billboardVertexShaderPath = "res://debug_billboard.vert";
    std::string_view billboardFragmentShaderPath = "res://debug_billboard.frag";
};

class DebugDraw {
public:
    // nullopt + ONE ERROR naming the cause on: colorFormat Invalid or a depth format; depthFormat
    // Invalid; any of the four shaders failing to load; any of the four pipelines failing; either
    // vertex buffer failing; the default texture or sampler failing. DESTROYS ANYTHING IT ALREADY
    // CREATED before returning -- no ~Device leak WARN on any failure path (DG2). All four shader
    // handles are SCOPE-OWNED (the ScopedShader idiom), so no exit can leak one.
    // Clamps the budget with ONE WARN naming the requested and the allocated numbers.
    [[nodiscard]] static std::optional<DebugDraw> create(rhi::Device& device, const VirtualFileSystem& shaderVfs,
                                                         const DebugDrawConfig& config);

    ~DebugDraw();                                // no-op if moved-from
    DebugDraw(DebugDraw&&) noexcept;             // USER-DEFINED: transfers + nulls the source
    DebugDraw& operator=(DebugDraw&&) noexcept;  // (a defaulted move double-frees eight handles)
    DebugDraw(const DebugDraw&) = delete;
    DebugDraw& operator=(const DebugDraw&) = delete;

    // The batch this instance owns. A consumer writes debugDraw()->batch().line(a, b, colour) and
    // never touches a buffer, a pipeline or a frame.
    [[nodiscard]] DebugDrawBatch& batch() noexcept;
    [[nodiscard]] const DebugDrawBatch& batch() const noexcept;

    // BORROWED, both of them -- material.hpp's ownership rule: the caller creates and destroys them,
    // and DebugDraw never touches a TextureHandle it did not make. An INVALID handle in either
    // position means "use the built-in default" for THAT position independently: the 1x1 white texel
    // and the Linear/ClampToEdge sampler this instance owns. One atlas per DebugDraw, not per
    // billboard -- a per-billboard texture would break the one-draw-per-bucket rule, and E.2.3's
    // icons are a handful of glyphs that belong in one atlas anyway; uvMin/uvMax select the glyph.
    void setBillboardTexture(rhi::TextureHandle texture, rhi::SamplerHandle sampler) noexcept;

    // Uploads the batch on its OWN command buffer (submitted before returning), records up to four
    // draws into `frame`'s ALREADY-OPEN pass, and CLEARS THE BATCH ON EVERY PATH including every
    // failure, so a failed frame cannot accumulate. Best-effort and void, matching
    // ForwardRenderer::draw. AN EMPTY BATCH IS FREE: no command buffer, no upload, no bind, no draw.
    void flush(Frame& frame, const CameraView& camera);

    // ---- diagnostics: they report, they never change behaviour -------------------------------
    // Every per-frame counter is reset at the TOP of flush(), before any early return, so a frame
    // that drew nothing reads zeros rather than the previous frame's numbers (3.6.1's CD5 lesson).
    [[nodiscard]] std::uint32_t lastFrameLines() const noexcept;
    [[nodiscard]] std::uint32_t lastFrameBillboards() const noexcept;
    [[nodiscard]] std::uint32_t lastFrameDroppedLines() const noexcept;
    [[nodiscard]] std::uint32_t lastFrameDroppedBillboards() const noexcept;
    [[nodiscard]] std::uint32_t lastFrameRejectedLines() const noexcept;
    [[nodiscard]] std::uint32_t lastFrameRejectedBillboards() const noexcept;
    [[nodiscard]] std::uint32_t lastFrameDrawCalls() const noexcept;  // 0..4
    [[nodiscard]] std::size_t flushCount() const noexcept;            // calls, lifetime
    // Upload command buffers ACQUIRED, lifetime -- moved at the ACQUISITION (the shadowPassCount
    // posture), so "acquired nothing" and "acquired and leaked" are distinguishable.
    [[nodiscard]] std::size_t uploadCount() const noexcept;
    [[nodiscard]] bool hasWarnedBudget() const noexcept;
    [[nodiscard]] bool hasWarnedUploadFailure() const noexcept;
    [[nodiscard]] bool hasBillboardTexture() const noexcept;
    [[nodiscard]] const DebugDrawBudget& budget() const noexcept;  // ALLOCATED, after clamping

private:
    // NOT noexcept: DebugDrawBatch's constructor reserves both buckets, so this can throw
    // bad_alloc. Marking it noexcept would turn an allocation failure into std::terminate
    // (clang-tidy's bugprone-exception-escape says so out loud). create() is the only caller
    // and is itself not noexcept.
    DebugDraw(rhi::Device* owner, DebugDrawBudget allocated);
    void destroyAll() noexcept;
    void reset() noexcept;
    void plotCounters() const noexcept;  // the two Tracy plots; a no-op when profiling is off

    rhi::Device* device = nullptr;  // non-owning; the Device outlives the DebugDraw (contract)
    DebugDrawBatch batchValue;      // member/accessor collision rule: batchValue / batch()
    std::array<rhi::GraphicsPipelineHandle, DEBUG_DEPTH_COUNT> linePipelines{};
    std::array<rhi::GraphicsPipelineHandle, DEBUG_DEPTH_COUNT> billboardPipelines{};
    rhi::BufferHandle lineBuffer{};                      // 2 * maxLines * 16 bytes, created ONCE at create()
    rhi::BufferHandle billboardBuffer{};                 // 6 * maxBillboards * 36 bytes, created ONCE
    rhi::TextureHandle defaultTexture{};                 // 1x1 white RGBA8Unorm, OWNED
    rhi::SamplerHandle defaultSampler{};                 // Linear / ClampToEdge, OWNED
    rhi::TextureHandle billboardTexture{};               // BORROWED; invalid == use defaultTexture
    rhi::SamplerHandle billboardSampler{};               // BORROWED; invalid == use defaultSampler
    std::vector<DebugLineVertex> lineStaging;            // reserved to 2 * maxLines, ONCE
    std::vector<DebugBillboardVertex> billboardStaging;  // reserved to 6 * maxBillboards, ONCE
    std::uint32_t lastLines = 0;
    std::uint32_t lastBillboards = 0;
    std::uint32_t lastDroppedLines = 0;
    std::uint32_t lastDroppedBillboards = 0;
    std::uint32_t lastRejectedLines = 0;
    std::uint32_t lastRejectedBillboards = 0;
    std::uint32_t lastDrawCalls = 0;
    std::size_t flushes = 0;
    std::size_t uploads = 0;
    bool warnedBudget = false;
    bool warnedUploadFailure = false;
};

}  // namespace engine::render

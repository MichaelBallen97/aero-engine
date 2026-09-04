#pragma once
// Aero Engine — render::SelectionOutline (task E.1.4): the edge-detect composite that turns an
// R8Unorm selection mask into a screen-space band over the ALREADY-RESOLVED image.
//
// TWO HALVES IN ONE HEADER, the post_process.hpp / debug_draw.hpp shape:
//   * the mask's VOCABULARY -- the format, its two occupied levels, the one threshold that reads
//     them back, SelectionMaskView, the params and their total sanitizer. All PURE: no rhi object,
//     no GPU, no logging, and assertable with no device at all.
//   * SelectionOutline -- the GPU half: one pipeline, one sampler, and a composite() that records a
//     fullscreen triangle into a caller's ALREADY-OPEN render pass.
//
// THE SLOT. composite() records into a pass the caller opened and has NOT ended -- in the editor,
// the OUTPUT target's pass, AFTER PostProcess::resolve, so the band lands in exact display bytes and
// does NOT go through the tone curve. That is the whole of D7: the outline is editor chrome and
// amber must stay amber regardless of exposure. Its consequence with teeth is that the params carry
// DISPLAY-space colour, the opposite convention from packDebugColor's linear -- which is why the
// fields are named primaryColorSrgb / secondaryColorSrgb and why the NAME IS THE DOCUMENTATION.
//
// THE EDGE RULE, and it is the whole of what this draws. For each output pixel: sample self and the
// eight box neighbours at exactly +/- radiusPixels texels; outline iff `mn < mx`, coloured from mx.
// Across an axis-aligned edge the band is EXACTLY 2*radius pixels -- an exact integer, because the
// sampler is Nearest on both filters, so a sampled value IS a texel value. The number is 2*radius
// and not 2*radius + 1, and it is worth the arithmetic: the neighbourhood in x is exactly
// {c - r, c, c + r}, so with a mask transition between texels k and k + 1 the pixels satisfying
// mn < mx are exactly k + 1 - r .. k + r -- r INSIDE the silhouette and r outside. MEASURED at
// radius 1, 2, 4 and 8 by OG3, not reasoned from. Across a 45-degree edge the box neighbourhood
// reaches radius*sqrt(2) and the band is wider there: the standard behaviour of a box
// neighbourhood, documented rather than corrected.
//
// LIFETIME CONTRACTS (post_process.hpp's, verbatim): the rhi::Device passed to create() MUST outlive
// the SelectionOutline. Move-only with USER-DEFINED noexcept moves -- a defaulted move would
// double-free the pipeline and the sampler.
//
// ERROR MODEL (docs/04): nothing throws. create() -> nullopt (+ one ERROR naming the cause), having
// destroyed anything it already made; composite() is void and best-effort with latched WARNs,
// matching PostProcess::resolve and ForwardRenderer::draw.

#include <aero/core/math.hpp>
#include <aero/rhi/format.hpp>
#include <aero/rhi/handles.hpp>
#include <aero/rhi/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace engine::rhi {
class Device;  // forward-declared, exactly as forward_renderer.hpp / post_process.hpp do
}  // namespace engine::rhi

namespace engine {
class VirtualFileSystem;  // forward-declared: create() takes it by ref; the .cpp includes vfs.hpp
}  // namespace engine

namespace engine::render {

class Frame;  // renderer.hpp; composite() takes it by reference

// ---- the mask's own vocabulary -----------------------------------------------------------------

// The mask's format, its two occupied levels, and the ONE threshold that reads them back. R8Unorm
// stores 0.5 and 1.0 as bytes 128 and 255, so the threshold has 0.25 of headroom on either side and
// the classification is exact rather than tolerant. PUBLIC so the shader's source-text pin (SO9) can
// name the same number the C++ side does, and so ForwardRenderer's four pipelines and its texture
// allocation take their format from ONE place.
//
// There is deliberately NO "is anything present" threshold: `mn < mx` is the whole edge rule, the
// all-background case satisfies `mn == mx == 0` on its own, and a constant with no consumer is the
// FillMode::Line trap E.1.1 recorded.
inline constexpr rhi::TextureFormat SELECTION_MASK_FORMAT = rhi::TextureFormat::R8Unorm;
inline constexpr float SELECTION_MASK_SECONDARY = 0.5F;
inline constexpr float SELECTION_MASK_PRIMARY = 1.0F;
inline constexpr float SELECTION_MASK_PRIMARY_THRESHOLD = 0.75F;

// What ForwardRenderer::renderSelectionMask RETURNS and SelectionOutline::composite reads. The
// ShadowView contract, verbatim: a DEFAULT-CONSTRUCTED view (valid == false) means "draw no outline",
// SILENTLY, and is what every failure path returns. There is deliberately NO SelectionOutline member
// holding any of this -- a cached mask handle is stale by default the moment the target reallocates,
// and every automated observable stays green while it is.
struct SelectionMaskView {
    rhi::TextureHandle texture{};   // R8Unorm, Sampler|ColorTarget; invalid when !valid
    rhi::Extent2D textureExtent{};  // the ALLOCATION -- the composite's UV denominator
    rhi::Extent2D drawExtent{};     // the rendered sub-rect -- the composite's UV numerator
    bool valid = false;
};

// ---- the composite's parameters ----------------------------------------------------------------

// sRGB DISPLAY colour, NOT linear. The composite runs AFTER 3.6.3's OETF, so these bytes are the
// bytes that land. This is the OPPOSITE convention from DebugDraw's packDebugColor, which takes linear
// because it records into the HDR pass -- the FIELD NAMES carry the difference, because a caller who
// gets it backwards produces a plausible-looking wrong colour rather than an obvious one. The two
// values are viewport_panel.cpp's SELECTION_PRIMARY_COLOR / SELECTION_COLOR bytes, so the picture's
// palette does not move.
inline constexpr Vec4 SELECTION_OUTLINE_PRIMARY_DEFAULT{1.0F, 0.690196F, 0.250980F, 1.0F};         // 255,176,64,255
inline constexpr Vec4 SELECTION_OUTLINE_SECONDARY_DEFAULT{1.0F, 0.580392F, 0.125490F, 0.745098F};  // 255,148,32,190

// The band across an axis-aligned edge is EXACTLY 2*radius pixels (see the header comment's
// arithmetic). 8 is not a performance ceiling
// -- it is the point past which the band stops reading as an outline and starts reading as a glow,
// and a bounded radius is what makes the tap loop unrollable.
inline constexpr std::uint32_t SELECTION_OUTLINE_MIN_RADIUS = 1;
inline constexpr std::uint32_t SELECTION_OUTLINE_MAX_RADIUS = 8;

struct SelectionOutlineParams {
    Vec4 primaryColorSrgb = SELECTION_OUTLINE_PRIMARY_DEFAULT;
    Vec4 secondaryColorSrgb = SELECTION_OUTLINE_SECONDARY_DEFAULT;
    std::uint32_t radiusPixels = SELECTION_OUTLINE_MIN_RADIUS;
    [[nodiscard]] bool operator==(const SelectionOutlineParams&) const = default;
};

// TOTAL, pure, tier-0. Every channel clamps to [0,1] with a NON-FINITE channel going to 0 -- the
// FINITENESS ARM COMES FIRST, because std::clamp(NaN, lo, hi) returns NaN on libc++ (the 3.7.2 rule,
// hit again by E.1.2's std::min(NaN, 48.0F)). radiusPixels clamps into [MIN, MAX]; 0 becomes MIN,
// because a radius of 0 takes no taps and would make the outline unconditionally absent.
[[nodiscard]] SelectionOutlineParams sanitizeSelectionOutlineParams(const SelectionOutlineParams& params) noexcept;

// ---- the composite ------------------------------------------------------------------------------

struct SelectionOutlineConfig {
    // REQUIRED. The format of the surface composite() writes into -- the editor's RGBA8Unorm OUTPUT
    // target, never the HDR pair. Invalid or a depth format FAILS create(). Named exactly as
    // PostProcessConfig's twin.
    rhi::TextureFormat outputColorFormat = rhi::TextureFormat::Invalid;
    // The pipeline's depth format MUST match the pass it records into (GraphicsPipelineDesc's
    // sentinel). Invalid means "composite into a depth-free frame", which is what the editor's output
    // target is.
    rhi::TextureFormat outputDepthFormat = rhi::TextureFormat::Invalid;
    // Extension-less res:// VFS paths, READ ONLY INSIDE create() and never after -- the stored copy's
    // views are never dereferenced again (PostProcessConfig's own recorded contract).
    std::string_view vertexShaderPath = "res://fullscreen.vert";
    std::string_view fragmentShaderPath = "res://selection_outline.frag";
};

class SelectionOutline {
public:
    [[nodiscard]] static std::optional<SelectionOutline> create(rhi::Device& device, const VirtualFileSystem& shaderVfs,
                                                                const SelectionOutlineConfig& config);

    ~SelectionOutline();                                       // no-op if moved-from
    SelectionOutline(SelectionOutline&&) noexcept;             // USER-DEFINED: transfers + nulls the source
    SelectionOutline& operator=(SelectionOutline&&) noexcept;  // (a defaulted move double-frees both handles)
    SelectionOutline(const SelectionOutline&) = delete;
    SelectionOutline& operator=(const SelectionOutline&) = delete;

    // Records the edge-detect into `output`'s ALREADY-OPEN pass, blended over whatever is there -- in
    // the editor, immediately after PostProcess::resolve, so the colour is exact display bytes.
    // Best-effort and void, matching PostProcess::resolve and ForwardRenderer::draw.
    //
    // An INVALID `mask` -- the default SelectionMaskView, which is what an empty selection and every
    // mask-pass failure return -- records NOTHING and moves compositeCount() not at all. That is the
    // zero-cost path, and it is ASSERTED (OG2), not assumed.
    //
    // THE ASYMMETRY BETWEEN THE TWO INVALID CASES IS DELIBERATE. A DEFAULT view is the ordinary
    // "nothing is selected" state and is SILENT; a view marked `valid` whose texture handle does not
    // resolve is a BUG IN THE PRODUCER and latches a WARN once.
    void composite(Frame& output, const SelectionMaskView& mask, const SelectionOutlineParams& params);

    [[nodiscard]] std::size_t compositeCount() const noexcept;   // draws ISSUED, object lifetime
    [[nodiscard]] bool hasWarnedInvalidMask() const noexcept;    // valid == true but a dead handle
    [[nodiscard]] bool hasWarnedNotRenderable() const noexcept;  // moved-from / pipeline lost

private:
    SelectionOutline(rhi::Device* device, const SelectionOutlineConfig& config,
                     rhi::GraphicsPipelineHandle pipelineHandle, rhi::SamplerHandle samplerHandle) noexcept;
    void destroyAll() noexcept;  // dtor + move-assign share this; no-op when device == nullptr
    void reset() noexcept;       // null every member WITHOUT releasing anything (the moved-from state)

    rhi::Device* device = nullptr;  // non-owning; outlives the composite (contract)
    SelectionOutlineConfig cfg{};
    rhi::GraphicsPipelineHandle pipeline{};
    rhi::SamplerHandle sampler{};
    std::size_t composites = 0;
    bool warnedInvalidMask = false;
    bool warnedNotRenderable = false;
};

}  // namespace engine::render

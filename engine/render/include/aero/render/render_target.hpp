#pragma once
// Aero Engine — render::RenderTarget (task 2.2.3): the OFFSCREEN sibling of render::Renderer. Owns a
// sampleable colour texture (+ an auto-picked depth target) and drives the same
// beginFrame -> record -> endFrame cycle into it instead of a swapchain, so every existing consumer
// of render::Frame (ForwardRenderer::draw, scene_render::SceneRenderer::render) works unchanged.
//
// LIFETIME CONTRACTS (mirroring renderer.hpp):
//   * The rhi::Device passed to create() MUST outlive the RenderTarget.
//   * A Frame must NOT outlive its RenderTarget; each beginFrame() is matched by exactly one
//     endFrame() (or the Frame is dropped, force-ended and submitted, with one WARN).
//   * FORMATS ARE FROZEN AT create(). resize() changes extents ONLY — a pipeline built against
//     colorFormat()/depthFormat() stays valid across any number of resizes (D10).
//
// SYNCHRONISATION: endFrame() submits its own command buffer. SDL_GPU transitions the colour
// attachment back to "ready for a graphics read" when the pass ends, and command buffers submitted
// earlier on the queue order before later ones, so a LATER command buffer (e.g. ImGui's) may sample
// colorTexture() with no explicit barrier. Compute passes are the one documented exception and are
// not used here.
//
// ERROR MODEL (docs/04, mirrors rhi/render): nothing throws. create() -> nullopt (+ ERROR);
// resize() -> false (+ ERROR) leaving the target NOT renderable; beginFrame() -> nullopt.

#include <aero/render/renderer.hpp>  // Frame
#include <aero/rhi/format.hpp>
#include <aero/rhi/handles.hpp>
#include <aero/rhi/types.hpp>

#include <cstdint>
#include <optional>

namespace engine::rhi {
class Device;  // forward-declared, exactly as renderer.hpp does
}  // namespace engine::rhi

namespace engine::render {

// Hard ceiling on either axis. 8192 is comfortably inside every desktop backend's 2D limit and is a
// multiple of every sane quantum. A larger request is CLAMPED (one WARN), never a creation failure.
inline constexpr std::uint32_t RENDER_TARGET_MAX_EXTENT = 8192;

struct RenderTargetConfig {
    // Must support Sampler|ColorTarget on this device (queried at create; a miss FAILS, D8).
    rhi::TextureFormat colorFormat = rhi::TextureFormat::RGBA8Unorm;
    // Auto-picked D32Float -> D24Unorm -> D16Unorm, exactly as RendererConfig::depth does (D9).
    bool depth = true;
    // Allocation granularity in pixels. 1 == exact (the default: textureExtent() == drawExtent()).
    // A consumer that resizes continuously (the editor viewport) sets this to bound reallocation.
    std::uint32_t quantum = 1;
    std::uint32_t maxExtent = RENDER_TARGET_MAX_EXTENT;
};

// PURE, GPU-free, total (D5/D6/AC-14): the allocation extent to use for `requested`, given the
// `current` allocation. Per axis, independently:
//   req  = clamp(requested, 1, maxExtent)
//   want = min(roundUpToMultiple(req, max(quantum,1)), maxExtent)
//   current == 0            -> want    (first allocation)
//   req > current           -> want    (grow: the current allocation cannot hold it)
//   want * 2 <= current     -> want    (shrink, only once the need at least halves — hysteresis)
//   otherwise               -> current (keep: no reallocation)
// POSTCONDITION, on every branch and every axis: result >= req.
// Computed in 64-bit internally and saturated at `maxExtent` before narrowing, so an extreme
// `maxExtent` cannot wrap the round-up and silently return an allocation SMALLER than the request
// (INV-1) — a 32-bit `((req + q - 1) / q) * q` wraps at `req == maxExtent == UINT32_MAX, q == 64`.
[[nodiscard]] rhi::Extent2D nextTargetExtent(rhi::Extent2D requested, rhi::Extent2D current, std::uint32_t quantum,
                                             std::uint32_t maxExtent) noexcept;

class RenderTarget {
public:
    [[nodiscard]] static std::optional<RenderTarget> create(rhi::Device& device, rhi::Extent2D requested,
                                                            const RenderTargetConfig& config = {});

    ~RenderTarget();  // destroys both textures (no-op if moved-from)
    RenderTarget(RenderTarget&&) noexcept;
    RenderTarget& operator=(RenderTarget&&) noexcept;
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    // Apply the sizing policy for `requested`. Reallocates only when nextTargetExtent says so; always
    // updates drawExtent(). false (+ ERROR) ONLY on a real texture-creation failure, which leaves the
    // target NOT renderable (colorTexture() invalid, beginFrame() -> nullopt) but destructible.
    bool resize(rhi::Extent2D requested);

    // Acquire a command buffer, open a pass that CLEARS the whole colour attachment to `clearColor`
    // (and depth to 1.0), and set viewport+scissor to the drawExtent() sub-rect (F13/F14).
    // nullopt when the target is not renderable or the command buffer cannot be acquired.
    [[nodiscard]] std::optional<Frame> beginFrame(const rhi::Color& clearColor);

    // End the pass and submit. Consumes `frame`. false (+ log) on submit failure or a moved-from /
    // already-ended frame — the Renderer::endFrame contract verbatim. Does NOT present anything.
    bool endFrame(Frame frame);

    // The sampleable colour texture. Sampler|ColorTarget. Invalid on a not-renderable target.
    // STABLE between reallocations only — re-read it after any resize() (E7).
    [[nodiscard]] rhi::TextureHandle colorTexture() const noexcept;
    [[nodiscard]] rhi::TextureFormat colorFormat() const noexcept;
    [[nodiscard]] rhi::TextureFormat depthFormat() const noexcept;  // Invalid when config.depth was false

    // The rendered sub-rect: what beginFrame sets viewport/scissor to, and what Frame::extent()
    // reports (so a projection's aspect ratio is right for free — F12).
    [[nodiscard]] rhi::Extent2D drawExtent() const noexcept;
    // The allocation. >= drawExtent() on both axes, always. Equal to it when quantum == 1.
    // The denominator for a UV sub-rect: uvMax = drawExtent / textureExtent.
    [[nodiscard]] rhi::Extent2D textureExtent() const noexcept;

private:
    RenderTarget(rhi::Device* device, const RenderTargetConfig& config, rhi::TextureFormat depthFormat) noexcept;
    bool allocate(rhi::Extent2D newAllocExtent);  // create both textures at newAllocExtent; destroys old first
    void destroyAll() noexcept;                   // dtor + move-assign share this (ForwardRenderer's C1 shape)

    rhi::Device* device = nullptr;  // non-owning; outlives the target (contract)
    RenderTargetConfig cfg{};
    rhi::TextureFormat depthFormatValue = rhi::TextureFormat::Invalid;
    rhi::TextureHandle color{};
    rhi::TextureHandle depth{};
    rhi::Extent2D allocExtent{};  // == textureExtent(); {0,0} == not renderable
    rhi::Extent2D drawRect{};     // == drawExtent()
};

}  // namespace engine::render

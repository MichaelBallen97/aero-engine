#pragma once
// Aero Engine — rhi Device (task 0.4.1; ADR-002 "the sacred wrapper"). The single owner of the GPU:
// it creates/destroys every resource handle, records and submits every frame, and presents. The
// backend (SDL_GPU) lives entirely behind the pimpl in src/sdl_gpu_backend.cpp — task 0.4.2; until
// that lands, member functions are DECLARED here and deliberately UNDEFINED (odr-using one is a
// link error, not a runtime surprise).
//
// LIFETIME CONTRACTS (E-numbered cases in the 0.4.1 spec):
//   * Create AT MOST ONE Device, on the main thread, with a live platform::Context outliving it.
//   * A Window must outlive any Swapchain created from it; destroy the swapchain first.
//   * ~Device: waits for GPU idle, then releases every still-live resource with a debug WARN per
//     category (leak diagnosis) — but relying on that is a bug in the caller, not a feature.
//   * Moved-from Device: inert; only destruction/assignment are legal on it (Window precedent).
//
// THREADING CONTRACT (v0, D13): every member below must be called on the thread that created the
// Context (asserted in debug by the backend). Multi-threaded recording is a planned Phase 3+
// extension — the backend's model already permits per-thread command buffers.
//
// ERROR MODEL (D9, docs/04): nothing throws. Creation returns an INVALID handle on failure (details
// at AERO_LOG_ERROR); operations on invalid/stale handles are logged no-ops; bool returns report
// external failures. Stale-handle detection is generational (core SlotMap) — use-after-submit,
// use-after-end, and use-after-destroy are all caught, logged, and ignored.

#include <aero/rhi/descriptors.hpp>
#include <aero/rhi/format.hpp>
#include <aero/rhi/handles.hpp>
#include <aero/rhi/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace engine::platform {
class Window;  // forward-declared on purpose: rhi's one platform type; no include needed for a ref
}  // namespace engine::platform

namespace engine::rhi {

// What acquireSwapchainTexture yields: a transient texture handle plus its REAL pixel size — during
// a live resize this extent is the truth, not Window::pixelSize(). The handle dies at submit/cancel.
struct SwapchainTexture {
    TextureHandle texture;
    Extent2D extent;
};

class Device {
public:
    // Create the process's GPU device. nullopt (+ ERROR log) if no usable GPU/driver exists, if the
    // requested DriverPreference is unavailable, or if a Device already lives (one-per-process).
    // Requires a live platform::Context (any mode — headless Contexts are fine for device-only use;
    // windows/swapchains obviously need a real one).
    [[nodiscard]] static std::optional<Device> create(const DeviceDesc& desc = {});

    ~Device();
    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // --- identity & capability queries ---------------------------------------------------------

    // The ONE bytecode dialect this device consumes (decided at creation from the platform's
    // driver). Shader loading (task 0.4.4) picks the matching cooked artifact with this.
    [[nodiscard]] ShaderFormat shaderFormat() const noexcept;

    // Engine-owned driver name for logs: "vulkan", "metal", or "direct3d12".
    [[nodiscard]] std::string_view backendName() const noexcept;

    // Whether `format` supports ALL the given usage bits on this device. Required before creating
    // D24*/D32* depth textures — drivers guarantee one family or the other, not both (format.hpp).
    [[nodiscard]] bool supportsTextureFormat(TextureFormat format, TextureUsage usage) const;

    // Block until the GPU finishes everything submitted. Init/teardown tool, never per-frame.
    bool waitIdle();

    // --- swapchain ------------------------------------------------------------------------------

    // Claim `window` for presentation. One swapchain per window; the Window must outlive it.
    // The swapchain's pixel size tracks the window automatically (resizes handled by the backend;
    // renderers consume the extent returned by acquireSwapchainTexture each frame).
    [[nodiscard]] SwapchainHandle createSwapchain(const platform::Window& window, const SwapchainDesc& desc = {});
    // Release the claim and invalidate the handle. Pipelines built against this swapchain's format
    // remain valid objects — reusable with another swapchain only if ITS format matches (ask again).
    void destroySwapchain(SwapchainHandle swapchain);

    // The format of this swapchain's textures — what a pipeline targeting it must declare in its
    // ColorTargetDesc. Stable for the life of the swapchain in v0 (SDR-only). Invalid on stale handle.
    [[nodiscard]] TextureFormat swapchainFormat(SwapchainHandle swapchain) const;

    // Vsync is always supported. Query before setPresentMode(Immediate|Mailbox).
    [[nodiscard]] bool supportsPresentMode(SwapchainHandle swapchain, PresentMode mode) const;
    // Fails (false + log) if unsupported — never silently downgrades.
    bool setPresentMode(SwapchainHandle swapchain, PresentMode mode);

    // --- resources ------------------------------------------------------------------------------

    // All creates: invalid handle + ERROR log on failure or invalid desc (validation table, spec §5).
    // All destroys: immediate handle invalidation; the backend defers the actual GPU release until
    // in-flight frames finish (safe to destroy a resource submitted this frame). Stale/invalid
    // handles: logged no-op.
    [[nodiscard]] BufferHandle createBuffer(const BufferDesc& desc);
    void destroyBuffer(BufferHandle buffer);

    [[nodiscard]] TextureHandle createTexture(const TextureDesc& desc);
    void destroyTexture(TextureHandle texture);

    [[nodiscard]] SamplerHandle createSampler(const SamplerDesc& desc);
    void destroySampler(SamplerHandle sampler);

    // Shaders exist to build pipelines; destroying a shader AFTER pipeline creation is safe and
    // normal (the pipeline holds what it needs).
    [[nodiscard]] ShaderHandle createShader(const ShaderDesc& desc);
    void destroyShader(ShaderHandle shader);

    [[nodiscard]] GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc);
    void destroyGraphicsPipeline(GraphicsPipelineHandle pipeline);

    // Debug names shown by GPU debuggers (RenderDoc/Xcode). No-op on invalid handles; cheap.
    void setDebugName(BufferHandle buffer, std::string_view name);
    void setDebugName(TextureHandle texture, std::string_view name);

    // BLOCKING uploads (D14): copy CPU bytes into a resource and wait for completion. Init-time
    // convenience for Phase 0/1 (cube geometry, first textures) — NOT a per-frame path; per-frame
    // data belongs in push uniforms until the streaming API lands (Phase 3).
    //   uploadBuffer: dstOffset + data.size() must fit in the buffer.
    //   uploadTexture: tightly-packed full mip level; data.size() must equal
    //                  texelBlockSize(format) * mipWidth * mipHeight; depth formats are rejected
    //                  (texelBlockSize == 0 — depth targets are GPU-written).
    bool uploadBuffer(BufferHandle buffer, std::uint32_t dstOffset, std::span<const std::byte> data);
    bool uploadTexture(TextureHandle texture, std::uint32_t mipLevel, std::span<const std::byte> data);

    // --- frame flow (D7) ------------------------------------------------------------------------
    // Shape of a frame:
    //     auto cmd = device.acquireCommandBuffer();
    //     if (auto sc = device.acquireSwapchainTexture(cmd, swapchain)) {
    //         const ColorAttachment color{.texture = sc->texture};  // defaults: Clear -> Store
    //         auto pass = device.beginRenderPass(cmd, {.colorAttachments = {&color, 1}});
    //         ... bind/draw ...
    //         device.endRenderPass(pass);
    //         device.submit(cmd);                  // presents sc->texture
    //     } else {
    //         device.cancel(cmd);                  // window not presentable (e.g. minimized): skip
    //     }

    // Invalid handle on failure. Acquiring MULTIPLE command buffers before submitting is legal and
    // normal (e.g. one for uploads, one for drawing; submission order = execution start order).
    [[nodiscard]] CommandBufferHandle acquireCommandBuffer();

    // Blocks until the swapchain can hand out an image (this is the vsync pacing point), then
    // acquires it onto `cmd`. nullopt when the window cannot present right now (minimized) — NOT an
    // error; skip the frame via submit() or cancel(). The returned texture:
    //   * is valid ONLY on `cmd`, and only until submit/cancel;
    //   * is WRITE-ONLY: color attachment yes; bindFragmentSamplers/uploads REJECT it;
    //   * presents automatically when `cmd` is submitted — there is no separate present() call.
    [[nodiscard]] std::optional<SwapchainTexture> acquireSwapchainTexture(CommandBufferHandle cmd,
                                                                          SwapchainHandle swapchain);

    // Submit `cmd` (presenting any swapchain texture acquired on it) and invalidate it plus its
    // transient handles. False + log on backend failure (the handles are invalidated regardless).
    // Ordering guarantee: commands in an earlier submit BEGIN before any command in a later one.
    bool submit(CommandBufferHandle cmd);

    // Abandon `cmd` without executing anything; invalidates it. ILLEGAL after a successful
    // acquireSwapchainTexture on it (backend constraint) -> false + log, buffer still consumed.
    bool cancel(CommandBufferHandle cmd);

    // Set push-uniform data for a slot on this command buffer; it persists across passes within the
    // buffer until pushed again (so a camera matrix can be pushed once per frame). Callable inside
    // or outside a render pass. Data must follow std140 layout rules — in practice: pad Vec3 to 16
    // bytes; Mat4 is fine as-is. Slots correspond to the shader's uniform-buffer declarations
    // (register/binding space is the shaderc pipeline's contract, task 0.4.3/0.4.4).
    void pushVertexUniforms(CommandBufferHandle cmd, std::uint32_t slot, std::span<const std::byte> data);
    void pushFragmentUniforms(CommandBufferHandle cmd, std::uint32_t slot, std::span<const std::byte> data);

    // --- render pass recording (D8) --------------------------------------------------------------

    // Open a render pass on `cmd`. Exactly ONE pass may be open per command buffer at a time
    // (nesting/overlap: invalid handle + log). All attachments must share sampleCount and extent;
    // >= 1 color attachment in v0. A default full-target viewport & scissor are set by the backend.
    [[nodiscard]] RenderPassHandle beginRenderPass(CommandBufferHandle cmd, const RenderPassDesc& desc);

    // Close the pass and invalidate its handle. Required before submit; if a pass is still open at
    // submit, the backend logs an ERROR, force-ends it, and submits anyway (E5).
    void endRenderPass(RenderPassHandle pass);

    // All recording calls below: logged no-op on stale/invalid pass or resource handles.
    void bindGraphicsPipeline(RenderPassHandle pass, GraphicsPipelineHandle pipeline);

    // Both default to the full target when never called (backend behavior at pass begin).
    void setViewport(RenderPassHandle pass, const Viewport& viewport);
    void setScissor(RenderPassHandle pass, const Rect& scissor);

    // The buffer must have Vertex usage; `slot` matches a VertexBufferLayout::slot of the pipeline.
    void bindVertexBuffer(RenderPassHandle pass, std::uint32_t slot, BufferHandle buffer, std::uint32_t offset = 0);
    // The buffer must have Index usage.
    void bindIndexBuffer(RenderPassHandle pass, BufferHandle buffer, IndexType indexType, std::uint32_t offset = 0);
    // Fragment-stage texture+sampler pairs starting at firstSlot (slot order = the shader's
    // declaration order; the shaderc contract). Vertex-stage sampling: future append.
    void bindFragmentSamplers(RenderPassHandle pass, std::uint32_t firstSlot,
                              std::span<const TextureSamplerBinding> bindings);

    // A bound pipeline is required before any draw (validated: logged no-op otherwise).
    void draw(RenderPassHandle pass, std::uint32_t vertexCount, std::uint32_t instanceCount = 1,
              std::uint32_t firstVertex = 0, std::uint32_t firstInstance = 0);
    void drawIndexed(RenderPassHandle pass, std::uint32_t indexCount, std::uint32_t instanceCount = 1,
                     std::uint32_t firstIndex = 0, std::int32_t vertexOffset = 0, std::uint32_t firstInstance = 0);

private:
    struct Impl;  // the ONLY place SDL_GPU exists (src/sdl_gpu_backend.cpp, task 0.4.2)
    explicit Device(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl;
};

}  // namespace engine::rhi

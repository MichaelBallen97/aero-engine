#pragma once
// Aero Engine — INTERNAL rhi seam (task 2.1.1). NOT a public header: shipped only through the
// aero::rhi_internal INTERFACE target, consumed PRIVATE by the ONE privileged client that must
// drive ImGui's SDL_GPU backend on the engine's device — the editor (editor/src/imgui_layer.cpp).
//
// Handles are returned as void* ON PURPOSE. The rhi-boundary guard (check-rhi-boundary.sh) rejects
// any SDL_<...>GPU token used as code anywhere in engine/ outside sdl_gpu_backend.cpp; a typed
// SDL_GPUDevice* here would trip it. The client casts the void* back to the SDL type it already
// speaks (the editor links SDL3::SDL3 directly). Definitions live in sdl_gpu_backend.cpp — the one
// allowlisted TU — so this header names no SDL type at all, not even in a comment-as-code.
//
// LIFETIME: the returned pointers alias engine-owned SDL_GPU objects. `device()` is valid for the
// life of the Device. `commandBuffer(cmd)`/`renderPass(pass)` are valid ONLY while their engine
// handle is live (before submit/cancel resp. before endRenderPass); nullptr on a stale/invalid
// handle. The client must use them within the same frame flow, exactly like the handles they mirror.
// Adding an accessor here is a spec-level decision (the platform_internal precedent).
// `texture(t)` is valid while the TextureHandle is live (until destroyTexture / ~Device) and is
// nullptr for a stale handle AND for a SWAPCHAIN-ACQUIRED texture -- those are write-only (see
// device.hpp's acquire contract), so handing one to a sampler would be a silent GPU error.

#include <aero/rhi/handles.hpp>  // CommandBufferHandle, RenderPassHandle, TextureHandle

namespace engine::rhi {
class Device;
namespace internal {

struct NativeDeviceAccessor {
    // SDL_GPUDevice* as void*.
    [[nodiscard]] static void* device(const Device& device) noexcept;
    // SDL_GPUCommandBuffer* as void*; nullptr if cmd is stale/invalid.
    [[nodiscard]] static void* commandBuffer(const Device& device, CommandBufferHandle cmd) noexcept;
    // SDL_GPURenderPass* as void*; nullptr if pass is stale/invalid.
    [[nodiscard]] static void* renderPass(const Device& device, RenderPassHandle pass) noexcept;
    // SDL_GPUTexture* as void*; nullptr for a stale/invalid handle or a swapchain-acquired one.
    [[nodiscard]] static void* texture(const Device& device, TextureHandle texture) noexcept;
};

}  // namespace internal
}  // namespace engine::rhi

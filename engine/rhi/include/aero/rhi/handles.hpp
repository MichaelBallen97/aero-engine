#pragma once
// Aero Engine — rhi handle vocabulary (task 0.4.1; ADR-002). The engine's rendering resources are
// named by generational handles (ADR-001 mitigation #1), minted and validated by the Device that
// owns them (device.hpp; backing SlotMaps land with the backend, task 0.4.2). A stale handle — the
// resource destroyed, the command buffer submitted, the render pass ended — no longer matches its
// slot's generation, so every operation on it is a logged no-op instead of undefined behavior.
//
// The tags are phantom types, DELIBERATELY NEVER DEFINED (the Handle<Job> precedent, core/jobs.hpp):
// Handle<Tag> uses the tag only to make the eight aliases mutually non-convertible at compile time.
// A BufferHandle cannot be passed where a TextureHandle is wanted.
//
// Lifetime vocabulary used across this module's contracts:
//   * PERSISTENT handles (Buffer, Texture, Sampler, Shader, GraphicsPipeline, Swapchain) stay valid
//     until the matching destroy*() — or ~Device, which releases stragglers with a debug warning.
//   * TRANSIENT handles (CommandBuffer, RenderPass, and swapchain-acquired Textures) are minted by
//     the frame flow and invalidated by it: a RenderPass at endRenderPass(); a CommandBuffer and
//     everything acquired on it at submit()/cancel().

#include <aero/core/handle.hpp>

namespace engine::rhi {

struct Buffer;            // GPU memory for vertex/index data (BufferDesc)
struct Texture;           // an image resource: sampled, color target, or depth target (TextureDesc)
struct Sampler;           // how shaders filter/address a texture (SamplerDesc)
struct Shader;            // one compiled shader stage, format per Device::shaderFormat() (ShaderDesc)
struct GraphicsPipeline;  // the full immutable draw-state bundle (GraphicsPipelineDesc)
struct Swapchain;         // a window claimed for presentation (SwapchainDesc)
struct CommandBuffer;     // one frame's recording context, transient (Device::acquireCommandBuffer)
struct RenderPass;        // an open render pass on a command buffer, transient (Device::beginRenderPass)

using BufferHandle = Handle<Buffer>;
using TextureHandle = Handle<Texture>;
using SamplerHandle = Handle<Sampler>;
using ShaderHandle = Handle<Shader>;
using GraphicsPipelineHandle = Handle<GraphicsPipeline>;
using SwapchainHandle = Handle<Swapchain>;
using CommandBufferHandle = Handle<CommandBuffer>;
using RenderPassHandle = Handle<RenderPass>;

}  // namespace engine::rhi

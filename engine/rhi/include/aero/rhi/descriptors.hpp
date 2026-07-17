#pragma once
// Aero Engine — rhi descriptors (task 0.4.1): the parameter structs for Device creation calls and
// render-pass begin. ALL of them are trivially-copyable aggregates built for designated
// initializers (D12): scalars, enums, handles, std::span views, std::optional of PODs — no strings,
// no owning containers. Spans and string_views inside a desc are BORROWED for the duration of the
// Device call that receives them, nothing more; the backend copies what it must keep.
//
// Defaults are load-bearing (D16): a default-initialized desc is the engine-conventional one
// (ADR-005: CCW front faces, back-face culling, depth [0,1] with 0 = near, clear-depth 1.0,
// compare Less, vsync, no blending, one sample). Fields marked "required" reject their default at
// create time (invalid handle + AERO_LOG_ERROR) — the aggregate must stay default-constructible,
// so validation, not the type system, enforces them.

#include <aero/rhi/format.hpp>
#include <aero/rhi/handles.hpp>
#include <aero/rhi/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace engine::rhi {

// --- device ---------------------------------------------------------------------------------

struct DeviceDesc {
    // Backend validation/debug layers. Default ON — this whole phase is bring-up; the exported
    // runtime (task 5.2.1) passes false explicitly. Costs nothing when the platform's validation
    // tooling isn't installed.
    bool enableDebugLayer = true;
    // Prefer the energy-efficient GPU on dual-GPU machines.
    bool preferLowPower = false;
    // Dev/debug override; Auto picks the platform's native API. Creation FAILS (nullopt) if the
    // named driver isn't available on this machine — it does not fall back.
    DriverPreference driver = DriverPreference::Auto;
};

// --- swapchain ------------------------------------------------------------------------------

struct SwapchainDesc {
    // Vsync is always supported (and is 0.5.3's gate mode). Immediate/Mailbox: query
    // Device::supportsPresentMode first; createSwapchain FAILS rather than silently downgrading
    // if an unsupported mode is requested.
    PresentMode presentMode = PresentMode::Vsync;
};

// --- resources ------------------------------------------------------------------------------

struct BufferDesc {
    BufferUsage usage = BufferUsage::None;  // required: at least one usage bit
    std::uint32_t size = 0;                 // required: bytes, > 0 (backend-wide 32-bit limit, E9)
};

// v0 textures are 2D, single-layer (D6). A `type` field (cube/array/3D) and layer counts are a
// planned additive change (Phase 3 skybox/shadows).
struct TextureDesc {
    TextureFormat format = TextureFormat::Invalid;  // required
    TextureUsage usage = TextureUsage::None;        // required: at least one bit
    std::uint32_t width = 0;                        // required: > 0
    std::uint32_t height = 0;                       // required: > 0
    std::uint32_t mipLevels = 1;                    // 1..floor(log2(max(w,h)))+1 (E10)
    SampleCount sampleCount = SampleCount::One;     // >One requires ColorTarget or DepthStencilTarget
                                                    // usage and mipLevels == 1 (E11)
};

struct SamplerDesc {
    Filter minFilter = Filter::Linear;
    Filter magFilter = Filter::Linear;
    MipmapMode mipmapMode = MipmapMode::Linear;
    AddressMode addressU = AddressMode::Repeat;
    AddressMode addressV = AddressMode::Repeat;
    AddressMode addressW = AddressMode::Repeat;
    float mipLodBias = 0.0F;  // backend note: a no-op on Metal (SDL documents this); shaders that
                              // need bias must apply it themselves to be portable
    float minLod = 0.0F;
    float maxLod = 1000.0F;  // effectively unclamped (the Vulkan LOD_CLAMP_NONE idiom)
    bool enableAnisotropy = false;
    float maxAnisotropy = 1.0F;             // used only when enableAnisotropy
    bool enableCompare = false;             // depth-compare sampler (shadow maps, Phase 3)
    CompareOp compareOp = CompareOp::Less;  // used only when enableCompare
};

// One compiled shader STAGE. The engine cannot reflect bytecode: the four resource counts MUST
// match what the shader declares (the shaderc pipeline, task 0.4.3, emits them alongside the
// bytecode; task 0.4.4 threads them through). Wrong counts are undetectable here and produce
// backend-defined failures — treat them as part of the artifact, never hand-typed.
struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    ShaderFormat format = ShaderFormat::SpirV;  // must equal Device::shaderFormat() (E20)
    std::span<const std::byte> bytecode;        // required: non-empty; borrowed for the call
    std::string_view entryPoint = "main";       // shaderc convention
    std::uint32_t samplerCount = 0;
    std::uint32_t storageTextureCount = 0;  // accepted now; bind calls arrive with compute
    std::uint32_t storageBufferCount = 0;   //   "
    std::uint32_t uniformBufferCount = 0;   // push-uniform slots used (pushVertexUniforms...)
};

// --- graphics pipeline ------------------------------------------------------------------------

// One vertex buffer slot's layout.
struct VertexBufferLayout {
    std::uint32_t slot = 0;   // binding slot, matches bindVertexBuffer's slot
    std::uint32_t pitch = 0;  // required: bytes between consecutive elements
    VertexInputRate inputRate = VertexInputRate::Vertex;
};

// One vertex attribute; location must be unique across the pipeline.
struct VertexAttribute {
    std::uint32_t location = 0;    // the shader input location
    std::uint32_t bufferSlot = 0;  // which VertexBufferLayout feeds it
    VertexFormat format = VertexFormat::Float4;
    std::uint32_t offset = 0;  // bytes from the element start
};

struct RasterizerState {
    FillMode fillMode = FillMode::Fill;
    CullMode cullMode = CullMode::Back;                 // engine convention
    FrontFace frontFace = FrontFace::CounterClockwise;  // RH / glTF winding (ADR-005)
    bool enableDepthBias = false;
    float depthBiasConstant = 0.0F;
    float depthBiasClamp = 0.0F;
    float depthBiasSlope = 0.0F;
    bool enableDepthClip = true;  // false = depth clamp (backend note: D3D12 clamps regardless)
};

struct StencilOpState {
    StencilOp failOp = StencilOp::Keep;
    StencilOp passOp = StencilOp::Keep;
    StencilOp depthFailOp = StencilOp::Keep;
    CompareOp compareOp = CompareOp::Always;
};

struct DepthStencilState {
    bool enableDepthTest = false;
    bool enableDepthWrite = false;          // ignored (off) while enableDepthTest is false
    CompareOp compareOp = CompareOp::Less;  // 0 = near => Less is "closer wins" (ADR-005)
    bool enableStencilTest = false;
    StencilOpState frontStencil = {};
    StencilOpState backStencil = {};
    std::uint8_t stencilCompareMask = 0xFF;
    std::uint8_t stencilWriteMask = 0xFF;
};

// Per-color-target blending. Default: blending OFF, mask All. (SDL's separate
// enable_color_write_mask bool is collapsed: the backend enables it iff writeMask != All.)
struct BlendState {
    bool enableBlend = false;
    BlendFactor srcColorFactor = BlendFactor::One;
    BlendFactor dstColorFactor = BlendFactor::Zero;
    BlendOp colorOp = BlendOp::Add;
    BlendFactor srcAlphaFactor = BlendFactor::One;
    BlendFactor dstAlphaFactor = BlendFactor::Zero;
    BlendOp alphaOp = BlendOp::Add;
    ColorWriteMask writeMask = ColorWriteMask::All;
};

// A color target's format (+ blending) as baked into a pipeline. For swapchain rendering the
// format comes from Device::swapchainFormat(...).
struct ColorTargetDesc {
    TextureFormat format = TextureFormat::Invalid;  // required
    BlendState blend = {};
};

// The immutable draw-state bundle. Shaders may be destroyed after the pipeline is created (the
// pipeline keeps what it needs). depthStencilFormat == Invalid <=> the pipeline has NO depth target
// (SDL's separate has_depth_stencil_target bool is collapsed into that sentinel).
struct GraphicsPipelineDesc {
    ShaderHandle vertexShader;                          // required: valid & Vertex stage
    ShaderHandle fragmentShader;                        // required: valid & Fragment stage
    std::span<const VertexBufferLayout> vertexBuffers;  // may be empty (vertex-pulling / gl_VertexIndex tricks)
    std::span<const VertexAttribute> vertexAttributes;  // may be empty
    PrimitiveType primitiveType = PrimitiveType::TriangleList;
    RasterizerState rasterizer = {};
    SampleCount sampleCount = SampleCount::One;  // must match the targets rendered into
    DepthStencilState depthStencil = {};
    std::span<const ColorTargetDesc> colorTargets;              // required: >= 1 in v0
    TextureFormat depthStencilFormat = TextureFormat::Invalid;  // Invalid = no depth attachment
};

// --- render pass ------------------------------------------------------------------------------

// One color attachment for beginRenderPass. The texture must have ColorTarget usage (or be a
// swapchain acquisition) and its format/sampleCount must match the bound pipeline's.
struct ColorAttachment {
    TextureHandle texture;  // required
    LoadOp loadOp = LoadOp::Clear;
    StoreOp storeOp = StoreOp::Store;
    Color clearColor = {};  // used when loadOp == Clear
};

struct DepthStencilAttachment {
    TextureHandle texture;  // required: DepthStencilTarget usage
    LoadOp depthLoadOp = LoadOp::Clear;
    StoreOp depthStoreOp = StoreOp::DontCare;  // depth rarely needs storing (SDL's own guidance)
    float clearDepth = 1.0F;                   // far plane under 0-near/Less (ADR-005)
    LoadOp stencilLoadOp = LoadOp::DontCare;
    StoreOp stencilStoreOp = StoreOp::DontCare;
    std::uint8_t clearStencil = 0;
};

struct RenderPassDesc {
    std::span<const ColorAttachment> colorAttachments;        // >= 1 in v0
    std::optional<DepthStencilAttachment> depthStencil = {};  // nullopt = no depth this pass
};

}  // namespace engine::rhi

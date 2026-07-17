// Aero Engine — the SDL_GPU backend (task 0.4.2; ADR-002 "the sacred wrapper"). The ONE TU in the
// whole repo that includes <SDL3/SDL_gpu.h>: it defines every engine::rhi::Device member declared
// in device.hpp, mapping the engine's SDL_GPU-free vocabulary onto real SDL_GPU calls (see the
// 0.4.1 spec §3.13 mapping table and the 0.4.2 spec for the Impl architecture this file follows).
//
// Error model (D13, docs/04): nothing throws. Stale/invalid-handle paths and validation rejections
// are AERO_LOG_ERROR + no-op/false/invalid-handle WITHOUT assert(false) — the Debug lanes run the
// full stale-handle test battery, so an abort there would be untestable by construction (plan C-1,
// a recorded deviation from the spec D13 parenthetical). assert() is reserved for: (a) the D12
// thread-ownership checks (never fire in a single-threaded test), and (b) internal invariants that
// indicate a BACKEND bug (e.g. a live slot holding a null SDL pointer, or E19's "the chosen shader
// format bit must be in SDL_GetGPUShaderFormats").

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/slot_map.hpp>
#include <aero/platform/internal/native_window.hpp>
#include <aero/platform/window.hpp>
#include <aero/rhi/rhi.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace engine::rhi {
namespace {

// --- mapping helpers (total, constexpr/noexcept switches over every engine enum) ----------------
// Every switch also carries a defensive `default:` even where all enumerators are handled: the
// enums have no reserved-invalid value of their own (unlike TextureFormat), so a future value added
// without updating this file falls back to a safe, documented default rather than undefined SDL
// behavior. format.cpp's exhaustive-switch style (no defensive default) is deliberately NOT mirrored
// here for that reason.

constexpr SDL_GPUTextureFormat toSdl(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::R8Unorm:
            return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        case TextureFormat::RG8Unorm:
            return SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
        case TextureFormat::RGBA8Unorm:
            return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        case TextureFormat::RGBA8UnormSrgb:
            return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
        case TextureFormat::BGRA8Unorm:
            return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
        case TextureFormat::BGRA8UnormSrgb:
            return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
        case TextureFormat::R16Float:
            return SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
        case TextureFormat::RG16Float:
            return SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
        case TextureFormat::RGBA16Float:
            return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        case TextureFormat::R32Float:
            return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
        case TextureFormat::RG32Float:
            return SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
        case TextureFormat::RGBA32Float:
            return SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
        case TextureFormat::R11G11B10Ufloat:
            return SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
        case TextureFormat::D16Unorm:
            return SDL_GPU_TEXTUREFORMAT_D16_UNORM;
        case TextureFormat::D24Unorm:
            return SDL_GPU_TEXTUREFORMAT_D24_UNORM;
        case TextureFormat::D32Float:
            return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        case TextureFormat::D24UnormS8Uint:
            return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
        case TextureFormat::D32FloatS8Uint:
            return SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
        default:
            return SDL_GPU_TEXTUREFORMAT_INVALID;  // Invalid, Count
    }
}

// E17/E30 reverse map: exactly the four SDR 8-bit swapchain formats; anything else is a hardening
// case (unreachable in v0's SDR-only world) that reports Invalid rather than guessing.
constexpr TextureFormat fromSdlSwapchainFormat(SDL_GPUTextureFormat format) noexcept {
    switch (format) {
        case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
            return TextureFormat::RGBA8Unorm;
        case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
            return TextureFormat::BGRA8Unorm;
        case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
            return TextureFormat::RGBA8UnormSrgb;
        case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:
            return TextureFormat::BGRA8UnormSrgb;
        default:
            return TextureFormat::Invalid;
    }
}

constexpr SDL_GPUVertexElementFormat toSdl(VertexFormat format) noexcept {
    switch (format) {
        case VertexFormat::Float:
            return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
        case VertexFormat::Float2:
            return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        case VertexFormat::Float3:
            return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
        case VertexFormat::Float4:
            return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        case VertexFormat::Int:
            return SDL_GPU_VERTEXELEMENTFORMAT_INT;
        case VertexFormat::Int2:
            return SDL_GPU_VERTEXELEMENTFORMAT_INT2;
        case VertexFormat::Int3:
            return SDL_GPU_VERTEXELEMENTFORMAT_INT3;
        case VertexFormat::Int4:
            return SDL_GPU_VERTEXELEMENTFORMAT_INT4;
        case VertexFormat::Uint:
            return SDL_GPU_VERTEXELEMENTFORMAT_UINT;
        case VertexFormat::Uint2:
            return SDL_GPU_VERTEXELEMENTFORMAT_UINT2;
        case VertexFormat::Uint3:
            return SDL_GPU_VERTEXELEMENTFORMAT_UINT3;
        case VertexFormat::Uint4:
            return SDL_GPU_VERTEXELEMENTFORMAT_UINT4;
        case VertexFormat::Byte4Norm:
            return SDL_GPU_VERTEXELEMENTFORMAT_BYTE4_NORM;
        case VertexFormat::UByte4Norm:
            return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
        case VertexFormat::Half2:
            return SDL_GPU_VERTEXELEMENTFORMAT_HALF2;
        case VertexFormat::Half4:
            return SDL_GPU_VERTEXELEMENTFORMAT_HALF4;
        default:
            return SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
    }
}

constexpr SDL_GPUVertexInputRate toSdl(VertexInputRate rate) noexcept {
    switch (rate) {
        case VertexInputRate::Vertex:
            return SDL_GPU_VERTEXINPUTRATE_VERTEX;
        case VertexInputRate::Instance:
            return SDL_GPU_VERTEXINPUTRATE_INSTANCE;
        default:
            return SDL_GPU_VERTEXINPUTRATE_VERTEX;
    }
}

constexpr SDL_GPUPrimitiveType toSdl(PrimitiveType type) noexcept {
    switch (type) {
        case PrimitiveType::TriangleList:
            return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        case PrimitiveType::TriangleStrip:
            return SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
        case PrimitiveType::LineList:
            return SDL_GPU_PRIMITIVETYPE_LINELIST;
        case PrimitiveType::LineStrip:
            return SDL_GPU_PRIMITIVETYPE_LINESTRIP;
        case PrimitiveType::PointList:
            return SDL_GPU_PRIMITIVETYPE_POINTLIST;
        default:
            return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    }
}

constexpr SDL_GPUFillMode toSdl(FillMode mode) noexcept {
    switch (mode) {
        case FillMode::Fill:
            return SDL_GPU_FILLMODE_FILL;
        case FillMode::Line:
            return SDL_GPU_FILLMODE_LINE;
        default:
            return SDL_GPU_FILLMODE_FILL;
    }
}

constexpr SDL_GPUCullMode toSdl(CullMode mode) noexcept {
    switch (mode) {
        case CullMode::None:
            return SDL_GPU_CULLMODE_NONE;
        case CullMode::Front:
            return SDL_GPU_CULLMODE_FRONT;
        case CullMode::Back:
            return SDL_GPU_CULLMODE_BACK;
        default:
            return SDL_GPU_CULLMODE_NONE;
    }
}

constexpr SDL_GPUFrontFace toSdl(FrontFace face) noexcept {
    switch (face) {
        case FrontFace::CounterClockwise:
            return SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        case FrontFace::Clockwise:
            return SDL_GPU_FRONTFACE_CLOCKWISE;
        default:
            return SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    }
}

constexpr SDL_GPUCompareOp toSdl(CompareOp op) noexcept {
    switch (op) {
        case CompareOp::Never:
            return SDL_GPU_COMPAREOP_NEVER;
        case CompareOp::Less:
            return SDL_GPU_COMPAREOP_LESS;
        case CompareOp::Equal:
            return SDL_GPU_COMPAREOP_EQUAL;
        case CompareOp::LessOrEqual:
            return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        case CompareOp::Greater:
            return SDL_GPU_COMPAREOP_GREATER;
        case CompareOp::NotEqual:
            return SDL_GPU_COMPAREOP_NOT_EQUAL;
        case CompareOp::GreaterOrEqual:
            return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
        case CompareOp::Always:
            return SDL_GPU_COMPAREOP_ALWAYS;
        default:
            return SDL_GPU_COMPAREOP_ALWAYS;
    }
}

constexpr SDL_GPUStencilOp toSdl(StencilOp op) noexcept {
    switch (op) {
        case StencilOp::Keep:
            return SDL_GPU_STENCILOP_KEEP;
        case StencilOp::Zero:
            return SDL_GPU_STENCILOP_ZERO;
        case StencilOp::Replace:
            return SDL_GPU_STENCILOP_REPLACE;
        case StencilOp::IncrementAndClamp:
            return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
        case StencilOp::DecrementAndClamp:
            return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
        case StencilOp::Invert:
            return SDL_GPU_STENCILOP_INVERT;
        case StencilOp::IncrementAndWrap:
            return SDL_GPU_STENCILOP_INCREMENT_AND_WRAP;
        case StencilOp::DecrementAndWrap:
            return SDL_GPU_STENCILOP_DECREMENT_AND_WRAP;
        default:
            return SDL_GPU_STENCILOP_KEEP;
    }
}

constexpr SDL_GPUBlendFactor toSdl(BlendFactor factor) noexcept {
    switch (factor) {
        case BlendFactor::Zero:
            return SDL_GPU_BLENDFACTOR_ZERO;
        case BlendFactor::One:
            return SDL_GPU_BLENDFACTOR_ONE;
        case BlendFactor::SrcColor:
            return SDL_GPU_BLENDFACTOR_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor:
            return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::DstColor:
            return SDL_GPU_BLENDFACTOR_DST_COLOR;
        case BlendFactor::OneMinusDstColor:
            return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
        case BlendFactor::SrcAlpha:
            return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha:
            return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha:
            return SDL_GPU_BLENDFACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha:
            return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
        case BlendFactor::ConstantColor:
            return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
        case BlendFactor::OneMinusConstantColor:
            return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
        case BlendFactor::SrcAlphaSaturate:
            return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
        default:
            return SDL_GPU_BLENDFACTOR_ONE;
    }
}

constexpr SDL_GPUBlendOp toSdl(BlendOp op) noexcept {
    switch (op) {
        case BlendOp::Add:
            return SDL_GPU_BLENDOP_ADD;
        case BlendOp::Subtract:
            return SDL_GPU_BLENDOP_SUBTRACT;
        case BlendOp::ReverseSubtract:
            return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
        case BlendOp::Min:
            return SDL_GPU_BLENDOP_MIN;
        case BlendOp::Max:
            return SDL_GPU_BLENDOP_MAX;
        default:
            return SDL_GPU_BLENDOP_ADD;
    }
}

constexpr SDL_GPUSampleCount toSdl(SampleCount count) noexcept {
    switch (count) {
        case SampleCount::One:
            return SDL_GPU_SAMPLECOUNT_1;
        case SampleCount::Two:
            return SDL_GPU_SAMPLECOUNT_2;
        case SampleCount::Four:
            return SDL_GPU_SAMPLECOUNT_4;
        case SampleCount::Eight:
            return SDL_GPU_SAMPLECOUNT_8;
        default:
            return SDL_GPU_SAMPLECOUNT_1;
    }
}

constexpr SDL_GPUFilter toSdl(Filter filter) noexcept {
    switch (filter) {
        case Filter::Nearest:
            return SDL_GPU_FILTER_NEAREST;
        case Filter::Linear:
            return SDL_GPU_FILTER_LINEAR;
        default:
            return SDL_GPU_FILTER_LINEAR;
    }
}

constexpr SDL_GPUSamplerMipmapMode toSdl(MipmapMode mode) noexcept {
    switch (mode) {
        case MipmapMode::Nearest:
            return SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        case MipmapMode::Linear:
            return SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        default:
            return SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    }
}

constexpr SDL_GPUSamplerAddressMode toSdl(AddressMode mode) noexcept {
    switch (mode) {
        case AddressMode::Repeat:
            return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        case AddressMode::MirroredRepeat:
            return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
        case AddressMode::ClampToEdge:
            return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        default:
            return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    }
}

constexpr SDL_GPUShaderStage toSdl(ShaderStage stage) noexcept {
    switch (stage) {
        case ShaderStage::Vertex:
            return SDL_GPU_SHADERSTAGE_VERTEX;
        case ShaderStage::Fragment:
            return SDL_GPU_SHADERSTAGE_FRAGMENT;
        default:
            return SDL_GPU_SHADERSTAGE_VERTEX;
    }
}

constexpr SDL_GPUShaderFormat toSdl(ShaderFormat format) noexcept {
    switch (format) {
        case ShaderFormat::SpirV:
            return SDL_GPU_SHADERFORMAT_SPIRV;
        case ShaderFormat::Dxil:
            return SDL_GPU_SHADERFORMAT_DXIL;
        case ShaderFormat::Msl:
            return SDL_GPU_SHADERFORMAT_MSL;
        default:
            return SDL_GPU_SHADERFORMAT_INVALID;
    }
}

constexpr SDL_GPUPresentMode toSdl(PresentMode mode) noexcept {
    switch (mode) {
        case PresentMode::Vsync:
            return SDL_GPU_PRESENTMODE_VSYNC;
        case PresentMode::Immediate:
            return SDL_GPU_PRESENTMODE_IMMEDIATE;
        case PresentMode::Mailbox:
            return SDL_GPU_PRESENTMODE_MAILBOX;
        default:
            return SDL_GPU_PRESENTMODE_VSYNC;
    }
}

constexpr SDL_GPULoadOp toSdl(LoadOp op) noexcept {
    switch (op) {
        case LoadOp::Load:
            return SDL_GPU_LOADOP_LOAD;
        case LoadOp::Clear:
            return SDL_GPU_LOADOP_CLEAR;
        case LoadOp::DontCare:
            return SDL_GPU_LOADOP_DONT_CARE;
        default:
            return SDL_GPU_LOADOP_DONT_CARE;
    }
}

constexpr SDL_GPUStoreOp toSdl(StoreOp op) noexcept {
    switch (op) {
        case StoreOp::Store:
            return SDL_GPU_STOREOP_STORE;
        case StoreOp::DontCare:
            return SDL_GPU_STOREOP_DONT_CARE;
        default:
            return SDL_GPU_STOREOP_DONT_CARE;
    }
}

constexpr SDL_GPUIndexElementSize toSdl(IndexType type) noexcept {
    switch (type) {
        case IndexType::Uint16:
            return SDL_GPU_INDEXELEMENTSIZE_16BIT;
        case IndexType::Uint32:
            return SDL_GPU_INDEXELEMENTSIZE_32BIT;
        default:
            return SDL_GPU_INDEXELEMENTSIZE_16BIT;
    }
}

// Usage-flag translators (engine bit -> SDL bitmask).
constexpr SDL_GPUTextureUsageFlags toSdl(TextureUsage usage) noexcept {
    SDL_GPUTextureUsageFlags flags = 0;
    if (has(usage, TextureUsage::Sampler)) {
        flags |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    }
    if (has(usage, TextureUsage::ColorTarget)) {
        flags |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    }
    if (has(usage, TextureUsage::DepthStencilTarget)) {
        flags |= SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    }
    return flags;
}

constexpr SDL_GPUBufferUsageFlags toSdl(BufferUsage usage) noexcept {
    SDL_GPUBufferUsageFlags flags = 0;
    if (has(usage, BufferUsage::Vertex)) {
        flags |= SDL_GPU_BUFFERUSAGE_VERTEX;
    }
    if (has(usage, BufferUsage::Index)) {
        flags |= SDL_GPU_BUFFERUSAGE_INDEX;
    }
    return flags;
}

// Accumulate in `unsigned` and narrow once (the fromSdlMod precedent, platform.cpp) — a per-bit `|=`
// on a Uint8 would trip bugprone-narrowing-conversions under --warnings-as-errors.
constexpr SDL_GPUColorComponentFlags toSdl(ColorWriteMask mask) noexcept {
    unsigned bits = 0;
    if (has(mask, ColorWriteMask::R)) {
        bits |= SDL_GPU_COLORCOMPONENT_R;
    }
    if (has(mask, ColorWriteMask::G)) {
        bits |= SDL_GPU_COLORCOMPONENT_G;
    }
    if (has(mask, ColorWriteMask::B)) {
        bits |= SDL_GPU_COLORCOMPONENT_B;
    }
    if (has(mask, ColorWriteMask::A)) {
        bits |= SDL_GPU_COLORCOMPONENT_A;
    }
    return static_cast<SDL_GPUColorComponentFlags>(bits);
}

// --- slot payload types (D3) ----------------------------------------------------------------
// Persistent resources own their SDL object; the destructor releases it (RAII), so destroy*() is
// just pool.remove() and ~Device's teardown is just pool.clear(). Hand-written move ctor/assign
// std::exchange the SDL pointer to null — a defaulted move would double-release in SlotMap's
// std::optional shuffle (plan K-3).

struct BufferSlot {
    SDL_GPUDevice* dev = nullptr;
    SDL_GPUBuffer* buffer = nullptr;
    BufferUsage usage = BufferUsage::None;
    std::uint32_t size = 0;

    BufferSlot(SDL_GPUDevice* deviceIn, SDL_GPUBuffer* bufferIn, BufferUsage usageIn, std::uint32_t sizeIn) noexcept
        : dev(deviceIn), buffer(bufferIn), usage(usageIn), size(sizeIn) {}
    BufferSlot(BufferSlot&& other) noexcept
        : dev(other.dev), buffer(std::exchange(other.buffer, nullptr)), usage(other.usage), size(other.size) {}
    BufferSlot& operator=(BufferSlot&& other) noexcept {
        if (this != &other) {
            release();
            dev = other.dev;
            buffer = std::exchange(other.buffer, nullptr);
            usage = other.usage;
            size = other.size;
        }
        return *this;
    }
    BufferSlot(const BufferSlot&) = delete;
    BufferSlot& operator=(const BufferSlot&) = delete;
    ~BufferSlot() { release(); }

private:
    void release() const noexcept {
        if (buffer != nullptr) {
            SDL_ReleaseGPUBuffer(dev, buffer);
        }
    }
};

struct TextureSlot {
    SDL_GPUDevice* dev = nullptr;
    SDL_GPUTexture* texture = nullptr;
    TextureFormat format = TextureFormat::Invalid;
    TextureUsage usage = TextureUsage::None;
    Extent2D extent{};
    std::uint32_t mipLevels = 1;
    SampleCount sampleCount = SampleCount::One;
    bool swapchainOwned = false;  // swapchain-acquired textures are released by the swapchain, never here

    TextureSlot(SDL_GPUDevice* deviceIn, SDL_GPUTexture* textureIn, TextureFormat formatIn, TextureUsage usageIn,
                Extent2D extentIn, std::uint32_t mipLevelsIn, SampleCount sampleCountIn, bool swapchainOwnedIn) noexcept
        : dev(deviceIn),
          texture(textureIn),
          format(formatIn),
          usage(usageIn),
          extent(extentIn),
          mipLevels(mipLevelsIn),
          sampleCount(sampleCountIn),
          swapchainOwned(swapchainOwnedIn) {}
    TextureSlot(TextureSlot&& other) noexcept
        : dev(other.dev),
          texture(std::exchange(other.texture, nullptr)),
          format(other.format),
          usage(other.usage),
          extent(other.extent),
          mipLevels(other.mipLevels),
          sampleCount(other.sampleCount),
          swapchainOwned(other.swapchainOwned) {}
    TextureSlot& operator=(TextureSlot&& other) noexcept {
        if (this != &other) {
            release();
            dev = other.dev;
            texture = std::exchange(other.texture, nullptr);
            format = other.format;
            usage = other.usage;
            extent = other.extent;
            mipLevels = other.mipLevels;
            sampleCount = other.sampleCount;
            swapchainOwned = other.swapchainOwned;
        }
        return *this;
    }
    TextureSlot(const TextureSlot&) = delete;
    TextureSlot& operator=(const TextureSlot&) = delete;
    ~TextureSlot() { release(); }

private:
    void release() const noexcept {
        if (texture != nullptr && !swapchainOwned) {
            SDL_ReleaseGPUTexture(dev, texture);
        }
    }
};

struct SamplerSlot {
    SDL_GPUDevice* dev = nullptr;
    SDL_GPUSampler* sampler = nullptr;

    SamplerSlot(SDL_GPUDevice* deviceIn, SDL_GPUSampler* samplerIn) noexcept : dev(deviceIn), sampler(samplerIn) {}
    SamplerSlot(SamplerSlot&& other) noexcept : dev(other.dev), sampler(std::exchange(other.sampler, nullptr)) {}
    SamplerSlot& operator=(SamplerSlot&& other) noexcept {
        if (this != &other) {
            release();
            dev = other.dev;
            sampler = std::exchange(other.sampler, nullptr);
        }
        return *this;
    }
    SamplerSlot(const SamplerSlot&) = delete;
    SamplerSlot& operator=(const SamplerSlot&) = delete;
    ~SamplerSlot() { release(); }

private:
    void release() const noexcept {
        if (sampler != nullptr) {
            SDL_ReleaseGPUSampler(dev, sampler);
        }
    }
};

struct ShaderSlot {
    SDL_GPUDevice* dev = nullptr;
    SDL_GPUShader* shader = nullptr;
    ShaderStage stage = ShaderStage::Vertex;

    ShaderSlot(SDL_GPUDevice* deviceIn, SDL_GPUShader* shaderIn, ShaderStage stageIn) noexcept
        : dev(deviceIn), shader(shaderIn), stage(stageIn) {}
    ShaderSlot(ShaderSlot&& other) noexcept
        : dev(other.dev), shader(std::exchange(other.shader, nullptr)), stage(other.stage) {}
    ShaderSlot& operator=(ShaderSlot&& other) noexcept {
        if (this != &other) {
            release();
            dev = other.dev;
            shader = std::exchange(other.shader, nullptr);
            stage = other.stage;
        }
        return *this;
    }
    ShaderSlot(const ShaderSlot&) = delete;
    ShaderSlot& operator=(const ShaderSlot&) = delete;
    ~ShaderSlot() { release(); }

private:
    void release() const noexcept {
        if (shader != nullptr) {
            SDL_ReleaseGPUShader(dev, shader);
        }
    }
};

struct PipelineSlot {
    SDL_GPUDevice* dev = nullptr;
    SDL_GPUGraphicsPipeline* pipeline = nullptr;

    PipelineSlot(SDL_GPUDevice* deviceIn, SDL_GPUGraphicsPipeline* pipelineIn) noexcept
        : dev(deviceIn), pipeline(pipelineIn) {}
    PipelineSlot(PipelineSlot&& other) noexcept : dev(other.dev), pipeline(std::exchange(other.pipeline, nullptr)) {}
    PipelineSlot& operator=(PipelineSlot&& other) noexcept {
        if (this != &other) {
            release();
            dev = other.dev;
            pipeline = std::exchange(other.pipeline, nullptr);
        }
        return *this;
    }
    PipelineSlot(const PipelineSlot&) = delete;
    PipelineSlot& operator=(const PipelineSlot&) = delete;
    ~PipelineSlot() { release(); }

private:
    void release() const noexcept {
        if (pipeline != nullptr) {
            SDL_ReleaseGPUGraphicsPipeline(dev, pipeline);
        }
    }
};

struct SwapchainSlot {
    SDL_GPUDevice* dev = nullptr;
    SDL_Window* window = nullptr;
    TextureFormat format = TextureFormat::Invalid;
    PresentMode mode = PresentMode::Vsync;

    SwapchainSlot(SDL_GPUDevice* deviceIn, SDL_Window* windowIn, TextureFormat formatIn, PresentMode modeIn) noexcept
        : dev(deviceIn), window(windowIn), format(formatIn), mode(modeIn) {}
    SwapchainSlot(SwapchainSlot&& other) noexcept
        : dev(other.dev), window(std::exchange(other.window, nullptr)), format(other.format), mode(other.mode) {}
    SwapchainSlot& operator=(SwapchainSlot&& other) noexcept {
        if (this != &other) {
            release();
            dev = other.dev;
            window = std::exchange(other.window, nullptr);
            format = other.format;
            mode = other.mode;
        }
        return *this;
    }
    SwapchainSlot(const SwapchainSlot&) = delete;
    SwapchainSlot& operator=(const SwapchainSlot&) = delete;
    ~SwapchainSlot() { release(); }

private:
    void release() const noexcept {
        if (window != nullptr) {
            SDL_ReleaseWindowFromGPUDevice(dev, window);
        }
    }
};

// Transients: disposal is CONTEXTUAL (submit/cancel/end), never via a destructor — trivial dtors.
struct AcquiredImage {
    TextureHandle texture;
    SwapchainHandle source;  // which swapchain this image came from (D15's destroy-while-acquired scan)
};

struct CommandBufferSlot {
    SDL_GPUCommandBuffer* cmd = nullptr;
    RenderPassHandle openPass{};
    std::vector<AcquiredImage> acquiredImages;
};

struct PassSlot {
    SDL_GPURenderPass* pass = nullptr;
    CommandBufferHandle owner{};
    bool pipelineBound = false;
};

// File-local process state (D12): a second concurrent create() is rejected in ALL configs.
std::atomic<int> liveDevices{0};

// Engine-owned driver-name constants — backendName() always points at THESE, never SDL's pointer
// (C-9).
constexpr std::string_view BACKEND_NAME_METAL = "metal";
constexpr std::string_view BACKEND_NAME_DIRECT3D12 = "direct3d12";
constexpr std::string_view BACKEND_NAME_VULKAN = "vulkan";

}  // namespace

// --- Device::Impl -------------------------------------------------------------------------------

struct Device::Impl {
    SDL_GPUDevice* device = nullptr;
    ShaderFormat shaderFormat = ShaderFormat::SpirV;
    std::string_view backendName;
    std::thread::id ownerThread;

    SlotMap<BufferSlot, Buffer> buffers;
    SlotMap<TextureSlot, Texture> textures;
    SlotMap<SamplerSlot, Sampler> samplers;
    SlotMap<ShaderSlot, Shader> shaders;
    SlotMap<PipelineSlot, GraphicsPipeline> pipelines;
    SlotMap<SwapchainSlot, Swapchain> swapchains;
    SlotMap<CommandBufferSlot, CommandBuffer> commandBuffers;
    SlotMap<PassSlot, RenderPass> renderPasses;

    // D3/E23: SlotMap has no iteration, so "every live command buffer" needs this minimal side
    // index — pushed at acquireCommandBuffer, erased at submit/cancel.
    std::vector<CommandBufferHandle> liveCommandBuffers;

    void forgetCommandBuffer(CommandBufferHandle handle) noexcept {
        const auto it = std::find(liveCommandBuffers.begin(), liveCommandBuffers.end(), handle);
        if (it != liveCommandBuffers.end()) {
            liveCommandBuffers.erase(it);
        }
    }

    ~Impl();
};

// E23 teardown order: dispose live transients first (force-end open passes, then
// submit-if-swapchain-acquired/cancel-otherwise per D10) -> SDL_WaitForGPUIdle -> WARN-and-clear
// every persistent pool (swapchains -> pipelines -> shaders -> samplers -> textures -> buffers) ->
// SDL_DestroyGPUDevice -> liveDevices.fetch_sub. Order exists so the wait actually covers the
// forced submits and no release races an in-flight buffer (D4 makes racing safe anyway; this makes
// it quiet).
Device::Impl::~Impl() {
    assert(std::this_thread::get_id() == ownerThread && "~Device (Impl teardown) off the owning thread");

    const std::vector<CommandBufferHandle> leaked = liveCommandBuffers;
    for (const CommandBufferHandle cmdHandle : leaked) {
        CommandBufferSlot* cmdSlot = commandBuffers.get(cmdHandle);
        assert(cmdSlot != nullptr && "rhi: liveCommandBuffers references a handle missing from the pool");
        if (cmdSlot == nullptr) {
            continue;  // internal invariant violated — skip defensively rather than crash in Release
        }
        if (cmdSlot->openPass.valid()) {
            PassSlot* passSlot = renderPasses.get(cmdSlot->openPass);
            if (passSlot != nullptr) {
                AERO_LOG_ERROR("rhi: ~Device force-ending an open render pass on a leaked command buffer");
                SDL_EndGPURenderPass(passSlot->pass);
                renderPasses.remove(cmdSlot->openPass);
            }
        }
        const bool swapchainAcquired = !cmdSlot->acquiredImages.empty();
        AERO_LOG_ERROR("rhi: ~Device disposing a leaked, un-submitted command buffer ({})",
                       swapchainAcquired ? "submitting (D10)" : "cancelling");
        if (swapchainAcquired) {
            SDL_SubmitGPUCommandBuffer(cmdSlot->cmd);
        } else {
            SDL_CancelGPUCommandBuffer(cmdSlot->cmd);
        }
        for (const AcquiredImage& acquired : cmdSlot->acquiredImages) {
            textures.remove(acquired.texture);
        }
        commandBuffers.remove(cmdHandle);
    }
    liveCommandBuffers.clear();

    SDL_WaitForGPUIdle(device);

    if (swapchains.size() > 0) {
        AERO_LOG_WARN("rhi: ~Device releasing {} leaked swapchain(s)", swapchains.size());
    }
    swapchains.clear();
    if (pipelines.size() > 0) {
        AERO_LOG_WARN("rhi: ~Device releasing {} leaked graphics pipeline(s)", pipelines.size());
    }
    pipelines.clear();
    if (shaders.size() > 0) {
        AERO_LOG_WARN("rhi: ~Device releasing {} leaked shader(s)", shaders.size());
    }
    shaders.clear();
    if (samplers.size() > 0) {
        AERO_LOG_WARN("rhi: ~Device releasing {} leaked sampler(s)", samplers.size());
    }
    samplers.clear();
    if (textures.size() > 0) {
        AERO_LOG_WARN("rhi: ~Device releasing {} leaked texture(s)", textures.size());
    }
    textures.clear();
    if (buffers.size() > 0) {
        AERO_LOG_WARN("rhi: ~Device releasing {} leaked buffer(s)", buffers.size());
    }
    buffers.clear();

    SDL_DestroyGPUDevice(device);
    liveDevices.fetch_sub(1, std::memory_order_relaxed);
}

// --- special members (C-2: the AudioDevice/Window precedent — teardown lives in ~Impl, never
// ~Device, because unique_ptr's move-assign destroys an overwritten Impl WITHOUT running an outer
// destructor) ------------------------------------------------------------------------------------

Device::Device(std::unique_ptr<Impl> impl) noexcept : impl(std::move(impl)) {}
Device::~Device() = default;
Device::Device(Device&&) noexcept = default;
Device& Device::operator=(Device&&) noexcept = default;

// --- create --------------------------------------------------------------------------------------

std::optional<Device> Device::create(const DeviceDesc& desc) {
    AERO_PROFILE_ZONE_NAMED("rhi::Device::create");

    if (liveDevices.fetch_add(1, std::memory_order_relaxed) != 0) {
        liveDevices.fetch_sub(1, std::memory_order_relaxed);
        AERO_LOG_ERROR("rhi: Device::create: a Device already lives in this process (one-per-process, D12)");
        return std::nullopt;
    }

    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, desc.enableDebugLayer);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_PREFERLOWPOWER_BOOLEAN, desc.preferLowPower);
    // D18: verbose logging follows the debug-layer flag — keeps Release/exported-runtime logs quiet.
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VERBOSE_BOOLEAN, desc.enableDebugLayer);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
    if (desc.driver != DriverPreference::Auto) {
        const char* name = nullptr;
        switch (desc.driver) {
            case DriverPreference::Vulkan:
                name = "vulkan";
                break;
            case DriverPreference::Metal:
                name = "metal";
                break;
            case DriverPreference::Direct3D12:
                name = "direct3d12";
                break;
            case DriverPreference::Auto:
                break;  // unreachable — guarded by the enclosing `if`
        }
        if (name != nullptr) {
            SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, name);
        }
    }

    SDL_GPUDevice* const sdlDevice = SDL_CreateGPUDeviceWithProperties(props);
    SDL_DestroyProperties(props);

    if (sdlDevice == nullptr) {
        AERO_LOG_ERROR("rhi: Device::create: SDL_CreateGPUDeviceWithProperties failed: {}", SDL_GetError());
        liveDevices.fetch_sub(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    const char* const driverName = SDL_GetGPUDeviceDriver(sdlDevice);
    const std::string_view driverNameView = driverName != nullptr ? std::string_view{driverName} : std::string_view{};

    std::string_view backendName;
    ShaderFormat shaderFormat = ShaderFormat::SpirV;
    if (driverNameView == BACKEND_NAME_METAL) {
        backendName = BACKEND_NAME_METAL;
        shaderFormat = ShaderFormat::Msl;
    } else if (driverNameView == BACKEND_NAME_DIRECT3D12) {
        backendName = BACKEND_NAME_DIRECT3D12;
        shaderFormat = ShaderFormat::Dxil;
    } else if (driverNameView == BACKEND_NAME_VULKAN) {
        backendName = BACKEND_NAME_VULKAN;
        shaderFormat = ShaderFormat::SpirV;
    } else {
        // C-9: unreachable on the pinned desktop SDL — defensive only.
        AERO_LOG_ERROR("rhi: Device::create: unknown backend driver '{}'", driverNameView);
        SDL_DestroyGPUDevice(sdlDevice);
        liveDevices.fetch_sub(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    // E19: the chosen format's bit must be among what the device actually accepts.
    assert((SDL_GetGPUShaderFormats(sdlDevice) & toSdl(shaderFormat)) != 0 &&
           "rhi: the chosen shader format is not among SDL_GetGPUShaderFormats' bits (E19)");

    auto impl = std::make_unique<Impl>();
    impl->device = sdlDevice;
    impl->shaderFormat = shaderFormat;
    impl->backendName = backendName;
    impl->ownerThread = std::this_thread::get_id();

    AERO_LOG_INFO("rhi: Device created (backend={}, shaderFormat={})", backendName, static_cast<int>(shaderFormat));

    return Device{std::move(impl)};
}

// --- identity & capability queries ----------------------------------------------------------------

ShaderFormat Device::shaderFormat() const noexcept {
    if (impl == nullptr) {
        AERO_LOG_ERROR("rhi: Device::shaderFormat: called on a moved-from Device");
        return ShaderFormat::SpirV;
    }
    assert(std::this_thread::get_id() == impl->ownerThread && "Device::shaderFormat off the owning thread");
    return impl->shaderFormat;
}

std::string_view Device::backendName() const noexcept {
    if (impl == nullptr) {
        AERO_LOG_ERROR("rhi: Device::backendName: called on a moved-from Device");
        return {};
    }
    assert(std::this_thread::get_id() == impl->ownerThread && "Device::backendName off the owning thread");
    return impl->backendName;
}

bool Device::supportsTextureFormat(TextureFormat format, TextureUsage usage) const {
    if (impl == nullptr) {
        AERO_LOG_ERROR("rhi: Device::supportsTextureFormat: called on a moved-from Device");
        return false;
    }
    assert(std::this_thread::get_id() == impl->ownerThread && "Device::supportsTextureFormat off the owning thread");
    if (format == TextureFormat::Invalid || format == TextureFormat::Count) {
        return false;
    }
    return SDL_GPUTextureSupportsFormat(impl->device, toSdl(format), SDL_GPU_TEXTURETYPE_2D, toSdl(usage));
}

bool Device::waitIdle() {
    AERO_PROFILE_ZONE_NAMED("rhi::Device::waitIdle");
    if (impl == nullptr) {
        AERO_LOG_ERROR("rhi: Device::waitIdle: called on a moved-from Device");
        return false;
    }
    assert(std::this_thread::get_id() == impl->ownerThread && "Device::waitIdle off the owning thread");
    return SDL_WaitForGPUIdle(impl->device);
}

}  // namespace engine::rhi

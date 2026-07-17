#pragma once
// Aero Engine — rhi shared vocabulary types (task 0.4.1): the state enums, the usage-flag enums with
// their bit operators, and the small POD structs the descriptors and Device API share. Everything is
// engine-owned; the backend (0.4.2) maps each value 1:1. Defaults across this module encode the
// engine-wide conventions pinned by ADR-005 at task 0.2.2: right-handed/Y-up/-Z-forward worlds,
// counter-clockwise front faces (glTF winding), and clip-space depth in [0,1] with 0 = near.

#include <aero/rhi/handles.hpp>

#include <cstdint>

namespace engine::rhi {

// --- frame & pass enums ------------------------------------------------------------------------

// Presentation timing. Vsync is ALWAYS supported; the other two must be checked per swapchain via
// Device::supportsPresentMode before Device::setPresentMode (backend contract).
enum class PresentMode : std::uint8_t { Vsync, Immediate, Mailbox };

// What happens to an attachment's contents when a render pass begins.
enum class LoadOp : std::uint8_t { Load, Clear, DontCare };

// What happens to an attachment's contents when a render pass ends. (MSAA resolve store ops are a
// future append alongside resolve attachments.)
enum class StoreOp : std::uint8_t { Store, DontCare };

enum class IndexType : std::uint8_t { Uint16, Uint32 };

// --- pipeline state enums ----------------------------------------------------------------------

enum class PrimitiveType : std::uint8_t { TriangleList, TriangleStrip, LineList, LineStrip, PointList };
enum class FillMode : std::uint8_t { Fill, Line };
enum class CullMode : std::uint8_t { None, Front, Back };
// CounterClockwise is the engine default: RH/glTF winding (ADR-005).
enum class FrontFace : std::uint8_t { CounterClockwise, Clockwise };
// Depth 0 = near (ADR-005) => Less means "closer wins".
enum class CompareOp : std::uint8_t { Never, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always };
enum class StencilOp : std::uint8_t {
    Keep,
    Zero,
    Replace,
    IncrementAndClamp,
    DecrementAndClamp,
    Invert,
    IncrementAndWrap,
    DecrementAndWrap
};
enum class BlendFactor : std::uint8_t {
    Zero,
    One,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    ConstantColor,
    OneMinusConstantColor,
    SrcAlphaSaturate
};
enum class BlendOp : std::uint8_t { Add, Subtract, ReverseSubtract, Min, Max };
// MSAA sample counts for render-target textures and pipelines. Support beyond One is
// hardware-dependent (query path arrives with the MSAA work; v0 renderers use One).
enum class SampleCount : std::uint8_t { One, Two, Four, Eight };

// --- vertex input enums ------------------------------------------------------------------------

enum class VertexFormat : std::uint8_t {
    Float,
    Float2,
    Float3,
    Float4,
    Int,
    Int2,
    Int3,
    Int4,
    Uint,
    Uint2,
    Uint3,
    Uint4,
    Byte4Norm,
    UByte4Norm,
    Half2,
    Half4,
};
enum class VertexInputRate : std::uint8_t { Vertex, Instance };

// --- sampler enums -----------------------------------------------------------------------------

enum class Filter : std::uint8_t { Nearest, Linear };
enum class MipmapMode : std::uint8_t { Nearest, Linear };
enum class AddressMode : std::uint8_t { Repeat, MirroredRepeat, ClampToEdge };

// --- shader enums ------------------------------------------------------------------------------

enum class ShaderStage : std::uint8_t { Vertex, Fragment };

// The bytecode dialects the engine's shader pipeline produces (ADR-002: HLSL -> SDL_shadercross ->
// all three, offline, task 0.4.3). A created Device consumes EXACTLY ONE — Device::shaderFormat();
// shader loading (0.4.4) picks the matching cooked artifact.
enum class ShaderFormat : std::uint8_t { SpirV, Dxil, Msl };

// The backend driver preference — a dev/debug knob for DeviceDesc; Auto lets the backend pick the
// platform's native API (Metal on macOS, D3D12 on Windows, Vulkan on Linux).
enum class DriverPreference : std::uint8_t { Auto, Vulkan, Metal, Direct3D12 };

// --- usage flags (D17: enum class + explicit constexpr operators) -------------------------------

// A texture must declare at least one usage. Storage usages are a future append (compute epic).
enum class TextureUsage : std::uint8_t {
    None = 0,
    Sampler = 1U << 0U,             // shaders may sample it (bindFragmentSamplers)
    ColorTarget = 1U << 1U,         // render passes may render color into it
    DepthStencilTarget = 1U << 2U,  // render passes may use it as the depth/stencil attachment
};
[[nodiscard]] constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) noexcept {
    return static_cast<TextureUsage>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr TextureUsage operator&(TextureUsage a, TextureUsage b) noexcept {
    return static_cast<TextureUsage>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr TextureUsage operator~(TextureUsage a) noexcept {
    return static_cast<TextureUsage>(static_cast<std::uint8_t>(~static_cast<std::uint8_t>(a)) & 0x07U);
}
[[nodiscard]] constexpr bool has(TextureUsage flags, TextureUsage bit) noexcept {
    return (flags & bit) != TextureUsage::None;
}

// A buffer must declare at least one usage. Indirect/storage usages are future appends.
enum class BufferUsage : std::uint8_t {
    None = 0,
    Vertex = 1U << 0U,
    Index = 1U << 1U,
};
[[nodiscard]] constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) noexcept {
    return static_cast<BufferUsage>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr BufferUsage operator&(BufferUsage a, BufferUsage b) noexcept {
    return static_cast<BufferUsage>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr BufferUsage operator~(BufferUsage a) noexcept {
    return static_cast<BufferUsage>(static_cast<std::uint8_t>(~static_cast<std::uint8_t>(a)) & 0x03U);
}
[[nodiscard]] constexpr bool has(BufferUsage flags, BufferUsage bit) noexcept {
    return (flags & bit) != BufferUsage::None;
}

// Which color channels a pipeline writes. All is the default everywhere.
enum class ColorWriteMask : std::uint8_t {
    None = 0,
    R = 1U << 0U,
    G = 1U << 1U,
    B = 1U << 2U,
    A = 1U << 3U,
    All = R | G | B | A,
};
[[nodiscard]] constexpr ColorWriteMask operator|(ColorWriteMask a, ColorWriteMask b) noexcept {
    return static_cast<ColorWriteMask>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr ColorWriteMask operator&(ColorWriteMask a, ColorWriteMask b) noexcept {
    return static_cast<ColorWriteMask>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr ColorWriteMask operator~(ColorWriteMask a) noexcept {
    return static_cast<ColorWriteMask>(static_cast<std::uint8_t>(~static_cast<std::uint8_t>(a)) & 0x0FU);
}
[[nodiscard]] constexpr bool has(ColorWriteMask flags, ColorWriteMask bit) noexcept {
    return (flags & bit) != ColorWriteMask::None;
}

// --- backend limits (task 0.4.2; verified against the pinned SDL_GPU's internal caps) -----------
// Engine-owned constants; the backend validates against THESE, so exceeding one is a logged
// creation/record failure, never backend UB. Values verified at SDL 3.4.12 src/gpu/SDL_sysgpu.h
// (MAX_COLOR_TARGET_BINDINGS / MAX_VERTEX_BUFFERS / MAX_VERTEX_ATTRIBUTES /
// MAX_UNIFORM_BUFFERS_PER_STAGE / MAX_TEXTURE_SAMPLERS_PER_STAGE).
inline constexpr std::uint32_t MAX_COLOR_ATTACHMENTS = 8;
inline constexpr std::uint32_t MAX_VERTEX_BUFFER_SLOTS = 16;
inline constexpr std::uint32_t MAX_VERTEX_ATTRIBUTES = 16;
inline constexpr std::uint32_t MAX_PUSH_UNIFORM_SLOTS = 4;
inline constexpr std::uint32_t MAX_FRAGMENT_SAMPLER_SLOTS = 16;

// --- POD value types ----------------------------------------------------------------------------

// A size in PIXELS (texel space). The swapchain's Extent2D comes from acquisition and is the
// authoritative render size during resizes — not Window::pixelSize() (see the acquire contract).
struct Extent2D {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    constexpr bool operator==(const Extent2D&) const noexcept = default;
};

// Integer pixel rectangle (scissor).
struct Rect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    constexpr bool operator==(const Rect&) const noexcept = default;
};

// Depth range [0,1], 0 = near — ADR-005's pinned NDC. Do not "fix" it per backend; the backend
// already normalizes (never proj[1][1] *= -1).
struct Viewport {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float minDepth = 0.0F;
    float maxDepth = 1.0F;
    constexpr bool operator==(const Viewport&) const noexcept = default;
};

// Normalized RGBA, non-premultiplied, in the target's encoding. Default opaque black.
struct Color {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 1.0F;
    constexpr bool operator==(const Color&) const noexcept = default;
};

// One combined texture+sampler bind (Device::bindFragmentSamplers). The texture must have been
// created with TextureUsage::Sampler; swapchain-acquired textures are write-only and REJECTED.
struct TextureSamplerBinding {
    TextureHandle texture;
    SamplerHandle sampler;
    constexpr bool operator==(const TextureSamplerBinding&) const noexcept = default;
};

}  // namespace engine::rhi

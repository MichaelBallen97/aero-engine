#pragma once
// Aero Engine — rhi texture formats (task 0.4.1). A curated subset of the backend's formats: every
// non-depth value below is on SDL_GPU's UNIVERSALLY-SUPPORTED lists for its intended usage, so code
// targeting this enum works on Vulkan, Metal, and D3D12 without a support query — with ONE
// exception the API is honest about: of the >16-bit depth formats, drivers guarantee EITHER D24 or
// D32F (and either D24S8 or D32FS8), NOT both. Anything creating a depth texture beyond D16 must
// pick via Device::supportsTextureFormat (see E7 in the 0.4.1 spec).
//
// Grows append-only: BCn/ASTC compressed formats arrive with the cooker (Phase 2); integer formats
// when a consumer names them. Values are the engine's own — the backend maps them; nothing here
// implies SDL numbering.

#include <cstdint>
#include <string_view>

namespace engine::rhi {

enum class TextureFormat : std::uint8_t {
    Invalid = 0,

    // 8-bit unorm color (universal for sampling & color targets)
    R8Unorm,
    RG8Unorm,
    RGBA8Unorm,
    RGBA8UnormSrgb,
    BGRA8Unorm,      // common swapchain format
    BGRA8UnormSrgb,  // common swapchain format (SDR_LINEAR composition; future)
    // float color (universal)
    R16Float,
    RG16Float,
    RGBA16Float,
    R32Float,
    RG32Float,
    RGBA32Float,
    R11G11B10Ufloat,  // packed HDR render-target format
    // depth / depth-stencil
    D16Unorm,        // the only UNIVERSAL depth format
    D24Unorm,        // either this or D32Float is supported — query!
    D32Float,        // either this or D24Unorm is supported — query!
    D24UnormS8Uint,  // either this or D32FloatS8Uint is supported — query!
    D32FloatS8Uint,  // either this or D24UnormS8Uint is supported — query!

    Count,  // sentinel for iteration/tests; never a real format
};

// True for the five D* formats.
[[nodiscard]] bool isDepthFormat(TextureFormat format) noexcept;

// True only for the two *S8Uint formats.
[[nodiscard]] bool hasStencilComponent(TextureFormat format) noexcept;

// True only for the two *Srgb formats.
[[nodiscard]] bool isSrgbFormat(TextureFormat format) noexcept;

// Bytes per texel for CPU-uploadable (color) formats — what Device::uploadTexture validates data
// sizes against. Returns 0 for Invalid, Count, and ALL depth formats: depth textures are GPU-written
// render targets in v0, never uploaded, and their in-memory layout is driver business.
[[nodiscard]] std::uint32_t texelBlockSize(TextureFormat format) noexcept;

// Stable name for logs/tests ("RGBA8Unorm", ...). Total over the enum; "Invalid" only for Invalid.
[[nodiscard]] std::string_view toString(TextureFormat format) noexcept;

}  // namespace engine::rhi

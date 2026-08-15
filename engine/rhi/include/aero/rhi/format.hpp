#pragma once
// Aero Engine — rhi texture formats (task 0.4.1). A curated subset of the backend's formats: every
// non-depth value below is on SDL_GPU's UNIVERSALLY-SUPPORTED lists for its intended usage, so code
// targeting this enum works on Vulkan, Metal, and D3D12 without a support query — with TWO
// exceptions the API is honest about. First: of the >16-bit depth formats, drivers guarantee EITHER
// D24 or D32F (and either D24S8 or D32FS8), NOT both. Anything creating a depth texture beyond D16
// must pick via Device::supportsTextureFormat (see E7 in the 0.4.1 spec). Second (task 3.4.1): the
// six BC* formats are universal on DESKTOP in practice but are not on SDL_GPU's universal SAMPLER
// list and are absent on most mobile GPUs — see the note at their declaration below.
//
// Grows append-only: BCn landed with the first cooked-texture consumer (task 3.4.1); ASTC arrives
// with the mobile texture profile (task 6.3.1); integer formats when a consumer names them. Values
// are the engine's own — the backend maps them; nothing here implies SDL numbering.

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
    // block-compressed color (task 3.4.1) — 4x4 texel blocks, 8 or 16 bytes per block. DESKTOP
    // SCOPE: sampler support is a hardware fact on the three desktop backends (D3D12 requires
    // every BC format at all feature levels; desktop Vulkan implementations expose
    // textureCompressionBC; Metal on macOS supports BC on all Macs), but these six are NOT on
    // SDL_GPU's universally-supported SAMPLER list (verified against the pinned SDL 3.4.12
    // SDL_gpu.h, whose list is seventeen uncompressed formats and no BC format) and are absent on
    // most mobile GPUs — the query is Device::supportsTextureFormat, and the mobile texture
    // profile (ASTC/ETC2 + the cooker's --platform flag) arrives with task 6.3.1. There is
    // deliberately no BC4/BC5 sRGB variant: Vulkan defines neither, SDL defines neither, and the
    // absence is what keeps "an sRGB normal map" unspellable end to end (docs/09 section 10.7).
    BC1RGBAUnorm,      // 8-byte blocks; uploads CookedTextureFormat::Bc1RgbUnorm (opaque blocks)
    BC1RGBAUnormSrgb,  // 8-byte blocks; uploads Bc1RgbSrgb
    BC3RGBAUnorm,      // 16-byte blocks; uploads Bc3Unorm
    BC3RGBAUnormSrgb,  // 16-byte blocks; uploads Bc3Srgb
    BC4RUnorm,         // 8-byte blocks; uploads Bc4Unorm
    BC5RGUnorm,        // 16-byte blocks; uploads Bc5Unorm

    Count,  // sentinel for iteration/tests; never a real format
};

// True for the five D* formats.
[[nodiscard]] bool isDepthFormat(TextureFormat format) noexcept;

// True only for the two *S8Uint formats.
[[nodiscard]] bool hasStencilComponent(TextureFormat format) noexcept;

// True only for the four *Srgb formats.
[[nodiscard]] bool isSrgbFormat(TextureFormat format) noexcept;

// Bytes per texel BLOCK — the unit Vulkan's own vocabulary uses: every uncompressed color format
// is a 1x1-block format (value unchanged, bit for bit, for every pre-3.4.1 enumerator), and the
// BC* formats are 4x4-block formats returning 8 (BC1/BC4) or 16 (BC3/BC5). What
// Device::uploadTexture validates data sizes against, via textureLevelByteSize below. Returns 0
// for Invalid, Count, and ALL depth formats: depth textures are GPU-written render targets in v0,
// never uploaded, and their in-memory layout is driver business.
[[nodiscard]] std::uint32_t texelBlockSize(TextureFormat format) noexcept;

// Texel-block extent. 1x1 for every uncompressed color format, 4x4 for the six BC* formats, 0 for
// Invalid/Count/depth. Two functions rather than one blockExtent, deliberately (the
// cookedTextureBlockWidth/Height rationale, cooked_texture.hpp): ASTC (task 6.3.1) has non-square
// blocks, and a caller that assumed one call answered both questions would be silently wrong for
// every one of them.
[[nodiscard]] std::uint32_t texelBlockWidth(TextureFormat format) noexcept;
[[nodiscard]] std::uint32_t texelBlockHeight(TextureFormat format) noexcept;

// THE upload-size formula — the only place upload arithmetic lives (both uploadTexture's
// validation and render's cooked-texture bridge agree with it by construction):
//   texelBlockSize(f) * ceil(width / blockWidth) * ceil(height / blockHeight)
// and 0 whenever texelBlockSize(f) is 0. For every 1x1-block format this equals the old
// bytes-per-texel * width * height exactly; for BC formats it is the docs/09 section 10 level
// arithmetic (a 4x4 BC1 level is 8 bytes; the 2x2 and 1x1 mip tail is ONE 8- or 16-byte block).
[[nodiscard]] std::uint64_t textureLevelByteSize(TextureFormat format, std::uint32_t width,
                                                 std::uint32_t height) noexcept;

// Stable name for logs/tests ("RGBA8Unorm", ...). Total over the enum; "Invalid" only for Invalid.
[[nodiscard]] std::string_view toString(TextureFormat format) noexcept;

}  // namespace engine::rhi

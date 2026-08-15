// Aero Engine — rhi format utilities (task 0.4.1). Pure classification/size functions — the only
// rhi code that EXISTS before the backend lands (0.4.2). Every switch is total over the enum
// (default: only for Invalid/Count) so a new format value fails loudly in review and in the
// exhaustive unit tests, not silently at runtime.

#include <aero/rhi/format.hpp>

namespace engine::rhi {

bool isDepthFormat(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::D16Unorm:
        case TextureFormat::D24Unorm:
        case TextureFormat::D32Float:
        case TextureFormat::D24UnormS8Uint:
        case TextureFormat::D32FloatS8Uint:
            return true;
        default:
            return false;
    }
}

bool hasStencilComponent(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::D24UnormS8Uint:
        case TextureFormat::D32FloatS8Uint:
            return true;
        default:
            return false;
    }
}

bool isSrgbFormat(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::RGBA8UnormSrgb:
        case TextureFormat::BGRA8UnormSrgb:
        case TextureFormat::BC1RGBAUnormSrgb:
        case TextureFormat::BC3RGBAUnormSrgb:
            return true;
        default:
            return false;
    }
}

std::uint32_t texelBlockSize(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::R8Unorm:
            return 1;
        case TextureFormat::RG8Unorm:
            return 2;
        case TextureFormat::RGBA8Unorm:
        case TextureFormat::RGBA8UnormSrgb:
        case TextureFormat::BGRA8Unorm:
        case TextureFormat::BGRA8UnormSrgb:
            return 4;
        case TextureFormat::R16Float:
            return 2;
        case TextureFormat::RG16Float:
            return 4;
        case TextureFormat::RGBA16Float:
            return 8;
        case TextureFormat::R32Float:
            return 4;
        case TextureFormat::RG32Float:
            return 8;
        case TextureFormat::RGBA32Float:
            return 16;
        case TextureFormat::R11G11B10Ufloat:
            return 4;
        // Block-compressed (task 3.4.1): the value is bytes per 4x4 BLOCK, not per texel. Every arm
        // above is a 1x1-block format, so its number is unchanged bit for bit by that re-reading.
        case TextureFormat::BC1RGBAUnorm:
        case TextureFormat::BC1RGBAUnormSrgb:
        case TextureFormat::BC4RUnorm:
            return 8;
        case TextureFormat::BC3RGBAUnorm:
        case TextureFormat::BC3RGBAUnormSrgb:
        case TextureFormat::BC5RGUnorm:
            return 16;
        default:
            return 0;  // Invalid, Count, and ALL depth formats: not CPU-uploadable (D14)
    }
}

std::uint32_t texelBlockWidth(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::BC1RGBAUnorm:
        case TextureFormat::BC1RGBAUnormSrgb:
        case TextureFormat::BC3RGBAUnorm:
        case TextureFormat::BC3RGBAUnormSrgb:
        case TextureFormat::BC4RUnorm:
        case TextureFormat::BC5RGUnorm:
            return 4;
        default:
            // 1 for every uncompressed COLOR format; 0 for Invalid/Count/depth, matching
            // texelBlockSize's "not CPU-uploadable" answer so the two are never contradictory.
            return texelBlockSize(format) == 0 ? 0U : 1U;
    }
}

// Identical body to texelBlockWidth — 4x4 blocks throughout today. The two functions exist
// separately because ASTC (task 6.3.1) has non-square blocks; do NOT merge them into one
// blockExtent, or every ASTC caller inherits a silently wrong answer on one axis.
std::uint32_t texelBlockHeight(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::BC1RGBAUnorm:
        case TextureFormat::BC1RGBAUnormSrgb:
        case TextureFormat::BC3RGBAUnorm:
        case TextureFormat::BC3RGBAUnormSrgb:
        case TextureFormat::BC4RUnorm:
        case TextureFormat::BC5RGUnorm:
            return 4;
        default:
            return texelBlockSize(format) == 0 ? 0U : 1U;
    }
}

std::uint64_t textureLevelByteSize(TextureFormat format, std::uint32_t width, std::uint32_t height) noexcept {
    const std::uint64_t blockBytes = texelBlockSize(format);
    if (blockBytes == 0) {
        return 0;  // Invalid, Count, depth — nothing is uploadable, so no size exists
    }
    // Safe by construction: both extents are non-zero exactly when blockBytes is (the guard above
    // returned already), so neither division below can divide by zero.
    const std::uint64_t blockW = texelBlockWidth(format);
    const std::uint64_t blockH = texelBlockHeight(format);
    const std::uint64_t blocksX = (std::uint64_t{width} + blockW - 1) / blockW;
    const std::uint64_t blocksY = (std::uint64_t{height} + blockH - 1) / blockH;
    return blockBytes * blocksX * blocksY;  // width/height 0 yield 0, as the old formula did
}

std::string_view toString(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::R8Unorm:
            return "R8Unorm";
        case TextureFormat::RG8Unorm:
            return "RG8Unorm";
        case TextureFormat::RGBA8Unorm:
            return "RGBA8Unorm";
        case TextureFormat::RGBA8UnormSrgb:
            return "RGBA8UnormSrgb";
        case TextureFormat::BGRA8Unorm:
            return "BGRA8Unorm";
        case TextureFormat::BGRA8UnormSrgb:
            return "BGRA8UnormSrgb";
        case TextureFormat::R16Float:
            return "R16Float";
        case TextureFormat::RG16Float:
            return "RG16Float";
        case TextureFormat::RGBA16Float:
            return "RGBA16Float";
        case TextureFormat::R32Float:
            return "R32Float";
        case TextureFormat::RG32Float:
            return "RG32Float";
        case TextureFormat::RGBA32Float:
            return "RGBA32Float";
        case TextureFormat::R11G11B10Ufloat:
            return "R11G11B10Ufloat";
        case TextureFormat::D16Unorm:
            return "D16Unorm";
        case TextureFormat::D24Unorm:
            return "D24Unorm";
        case TextureFormat::D32Float:
            return "D32Float";
        case TextureFormat::D24UnormS8Uint:
            return "D24UnormS8Uint";
        case TextureFormat::D32FloatS8Uint:
            return "D32FloatS8Uint";
        case TextureFormat::BC1RGBAUnorm:
            return "BC1RGBAUnorm";
        case TextureFormat::BC1RGBAUnormSrgb:
            return "BC1RGBAUnormSrgb";
        case TextureFormat::BC3RGBAUnorm:
            return "BC3RGBAUnorm";
        case TextureFormat::BC3RGBAUnormSrgb:
            return "BC3RGBAUnormSrgb";
        case TextureFormat::BC4RUnorm:
            return "BC4RUnorm";
        case TextureFormat::BC5RGUnorm:
            return "BC5RGUnorm";
        default:
            return "Invalid";  // covers Invalid, Count, and out-of-range casts
    }
}

}  // namespace engine::rhi

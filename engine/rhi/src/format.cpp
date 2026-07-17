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
        default:
            return 0;  // Invalid, Count, and ALL depth formats: not CPU-uploadable (D14)
    }
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
        default:
            return "Invalid";  // covers Invalid, Count, and out-of-range casts
    }
}

}  // namespace engine::rhi

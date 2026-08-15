// engine/render/src/texture_upload.cpp — task 3.4.1: the cooked-texture → GPU bridge. Refuse first,
// create second, upload third; on any failure the partial texture is destroyed and an invalid handle
// comes back. Nothing logs on the happy path (docs/04).

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/render/texture_upload.hpp>
#include <aero/rhi/device.hpp>
#include <aero/rhi/types.hpp>

#include <cstdint>

namespace engine::render {

rhi::TextureFormat cookedTextureToRhiFormat(assets::CookedTextureFormat format) noexcept {
    switch (format) {
        case assets::CookedTextureFormat::Rgba8Unorm:
            return rhi::TextureFormat::RGBA8Unorm;
        case assets::CookedTextureFormat::Rgba8Srgb:
            return rhi::TextureFormat::RGBA8UnormSrgb;
        case assets::CookedTextureFormat::Bc1RgbUnorm:
            return rhi::TextureFormat::BC1RGBAUnorm;
        case assets::CookedTextureFormat::Bc1RgbSrgb:
            return rhi::TextureFormat::BC1RGBAUnormSrgb;
        case assets::CookedTextureFormat::Bc3Unorm:
            return rhi::TextureFormat::BC3RGBAUnorm;
        case assets::CookedTextureFormat::Bc3Srgb:
            return rhi::TextureFormat::BC3RGBAUnormSrgb;
        case assets::CookedTextureFormat::Bc4Unorm:
            return rhi::TextureFormat::BC4RUnorm;
        case assets::CookedTextureFormat::Bc5Unorm:
            return rhi::TextureFormat::BC5RGUnorm;
    }
    // NO `default:` above, deliberately. This line is reachable only by casting an unlisted u32 into
    // the enum (legal — it has a fixed underlying type), which cannot happen through a
    // CookedTextureView: only parseCookedTexture constructs one, and its step 5 refuses any value
    // outside the eight. Stating the reachability argument beats adding a dead runtime arm.
    return rhi::TextureFormat::Invalid;
}

rhi::TextureHandle createTextureFromCookedTexture(rhi::Device& device, const assets::CookedTextureView& view) {
    AERO_PROFILE_ZONE;
    const rhi::TextureFormat format = cookedTextureToRhiFormat(view.format());
    const std::uint32_t width = view.width();
    const std::uint32_t height = view.height();

    // The SAME predicate the backend's validateDesc enforces, checked here FIRST so the one message a
    // caller sees names the artifact rather than the desc. Two sites, one rule: the backend's is the
    // enforcement (every creation path inherits it), this one is the diagnosis.
    const std::uint32_t blockW = rhi::texelBlockWidth(format);
    const std::uint32_t blockH = rhi::texelBlockHeight(format);
    if (blockW == 0 || blockH == 0) {
        AERO_LOG_ERROR("render: createTextureFromCookedTexture: cooked format {} has no rhi mapping",
                       assets::toString(view.format()));
        return {};
    }
    if (width % blockW != 0 || height % blockH != 0) {
        AERO_LOG_ERROR(
            "render: createTextureFromCookedTexture: cooked top level {}x{} is not {}x{}-aligned for {}; "
            "block-compressed textures require an aligned top level on every backend — cook as rgba8 "
            "or resize the source",
            width, height, blockW, blockH, assets::toString(view.format()));
        return {};
    }
    if (width > rhi::MAX_TEXTURE_DIMENSION_2D || height > rhi::MAX_TEXTURE_DIMENSION_2D) {
        AERO_LOG_ERROR("render: createTextureFromCookedTexture: {}x{} exceeds the maximum 2D texture dimension ({})",
                       width, height, rhi::MAX_TEXTURE_DIMENSION_2D);
        return {};
    }

    const rhi::TextureHandle texture = device.createTexture({.format = format,
                                                             .usage = rhi::TextureUsage::Sampler,
                                                             .width = width,
                                                             .height = height,
                                                             .mipLevels = view.levelCount()});
    if (!texture.valid()) {
        AERO_LOG_ERROR("render: createTextureFromCookedTexture: createTexture failed for {}x{} {} ({} levels)", width,
                       height, assets::toString(view.format()), view.levelCount());
        return {};
    }

    for (std::uint32_t level = 0; level < view.levelCount(); ++level) {
        if (!device.uploadTexture(texture, level, view.levelBytes(level))) {
            AERO_LOG_ERROR("render: createTextureFromCookedTexture: level {} upload failed for {}x{} {}", level, width,
                           height, assets::toString(view.format()));
            device.destroyTexture(texture);
            return {};
        }
    }
    return texture;
}

}  // namespace engine::render

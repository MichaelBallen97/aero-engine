// Aero Engine — the ONLY stb_image TU and the ONLY GPU TU for thumbnails (task 3.1.3). Every exit is
// a state, never an exception, and this TU never logs (INV-V8: the caller decides what to do with a
// Failed/Skipped key).
#include "thumbnail_store.hpp"

#include <aero/core/guid.hpp>
#include <aero/editor/text_file.hpp>
#include <aero/rhi/descriptors.hpp>
#include <aero/rhi/device.hpp>
#include <aero/rhi/format.hpp>
#include <aero/rhi/internal/native_device.hpp>
#include <aero/rhi/types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ---- stb hygiene, ABOVE the include, all in this one TU ------------------------------------------
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO            // removes stbi_load(const char*) from the TU ENTIRELY, so "all disk access
                                 // goes through the editor's own primitives" (INV-2) cannot be broken here by
                                 // accident, and <stdio.h> never reaches this line
#define STBI_NO_FAILURE_STRINGS  // no global error string; we report a STATE, not text. The stbi__err
                                 // FUNCTION is itself guarded by this macro (stb_image.h:977-983), so
                                 // nothing is left unused
// NOLINTBEGIN -- vendored, third-party code this project neither owns nor may patch (R1/A14).
#include <stb_image.h>
// NOLINTEND

namespace engine::editor {

ThumbnailStore::ThumbnailStore(rhi::Device* deviceIn) noexcept : device(deviceIn) {}

ThumbnailStore::~ThumbnailStore() { clear(); }

ThumbnailStore::ThumbnailStore(ThumbnailStore&& other) noexcept
    : device(other.device), textures(std::move(other.textures)), attempts(other.attempts) {
    other.device = nullptr;  // a moved-from store owns nothing and available() is false (INV-V6)
    other.textures.clear();
}

ThumbnailStore& ThumbnailStore::operator=(ThumbnailStore&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    clear();  // destroy OUR OWN textures through OUR OWN device first
    device = other.device;
    textures = std::move(other.textures);
    attempts = other.attempts;
    other.device = nullptr;
    other.textures.clear();
    return *this;
}

bool ThumbnailStore::available() const noexcept { return device != nullptr; }

ThumbnailState ThumbnailStore::load(const ThumbnailKey& key, std::string_view absolutePathUtf8) {
    ++attempts;  // FIRST, unconditionally -- I37 proves a Failed key was read exactly once, ever
    if (device == nullptr) {
        return ThumbnailState::Failed;
    }

    const FileBytesResult fileResult = readFileBytes(absolutePathUtf8, MAX_THUMBNAIL_SOURCE_BYTES);
    if (!fileResult.bytes.has_value()) {
        // A size over the cap and any OS failure both land here -- the cap-exceeded case reports
        // Skipped so it never re-attempts. code-review finding 6: discriminated on
        // FileBytesResult::refusedByCap, a bool `readFileBytes` sets explicitly -- NEVER on
        // `fileResult.error`'s exact text, which used to be a literal owned by a DIFFERENT TU
        // (text_file.cpp) with nothing pinning the two together.
        return fileResult.refusedByCap ? ThumbnailState::Skipped : ThumbnailState::Failed;
    }
    const std::string& bytes = *fileResult.bytes;

    // A4's guard: the narrowing to `int` (stb's own width) must be explicit and provably safe from
    // THIS function alone, not from a constant in another header -- unreachable behind the 64 MiB
    // cap, present anyway.
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return ThumbnailState::Skipped;
    }
    const int len = static_cast<int>(bytes.size());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- viewport_panel.cpp:200's idiom
    const auto* const rawBytes = reinterpret_cast<const unsigned char*>(bytes.data());

    int infoW = 0;
    int infoH = 0;
    int infoComp = 0;
    if (stbi_info_from_memory(rawBytes, len, &infoW, &infoH, &infoComp) == 0) {
        return ThumbnailState::Failed;
    }
    if (infoW <= 0 || infoH <= 0) {
        return ThumbnailState::Failed;
    }
    const std::uint64_t pixelCount = static_cast<std::uint64_t>(infoW) * static_cast<std::uint64_t>(infoH);
    if (pixelCount > MAX_THUMBNAIL_SOURCE_PIXELS) {
        // E3: this is what stops a 20 000 x 20 000 PNG from allocating ~1.6 GB before we can refuse
        // it -- decided BEFORE the decode allocation below.
        return ThumbnailState::Skipped;
    }

    int decodedW = 0;
    int decodedH = 0;
    int decodedComp = 0;
    stbi_uc* const rawPixels = stbi_load_from_memory(rawBytes, len, &decodedW, &decodedH, &decodedComp, 4);
    if (rawPixels == nullptr) {
        return ThumbnailState::Failed;
    }
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{rawPixels, &stbi_image_free};
    if (decodedW <= 0 || decodedH <= 0) {
        return ThumbnailState::Failed;  // defensive; unreachable given stbi_info_from_memory's guards
    }
    const auto srcW = static_cast<std::uint32_t>(decodedW);
    const auto srcH = static_cast<std::uint32_t>(decodedH);
    const std::size_t pixelBytes = static_cast<std::size_t>(srcW) * srcH * 4U;

    const std::vector<std::uint8_t> tile =
        fitRgbaIntoTile(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(pixels.get()), pixelBytes),
                        srcW, srcH, THUMBNAIL_EDGE_TEXELS);
    if (tile.empty()) {
        return ThumbnailState::Failed;  // defensive; unreachable given step 3's guards
    }

    // F8: RGBA8Unorm, NOT RGBA8UnormSrgb -- ImGui's own atlas is R8G8B8A8_UNORM + SAMPLER, and using
    // the sRGB variant here would linearize on sample while ImGui's pipeline re-encodes nothing, so
    // thumbnails would read washed out beside the rest of the UI (R7).
    const rhi::TextureDesc desc{.format = rhi::TextureFormat::RGBA8Unorm,
                                .usage = rhi::TextureUsage::Sampler,
                                .width = THUMBNAIL_EDGE_TEXELS,
                                .height = THUMBNAIL_EDGE_TEXELS};
    const rhi::TextureHandle tex = device->createTexture(desc);
    if (!tex.valid()) {
        return ThumbnailState::Failed;  // E14: GPU OOM or another creation failure
    }
    device->setDebugName(tex, "thumbnail:" + formatGuid(key.guid));

    if (!device->uploadTexture(tex, 0, std::as_bytes(std::span<const std::uint8_t>(tile)))) {
        device->destroyTexture(tex);
        return ThumbnailState::Failed;
    }

    const auto it = std::lower_bound(textures.begin(), textures.end(), key,
                                     [](const auto& entry, const ThumbnailKey& k) { return entry.first < k; });
    textures.insert(it, std::pair<ThumbnailKey, rhi::TextureHandle>{key, tex});
    return ThumbnailState::Ready;
}

void* ThumbnailStore::nativeTextureFor(const ThumbnailKey& key) const noexcept {
    if (device == nullptr) {
        return nullptr;
    }
    const auto it = std::lower_bound(textures.begin(), textures.end(), key,
                                     [](const auto& entry, const ThumbnailKey& k) { return entry.first < k; });
    if (it == textures.end() || !(it->first == key)) {
        return nullptr;
    }
    return rhi::internal::NativeDeviceAccessor::texture(*device, it->second);
}

void ThumbnailStore::destroy(const ThumbnailKey& key) {
    const auto it = std::lower_bound(textures.begin(), textures.end(), key,
                                     [](const auto& entry, const ThumbnailKey& k) { return entry.first < k; });
    if (it == textures.end() || !(it->first == key)) {
        return;
    }
    if (device != nullptr) {
        device->destroyTexture(it->second);
    }
    textures.erase(it);
}

void ThumbnailStore::clear() {
    if (device != nullptr) {
        for (const auto& entry : textures) {
            device->destroyTexture(entry.second);
        }
    }
    textures.clear();
}

std::size_t ThumbnailStore::loadAttempts() const noexcept { return attempts; }
std::size_t ThumbnailStore::residentCount() const noexcept { return textures.size(); }

}  // namespace engine::editor

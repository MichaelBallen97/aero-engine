#pragma once
// Aero Engine — src-private: the ONLY stb_image TU and the ONLY GPU TU for thumbnails (task 3.1.3).
// Everything above it (the panel, EditorApp) speaks only ThumbnailState/ThumbnailKey -- the RHI
// texture handle and the decoded pixel buffer never leave this pair of files.
#include <aero/editor/thumbnail_cache.hpp>
#include <aero/rhi/handles.hpp>

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::rhi {
class Device;  // forward-declared, never #included here -- viewport_panel.hpp:19-21's shape
}  // namespace engine::rhi

namespace engine::editor {

class ThumbnailStore {
public:
    ThumbnailStore() noexcept = default;
    explicit ThumbnailStore(rhi::Device* device) noexcept;  // nullptr == thumbnails unavailable
    ~ThumbnailStore();                                      // destroys every live texture
    ThumbnailStore(const ThumbnailStore&) = delete;
    ThumbnailStore& operator=(const ThumbnailStore&) = delete;
    ThumbnailStore(ThumbnailStore&& other) noexcept;             // steals `textures`, NULLS other.device
    ThumbnailStore& operator=(ThumbnailStore&& other) noexcept;  // destroys OURS first, then steals

    [[nodiscard]] bool available() const noexcept;  // device != nullptr

    // Reads, decodes, resamples and uploads ONE thumbnail. Returns the resulting state; NEVER throws
    // and NEVER logs. `absolutePathUtf8` is built by the caller; this function knows nothing of roots.
    [[nodiscard]] ThumbnailState load(const ThumbnailKey& key, std::string_view absolutePathUtf8);

    [[nodiscard]] void* nativeTextureFor(const ThumbnailKey& key) const noexcept;  // nullptr if absent
    void destroy(const ThumbnailKey& key);
    void clear();

    // Black-box observability, the ViewportPanel::logRecordCount() / 2.2.5 D16 precedent. These exist
    // so I36/I37 can assert "no tick decoded more than the budget" and "a Failed key was read exactly
    // once, ever" -- neither is observable from outside otherwise. Monotonic; reset only by clear().
    [[nodiscard]] std::size_t loadAttempts() const noexcept;
    [[nodiscard]] std::size_t residentCount() const noexcept;

private:
    rhi::Device* device = nullptr;                                      // non-owning (F12)
    std::vector<std::pair<ThumbnailKey, rhi::TextureHandle>> textures;  // sorted, §D-2's reasoning
    std::size_t attempts = 0;
};

}  // namespace engine::editor

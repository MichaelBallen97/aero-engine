#pragma once
// SRC-PRIVATE: it owns a ThumbnailStore, the GPU half (thumbnail_store.hpp). task E.3.3 (D5): ONE
// ledger, ONE store, ONE clock, ONE budget, THREE consumers. This is 3.1.3's two-phase wiring lifted
// out of AssetBrowserPanel unchanged: a consumer NOTES the keys it drew (draw walk, no mutation),
// service() runs in EditorApp::tick's post-draw slot -- the SAME slot and the SAME statement position
// the browser's serviceThumbnails() occupied -- and inside it the order is
// touch -> reimport clear -> superseded sweep -> EVICT -> DECODE, for the reasons the browser's
// comments gave, which move with the code.
#include <aero/core/guid.hpp>
#include <aero/editor/thumbnail_cache.hpp>

#include "thumbnail_store.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::rhi {
class Device;  // forward-declared, never #included here -- thumbnail_store.hpp:12-14's shape
}  // namespace engine::rhi

namespace engine::editor {

class AssetDatabase;  // forward-declared: the .cpp includes <aero/editor/asset_database.hpp>

class ThumbnailService {
public:
    explicit ThumbnailService(rhi::Device* device) noexcept;  // nullptr == thumbnails unavailable
    ~ThumbnailService() = default;
    ThumbnailService(const ThumbnailService&) = delete;
    ThumbnailService& operator=(const ThumbnailService&) = delete;
    ThumbnailService(ThumbnailService&&) = delete;  // held through a unique_ptr; never moved
    ThumbnailService& operator=(ThumbnailService&&) = delete;

    [[nodiscard]] bool available() const noexcept;

    // Draw-walk side, and NEITHER of these touches the GPU or the ledger's state machine.
    void noteVisible(const ThumbnailKey& key);
    [[nodiscard]] void* nativeTextureFor(const ThumbnailKey& key) const noexcept;  // nullptr until Ready

    // Set from ANY phase; drained INSIDE service(), AFTER its touch loop (3.1.3's BLOCKING-1 ordering).
    void requestReimportClear() noexcept;
    void noteDatabaseRescanned() noexcept;  // 3.1.4's superseded sweep
    void clear();                           // a project swap: ledger + store, both

    // THE ONLY MUTATOR. Never called from a draw walk.
    void service(const AssetDatabase& database);

    [[nodiscard]] std::size_t readyCount() const noexcept;
    [[nodiscard]] std::size_t unavailableCount() const noexcept;
    [[nodiscard]] std::size_t residentCount() const noexcept;
    [[nodiscard]] std::size_t loadAttempts() const noexcept;

private:
    // "" when the record behind `key` has vanished (a rescan raced the decode) -- the caller treats an
    // empty path as Failed, never as "try again". Reads database.root() where the browser read its own
    // `rootUtf8`: THE SAME STRING BY CONSTRUCTION, both reconciled from project.assetsRoot() in one
    // block (INV-A9/A16).
    [[nodiscard]] std::string absolutePathFor(const ThumbnailKey& key, const AssetDatabase& database) const;

    ThumbnailLedger ledger;
    ThumbnailStore store;
    std::vector<ThumbnailKey> visible;  // per-frame scratch; cleared by service()
    std::uint64_t frame = 0;            // the LRU's clock; monotonic, NEVER wall time
    bool pendingReimportClear = false;
    bool pendingSupersededSweep = false;
    // MEMBERS, never locals: the visible/breadcrumb/labelScratch idiom, so a 50 000-record project does
    // not allocate two large vectors on every rescan.
    std::vector<ThumbnailKey> liveKeyScratch;
    std::vector<Guid> abstainingScratch;
};

}  // namespace engine::editor

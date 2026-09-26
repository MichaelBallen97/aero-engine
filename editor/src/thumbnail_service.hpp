#pragma once
// SRC-PRIVATE: it owns a ThumbnailStore, the GPU half (thumbnail_store.hpp). task E.3.3 (D5): ONE
// ledger, ONE store, ONE clock, ONE budget, THREE consumers. This is 3.1.3's two-phase wiring lifted
// out of AssetBrowserPanel unchanged: a consumer NOTES the keys it drew (draw walk, no mutation),
// service() runs in EditorApp::tick's post-draw slot -- the SAME slot and the SAME statement position
// the browser's serviceThumbnails() occupied -- and inside it the order is
// touch -> reimport clear -> superseded sweep -> EVICT -> DECODE, for the reasons the browser's
// comments gave, which move with the code.
//
// task E.4.5: A SECOND PRODUCER, NOT A SECOND CACHE. One ledger, one clock, one cap, one LRU and ONE release
// site (releaseKey) -- and two backing stores behind them: ThumbnailStore (decoded images) and
// MaterialThumbnailRenderer (rendered materials), routed by thumbnailSourceForName in ONE walk over every
// Absent key with one budget per producer. The material CARD cache (names and swatches) sits beside them and
// is serviced ABOVE the device gate, because it needs no GPU at all.
#include <aero/core/guid.hpp>
#include <aero/editor/thumbnail_cache.hpp>

#include "material_card_cache.hpp"  // task E.4.5 -- names and swatches, device-free, held by value
#include "material_thumbnail.hpp"   // task E.4.5 -- the second backing store, held by value
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
    // task E.4.5: THE RENDER STORE's own counters. residentCount() and loadAttempts() above stay the DECODE
    // store's -- I36, I37, I38 and I167 assert exactly that meaning -- while readyCount() and
    // unavailableCount() stay the LEDGER's, spanning both producers, as they always meant "the editor's
    // thumbnails". INVARIANT at the end of every service() and clear(), because each Ready key owns exactly
    // one texture in exactly one store: residentCount() + materialResidentCount() == readyCount().
    [[nodiscard]] std::size_t materialResidentCount() const noexcept;
    [[nodiscard]] std::size_t materialRenderAttempts() const noexcept;  // monotonic; every produce() call
    [[nodiscard]] bool materialThumbnailsAvailable() const noexcept;    // MaterialThumbnailRenderer::available
    [[nodiscard]] const render::RenderTarget* materialTargetFor(const ThumbnailKey& key) const noexcept;
    // task E.4.5: the material CARD -- a name and a swatch -- on its OWN request queue, never noteVisible, which
    // is the LEDGER's touch list and means exactly that (E.3.3's S34/I167). DRAW-WALK SAFE, both; cardFor's
    // pointer is valid until the next service() or clear().
    void noteCardWanted(const ThumbnailKey& key);
    [[nodiscard]] const MaterialCard* cardFor(const ThumbnailKey& key) const noexcept;
    [[nodiscard]] std::size_t materialCardCount() const noexcept;
    [[nodiscard]] std::size_t materialCardReadCount() const noexcept;  // monotonic

private:
    // task E.4.5: THE ONE RELEASE SITE (D3). Three callers -- the reimport clear, the superseded sweep and the
    // LRU eviction -- and no fourth. EXACTLY ONE of its two destroys does work for any given key, because
    // ThumbnailSource is a partition; calling both unconditionally spares every caller from knowing which
    // producer owns the key it releases. (absolutePathFor is gone: the walk resolves the record itself,
    // because it needs the record to route, and treats a vanished one as Failed -- that function's own
    // contract, made explicit.)
    void releaseKey(const ThumbnailKey& key);
    // The ledger's four-arm mark, shared by both producers' results.
    void markLedger(const ThumbnailKey& key, ThumbnailState state);

    ThumbnailLedger ledger;
    ThumbnailStore store;
    // task E.4.5: the second backing store (D3). Declared AFTER `store`, and the constructor's init list keeps
    // that order (-Wreorder). Destroyed BEFORE `store` and the ledger, and before ~Device either way.
    MaterialThumbnailRenderer renders;
    MaterialCardCache cards;            // task E.4.5 -- DEVICE-FREE; serviced ABOVE the device gate in service()
    std::vector<ThumbnailKey> visible;  // per-frame scratch; cleared by service()
    // task E.4.5's code-review round: THIS frame's `visible`, sorted and de-duplicated before the scratch is
    // cleared, so the produce walk can spend a RENDER only on a tile drawn this frame. A MEMBER, never a local:
    // the visible/liveKeyScratch idiom.
    std::vector<ThumbnailKey> drawnThisFrame;
    std::uint64_t frame = 0;  // the LRU's clock; monotonic, NEVER wall time
    bool pendingReimportClear = false;
    bool pendingSupersededSweep = false;
    // MEMBERS, never locals: the visible/breadcrumb/labelScratch idiom, so a 50 000-record project does
    // not allocate two large vectors on every rescan.
    std::vector<ThumbnailKey> liveKeyScratch;
    std::vector<Guid> abstainingScratch;
};

}  // namespace engine::editor

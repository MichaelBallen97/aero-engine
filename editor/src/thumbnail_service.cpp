// editor/src/thumbnail_service.cpp -- task E.3.3 (D5): the shared thumbnail ledger/store/budget,
// lifted out of AssetBrowserPanel with THREE mechanical differences and nothing else:
//   1. `++frame;` is the FIRST statement of service(), replacing the browser's reconcile()-time
//      increment. Touching and evicting still read the same value within one pass, so the semantics
//      are identical -- but the DIRECTION matters: incrementing AFTER the touch loop would let
//      evictions see last frame's touches and destroy a key drawn THIS frame, which is 3.1.3's
//      BLOCKING-1 (synchronous on Vulkan/D3D12, deferred on Metal).
//   2. absolutePathFor takes `const AssetDatabase&` and reads database.root() where the browser read
//      its own rootUtf8; the null-database branch disappears with the reference.
//   3. clear() is new -- the two lines the browser's setRoot() used to perform inline.
// Everything else below is the browser's own body, comments included.
#include "thumbnail_service.hpp"

#include <aero/editor/asset_cache.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/asset_meta.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

namespace engine::editor {

ThumbnailService::ThumbnailService(rhi::Device* device) noexcept : store(device) {}

bool ThumbnailService::available() const noexcept { return store.available(); }

void ThumbnailService::noteVisible(const ThumbnailKey& key) { visible.push_back(key); }

void* ThumbnailService::nativeTextureFor(const ThumbnailKey& key) const noexcept { return store.nativeTextureFor(key); }

void ThumbnailService::requestReimportClear() noexcept { pendingReimportClear = true; }

void ThumbnailService::noteDatabaseRescanned() noexcept { pendingSupersededSweep = true; }

void ThumbnailService::clear() {
    ledger.clear();
    store.clear();
}

std::size_t ThumbnailService::readyCount() const noexcept { return ledger.readyCount(); }
std::size_t ThumbnailService::unavailableCount() const noexcept { return ledger.unavailableCount(); }
std::size_t ThumbnailService::residentCount() const noexcept { return store.residentCount(); }
std::size_t ThumbnailService::loadAttempts() const noexcept { return store.loadAttempts(); }

std::string ThumbnailService::absolutePathFor(const ThumbnailKey& key, const AssetDatabase& database) const {
    const AssetRecord* const record = database.findByGuid(key.guid);
    if (record == nullptr) {
        return {};  // the record vanished (a rescan raced the decode) -- treated as Failed, never retried
    }
    return database.root() + "/" + record->relativePath;
}

void ThumbnailService::service(const AssetDatabase& database) {
    ++frame;                   // the LRU's clock, advanced BEFORE the touch loop (difference 1 in the banner above)
    if (!store.available()) {  // E13/AC-11: no device -- thumbnails stay unavailable forever
        visible.clear();
        return;
    }
    for (const ThumbnailKey& key : visible) {
        ledger.touch(key, frame);
    }
    visible.clear();

    // code-review BLOCKING-1: the ReimportAll flag, drained HERE -- after the touch loop above, so
    // anything drawn (and therefore touched) THIS SAME frame is already marked at `frame` and
    // is excluded by evictions()'s own "never evict a key touched at currentFrame" rule (E12). A key
    // still visible next tick is touched again and survives again, forever, at zero cost beyond
    // holding a possibly-stale (but never dangling) texture one tick longer; a key whose content
    // actually changed gets a brand-new ThumbnailKey once EditorApp's own rescan (reimportRequested,
    // set by the browser) completes and is decoded fresh regardless. `evictions(0, frame)` reads as
    // "every Ready key beyond a cap of zero" -- i.e. every Ready key that eviction's own protection
    // does not shield.
    if (pendingReimportClear) {
        pendingReimportClear = false;
        for (const ThumbnailKey& key : ledger.evictions(0, frame)) {
            store.destroy(key);
            ledger.forget(key);
        }
    }

    // task 3.1.4 (D9/AC-31): drained HERE -- after the touch loop above, so every key drawn this
    // frame is already marked at `frame` and is excluded by supersededBy's own currentFrame
    // rule, the same protection E12 gives ordinary eviction. NEVER from a draw walk: 3.1.3's
    // BLOCKING-1, where SDL_ReleaseGPUTexture frees SYNCHRONOUSLY on Vulkan
    // (SDL_gpu_vulkan.c:7070-7073) and D3D12 (SDL_gpu_d3d12.c:1460-1463) and only DEFERS on Metal
    // (SDL_gpu_metal.m:936-944).
    //
    // What this exists for: ThumbnailKey is {Guid, ContentHash}, so an edited texture already gets a
    // FRESH key and re-decodes for free. What needs code is the OLD key -- its GPU texture is now
    // unreachable forever, and under LRU alone it survives until 256 residents push it out.
    // Iterating in Photoshop therefore strands one dead 128x128 RGBA8 texture per save.
    if (pendingSupersededSweep) {
        pendingSupersededSweep = false;
        liveKeyScratch.clear();
        abstainingScratch.clear();
        for (const AssetRecord& assetRecord : database.records()) {
            // metaWriteFailed FIRST -- a failed sidecar write leaves `change` at its default
            // UpToDate for a file whose bytes on disk are not what was hashed (3.1.2's own
            // code-review finding 3). An all-zero contentHash is the EMPTY FILE's real digest,
            // not a sentinel, so the ONLY "was this hashed?" test is the `change` enum (3.1.2 A4).
            const bool hashUsable = !assetRecord.metaWriteFailed && assetRecord.guid.valid() &&
                                    assetRecord.change != ImportChange::NotHashed &&
                                    assetRecord.change != ImportChange::Unhashable;
            if (hashUsable) {
                liveKeyScratch.push_back(ThumbnailKey{.guid = assetRecord.guid, .hash = assetRecord.contentHash});
            } else if (assetRecord.guid.valid()) {
                abstainingScratch.push_back(assetRecord.guid);  // AC-32: NO OPINION about its keys
            }
        }
        std::sort(liveKeyScratch.begin(), liveKeyScratch.end());  // supersededBy's precondition
        std::sort(abstainingScratch.begin(), abstainingScratch.end());
        for (const ThumbnailKey& key : ledger.supersededBy(liveKeyScratch, abstainingScratch, frame)) {
            store.destroy(key);
            ledger.forget(key);
        }
    }

    // EVICTION RUNS BEFORE DECODING, DELIBERATELY (INV-V5, seed S3): the other order lets the
    // resident count exceed the cap by up to MAX_THUMBNAIL_DECODES_PER_TICK for a tick, which makes
    // the bound this task states in a FOOTER a lie.
    for (const ThumbnailKey& key : ledger.evictions(MAX_THUMBNAILS_RESIDENT, frame)) {
        store.destroy(key);
        ledger.forget(key);
    }
    for (const ThumbnailKey& key : ledger.nextDecodes(MAX_THUMBNAIL_DECODES_PER_TICK)) {
        const std::string absolute = absolutePathFor(key, database);
        const ThumbnailState state = absolute.empty() ? ThumbnailState::Failed : store.load(key, absolute);
        switch (state) {  // NO default: -- a new state is a -Wswitch warning, not a silent fallthrough
            case ThumbnailState::Ready:
                ledger.markReady(key);
                break;
            case ThumbnailState::Failed:
                ledger.markFailed(key);
                break;
            case ThumbnailState::Skipped:
                ledger.markSkipped(key);
                break;
            case ThumbnailState::Absent:
                ledger.markFailed(key);  // load() never returns it; defensive
                break;
        }
    }
}

}  // namespace engine::editor

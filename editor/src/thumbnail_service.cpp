// editor/src/thumbnail_service.cpp -- task E.3.3 (D5): the shared thumbnail ledger/store/budget,
// lifted out of AssetBrowserPanel with THREE mechanical differences and nothing else:
//   1. `++frame;` is the FIRST statement of service(), replacing the browser's reconcile()-time
//      increment. Touching and evicting still read the same value within one pass, so the semantics
//      are identical -- but the DIRECTION matters: incrementing AFTER the touch loop would let
//      evictions see last frame's touches and destroy a key drawn THIS frame, which is 3.1.3's
//      BLOCKING-1 (synchronous on Vulkan/D3D12, deferred on Metal).
//   2. absolutePathFor took `const AssetDatabase&` and read database.root() where the browser read its
//      own rootUtf8; the null-database branch disappeared with the reference. (task E.4.5 removed it: the
//      produce walk resolves the record itself, because it needs the record to route.)
//   3. clear() is new -- the two lines the browser's setRoot() used to perform inline.
// Everything else below is the browser's own body, comments included.
#include "thumbnail_service.hpp"

#include <aero/editor/asset_cache.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/project_files.hpp>  // task E.4.5 -- leafOf, for the walk's routing

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>

namespace engine::editor {

namespace {

// task E.4.5 (D-H): "every Absent key", in the ledger's own vocabulary. nextDecodes collects and sorts every
// Absent entry whatever its budget, so asking for all of them costs only the copies it would have discarded.
constexpr std::size_t ALL_ABSENT_KEYS = std::numeric_limits<std::size_t>::max();

}  // namespace

ThumbnailService::ThumbnailService(rhi::Device* device) noexcept : store(device), renders(device) {}

bool ThumbnailService::available() const noexcept { return store.available(); }

void ThumbnailService::noteVisible(const ThumbnailKey& key) { visible.push_back(key); }

void* ThumbnailService::nativeTextureFor(const ThumbnailKey& key) const noexcept {
    // task E.4.5: the decode store FIRST, so the common path is one binary search. The two key sets are
    // DISJOINT by construction (ThumbnailSource partitions), so the order is a cost decision, not a semantic one.
    if (void* const decoded = store.nativeTextureFor(key); decoded != nullptr) {
        return decoded;
    }
    return renders.nativeTextureFor(key);
}

void ThumbnailService::requestReimportClear() noexcept { pendingReimportClear = true; }

void ThumbnailService::noteDatabaseRescanned() noexcept { pendingSupersededSweep = true; }

void ThumbnailService::clear() {
    ledger.clear();
    store.clear();
    renders.clear();  // task E.4.5 -- the same swap point; the render CHAIN survives, it is project-independent
}

std::size_t ThumbnailService::readyCount() const noexcept { return ledger.readyCount(); }
std::size_t ThumbnailService::unavailableCount() const noexcept { return ledger.unavailableCount(); }
std::size_t ThumbnailService::residentCount() const noexcept { return store.residentCount(); }
std::size_t ThumbnailService::loadAttempts() const noexcept { return store.loadAttempts(); }
// task E.4.5: the render store's own four -- see the header for why the four above keep their meanings.
std::size_t ThumbnailService::materialResidentCount() const noexcept { return renders.residentCount(); }
std::size_t ThumbnailService::materialRenderAttempts() const noexcept { return renders.renderAttempts(); }
bool ThumbnailService::materialThumbnailsAvailable() const noexcept { return renders.available(); }
const render::RenderTarget* ThumbnailService::materialTargetFor(const ThumbnailKey& key) const noexcept {
    return renders.targetFor(key);
}

void ThumbnailService::releaseKey(const ThumbnailKey& key) {
    store.destroy(key);    // a no-op for a rendered key
    renders.destroy(key);  // a no-op for a decoded key
    ledger.forget(key);
}

void ThumbnailService::markLedger(const ThumbnailKey& key, ThumbnailState state) {
    switch (state) {  // NO default: -- a new state is a -Wswitch diagnostic, not a silent fallthrough
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
            ledger.markFailed(key);  // neither producer returns it; defensive
            break;
    }
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
            releaseKey(key);  // task E.4.5 -- both stores, one helper
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
            releaseKey(key);  // task E.4.5
        }
    }

    // EVICTION RUNS BEFORE DECODING, DELIBERATELY (INV-V5, seed S3): the other order lets the
    // resident count exceed the cap by up to MAX_THUMBNAIL_DECODES_PER_TICK for a tick, which makes
    // the bound this task states in a FOOTER a lie.
    for (const ThumbnailKey& key : ledger.evictions(MAX_THUMBNAILS_RESIDENT, frame)) {
        releaseKey(key);  // task E.4.5
    }
    // task E.4.5 (D-H): ONE walk over EVERY Absent key, oldest first, spending each budget as it meets a
    // candidate of its own kind and stopping the moment both are spent. Asking the ledger for only
    // (decodes + renders) keys would hand this loop the THREE oldest, which can all be materials -- tiles drawn
    // in one frame share lastTouched, so their order is the ledger's GUID order -- and no image would decode
    // until every material ahead of it had rendered, one per tick. Two nextDecodes calls, one per budget,
    // starve the same way. A key skipped for budget stays Absent and is first in line next tick.
    std::size_t decodesSpent = 0;
    std::size_t rendersSpent = 0;
    for (const ThumbnailKey& key : ledger.nextDecodes(ALL_ABSENT_KEYS)) {
        if (decodesSpent >= MAX_THUMBNAIL_DECODES_PER_TICK && rendersSpent >= MAX_THUMBNAIL_RENDERS_PER_TICK) {
            break;
        }
        const AssetRecord* const record = database.findByGuid(key.guid);
        if (record == nullptr) {
            ledger.markFailed(key);  // the record vanished (a rescan raced the draw walk) -- Failed, NEVER retried
            continue;
        }
        switch (thumbnailSourceForName(leafOf(record->relativePath))) {  // NO default: -- -Wswitch
            case ThumbnailSource::DecodedImage:
                if (decodesSpent >= MAX_THUMBNAIL_DECODES_PER_TICK) {
                    continue;
                }
                ++decodesSpent;
                markLedger(key, store.load(key, database.root() + "/" + record->relativePath));
                break;
            case ThumbnailSource::RenderedMaterial:
                if (rendersSpent >= MAX_THUMBNAIL_RENDERS_PER_TICK) {
                    continue;
                }
                ++rendersSpent;
                markLedger(key, renders.produce(key, database.root() + "/" + record->relativePath, database));
                break;
            case ThumbnailSource::None:
                // UNREACHABLE by construction: a key exists only because thumbnailKeyForRecord accepted this
                // very record, and it refuses None. Marked sticky so it can never occupy the walk again.
                ledger.markFailed(key);
                break;
        }
    }
}

}  // namespace engine::editor

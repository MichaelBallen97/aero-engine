// editor/src/material_card_cache.cpp -- task E.4.5: the material card cache. material_card_cache.hpp states
// every rule; the presentation decisions themselves are material_card.hpp's.
#include "material_card_cache.hpp"

#include <aero/editor/asset_database.hpp>
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/project_files.hpp>  // leafOf
#include <aero/editor/text_file.hpp>      // readFileBytes -- the CAPPED read
#include <aero/reflect/material_format.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

template <typename Entries>
[[nodiscard]] auto entryLowerBound(Entries& entries, const ThumbnailKey& key) noexcept {
    return std::lower_bound(entries.begin(), entries.end(), key,
                            [](const auto& entry, const ThumbnailKey& k) { return entry.key < k; });
}

}  // namespace

MaterialCardCache::MaterialCardCache(std::size_t capacityIn) noexcept : capacity(capacityIn) {}

void MaterialCardCache::noteCardWanted(const ThumbnailKey& key) { wanted.push_back(key); }

const MaterialCard* MaterialCardCache::cardFor(const ThumbnailKey& key) const noexcept {
    const auto at = entryLowerBound(entries, key);
    if (at == entries.end() || !(at->key == key) || !at->parsed) {
        return nullptr;
    }
    return &at->card;
}

void MaterialCardCache::service(const AssetDatabase& database, std::uint64_t frame) {
    std::size_t readsThisTick = 0;
    for (const ThumbnailKey& key : wanted) {
        const auto at = entryLowerBound(entries, key);
        if (at != entries.end() && at->key == key) {
            at->lastWanted = frame;  // KNOWN: a card, or a sticky failure -- never read again for this key
            continue;
        }
        if (readsThisTick >= MAX_MATERIAL_CARD_READS_PER_TICK) {
            continue;  // still wanted next frame while it is still on screen; nothing is lost
        }
        // THE GATE (task E.4.5's C8), in full, BEFORE any byte is read. The bytes about to be read must be the
        // bytes this key names -- the record's CURRENT key -- and they must be a material's.
        const AssetRecord* const record = database.findByGuid(key.guid);
        if (record == nullptr) {
            continue;  // a rescan raced the draw walk: no read, no entry
        }
        const std::optional<ThumbnailKey> current = thumbnailKeyForRecord(*record);
        if (!current.has_value() || !(*current == key)) {
            continue;  // a stale key: the bytes on disk are no longer the bytes it names
        }
        if (thumbnailSourceForName(leafOf(record->relativePath)) != ThumbnailSource::RenderedMaterial) {
            continue;  // an IMAGE's key -- never read as a material
        }
        ++readsThisTick;
        ++reads;
        Entry entry{.key = key, .card = {}, .lastWanted = frame, .parsed = false};
        const FileBytesResult file =
            readFileBytes(database.root() + "/" + record->relativePath, MAX_THUMBNAIL_SOURCE_BYTES);
        if (file.bytes.has_value()) {
            const MaterialParseResult parsed = parseMaterial(*file.bytes);
            if (parsed.document.has_value()) {
                entry.card = materialCardFor(*parsed.document);
                entry.parsed = true;
            }
        }
        entries.insert(at, std::move(entry));  // `at` is this iteration's own lower bound -- still valid
    }
    wanted.clear();
    evictDownToCapacity(frame);
}

void MaterialCardCache::evictDownToCapacity(std::uint64_t frame) {
    while (entries.size() > capacity) {
        auto victim = entries.end();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->lastWanted == frame) {
                continue;  // wanted THIS frame -- never evicted (the ledger's E12 rule, for the same reason)
            }
            if (victim == entries.end() || it->lastWanted < victim->lastWanted) {
                victim = it;  // least recently wanted; the first in key order on a tie
            }
        }
        if (victim == entries.end()) {
            return;  // everything left was wanted this frame: over capacity for one frame, never thrashing
        }
        entries.erase(victim);
    }
}

void MaterialCardCache::clear() noexcept {
    entries.clear();
    wanted.clear();
}

std::size_t MaterialCardCache::readCount() const noexcept { return reads; }

std::size_t MaterialCardCache::cardCount() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(entries.begin(), entries.end(), [](const Entry& entry) { return entry.parsed; }));
}

}  // namespace engine::editor

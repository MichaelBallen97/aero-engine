// Aero Engine — the thumbnail policy layer (task 3.1.3). PURE: no stb, no GPU, no ImGui, no
// <filesystem>, no logging (INV-V8). The ledger's bounds and the box resampler are both provable
// from a std::vector/std::span literal with no context of any kind.
#include <aero/editor/thumbnail_cache.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::editor {

void ThumbnailLedger::touch(const ThumbnailKey& key, std::uint64_t frame) {
    const auto it = std::lower_bound(entries.begin(), entries.end(), key,
                                     [](const Entry& e, const ThumbnailKey& k) { return e.key < k; });
    if (it != entries.end() && it->key == key) {
        it->lastTouched = frame;  // does NOT reset state (seed S1)
        return;
    }
    entries.insert(it, Entry{.key = key, .state = ThumbnailState::Absent, .lastTouched = frame});
}

ThumbnailState ThumbnailLedger::stateOf(const ThumbnailKey& key) const noexcept {
    const auto it = std::lower_bound(entries.begin(), entries.end(), key,
                                     [](const Entry& e, const ThumbnailKey& k) { return e.key < k; });
    if (it == entries.end() || !(it->key == key)) {
        return ThumbnailState::Absent;
    }
    return it->state;
}

void ThumbnailLedger::setState(const ThumbnailKey& key, ThumbnailState state) noexcept {
    const auto it = std::lower_bound(entries.begin(), entries.end(), key,
                                     [](const Entry& e, const ThumbnailKey& k) { return e.key < k; });
    if (it != entries.end() && it->key == key) {
        it->state = state;
    }
    // A mark for a key never touched is a no-op -- serviceThumbnails() only ever marks a key that
    // nextDecodes() itself returned, which is always already an entry.
}

void ThumbnailLedger::markReady(const ThumbnailKey& key) { setState(key, ThumbnailState::Ready); }
void ThumbnailLedger::markFailed(const ThumbnailKey& key) { setState(key, ThumbnailState::Failed); }
void ThumbnailLedger::markSkipped(const ThumbnailKey& key) { setState(key, ThumbnailState::Skipped); }

std::vector<ThumbnailKey> ThumbnailLedger::nextDecodes(std::size_t budget) const {
    std::vector<ThumbnailKey> result;
    if (budget == 0) {
        return result;
    }
    std::vector<const Entry*> absent;
    for (const Entry& e : entries) {
        if (e.state == ThumbnailState::Absent) {  // INV-V4: NEVER a Failed or Skipped key
            absent.push_back(&e);
        }
    }
    std::stable_sort(absent.begin(), absent.end(),
                     [](const Entry* a, const Entry* b) { return a->lastTouched < b->lastTouched; });
    result.reserve(std::min(budget, absent.size()));
    for (const Entry* e : absent) {
        if (result.size() >= budget) {
            break;
        }
        result.push_back(e->key);
    }
    return result;
}

std::vector<ThumbnailKey> ThumbnailLedger::evictions(std::size_t residentCap, std::uint64_t currentFrame) const {
    std::vector<const Entry*> ready;
    for (const Entry& e : entries) {
        if (e.state == ThumbnailState::Ready) {
            ready.push_back(&e);
        }
    }
    if (ready.size() <= residentCap) {
        return {};  // INV-V5: a cap, not a TTL -- nothing beyond it is evicted
    }
    const std::size_t toEvict = ready.size() - residentCap;
    std::stable_sort(ready.begin(), ready.end(),
                     [](const Entry* a, const Entry* b) { return a->lastTouched < b->lastTouched; });
    std::vector<ThumbnailKey> result;
    result.reserve(toEvict);
    for (const Entry* e : ready) {
        if (result.size() >= toEvict) {
            break;
        }
        if (e->lastTouched == currentFrame) {
            continue;  // E12/seed S2: never evict a key touched THIS frame, even if that leaves us
                       // above the cap (seed S2/S3 -- this is the ONLY thing that protects it)
        }
        result.push_back(e->key);
    }
    return result;
}

std::vector<ThumbnailKey> ThumbnailLedger::supersededBy(std::span<const ThumbnailKey> liveKeys,
                                                        std::span<const Guid> abstainingGuids,
                                                        std::uint64_t currentFrame) const {
    std::vector<ThumbnailKey> result;
    for (const Entry& e : entries) {
        if (e.lastTouched == currentFrame) {
            continue;  // AC-33 -- E12's rule, and 3.1.3's BLOCKING-1 made structural
        }
        if (std::binary_search(liveKeys.begin(), liveKeys.end(), e.key)) {
            continue;  // still live: some record still carries exactly this {guid, hash}
        }
        if (std::binary_search(abstainingGuids.begin(), abstainingGuids.end(), e.key.guid)) {
            continue;  // AC-32 -- a record with no opinion about its own keys
        }
        result.push_back(e.key);
    }
    return result;
}

void ThumbnailLedger::forget(const ThumbnailKey& key) {
    const auto it = std::lower_bound(entries.begin(), entries.end(), key,
                                     [](const Entry& e, const ThumbnailKey& k) { return e.key < k; });
    if (it != entries.end() && it->key == key) {
        entries.erase(it);
    }
}

void ThumbnailLedger::clear() noexcept { entries.clear(); }

std::size_t ThumbnailLedger::readyCount() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(entries.begin(), entries.end(), [](const Entry& e) { return e.state == ThumbnailState::Ready; }));
}

std::size_t ThumbnailLedger::unavailableCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(), [](const Entry& e) {
        return e.state == ThumbnailState::Failed || e.state == ThumbnailState::Skipped;
    }));
}

// ---- the resampler ----------------------------------------------------------------------------
std::vector<std::uint8_t> fitRgbaIntoTile(std::span<const std::uint8_t> src, std::uint32_t srcW, std::uint32_t srcH,
                                          std::uint32_t edge) {
    if (edge == 0 || srcW == 0 || srcH == 0) {
        return {};
    }
    const std::uint64_t requiredSrcBytes = static_cast<std::uint64_t>(srcW) * static_cast<std::uint64_t>(srcH) * 4ULL;
    if (src.size() < requiredSrcBytes) {
        return {};
    }

    // scale = min(1, edge/srcW, edge/srcH), computed WITHOUT floating point (A11): a source smaller
    // than the tile in both axes stays at 1:1 (E5); otherwise the LARGER axis is fit exactly to
    // `edge` and the other axis follows proportionally, floored, and never below 1 (E6).
    std::uint32_t dstW = srcW;
    std::uint32_t dstH = srcH;
    if (srcW > edge || srcH > edge) {
        if (srcW >= srcH) {
            dstW = edge;
            dstH = static_cast<std::uint32_t>((static_cast<std::uint64_t>(srcH) * edge) / srcW);
        } else {
            dstH = edge;
            dstW = static_cast<std::uint32_t>((static_cast<std::uint64_t>(srcW) * edge) / srcH);
        }
        dstW = std::max(1U, dstW);
        dstH = std::max(1U, dstH);
    }

    const std::uint32_t offsetX = (edge - dstW) / 2U;
    const std::uint32_t offsetY = (edge - dstH) / 2U;

    // Transparent black by default (E8) -- only the dstW x dstH region below is ever overwritten.
    std::vector<std::uint8_t> tile(static_cast<std::size_t>(edge) * edge * 4U, 0U);

    for (std::uint32_t dy = 0; dy < dstH; ++dy) {
        const std::uint64_t y0 = (static_cast<std::uint64_t>(dy) * srcH) / dstH;
        std::uint64_t y1 = (static_cast<std::uint64_t>(dy) + 1U) * srcH / dstH;
        if (y1 <= y0) {
            y1 = y0 + 1U;  // clamp so the rectangle is never empty
        }
        for (std::uint32_t dx = 0; dx < dstW; ++dx) {
            const std::uint64_t x0 = (static_cast<std::uint64_t>(dx) * srcW) / dstW;
            std::uint64_t x1 = (static_cast<std::uint64_t>(dx) + 1U) * srcW / dstW;
            if (x1 <= x0) {
                x1 = x0 + 1U;
            }

            std::uint64_t sumR = 0;
            std::uint64_t sumG = 0;
            std::uint64_t sumB = 0;
            std::uint64_t sumA = 0;
            std::uint64_t count = 0;  // std::uint64_t (A11), not uint32_t -- 500 000 samples max
            for (std::uint64_t sy = y0; sy < y1; ++sy) {
                for (std::uint64_t sx = x0; sx < x1; ++sx) {
                    const std::size_t srcIndex =
                        (static_cast<std::size_t>(sy) * srcW + static_cast<std::size_t>(sx)) * 4U;
                    sumR += src[srcIndex + 0];
                    sumG += src[srcIndex + 1];
                    sumB += src[srcIndex + 2];
                    sumA += src[srcIndex + 3];
                    ++count;
                }
            }
            const std::size_t dstIndex =
                (static_cast<std::size_t>(offsetY + dy) * edge + static_cast<std::size_t>(offsetX + dx)) * 4U;
            tile[dstIndex + 0] = static_cast<std::uint8_t>(sumR / count);
            tile[dstIndex + 1] = static_cast<std::uint8_t>(sumG / count);
            tile[dstIndex + 2] = static_cast<std::uint8_t>(sumB / count);
            tile[dstIndex + 3] = static_cast<std::uint8_t>(sumA / count);
        }
    }
    return tile;
}

}  // namespace engine::editor

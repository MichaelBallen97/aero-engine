#pragma once
// SRC-PRIVATE (task E.4.5): the material CARD cache -- each material's sanitised document name and its
// base-colour swatch, keyed by ThumbnailKey so it invalidates on CONTENT exactly as the picture does.
// DEVICE-FREE by design, and that is the whole point: ThumbnailService services it ABOVE its device gate, so a
// machine with no GPU, and a build with no cooked shaders, still shows every material's name and its colour.
//
// WHY IT IS NOT IN THE ThumbnailLedger: a ledger exists to BOUND GPU MEMORY -- a clock, a resident cap, an LRU,
// a sticky rule, a superseded sweep -- and a card holds no GPU object. It borrows the SERVICE's frame counter,
// so there is still one clock in the editor, and keeps its own small LRU. A card is NOT released with its
// picture: the Inspector's Guid row wants a name while no tile is on screen.
//
// ITS OWN REQUEST QUEUE, deliberately not ThumbnailService::noteVisible: noteVisible is the LEDGER's touch
// list, and E.3.3's S34/I167 assert its exact meaning.
//
// IT READS ONLY WHAT IT MAY. Before any read, the key must be the record's CURRENT key and the record a
// RenderedMaterial: every decodable IMAGE has a key too, and a cache that read whatever it was asked for would
// read whole PNGs as text. The read is readFileBytes with the decode store's own cap. NOTHING HERE LOGS -- a
// refusal is an absent card -- except that parseMaterial, on a SUCCESSFUL parse of a hand-edited file, emits
// one WARN per unknown key: the parser's own contract, passed through unchanged.
//
// AND IT STORES ONLY WHAT ITS KEY NAMES (the code-review round). The gate trusts the DATABASE's key, and the
// database is stale from an external edit until the watcher's rescan -- so a read inside that window returns
// bytes the key does not name. The bytes read are therefore hashed (hashBytes, the scan's own MurmurHash3,
// one-shot where the scan streams) and a card is stored ONLY when that hash IS key.hash. The failures split
// by what decided them:
//   * decided by the key's OWN content -- bytes whose hash is key.hash and that do not parse -- is STICKY for
//     that key, like ThumbnailState's Failed: the same bytes will never parse;
//   * NOT decided by it -- a hash mismatch (the database is stale), an OS read failure (a file moved before
//     the rescan saw it), or a refusal by the size cap (it reads no byte, so it cannot prove whose content it
//     refused) -- is never cached and never retried before the NEXT RESCAN: the entry records
//     database.generation() at the attempt, and the key is read again only once that has changed.
#include <aero/editor/material_card.hpp>
#include <aero/editor/thumbnail_cache.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::editor {

class AssetDatabase;  // forward-declared: the .cpp includes <aero/editor/asset_database.hpp>

// A sub-2 KiB read plus a JSON parse. Four per tick empties a screenful of tiles in a handful of frames.
// STARTING VALUE, judged on the validation page.
inline constexpr std::size_t MAX_MATERIAL_CARD_READS_PER_TICK = 4;
// A grid shows at most ~200 tiles at the smallest tile size in a maximised panel; 512 covers a screenful plus
// scroll history at roughly a hundred bytes a card.
inline constexpr std::size_t MAX_MATERIAL_CARDS = 512;

class MaterialCardCache {
public:
    // A real parameter of a cache, not a test seam: ThumbnailService passes nothing and gets 512.
    explicit MaterialCardCache(std::size_t capacityIn = MAX_MATERIAL_CARDS) noexcept;

    // DRAW-WALK SAFE, both. noteCardWanted appends to a per-frame scratch that service() drains; cardFor is a
    // const lookup that answers nullptr until the read has happened, and for a key whose read failed.
    // A NON-NULL POINTER STAYS VALID UNTIL THE NEXT service() OR clear() -- neither runs inside a draw walk -- so
    // a host may hold it, and a view into its name, for the whole walk.
    void noteCardWanted(const ThumbnailKey& key);
    [[nodiscard]] const MaterialCard* cardFor(const ThumbnailKey& key) const noexcept;

    // THE ONLY MUTATOR, called from ThumbnailService::service ABOVE the device gate. Touches every wanted key it
    // already knows at `frame`; reads at most MAX_MATERIAL_CARD_READS_PER_TICK wanted keys it does not -- or
    // whose last attempt failed for a reason a rescan since then may have answered; then evicts down to
    // capacity, least-recently-wanted first, never a key wanted at `frame`.
    void service(const AssetDatabase& database, std::uint64_t frame);
    void clear() noexcept;

    // Black-box observability, ThumbnailStore::loadAttempts' precedent. readCount() is MONOTONIC for this
    // object's lifetime -- clear() does not reset it -- and is what makes "a malformed .aeromat is read once,
    // ever" observable at all.
    [[nodiscard]] std::size_t readCount() const noexcept;
    [[nodiscard]] std::size_t cardCount() const noexcept;  // entries HOLDING a card; a failed read holds none

private:
    struct Entry {
        ThumbnailKey key;
        MaterialCard card;
        std::uint64_t lastWanted = 0;
        bool parsed = false;  // TRUE == `card` holds the card of the bytes `key` names
        // The code-review round, APPENDED: TRUE == no card, and the failure was NOT decided by this key's
        // content (a hash mismatch, an OS failure, a cap refusal). Read again only once database.generation()
        // differs from `attemptGeneration`. FALSE with `parsed` FALSE == the key's own bytes do not parse: STICKY.
        bool retryAfterRescan = false;
        std::uint64_t attemptGeneration = 0;
    };
    void evictDownToCapacity(std::uint64_t frame);

    std::vector<Entry> entries;        // SORTED by ThumbnailKey -- the ledger's container, for its reason
    std::vector<ThumbnailKey> wanted;  // per-frame scratch, drained by service()
    std::size_t capacity = MAX_MATERIAL_CARDS;
    std::size_t reads = 0;
};

}  // namespace engine::editor

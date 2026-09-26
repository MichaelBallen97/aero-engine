#pragma once
// Aero Engine — the thumbnail policy layer (task 3.1.3). PUBLIC, pure, GPU-free and stb-free: the
// key, the state machine, the budget/LRU ledger, and a deterministic INTEGER box resampler. Nothing
// here touches the GPU or stb_image -- that is `ThumbnailStore`'s job (editor/src/thumbnail_store.hpp,
// src-private). NOTHING HERE LOGS (INV-V8).
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/asset_cache.hpp>  // task E.3.3 -- ImportChange
#include <aero/editor/asset_meta.hpp>   // task E.3.3 -- AssetRecord, AssetMetaState
#include <aero/editor/asset_view.hpp>   // task E.3.3 -- isThumbnailDecodable

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>  // task E.4.5 -- thumbnailSourceForName's parameter
#include <vector>

namespace engine::editor {

struct ThumbnailKey {
    Guid guid;
    ContentHash hash;
    [[nodiscard]] constexpr bool operator==(const ThumbnailKey&) const noexcept = default;
    // A10: the sorted-vector ledger and store both need an ordering. Guid::operator< (guid.hpp:40)
    // and ContentHash::operator< (content_hash.hpp:45) already exist; this is their lexicographic
    // composition and nothing more.
    [[nodiscard]] constexpr bool operator<(const ThumbnailKey& other) const noexcept {
        if (guid < other.guid) {
            return true;
        }
        if (other.guid < guid) {
            return false;
        }
        return hash < other.hash;
    }
};

enum class ThumbnailState : std::uint8_t {
    Absent = 0,  // never attempted -- the ONLY state nextDecodes returns
    Ready,       // a texture exists for this key
    Failed,      // decode or read failed -- STICKY (D9)
    Skipped,     // refused by a size/pixel bound -- STICKY (D9)
};

inline constexpr std::size_t MAX_THUMBNAIL_DECODES_PER_TICK = 2;
inline constexpr std::uint32_t THUMBNAIL_EDGE_TEXELS = 128;
inline constexpr std::uint64_t MAX_THUMBNAIL_SOURCE_BYTES = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t MAX_THUMBNAIL_SOURCE_PIXELS = 64ULL * 1000ULL * 1000ULL;
inline constexpr std::size_t MAX_THUMBNAILS_RESIDENT = 256;  // ~16 MiB at 128x128 RGBA8
// task E.4.5: a material RENDER is not an image DECODE and must not spend a decode's budget -- it reads an
// .aeromat, loads up to five source textures, pushes a material and records a sky, a forward and a resolve
// pass. ThumbnailService::service walks EVERY Absent key once and spends each budget as it meets a
// candidate of its own kind, so neither producer can starve the other (see that function). A rendered
// thumbnail is one 128x128 RGBA8 texture -- exactly a decoded one's cost -- so the cap above is unchanged.
// STARTING VALUE, judged on the validation page.
inline constexpr std::size_t MAX_THUMBNAIL_RENDERS_PER_TICK = 1;

class ThumbnailLedger {
public:
    void touch(const ThumbnailKey& key, std::uint64_t frame);  // inserts as Absent if new
    [[nodiscard]] ThumbnailState stateOf(const ThumbnailKey& key) const noexcept;
    void markReady(const ThumbnailKey& key);
    void markFailed(const ThumbnailKey& key);
    void markSkipped(const ThumbnailKey& key);

    // The keys to decode THIS tick: Absent ONLY, oldest-touched first, at most `budget`. It NEVER
    // returns a Failed or Skipped key -- D9's stickiness is enforced HERE, not at the call site
    // where it could be forgotten. budget == 0 returns empty.
    [[nodiscard]] std::vector<ThumbnailKey> nextDecodes(std::size_t budget) const;

    // Ready keys BEYOND `residentCap`, least-recently-touched first, EXCLUDING anything touched at
    // `currentFrame` -- evicting a tile that is on screen right now would decode it again next tick,
    // forever, in any folder with more visible tiles than the cap (E12).
    [[nodiscard]] std::vector<ThumbnailKey> evictions(std::size_t residentCap, std::uint64_t currentFrame) const;

    // task 3.1.4 (D9): every TRACKED key -- ANY state, including Absent, Failed and Skipped -- that
    // no longer corresponds to a live record, EXCLUDING two classes:
    //   * a key whose guid appears in `abstainingGuids`. A record whose content hash was not computed
    //     this scan (ImportChange::NotHashed / Unhashable, or metaWriteFailed) has NO OPINION about
    //     its own keys; without this carve-out, a project past the hash budget would release and
    //     re-decode the same thumbnails on every scan, forever.
    //   * a key touched at `currentFrame`. This is evictions()'s own E12 rule, applied here for the
    //     IDENTICAL reason and NOT as defensive padding: it makes the caller STRUCTURALLY unable to
    //     destroy a texture whose native SDL_GPUTexture* this frame's drawTile already wrote into the
    //     ImGui draw list. That is 3.1.3's BLOCKING-1 -- deterministic on Vulkan/D3D12 (SDL frees
    //     synchronously) and structurally unreachable on Metal (SDL defers), i.e. invisible to the
    //     only platform with a human pass.
    // Returning Failed/Skipped keys too is deliberate: D9's stickiness must be released along with
    // the texture, or a file that failed once under an old hash stays un-retryable under a new one.
    // BOTH spans MUST be sorted -- `liveKeys` by ThumbnailKey::operator<, `abstainingGuids` by
    // Guid::operator< -- and that is a PRECONDITION, not a checked contract. Returns keys in the
    // ledger's own sorted order.
    [[nodiscard]] std::vector<ThumbnailKey> supersededBy(std::span<const ThumbnailKey> liveKeys,
                                                         std::span<const Guid> abstainingGuids,
                                                         std::uint64_t currentFrame) const;

    void forget(const ThumbnailKey& key);
    void clear() noexcept;
    [[nodiscard]] std::size_t readyCount() const noexcept;
    [[nodiscard]] std::size_t unavailableCount() const noexcept;  // Failed + Skipped, for the footer
    // task E.4.5's code-review round, APPENDED: the keys still waiting for a producer. What makes "an off-screen
    // material key does not linger" observable at all -- the walk releases one rather than keep it pending.
    [[nodiscard]] std::size_t absentCount() const noexcept;

private:
    // Shared by markReady/markFailed/markSkipped: a no-op for a key never touched (see the .cpp).
    void setState(const ThumbnailKey& key, ThumbnailState state) noexcept;

    struct Entry {
        ThumbnailKey key;
        ThumbnailState state = ThumbnailState::Absent;
        std::uint64_t lastTouched = 0;
    };
    // A SORTED VECTOR keyed by (guid, hash) -- NOT std::unordered_map. MSVC's unordered_map move
    // CONSTRUCTOR is not noexcept (measured in CI at task 3.1.1: C2607), and this ledger is a member
    // of a panel a future refactor could make a value. asset_database.hpp's precedent (recordList) is
    // the reason; following it costs one std::lower_bound and buys a member that is unconditionally
    // nothrow-movable on all three standard libraries.
    std::vector<Entry> entries;
};

// task E.4.5: WHICH PRODUCER OWNS A KEY. A TOTAL enumeration, used to ROUTE at exactly two sites --
// thumbnailKeyForRecord's guard 2 below, and ThumbnailService::service's produce walk -- so a THIRD producer
// is a third enumerator and a -Wswitch diagnostic at the walk's switch -- a WARNING, on by default in clang,
// and never an error: no lane builds with -Werror, and .clang-tidy's `-*` check list does not re-enable
// clang-diagnostic-*, so clang-tidy suppresses it (measured in the code-review round) -- never an `||` clause
// somebody forgets at one site (3.7.3's standing rule: "if a guard ever needs a second arm for a second
// spelling of one predicate, stop and invert it"). MaterialCardCache reads it a third time as a GATE -- a
// card is only ever read for a RenderedMaterial key -- and never routes on it.
//
// IT COMPOSES, IT NEVER RESTATES. DecodedImage is isThumbnailDecodable's answer -- 3.1.3's deliberately
// SEPARATE stb-readable subset, which is what keeps .ktx2 and .dds on the icon path; RenderedMaterial is
// classifyAssetKind's, the one table that owns ".aeromat". No extension literal appears in its body.
// DecodedImage IS TESTED FIRST, and the order is not arbitrary: an extension that ever appeared in both
// tables must keep today's behaviour, and today's is the decode path.
enum class ThumbnailSource : std::uint8_t {
    None = 0,          // no producer: .ktx2, .dds, a model, audio, text, anything unclassified
    DecodedImage,      // task 3.1.3's stb chain -- ThumbnailStore
    RenderedMaterial,  // task E.4.5's render chain -- MaterialThumbnailRenderer
};
[[nodiscard]] ThumbnailSource thumbnailSourceForName(std::string_view fileName) noexcept;

// task E.3.3: FIVE of the browser's seven guards, as a pure function over a RECORD -- because a picker
// CANDIDATE is a record, not a (FileEntry, path) pair. The browser keeps guards 1 (a folder is never a
// candidate) and 3 (no database) and composes the rest through this.
//
// ORDER MATTERS AND IS PRESERVED: metaWriteFailed is checked BEFORE `change`, because phase 8 never
// assigns such a record a `change` at all, so it reads as the default UpToDate to any test on `change`
// alone (asset_meta.hpp's own note at the field, and 3.4.2's finding 3).
//
//   nullopt when: no producer owns the leaf (thumbnailSourceForName answers None -- .ktx2/.dds are Texture
//                 but get an ICON; task E.4.5 made .aeromat a RenderedMaterial), state == Invalid,
//                 metaWriteFailed, or change is NotHashed / Unhashable.
//
// An all-zero contentHash is the EMPTY FILE's real digest and never a sentinel (3.1.2's A4), so the
// ONLY "was this hashed?" test is the `change` enum. NOTHING HERE LOGS (INV-V8): a refusal is nullopt
// and nothing else.
[[nodiscard]] std::optional<ThumbnailKey> thumbnailKeyForRecord(const AssetRecord& record) noexcept;

// ---- the resampler --------------------------------------------------------------------------------
// Fits `src` (RGBA8, tightly packed, srcW*srcH*4 bytes) into an `edge` x `edge` RGBA8 tile,
// PRESERVING ASPECT and letterboxing the remainder with TRANSPARENT BLACK (0,0,0,0).
// NEVER UPSCALES: scale = min(1, edge/srcW, edge/srcH), so a source smaller than the tile in both
// axes is centred at 1:1. Both destination dimensions are std::max(1u, ...) -- a zero-height
// destination is a division by zero (E6).
// Downscaling is a box filter over the exact source rectangle each destination texel covers, summing
// in std::uint64_t (A11) and dividing by the sample count. NO FLOATING POINT ANYWHERE -- that is what
// makes the output byte-identical on macOS, Windows and Linux and therefore assertable in a test.
// Returns exactly edge*edge*4 bytes, or an EMPTY vector when edge == 0, srcW == 0, srcH == 0, or
// src.size() < srcW*srcH*4.
[[nodiscard]] std::vector<std::uint8_t> fitRgbaIntoTile(std::span<const std::uint8_t> src, std::uint32_t srcW,
                                                        std::uint32_t srcH, std::uint32_t edge);

}  // namespace engine::editor

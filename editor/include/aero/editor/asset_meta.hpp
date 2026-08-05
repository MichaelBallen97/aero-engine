#pragma once
// Aero Engine — the .meta v1 format and the pure asset-identity lifecycle planner (task 3.1.1).
// PUBLIC, and the project.hpp shape verbatim: free of ImGui, SDL, entt and <filesystem>; free of
// every build gate (D4/AC-17/INV-P5, project.hpp's precedent, a second application). NOTHING HERE
// LOGS (INV-A3) -- status is RETURNED, never printed (project_files.hpp:15-16's convention, a
// fourth application).
//
// planAssetMetas is the single most important function in this task. It touches no disk, no clock
// and no global state, and every lifecycle rule (D5-D9: create on discovery, never rewrite a valid
// sidecar, never overwrite an invalid one, never delete an orphan, repair a duplicate
// deterministically) is provable from a std::vector literal and a fixed seed.
#include <aero/core/guid.hpp>
#include <aero/editor/asset_cache.hpp>  // task 3.1.2: ImportChange, ContentHash. ONE WAY ONLY -- asset_cache.hpp
                                        // must NEVER include this file (docs/09 §6.9 / plan A20).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

inline constexpr int ASSET_META_FORMAT_VERSION = 1;
inline constexpr std::string_view ASSET_META_SUFFIX = ".meta";
inline constexpr std::string_view ATOMIC_TEMP_SUFFIX = ".aero-tmp";  // D16
inline constexpr std::size_t MAX_ASSETS = 50000;                     // D14
inline constexpr std::size_t MAX_REPORTED_PER_CATEGORY = 20;
inline constexpr std::size_t CREATE_NOTICE_THRESHOLD = 1000;

// ---- naming --------------------------------------------------------------------------------------

// "wood.png" -> "wood.png.meta". Appends to the FULL file name, never replaces an extension -- so
// "wood.png" and "wood.jpg" never collide on their sidecar name (AC-18).
[[nodiscard]] std::string metaFileNameFor(std::string_view assetFileName);

// True iff `fileName` is longer than the suffix and its last 5 bytes ASCII-fold to ".meta" (AC-19).
// Case-INSENSITIVE on the suffix only -- ".meta" alone (5 bytes, not > 5) is NOT a sidecar name; the
// base-name half of a pairing is compared byte for byte elsewhere (asset_database.cpp).
[[nodiscard]] bool isMetaFileName(std::string_view fileName) noexcept;

// "wood.png.meta" -> "wood.png"; "" for a name that is not a sidecar (AC-20). POINTS INTO `metaName`
// -- never call it on a temporary whose lifetime ends before the result is used.
[[nodiscard]] std::string_view assetNameForMeta(std::string_view metaName) noexcept;

// D5/AC-21: is `fileName` a file the scan should mint an identity for? Rejects a hidden name, a
// sidecar itself, anything ending ATOMIC_TEMP_SUFFIX (a suffix test, not equality -- E19), and the
// two literal OS-noise names `Thumbs.db` / `desktop.ini` (A15's "two literal names plus the
// .aero-tmp suffix"; `.DS_Store` needs no entry, it is dot-prefixed and isHiddenName already removes
// it). Directories are never scannable asset names either -- the caller filters by isDirectory first.
[[nodiscard]] bool isScannableAssetName(std::string_view fileName) noexcept;

// ---- the format -----------------------------------------------------------------------------------

enum class MetaError : std::uint8_t {
    None = 0,
    BadJson,
    NotAnObject,
    BadVersion,
    UnsupportedVersion,
    MissingGuid,
    BadGuidKind,
    BadGuidText,
    NilGuid,
};

struct MetaParseResult {
    std::optional<Guid> guid;  // engaged == success
    MetaError error = MetaError::None;
    std::string message;     // the exact docs/09 §5.4 text; "" iff `guid` is engaged
    std::uint32_t line = 0;  // > 0 ONLY for a JSON-stage failure
    std::uint32_t column = 0;
    std::vector<std::string> unknownKeys;  // AC-14: WARNed by the CALLER, never here
};
[[nodiscard]] MetaParseResult parseMeta(std::string_view text);
[[nodiscard]] std::string writeMetaText(Guid guid);  // canonical, exactly one trailing '\n'

// ---- the lifecycle, as a PURE function (D5-D9) ------------------------------------------------

// `Reattached` is APPENDED, never inserted (task 3.1.2, D13): AssetMetaState is serialized nowhere
// (in-memory scan state only), so appending is safe; inserting would silently renumber `Invalid`,
// which several tests compare by value.
enum class AssetMetaState : std::uint8_t { Ok = 0, Created, Repaired, Invalid, Reattached };

struct AssetRecord {
    std::string relativePath;  // '/'-separated, relative to the assets root
    Guid guid;                 // nil iff state == Invalid
    AssetMetaState state = AssetMetaState::Ok;
    // ---- task 3.1.2, additive ----
    ContentHash contentHash;  // MEANINGLESS unless `change` is neither Unhashable nor NotHashed --
                              // all-zero is the EMPTY FILE's real digest, not a sentinel (plan A4).
    ImportChange change = ImportChange::UpToDate;  // filled by AssetDatabase, not by planAssetMetas
    // Code-review finding 3: true iff phase 7's writeTextFileAtomic for THIS record's sidecar failed
    // this scan (state still Created/Repaired/Reattached -- the write failure does not change it, D7's
    // "kept its in-memory GUID" posture). Phase 8 never assigns such a record a `change` at all (its
    // bytes on disk are not what would have been hashed), so `change` stays its default UpToDate --
    // every READER of `change` (the accessor, the footer) must check this flag first, or a failed write
    // silently reads as "up to date" for a file with no sidecar and no cache entry.
    bool metaWriteFailed = false;
};

struct AssetPlanEntry {
    std::string relativePath;
    std::optional<Guid> guid;  // nullopt == no sidecar, OR one that failed to parse
    bool metaPresent = false;
    std::optional<Guid> reattachedGuid;  // task 3.1.2 (D13). Set ONLY by asset_database.cpp's phase 5.
};

struct AssetPlanResult {
    std::vector<AssetRecord> records;       // sorted byte-lexicographically by relativePath
    std::vector<std::size_t> writeIndices;  // indices into `records` whose .meta must be WRITTEN
    std::size_t created = 0;
    std::size_t repaired = 0;
    std::size_t invalid = 0;
    std::size_t reattached = 0;  // task 3.1.2
};

// THE WHOLE OF D5-D9, as a pure function of (entries, seed). Sorts first (AC-22), so the result is
// independent of the walk order, of the OS, and of entryOrderLess's case folding.
[[nodiscard]] AssetPlanResult planAssetMetas(std::vector<AssetPlanEntry> entries, GuidGenerator& generator);

}  // namespace engine::editor

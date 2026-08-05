#pragma once
// Aero Engine — the asset import cache index v1 format, and the pure change-detection cascade
// (task 3.1.2). PUBLIC, and the .meta format header's shape verbatim (task 3.1.1): free of ImGui,
// SDL, entt, <filesystem>, <fstream> and every build gate (D4/AC-17/INV-P5, project.hpp's precedent,
// a third application). NOTHING HERE LOGS (INV-A3/INV-C7, a sixth application) -- status is
// RETURNED, never printed.
//
// THIS FILE NAMES NO OTHER EDITOR HEADER AT ALL (docs/09 §6.9 / plan A20) -- the dependency runs the
// other way: the .meta format's own header includes THIS one (from Step 7 onward) to give
// AssetRecord its two new fields. Getting the direction backwards would make the .meta format (task
// 3.1.1, a settled, committed format) depend on the cache (this task's disposable, machine-local
// one), which is precisely backwards from their lifetimes.
//
// Step 4 landed the format half: parseAssetCache/writeAssetCacheText/importChangeLabel, the format
// types, and the constants. Step 6 defined planImports and commitImports. Step 7 (this revision)
// defines planReattachments -- every function this header declares is now defined.
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

inline constexpr int ASSET_CACHE_FORMAT_VERSION = 1;
inline constexpr std::string_view ASSET_CACHE_DIR_NAME = "Library";
inline constexpr std::string_view ASSET_CACHE_FILE_NAME = "asset-cache.json";
inline constexpr std::string_view ASSET_CACHE_GITIGNORE_NAME = ".gitignore";
inline constexpr std::string_view LIBRARY_GITIGNORE_TEXT =
    "# Aero Engine derived data (task 3.1.2) -- machine-local import cache. Never commit this directory.\n"
    "*\n";
inline constexpr std::size_t MAX_CACHE_ENTRIES = 200000;  // 4x MAX_ASSETS (D10)
inline constexpr std::size_t MAX_DEPENDENCIES_PER_ENTRY = 1024;
inline constexpr std::size_t MAX_ORPHANS_READ = 256;
inline constexpr std::uint32_t MISSING_SCAN_GRACE = 3;  // D14
inline constexpr std::uint64_t MAX_HASH_BYTES_PER_SCAN = 2ULL * 1024 * 1024 * 1024;
inline constexpr std::uint64_t HASH_NOTICE_THRESHOLD_BYTES = 64ULL * 1024 * 1024;

// ---- the format ------------------------------------------------------------------------------------

struct AssetCacheEntry {
    Guid guid;         // the KEY (D11) -- never nil (INV-C1)
    std::string path;  // informational; a move updates it without invalidating the entry
    std::uint64_t size = 0;
    std::int64_t mtime = 0;   // OPAQUE, machine-local file_time_type ticks (docs/09 §6.5). Never a date.
    ContentHash contentHash;  // MAY be all-zero: that is the empty file's digest (plan A4)
    ContentHash metaHash;     // the digest of the WHOLE sidecar (D4); may likewise be zero
    std::string importer;     // "" until 3.2 registers one
    std::uint32_t importerVersion = 0;
    std::vector<Guid> dependencies;  // D12; produced by nobody in this task
    std::uint32_t missing = 0;       // D14
};

struct AssetCacheIndex {
    std::vector<AssetCacheEntry> entries;                                 // sorted by guid (AC-18)
    [[nodiscard]] const AssetCacheEntry* find(Guid guid) const noexcept;  // std::lower_bound
};

enum class CacheLoadOutcome : std::uint8_t { Ok = 0, Absent, Discarded };

struct AssetCacheParseResult {
    AssetCacheIndex index;
    CacheLoadOutcome outcome = CacheLoadOutcome::Ok;
    std::string discardReason;            // "" unless Discarded; the exact docs/09 §6.9 text
    std::size_t droppedEntries = 0;       // malformed or duplicate (AC-16)
    std::size_t droppedDependencies = 0;  // MAX_DEPENDENCIES_PER_ENTRY excess (E25)
    bool truncated = false;               // MAX_CACHE_ENTRIES
};
// `maxEntries` is a DEFAULTED PARAMETER, not a test-only seam and not a mutable constant -- plan A9's
// decision, applied a second time (AssetDatabase::rescan's `hashBudgetBytes` is the first). Production
// calls this with the default, so the REAL MAX_CACHE_ENTRIES is what every scan exercises and the
// constant stays pinned at its own declaration above; a test passes a small value to reach the
// truncation branch in microseconds instead of the ~10.8s a 200 001-entry document costs under ASan.
// A test that passes a cap is testing the CAP MECHANISM, which is exactly the thing under test.
[[nodiscard]] AssetCacheParseResult parseAssetCache(std::string_view text, std::size_t maxEntries = MAX_CACHE_ENTRIES);
[[nodiscard]] std::string writeAssetCacheText(const AssetCacheIndex& index);  // canonical, one '\n'

// ---- change detection + the cascade, as a PURE function (D12) --------------------------------------
// ORDER IS PRECEDENCE (AC-22) for the OWN-CAUSE pass only (planImports step 2: New, then
// SourceChanged, then MetaChanged, then ImporterChanged, else UpToDate) -- the FIRST of those four
// that applies is reported. Unhashable/NotHashed are decided BEFORE that pass even runs (whichever
// applies wins outright, with no previous-entry comparison at all), and DependencyChanged is decided
// in a SEPARATE, LATER pass (step 3/4's worklist) that only ever overwrites an entry still UpToDate --
// so Unhashable and NotHashed both win over DependencyChanged in practice, the reverse of this enum's
// own declaration order. Corrected here (code-review finding 5) to match the implementation, which is
// the more useful behaviour and is not being changed: a source that could not be read this scan is a
// more actionable fact than "some dependency, possibly unrelated, changed," and is worth keeping.
enum class ImportChange : std::uint8_t {
    UpToDate = 0,
    New,                // no entry for this GUID
    SourceChanged,      // contentHash differs
    MetaChanged,        // metaHash differs -- identity repaired, or import settings edited (D4)
    ImporterChanged,    // importer id / version differ from what the cache recorded (D16)
    DependencyChanged,  // a TRANSITIVE dependency is dirty, or gone (D12) -- loses to Unhashable/NotHashed
    Unhashable,         // the source could not be read this scan -- wins over DependencyChanged
    NotHashed,          // the per-scan hash budget was exhausted before reaching it (D10) -- ditto
};
// The footer/log text. A switch with NO default:, so a future enumerator is a -Wswitch warning rather
// than a silent "unknown" (logAssetScan's ScanStatus switch is the precedent).
[[nodiscard]] std::string_view importChangeLabel(ImportChange change) noexcept;

struct ImportInput {
    Guid guid;
    std::string relativePath;
    std::optional<ContentHash> contentHash;  // nullopt == Unhashable or NotHashed. ENGAGEMENT, never
                                             // .valid(), is the "was it hashed?" test (plan A4).
    ContentHash metaHash;
    std::uint64_t size = 0;  // plan A19: commitImports records what was OBSERVED
    std::int64_t mtime = 0;  // plan A19
    std::string importer;    // "" in this build
    std::uint32_t importerVersion = 0;
    bool hashSkippedByBudget = false;  // discriminates NotHashed from Unhashable
};

struct ImportPlanEntry {
    Guid guid;
    std::string relativePath;
    ImportChange change = ImportChange::UpToDate;
};

struct ImportPlanResult {
    std::vector<ImportPlanEntry> entries;  // sorted byte-lexicographically by relativePath
    std::vector<std::size_t> jobIndices;   // entries that are NOT UpToDate, in that order
    // `newAssets`, NOT `created`: AssetScanReport::created already means ".meta files written", and
    // two different "created" counts in one report is a defect waiting to be logged.
    std::size_t upToDate = 0, newAssets = 0, changed = 0, dependencyChanged = 0;
    std::size_t unhashable = 0, notHashed = 0;
};
[[nodiscard]] ImportPlanResult planImports(std::vector<ImportInput> inputs, const AssetCacheIndex& previous);

// ---- the commit, as a PURE function (D14/D16) -------------------------------------------------------
[[nodiscard]] AssetCacheIndex commitImports(const AssetCacheIndex& previous, const std::vector<ImportInput>& inputs,
                                            const ImportPlanResult& plan);

// ---- orphan re-attachment, as a PURE function (D13) --------------------------------------------------
struct ReattachCandidate {
    std::string relativePath;
    ContentHash contentHash;
};
struct OrphanMeta {  // a PARSED orphan sidecar
    std::string relativePath;
    Guid guid;
};
struct ReattachMatch {
    std::size_t candidateIndex = 0;
    Guid guid;
    std::string fromMetaPath;   // the orphan sidecar that supplied the identity, for the WARN
    std::string fromAssetPath;  // the absent cache entry's recorded path, for the WARN
};
// ALL FIVE of D13's conditions, or nothing. `liveGuids` is every GUID a live sidecar already claims;
// `livePaths` is every asset path this scan saw.
[[nodiscard]] std::vector<ReattachMatch> planReattachments(const std::vector<ReattachCandidate>& candidates,
                                                           const std::vector<OrphanMeta>& orphans,
                                                           const AssetCacheIndex& previous,
                                                           const std::vector<Guid>& liveGuids,
                                                           const std::vector<std::string>& livePaths);

}  // namespace engine::editor

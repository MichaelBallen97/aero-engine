#pragma once
// Aero Engine — AssetDatabase: the scan over a project's assets root (task 3.1.1), now with the
// import cache and dependency tracking woven in (task 3.1.2). PUBLIC, and free of ImGui, SDL, entt
// and every build gate (D4, project.hpp's precedent). Still `<filesystem>`-free by file placement --
// all disk access lives in asset_database.cpp, which composes 2.2.4's listDirectory, 2.5.1/2.6.1's
// text_file and 3.1.2's asset_cache rather than touching std::filesystem directly.
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/project_files.hpp>  // ScanStatus -- reused, never redeclared

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace engine::editor {

struct AssetScanReport {
    ScanStatus status = ScanStatus::Ok;  // the ROOT's status; Missing when no project is open
    std::size_t filesSeen = 0;
    std::size_t created = 0;
    std::size_t repaired = 0;
    std::size_t invalid = 0;
    std::vector<std::string> invalidPaths;  // capped; "<path>" or "<path>: <reason>"
    std::vector<std::string> orphans;       // capped -- task 3.1.2's phase 5 REBUILDS this from
                                            // allOrphanPaths minus consumed re-attachments (§6.8)
    std::size_t orphanTotal = 0;
    std::vector<std::string> repairs;        // A9. capped; "<path>: <old guid> -> <new guid>"
    std::vector<std::string> writeFailures;  // capped; "<path>: <os reason>"
    std::size_t writeFailureTotal = 0;
    // Code-review finding 2: a Created/Repaired/Reattached write that would land on a path phase 3
    // already classified as an orphan (compared ASCII-case-insensitively) is refused rather than
    // performed -- on a case-insensitive filesystem that write would silently destroy the orphan's
    // real, committed identity. The record involved is downgraded to AssetMetaState::Invalid with a
    // nil guid (D7's own posture: no identity this session) and counted in `invalid`, not in
    // `created`/`repaired`; capped; "<path>: collides with orphan '<meta path>'".
    std::vector<std::string> writeConflicts;
    std::size_t writeConflictTotal = 0;
    std::vector<std::string> unknownKeyWarnings;  // capped
    std::size_t unknownKeyTotal = 0;              // A9
    std::size_t skippedEntries = 0;               // A10 -- listDirectory::skipped, summed
    std::size_t unreadableDirs = 0;               // A10 -- a non-Ok listing BELOW the root
    bool truncated = false;                       // MAX_ASSETS or a listDirectory cap
    bool depthLimited = false;                    // MAX_TREE_DEPTH
    bool largeCreateNotice = false;               // A6 -- writeIndices > CREATE_NOTICE_THRESHOLD

    // ---- task 3.1.2: the import cache -------------------------------------------------------------
    // cache
    std::size_t cacheEntriesLoaded = 0;
    std::size_t cacheEntriesDropped = 0;
    std::size_t cacheDepsDropped = 0;
    std::string cacheDiscardReason;  // "" unless the index was discarded whole (docs/09 §6.9's text)
    bool cacheTruncated = false;
    bool cacheWritten = false;    // D15: TRUE only when the text actually differed AND landed
    std::string cacheWriteError;  // "" == nothing went wrong; E33/E34
    // hashing
    std::size_t hashed = 0;  // files read+hashed this scan
    std::uint64_t hashedBytes = 0;
    std::size_t fastPathHits = 0;  // (size,mtime) matched -> ZERO bytes read
    bool hashBudgetExhausted = false;
    bool largeHashNotice = false;
    // the import plan, folded for the log line (the per-asset reason lives on AssetRecord::change)
    std::size_t upToDate = 0;
    std::size_t newAssets = 0;
    std::size_t changed = 0;
    std::size_t dependencyChanged = 0;
    std::size_t unhashable = 0;
    std::size_t notHashed = 0;
    // three new capped categories, each with its own uncapped total
    std::vector<std::string> reattachments;
    std::size_t reattachmentTotal = 0;
    std::vector<std::string> hashFailures;
    std::size_t hashFailureTotal = 0;
    std::vector<std::string> aliasedDirs;
    std::size_t aliasedDirTotal = 0;
};

class AssetDatabase {
public:
    [[nodiscard]] const std::string& root() const noexcept;         // the ASSETS root (unchanged)
    [[nodiscard]] const std::string& projectRoot() const noexcept;  // task 3.1.2

    // The ONE mutating entry point. Never throws, never logs (INV-A3). EITHER root empty clears the
    // database and returns ScanStatus::Missing with zero writes and the cache file UNTOUCHED.
    // `hashBudgetBytes` defaults to MAX_HASH_BYTES_PER_SCAN (plan A9): a DEFAULTED PARAMETER, not a
    // test-only seam -- production exercises the real constant, which stays pinned at its own
    // declaration. listDirectory(root, rel, includeHidden) is the same shape.
    AssetScanReport rescan(std::string projectRootUtf8, std::string assetsRootUtf8, GuidGenerator& generator,
                           std::uint64_t hashBudgetBytes = MAX_HASH_BYTES_PER_SCAN);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const AssetRecord* findByPath(std::string_view relativePath) const noexcept;
    [[nodiscard]] const AssetRecord* findByGuid(Guid guid) const noexcept;  // nullptr for nil
    [[nodiscard]] std::optional<Guid> guidForPath(std::string_view relativePath) const noexcept;

    // task 3.1.3 (D5): THE SEARCH INDEX. The vector is already sorted byte-lexicographically by
    // relativePath and already excludes sidecars, hidden names and .aero-tmp leftovers, so a caller
    // needs nothing but a view of it. Invalid records ARE included: they carry a nil guid and
    // therefore no thumbnail key, but a user searching for a file must still find it. The span is
    // valid until the next rescan(), exactly like every AssetRecord* the other accessors return.
    [[nodiscard]] std::span<const AssetRecord> records() const noexcept;

    // ---- task 3.1.2 --------------------------------------------------------------------------------
    // Clears the in-memory index and arms the one-shot below, so the NEXT scan skips phase 3's reload
    // (AC-35). It also clears `cacheTextOnDisk`, but defensively, not load-bearingly: phase 3 clears
    // that field unconditionally anyway. The .cpp says so at the line itself.
    void invalidateCache() noexcept;
    [[nodiscard]] std::size_t cacheSize() const noexcept;
    [[nodiscard]] const ImportPlanResult& importPlan() const noexcept;  // 3.2's seam (D16)

private:
    std::string rootUtf8;
    std::string projectRootUtf8;  // task 3.1.2 -- NEVER derived from rootUtf8 (AC-38, seed S27b):
                                  // <assetsRoot>/.. is wrong the moment paths.assets is nested or "."
    // task 3.1.3 (A18, deviation logged): named `recordList`, NOT `records` -- the plan's own §D-6
    // text says "one line in the .cpp (`return records;`)", but this member and the new public
    // `records()` accessor cannot share an identifier (a duplicate-member compile error, caught by
    // the very first build of this step). Renamed on the `databasePtr`/`database()` precedent
    // (3.1.1's D13 naming note): a data member and a member function never share a name.
    std::vector<AssetRecord> recordList;  // sorted by relativePath (planAssetMetas' own contract) --
                                          // findByPath is a std::lower_bound over THIS vector directly.
    // MSVC's std::unordered_map move CONSTRUCTOR is not noexcept (measured in CI: C2607 on the
    // aggregate static_assert below, libc++/libstdc++ both hold, MSVC's STL does not) -- the
    // documented fallback (plan A2 part 2), applied for real rather than staying theoretical. A
    // sorted index vector is unconditionally nothrow-movable on all three standard libraries: no
    // hash table, no bucket array, no allocator-equality question. Guid has operator< (AC-2), so
    // std::lower_bound applies here exactly as it does to `recordList` above. Invalid records ABSENT
    // (INV-A7); sorted by Guid, NOT by index -- rebuilt (sorted) once per rescan, not maintained
    // incrementally.
    std::vector<std::pair<Guid, std::size_t>> byGuid;
    AssetCacheIndex cache;        // task 3.1.2 -- the previous scan's committed index, in memory
    std::string cacheTextOnDisk;  // task 3.1.2 -- D15's comparand, held VERBATIM as read/written
    ImportPlanResult plan;        // task 3.1.2 -- 3.2's seam (importPlan())
    // Deviation from the plan's own D-8 phase-3 snippet, logged: that snippet reloads `cache` from
    // disk UNCONDITIONALLY on every scan, which would silently UNDO invalidateCache()'s clear before
    // phase 4 ever runs -- an on-disk file invalidateCache() never touches always reloads identically,
    // so AC-35/AD58's "every asset New, index rewritten" could never be observed. This one-shot flag
    // is what phase 3 consults to skip that ONE reload, consumed immediately (never left set across a
    // scan that could not run at all, e.g. a Missing root -- exactly like assetRescanRequested's own
    // survive-until-honored posture, EditorApp's precedent).
    bool cacheInvalidated = false;
};

// F10 (editor_app.hpp): EditorApp's move is `noexcept = default`, so every value member must be
// noexcept-movable. A2/command_stack.hpp:189-194's precedent: aggregate asserts FIRST, then
// per-member ones, so a future regression NAMES the culprit instead of failing an opaque aggregate.
// Ten in total (plan A17): the two AssetDatabase aggregate ones, then one pair per new member type.
static_assert(std::is_nothrow_move_constructible_v<AssetDatabase>);
static_assert(std::is_nothrow_move_assignable_v<AssetDatabase>);
static_assert(std::is_nothrow_move_constructible_v<std::vector<AssetRecord>>);
static_assert(std::is_nothrow_move_assignable_v<std::vector<AssetRecord>>);
static_assert(std::is_nothrow_move_constructible_v<std::vector<std::pair<Guid, std::size_t>>>);
static_assert(std::is_nothrow_move_assignable_v<std::vector<std::pair<Guid, std::size_t>>>);
static_assert(std::is_nothrow_move_constructible_v<AssetCacheIndex>);
static_assert(std::is_nothrow_move_assignable_v<AssetCacheIndex>);
static_assert(std::is_nothrow_move_constructible_v<ImportPlanResult>);
static_assert(std::is_nothrow_move_assignable_v<ImportPlanResult>);
// task 3.1.3 (A19): EditorApp gains its own `AssetScanReport lastAssetReport` value member, held to
// the SAME `noexcept = default` move requirement -- asserted here, beside AssetDatabase's own ten,
// because AssetScanReport is defined in this same header. 3.1.2's R9 fired for real on MSVC once
// already; the fallback if either ever reddens on a lane this machine cannot test is NOT to relax the
// assert, but to hold the report indirectly (3.1.1's BLOCKING-2 precedent).
static_assert(std::is_nothrow_move_constructible_v<AssetScanReport>);
static_assert(std::is_nothrow_move_assignable_v<AssetScanReport>);

}  // namespace engine::editor

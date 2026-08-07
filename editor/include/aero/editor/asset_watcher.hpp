#pragma once
// Aero Engine — the assets-tree change watcher (task 3.1.4). PUBLIC, and the asset_cache.hpp /
// asset_database.hpp shape verbatim: free of ImGui, SDL, entt, <filesystem>, <fstream>, <thread>,
// <mutex>, <atomic> and every build gate.
//
// NOTHING HERE LOGS (INV-A3, a seventh application). This runs EVERY TICK -- anything it printed
// would print every tick. Status is RETURNED and SURFACED, never printed; EditorApp emits the one
// INFO line, from editor_app.cpp and nowhere else (AC-37).
//
// NOTHING HERE WRITES, RENAMES, COPIES OR DELETES (D7/INV-W1). This file is strictly read-only, like
// project_files.hpp, and asset_watcher.cpp must NEVER be added to check-project-no-delete.sh's
// Check B PERMITTED_DELETERS allowlist -- being OUTSIDE that allowlist is precisely what makes a
// future std::filesystem::remove here a hard CI failure. A watcher that deletes anything is a bug by
// construction.
//
// All disk access is ONE function -- AssetWatcher::poll() -- and it composes listDirectory() and
// canonicalDirectory() and nothing else, exactly as asset_database.cpp does. It performs ZERO file
// reads: no .meta is parsed and no byte is hashed by anything in this file (INV-W2/AC-23). That is
// what makes it categorically cheaper than the rescan it triggers, which reads and parses one
// sidecar per asset -- the expensive operation is gated behind the cheap one.
//
// NO THREAD, NO ATOMIC, NO MUTEX (D1): poll() runs on the tick thread, advances a budgeted cursor,
// and returns. The editor owns no thread today and this task does not give it one.
#include <aero/editor/asset_meta.hpp>     // MAX_ASSETS, isWatchableAssetName
#include <aero/editor/project_files.hpp>  // FileEntry/DirectoryListing/ScanStatus, listDirectory,
                                          // canonicalDirectory, joinRelative, depthOf,
                                          // MAX_TREE_DEPTH, currentFileTimeTicks,
                                          // fileTimeTicksFromMillis

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace engine::editor {

// ---- tunables ------------------------------------------------------------------------------------
inline constexpr std::size_t WATCH_DIRS_PER_POLL = 8;    // D6 -- ~480 dirs/s at 60fps, ~160/s unfocused
inline constexpr std::int64_t WATCH_COOLDOWN_MS = 1000;  // D6 -- idle between completed sweeps
// D2 -- ABOVE FAT32's own 2 s mtime quantum, not merely "a bit". A stamp younger than this is never
// trusted, which is the ONLY thing that catches two same-size writes inside one coarse quantum.
inline constexpr std::int64_t WATCH_SETTLE_MS = 2000;
inline constexpr std::uint32_t MAX_DEFERRED_SWEEPS = 30;      // D3 -- ~30 s, then FORCE
inline constexpr std::size_t MAX_WATCH_ENTRIES = MAX_ASSETS;  // D4 -- the SCAN's own ceiling, reused
inline constexpr std::size_t MAX_WATCH_DIRS = 8192;           // BFS queue ceiling; surfaced, never silent

// ---- the snapshot --------------------------------------------------------------------------------
struct WatchEntry {
    // '/'-separated, relative to the ASSETS root; NEVER "" (the root itself is not an entry).
    std::string path;
    std::uint64_t size = 0;
    // OPAQUE file_time_type ticks (docs/09 §6.5). NEVER a date, and never compared to anything but
    // another tick value from the same clock (currentFileTimeTicks / fileTimeTicksFromMillis).
    std::int64_t mtime = 0;
    bool isDirectory = false;
    // sizeKnown && mtimeKnown, folded. FALSE means "this entry CANNOT BE COMPARED" -- a broken
    // symlink, or an entry the OS refused to classify. D2's third carve-out: never settled, ever.
    bool stampKnown = false;
};

struct WatchSnapshot {
    // Sorted BYTE-lexicographically by `path`, NEVER case-folded (INV-W10 -- 3.1.1 D9's determinism
    // rule, a second application). A case-only rename must be a real change on a case-sensitive
    // filesystem.
    std::vector<WatchEntry> entries;
    bool truncated = false;          // MAX_WATCH_ENTRIES, MAX_WATCH_DIRS, or a listDirectory cap
    bool depthLimited = false;       // MAX_TREE_DEPTH
    std::size_t unreadableDirs = 0;  // D5 -- listings BELOW the root that failed

    // std::lower_bound over `entries`; nullptr when absent. NEVER constructs a std::string from the
    // argument -- the AssetDatabase::findByPath idiom (asset_database.cpp:119-130) verbatim.
    [[nodiscard]] const WatchEntry* find(std::string_view path) const noexcept;
};

// ---- the diff ------------------------------------------------------------------------------------
enum class WatchChangeKind : std::uint8_t { Added = 0, Removed, Modified };

struct WatchChange {
    std::string path;
    WatchChangeKind kind = WatchChangeKind::Added;
    bool isDirectory = false;
};

struct WatchDiff {
    std::vector<WatchChange> changes;  // SETTLED changes only, sorted byte-lexicographically by path
    std::size_t unsettled = 0;         // changed, but not yet safe to act on (D2)
    [[nodiscard]] bool empty() const noexcept { return changes.empty(); }
};

// D2, as ONE function so no call site can implement half of it (INV-W4). `previous` is the entry as
// the PREVIOUS COMPLETED SWEEP saw it, or nullptr when there was none.
//   * a directory                     -> settled (its stamp is never read; there is no torn state)
//   * !current.stampKnown             -> NEVER settled (it cannot be compared and must not be guessed)
//   * previous == nullptr (an add)    -> settled iff the AGE test passes
//   * otherwise                       -> settled iff (size, mtime) == previous' AND the AGE test passes
// A removal has no `current` at all and is settled by construction -- diffSnapshots handles it
// directly rather than synthesising an entry to pass here.
//
// The two conditions each close a hole the other cannot, and this is the single most load-bearing
// rule in the task:
//   * AGE ALONE is wrong on Windows. NTFS does not flush last-write-time while a handle is open for
//     many write paths, so a 2 GB copy can present a STALE, OLD mtime for its whole duration and pass
//     the age test on a half-written file. STABILITY catches it, because the SIZE is still growing.
//   * STABILITY ALONE is wrong on coarse-mtime volumes. FAT32 (2 s) and HFS+ (1 s) quantise mtime, so
//     two same-size writes inside one quantum are indistinguishable: two consecutive sweeps read an
//     identical (size, mtime) while the bytes underneath differ. The AGE test refuses to trust any
//     stamp younger than settleTicks, which is why WATCH_SETTLE_MS is 2000.
[[nodiscard]] bool isSettled(const WatchEntry& current, const WatchEntry* previous, std::int64_t nowTicks,
                             std::int64_t settleTicks) noexcept;

// PURE. Walks `committed` and `probe` in lockstep (both sorted) and classifies every path.
// `lastProbe` supplies isSettled's `previous`. Removals and directories are settled outright;
// everything else goes through isSettled. `unsettled` counts candidates that did NOT settle and are
// therefore NOT emitted.
[[nodiscard]] WatchDiff diffSnapshots(const WatchSnapshot& committed, const WatchSnapshot& probe,
                                      const WatchSnapshot& lastProbe, std::int64_t nowTicks, std::int64_t settleTicks);

// PURE. `committed` with ONLY `diff.changes` applied, taking each changed path's entry from `probe`
// (or dropping it, for a Removed). D3's SELECTIVE commit: on a FORCED fire the unsettled entries'
// stamps must NOT enter `committed`, or a torn mid-write stamp becomes the baseline and the file's
// real final state is never noticed (INV-W5/AC-12). Flags come from `probe`. The result is sorted.
[[nodiscard]] WatchSnapshot applyChanges(const WatchSnapshot& committed, const WatchSnapshot& probe,
                                         const WatchDiff& diff);

// F11: EditorApp's move is `noexcept = default`, so every value member must be noexcept-movable.
// asset_database.hpp:148-168's precedent -- the AGGREGATE asserts come FIRST, so a regression NAMES
// this type rather than failing an opaque member's assert (AC-43). 3.1.2's R9 fired for real on MSVC
// once already; if either of the first two ever reddens, the fix is to remove the offending member
// TYPE, NEVER to relax the assert.
static_assert(std::is_nothrow_move_constructible_v<WatchSnapshot>);
static_assert(std::is_nothrow_move_assignable_v<WatchSnapshot>);
static_assert(std::is_nothrow_move_constructible_v<WatchDiff>);
static_assert(std::is_nothrow_move_assignable_v<WatchDiff>);

}  // namespace engine::editor

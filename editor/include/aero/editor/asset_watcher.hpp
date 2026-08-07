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
inline constexpr std::uint32_t MAX_DEFERRED_SWEEPS = 30;  // D3 -- ~30 s, then FORCE
// D4 -- derived from the scan's ceiling, NOT equal to it, because the two count DIFFERENT
// POPULATIONS. The scan tests `report.filesSeen >= MAX_ASSETS`, and `filesSeen` counts scannable
// ASSETS only -- never a .meta sidecar, never a directory (asset_database.cpp's isScannableAssetName
// arm is its only increment). The watcher's snapshot holds assets PLUS one sidecar per described
// asset PLUS every directory. Reusing MAX_ASSETS verbatim would therefore truncate the watcher at
// roughly HALF the tree the scan still indexes -- silently un-watching the tail in BFS order while
// the footer read "Watching (partial...)". That is not the D4 rescan spin (the watcher would see
// FEWER paths than the scan, which is the safe direction, and `committed` converges after one extra
// rescan), but it is a real coverage cliff at scale, so the ceiling is scaled to the population it
// actually counts: one asset + one sidecar + one directory's worth of headroom each.
inline constexpr std::size_t MAX_WATCH_ENTRIES = MAX_ASSETS * 3;
inline constexpr std::size_t MAX_WATCH_DIRS = 8192;  // BFS queue ceiling; surfaced, never silent

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

// ---- configuration & status ----------------------------------------------------------------------
struct AssetWatchConfig {
    bool enabled = true;
    std::size_t dirsPerPoll = WATCH_DIRS_PER_POLL;
    std::int64_t cooldownMs = WATCH_COOLDOWN_MS;
    std::int64_t settleMs = WATCH_SETTLE_MS;
    std::uint32_t maxDeferredSweeps = MAX_DEFERRED_SWEEPS;
    // The two ceilings are CONFIGURABLE for the same reason parseAssetCache's `maxEntries` is:
    // production takes the defaults, so the REAL constants above are what every frame exercises and
    // they stay pinned at their own declarations -- while a test reaches the truncation branches with
    // a 3-entry tree in microseconds instead of building 50 001 files. A test that passes a ceiling is
    // testing the CEILING MECHANISM, which is exactly the thing under test.
    std::size_t maxEntries = MAX_WATCH_ENTRIES;
    std::size_t maxDirs = MAX_WATCH_DIRS;
};

enum class WatchPhase : std::uint8_t {
    Disabled = 0,  // enabled == false, or no assets root
    Sweeping,      // a sweep is in flight
    Cooldown,      // waiting out cooldownMs before the next sweep
};

struct WatchStatus {
    WatchPhase phase = WatchPhase::Disabled;
    bool enabled = true;
    std::size_t dirsRemaining = 0;      // in the in-flight sweep's queue
    std::size_t entriesSeen = 0;        // the LAST COMPLETED sweep's entry count
    std::uint64_t sweepsCompleted = 0;  // monotonic for the object's lifetime -- AC-38's tick-until signal
    std::uint64_t triggers = 0;         // rescans THIS watcher caused -- monotonic (AC-38)
    std::uint32_t deferredSweeps = 0;   // D3's counter, live
    std::size_t lastUnsettled = 0;      // the last completed sweep's unsettled count
    std::size_t lastChanges = 0;        // the last completed sweep's settled change count
    std::size_t unreadableDirs = 0;     // D5
    bool rootUnreadable = false;        // D5 -- the last sweep ABORTED
    bool truncated = false;
    bool depthLimited = false;
};

// ---- the watcher ---------------------------------------------------------------------------------
class AssetWatcher {
public:
    // TWO roots, for exactly the reason AssetDatabase::rescan takes two (3.1.2 D9): <assetsRoot>/..
    // is wrong the moment paths.assets is nested or ".", and deriving the project root would compute
    // the WRONG Library/ path -- defeating D4's exclusion, which is the ONE exclusion no normal
    // project's tests can reach (AC-16).
    //   * an EMPTY assets root disables the watcher and clears every snapshot (AC-29).
    //   * a DIFFERENT non-empty assets root arms primeOnNextSweep: the first completed sweep becomes
    //     the baseline SILENTLY, because EditorApp already rescans on a root change and a second
    //     rescan would be pure noise (AC-26).
    //   * the SAME pair is a NO-OP -- idempotent on purpose, so a future caller cannot re-prime the
    //     watcher by accident and silently absorb a pending change.
    void setRoot(std::string projectRootUtf8Value, std::string assetsRootUtf8Value);
    [[nodiscard]] const std::string& root() const noexcept;         // the ASSETS root -- what
                                                                    // EditorApp reconciles against
    [[nodiscard]] const std::string& projectRoot() const noexcept;  // AssetDatabase's own shape

    // Writes the config and mirrors cfg.enabled into the status. Does NOT reset the cooldown or abort
    // a sweep. This ALREADY establishes the enabled state -- a following setEnabled(config.enabled)
    // is a provable no-op, since setEnabled early-returns on `cfg.enabled == on`, so do not add one
    // back "to normalise the phase" (an earlier draft of this comment claimed exactly that, and the
    // call it described was dead: WatchPhase is read by no production code, and the first poll() sets
    // it either way). `dirsPerPoll` and `maxDirs` are CLAMPED to at least 1: a zero there is a
    // wedged watcher with no diagnostic, and these are a test seam where a zero is a plausible typo.
    void configure(const AssetWatchConfig& config) noexcept;

    // D10's checkbox. Disabling ABORTS an in-flight sweep (the partial `building` and `queue` are
    // discarded, never diffed -- half a tree looks like a wholesale deletion, exactly the reading D5
    // forbids). Enabling ZEROES the cooldown so a fresh sweep starts on the very next poll -- a user
    // who just re-enabled the watcher is asking for an answer now, not in a second (E21). Neither
    // direction touches `committed` or `lastProbe`: turning the watcher off and on must not resurrect
    // changes that were already acted on. Setting it to its CURRENT value is a no-op.
    void setEnabled(bool on) noexcept;
    [[nodiscard]] bool enabled() const noexcept;

    // D6 -- cancels the cooldown so the next poll starts a sweep immediately. EditorApp calls it on
    // platform::EventType::WindowFocusGained: the alt-tab-back path, and the whole of it. No "force"
    // mode is needed, because sweeps continue (slower) while unfocused, so a change made in another
    // application is usually already SETTLED by the time focus returns.
    void requestImmediateSweep() noexcept;

    // THE ONE IMPURE ENTRY POINT. Advances the sweep by at most cfg.dirsPerPoll directory
    // enumerations and returns TRUE exactly when a sweep completed, settled changes were found, and
    // the caller should rescan. NEVER throws, NEVER logs, performs ZERO file reads.
    //
    // `nowTicks` is a DEFAULTED PARAMETER, not a test-only seam -- AssetDatabase::rescan's
    // hashBudgetBytes and parseAssetCache's maxEntries are the two precedents. Production calls it
    // with the default, so the REAL clock is what every frame exercises; a test passes a synthetic
    // value and never sleeps.
    //
    // `deltaSeconds` is FrameClock::deltaSeconds() -- already SPIKE-CLAMPED (2.3.1), so a long frame
    // (a big rescan) decrements the cooldown by a clamped amount and the next sweep starts at most
    // one frame late (E29).
    [[nodiscard]] bool poll(float deltaSeconds, std::int64_t nowTicks = currentFileTimeTicks());

    [[nodiscard]] const WatchStatus& status() const noexcept;
    [[nodiscard]] const WatchDiff& lastDiff() const noexcept;  // EditorApp's ONE log line reads this

    // A rescan happened that this watcher did NOT cause (a manual Refresh, Reimport All,
    // requestAssetRescan, an orphan delete, or a root change). Adopts the last COMPLETED sweep as the
    // baseline so those same changes are not reported again (Q7/AC-27).
    //
    // Deliberately NOT primeOnNextSweep: `lastProbe` may predate a change made AFTER the manual
    // refresh, and adopting it keeps that change detectable, where priming would silently absorb it
    // into the baseline.
    //
    // This is the ONE sanctioned relaxation of INV-W5 (an unsettled stamp never enters `committed`),
    // and it costs nothing: a torn stamp means the file was still GROWING, so its final stamp
    // necessarily differs from the adopted one and the next two sweeps still report it. Do not "fix"
    // this into primeOnNextSweep.
    //
    // NOT noexcept: `committed = lastProbe` copy-assigns a std::vector, which allocates.
    void noteExternalScan();

    // Forget everything; arm primeOnNextSweep. `sweepsCompleted` and `triggers` are MONOTONIC for the
    // object's lifetime and deliberately survive (the ThumbnailStore::loadAttempts() posture).
    void reset() noexcept;

private:
    struct PendingDir {
        std::string rel;
        std::string canonical;  // "" == unknown; drives the symlink-only canonicalDirectory rule
    };

    void beginSweep();
    [[nodiscard]] bool finishSweep(std::int64_t nowTicks);
    // D5's carry-forward. Copies every `committed` entry under `dirRel + '/'` into `building`. Both
    // are sorted byte-lexicographically, so that is a CONTIGUOUS range found with two std::lower_bounds.
    void carryForward(std::string_view dirRel);

    std::string rootUtf8;         // the ASSETS root
    std::string projectRootUtf8;  // NEVER derived from rootUtf8 -- AssetDatabase's own member comment
    AssetWatchConfig cfg;
    WatchSnapshot committed;  // what the last rescan saw
    WatchSnapshot lastProbe;  // the previous COMPLETED sweep -- isSettled's condition-1 comparand
    WatchSnapshot building;   // the sweep in flight
    // A SORTED VECTOR, never std::set/std::unordered_set (INV-W9): MSVC's node-based containers'
    // move constructors are not noexcept (3.1.2's R9, measured in CI as C2607), and this class is a
    // VALUE member of EditorApp, whose move is `noexcept = default`. asset_database.hpp's
    // `recordList`/`byGuid` and thumbnail_cache.hpp's `entries` are the three precedents.
    std::vector<std::string> visitedCanonical;
    std::vector<PendingDir> queue;  // BFS, cursored -- a cursor is what makes the budget RESUMABLE
    std::size_t queueCursor = 0;
    std::string libraryCanonical;  // resolved once per sweep (D4's Library/ exclusion)
    std::int64_t cooldownRemainingMs = 0;
    bool sweeping = false;
    // TRUE from construction: the very first sweep of any root is a BASELINE, never a trigger.
    bool primeOnNextSweep = true;
    WatchDiff diff;
    WatchStatus statusValue;  // a data member and a member function may not share a name --
                              // the databasePtr/database() and depthFormatValue/depthFormat()
                              // precedent (3.1.1 D13's naming note)
};

// F11: EditorApp's move is `noexcept = default`, so every value member must be noexcept-movable.
// asset_database.hpp:148-168's precedent -- the AGGREGATE asserts come FIRST, so a regression NAMES
// this type rather than failing an opaque member's assert (AC-43). 3.1.2's R9 fired for real on MSVC
// once already; if either of the first two ever reddens, the fix is to remove the offending member
// TYPE, NEVER to relax the assert.
static_assert(std::is_nothrow_move_constructible_v<AssetWatcher>);
static_assert(std::is_nothrow_move_assignable_v<AssetWatcher>);
static_assert(std::is_nothrow_move_constructible_v<WatchSnapshot>);
static_assert(std::is_nothrow_move_assignable_v<WatchSnapshot>);
static_assert(std::is_nothrow_move_constructible_v<WatchDiff>);
static_assert(std::is_nothrow_move_assignable_v<WatchDiff>);

}  // namespace engine::editor

// Aero Engine — the assets-tree change watcher (task 3.1.4). The ONE impure entry point is poll(),
// and every rule it applies is decided by the PURE functions above it, from vector literals with an
// injected clock. NO <filesystem>, NO <fstream>, NO <thread>/<mutex>/<atomic>, NO ImGui, NO entt,
// NO SDL, and NO LOGGING (INV-W1/INV-W2/AC-40) -- all disk access composes listDirectory() and
// canonicalDirectory(), exactly as asset_database.cpp does, and NOTHING here reads a file's bytes.
// NO RECURSION anywhere: the walk is an explicit, cursored queue (misc-no-recursion).
//
// asset_cache.hpp is included for ASSET_CACHE_DIR_NAME alone (D4's Library/ exclusion), used
// DIRECTLY here rather than left to arrive transitively through asset_meta.hpp: asset_watcher.HPP
// deliberately names neither -- the header depends only on asset_meta.hpp and project_files.hpp,
// keeping the dependency direction 3.1.2's A20 established (the .meta format must never depend on
// the cache).
#include <aero/editor/asset_cache.hpp>
#include <aero/editor/asset_watcher.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// Appends when there is room; otherwise sets `truncated` and REFUSES (AC-18/E5). The return value is
// deliberately not [[nodiscard]] -- the flag IS the signal -- but it exists so a future caller can
// branch on it.
bool pushEntry(WatchSnapshot& snapshot, std::size_t maxEntries, WatchEntry entry) {
    if (snapshot.entries.size() >= maxEntries) {
        snapshot.truncated = true;
        return false;
    }
    snapshot.entries.push_back(std::move(entry));
    return true;
}

// The sorted-vector stand-in for asset_database.cpp:211's `std::set<std::string> visitedCanonical`.
// INV-W9 forbids std::set here. Returns TRUE iff it INSERTED -- i.e. iff this canonical path had not
// been visited yet, which is exactly the scan's own `visitedCanonical.insert(...).second`.
bool insertSorted(std::vector<std::string>& sorted, const std::string& value) {
    const auto it = std::lower_bound(sorted.begin(), sorted.end(), value);
    if (it != sorted.end() && *it == value) {
        return false;
    }
    sorted.insert(it, value);
    return true;
}

// ONE place where "settled -> emit, unsettled -> count" is decided, so neither diffSnapshots call
// site can implement half of it (INV-W4).
void appendIfSettled(WatchDiff& out, const WatchEntry& current, const WatchSnapshot& lastProbe, std::int64_t nowTicks,
                     std::int64_t settleTicks, WatchChangeKind kind) {
    if (isSettled(current, lastProbe.find(current.path), nowTicks, settleTicks)) {
        out.changes.push_back(WatchChange{.path = current.path, .kind = kind, .isDirectory = current.isDirectory});
        return;
    }
    ++out.unsettled;
}

}  // namespace

// ---- WatchSnapshot::find -------------------------------------------------------------------------
const WatchEntry* WatchSnapshot::find(std::string_view path) const noexcept {
    // The AssetDatabase::findByPath idiom (asset_database.cpp:119-130) verbatim: a std::lower_bound
    // over the sorted vector directly, NEVER a std::string constructed from the std::string_view
    // argument (that would allocate inside a noexcept function).
    const auto it = std::lower_bound(entries.begin(), entries.end(), path,
                                     [](const WatchEntry& entry, std::string_view key) { return entry.path < key; });
    if (it == entries.end() || it->path != path) {
        return nullptr;
    }
    return &*it;
}

// ---- isSettled -----------------------------------------------------------------------------------
bool isSettled(const WatchEntry& current, const WatchEntry* previous, std::int64_t nowTicks,
               std::int64_t settleTicks) noexcept {
    if (current.isDirectory) {
        return true;  // D2 carve-out 2: a directory's stamp is never read -- no torn state to observe
    }
    if (!current.stampKnown) {
        return false;  // D2 carve-out 3: it cannot be compared and must not be guessed at
    }
    // CONDITION 2 (age), computed WITHOUT signed overflow. `mtime` is an OPAQUE, unvalidated int64
    // straight from last_write_time(); on libc++ and libstdc++ that is NANOSECONDS since 1970, so a
    // file dated before roughly 1732 makes a bare `nowTicks - current.mtime` overflow int64 -- and
    // cmake/sanitizers.cmake builds the Debug lanes with -fno-sanitize-recover=all, where a signed
    // overflow is an ABORT, not a warning (3.1.3 shipped exactly one of these, on +inf -> int). A
    // stamp in the FUTURE (clock skew on a network volume) must answer "not old enough" anyway, which
    // is what the first test says; after it the difference is taken in std::uint64_t, where
    // wraparound is DEFINED and the true difference always fits.
    if (current.mtime > nowTicks) {
        return false;
    }
    const std::uint64_t age = static_cast<std::uint64_t>(nowTicks) - static_cast<std::uint64_t>(current.mtime);
    const std::uint64_t threshold = settleTicks > 0 ? static_cast<std::uint64_t>(settleTicks) : 0U;
    const bool oldEnough = age >= threshold;  // >=, NEVER > -- AW27 pins this exact boundary
    if (previous == nullptr) {
        return oldEnough;  // an ADDITION: there is no previous observation to be stable against
    }
    // CONDITION 1 (stability): byte-identical to what the PREVIOUS COMPLETED SWEEP observed.
    const bool stable = previous->stampKnown && previous->size == current.size && previous->mtime == current.mtime;
    return stable && oldEnough;
}

// ---- diffSnapshots -------------------------------------------------------------------------------
WatchDiff diffSnapshots(const WatchSnapshot& committed, const WatchSnapshot& probe, const WatchSnapshot& lastProbe,
                        std::int64_t nowTicks, std::int64_t settleTicks) {
    WatchDiff out;
    std::size_t i = 0;  // cursor into `committed`
    std::size_t j = 0;  // cursor into `probe`
    while (i < committed.entries.size() || j < probe.entries.size()) {
        const bool committedOnly = j == probe.entries.size() ||
                                   (i < committed.entries.size() && committed.entries[i].path < probe.entries[j].path);
        if (committedOnly) {
            // D2 carve-out 1: a REMOVAL has no `current` at all, so there is no torn state to observe
            // and it is settled by construction -- it never passes through isSettled (AC-9).
            const WatchEntry& gone = committed.entries[i];
            out.changes.push_back(
                WatchChange{.path = gone.path, .kind = WatchChangeKind::Removed, .isDirectory = gone.isDirectory});
            ++i;
            continue;
        }
        const bool probeOnly = i == committed.entries.size() || probe.entries[j].path < committed.entries[i].path;
        if (probeOnly) {
            appendIfSettled(out, probe.entries[j], lastProbe, nowTicks, settleTicks, WatchChangeKind::Added);
            ++j;
            continue;
        }
        // The same path in both snapshots.
        const WatchEntry& old = committed.entries[i];
        const WatchEntry& cur = probe.entries[j];
        // A path that changed KIND (file <-> directory) is a real, ordinary edit that a (size, mtime)
        // comparison alone would miss ENTIRELY. isSettled's directory carve-out then makes a
        // change-to-directory settle outright and a change-to-file go through both conditions -- so
        // this composes with D2 rather than adding a fourth carve-out.
        const bool kindChanged = old.isDirectory != cur.isDirectory;
        const bool stampChanged =
            !cur.isDirectory && (old.size != cur.size || old.mtime != cur.mtime || old.stampKnown != cur.stampKnown);
        if (kindChanged || stampChanged) {
            appendIfSettled(out, cur, lastProbe, nowTicks, settleTicks, WatchChangeKind::Modified);
        }
        ++i;
        ++j;
    }
    return out;
}

// ---- applyChanges --------------------------------------------------------------------------------
WatchSnapshot applyChanges(const WatchSnapshot& committed, const WatchSnapshot& probe, const WatchDiff& diff) {
    WatchSnapshot out;
    // The FLAGS describe the sweep that produced them, so they come from `probe`, never from the old
    // committed snapshot.
    out.truncated = probe.truncated;
    out.depthLimited = probe.depthLimited;
    out.unreadableDirs = probe.unreadableDirs;
    out.entries.reserve(committed.entries.size() + diff.changes.size());

    std::size_t i = 0;  // cursor into `committed`
    std::size_t k = 0;  // cursor into `diff.changes` -- sorted by path, the SAME order
    while (i < committed.entries.size() || k < diff.changes.size()) {
        const bool untouched = k == diff.changes.size() ||
                               (i < committed.entries.size() && committed.entries[i].path < diff.changes[k].path);
        if (untouched) {
            out.entries.push_back(committed.entries[i]);
            ++i;
            continue;
        }
        const WatchChange& change = diff.changes[k];
        if (i < committed.entries.size() && committed.entries[i].path == change.path) {
            ++i;  // drop the old entry: a Removed leaves nothing, a Modified is re-added from `probe`
        }
        if (change.kind != WatchChangeKind::Removed) {
            // INV-W5: only a SETTLED change reaches here, so only a settled stamp can enter the
            // result. An unsettled entry's stamp never becomes the baseline (AC-12).
            const WatchEntry* const fresh = probe.find(change.path);
            if (fresh != nullptr) {
                out.entries.push_back(*fresh);
            }
        }
        ++k;
    }
    return out;
}

// ---- AssetWatcher --------------------------------------------------------------------------------
void AssetWatcher::setRoot(std::string projectRootUtf8Value, std::string assetsRootUtf8Value) {
    if (rootUtf8 == assetsRootUtf8Value && projectRootUtf8 == projectRootUtf8Value) {
        return;  // idempotent: re-setting the SAME pair must not re-prime and absorb a pending change
    }
    projectRootUtf8 = std::move(projectRootUtf8Value);
    rootUtf8 = std::move(assetsRootUtf8Value);
    reset();  // arms primeOnNextSweep: the first completed sweep of a new root is a SILENT baseline
}

const std::string& AssetWatcher::root() const noexcept { return rootUtf8; }
const std::string& AssetWatcher::projectRoot() const noexcept { return projectRootUtf8; }

void AssetWatcher::configure(const AssetWatchConfig& config) noexcept {
    cfg = config;
    // A zero here is a watcher that never advances and never enqueues its own root, with no
    // diagnostic anywhere. These two are a TEST SEAM, where a zero is a plausible typo; the other
    // four fields are all meaningful at zero and are deliberately NOT clamped.
    if (cfg.dirsPerPoll == 0) {
        cfg.dirsPerPoll = 1;
    }
    if (cfg.maxDirs == 0) {
        cfg.maxDirs = 1;
    }
    statusValue.enabled = cfg.enabled;
}

void AssetWatcher::setEnabled(bool on) noexcept {
    if (cfg.enabled == on) {
        return;  // re-checking an already-checked box must not restart a sweep
    }
    cfg.enabled = on;
    statusValue.enabled = on;
    if (on) {
        cooldownRemainingMs = 0;  // E21: a user who just re-enabled it wants an answer NOW
        return;
    }
    // ABANDON the in-flight sweep. `committed` and `lastProbe` are deliberately untouched: turning
    // the watcher off and on must not resurrect changes that were already acted on.
    sweeping = false;
    queue.clear();
    queueCursor = 0;
    building = WatchSnapshot{};
    visitedCanonical.clear();
    libraryCanonical.clear();
    statusValue.phase = WatchPhase::Disabled;
    statusValue.dirsRemaining = 0;
}

bool AssetWatcher::enabled() const noexcept { return cfg.enabled; }

void AssetWatcher::requestImmediateSweep() noexcept { cooldownRemainingMs = 0; }

const WatchStatus& AssetWatcher::status() const noexcept { return statusValue; }
const WatchDiff& AssetWatcher::lastDiff() const noexcept { return diff; }

void AssetWatcher::noteExternalScan() {
    committed = lastProbe;
    diff = WatchDiff{};
    statusValue.deferredSweeps = 0;
    statusValue.lastChanges = 0;
    statusValue.lastUnsettled = 0;
}

void AssetWatcher::reset() noexcept {
    committed = WatchSnapshot{};
    lastProbe = WatchSnapshot{};
    building = WatchSnapshot{};
    diff = WatchDiff{};
    queue.clear();
    queueCursor = 0;
    visitedCanonical.clear();
    libraryCanonical.clear();
    cooldownRemainingMs = 0;
    sweeping = false;
    primeOnNextSweep = true;
    // sweepsCompleted and triggers are MONOTONIC for the object's lifetime and deliberately survive a
    // reset -- the ThumbnailStore::loadAttempts() posture. Everything else returns to its default.
    const std::uint64_t sweeps = statusValue.sweepsCompleted;
    const std::uint64_t fires = statusValue.triggers;
    statusValue = WatchStatus{};
    statusValue.sweepsCompleted = sweeps;
    statusValue.triggers = fires;
    statusValue.enabled = cfg.enabled;
    statusValue.phase = cfg.enabled && !rootUtf8.empty() ? WatchPhase::Cooldown : WatchPhase::Disabled;
}

void AssetWatcher::beginSweep() {
    building = WatchSnapshot{};
    queue.clear();
    queueCursor = 0;
    // The root's OWN canonical path seeds the visited set BEFORE the walk, exactly as
    // asset_database.cpp:211 does, so a self-link (assets/link -> assets) is caught on the first
    // repeat. "" is fine: an unresolvable root simply disables the free-path optimisation below, and
    // no child can ever compare equal to it because an empty childCanonical is `continue`d first.
    std::string rootCanonical = canonicalDirectory(rootUtf8);
    visitedCanonical.clear();
    visitedCanonical.push_back(rootCanonical);
    queue.push_back(PendingDir{std::string(), std::move(rootCanonical)});
    // D4/AC-16: derived from the PROJECT root, exactly as asset_database.cpp:178-180 derives it --
    // NEVER from rootUtf8 + "/..", which is wrong the moment paths.assets is nested or "." (3.1.2
    // D9). "" while Library/ does not exist yet, which is the common case and which the comparison
    // handles (the guard is `!libraryCanonical.empty() && ...`). This is the ONE exclusion that
    // cannot be discovered by testing a normal project: a project whose assets root is "." puts
    // Library/ INSIDE the watched tree, and every scan may rewrite Library/asset-cache.json -- so
    // without this the watcher rescans forever at the cooldown's full rate.
    libraryCanonical = canonicalDirectory(projectRootUtf8 + '/' + std::string(ASSET_CACHE_DIR_NAME));
    sweeping = true;
    statusValue.rootUnreadable = false;
    statusValue.phase = WatchPhase::Sweeping;
}

void AssetWatcher::carryForward(std::string_view dirRel) {
    // D5/AC-20: a sub-directory whose listing failed contributes NO CHANGE AT ALL. Its
    // previously-committed entries are every path under `dirRel + '/'`, a CONTIGUOUS range in the
    // sorted snapshot (INV-W10). The upper bound is `dirRel + '0'` because '0' (0x30) is exactly one
    // past '/' (0x2F), making it the first byte-lexicographic string that is NOT a descendant.
    //
    // Why this rule exists: a directory that momentarily refuses enumeration -- an antivirus lock, a
    // Dropbox/OneDrive sync, a network hiccup, a permission change -- would otherwise present as
    // "every file under it was deleted". Acted on, that means a rescan that drops those records,
    // orphan reports for their sidecars, their thumbnails released from the GPU, and the exact
    // reverse one sweep later. ABSENCE OBSERVED THROUGH A FAILURE IS NOT ABSENCE (INV-W6).
    std::string lower(dirRel);
    lower += '/';
    std::string upper(dirRel);
    upper += '0';
    const auto byPath = [](const WatchEntry& entry, const std::string& key) { return entry.path < key; };
    const auto first = std::lower_bound(committed.entries.begin(), committed.entries.end(), lower, byPath);
    const auto last = std::lower_bound(first, committed.entries.end(), upper, byPath);
    for (auto it = first; it != last; ++it) {
        // APPENDED; finishSweep sorts `building` once, so insertion order needs no care here.
        pushEntry(building, cfg.maxEntries, *it);
    }
}

bool AssetWatcher::poll(float deltaSeconds, std::int64_t nowTicks) {
    statusValue.enabled = cfg.enabled;
    if (!cfg.enabled || rootUtf8.empty()) {
        // AC-28/AC-29. An in-flight sweep is ABANDONED, never diffed.
        sweeping = false;
        queue.clear();
        queueCursor = 0;
        building = WatchSnapshot{};
        visitedCanonical.clear();
        libraryCanonical.clear();
        statusValue.phase = WatchPhase::Disabled;
        statusValue.dirsRemaining = 0;
        return false;
    }

    if (!sweeping) {
        // The upper clamp is NOT defensive padding: 3.1.3 shipped a real UBSan ABORT from casting an
        // infinite float to an integer, and -fno-sanitize-recover=all makes that a crash. FrameClock
        // spike-clamps its delta, so this can only fire if a future caller passes a raw one.
        constexpr float MAX_POLL_DELTA_SECONDS = 3600.0F;
        std::int64_t elapsedMs = 0;
        if (deltaSeconds > 0.0F) {  // NaN-safe (editor_app.cpp:203's idiom): NaN fails every `>`
            const float clamped = deltaSeconds < MAX_POLL_DELTA_SECONDS ? deltaSeconds : MAX_POLL_DELTA_SECONDS;
            // std::lround, never a truncating cast: truncation would round 0.999 ms to 0 and let a
            // fast caller stall the cooldown forever.
            elapsedMs = static_cast<std::int64_t>(std::lround(static_cast<double>(clamped) * 1000.0));
        }
        cooldownRemainingMs -= elapsedMs;
        if (cooldownRemainingMs > 0) {
            statusValue.phase = WatchPhase::Cooldown;
            statusValue.dirsRemaining = 0;
            return false;  // AC-24
        }
        cooldownRemainingMs = 0;
        beginSweep();
    }

    std::size_t processed = 0;
    while (queueCursor < queue.size() && processed < cfg.dirsPerPoll) {
        // A COPY, never a reference: queue.push_back() below can reallocate, which would leave a
        // reference into the old buffer dangling (a heap-use-after-free ASan would catch, but only on
        // a tree deep enough to grow the vector mid-iteration).
        const PendingDir dir = queue[queueCursor];
        ++queueCursor;
        ++processed;

        // F4's EXACT call: hidden names are filtered by listDirectory itself, which is half of what
        // keeps the watcher's visible set identical to the scan's (D4/INV-W3/AC-13).
        const DirectoryListing listing = listDirectory(rootUtf8, dir.rel, /*includeHidden=*/false);
        if (listing.truncated) {
            building.truncated = true;
        }
        if (listing.status != ScanStatus::Ok) {
            if (dir.rel.empty()) {
                // D5/AC-21: the ROOT failed. Nothing is diffed, nothing is committed, nothing fires.
                // A genuinely deleted assets root is still handled correctly -- by rescan(), on the
                // next manual refresh or project reopen, which is where the destructive reading
                // belongs.
                sweeping = false;
                queue.clear();
                queueCursor = 0;
                building = WatchSnapshot{};
                visitedCanonical.clear();
                statusValue.rootUnreadable = true;
                statusValue.dirsRemaining = 0;
                statusValue.phase = WatchPhase::Cooldown;
                cooldownRemainingMs = cfg.cooldownMs;
                return false;
            }
            ++building.unreadableDirs;
            carryForward(dir.rel);  // AC-20
            continue;
        }

        for (const FileEntry& entry : listing.entries) {
            if (entry.isDirectory) {
                std::string childRel = joinRelative(dir.rel, entry.name);
                // asset_database.cpp:249-254's optimisation, copied EXACTLY: derive the child's
                // canonical path for FREE when no link is in play, and pay for the one real
                // canonicalDirectory call otherwise. Calling it per directory per sweep would
                // dominate the poll's whole cost.
                std::string childCanonical;
                if (!entry.isSymlink && !dir.canonical.empty()) {
                    childCanonical = dir.canonical + '/' + entry.name;  // EXACT: no link in this chain
                } else {
                    childCanonical = canonicalDirectory(rootUtf8 + '/' + childRel);
                }
                if (!libraryCanonical.empty() && childCanonical == libraryCanonical) {
                    continue;  // D4/AC-16: our OWN output. Reported nowhere, exactly as the scan does.
                }
                if (childCanonical.empty()) {
                    continue;  // unresolvable: refuse to descend rather than guess (the scan's rule)
                }
                if (!insertSorted(visitedCanonical, childCanonical)) {
                    continue;  // AC-17/E8: an alias of an already-visited directory -- NO RECORDS
                }
                // DESIGNATED initializers throughout, never positional -- 3.1.2's A2 lesson: a
                // positional aggregate silently re-maps the moment a field is added, because
                // bool -> int64 is a PROMOTION and nothing diagnoses it.
                pushEntry(building, cfg.maxEntries,
                          WatchEntry{.path = childRel, .isDirectory = true, .stampKnown = false});
                if (depthOf(childRel) >= MAX_TREE_DEPTH) {
                    building.depthLimited = true;  // AC-18 -- the SCAN's own bound, reused (F13)
                } else if (queue.size() >= cfg.maxDirs) {
                    building.truncated = true;  // surfaced in the footer, never silent
                } else {
                    queue.push_back(PendingDir{std::move(childRel), std::move(childCanonical)});
                }
            } else if (isWatchableAssetName(entry.name)) {
                // D4/INV-W3: the SHARED predicate -- isScannableAssetName || isMetaFileName, i.e.
                // exactly "a name the scan's own file-bucketing arm can see". AC-14 (.aero-tmp is
                // excluded) and AC-15 (a .meta sidecar IS watched) both fall out of it.
                pushEntry(building, cfg.maxEntries,
                          WatchEntry{.path = joinRelative(dir.rel, entry.name),
                                     .size = entry.size,
                                     .mtime = entry.mtime,
                                     .isDirectory = false,
                                     .stampKnown = entry.sizeKnown && entry.mtimeKnown});
            }
        }
    }

    statusValue.dirsRemaining = queue.size() - queueCursor;
    if (queueCursor < queue.size()) {
        statusValue.phase = WatchPhase::Sweeping;
        return false;  // AC-22: at most cfg.dirsPerPoll directory enumerations per call
    }
    return finishSweep(nowTicks);
}

bool AssetWatcher::finishSweep(std::int64_t nowTicks) {
    // BYTE-lexicographic, never case-folded (INV-W10). Duplicates are impossible by construction: a
    // directory whose listing failed is never descended into, so its carried-forward children can
    // never also be walked, and an aliased directory contributes no records at all. Do not "harden"
    // this with std::unique -- that would MASK the bug it looks like it is preventing.
    std::sort(building.entries.begin(), building.entries.end(),
              [](const WatchEntry& a, const WatchEntry& b) { return a.path < b.path; });

    ++statusValue.sweepsCompleted;
    statusValue.entriesSeen = building.entries.size();
    statusValue.truncated = building.truncated;
    statusValue.depthLimited = building.depthLimited;
    statusValue.unreadableDirs = building.unreadableDirs;
    statusValue.dirsRemaining = 0;
    statusValue.rootUnreadable = false;
    sweeping = false;
    queue.clear();
    queueCursor = 0;
    visitedCanonical.clear();
    cooldownRemainingMs = cfg.cooldownMs;
    statusValue.phase = WatchPhase::Cooldown;

    if (primeOnNextSweep) {
        // AC-26/Q8: the FIRST completed sweep of any root is a BASELINE, never a trigger.
        primeOnNextSweep = false;
        committed = building;
        lastProbe = std::move(building);
        building = WatchSnapshot{};
        diff = WatchDiff{};
        statusValue.deferredSweeps = 0;
        statusValue.lastUnsettled = 0;
        statusValue.lastChanges = 0;
        return false;
    }

    const std::int64_t settleTicks = fileTimeTicksFromMillis(cfg.settleMs);
    diff = diffSnapshots(committed, building, lastProbe, nowTicks, settleTicks);
    statusValue.lastUnsettled = diff.unsettled;
    statusValue.lastChanges = diff.changes.size();
    lastProbe = building;  // condition 1's comparand, updated ALWAYS -- even on a deferred sweep

    if (diff.unsettled > 0 && statusValue.deferredSweeps < cfg.maxDeferredSweeps) {
        // AC-11/D3: DEFER THE WHOLE BATCH, settled changes included. Without this, a 30-second copy
        // of 500 files produces roughly one rescan per sweep for its entire duration, and each rescan
        // reads and parses every .meta in the project.
        ++statusValue.deferredSweeps;
        building = WatchSnapshot{};
        return false;
    }
    // On the sweep that EXCEEDS the cap the watcher FORCES: it fires on whatever IS settled, leaves
    // the unsettled entries uncommitted, and resets the counter. A file being rewritten continuously
    // therefore delays an unrelated settled change by at most maxDeferredSweeps sweeps, never
    // forever -- and its own churning stamp is never committed, so it is re-examined every sweep
    // rather than being silently accepted mid-write.
    statusValue.deferredSweeps = 0;
    if (diff.changes.empty()) {
        building = WatchSnapshot{};
        return false;  // AC-19: the steady state is SILENT, and so is a forced fire with nothing settled
    }
    // AC-12/D3: SELECTIVE. `committed` becomes the previous committed with ONLY the settled changes
    // applied -- copying the whole probe across would bake a torn stamp in as truth (INV-W5).
    committed = applyChanges(committed, building, diff);
    building = WatchSnapshot{};
    ++statusValue.triggers;
    return true;
}

}  // namespace engine::editor

// Aero Engine — the assets-tree change watcher (task 3.1.4). The ONE impure entry point is poll(),
// and every rule it applies is decided by the PURE functions above it, from vector literals with an
// injected clock. NO <filesystem>, NO <fstream>, NO <thread>/<mutex>/<atomic>, NO ImGui, NO entt,
// NO SDL, and NO LOGGING (INV-W1/INV-W2/AC-40) -- all disk access composes listDirectory() and
// canonicalDirectory(), exactly as asset_database.cpp does, and NOTHING here reads a file's bytes.
// NO RECURSION anywhere: the walk is an explicit, cursored queue (misc-no-recursion).
#include <aero/editor/asset_watcher.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

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

}  // namespace engine::editor

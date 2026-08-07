// tests/editor/asset_watcher_test.cpp -- task 3.1.4: the assets-tree change watcher. A TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED (D4/AC-17/INV-P5, the asset_meta_test.cpp precedent): asset_watcher.hpp depends only on
// asset_meta.hpp and project_files.hpp, neither of which needs reflection, so every case here must be
// PRESENT and PASSING in all three build configurations -- prove it with --list-test-cases.
//
// Tier-0: no GPU, no window, no ImGui context, and NO TEST SLEEPS anywhere in this file -- every
// clock value used by the pure functions below (isSettled/diffSnapshots/applyChanges) is an injected
// literal. The AssetWatcher class itself, against a real scratch TempDir with an injected clock, is
// Step 3's addition (cases AW29-AW46).
#include <aero/editor/asset_watcher.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using engine::editor::applyChanges;
using engine::editor::diffSnapshots;
using engine::editor::isSettled;
using engine::editor::WatchChange;
using engine::editor::WatchChangeKind;
using engine::editor::WatchDiff;
using engine::editor::WatchEntry;
using engine::editor::WatchSnapshot;

namespace {

// Designated initializers throughout (3.1.2's A2 lesson) -- never positional.
WatchEntry fileEntry(std::string path, std::uint64_t size, std::int64_t mtime, bool stampKnown = true) {
    return WatchEntry{
        .path = std::move(path), .size = size, .mtime = mtime, .isDirectory = false, .stampKnown = stampKnown};
}

WatchEntry dirEntry(std::string path) {
    return WatchEntry{.path = std::move(path), .isDirectory = true, .stampKnown = false};
}

WatchSnapshot snapshotOf(std::vector<WatchEntry> entries) {
    WatchSnapshot out;
    out.entries = std::move(entries);
    return out;
}

}  // namespace

// ---- WatchSnapshot::find ---------------------------------------------------------------------------

TEST_CASE("asset_watcher: find on an empty snapshot is nullptr (AW1)") {
    const WatchSnapshot empty;
    CHECK(empty.find("a") == nullptr);
}

TEST_CASE("asset_watcher: find on a single-entry snapshot (AW2)") {
    const WatchSnapshot snap = snapshotOf({fileEntry("a", 1, 1)});
    CHECK(snap.find("a") == &snap.entries[0]);
    CHECK(snap.find("b") == nullptr);
}

TEST_CASE("asset_watcher: find over several entries hits and misses correctly (AW3)") {
    const WatchSnapshot snap = snapshotOf(
        {fileEntry("a", 1, 1), fileEntry("b", 1, 1), fileEntry("c", 1, 1), fileEntry("m", 1, 1), fileEntry("z", 1, 1)});
    CHECK(snap.find("a") != nullptr);
    CHECK(snap.find("b") != nullptr);
    CHECK(snap.find("c") != nullptr);
    CHECK(snap.find("m") != nullptr);
    CHECK(snap.find("z") != nullptr);
    CHECK(snap.find("aa") == nullptr);
    CHECK(snap.find("") == nullptr);
}

TEST_CASE("asset_watcher: find is BYTE order, never case-folded (AW4, INV-W10)") {
    // "Z.png" < "a.png" byte-lexicographically ('Z' == 0x5A < 'a' == 0x61).
    const WatchSnapshot snap = snapshotOf({fileEntry("Z.png", 1, 1), fileEntry("a.png", 1, 1)});
    CHECK(snap.find("Z.png") != nullptr);
    CHECK(snap.find("a.png") != nullptr);
}

// ---- isSettled --------------------------------------------------------------------------------------

TEST_CASE("asset_watcher: a directory settles outright -- its stamp is never read (AW5)") {
    const WatchEntry dir = dirEntry("sub");
    CHECK(isSettled(dir, nullptr, /*nowTicks=*/0, /*settleTicks=*/2000));
}

TEST_CASE("asset_watcher: an entry with an unknown stamp is never settled, even with no previous (AW6, AC-10)") {
    const WatchEntry unknown = fileEntry("a", 1, /*mtime=*/-5000, /*stampKnown=*/false);
    CHECK_FALSE(isSettled(unknown, nullptr, /*nowTicks=*/0, /*settleTicks=*/2000));
}

// Sabotage-found gap (S4): AW6 only exercises current.stampKnown == false against previous == nullptr,
// and AW22's own previous entry happens to ALSO carry stampKnown == false, so removing the current-side
// guard (D2 carve-out 3) alone leaves AW22 green -- the previous-side check inside `stable` catches it
// there by coincidence. This case gives current.stampKnown == false a previous entry that is otherwise
// a PERFECT, STABLE match (valid stamp, identical size/mtime), so ONLY the current-side guard can
// refuse it -- the one case that discriminates S4 on its own.
TEST_CASE(
    "asset_watcher: current.stampKnown == false is never settled, even against a matching, "
    "stable previous (AW6b, AC-10, seed S4)") {
    const WatchEntry previous = fileEntry("a", 1, -5000);
    const WatchEntry current = fileEntry("a", 1, -5000, /*stampKnown=*/false);
    CHECK_FALSE(isSettled(current, &previous, /*nowTicks=*/0, /*settleTicks=*/2000));
}

TEST_CASE("asset_watcher: an addition old enough settles (AW7)") {
    const WatchEntry added = fileEntry("a", 1, /*mtime=*/-5000);
    CHECK(isSettled(added, nullptr, /*nowTicks=*/0, /*settleTicks=*/2000));
}

TEST_CASE("asset_watcher: an addition too young does NOT settle (AW8, AC-8)") {
    const WatchEntry added = fileEntry("a", 1, /*mtime=*/0);
    CHECK_FALSE(isSettled(added, nullptr, /*nowTicks=*/0, /*settleTicks=*/2000));
}

TEST_CASE("asset_watcher: stable + old settles (AW9)") {
    const WatchEntry previous = fileEntry("a", 1, -5000);
    const WatchEntry current = fileEntry("a", 1, -5000);
    CHECK(isSettled(current, &previous, /*nowTicks=*/0, /*settleTicks=*/2000));
}

TEST_CASE("asset_watcher: stable but too young does NOT settle (AW10, AC-8)") {
    const WatchEntry previous = fileEntry("a", 1, 0);
    const WatchEntry current = fileEntry("a", 1, 0);
    CHECK_FALSE(isSettled(current, &previous, /*nowTicks=*/0, /*settleTicks=*/2000));
}

TEST_CASE("asset_watcher: a different SIZE from previous is not stable (AW11, AC-7)") {
    const WatchEntry previous = fileEntry("a", 1, -5000);
    const WatchEntry current = fileEntry("a", 2, -5000);
    CHECK_FALSE(isSettled(current, &previous, /*nowTicks=*/0, /*settleTicks=*/2000));
}

TEST_CASE("asset_watcher: a different MTIME from previous is not stable (AW12, AC-7)") {
    const WatchEntry previous = fileEntry("a", 1, -6000);
    const WatchEntry current = fileEntry("a", 1, -5000);
    CHECK_FALSE(isSettled(current, &previous, /*nowTicks=*/0, /*settleTicks=*/2000));
}

TEST_CASE("asset_watcher: an uncomparable previous is never stability (AW13)") {
    const WatchEntry previous = fileEntry("a", 1, -5000, /*stampKnown=*/false);
    const WatchEntry current = fileEntry("a", 1, -5000);
    CHECK_FALSE(isSettled(current, &previous, /*nowTicks=*/0, /*settleTicks=*/2000));
}

// ---- diffSnapshots ------------------------------------------------------------------------------

TEST_CASE("asset_watcher: diffSnapshots on two empty snapshots (AW14)") {
    const WatchSnapshot empty;
    const WatchDiff diff = diffSnapshots(empty, empty, empty, 0, 2000);
    CHECK(diff.changes.empty());
    CHECK(diff.unsettled == 0);
}

TEST_CASE("asset_watcher: diffSnapshots on identical committed/probe reports nothing (AW15, AC-19's pure half)") {
    const WatchSnapshot both = snapshotOf({fileEntry("a", 1, -5000)});
    const WatchDiff diff = diffSnapshots(both, both, both, 0, 2000);
    CHECK(diff.changes.empty());
}

TEST_CASE("asset_watcher: diffSnapshots reports one Added for a new settled entry (AW16)") {
    const WatchSnapshot committed;
    const WatchSnapshot probe = snapshotOf({fileEntry("a", 1, -5000)});
    const WatchDiff diff = diffSnapshots(committed, probe, WatchSnapshot{}, 0, 2000);
    REQUIRE(diff.changes.size() == 1);
    CHECK(diff.changes[0].path == "a");
    CHECK(diff.changes[0].kind == WatchChangeKind::Added);
}

TEST_CASE("asset_watcher: a removal is settled by construction, even against an unsettleable stamp (AW17, AC-9)") {
    // BOTH of isSettled's refusals would fire if a Removed were routed through it: stampKnown ==
    // false AND a fresh mtime. This exact setup is what makes sabotage seed S5 discriminate; a
    // plainly-stamped old entry would not.
    const WatchSnapshot committed = snapshotOf({fileEntry("a", 1, /*mtime=*/0, /*stampKnown=*/false)});
    const WatchSnapshot probe;
    const WatchDiff diff = diffSnapshots(committed, probe, WatchSnapshot{}, /*nowTicks=*/0, /*settleTicks=*/2000);
    REQUIRE(diff.changes.size() == 1);
    CHECK(diff.changes[0].kind == WatchChangeKind::Removed);
    CHECK(diff.unsettled == 0);
}

TEST_CASE("asset_watcher: a modification is reported (AW18)") {
    const WatchSnapshot committed = snapshotOf({fileEntry("a", 1, -5000)});
    const WatchSnapshot probe = snapshotOf({fileEntry("a", 2, -5000)});
    const WatchDiff diff = diffSnapshots(committed, probe, probe, /*nowTicks=*/0, /*settleTicks=*/2000);
    REQUIRE(diff.changes.size() == 1);
    CHECK(diff.changes[0].kind == WatchChangeKind::Modified);
}

TEST_CASE("asset_watcher: a mixed diff is sorted and unsettled stays zero (AW19)") {
    const WatchSnapshot committed =
        snapshotOf({fileEntry("a", 1, -5000), fileEntry("b", 1, -5000), fileEntry("c", 1, -5000)});
    const WatchSnapshot probe =
        snapshotOf({fileEntry("a", 1, -5000), fileEntry("c", 1, -5000), fileEntry("d", 1, -5000)});
    const WatchDiff diff = diffSnapshots(committed, probe, probe, /*nowTicks=*/0, /*settleTicks=*/2000);
    REQUIRE(diff.changes.size() == 2);
    CHECK(diff.changes[0].path == "b");
    CHECK(diff.changes[0].kind == WatchChangeKind::Removed);
    CHECK(diff.changes[1].path == "d");
    CHECK(diff.changes[1].kind == WatchChangeKind::Added);
    CHECK(diff.unsettled == 0);
}

TEST_CASE("asset_watcher: a directory add and remove settle without reading any stamp (AW20, AC-4/AC-9)") {
    const WatchSnapshot committed = snapshotOf({dirEntry("gone")});
    const WatchSnapshot probe = snapshotOf({dirEntry("new")});
    const WatchDiff diff = diffSnapshots(committed, probe, WatchSnapshot{}, /*nowTicks=*/0, /*settleTicks=*/2000);
    REQUIRE(diff.changes.size() == 2);
    CHECK(diff.changes[0].path == "gone");
    CHECK(diff.changes[0].kind == WatchChangeKind::Removed);
    CHECK(diff.changes[1].path == "new");
    CHECK(diff.changes[1].kind == WatchChangeKind::Added);
}

TEST_CASE("asset_watcher: a path that changes KIND is a settled Modified carrying the probe's kind (AW21, A4)") {
    const WatchSnapshot committed = snapshotOf({fileEntry("x", 1, -5000)});
    const WatchSnapshot probe = snapshotOf({dirEntry("x")});
    const WatchDiff diff = diffSnapshots(committed, probe, WatchSnapshot{}, /*nowTicks=*/0, /*settleTicks=*/2000);
    REQUIRE(diff.changes.size() == 1);
    CHECK(diff.changes[0].kind == WatchChangeKind::Modified);
    CHECK(diff.changes[0].isDirectory);
}

TEST_CASE("asset_watcher: one settled and one unsettled change in the same diff (AW22, AC-10)") {
    const WatchSnapshot committed = snapshotOf({fileEntry("settled", 1, -5000), fileEntry("unsettled", 1, -5000)});
    const WatchSnapshot probe = snapshotOf({fileEntry("settled", 2, -5000), fileEntry("unsettled", 1, -5000, false)});
    const WatchDiff diff = diffSnapshots(committed, probe, probe, /*nowTicks=*/0, /*settleTicks=*/2000);
    REQUIRE(diff.changes.size() == 1);
    CHECK(diff.changes[0].path == "settled");
    CHECK(diff.unsettled == 1);
}

// ---- applyChanges -------------------------------------------------------------------------------

TEST_CASE("asset_watcher: applyChanges with one Added (AW23)") {
    const WatchSnapshot committed;
    const WatchSnapshot probe = snapshotOf({fileEntry("a", 1, -5000)});
    WatchDiff diff;
    diff.changes.push_back(WatchChange{.path = "a", .kind = WatchChangeKind::Added, .isDirectory = false});
    const WatchSnapshot result = applyChanges(committed, probe, diff);
    REQUIRE(result.entries.size() == 1);
    CHECK(result.entries[0].path == "a");
}

TEST_CASE("asset_watcher: applyChanges with one Removed (AW24)") {
    const WatchSnapshot committed = snapshotOf({fileEntry("a", 1, -5000)});
    const WatchSnapshot probe;
    WatchDiff diff;
    diff.changes.push_back(WatchChange{.path = "a", .kind = WatchChangeKind::Removed, .isDirectory = false});
    const WatchSnapshot result = applyChanges(committed, probe, diff);
    CHECK(result.entries.empty());
}

TEST_CASE("asset_watcher: applyChanges keeps the result sorted and an untouched neighbour intact (AW25)") {
    const WatchSnapshot committed = snapshotOf({fileEntry("a", 1, -5000), fileEntry("m", 1, -5000)});
    const WatchSnapshot probe = snapshotOf({fileEntry("a", 2, -5000), fileEntry("m", 1, -5000)});
    WatchDiff diff;
    diff.changes.push_back(WatchChange{.path = "a", .kind = WatchChangeKind::Modified, .isDirectory = false});
    const WatchSnapshot result = applyChanges(committed, probe, diff);
    REQUIRE(result.entries.size() == 2);
    CHECK(result.entries[0].path == "a");
    CHECK(result.entries[0].size == 2);
    CHECK(result.entries[1].path == "m");
    CHECK(result.entries[1].size == 1);  // untouched neighbour keeps its old stamp
}

TEST_CASE("asset_watcher: applyChanges' flags come from probe, never from committed (AW26)") {
    WatchSnapshot committed;
    committed.truncated = false;
    committed.depthLimited = false;
    committed.unreadableDirs = 0;
    WatchSnapshot probe;
    probe.truncated = true;
    probe.depthLimited = true;
    probe.unreadableDirs = 3;
    const WatchDiff diff;
    const WatchSnapshot result = applyChanges(committed, probe, diff);
    CHECK(result.truncated);
    CHECK(result.depthLimited);
    CHECK(result.unreadableDirs == 3);
}

// ---- the exact settlement boundary and overflow safety (A3) --------------------------------------

TEST_CASE("asset_watcher: isSettled at EXACTLY nowTicks - mtime == settleTicks is TRUE (AW27)") {
    CHECK(isSettled(fileEntry("a", 1, /*mtime=*/0), nullptr, /*nowTicks=*/2000, /*settleTicks=*/2000));
}

TEST_CASE("asset_watcher: settleTicks == 0 makes the age test vacuous (AW28)") {
    CHECK(isSettled(fileEntry("a", 1, /*mtime=*/0), nullptr, /*nowTicks=*/0, /*settleTicks=*/0));
}

TEST_CASE("asset_watcher: a future-dated stamp is never old enough, and never overflows (AW28b, A3)") {
    CHECK_FALSE(isSettled(fileEntry("a", 1, /*mtime=*/1000), nullptr, /*nowTicks=*/0, /*settleTicks=*/0));
}

TEST_CASE("asset_watcher: the age subtraction does not abort under UBSan on extreme values (AW28c, A3)") {
    const WatchEntry extreme = fileEntry("a", 1, /*mtime=*/std::numeric_limits<std::int64_t>::min());
    CHECK(isSettled(extreme, nullptr, /*nowTicks=*/0, /*settleTicks=*/0));
}

// =====================================================================================================
// ---- Step 3: the AssetWatcher class, against a real scratch TempDir with an injected clock ----------
// =====================================================================================================

namespace {

// A unique temp directory that removes itself on destruction -- the NINTH TU-local copy of this shape
// (project_test.cpp:130-162's precedent; scaffolding is copied, the ASSERTION is shared).
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_asset_watcher_test_" + std::to_string(++counter));
        std::filesystem::remove_all(dirPath, ec);
        std::filesystem::create_directories(dirPath, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dirPath, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return dirPath; }

    [[nodiscard]] std::string utf8() const {
        const std::u8string bytes = dirPath.u8string();
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    [[nodiscard]] std::string join(std::string_view leaf) const {
        std::string result = utf8();
        result += '/';
        result += leaf;
        return result;
    }

    void write(std::string_view relativeUtf8, std::string_view contents) const {
        const std::filesystem::path full = relPath(relativeUtf8);
        std::error_code ec;
        std::filesystem::create_directories(full.parent_path(), ec);
        std::ofstream stream(full, std::ios::binary | std::ios::trunc);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        // R9: the stream is a LOCAL, closed here at scope exit -- MSVC cannot remove/rewrite a file
        // still open elsewhere.
    }

    void makeDir(std::string_view relativeUtf8) const {
        std::error_code ec;
        std::filesystem::create_directories(relPath(relativeUtf8), ec);
    }

    void remove(std::string_view relativeUtf8) const {
        std::error_code ec;
        std::filesystem::remove_all(relPath(relativeUtf8), ec);
    }

private:
    [[nodiscard]] std::filesystem::path relPath(std::string_view relativeUtf8) const {
        const std::u8string rel(reinterpret_cast<const char8_t*>(relativeUtf8.data()), relativeUtf8.size());
        return dirPath / std::filesystem::path(rel);
    }

    std::filesystem::path dirPath;
};

// The synthetic clock. NO TEST SLEEPS: every case advances this by hand and passes it to poll().
// Values are in file_time_type ticks, so a "second" is fileTimeTicksFromMillis(1000).
struct FakeClock {
    std::int64_t now = 0;
    void advanceMs(std::int64_t ms) { now += engine::editor::fileTimeTicksFromMillis(ms); }
};

// Drives poll() until a sweep completes (or `maxPolls` is exhausted, which FAILS the case rather than
// hanging). `deltaSeconds` is what the cooldown consumes; 1.0F drains a 1000 ms cooldown in one call.
// Returns poll()'s value on the completing call.
[[nodiscard]] bool pollUntilSweep(engine::editor::AssetWatcher& w, const FakeClock& clock, float deltaSeconds = 1.0F,
                                  int maxPolls = 200) {
    const std::uint64_t before = w.status().sweepsCompleted;
    for (int i = 0; i < maxPolls; ++i) {
        const bool fired = w.poll(deltaSeconds, clock.now);
        if (w.status().sweepsCompleted != before) {
            return fired;
        }
    }
    FAIL("pollUntilSweep exhausted its budget -- the sweep never completed");
    return false;
}

// A disclosed, minor addition to the plan's literal FakeClock{now = 0} default: every case below
// drives a REAL TempDir, so every WatchEntry's mtime is a REAL wall-clock stamp from
// currentFileTimeTicks()'s own domain -- never zero. A FakeClock left at its zero default would place
// every real file's mtime in the "future" relative to it, and isSettled's very first age guard
// (`current.mtime > nowTicks`) would then refuse EVERY real entry forever, regardless of settleMs.
// Every case therefore resyncs `clock.now` to `currentFileTimeTicks()` immediately before a sweep it
// expects to observe a fresh write -- keeping the clock in the SAME domain (PF-c5 is the proof the two
// agree) while remaining fully injected and controllable. This is still "no test sleeps": every wait
// below is on a completed-sweep COUNT, never on wall-clock time.
void syncNow(FakeClock& clock) { clock.now = engine::editor::currentFileTimeTicks(); }

}  // namespace

TEST_CASE("asset_watcher: setRoot establishes a baseline without triggering (AW29, AC-26)") {
    const TempDir tmp;
    tmp.write("a.txt", "hello");
    tmp.write("sub/b.txt", "world");

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});
    watcher.setRoot(tmp.utf8(), tmp.utf8());

    FakeClock clock;
    syncNow(clock);
    const bool fired = pollUntilSweep(watcher, clock);

    CHECK_FALSE(fired);
    CHECK(watcher.status().sweepsCompleted == 1);
    CHECK(watcher.status().triggers == 0);
}

TEST_CASE("asset_watcher: a created file is detected exactly once and never again (AW30, AC-1/AC-19)") {
    const TempDir tmp;
    tmp.write("a.txt", "hello");

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});
    watcher.setRoot(tmp.utf8(), tmp.utf8());
    FakeClock clock;
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);  // baseline, priming -- never a trigger

    tmp.write("new.txt", "content");
    syncNow(clock);
    // settleMs == 0: an ADDITION settles on the FIRST sweep that sees it (§A-2).
    const bool firstSweepFired = pollUntilSweep(watcher, clock);
    CHECK(firstSweepFired);

    syncNow(clock);
    const bool secondSweepFired = pollUntilSweep(watcher, clock);
    CHECK_FALSE(secondSweepFired);

    for (int i = 0; i < 5; ++i) {
        syncNow(clock);
        CHECK_FALSE(pollUntilSweep(watcher, clock));
    }
    CHECK(watcher.status().triggers == 1);
}

// AC-6's ONLY proof, added by the code-review round. §T cited AW30/AW33 for it, but neither reaches
// the criterion: AW30 writes at the assets ROOT, AW33 creates and removes a directory at the ROOT, and
// the one case that writes into a subdirectory at all (AW29's "sub/b.txt") does so in its BASELINE and
// asserts the opposite -- that nothing fires. Nothing anywhere wrote into a subdirectory AFTER a
// baseline and asserted a fire, so "the sweep covers the whole assets tree, not the visible subset"
// was structurally certain (the file arm has no depth dependence) but entirely unasserted.
TEST_CASE("asset_watcher: a change inside a SUBDIRECTORY is detected (AW30b, AC-6)") {
    const TempDir tmp;
    tmp.write("a.txt", "hello");
    tmp.write("sub/deep/b.txt", "world");  // TWO levels down: the sweep must descend, not just list

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});
    watcher.setRoot(tmp.utf8(), tmp.utf8());
    FakeClock clock;
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);  // baseline, priming -- never a trigger
    CHECK(watcher.status().triggers == 0);

    // A brand-new file nested two directories below the root, with nothing at the root changing.
    tmp.write("sub/deep/new.txt", "content");
    syncNow(clock);
    CHECK(pollUntilSweep(watcher, clock));

    // And a MODIFICATION nested just as deep -- the path that needs both settlement conditions, where
    // an addition needs only the age one (§A-2). Modifying takes two sweeps at settleMs == 0: one to
    // observe the new stamp, one to find it stable against that observation.
    const std::uint64_t afterAdd = watcher.status().triggers;
    tmp.write("sub/deep/b.txt", "world, but genuinely longer now");
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);
    CHECK(watcher.status().triggers > afterAdd);
}

TEST_CASE("asset_watcher: a removed file is detected (AW31, AC-3)") {
    const TempDir tmp;
    tmp.write("a.txt", "hello");
    tmp.write("gone.txt", "bye");

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});
    watcher.setRoot(tmp.utf8(), tmp.utf8());
    FakeClock clock;
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);

    tmp.remove("gone.txt");
    syncNow(clock);
    const bool fired = pollUntilSweep(watcher, clock);
    CHECK(fired);
    REQUIRE(watcher.lastDiff().changes.size() == 1);
    CHECK(watcher.lastDiff().changes[0].kind == WatchChangeKind::Removed);
    CHECK(watcher.status().triggers == 1);
}

TEST_CASE("asset_watcher: a content change fires on the SECOND sweep -- condition 1 (AW32, AC-2/AC-7)") {
    const TempDir tmp;
    tmp.write("a.txt", "hello");

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});
    watcher.setRoot(tmp.utf8(), tmp.utf8());
    FakeClock clock;
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);

    tmp.write("a.txt", "hello, much longer than before now -- a genuinely different size");
    syncNow(clock);
    const bool firstFired = pollUntilSweep(watcher, clock);
    CHECK_FALSE(firstFired);  // unstable this sweep -- condition 1 needs ONE prior observation first

    syncNow(clock);
    const bool secondFired = pollUntilSweep(watcher, clock);
    CHECK(secondFired);
    REQUIRE(watcher.lastDiff().changes.size() == 1);
    CHECK(watcher.lastDiff().changes[0].kind == WatchChangeKind::Modified);
    CHECK(watcher.status().triggers == 1);
}

TEST_CASE("asset_watcher: a directory add and a directory remove are each detected (AW33, AC-4)") {
    const TempDir tmp;
    tmp.write("a.txt", "hello");

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});
    watcher.setRoot(tmp.utf8(), tmp.utf8());
    FakeClock clock;
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);

    tmp.makeDir("sub");
    syncNow(clock);
    CHECK(pollUntilSweep(watcher, clock));
    REQUIRE(watcher.lastDiff().changes.size() == 1);
    CHECK(watcher.lastDiff().changes[0].kind == WatchChangeKind::Added);
    CHECK(watcher.lastDiff().changes[0].isDirectory);

    tmp.remove("sub");
    syncNow(clock);
    CHECK(pollUntilSweep(watcher, clock));
    REQUIRE(watcher.lastDiff().changes.size() == 1);
    CHECK(watcher.lastDiff().changes[0].kind == WatchChangeKind::Removed);
    CHECK(watcher.lastDiff().changes[0].isDirectory);
}

TEST_CASE(
    "asset_watcher: hidden files, .aero-tmp files and Library/ inside the assets root never fire "
    "(AW34, AC-13/AC-14/AC-16)") {
    const TempDir tmp;
    tmp.write("a.txt", "hello");

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});
    // project root == assets root, so Library/ sits INSIDE the watched tree -- the ONE configuration
    // where the exclusion can be exercised at all (AC-16).
    watcher.setRoot(tmp.utf8(), tmp.utf8());
    FakeClock clock;
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);

    // (a) a hidden file.
    tmp.write(".hidden", "secret");
    syncNow(clock);
    CHECK_FALSE(pollUntilSweep(watcher, clock));

    // (b) an .aero-tmp file.
    tmp.write("x.png.aero-tmp", "transient");
    syncNow(clock);
    CHECK_FALSE(pollUntilSweep(watcher, clock));

    // (c) THE single most important assertion in the whole tier: without the canonical Library/
    // exclusion this fires forever, since every scan legitimately rewrites Library/asset-cache.json.
    tmp.makeDir("Library");
    tmp.write("Library/asset-cache.json", "{}");
    syncNow(clock);
    CHECK_FALSE(pollUntilSweep(watcher, clock));
    CHECK(watcher.status().triggers == 0);
}

TEST_CASE("asset_watcher: a .meta sidecar change DOES trigger (AW35, AC-15)") {
    const TempDir tmp;
    tmp.write("wood.png", "pixels");

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});
    watcher.setRoot(tmp.utf8(), tmp.utf8());
    FakeClock clock;
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);

    tmp.write("wood.png.meta", "{\"version\": 1}");
    syncNow(clock);
    CHECK(pollUntilSweep(watcher, clock));
    CHECK(watcher.status().triggers == 1);
}

TEST_CASE("asset_watcher: no single poll() enumerates more than dirsPerPoll directories (AW36, AC-22)") {
    const TempDir tmp;
    tmp.makeDir("d1");
    tmp.makeDir("d2");
    tmp.makeDir("d3");
    tmp.makeDir("d4");

    engine::editor::AssetWatcher watcher;
    // dirsPerPoll = 1: the root itself is the first directory processed, so a 5-directory tree (root +
    // 4 children) needs exactly 5 poll() calls to complete one sweep.
    watcher.configure({.dirsPerPoll = 1, .cooldownMs = 0, .settleMs = 0});
    watcher.setRoot(tmp.utf8(), tmp.utf8());

    FakeClock clock;
    syncNow(clock);
    std::size_t previousRemaining = std::numeric_limits<std::size_t>::max();
    for (int i = 0; i < 4; ++i) {
        const bool fired = watcher.poll(1.0F, clock.now);
        CHECK_FALSE(fired);
        CHECK(watcher.status().sweepsCompleted == 0);
        CHECK(watcher.status().phase == engine::editor::WatchPhase::Sweeping);
        CHECK(watcher.status().dirsRemaining < previousRemaining);
        previousRemaining = watcher.status().dirsRemaining;
    }
    const bool fifthFired = watcher.poll(1.0F, clock.now);
    CHECK_FALSE(fifthFired);  // priming sweep, never a trigger
    CHECK(watcher.status().sweepsCompleted == 1);
}

TEST_CASE(
    "asset_watcher: a cooldown separates completed sweeps, and focus-regain cancels it "
    "(AW37, AC-24/AC-25)") {
    const TempDir tmp;
    tmp.write("a.txt", "hello");

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 1000, .settleMs = 0});
    watcher.setRoot(tmp.utf8(), tmp.utf8());
    FakeClock clock;
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);  // the baseline sweep; cooldownRemainingMs is now 1000

    for (int i = 0; i < 9; ++i) {
        const bool fired = watcher.poll(0.1F, clock.now);
        CHECK_FALSE(fired);
        CHECK(watcher.status().sweepsCompleted == 1);
        CHECK(watcher.status().phase == engine::editor::WatchPhase::Cooldown);
    }
    const bool tenthFired = watcher.poll(0.1F, clock.now);
    CHECK_FALSE(tenthFired);
    CHECK(watcher.status().sweepsCompleted == 2);

    watcher.requestImmediateSweep();
    const bool afterFocusFired = watcher.poll(0.0F, clock.now);
    CHECK_FALSE(afterFocusFired);
    CHECK(watcher.status().sweepsCompleted == 3);
}

TEST_CASE("asset_watcher: the whole batch defers until maxDeferredSweeps, then FORCES (AW38, AC-11)") {
    const TempDir tmp;
    tmp.write("baseline.txt", "keep");

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0, .maxDeferredSweeps = 3});
    watcher.setRoot(tmp.utf8(), tmp.utf8());
    FakeClock clock;
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);  // baseline priming, never a trigger

    tmp.write("settled.txt", "will settle");
    // Forward-date "unsettled.txt" into the future relative to any nowTicks this case will ever pass
    // -- the PERMANENTLY-unsettled entry the case needs, without pinning (and thereby desyncing) the
    // clock used for the settled entry above.
    tmp.write("unsettled.txt", "never settles");
    {
        std::error_code ec;
        const auto future = std::filesystem::file_time_type::clock::now() + std::chrono::hours(1);
        std::filesystem::last_write_time(tmp.path() / "unsettled.txt", future, ec);
        REQUIRE_FALSE(ec);
    }

    for (std::uint32_t sweep = 1; sweep <= 3; ++sweep) {
        syncNow(clock);
        const bool fired = watcher.poll(1.0F, clock.now);
        CHECK_FALSE(fired);
        CHECK(watcher.status().deferredSweeps == sweep);
    }

    syncNow(clock);
    const bool forcedFired = watcher.poll(1.0F, clock.now);
    CHECK(forcedFired);
    CHECK(watcher.status().deferredSweeps == 0);
    CHECK(watcher.status().triggers == 1);
    REQUIRE(watcher.lastDiff().changes.size() == 1);
    CHECK(watcher.lastDiff().changes[0].path == "settled.txt");
}

TEST_CASE(
    "asset_watcher: after a forced fire, the unsettled entry is reported again and the settled "
    "one is not (AW39, AC-12/INV-W5)") {
    const TempDir tmp;
    tmp.write("baseline.txt", "keep");

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0, .maxDeferredSweeps = 3});
    watcher.setRoot(tmp.utf8(), tmp.utf8());
    FakeClock clock;
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);

    tmp.write("settled.txt", "will settle");
    tmp.write("unsettled.txt", "never settles");
    {
        std::error_code ec;
        const auto future = std::filesystem::file_time_type::clock::now() + std::chrono::hours(1);
        std::filesystem::last_write_time(tmp.path() / "unsettled.txt", future, ec);
        REQUIRE_FALSE(ec);
    }
    for (int i = 0; i < 4; ++i) {  // three deferrals + the forced fire (AW38's own sequence)
        syncNow(clock);
        (void)watcher.poll(1.0F, clock.now);
    }
    REQUIRE(watcher.status().triggers == 1);

    // The unsettled entry's stamp never entered `committed`, so it is a fresh Added again -- reported,
    // never committed. The settled entry, now IN `committed`, produces no further change.
    syncNow(clock);
    (void)watcher.poll(1.0F, clock.now);
    CHECK(watcher.status().lastUnsettled == 1);
    bool settledReportedAgain = false;
    for (const WatchChange& change : watcher.lastDiff().changes) {
        if (change.path == "settled.txt") {
            settledReportedAgain = true;
        }
    }
    CHECK_FALSE(settledReportedAgain);
}

TEST_CASE(
    "asset_watcher: enabled == false makes poll() a no-op; re-enabling starts immediately "
    "(AW40, AC-28)") {
    const TempDir tmp;
    tmp.write("a.txt", "hello");

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});
    watcher.setRoot(tmp.utf8(), tmp.utf8());
    watcher.setEnabled(false);

    FakeClock clock;
    syncNow(clock);
    for (int i = 0; i < 10; ++i) {
        const bool fired = watcher.poll(1.0F, clock.now);
        CHECK_FALSE(fired);
        CHECK(watcher.status().phase == engine::editor::WatchPhase::Disabled);
    }
    CHECK(watcher.status().sweepsCompleted == 0);

    watcher.setEnabled(true);
    syncNow(clock);
    const bool firstEnabledFired = watcher.poll(1.0F, clock.now);
    CHECK_FALSE(firstEnabledFired);  // priming
    CHECK(watcher.status().sweepsCompleted == 1);
}

TEST_CASE(
    "asset_watcher: an empty assets root, a different assets root, and a changed PROJECT root "
    "(AW41, AC-26/AC-29)") {
    const TempDir tmp;
    tmp.makeDir("assets");
    tmp.write("assets/a.txt", "hello");

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});

    // (a) an empty assets root is a no-op.
    watcher.setRoot(tmp.utf8(), "");
    FakeClock clock;
    syncNow(clock);
    CHECK_FALSE(watcher.poll(1.0F, clock.now));
    CHECK(watcher.status().phase == engine::editor::WatchPhase::Disabled);

    // (b) a DIFFERENT non-empty assets root primes silently -- the real project/assets relationship,
    // where "assetsRoot + /.." happens to equal the real project root (so a bug deriving it that way
    // could not be caught by this arm alone).
    watcher.setRoot(tmp.utf8(), tmp.join("assets"));
    syncNow(clock);
    const bool primedFired = pollUntilSweep(watcher, clock);
    CHECK_FALSE(primedFired);
    CHECK(watcher.status().triggers == 0);

    // (c) change ONLY the project root, to be the SAME directory as the assets root -- so
    // "assetsRoot + /.." no longer agrees with the real project root at all. If the exclusion were
    // ever derived from the assets root instead of the STORED project root, it would exclude the
    // WRONG directory (tmp/Library) and this Library/ (tmp/assets/Library -- the correct location once
    // project root == assets root) would wrongly fire.
    watcher.setRoot(tmp.join("assets"), tmp.join("assets"));
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);  // re-primes silently

    tmp.makeDir("assets/Library");
    tmp.write("assets/Library/asset-cache.json", "{}");
    syncNow(clock);
    CHECK_FALSE(pollUntilSweep(watcher, clock));
    CHECK(watcher.status().triggers == 0);
}

TEST_CASE("asset_watcher: noteExternalScan absorbs a change so it is not reported twice (AW42, AC-27)") {
    const TempDir tmp;
    tmp.write("a.txt", "hello");

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});
    watcher.setRoot(tmp.utf8(), tmp.utf8());
    FakeClock clock;
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);

    tmp.write("new.txt", "content");
    syncNow(clock);
    CHECK(pollUntilSweep(watcher, clock));  // settleMs == 0: fires on the first sweep that sees it

    watcher.noteExternalScan();
    syncNow(clock);
    CHECK_FALSE(pollUntilSweep(watcher, clock));
    CHECK(watcher.status().triggers == 1);
}

TEST_CASE(
    "asset_watcher: an unreadable sub-directory carries its children forward "
    "(AW43, AC-20, POSIX only)") {
    const TempDir tmp;
    tmp.makeDir("locked");
    tmp.write("locked/one.txt", "a");
    tmp.write("locked/two.txt", "b");

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});
    watcher.setRoot(tmp.utf8(), tmp.utf8());
    FakeClock clock;
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);  // baseline: commits both files under locked/

    std::error_code ec;
    std::filesystem::permissions(tmp.path() / "locked", std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);

    syncNow(clock);
    const bool fired = watcher.poll(1.0F, clock.now);

    // Restore BEFORE any assertion can fail loudly and BEFORE ~TempDir runs (asset_database_test.cpp's
    // AD4 rule).
    std::error_code restoreEc;
    std::filesystem::permissions(tmp.path() / "locked", std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, restoreEc);

    if (watcher.status().unreadableDirs == 0) {
        MESSAGE("running as a user for whom chmod 000 does not block reads (e.g. root) -- seed did not land");
        return;
    }
    CHECK_FALSE(fired);
    CHECK(watcher.status().unreadableDirs == 1);

    syncNow(clock);
    const bool secondFired = pollUntilSweep(watcher, clock);
    CHECK_FALSE(secondFired);  // the two files were carried forward -- no change reported for them
}

TEST_CASE("asset_watcher: an unreadable ROOT aborts the sweep (AW44, AC-21)") {
    const TempDir tmp;
    tmp.write("a.txt", "hello");
    const std::string assetsRoot = tmp.join("assets_root_that_will_vanish");
    std::error_code mkEc;
    std::filesystem::create_directories(std::filesystem::path(assetsRoot), mkEc);
    REQUIRE_FALSE(mkEc);

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});
    watcher.setRoot(tmp.utf8(), assetsRoot);
    FakeClock clock;
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);  // baseline over the empty root

    std::error_code removeEc;
    std::filesystem::remove_all(std::filesystem::path(assetsRoot), removeEc);
    REQUIRE_FALSE(removeEc);

    syncNow(clock);
    const bool fired = watcher.poll(1.0F, clock.now);
    CHECK_FALSE(fired);
    CHECK(watcher.status().rootUnreadable);
    CHECK(watcher.status().triggers == 0);

    // Restore the root and confirm the next sweep reports nothing -- `committed` was untouched.
    std::error_code restoreEc;
    std::filesystem::create_directories(std::filesystem::path(assetsRoot), restoreEc);
    REQUIRE_FALSE(restoreEc);
    syncNow(clock);
    CHECK_FALSE(pollUntilSweep(watcher, clock));
}

TEST_CASE(
    "asset_watcher: a symlinked directory alias is skipped and silent "
    "(AW45, AC-17, symlink-capable hosts only)") {
    const TempDir tmp;
    tmp.makeDir("real");
    tmp.write("real/a.png", "pixels");

    std::error_code ec;
    std::filesystem::create_directory_symlink(tmp.path() / "real", tmp.path() / "alias", ec);
    if (ec) {
        MESSAGE("skipped: this platform/filesystem refuses create_directory_symlink (Windows needs Developer Mode)");
        return;
    }

    engine::editor::AssetWatcher watcher;
    watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});
    watcher.setRoot(tmp.utf8(), tmp.utf8());
    FakeClock clock;
    syncNow(clock);
    (void)pollUntilSweep(watcher, clock);
    CHECK(watcher.status().entriesSeen == 2);  // "real" dir + "real/a.png" -- the alias contributes NOTHING

    syncNow(clock);
    CHECK_FALSE(pollUntilSweep(watcher, clock));
}

TEST_CASE(
    "asset_watcher: the entry, depth and directory ceilings are respected and surfaced "
    "(AW46, AC-18)") {
    // (a) maxEntries over a small tree.
    {
        const TempDir tmp;
        tmp.write("a.txt", "1");
        tmp.write("b.txt", "2");
        tmp.write("c.txt", "3");
        tmp.write("d.txt", "4");
        tmp.write("e.txt", "5");

        engine::editor::AssetWatcher watcher;
        watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0, .maxEntries = 2});
        watcher.setRoot(tmp.utf8(), tmp.utf8());
        FakeClock clock;
        syncNow(clock);
        (void)pollUntilSweep(watcher, clock);
        CHECK(watcher.status().truncated);
    }

    // (b) a tree nested past MAX_TREE_DEPTH (32) -- build 34 nested directories to guarantee it.
    {
        const TempDir tmp;
        std::filesystem::path deep = tmp.path();
        std::error_code ec;
        for (int i = 0; i < 34; ++i) {
            deep /= ("d" + std::to_string(i));
        }
        std::filesystem::create_directories(deep, ec);
        REQUIRE_FALSE(ec);

        engine::editor::AssetWatcher watcher;
        watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0});
        watcher.setRoot(tmp.utf8(), tmp.utf8());
        FakeClock clock;
        syncNow(clock);
        (void)pollUntilSweep(watcher, clock);
        CHECK(watcher.status().depthLimited);
    }

    // (c) maxDirs over a 3-directory tree.
    {
        const TempDir tmp;
        tmp.makeDir("d1");
        tmp.makeDir("d2");
        tmp.makeDir("d3");

        engine::editor::AssetWatcher watcher;
        watcher.configure({.dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0, .maxDirs = 1});
        watcher.setRoot(tmp.utf8(), tmp.utf8());
        FakeClock clock;
        syncNow(clock);
        (void)pollUntilSweep(watcher, clock);
        CHECK(watcher.status().truncated);
    }
}

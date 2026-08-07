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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
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

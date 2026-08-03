// tests/editor/asset_database_test.cpp -- task 3.1.1: AssetDatabase's scan over a real filesystem
// tree. A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED (D4/AC-17/INV-P5, the project_test.cpp precedent): asset_database.hpp depends on nothing
// but asset_meta.hpp and project_files.hpp, neither of which needs reflection, so every case here
// must be PRESENT and PASSING in all three build configurations -- prove it with --list-test-cases.
// Tier-0: no GPU, no window, no ImGui context. Real, bounded disk I/O through a scratch TempDir.
#include <aero/core/guid.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/text_file.hpp>

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using engine::formatGuid;
using engine::Guid;
using engine::GuidGenerator;
using engine::editor::ASSET_META_SUFFIX;
using engine::editor::AssetDatabase;
using engine::editor::AssetMetaState;
using engine::editor::AssetRecord;
using engine::editor::AssetScanReport;
using engine::editor::fileExists;
using engine::editor::ScanStatus;
using engine::editor::writeMetaText;

namespace {

// A unique temp directory that removes itself on destruction -- the SEVENTH TU-local copy of this
// shape (project_test.cpp:130-162's precedent; scaffolding is copied, the ASSERTION is shared).
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_asset_db_test_" + std::to_string(++counter));
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

    [[nodiscard]] std::string utf8() const {
        const std::u8string bytes = dirPath.u8string();
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
    [[nodiscard]] std::string join(std::string_view leaf) const {
        std::string result = utf8();
        result += '/';
        result += leaf;
        return result;
    }

private:
    std::filesystem::path dirPath;
};

void writeFile(std::string_view absolutePath, std::string_view bytes) {
    std::ofstream out(std::filesystem::path(std::string(absolutePath)), std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

struct Stat {
    bool exists = false;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
};

[[nodiscard]] Stat statOf(std::string_view absolutePath) {
    std::error_code ec;
    const std::string pathText(absolutePath);
    const std::filesystem::path path{pathText};
    Stat stat;
    stat.exists = std::filesystem::exists(path, ec) && !ec;
    if (!stat.exists) {
        return stat;
    }
    stat.size = std::filesystem::file_size(path, ec);
    stat.mtime = std::filesystem::last_write_time(path, ec);
    return stat;
}

}  // namespace

// ---- E1-E6: the guard and the empty tree ------------------------------------------------------

TEST_CASE("asset_database: rescan(\"\") is Missing, empty, zero writes (AD1, E1)") {
    AssetDatabase db;
    GuidGenerator gen(1);
    const AssetScanReport report = db.rescan("", gen);
    CHECK(report.status == ScanStatus::Missing);
    CHECK(db.size() == 0);
    CHECK(report.created == 0);
    CHECK(report.repaired == 0);
}

TEST_CASE("asset_database: a configured but absent root is Missing (AD2, E2)") {
    const TempDir dir;
    AssetDatabase db;
    GuidGenerator gen(2);
    const AssetScanReport report = db.rescan(dir.join("does-not-exist"), gen);
    CHECK(report.status == ScanStatus::Missing);
    CHECK(db.size() == 0);
    CHECK_FALSE(std::filesystem::exists(dir.join("does-not-exist")));  // nothing created on disk
}

TEST_CASE("asset_database: a root that is a FILE is NotADirectory (AD3, E3)") {
    const TempDir dir;
    writeFile(dir.join("not-a-dir"), "x");
    AssetDatabase db;
    GuidGenerator gen(3);
    const AssetScanReport report = db.rescan(dir.join("not-a-dir"), gen);
    CHECK(report.status == ScanStatus::NotADirectory);
    CHECK(db.size() == 0);
}

TEST_CASE("asset_database: an unreadable root is Unreadable (AD4, E4)") {
    const TempDir dir;
    const std::string target = dir.join("locked");
    std::error_code ec;
    std::filesystem::create_directory(target, ec);
    REQUIRE_FALSE(ec);
    std::filesystem::permissions(target, std::filesystem::perms::none, std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);

    AssetDatabase db;
    GuidGenerator gen(4);
    const AssetScanReport report = db.rescan(target, gen);

    // Restore BEFORE any assertion can fail loudly and BEFORE ~TempDir runs (project_test.cpp:980's rule).
    std::error_code restoreEc;
    std::filesystem::permissions(target, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace,
                                 restoreEc);

    if (report.status == ScanStatus::Ok) {
        MESSAGE("running as a user for whom chmod 000 does not block reads (e.g. root) -- seed did not land");
        return;
    }
    CHECK(report.status == ScanStatus::Unreadable);
    CHECK(db.size() == 0);
}

TEST_CASE("asset_database: an empty directory scans clean (AD5, E5)") {
    const TempDir dir;
    AssetDatabase db;
    GuidGenerator gen(5);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    CHECK(report.status == ScanStatus::Ok);
    CHECK(db.size() == 0);
    CHECK(report.created == 0);
}

// ---- create / write --------------------------------------------------------------------------

TEST_CASE("asset_database: three loose files are all Created, with sidecars on disk (AD6)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("b.png"), "b");
    writeFile(dir.join("c.png"), "c");

    AssetDatabase db;
    GuidGenerator gen(6);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    CHECK(report.status == ScanStatus::Ok);
    CHECK(db.size() == 3);
    CHECK(report.created == 3);
    for (const std::string_view leaf : {"a.png", "b.png", "c.png"}) {
        const AssetRecord* const record = db.findByPath(leaf);
        REQUIRE(record != nullptr);
        CHECK(record->state == AssetMetaState::Created);
        CHECK(record->guid.valid());
        CHECK(fileExists(dir.join(std::string(leaf) + std::string(ASSET_META_SUFFIX))));
    }
}

TEST_CASE("asset_database: a created sidecar's bytes equal writeMetaText(record.guid) (AD7)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(7);
    db.rescan(dir.utf8(), gen);
    const AssetRecord* const record = db.findByPath("a.png");
    REQUIRE(record != nullptr);
    const auto onDisk = scene_golden::readBytes(dir.join("a.png.meta"));
    REQUIRE(onDisk.ok);
    INFO(scene_golden::describeMismatch(writeMetaText(record->guid), onDisk.text));
    CHECK(onDisk.text == writeMetaText(record->guid));
}

TEST_CASE("asset_database: a second rescan of a fully-described tree writes NOTHING (AD8, D6/R-A3, seed S13)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("b.png"), "b");
    AssetDatabase db;
    GuidGenerator gen(8);
    const AssetScanReport first = db.rescan(dir.utf8(), gen);
    REQUIRE(first.created == 2);

    // Code-review finding 5: a D6 regression rewrites the SAME GUID, so `size` is identical either
    // way -- `mtime` is the ONLY discriminator, and on a coarse-granularity filesystem (or if both
    // scans land within one clock tick, which they easily can on a fast machine) a real rewrite could
    // pass this comparison anyway. Back-dating BEFORE the second scan removes both failure modes: a
    // real rewrite sets mtime to "now", which is provably later than an hour in the past regardless of
    // granularity or clock resolution.
    std::error_code aBackdateEc;
    std::error_code bBackdateEc;
    const auto backdated = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    std::filesystem::last_write_time(std::filesystem::path(dir.join("a.png.meta")), backdated, aBackdateEc);
    std::filesystem::last_write_time(std::filesystem::path(dir.join("b.png.meta")), backdated, bBackdateEc);
    REQUIRE_FALSE(aBackdateEc);
    REQUIRE_FALSE(bBackdateEc);

    const Stat aBefore = statOf(dir.join("a.png.meta"));
    const Stat bBefore = statOf(dir.join("b.png.meta"));
    REQUIRE(aBefore.exists);
    REQUIRE(bBefore.exists);
    CHECK(aBefore.mtime == backdated);  // the back-date actually landed -- not a vacuous proof
    CHECK(bBefore.mtime == backdated);

    const AssetScanReport second = db.rescan(dir.utf8(), gen);

    const Stat aAfter = statOf(dir.join("a.png.meta"));
    const Stat bAfter = statOf(dir.join("b.png.meta"));

    CHECK(second.created == 0);
    CHECK(aAfter.size == aBefore.size);
    CHECK(aAfter.mtime == aBefore.mtime);
    CHECK(bAfter.size == bBefore.size);
    CHECK(bAfter.mtime == bBefore.mtime);
}

TEST_CASE("asset_database: the second scan's GUIDs equal the first's, per path (AD9)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(9);
    db.rescan(dir.utf8(), gen);
    const std::optional<Guid> firstGuid = db.guidForPath("a.png");
    REQUIRE(firstGuid.has_value());
    db.rescan(dir.utf8(), gen);
    const std::optional<Guid> secondGuid = db.guidForPath("a.png");
    REQUIRE(secondGuid.has_value());
    CHECK(*firstGuid == *secondGuid);
}

// ---- invalid sidecars: never overwritten (D7) --------------------------------------------------

TEST_CASE("asset_database: unparseable JSON is Invalid and untouched (AD10, E7, seed S14)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("a.png.meta"), "not json at all");
    AssetDatabase db;
    GuidGenerator gen(10);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    CHECK(report.invalid == 1);
    const AssetRecord* const record = db.findByPath("a.png");
    REQUIRE(record != nullptr);
    CHECK(record->state == AssetMetaState::Invalid);
    CHECK(record->guid == Guid{});
    const auto onDisk = scene_golden::readBytes(dir.join("a.png.meta"));
    REQUIRE(onDisk.ok);
    CHECK(onDisk.text == "not json at all");  // byte-identical to what was written
}

TEST_CASE("asset_database: a zero-byte sidecar is Invalid, same rule, no carve-out (AD11, E8)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("a.png.meta"), "");
    AssetDatabase db;
    GuidGenerator gen(11);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    CHECK(report.invalid == 1);
    const auto onDisk = scene_golden::readBytes(dir.join("a.png.meta"));
    REQUIRE(onDisk.ok);
    CHECK(onDisk.text.empty());
}

TEST_CASE("asset_database: an unsupported version is Invalid, file untouched (AD12, E9)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    const std::string body = "{\n  \"version\": 2,\n  \"guid\": \"a3f1c07e5b8d42198e6f0c3d7a2b4b92\"\n}\n";
    writeFile(dir.join("a.png.meta"), body);
    AssetDatabase db;
    GuidGenerator gen(12);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    CHECK(report.invalid == 1);
    REQUIRE(report.invalidPaths.size() == 1);
    CHECK(report.invalidPaths[0].find('2') != std::string::npos);  // names the offending version
    const auto onDisk = scene_golden::readBytes(dir.join("a.png.meta"));
    REQUIRE(onDisk.ok);
    CHECK(onDisk.text == body);
}

TEST_CASE("asset_database: a dashed / short / nil guid is Invalid, file untouched (AD13, E10)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("b.png"), "b");
    writeFile(dir.join("c.png"), "c");
    const std::string dashed = "{\n  \"version\": 1,\n  \"guid\": \"a3f1c07e-5b8d-4219-8e6f-0c3d7a2b4b92\"\n}\n";
    const std::string shortGuid = "{\n  \"version\": 1,\n  \"guid\": \"a3f1\"\n}\n";
    const std::string nilGuid = "{\n  \"version\": 1,\n  \"guid\": \"" + std::string(32, '0') + "\"\n}\n";
    writeFile(dir.join("a.png.meta"), dashed);
    writeFile(dir.join("b.png.meta"), shortGuid);
    writeFile(dir.join("c.png.meta"), nilGuid);

    AssetDatabase db;
    GuidGenerator gen(13);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    CHECK(report.invalid == 3);
    for (const std::string_view leaf : {"a.png", "b.png", "c.png"}) {
        const AssetRecord* const record = db.findByPath(leaf);
        REQUIRE(record != nullptr);
        CHECK(record->state == AssetMetaState::Invalid);
    }
    CHECK(scene_golden::readBytes(dir.join("a.png.meta")).text == dashed);
    CHECK(scene_golden::readBytes(dir.join("b.png.meta")).text == shortGuid);
    CHECK(scene_golden::readBytes(dir.join("c.png.meta")).text == nilGuid);
}

TEST_CASE("asset_database: unknown keys are tolerated, GUID used, sidecar not rewritten (AD14, E11)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    const std::string body =
        "{\n  \"version\": 1,\n  \"importer\": \"texture\",\n  \"guid\": \"a3f1c07e5b8d42198e6f0c3d7a2b4b92\",\n  "
        "\"userData\": {}\n}\n";
    writeFile(dir.join("a.png.meta"), body);
    const Stat before = statOf(dir.join("a.png.meta"));

    AssetDatabase db;
    GuidGenerator gen(14);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);

    CHECK(report.unknownKeyTotal == 2);
    const AssetRecord* const record = db.findByPath("a.png");
    REQUIRE(record != nullptr);
    CHECK(record->state == AssetMetaState::Ok);
    CHECK(formatGuid(record->guid) == "a3f1c07e5b8d42198e6f0c3d7a2b4b92");
    const Stat after = statOf(dir.join("a.png.meta"));
    CHECK(after.mtime == before.mtime);
}

// ---- orphans: reported, never deleted (D8) ------------------------------------------------------

TEST_CASE("asset_database: an orphaned sidecar is reported and left alone (AD15, E12)") {
    const TempDir dir;
    const std::string body = "{\n  \"version\": 1,\n  \"guid\": \"a3f1c07e5b8d42198e6f0c3d7a2b4b92\"\n}\n";
    writeFile(dir.join("gone.png.meta"), body);
    AssetDatabase db;
    GuidGenerator gen(15);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    CHECK(report.orphanTotal == 1);
    REQUIRE(report.orphans.size() == 1);
    CHECK(report.orphans[0] == "gone.png.meta");
    CHECK(fileExists(dir.join("gone.png.meta")));
    CHECK(db.size() == 0);
}

// ---- duplicate repair (D9) -----------------------------------------------------------------------

TEST_CASE("asset_database: two assets sharing one GUID: first keeps it, other repaired (AD16, E13)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("z.png"), "z");
    const std::string body = "{\n  \"version\": 1,\n  \"guid\": \"a3f1c07e5b8d42198e6f0c3d7a2b4b92\"\n}\n";
    writeFile(dir.join("a.png.meta"), body);
    writeFile(dir.join("z.png.meta"), body);

    AssetDatabase db;
    GuidGenerator gen(16);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);

    CHECK(report.repaired == 1);
    const AssetRecord* const keeper = db.findByPath("a.png");
    const AssetRecord* const loser = db.findByPath("z.png");
    REQUIRE(keeper != nullptr);
    REQUIRE(loser != nullptr);
    CHECK(formatGuid(keeper->guid) == "a3f1c07e5b8d42198e6f0c3d7a2b4b92");
    CHECK(keeper->state == AssetMetaState::Ok);
    CHECK(loser->guid != keeper->guid);
    CHECK(loser->state == AssetMetaState::Repaired);

    REQUIRE(report.repairs.size() == 1);
    CHECK(report.repairs[0].find("z.png") != std::string::npos);
    CHECK(report.repairs[0].find("a3f1c07e5b8d42198e6f0c3d7a2b4b92") != std::string::npos);
}

TEST_CASE("asset_database: three assets, one GUID -> two repairs, mutually distinct (AD17, E14)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("m.png"), "m");
    writeFile(dir.join("z.png"), "z");
    const std::string body = "{\n  \"version\": 1,\n  \"guid\": \"a3f1c07e5b8d42198e6f0c3d7a2b4b92\"\n}\n";
    writeFile(dir.join("a.png.meta"), body);
    writeFile(dir.join("m.png.meta"), body);
    writeFile(dir.join("z.png.meta"), body);

    AssetDatabase db;
    GuidGenerator gen(17);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);

    CHECK(report.repaired == 2);
    const Guid mGuid = *db.guidForPath("m.png");
    const Guid zGuid = *db.guidForPath("z.png");
    CHECK(mGuid != zGuid);
    CHECK(mGuid.valid());
    CHECK(zGuid.valid());
}

TEST_CASE("asset_database: repair determinism -- WHICH asset is repaired is seed-independent (AD18)") {
    const std::string body = "{\n  \"version\": 1,\n  \"guid\": \"a3f1c07e5b8d42198e6f0c3d7a2b4b92\"\n}\n";

    const TempDir dirA;
    writeFile(dirA.join("a.png"), "a");
    writeFile(dirA.join("z.png"), "z");
    writeFile(dirA.join("a.png.meta"), body);
    writeFile(dirA.join("z.png.meta"), body);
    AssetDatabase dbA;
    GuidGenerator genA(101);
    dbA.rescan(dirA.utf8(), genA);

    const TempDir dirB;
    writeFile(dirB.join("a.png"), "a");
    writeFile(dirB.join("z.png"), "z");
    writeFile(dirB.join("a.png.meta"), body);
    writeFile(dirB.join("z.png.meta"), body);
    AssetDatabase dbB;
    GuidGenerator genB(202);  // a DIFFERENT seed
    dbB.rescan(dirB.utf8(), genB);

    CHECK(dbA.findByPath("z.png")->state == AssetMetaState::Repaired);
    CHECK(dbB.findByPath("z.png")->state == AssetMetaState::Repaired);
    CHECK(dbA.findByPath("a.png")->state == AssetMetaState::Ok);
    CHECK(dbB.findByPath("a.png")->state == AssetMetaState::Ok);
    // the new GUIDs need not (and generally will not) match -- only WHICH path was repaired does.
}

// ---- nesting, noise and bounds -----------------------------------------------------------------

TEST_CASE("asset_database: nested directories to depth 4 all get sidecars at the right path (AD19)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("a/b/c"), ec);
    REQUIRE_FALSE(ec);
    writeFile(dir.join("a/b/c/leaf.png"), "x");

    AssetDatabase db;
    GuidGenerator gen(19);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    CHECK(report.created == 1);
    const AssetRecord* const record = db.findByPath("a/b/c/leaf.png");
    REQUIRE(record != nullptr);
    CHECK(record->guid.valid());
    CHECK(fileExists(dir.join("a/b/c/leaf.png.meta")));
}

TEST_CASE("asset_database: a .git/ directory is never descended into (AD20, E20, seed S19)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join(".git/objects"), ec);
    REQUIRE_FALSE(ec);
    writeFile(dir.join(".git/objects/blob"), "x");
    writeFile(dir.join("visible.png"), "y");

    AssetDatabase db;
    GuidGenerator gen(20);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    CHECK(db.size() == 1);
    CHECK(report.filesSeen == 1);
    CHECK(db.findByPath("visible.png") != nullptr);
    CHECK_FALSE(fileExists(dir.join(".git/objects/blob.meta")));
}

TEST_CASE("asset_database: a left-behind *.aero-tmp is skipped, not deleted (AD21, E19, seed S18)") {
    // wood.png is given a REAL, valid, on-disk sidecar (Ok, no write) -- deliberately, NOT a bare
    // Created asset. writeTextFileAtomic's OWN working file for a CREATE of "wood.png.meta" is named
    // exactly "wood.png.meta.aero-tmp" (text_file.cpp): giving wood.png no sidecar here would make the
    // scan's own legitimate create-write reuse and rename this exact leftover into place, which
    // proves nothing about D16 -- it would merely be self-healing overwrite behaviour, not a skip.
    const TempDir dir;
    writeFile(dir.join("wood.png"), "w");
    const std::string body = "{\n  \"version\": 1,\n  \"guid\": \"a3f1c07e5b8d42198e6f0c3d7a2b4b92\"\n}\n";
    writeFile(dir.join("wood.png.meta"), body);
    writeFile(dir.join("wood.png.meta.aero-tmp"), "leftover");

    AssetDatabase db;
    GuidGenerator gen(21);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    CHECK(db.size() == 1);
    CHECK(report.filesSeen == 1);
    CHECK(report.created == 0);
    const AssetRecord* const record = db.findByPath("wood.png");
    REQUIRE(record != nullptr);
    CHECK(record->state == AssetMetaState::Ok);  // no write was ever attempted
    CHECK(db.findByPath("wood.png.meta.aero-tmp") == nullptr);
    CHECK(fileExists(dir.join("wood.png.meta.aero-tmp")));  // still on disk, untouched
    CHECK(scene_golden::readBytes(dir.join("wood.png.meta.aero-tmp")).text == "leftover");
}

TEST_CASE("asset_database: the OS-noise names are skipped (AD22, E21)") {
    const TempDir dir;
    writeFile(dir.join("Thumbs.db"), "t");
    writeFile(dir.join("desktop.ini"), "d");
    writeFile(dir.join("real.png"), "r");

    AssetDatabase db;
    GuidGenerator gen(22);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    CHECK(db.size() == 1);
    CHECK(report.filesSeen == 1);
    CHECK(db.findByPath("real.png") != nullptr);
}

TEST_CASE("asset_database: a listDirectory-level cap propagates into `truncated` (AD23, E24/E25)") {
    // MAX_ASSETS is 50000 (D14) -- creating 50001 real files plus 50001 real .meta writes is not
    // affordable inside a Debug/ASan test (measured at plan time: NOT lowering MAX_ASSETS for the
    // test -- a constant a test changes is a constant no test pins). Instead this drives the SAME
    // `truncated` flag through the reachable, cheap path: MAX_ENTRIES_PER_DIRECTORY (10000) is a
    // listDirectory-level cap that AssetDatabase folds into its own `truncated`, exactly like the
    // MAX_ASSETS path does -- both set the identical bit and both are surfaced the identical way.
    const TempDir dir;
    for (int i = 0; i < 10001; ++i) {
        writeFile(dir.join("f" + std::to_string(i) + ".png"), "x");
    }
    AssetDatabase db;
    GuidGenerator gen(23);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    CHECK(report.truncated);
}

TEST_CASE("asset_database: depth beyond MAX_TREE_DEPTH sets depthLimited (AD24, E23, seed S20)") {
    const TempDir dir;
    std::filesystem::path deep(dir.utf8());
    std::error_code ec;
    // MAX_TREE_DEPTH is 32; build 34 nested directories so the walk is guaranteed to exceed it.
    for (int i = 0; i < 34; ++i) {
        deep /= ("d" + std::to_string(i));
    }
    std::filesystem::create_directories(deep, ec);
    REQUIRE_FALSE(ec);
    // A shallow file (well within the limit) must still be identified.
    writeFile(dir.join("shallow.png"), "s");

    AssetDatabase db;
    GuidGenerator gen(24);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    CHECK(report.depthLimited);
    CHECK(db.findByPath("shallow.png") != nullptr);
}

TEST_CASE("asset_database: a read-only root: every write fails, nothing is removed (AD25, E26/E27)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("b.png"), "b");
    writeFile(dir.join("c.png"), "c");
    std::error_code ec;
    std::filesystem::permissions(dir.utf8(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);

    AssetDatabase db;
    GuidGenerator gen(25);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);

    std::error_code restoreEc;
    std::filesystem::permissions(dir.utf8(), std::filesystem::perms::owner_all, std::filesystem::perm_options::replace,
                                 restoreEc);

    if (report.writeFailureTotal == 0) {
        MESSAGE("running as a user for whom a read-only directory does not block file creation -- seed did not land");
        return;
    }
    CHECK(report.writeFailureTotal == 3);
    CHECK(db.size() == 3);
    for (const std::string_view leaf : {"a.png", "b.png", "c.png"}) {
        const AssetRecord* const record = db.findByPath(leaf);
        REQUIRE(record != nullptr);
        CHECK(record->guid.valid());        // kept its in-memory GUID even though the write failed
        CHECK(fileExists(dir.join(leaf)));  // nothing was removed
    }
}

// ---- accessors --------------------------------------------------------------------------------

TEST_CASE("asset_database: findByPath / guidForPath agree; a miss is nullopt (AD26)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(26);
    db.rescan(dir.utf8(), gen);

    const AssetRecord* const hit = db.findByPath("a.png");
    REQUIRE(hit != nullptr);
    const std::optional<Guid> guid = db.guidForPath("a.png");
    REQUIRE(guid.has_value());
    CHECK(*guid == hit->guid);

    CHECK(db.findByPath("missing.png") == nullptr);
    CHECK_FALSE(db.guidForPath("missing.png").has_value());
}

TEST_CASE("asset_database: findByGuid round-trips; nil and Invalid are unreachable (AD27, AC-31, seed S24)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("bad.png"), "b");
    writeFile(dir.join("bad.png.meta"), "not json");
    AssetDatabase db;
    GuidGenerator gen(27);
    db.rescan(dir.utf8(), gen);

    const AssetRecord* const good = db.findByPath("a.png");
    REQUIRE(good != nullptr);
    const AssetRecord* const foundByGuid = db.findByGuid(good->guid);
    REQUIRE(foundByGuid != nullptr);
    CHECK(foundByGuid->relativePath == "a.png");

    CHECK(db.findByGuid(Guid{}) == nullptr);

    const AssetRecord* const invalidRecord = db.findByPath("bad.png");
    REQUIRE(invalidRecord != nullptr);
    CHECK(invalidRecord->state == AssetMetaState::Invalid);
    CHECK(db.findByGuid(invalidRecord->guid) == nullptr);  // nil guid -> nullptr regardless
}

TEST_CASE("asset_database: rescanning a DIFFERENT root fully replaces the contents (AD28, E31)") {
    const TempDir dirA;
    writeFile(dirA.join("a.png"), "a");
    const TempDir dirB;
    writeFile(dirB.join("b.png"), "b");

    AssetDatabase db;
    GuidGenerator gen(28);
    db.rescan(dirA.utf8(), gen);
    REQUIRE(db.findByPath("a.png") != nullptr);
    const Guid oldGuid = *db.guidForPath("a.png");

    db.rescan(dirB.utf8(), gen);
    CHECK(db.findByPath("a.png") == nullptr);
    CHECK(db.findByGuid(oldGuid) == nullptr);
    CHECK(db.findByPath("b.png") != nullptr);
    CHECK(db.size() == 1);
}

TEST_CASE("asset_database: x.meta and x.META both present -- byte-first wins, other is an orphan (AD29, E29)") {
    const TempDir dir;
    writeFile(dir.join("x.png"), "x");
    const std::string body = "{\n  \"version\": 1,\n  \"guid\": \"a3f1c07e5b8d42198e6f0c3d7a2b4b92\"\n}\n";
    writeFile(dir.join("x.png.meta"), body);

    std::error_code ec;
    std::filesystem::copy_file(std::filesystem::path(dir.join("x.png.meta")),
                               std::filesystem::path(dir.join("x.png.META")), ec);
    if (ec) {
        // A case-INSENSITIVE filesystem collapsed the second write onto the first -- skip cleanly,
        // never a failure (plan §S3, step 3): there is only ever one file here on such a filesystem.
        MESSAGE("case-insensitive filesystem: x.png.META collided with x.png.meta, nothing to test here");
        return;
    }

    AssetDatabase db;
    GuidGenerator gen(29);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    const AssetRecord* const record = db.findByPath("x.png");
    REQUIRE(record != nullptr);
    CHECK(record->state == AssetMetaState::Ok);  // "x.png.META" < "x.png.meta" (byte order): it wins
    CHECK(report.orphanTotal == 1);
    REQUIRE(report.orphans.size() == 1);
    CHECK(report.orphans[0] == "x.png.meta");
}

TEST_CASE("asset_database: a non-ASCII filename round-trips end to end (AD30, E28)") {
    const TempDir dir;
    const std::string leaf = "\xF0\x9F\x9A\x80.png";  // an emoji leaf name
    writeFile(dir.join(leaf), "x");

    AssetDatabase db;
    GuidGenerator gen(30);
    const AssetScanReport report = db.rescan(dir.utf8(), gen);
    CHECK(report.created == 1);
    const AssetRecord* const record = db.findByPath(leaf);
    REQUIRE(record != nullptr);
    CHECK(record->relativePath == leaf);
    CHECK(fileExists(dir.join(leaf + std::string(ASSET_META_SUFFIX))));
}

// ---- the golden battery's database-dependent case (docs/09 §5.7) --------------------------------

TEST_CASE("asset_database: a freshly created sidecar matches minimal.meta modulo its GUID (AG6)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(6006);
    db.rescan(dir.utf8(), gen);
    const AssetRecord* const record = db.findByPath("a.png");
    REQUIRE(record != nullptr);

    const auto created = scene_golden::readBytes(dir.join("a.png.meta"));
    REQUIRE(created.ok);
    const auto fixture = scene_golden::readBytes(AERO_ASSET_FIXTURES_DIR "/minimal.meta");
    REQUIRE(fixture.ok);

    // Replace the created file's 32-hex GUID span with the fixture's, then compare byte for byte.
    std::string patched = created.text;
    const std::string createdGuidText = formatGuid(record->guid);
    const std::size_t guidPos = patched.find(createdGuidText);
    REQUIRE(guidPos != std::string::npos);
    patched.replace(guidPos, createdGuidText.size(), "a3f1c07e5b8d42198e6f0c3d7a2b4b92");

    INFO(scene_golden::describeMismatch(fixture.text, patched));
    CHECK(patched == fixture.text);
}

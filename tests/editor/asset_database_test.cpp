// tests/editor/asset_database_test.cpp -- task 3.1.1: AssetDatabase's scan over a real filesystem
// tree; task 3.1.2 extends it with the import cache, alias dedup and dependency tracking (AD32-AD62).
// A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
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
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using engine::ContentHash;
using engine::formatGuid;
using engine::Guid;
using engine::GuidGenerator;
using engine::editor::ASSET_META_SUFFIX;
using engine::editor::AssetCacheEntry;
using engine::editor::AssetCacheParseResult;
using engine::editor::AssetDatabase;
using engine::editor::AssetMetaState;
using engine::editor::AssetRecord;
using engine::editor::AssetScanReport;
using engine::editor::CacheLoadOutcome;
using engine::editor::fileExists;
using engine::editor::ImportChange;
using engine::editor::MAX_HASH_BYTES_PER_SCAN;
using engine::editor::parseAssetCache;
using engine::editor::ScanStatus;
using engine::editor::writeAssetCacheText;
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

// A path built from UTF-8 BYTES, never from a narrow std::string: std::filesystem::path's
// narrow-char constructor assumes the ACTIVE CODE PAGE on Windows, not UTF-8, so an emoji leaf
// (U+1F680 -- four UTF-8 bytes, a UTF-16 surrogate pair) is written under a MANGLED name there while
// resolving fine on macOS/Linux, where narrow paths already are UTF-8. This is `pathFromUtf8`
// (project_files.cpp:29) and `TempDir::write` (project_files_test.cpp:69) in the test tier -- the
// product code was always correct; only these helpers were not. Windows CI caught it on AD30, which
// is the ONLY case here whose leaf leaves ASCII (2.2.4's own F18 case uses U+00DF, a BMP character
// that survives a round trip through many code pages, so it never discriminated this).
[[nodiscard]] std::filesystem::path pathOf(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

void writeFile(std::string_view absolutePath, std::string_view bytes) {
    std::ofstream out(pathOf(absolutePath), std::ios::binary | std::ios::trunc);
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
    const std::filesystem::path path = pathOf(absolutePath);
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
    const AssetScanReport report = db.rescan("", "", gen);
    CHECK(report.status == ScanStatus::Missing);
    CHECK(db.size() == 0);
    CHECK(report.created == 0);
    CHECK(report.repaired == 0);
}

TEST_CASE("asset_database: a configured but absent root is Missing (AD2, E2)") {
    const TempDir dir;
    AssetDatabase db;
    GuidGenerator gen(2);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.join("does-not-exist"), gen);
    CHECK(report.status == ScanStatus::Missing);
    CHECK(db.size() == 0);
    CHECK_FALSE(std::filesystem::exists(dir.join("does-not-exist")));  // nothing created on disk
}

TEST_CASE("asset_database: a root that is a FILE is NotADirectory (AD3, E3)") {
    const TempDir dir;
    writeFile(dir.join("not-a-dir"), "x");
    AssetDatabase db;
    GuidGenerator gen(3);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.join("not-a-dir"), gen);
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
    const AssetScanReport report = db.rescan(dir.utf8(), target, gen);

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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
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
    db.rescan(dir.utf8(), dir.utf8(), gen);
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
    const AssetScanReport first = db.rescan(dir.utf8(), dir.utf8(), gen);
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
    std::filesystem::last_write_time(pathOf(dir.join("a.png.meta")), backdated, aBackdateEc);
    std::filesystem::last_write_time(pathOf(dir.join("b.png.meta")), backdated, bBackdateEc);
    REQUIRE_FALSE(aBackdateEc);
    REQUIRE_FALSE(bBackdateEc);

    const Stat aBefore = statOf(dir.join("a.png.meta"));
    const Stat bBefore = statOf(dir.join("b.png.meta"));
    REQUIRE(aBefore.exists);
    REQUIRE(bBefore.exists);
    CHECK(aBefore.mtime == backdated);  // the back-date actually landed -- not a vacuous proof
    CHECK(bBefore.mtime == backdated);

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);

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
    db.rescan(dir.utf8(), dir.utf8(), gen);
    const std::optional<Guid> firstGuid = db.guidForPath("a.png");
    REQUIRE(firstGuid.has_value());
    db.rescan(dir.utf8(), dir.utf8(), gen);
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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);

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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);

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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);

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
    dbA.rescan(dirA.utf8(), dirA.utf8(), genA);

    const TempDir dirB;
    writeFile(dirB.join("a.png"), "a");
    writeFile(dirB.join("z.png"), "z");
    writeFile(dirB.join("a.png.meta"), body);
    writeFile(dirB.join("z.png.meta"), body);
    AssetDatabase dbB;
    GuidGenerator genB(202);  // a DIFFERENT seed
    dbB.rescan(dirB.utf8(), dirB.utf8(), genB);

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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(report.truncated);
}

TEST_CASE("asset_database: depth beyond MAX_TREE_DEPTH sets depthLimited (AD24, E23, seed S20)") {
    const TempDir dir;
    std::filesystem::path deep = pathOf(dir.utf8());
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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
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
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);

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
    db.rescan(dir.utf8(), dir.utf8(), gen);

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
    db.rescan(dir.utf8(), dir.utf8(), gen);

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
    db.rescan(dirA.utf8(), dirA.utf8(), gen);
    REQUIRE(db.findByPath("a.png") != nullptr);
    const Guid oldGuid = *db.guidForPath("a.png");

    db.rescan(dirB.utf8(), dirB.utf8(), gen);
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
    std::filesystem::copy_file(pathOf(dir.join("x.png.meta")), pathOf(dir.join("x.png.META")), ec);
    if (ec) {
        // A case-INSENSITIVE filesystem collapsed the second write onto the first -- skip cleanly,
        // never a failure (plan §S3, step 3): there is only ever one file here on such a filesystem.
        MESSAGE("case-insensitive filesystem: x.png.META collided with x.png.meta, nothing to test here");
        return;
    }

    AssetDatabase db;
    GuidGenerator gen(29);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    const AssetRecord* const record = db.findByPath("x.png");
    REQUIRE(record != nullptr);
    CHECK(record->state == AssetMetaState::Ok);  // "x.png.META" < "x.png.meta" (byte order): it wins
    CHECK(report.orphanTotal == 1);
    REQUIRE(report.orphans.size() == 1);
    CHECK(report.orphans[0] == "x.png.meta");
}

TEST_CASE(
    "asset_database: a Created write never lands on an orphan's path, case-insensitively (AD31, "
    "code-review finding 2)") {
    // The exact scenario from the finding: a case-only rename (wood.png -> Wood.png) leaves the old
    // sidecar behind as a real, valid, ORPHANED .meta -- unconsumed because pairing is EXACT bytes
    // (AC-19), so "wood.png.meta" never claims "Wood.png". Unlike AD29's x.meta/x.META, "Wood.png"
    // and "wood.png.meta" are NOT case-variants of the same full name, so both coexist as distinct
    // directory entries on ANY filesystem -- this case needs no case-insensitive-volume skip and must
    // run everywhere (the finding's own requirement).
    const TempDir dir;
    writeFile(dir.join("Wood.png"), "w");
    const std::string orphanBody = "{\n  \"version\": 1,\n  \"guid\": \"a3f1c07e5b8d42198e6f0c3d7a2b4b92\"\n}\n";
    writeFile(dir.join("wood.png.meta"), orphanBody);

    AssetDatabase db;
    GuidGenerator gen(31);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);

    // The orphan is still reported, and its bytes are UNCHANGED -- the write that would have
    // collided with it (case-insensitively) on disk never happened.
    CHECK(report.orphanTotal == 1);
    REQUIRE(report.orphans.size() == 1);
    CHECK(report.orphans[0] == "wood.png.meta");
    const auto onDisk = scene_golden::readBytes(dir.join("wood.png.meta"));
    REQUIRE(onDisk.ok);
    CHECK(onDisk.text == orphanBody);

    // The conflict is reported in its OWN category, not folded into invalidPaths.
    CHECK(report.writeConflictTotal == 1);
    REQUIRE(report.writeConflicts.size() == 1);
    CHECK(report.writeConflicts[0].find("Wood.png") != std::string::npos);
    CHECK(report.invalidPaths.empty());

    // "Wood.png" has no identity this session -- D7's own posture for an invalid sidecar (never Ok,
    // never Created, no guid) -- and is counted among `invalid`, not `created`.
    CHECK(report.created == 0);
    CHECK(report.invalid == 1);
    const AssetRecord* const record = db.findByPath("Wood.png");
    REQUIRE(record != nullptr);
    CHECK(record->state == AssetMetaState::Invalid);
    CHECK(record->guid == Guid{});
    CHECK(db.findByGuid(record->guid) == nullptr);  // nil guid -> nullptr regardless (INV-A7)
    // NOTE: no separate "Wood.png.meta was never written" check beyond the byte-identity assertion
    // above -- on a case-INSENSITIVE filesystem, fileExists(".../Wood.png.meta") would read TRUE
    // regardless (it resolves to the same entry as "wood.png.meta"), so it would prove nothing extra;
    // the unchanged-bytes check already proves no write landed on that path.
}

TEST_CASE("asset_database: a non-ASCII filename round-trips end to end (AD30, E28)") {
    const TempDir dir;
    const std::string leaf = "\xF0\x9F\x9A\x80.png";  // an emoji leaf name
    writeFile(dir.join(leaf), "x");

    AssetDatabase db;
    GuidGenerator gen(30);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(report.created == 1);
    const AssetRecord* const record = db.findByPath(leaf);
    REQUIRE(record != nullptr);
    CHECK(record->relativePath == leaf);
    CHECK(fileExists(dir.join(leaf + std::string(ASSET_META_SUFFIX))));
}

// ---- task 3.1.2: the import cache, alias dedup and dependency tracking (AD32-AD62) --------------
// Every case below passes the SAME string as both the project root and the assets root unless the
// case is specifically about the two roots diverging (AD54, AD60, AD61) -- the "." layout (docs/09
// §4.4), matching AD61's own explicit coverage of it.

TEST_CASE("asset_database: first scan of three loose files creates the cache under Library/ (AD32, E3, AC-37)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("b.png"), "b");
    writeFile(dir.join("c.png"), "c");

    AssetDatabase db;
    GuidGenerator gen(32);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);

    CHECK(report.status == ScanStatus::Ok);
    CHECK(report.newAssets == 3);
    CHECK(report.hashed == 3);
    CHECK(report.cacheWritten);
    CHECK(db.cacheSize() == 3);
    CHECK(fileExists(dir.join("Library/asset-cache.json")));
    CHECK(fileExists(dir.join("Library/.gitignore")));
}

TEST_CASE("asset_database: a second rescan reads ZERO asset bytes (AD33, AC-28, seed S13)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("b.png"), "b");
    writeFile(dir.join("c.png"), "c");
    AssetDatabase db;
    GuidGenerator gen(33);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(second.hashed == 0);
    CHECK(second.hashedBytes == 0);
    CHECK(second.fastPathHits == 3);
}

TEST_CASE(
    "asset_database: a second rescan writes ZERO bytes anywhere, assets AND Library (AD34, AC-29, INV-C5, seed "
    "S17)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("b.png"), "b");
    AssetDatabase db;
    GuidGenerator gen(34);
    const AssetScanReport first = db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(first.cacheWritten);

    // Back-date every artifact from the first scan by an hour BEFORE the second scan -- AD8's own
    // rule, extended to the cache index and its .gitignore: mtime is the only discriminator of a real
    // rewrite, and identical bytes would false-pass this comparison (R-C3) if it compared content.
    const auto backdated = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    const std::vector<std::string> artifacts = {dir.join("a.png.meta"), dir.join("b.png.meta"),
                                                dir.join("Library/asset-cache.json"), dir.join("Library/.gitignore")};
    for (const std::string& path : artifacts) {
        std::error_code ec;
        std::filesystem::last_write_time(pathOf(path), backdated, ec);
        REQUIRE_FALSE(ec);
    }

    std::vector<Stat> before;
    for (const std::string& path : artifacts) {
        before.push_back(statOf(path));
        REQUIRE(before.back().exists);
        CHECK(before.back().mtime == backdated);
    }

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK_FALSE(second.cacheWritten);

    for (std::size_t i = 0; i < artifacts.size(); ++i) {
        const Stat after = statOf(artifacts[i]);
        CHECK(after.size == before[i].size);
        CHECK(after.mtime == before[i].mtime);
    }
}

TEST_CASE(
    "asset_database: Library/.gitignore holds the exact text, and a hand-edited one is preserved (AD35, AC-37, "
    "E35, D6)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(35);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    const auto gitignore = scene_golden::readBytes(dir.join("Library/.gitignore"));
    REQUIRE(gitignore.ok);
    CHECK(gitignore.text == std::string(engine::editor::LIBRARY_GITIGNORE_TEXT));

    // Hand-edit it and back-date it, then force the index to actually be rewritten (a real content
    // change) -- the .gitignore itself must stay untouched regardless: D6/E35's "written ONLY when
    // absent" rule, one file over from .meta's.
    writeFile(dir.join("Library/.gitignore"), "# hand-edited\n");
    std::error_code ec;
    const auto backdated = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    std::filesystem::last_write_time(pathOf(dir.join("Library/.gitignore")), backdated, ec);
    REQUIRE_FALSE(ec);
    const Stat before = statOf(dir.join("Library/.gitignore"));
    REQUIRE(before.exists);

    writeFile(dir.join("b.png"), "b");  // a real change -- phase 8 re-enters the write branch
    db.rescan(dir.utf8(), dir.utf8(), gen);

    const Stat after = statOf(dir.join("Library/.gitignore"));
    CHECK(after.size == before.size);
    CHECK(after.mtime == before.mtime);
    const auto handEdited = scene_golden::readBytes(dir.join("Library/.gitignore"));
    REQUIRE(handEdited.ok);
    CHECK(handEdited.text == "# hand-edited\n");
}

TEST_CASE("asset_database: an edited file is SourceChanged, exactly one job (AD36, E5)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("b.png"), "bb");
    AssetDatabase db;
    GuidGenerator gen(36);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    writeFile(dir.join("a.png"), "aaaaaaaaaa");  // a different LENGTH -- never flakes on 1s-granularity (R-C1)
    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);

    CHECK(second.changed == 1);
    CHECK(db.importPlan().jobIndices.size() == 1);
    const AssetRecord* const record = db.findByPath("a.png");
    REQUIRE(record != nullptr);
    CHECK(record->change == ImportChange::SourceChanged);
}

TEST_CASE("asset_database: same size, different mtime forces a re-hash that finds SourceChanged (AD37, seed S14)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "aaa");
    AssetDatabase db;
    GuidGenerator gen(37);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    // Same LENGTH, different bytes, and an mtime bumped forward so the (size, mtime) fast path cannot
    // fire -- the file must actually be re-hashed to notice the content changed.
    writeFile(dir.join("a.png"), "bbb");
    std::error_code ec;
    std::filesystem::last_write_time(pathOf(dir.join("a.png")),
                                     std::filesystem::file_time_type::clock::now() + std::chrono::hours(1), ec);
    REQUIRE_FALSE(ec);

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(second.hashed == 1);
    CHECK(second.fastPathHits == 0);
    const AssetRecord* const record = db.findByPath("a.png");
    REQUIRE(record != nullptr);
    CHECK(record->change == ImportChange::SourceChanged);
}

TEST_CASE("asset_database: touch-but-unchanged is re-hashed once, then the THIRD scan is free (AD38, E6)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "aaa");
    AssetDatabase db;
    GuidGenerator gen(38);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    std::error_code ec;
    std::filesystem::last_write_time(pathOf(dir.join("a.png")),
                                     std::filesystem::file_time_type::clock::now() + std::chrono::hours(1), ec);
    REQUIRE_FALSE(ec);

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(second.hashed == 1);
    CHECK(second.upToDate == 1);  // bytes identical -> UpToDate, not SourceChanged
    const AssetRecord* const afterSecond = db.findByPath("a.png");
    REQUIRE(afterSecond != nullptr);
    CHECK(afterSecond->change == ImportChange::UpToDate);

    const AssetScanReport third = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(third.hashed == 0);
    CHECK(third.fastPathHits == 1);
}

TEST_CASE("asset_database: a moved asset (with its sidecar) keeps its GUID, UpToDate, hashed==0 (AD39, E8, D11)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(39);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    const Guid original = *db.guidForPath("a.png");

    std::error_code ec;
    std::filesystem::create_directories(dir.join("moved"), ec);
    REQUIRE_FALSE(ec);
    std::filesystem::rename(pathOf(dir.join("a.png")), pathOf(dir.join("moved/a.png")), ec);
    REQUIRE_FALSE(ec);
    std::filesystem::rename(pathOf(dir.join("a.png.meta")), pathOf(dir.join("moved/a.png.meta")), ec);
    REQUIRE_FALSE(ec);

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(second.hashed == 0);
    CHECK(second.fastPathHits == 1);
    CHECK(db.findByPath("a.png") == nullptr);
    const AssetRecord* const moved = db.findByPath("moved/a.png");
    REQUIRE(moved != nullptr);
    CHECK(moved->guid == original);
    CHECK(moved->change == ImportChange::UpToDate);
}

TEST_CASE(
    "asset_database: assets/link -> assets/real dedups by canonical path, closing the carried-forward defect "
    "(AD40, AC-31, seed S25, E14, symlink-capable hosts only)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("real"), ec);
    REQUIRE_FALSE(ec);
    writeFile(dir.join("real/a.png"), "a");
    std::filesystem::create_directory_symlink(pathOf(dir.join("real")), pathOf(dir.join("link")), ec);
    if (ec) {
        MESSAGE("skipped: this platform/filesystem refuses create_directory_symlink (Windows needs Developer Mode)");
        return;
    }

    AssetDatabase db;
    GuidGenerator gen(40);
    const AssetScanReport first = db.rescan(dir.utf8(), dir.utf8(), gen);

    CHECK(db.size() == 1);  // ONE record for the physical file, not two
    CHECK(first.repaired == 0);
    CHECK(first.aliasedDirTotal == 1);
    REQUIRE(first.aliasedDirs.size() == 1);
    CHECK(first.aliasedDirs[0].find("link") != std::string::npos);
    CHECK(first.aliasedDirs[0].find("real") != std::string::npos);

    std::error_code backdateEc;
    const auto backdated = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    std::filesystem::last_write_time(pathOf(dir.join("real/a.png.meta")), backdated, backdateEc);
    REQUIRE_FALSE(backdateEc);
    std::filesystem::last_write_time(pathOf(dir.join("Library/asset-cache.json")), backdated, backdateEc);
    REQUIRE_FALSE(backdateEc);
    const Stat metaBackdated = statOf(dir.join("real/a.png.meta"));
    const Stat cacheBackdated = statOf(dir.join("Library/asset-cache.json"));
    REQUIRE(metaBackdated.exists);
    REQUIRE(cacheBackdated.exists);

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(db.size() == 1);
    CHECK(second.repaired == 0);
    CHECK_FALSE(second.cacheWritten);
    const Stat metaAfter = statOf(dir.join("real/a.png.meta"));
    const Stat cacheAfter = statOf(dir.join("Library/asset-cache.json"));
    CHECK(metaAfter.size == metaBackdated.size);
    CHECK(metaAfter.mtime == metaBackdated.mtime);
    CHECK(cacheAfter.size == cacheBackdated.size);
    CHECK(cacheAfter.mtime == cacheBackdated.mtime);
}

TEST_CASE(
    "asset_database: two byte-identical files in different directories keep two identities (AD41, seed S26, E18)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("x"), ec);
    std::filesystem::create_directories(dir.join("y"), ec);
    REQUIRE_FALSE(ec);
    writeFile(dir.join("x/a.png"), "same bytes");
    writeFile(dir.join("y/a.png"), "same bytes");

    AssetDatabase db;
    GuidGenerator gen(41);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);

    CHECK(db.size() == 2);
    const AssetRecord* const x = db.findByPath("x/a.png");
    const AssetRecord* const y = db.findByPath("y/a.png");
    REQUIRE(x != nullptr);
    REQUIRE(y != nullptr);
    CHECK(x->guid != y->guid);
    CHECK(report.repaired == 0);
    CHECK(fileExists(dir.join("x/a.png.meta")));
    CHECK(fileExists(dir.join("y/a.png.meta")));
}

TEST_CASE(
    "asset_database: a FILE symlink to another asset gets its own sidecar and GUID (AD42, E18, D9, symlink-capable "
    "hosts only)") {
    const TempDir dir;
    writeFile(dir.join("real.png"), "r");
    std::error_code ec;
    std::filesystem::create_symlink(pathOf(dir.join("real.png")), pathOf(dir.join("link.png")), ec);
    if (ec) {
        MESSAGE("skipped: this platform/filesystem refuses create_symlink (Windows needs Developer Mode)");
        return;
    }

    AssetDatabase db;
    GuidGenerator gen(42);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);

    CHECK(db.size() == 2);
    const AssetRecord* const real = db.findByPath("real.png");
    const AssetRecord* const link = db.findByPath("link.png");
    REQUIRE(real != nullptr);
    REQUIRE(link != nullptr);
    CHECK(real->guid != link->guid);
    CHECK(report.repaired == 0);
    CHECK(fileExists(dir.join("real.png.meta")));
    CHECK(fileExists(dir.join("link.png.meta")));
}

TEST_CASE(
    "asset_database: a symlinked directory pointing OUTSIDE the project is scanned once, normally (AD43, E15, "
    "symlink-capable hosts only)") {
    const TempDir dir;
    const TempDir outside;
    writeFile(outside.join("ext.png"), "e");
    std::error_code ec;
    std::filesystem::create_directory_symlink(pathOf(outside.utf8()), pathOf(dir.join("external")), ec);
    if (ec) {
        MESSAGE("skipped: this platform/filesystem refuses create_directory_symlink (Windows needs Developer Mode)");
        return;
    }

    AssetDatabase db;
    GuidGenerator gen(43);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);

    CHECK(report.aliasedDirTotal == 0);
    const AssetRecord* const record = db.findByPath("external/ext.png");
    REQUIRE(record != nullptr);
    CHECK(record->state == AssetMetaState::Created);
    CHECK(fileExists(outside.join("ext.png.meta")));
}

TEST_CASE(
    "asset_database: a symlink cycle terminates on the first repeat, no depth limit needed (AD44, E16, "
    "symlink-capable hosts only)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("a"), ec);
    REQUIRE_FALSE(ec);
    writeFile(dir.join("a/leaf.png"), "x");
    std::filesystem::create_directory_symlink(pathOf(dir.join("a")), pathOf(dir.join("a/link")), ec);
    if (ec) {
        MESSAGE("skipped: this platform/filesystem refuses create_directory_symlink (Windows needs Developer Mode)");
        return;
    }

    AssetDatabase db;
    GuidGenerator gen(44);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);

    CHECK_FALSE(report.depthLimited);
    CHECK(report.aliasedDirTotal >= 1);
    const AssetRecord* const record = db.findByPath("a/leaf.png");
    REQUIRE(record != nullptr);
    CHECK(db.size() == 1);
}

TEST_CASE("asset_database: a symlinked directory whose target cannot be resolved is not descended into (AD45, E17)") {
    const TempDir dir;
    std::error_code ec;
    // Measured directly against this build's OWN listDirectory (task 3.1.2's engineering-log entry),
    // not assumed: on this platform (macOS/APFS/libc++) a dangling directory-style symlink is
    // classified isDirectory == false BY THE WALK ITSELF (project_files.cpp's review-gap-1 precedent
    // -- is_directory() fails FIRST for an unresolvable target, so it is listed as a size-unknown
    // FILE, never a directory). That makes the "could not be resolved" WARN inside the D9 alias-dedup
    // branch (isDirectory == true but canonicalDirectory then fails) UNREACHABLE through a dangling
    // symlink, a mutual two-link cycle, or a 60-hop dangling chain alike -- all three were tried and
    // all three agree with canonicalDirectory rather than diverging from it. The branch stays in
    // production as defence in depth for a platform where the two checks CAN disagree (untested here).
    std::filesystem::create_symlink("aero-definitely-no-such-target-3.1.2-scan", pathOf(dir.join("dangling")), ec);
    if (ec) {
        MESSAGE("skipped: this platform/filesystem refuses create_symlink (Windows needs Developer Mode)");
        return;
    }

    AssetDatabase db;
    GuidGenerator gen(45);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);

    if (report.aliasedDirTotal == 0) {
        MESSAGE(
            "skipped: on this platform a dangling symlink is classified isDirectory==false by listDirectory, so "
            "the D9 'could not be resolved' branch is unreachable through any symlink construction -- measured "
            "directly (AD45, a real, documented coverage gap)");
        return;
    }
    REQUIRE(report.aliasedDirs.size() >= 1);
    CHECK(report.aliasedDirs[0].find("could not be resolved") != std::string::npos);
    CHECK(db.size() == 0);
}

TEST_CASE("asset_database: orphan re-attachment end to end (AD46, AC-27, D13, seed S24)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "hello world");
    AssetDatabase db;
    GuidGenerator gen(46);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    const Guid original = *db.guidForPath("a.png");
    const auto originalMeta = scene_golden::readBytes(dir.join("a.png.meta"));
    REQUIRE(originalMeta.ok);

    // Rename the ASSET but leave its sidecar behind (a rename tool that only moved the source file) --
    // "a.png.meta" is now an ORPHAN, and "b.png" is byte-identical to the old "a.png", so it re-attaches.
    std::error_code ec;
    std::filesystem::rename(pathOf(dir.join("a.png")), pathOf(dir.join("b.png")), ec);
    REQUIRE_FALSE(ec);

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);

    CHECK(second.reattachmentTotal == 1);
    REQUIRE(second.reattachments.size() == 1);
    CHECK(second.reattachments[0].find("b.png") != std::string::npos);
    CHECK(second.reattachments[0].find("a.png.meta") != std::string::npos);

    const AssetRecord* const record = db.findByPath("b.png");
    REQUIRE(record != nullptr);
    CHECK(record->guid == original);
    CHECK(record->state == AssetMetaState::Reattached);
    CHECK(fileExists(dir.join("b.png.meta")));

    // The OLD sidecar is untouched, byte for byte -- D8's "never delete an orphan", extended.
    const auto oldMetaAfter = scene_golden::readBytes(dir.join("a.png.meta"));
    REQUIRE(oldMetaAfter.ok);
    CHECK(oldMetaAfter.text == originalMeta.text);
}

TEST_CASE(
    "asset_database: a consumed orphan is reported once, never doubled into orphans (AD47, section 6.8 phase 5)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "content");
    AssetDatabase db;
    GuidGenerator gen(47);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    std::error_code ec;
    std::filesystem::rename(pathOf(dir.join("a.png")), pathOf(dir.join("b.png")), ec);
    REQUIRE_FALSE(ec);

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(second.reattachmentTotal == 1);
    CHECK(second.orphanTotal == 0);
    CHECK(second.orphans.empty());
}

TEST_CASE(
    "asset_database: delete + add a byte-DIFFERENT file -> fresh GUID, orphan, no re-attachment (AD48, "
    "E9-negative)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "original");
    AssetDatabase db;
    GuidGenerator gen(48);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    const Guid original = *db.guidForPath("a.png");

    std::error_code ec;
    std::filesystem::remove(pathOf(dir.join("a.png")), ec);
    REQUIRE_FALSE(ec);
    writeFile(dir.join("b.png"), "totally different bytes");

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(second.reattachmentTotal == 0);
    CHECK(second.orphanTotal == 1);
    const AssetRecord* const record = db.findByPath("b.png");
    REQUIRE(record != nullptr);
    CHECK(record->guid != original);
    CHECK(record->state == AssetMetaState::Created);
}

TEST_CASE(
    "asset_database: a hand-written dependency edge drives a cascade through a real scan (AD49, AC-23 end to "
    "end)") {
    // What this case proves is that a dependency edge READ BACK FROM THE INDEX FILE reaches planImports
    // through a real scan at all -- the plumbing, end to end, on real bytes. It does NOT test
    // TRANSITIVITY: there is exactly ONE hand-written edge here, so the cascade only ever runs one hop
    // and an implementation that stopped at depth 1 would leave this case green. IP14 (A -> B -> C),
    // IP15 (four deep) and IP16 (a diamond) carry transitivity, at the pure tier where a multi-level
    // graph costs a std::vector literal instead of a filesystem.
    const TempDir dir;
    writeFile(dir.join("a.png"), "a-content");
    writeFile(dir.join("b.png"), "b-content");
    AssetDatabase db;
    GuidGenerator gen(49);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    const Guid aGuid = *db.guidForPath("a.png");
    const Guid bGuid = *db.guidForPath("b.png");

    // Hand-edit the index: A now DEPENDS ON B, exactly the shape a future importer (3.2) would record.
    const auto indexText = scene_golden::readBytes(dir.join("Library/asset-cache.json"));
    REQUIRE(indexText.ok);
    AssetCacheParseResult parsed = parseAssetCache(indexText.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    bool foundA = false;
    for (auto& entry : parsed.index.entries) {
        if (entry.guid == aGuid) {
            entry.dependencies = {bGuid};
            foundA = true;
        }
    }
    REQUIRE(foundA);
    writeFile(dir.join("Library/asset-cache.json"), writeAssetCacheText(parsed.index));

    // Edit B -- a real content change, a different LENGTH (R-C1).
    writeFile(dir.join("b.png"), "b-content-edited-with-more-bytes");

    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    const AssetRecord* const aRecord = db.findByPath("a.png");
    const AssetRecord* const bRecord = db.findByPath("b.png");
    REQUIRE(aRecord != nullptr);
    REQUIRE(bRecord != nullptr);
    CHECK(bRecord->change == ImportChange::SourceChanged);
    CHECK(aRecord->change == ImportChange::DependencyChanged);
    CHECK(report.dependencyChanged == 1);
    CHECK(report.changed == 1);
}

TEST_CASE("asset_database: a damaged index (bad version) is discarded whole and rebuilt (AD50, E20, seed S10)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(50);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    std::error_code ec;
    std::filesystem::create_directories(dir.join("Library"), ec);
    writeFile(dir.join("Library/asset-cache.json"),
              "{\n  \"version\": 99,\n  \"hashAlgorithm\": \"murmur3-x64-128\",\n  \"entries\": []\n}\n");

    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(report.status == ScanStatus::Ok);
    CHECK(report.cacheDiscardReason == "unsupported asset cache format version 99 (this build reads version 1)");
    CHECK(db.findByPath("a.png") != nullptr);
    CHECK(db.cacheSize() == 1);  // rebuilt from this scan's own fresh hash
    CHECK(fileExists(dir.join("Library/asset-cache.json")));
}

TEST_CASE("asset_database: an index with ONE malformed entry drops only it (AD51, E21, seed S9)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("b.png"), "b");
    AssetDatabase db;
    GuidGenerator gen(51);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    const Guid bGuid = *db.guidForPath("b.png");

    // Hand-author an index where B's entry is complete and A's is missing "path" -- parseAssetCache
    // drops only the malformed ELEMENT (AC-16), never the whole document.
    std::string text = "{\n  \"version\": 1,\n  \"hashAlgorithm\": \"murmur3-x64-128\",\n  \"entries\": [\n";
    text += R"(    {"guid": ")" + std::string(32, '1') + "\"},\n";
    text += R"(    {"guid": ")" + formatGuid(bGuid) + R"(", "path": "b.png", "size": 1, "mtime": 0, "contentHash": ")" +
            std::string(32, '0') + R"(", "metaHash": ")" + std::string(32, '0') + "\"}\n";
    text += "  ]\n}\n";
    std::error_code ec;
    std::filesystem::create_directories(dir.join("Library"), ec);
    writeFile(dir.join("Library/asset-cache.json"), text);

    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(report.status == ScanStatus::Ok);
    CHECK(report.cacheEntriesLoaded == 1);     // only B survived
    CHECK(report.cacheEntriesDropped == 1);    // A's malformed element
    CHECK(report.cacheDiscardReason.empty());  // NOT a whole-document discard
    const AssetRecord* const aRecord = db.findByPath("a.png");
    const AssetRecord* const bRecord = db.findByPath("b.png");
    REQUIRE(aRecord != nullptr);
    REQUIRE(bRecord != nullptr);
    CHECK(aRecord->change == ImportChange::New);  // no cache entry survived for A's real guid
}

TEST_CASE("asset_database: an absent index is Absent, everything New, no discard reason (AD52, E19)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(52);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(report.cacheEntriesLoaded == 0);
    CHECK(report.cacheDiscardReason.empty());
    CHECK(report.newAssets == 1);
    const AssetRecord* const record = db.findByPath("a.png");
    REQUIRE(record != nullptr);
    CHECK(record->change == ImportChange::New);
}

TEST_CASE("asset_database: a read-only project root leaves cacheWriteError set, nothing removed (AD53, E33, AC-37)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(53);

    std::error_code ec;
    std::filesystem::permissions(dir.utf8(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);

    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);

    std::error_code restoreEc;
    std::filesystem::permissions(dir.utf8(), std::filesystem::perms::owner_all, std::filesystem::perm_options::replace,
                                 restoreEc);

    if (report.writeFailureTotal == 0 && report.cacheWriteError.empty()) {
        MESSAGE("running as a user for whom a read-only directory does not block file creation -- seed did not land");
        return;
    }
    CHECK_FALSE(report.cacheWriteError.empty());
    CHECK(fileExists(dir.join("a.png")));
    CHECK(db.size() == 1);

    // A10's own pinned case: the .meta write for a.png failed too (same read-only root), so phase 8
    // must exclude it from the import inputs entirely -- committing a hash for a file we never
    // confirmed we wrote is exactly the false-UpToDate R-C2 forbids. No entry for it survives.
    if (report.writeFailureTotal > 0) {
        CHECK(db.cacheSize() == 0);
    }
}

TEST_CASE(
    "asset_database: a missing assets root leaves an existing cache index completely untouched (AD54, AC-36, "
    "E2)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("assets"), ec);
    REQUIRE_FALSE(ec);
    writeFile(dir.join("assets/a.png"), "a");

    AssetDatabase db;
    GuidGenerator gen(54);
    const AssetScanReport first = db.rescan(dir.utf8(), dir.join("assets"), gen);
    REQUIRE(first.status == ScanStatus::Ok);
    REQUIRE(fileExists(dir.join("Library/asset-cache.json")));

    const Stat before = statOf(dir.join("Library/asset-cache.json"));
    REQUIRE(before.exists);

    // The assets root vanishes -- the project root (and its Library/) stays completely valid.
    std::filesystem::remove_all(dir.join("assets"), ec);
    REQUIRE_FALSE(ec);

    const AssetScanReport second = db.rescan(dir.utf8(), dir.join("assets"), gen);
    CHECK(second.status == ScanStatus::Missing);

    const Stat after = statOf(dir.join("Library/asset-cache.json"));
    CHECK(after.size == before.size);
    CHECK(after.mtime == before.mtime);
}

TEST_CASE(
    "asset_database: the hash budget leaves later files NotHashed and commits none of them (AD55, AC-30, seed "
    "S15, E29)") {
    const TempDir dir;
    const std::string hundred(100, 'x');
    writeFile(dir.join("a.png"), hundred);
    writeFile(dir.join("b.png"), hundred);
    writeFile(dir.join("c.png"), hundred);
    writeFile(dir.join("d.png"), hundred);

    AssetDatabase db;
    GuidGenerator gen(55);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen, /*hashBudgetBytes=*/64);

    CHECK(report.hashBudgetExhausted);
    CHECK(report.hashed == 1);
    CHECK(report.notHashed == 3);
    CHECK(db.cacheSize() == 1);  // NotHashed assets commit NOTHING (R-C2's forbidden false UpToDate)

    std::size_t notHashedCount = 0;
    for (const std::string_view leaf : {"a.png", "b.png", "c.png", "d.png"}) {
        const AssetRecord* const record = db.findByPath(leaf);
        REQUIRE(record != nullptr);
        if (record->change == ImportChange::NotHashed) {
            ++notHashedCount;
        }
    }
    CHECK(notHashedCount == 3);

    // A second scan with the DEFAULT budget makes progress: the remaining three are hashed and committed.
    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK_FALSE(second.hashBudgetExhausted);
    CHECK(second.hashed == 3);
    CHECK(db.cacheSize() == 4);
}

TEST_CASE("asset_database: a single file bigger than the whole budget is still hashed to completion once (AD56, E30)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), std::string(100, 'y'));
    AssetDatabase db;
    GuidGenerator gen(56);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen, /*hashBudgetBytes=*/8);

    CHECK(report.hashed == 1);
    CHECK(report.hashedBytes == 100);
    CHECK_FALSE(report.hashBudgetExhausted);
    const AssetRecord* const record = db.findByPath("a.png");
    REQUIRE(record != nullptr);
    CHECK(record->contentHash.valid());
    CHECK(db.cacheSize() == 1);
}

TEST_CASE("asset_database: an unreadable asset is Unhashable, previous entry preserved, nothing removed (AD57, E31)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(57);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(db.cacheSize() == 1);
    const Guid guid = *db.guidForPath("a.png");

    std::error_code ec;
    std::filesystem::last_write_time(pathOf(dir.join("a.png")),
                                     std::filesystem::file_time_type::clock::now() + std::chrono::hours(1), ec);
    REQUIRE_FALSE(ec);
    std::filesystem::permissions(pathOf(dir.join("a.png")), std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);

    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);

    std::error_code restoreEc;
    std::filesystem::permissions(pathOf(dir.join("a.png")), std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, restoreEc);

    if (report.hashFailureTotal == 0) {
        MESSAGE("running as a user for whom chmod 000 does not block reads (e.g. root) -- seed did not land");
        return;
    }
    CHECK(report.hashFailureTotal == 1);
    REQUIRE(report.hashFailures.size() == 1);
    CHECK(report.hashFailures[0].find("a.png") != std::string::npos);
    const AssetRecord* const record = db.findByPath("a.png");
    REQUIRE(record != nullptr);
    CHECK(record->change == ImportChange::Unhashable);
    CHECK(record->guid == guid);
    CHECK(db.cacheSize() == 1);  // the PREVIOUS entry preserved verbatim, nothing removed
    CHECK(db.findByGuid(guid) != nullptr);
}

TEST_CASE(
    "asset_database: invalidateCache() makes every asset New next scan and rewrites the index (AD58, AC-35, E40)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(58);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(db.cacheSize() == 1);

    std::error_code ec;
    const auto backdated = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    std::filesystem::last_write_time(pathOf(dir.join("Library/asset-cache.json")), backdated, ec);
    REQUIRE_FALSE(ec);
    const Stat before = statOf(dir.join("Library/asset-cache.json"));
    REQUIRE(before.exists);

    db.invalidateCache();
    CHECK(db.cacheSize() == 0);

    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(report.newAssets == 1);
    CHECK(report.cacheWritten);
    const Stat after = statOf(dir.join("Library/asset-cache.json"));
    CHECK(after.mtime != before.mtime);
}

TEST_CASE("asset_database: rescanning a DIFFERENT project fully replaces the cache too (AD59, E41)") {
    const TempDir dirA;
    writeFile(dirA.join("a.png"), "a");
    const TempDir dirB;
    writeFile(dirB.join("b.png"), "b");

    AssetDatabase db;
    GuidGenerator gen(59);
    db.rescan(dirA.utf8(), dirA.utf8(), gen);
    REQUIRE(db.cacheSize() == 1);
    REQUIRE(db.projectRoot() == dirA.utf8());

    db.rescan(dirB.utf8(), dirB.utf8(), gen);
    CHECK(db.projectRoot() == dirB.utf8());
    CHECK(db.cacheSize() == 1);
    CHECK(db.findByPath("a.png") == nullptr);
    CHECK(db.findByPath("b.png") != nullptr);
    CHECK(fileExists(dirB.join("Library/asset-cache.json")));
}

TEST_CASE(
    "asset_database: the index lands at <projectRoot>/Library/asset-cache.json for all three layouts (AD60, "
    "AC-38, seeds S27/S27b)") {
    // Layout 1: default -- the assets root is a direct subdirectory "assets" of the project root.
    {
        const TempDir dir;
        std::error_code ec;
        std::filesystem::create_directories(dir.join("assets"), ec);
        REQUIRE_FALSE(ec);
        writeFile(dir.join("assets/a.png"), "a");
        AssetDatabase db;
        GuidGenerator gen(601);
        db.rescan(dir.utf8(), dir.join("assets"), gen);
        CHECK(fileExists(dir.join("Library/asset-cache.json")));
    }
    // Layout 2: nested -- "content/assets".
    {
        const TempDir dir;
        std::error_code ec;
        std::filesystem::create_directories(dir.join("content/assets"), ec);
        REQUIRE_FALSE(ec);
        writeFile(dir.join("content/assets/a.png"), "a");
        AssetDatabase db;
        GuidGenerator gen(602);
        db.rescan(dir.utf8(), dir.join("content/assets"), gen);
        CHECK(fileExists(dir.join("Library/asset-cache.json")));
        CHECK_FALSE(fileExists(dir.join("content/Library/asset-cache.json")));
    }
    // Layout 3: "." -- the assets root IS the project root.
    {
        const TempDir dir;
        writeFile(dir.join("a.png"), "a");
        AssetDatabase db;
        GuidGenerator gen(603);
        db.rescan(dir.utf8(), dir.utf8(), gen);
        CHECK(fileExists(dir.join("Library/asset-cache.json")));
    }
}

TEST_CASE("asset_database: with paths.assets == \".\" the walk skips Library/ entirely (AD61, AC-38, seed S27)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(61);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(fileExists(dir.join("Library/asset-cache.json")));

    // A second scan re-walks the SAME tree, which now contains Library/ -- it must still be excluded.
    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(db.findByPath("Library/asset-cache.json") == nullptr);
    CHECK(db.findByPath("Library/.gitignore") == nullptr);
    CHECK_FALSE(fileExists(dir.join("Library/asset-cache.json.meta")));
    CHECK_FALSE(fileExists(dir.join("Library/.gitignore.meta")));
    CHECK(second.aliasedDirTotal == 0);  // A7: our own output is excluded silently, never reported
    CHECK(db.size() == 1);               // only a.png -- Library/'s contents never entered the walk at all
}

TEST_CASE("asset_database: a newly created asset is UpToDate on the VERY NEXT scan (AD62, A10, AC-28/AC-29)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(62);
    const AssetScanReport first = db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(first.created == 1);
    REQUIRE(first.newAssets == 1);

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(second.upToDate == 1);
    CHECK(second.newAssets == 0);
    CHECK(second.changed == 0);
    const AssetRecord* const record = db.findByPath("a.png");
    REQUIRE(record != nullptr);
    CHECK(record->change == ImportChange::UpToDate);
}

TEST_CASE(
    "asset_database: a fast-path collision on a pre-repair duplicate GUID is refused, so the repaired "
    "record's contentHash is its OWN content's hash (AD63, code-review finding 2)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "aaaa");  // 4 bytes
    AssetDatabase db;
    GuidGenerator gen(63);
    const AssetScanReport first = db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(first.created == 1);
    const AssetRecord* const aFirst = db.findByPath("a.png");
    REQUIRE(aFirst != nullptr);
    const ContentHash aHash = aFirst->contentHash;
    const Stat aStat = statOf(dir.join("a.png"));
    REQUIRE(aStat.exists);

    // z.png claims the SAME on-disk GUID as a.png (a genuine duplicate -- exactly what D9's repair pass
    // exists to fix), with the SAME byte length but DIFFERENT bytes, and its mtime forced to match
    // a.png's exactly -- both halves of the (size, mtime) fast path the finding's bug exploits.
    writeFile(dir.join("z.png"), "zzzz");
    const auto aMetaBytes = scene_golden::readBytes(dir.join("a.png.meta"));
    REQUIRE(aMetaBytes.ok);
    writeFile(dir.join("z.png.meta"), aMetaBytes.text);  // literally the same sidecar bytes -- same GUID
    std::error_code ec;
    std::filesystem::last_write_time(pathOf(dir.join("z.png")), aStat.mtime, ec);
    REQUIRE_FALSE(ec);
    const Stat zStat = statOf(dir.join("z.png"));
    REQUIRE(zStat.exists);
    REQUIRE(zStat.size == aStat.size);    // the size half of the fast path
    REQUIRE(zStat.mtime == aStat.mtime);  // the mtime half of the fast path

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(second.repaired == 1);

    const AssetRecord* const aSecond = db.findByPath("a.png");
    const AssetRecord* const zSecond = db.findByPath("z.png");
    REQUIRE(aSecond != nullptr);
    REQUIRE(zSecond != nullptr);
    CHECK(zSecond->state == AssetMetaState::Repaired);
    CHECK(zSecond->guid != aSecond->guid);

    // The bug: z.png's committed contentHash equals a.png's (the cache entry the shared, pre-repair GUID
    // wrongly vouched for). The fix: neither entry may take the fast path while the GUID is still
    // claimed twice, so z.png is genuinely re-hashed and its contentHash is its OWN content's digest.
    const engine::editor::FileHashResult zTrueHash = engine::editor::hashFileContents(dir.join("z.png"));
    REQUIRE(zTrueHash.hash.has_value());
    CHECK(zSecond->contentHash == *zTrueHash.hash);
    CHECK(zSecond->contentHash != aHash);
    CHECK(aSecond->contentHash == aHash);  // a.png's own hash is unaffected either way
}

// ---- task 3.1.3 (D6): records() -- the search index -----------------------------------------------

TEST_CASE("asset_database: records().size() == size() after a real scan (AD64, AC-12)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("b.txt"), "b");
    AssetDatabase db;
    GuidGenerator gen(64);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(db.records().size() == db.size());
    CHECK(db.records().size() == 2);
}

TEST_CASE("asset_database: the span is sorted byte-lexicographically by relativePath (AD65, D5)") {
    const TempDir dir;
    writeFile(dir.join("Z.png"), "z");
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(65);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    const std::span<const AssetRecord> records = db.records();
    REQUIRE(records.size() == 2);
    // Byte order, never case-folded: 'Z' (0x5A) sorts before 'a' (0x61).
    CHECK(records[0].relativePath == "Z.png");
    CHECK(records[1].relativePath == "a.png");
}

TEST_CASE("asset_database: every findByPath(r.relativePath) points INTO the span (AD66)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("b.png"), "b");
    writeFile(dir.join("c.png"), "c");
    AssetDatabase db;
    GuidGenerator gen(66);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    const std::span<const AssetRecord> records = db.records();
    for (const AssetRecord& record : records) {
        const AssetRecord* const found = db.findByPath(record.relativePath);
        REQUIRE(found != nullptr);
        CHECK(found >= records.data());
        CHECK(found < records.data() + records.size());
    }
}

TEST_CASE("asset_database: the span is empty for a Missing root (AD67, E16)") {
    AssetDatabase db;
    GuidGenerator gen(67);
    const AssetScanReport report = db.rescan("", "", gen);
    CHECK(report.status == ScanStatus::Missing);
    CHECK(db.records().empty());
}

TEST_CASE("asset_database: the span survives a second rescan and reflects an added file (AD68, AC-12)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(68);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(db.records().size() == 1);
    writeFile(dir.join("b.png"), "b");
    db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(db.records().size() == 2);
}

TEST_CASE("asset_database: an Invalid record IS in the span (AD69, §D-6)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    writeFile(dir.join("a.png.meta"), "not json");  // unparseable -- Invalid, nil guid
    AssetDatabase db;
    GuidGenerator gen(69);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(report.invalid == 1);
    bool found = false;
    for (const AssetRecord& record : db.records()) {
        if (record.relativePath == "a.png") {
            found = true;
            CHECK(record.state == AssetMetaState::Invalid);
            CHECK_FALSE(record.guid.valid());
        }
    }
    CHECK(found);
    // still findable by path, per §D-6's own comment
    CHECK(db.findByPath("a.png") != nullptr);
}

// ---- the golden battery's database-dependent case (docs/09 §5.7) --------------------------------

TEST_CASE("asset_database: a freshly created sidecar matches minimal.meta modulo its GUID (AG6)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(6006);
    db.rescan(dir.utf8(), dir.utf8(), gen);
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

// ---- generation() (task 3.1.4, D8) ----------------------------------------------------------------

TEST_CASE("asset_database: a fresh database's generation is 0 (AD-g1)") {
    const AssetDatabase db;
    CHECK(db.generation() == 0);
}

TEST_CASE("asset_database: one rescan of a populated tree bumps generation to 1 (AD-g2)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(1);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(db.generation() == 1);
}

TEST_CASE("asset_database: a second rescan that changes nothing STILL bumps generation (AD-g3, D8)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(3);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(db.generation() == 1);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(db.generation() == 2);  // the no-op bump -- D8's "no exceptions to remember"
}

TEST_CASE("asset_database: an empty-roots rescan still bumps generation (AD-g4, the phase-1 guard)") {
    AssetDatabase db;
    GuidGenerator gen(4);
    db.rescan("", "", gen);
    CHECK(db.generation() == 1);
}

TEST_CASE("asset_database: a rescan on a Missing assets root still bumps generation (AD-g5)") {
    const TempDir dir;
    AssetDatabase db;
    GuidGenerator gen(5);
    db.rescan(dir.utf8(), dir.join("does-not-exist"), gen);
    CHECK(db.generation() == 1);
}

TEST_CASE(
    "asset_database: five rescans in a row leave generation strictly increasing, never resetting "
    "(AD-g6)") {
    const TempDir dir;
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(6);
    std::uint64_t previous = db.generation();
    for (int i = 0; i < 5; ++i) {
        db.rescan(dir.utf8(), dir.utf8(), gen);
        CHECK(db.generation() > previous);
        previous = db.generation();
    }
    CHECK(db.generation() == 5);
}

// =====================================================================================================
// AD-i -- phase 7.5, the model probe (task 3.2.1). Real bytes, a scratch TempDir, the same idiom as
// AD32-AD62 above -- the plumbing, end to end, on real files.
// =====================================================================================================

TEST_CASE("asset_database: a scanned model records \"gltf\"/1 as its importer (AD-i1, AC-1)") {
    const TempDir dir;
    writeFile(dir.join("chair.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(1001);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(report.status == ScanStatus::Ok);
    CHECK(report.modelsProbed == 1);
    const AssetRecord* const record = db.findByPath("chair.gltf");
    REQUIRE(record != nullptr);

    const auto indexText = scene_golden::readBytes(dir.join("Library/asset-cache.json"));
    REQUIRE(indexText.ok);
    const AssetCacheParseResult parsed = parseAssetCache(indexText.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    const AssetCacheEntry* const entry = parsed.index.find(record->guid);
    REQUIRE(entry != nullptr);
    CHECK(entry->importer == "gltf");
    CHECK(entry->importerVersion == 1);
}

TEST_CASE("asset_database: a non-model asset's importer stays empty and version stays 0 (AD-i2, AC-2)") {
    const TempDir dir;
    writeFile(dir.join("chair.gltf"), R"({"asset":{"version":"2.0"}})");
    writeFile(dir.join("notes.txt"), "hello");
    AssetDatabase db;
    GuidGenerator gen(1002);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(report.modelsProbed == 1);  // only the model -- the .txt is never probed
    const AssetRecord* const record = db.findByPath("notes.txt");
    REQUIRE(record != nullptr);

    const auto indexText = scene_golden::readBytes(dir.join("Library/asset-cache.json"));
    REQUIRE(indexText.ok);
    const AssetCacheParseResult parsed = parseAssetCache(indexText.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    const AssetCacheEntry* const entry = parsed.index.find(record->guid);
    REQUIRE(entry != nullptr);
    CHECK(entry->importer.empty());
    CHECK(entry->importerVersion == 0);
}

TEST_CASE("asset_database: a model's external texture URI becomes a dependency GUID (AD-i3, AC-3)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("models"), ec);
    REQUIRE_FALSE(ec);
    std::filesystem::create_directories(dir.join("textures"), ec);
    REQUIRE_FALSE(ec);
    writeFile(dir.join("models/chair.gltf"),
              R"({"asset":{"version":"2.0"},"images":[{"uri":"../textures/wood.png"}]})");
    writeFile(dir.join("textures/wood.png"), "pixels");

    AssetDatabase db;
    GuidGenerator gen(1003);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(report.modelsProbed == 1);
    CHECK(report.dependenciesRecorded == 1);
    const Guid woodGuid = *db.guidForPath("textures/wood.png");
    const AssetRecord* const chairRecord = db.findByPath("models/chair.gltf");
    REQUIRE(chairRecord != nullptr);

    const auto indexText = scene_golden::readBytes(dir.join("Library/asset-cache.json"));
    REQUIRE(indexText.ok);
    const AssetCacheParseResult parsed = parseAssetCache(indexText.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    const AssetCacheEntry* const entry = parsed.index.find(chairRecord->guid);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->dependencies.size() == 1);
    CHECK(entry->dependencies[0] == woodGuid);
}

TEST_CASE(
    "asset_database: duplicate-normalising and self-referencing URIs collapse to one sorted, "
    "deduplicated, self-edge-free dependency (AD-i4, INV-M8, E8, E9)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("models"), ec);
    REQUIRE_FALSE(ec);
    std::filesystem::create_directories(dir.join("textures"), ec);
    REQUIRE_FALSE(ec);
    writeFile(dir.join("models/chair.gltf"), R"({"asset":{"version":"2.0"},"images":[)"
                                             R"({"uri":"../textures/wood.png"},)"
                                             R"({"uri":"sub/../../textures/wood.png"},)"
                                             R"({"uri":"chair.gltf"}]})");
    writeFile(dir.join("textures/wood.png"), "pixels");

    AssetDatabase db;
    GuidGenerator gen(1004);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(report.modelsProbed == 1);
    const Guid woodGuid = *db.guidForPath("textures/wood.png");
    const AssetRecord* const chairRecord = db.findByPath("models/chair.gltf");
    REQUIRE(chairRecord != nullptr);

    const auto indexText = scene_golden::readBytes(dir.join("Library/asset-cache.json"));
    REQUIRE(indexText.ok);
    const AssetCacheParseResult parsed = parseAssetCache(indexText.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    const AssetCacheEntry* const entry = parsed.index.find(chairRecord->guid);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->dependencies.size() == 1);  // ONE edge -- never a self-edge, never a duplicate
    CHECK(entry->dependencies[0] == woodGuid);
}

TEST_CASE(
    "asset_database: a refused URI and a URI to an unrecognised file both contribute no dependency "
    "(AD-i5, AC-5, E7)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("models"), ec);
    REQUIRE_FALSE(ec);
    writeFile(dir.join("models/chair.gltf"), R"({"asset":{"version":"2.0"},"images":[)"
                                             R"({"uri":"http://evil.example/x.png"},)"
                                             R"({"uri":"../textures/missing.png"}]})");
    // Deliberately no textures/ directory at all -- the second URI names a file this scan never sees,
    // so it never earns a .meta or a GUID.

    AssetDatabase db;
    GuidGenerator gen(1005);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    // A refused or unresolvable URI is a WARNING inside the ImportResult (imported.warnings), never a
    // whole-import FAILURE -- so the import itself still succeeds and nothing lands in
    // report.importFailures for either URI here.
    CHECK(report.modelsProbed == 1);
    CHECK(report.dependenciesRecorded == 0);
    const AssetRecord* const chairRecord = db.findByPath("models/chair.gltf");
    REQUIRE(chairRecord != nullptr);

    const auto indexText = scene_golden::readBytes(dir.join("Library/asset-cache.json"));
    REQUIRE(indexText.ok);
    const AssetCacheParseResult parsed = parseAssetCache(indexText.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    const AssetCacheEntry* const entry = parsed.index.find(chairRecord->guid);
    REQUIRE(entry != nullptr);
    CHECK(entry->dependencies.empty());  // NEVER a nil GUID -- both URIs contribute nothing
}

TEST_CASE("asset_database: editing a referenced texture marks the model DependencyChanged (AD-i6, AC-4, E11)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("models"), ec);
    REQUIRE_FALSE(ec);
    std::filesystem::create_directories(dir.join("textures"), ec);
    REQUIRE_FALSE(ec);
    writeFile(dir.join("models/chair.gltf"),
              R"({"asset":{"version":"2.0"},"images":[{"uri":"../textures/wood.png"}]})");
    writeFile(dir.join("textures/wood.png"), "original-pixels");

    AssetDatabase db;
    GuidGenerator gen(1006);
    // scan 1: both New. The edge IS recorded this same scan (phase 6 already assigned wood.png its
    // GUID before phase 7.5 runs -- confirmed directly: the committed cache already holds
    // chair -> wood after this one scan), but a New record's `change` reports New, not
    // DependencyChanged -- there is no "cascade" to observe yet (E11).
    const AssetScanReport first = db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(first.status == ScanStatus::Ok);
    CHECK(first.modelsProbed == 1);
    CHECK(first.dependenciesRecorded == 1);
    const AssetRecord* const chairAfterFirst = db.findByPath("models/chair.gltf");
    REQUIRE(chairAfterFirst != nullptr);
    CHECK(chairAfterFirst->change == ImportChange::New);

    // Edit the texture -- a different LENGTH (R-C1, never flakes on 1s-granularity mtime).
    writeFile(dir.join("textures/wood.png"), "edited-pixels-longer");

    // scan 2 -- the FIRST tick after the edit: planImports reads scan 1's cache, which already has the
    // edge, so wood.png's SourceChanged cascades to chair.gltf's DependencyChanged in this SAME scan.
    // Measured directly (not assumed): this is where it actually happens, one tick sooner than E11's
    // own "at the latest" hedge requires.
    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    const AssetRecord* const woodAfterSecond = db.findByPath("textures/wood.png");
    REQUIRE(woodAfterSecond != nullptr);
    CHECK(woodAfterSecond->change == ImportChange::SourceChanged);
    const AssetRecord* const chairAfterSecond = db.findByPath("models/chair.gltf");
    REQUIRE(chairAfterSecond != nullptr);
    CHECK(chairAfterSecond->change == ImportChange::DependencyChanged);
    CHECK(second.dependencyChanged == 1);

    // scan 3 -- the SECOND tick after the edit: nothing further changed, so the cascade has already
    // settled and chair.gltf reports UpToDate again. This is what "ticks twice" buys in this
    // construction: the SECOND tick confirms the DependencyChanged classification is not sticky --
    // and phase 7.5 DID re-probe chair.gltf on scan 2 (it was in that scan's jobIndices), re-recording
    // the identical edge, which is why scan 3 has nothing left to report.
    const AssetScanReport third = db.rescan(dir.utf8(), dir.utf8(), gen);
    const AssetRecord* const chairAfterThird = db.findByPath("models/chair.gltf");
    REQUIRE(chairAfterThird != nullptr);
    CHECK(chairAfterThird->change == ImportChange::UpToDate);
    CHECK(third.dependencyChanged == 0);
}

TEST_CASE(
    "asset_database: an exhausted probe budget disengages the probe and carries the previous record "
    "forward (AD-i7, AC-6)") {
    const TempDir dir;
    writeFile(dir.join("chair.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(1007);
    const AssetScanReport first = db.rescan(dir.utf8(), dir.utf8(), gen);  // generous default budgets
    REQUIRE(first.status == ScanStatus::Ok);
    CHECK(first.modelsProbed == 1);

    // A real content change -- a different LENGTH -- so the SECOND scan has something to re-probe.
    writeFile(dir.join("chair.gltf"), R"({"asset":{"version":"2.0"},"extensionsUsed":["X"]})");
    const AssetScanReport second =
        db.rescan(dir.utf8(), dir.utf8(), gen, MAX_HASH_BYTES_PER_SCAN, /*probeBudgetBytes=*/1);
    CHECK(second.probeBudgetExhausted);
    CHECK(second.modelsProbed == 0);

    const AssetRecord* const record = db.findByPath("chair.gltf");
    REQUIRE(record != nullptr);
    const auto indexText = scene_golden::readBytes(dir.join("Library/asset-cache.json"));
    REQUIRE(indexText.ok);
    const AssetCacheParseResult parsed = parseAssetCache(indexText.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    const AssetCacheEntry* const entry = parsed.index.find(record->guid);
    REQUIRE(entry != nullptr);
    CHECK(entry->importer == "gltf");  // carried forward from the FIRST scan's probe, never reset
    CHECK(entry->importerVersion == 1);
}

TEST_CASE(
    "asset_database: an unreadable model is skipped by the probe and reported via the HASH failure, "
    "never a redundant import failure (AD-i8, AC-6, code review SHOULD-FIX 9, corrected)") {
    const TempDir dir;
    writeFile(dir.join("chair.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(1008);
    const AssetScanReport first = db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(first.modelsProbed == 1);
    const Guid guid = *db.guidForPath("chair.gltf");

    // AD57's exact shape: bump the mtime forward FIRST so the (size, mtime) fast path cannot vouch for
    // the file unread, THEN chmod it -- otherwise phase 4 (and phase 7.5) would never even try to open
    // it, since nothing on disk looks different.
    std::error_code ec;
    std::filesystem::last_write_time(pathOf(dir.join("chair.gltf")),
                                     std::filesystem::file_time_type::clock::now() + std::chrono::hours(1), ec);
    REQUIRE_FALSE(ec);
    std::filesystem::permissions(pathOf(dir.join("chair.gltf")), std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);

    // Restore BEFORE any assertion can fail loudly and BEFORE ~TempDir runs (project_test.cpp's rule).
    std::error_code restoreEc;
    std::filesystem::permissions(pathOf(dir.join("chair.gltf")), std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, restoreEc);

    if (second.hashFailureTotal == 0) {
        MESSAGE("running as a user for whom chmod 000 does not block reads (e.g. root) -- seed did not land");
        return;
    }
    CHECK(second.modelsProbed == 0);
    // CORRECTED (code review SHOULD-FIX 9): this used to assert the OPPOSITE -- that an unreadable
    // model ALSO landed in report.importFailures. That was phase 7.5 redundantly attempting (and
    // redundantly failing) to open a file phase 4's OWN hash attempt had already failed to read for the
    // identical reason (captured above by requiring second.hashFailureTotal > 0): an Unhashable entry's
    // probe is thrown away wholesale by commitImports' un-hashed arm (asset_cache.cpp), so phase 7.5 now
    // skips it before ever touching the file. The failure is reported exactly ONCE, through
    // report.hashFailures -- never duplicated into report.importFailures too.
    CHECK(second.importFailures.empty());

    const auto indexText = scene_golden::readBytes(dir.join("Library/asset-cache.json"));
    REQUIRE(indexText.ok);
    const AssetCacheParseResult parsed = parseAssetCache(indexText.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    const AssetCacheEntry* const entry = parsed.index.find(guid);
    REQUIRE(entry != nullptr);
    CHECK(entry->importer == "gltf");  // the disengaged probe carries the FIRST scan's record forward
}

TEST_CASE(
    "asset_database: a scan of an unchanged project with models writes ZERO bytes and probes ZERO "
    "models (AD-i9, AC-8, seed S7)") {
    const TempDir dir;
    writeFile(dir.join("chair.gltf"), R"({"asset":{"version":"2.0"}})");
    writeFile(dir.join("a.png"), "a");
    AssetDatabase db;
    GuidGenerator gen(1009);
    const AssetScanReport first = db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(first.status == ScanStatus::Ok);
    REQUIRE(first.cacheWritten);
    CHECK(first.modelsProbed == 1);

    // Back-date every artifact from the first scan -- AD34's own rule: mtime is the only discriminator
    // of a real rewrite; comparing content alone would false-pass (R-C3).
    const auto backdated = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    const std::vector<std::string> artifacts = {dir.join("chair.gltf.meta"), dir.join("a.png.meta"),
                                                dir.join("Library/asset-cache.json"), dir.join("Library/.gitignore")};
    for (const std::string& path : artifacts) {
        std::error_code ec;
        std::filesystem::last_write_time(pathOf(path), backdated, ec);
        REQUIRE_FALSE(ec);
    }
    std::vector<Stat> before;
    for (const std::string& path : artifacts) {
        before.push_back(statOf(path));
        REQUIRE(before.back().exists);
    }

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK_FALSE(second.cacheWritten);
    CHECK(second.modelsProbed == 0);

    for (std::size_t i = 0; i < artifacts.size(); ++i) {
        const Stat after = statOf(artifacts[i]);
        CHECK(after.size == before[i].size);
        CHECK(after.mtime == before[i].mtime);
    }
}

TEST_CASE(
    "asset_database: two consecutive scans of a model both report UpToDate, importer never flips "
    "(AD-i10, AC-6)") {
    const TempDir dir;
    writeFile(dir.join("chair.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(1010);
    const AssetScanReport first = db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(first.status == ScanStatus::Ok);
    REQUIRE(first.newAssets == 1);  // the FIRST scan: New, not UpToDate yet

    const AssetScanReport second = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(second.upToDate == 1);
    const AssetRecord* const record = db.findByPath("chair.gltf");
    REQUIRE(record != nullptr);
    CHECK(record->change == ImportChange::UpToDate);

    // `record` is a POINTER INTO recordList and is invalidated by the NEXT rescan() (AssetDatabase's own
    // documented contract: "the span is valid until the next rescan()"); its GUID is copied into a
    // plain VALUE first, before the third scan, rather than dereferenced after it (a real
    // heap-use-after-free ASan caught in an earlier version of this exact case).
    const Guid guid = record->guid;

    const AssetScanReport third = db.rescan(dir.utf8(), dir.utf8(), gen);
    CHECK(third.upToDate == 1);  // TWO CONSECUTIVE UpToDate scans (AC-6's own proof shape)
    const AssetRecord* const recordAgain = db.findByPath("chair.gltf");
    REQUIRE(recordAgain != nullptr);
    CHECK(recordAgain->change == ImportChange::UpToDate);
    CHECK(recordAgain->guid == guid);

    const auto indexText = scene_golden::readBytes(dir.join("Library/asset-cache.json"));
    REQUIRE(indexText.ok);
    const AssetCacheParseResult parsed = parseAssetCache(indexText.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    const AssetCacheEntry* const entry = parsed.index.find(guid);
    REQUIRE(entry != nullptr);
    CHECK(entry->importer == "gltf");
    CHECK(entry->importerVersion == 1);
}

TEST_CASE(
    "asset_database: a probe budget smaller than the SECOND model's size leaves it unprobed -- the "
    "FIRST is still probed normally (AD-i11, code review SHOULD-FIX 8)") {
    // What this proves, and what it structurally cannot: readFileBytes' cap decides whether the SECOND
    // model's bytes are ever OPENED at all (SHOULD-FIX 8's actual fix, in asset_database.cpp's phase
    // 7.5), but that decision has NO independently observable effect on AssetScanReport in ANY
    // construction reachable from this test tier -- file_size() (which decides exhaustion, below) is
    // measured before EITHER cap chooses whether to open the file, whether it refused before opening or
    // read to completion and was then discarded. This case proves the OUTCOME the fix must still get
    // right (the second model stays genuinely unprobed at a tight budget, the first is unaffected) --
    // the I/O saved is a resource-usage property no automated tier in this tree can independently
    // measure; manual validation row 9 is this rule's only behavioural cover, exactly as it is for
    // 3.1.3's BLOCKING-1 and 3.1.4's D9.
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");  // 28 bytes -- fits a tight budget
    writeFile(dir.join("b.gltf"), std::string(200, ' ') + R"({"asset":{"version":"2.0"}})");  // > budget
    AssetDatabase db;
    GuidGenerator gen(1011);
    // "a.gltf" < "b.gltf" byte-lexicographically, so it is probed FIRST (plan.jobIndices' own order) --
    // the budget is large enough for it alone, too small once "b.gltf" is reached.
    const AssetScanReport report =
        db.rescan(dir.utf8(), dir.utf8(), gen, MAX_HASH_BYTES_PER_SCAN, /*probeBudgetBytes=*/100);
    CHECK(report.modelsProbed == 1);
    CHECK(report.probeBudgetExhausted);
    CHECK(report.importFailureTotal == 0);  // exhaustion is NOT a failure -- carried forward silently

    const auto indexText = scene_golden::readBytes(dir.join("Library/asset-cache.json"));
    REQUIRE(indexText.ok);
    const AssetCacheParseResult parsed = parseAssetCache(indexText.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    const AssetCacheEntry* const aEntry = parsed.index.find(*db.guidForPath("a.gltf"));
    const AssetCacheEntry* const bEntry = parsed.index.find(*db.guidForPath("b.gltf"));
    REQUIRE(aEntry != nullptr);
    REQUIRE(bEntry != nullptr);
    CHECK(aEntry->importer == "gltf");  // probed
    CHECK(bEntry->importer.empty());    // NEVER probed -- the budget ran out before it was even opened
}

TEST_CASE(
    "asset_database: a model with TWO distinct textures records them SORTED and deduplicated in its "
    "cache entry (AD-i12, INV-M8, seed S23)") {
    // THE GAP THIS CLOSES. AD-i4 already covers dedup and the self-edge, but every phase-7.5 assertion
    // in this file ends at `dependencies.size() == 1`, and a one-element vector is sorted no matter
    // what -- so seed S23 (dropping the std::sort/std::unique on outcome.dependencies) reddened
    // nothing anywhere. The dedup AD-i4 *appears* to prove happens one layer up, in gltf_import.cpp's
    // phase 3 (recordExternalUri's own std::find), never in phase 7.5.
    //
    // A scan-probed model with TWO dependencies is the smallest construction that can see the SORT.
    //
    // Measured while proving this case, and recorded rather than implied: a variant seed dropping ONLY
    // the `std::unique` (keeping the `std::sort`) reddens NOTHING here or in AD-i4. gltf_import.cpp's
    // recordExternalUri already deduplicates `externalUris` with its own std::find, and two DISTINCT
    // project-relative paths resolving to ONE GUID is not reachable from this tier (the scan dedups
    // directories by canonical physical path, so an aliased path earns no record and guidForPath
    // returns nullopt for it). Phase 7.5's `unique` is defence in depth with no reachable input today;
    // the dependencies.size() == 2 assertion below asserts the OBSERVABLE -- two edges from three URIs
    // -- without claiming to say which layer collapsed the third.
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("models"), ec);
    REQUIRE_FALSE(ec);
    std::filesystem::create_directories(dir.join("textures"), ec);
    REQUIRE_FALSE(ec);
    // "wood.png" is named TWICE, by two URIs that normalise to the same path, so `unique` has real
    // work; "steel.png" is the second DISTINCT dependency, so `sort` has real work. Both are listed in
    // an order chosen to be the REVERSE of their GUID order -- see the precondition REQUIRE below.
    writeFile(dir.join("models/chair.gltf"), R"({"asset":{"version":"2.0"},"images":[)"
                                             R"({"uri":"../textures/wood.png"},)"
                                             R"({"uri":"../textures/steel.png"},)"
                                             R"({"uri":"sub/../../textures/wood.png"}]})");
    writeFile(dir.join("textures/steel.png"), "steel-pixels");
    writeFile(dir.join("textures/wood.png"), "wood-pixels");

    AssetDatabase db;
    GuidGenerator gen(1012);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(report.status == ScanStatus::Ok);
    CHECK(report.modelsProbed == 1);
    CHECK(report.dependenciesRecorded == 2);  // TWO edges -- three URIs, one duplicate collapsed

    const Guid woodGuid = *db.guidForPath("textures/wood.png");
    const Guid steelGuid = *db.guidForPath("textures/steel.png");
    REQUIRE(woodGuid.valid());
    REQUIRE(steelGuid.valid());
    // THE PRECONDITION THAT KEEPS THIS CASE A DISCRIMINATOR. Phase 7.5 pushes dependencies in the
    // document's own URI order (wood, then steel), so this case can only see a missing `sort` while
    // that order is the REVERSE of the GUID order. GuidGenerator is deterministic given its seed, so
    // this holds on all three OSes -- but if the generator, the seeding order or the fixture's names
    // ever change, this REQUIRE fails LOUDLY rather than letting the case rot into a silent pass.
    REQUIRE(steelGuid < woodGuid);

    const AssetRecord* const chairRecord = db.findByPath("models/chair.gltf");
    REQUIRE(chairRecord != nullptr);
    const auto indexText = scene_golden::readBytes(dir.join("Library/asset-cache.json"));
    REQUIRE(indexText.ok);
    const AssetCacheParseResult parsed = parseAssetCache(indexText.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    const AssetCacheEntry* const entry = parsed.index.find(chairRecord->guid);
    REQUIRE(entry != nullptr);

    REQUIRE(entry->dependencies.size() == 2);  // two edges from three URIs
    // SORTED -- the assertion seed S23 cannot survive.
    CHECK(std::is_sorted(entry->dependencies.begin(), entry->dependencies.end()));
    CHECK(entry->dependencies[0] == steelGuid);  // ... and sorted into THIS order, not the document's
    CHECK(entry->dependencies[1] == woodGuid);
    for (const Guid& dep : entry->dependencies) {
        CHECK(dep.valid());               // INV-M8: never a nil GUID
        CHECK(dep != chairRecord->guid);  // INV-M8: never a self-edge
    }
}

TEST_CASE(
    "asset_database: a model whose buffer is a real EXTERNAL .bin still probes clean -- the scan runs "
    "Structure, never Full (AD-i13, INV-M4, seed S9)") {
    // THE GAP THIS CLOSES. Seed S9 (phase 7.5 running ImportDepth::Full instead of Structure) reddened
    // nothing -- the contingency the plan itself flagged. The measured root cause is the fixtures, not
    // the assertions: EVERY buffer in every committed .gltf fixture uses a `data:` URI, which fastgltf
    // decodes during parse, so EditorBufferAdapter's sources::URI arm never runs and a Full probe
    // succeeds identically to a Structure one.
    //
    // A GENUINE external .bin makes the two depths structurally different at scan time, because phase
    // 7.5 hands importModel an EMPTY external-buffer span ({}) by construction -- it never reads a
    // dependency's bytes, only the model's own. So:
    //   Structure -> no accessor is touched at all       -> Ok, importer recorded, dependency recorded
    //   Full      -> the adapter is asked for the buffer -> MissingBuffer -> an import FAILURE, no
    //                                                       importer, no dependencies, nothing probed
    // The .bin is deliberately present on disk WITH valid bytes: that is what makes this a proof that
    // the scan does not decode a vertex, rather than a proof that the file was missing.
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.join("models"), ec);
    REQUIRE_FALSE(ec);
    writeFile(dir.join("models/chair.gltf"),
              R"({"asset":{"version":"2.0"},"meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4}]}],)"
              R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
              R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
              R"("buffers":[{"byteLength":36,"uri":"chair.bin"}]})");
    writeFile(dir.join("models/chair.bin"), std::string(36, '\0'));  // 3 * VEC3 of float zeroes

    AssetDatabase db;
    GuidGenerator gen(1013);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(report.status == ScanStatus::Ok);

    // ALL FIVE of these invert under seed S9.
    CHECK(report.modelsProbed == 1);
    CHECK(report.importFailureTotal == 0);
    CHECK(report.importFailures.empty());
    CHECK(report.dependenciesRecorded == 1);

    const Guid binGuid = *db.guidForPath("models/chair.bin");
    REQUIRE(binGuid.valid());
    const AssetRecord* const chairRecord = db.findByPath("models/chair.gltf");
    REQUIRE(chairRecord != nullptr);
    const auto indexText = scene_golden::readBytes(dir.join("Library/asset-cache.json"));
    REQUIRE(indexText.ok);
    const AssetCacheParseResult parsed = parseAssetCache(indexText.text);
    REQUIRE(parsed.outcome == CacheLoadOutcome::Ok);
    const AssetCacheEntry* const entry = parsed.index.find(chairRecord->guid);
    REQUIRE(entry != nullptr);
    CHECK(entry->importer == "gltf");
    CHECK(entry->importerVersion == 1);
    REQUIRE(entry->dependencies.size() == 1);
    CHECK(entry->dependencies[0] == binGuid);
}

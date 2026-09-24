// tests/editor/asset_actions_test.cpp -- task 3.1.3, Step 9: the orphan-sidecar delete planner
// (validateOrphanPath) and the action itself (deleteOrphanMeta) -- the first destructive path in the
// editor (R5). A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT
// define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED (D4/AC-17/INV-P5, the asset_meta_test.cpp precedent): asset_actions.hpp depends on nothing
// that needs reflection -- every case here must be PRESENT and PASSING in all three build
// configurations. Tier-0 except for real, bounded disk I/O through a scratch TempDir (the EIGHTH
// TU-local copy of that shape, asset_database_test.cpp:52-70's precedent).
#include <aero/core/guid.hpp>
#include <aero/editor/asset_actions.hpp>
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/text_file.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
    #include <unistd.h>  // geteuid -- AA17's vacuity guard
#endif

using engine::Guid;
using engine::GuidGenerator;
using engine::editor::deleteOrphanMeta;
using engine::editor::ensureDirectory;
using engine::editor::fileExists;
using engine::editor::OrphanDeleteRefusal;
using engine::editor::OrphanDeleteResult;
using engine::editor::validateOrphanPath;
using engine::editor::writeMetaText;
using engine::editor::writeTextFileAtomic;

namespace {

// The EIGHTH copy of this shape (text_file_test.cpp's own precedent, itself the seventh).
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_asset_actions_test_" + std::to_string(++counter));
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

[[nodiscard]] std::filesystem::path pathOf(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

void writeBytes(std::string_view absolutePath, std::string_view bytes) {
    std::ofstream out(pathOf(absolutePath), std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

// ================================================================================================
// validateOrphanPath -- pure, tier-0
// ================================================================================================

TEST_CASE("asset actions: validateOrphanPath(\"wood.png.meta\") -> None (AA1)") {
    CHECK(validateOrphanPath("wood.png.meta") == OrphanDeleteRefusal::None);
}

TEST_CASE("asset actions: a nested sidecar validates (AA2)") {
    CHECK(validateOrphanPath("tex/wood.png.meta") == OrphanDeleteRefusal::None);
}

TEST_CASE("asset actions: a name not ending .meta -> NotAMetaName (AA3)") {
    CHECK(validateOrphanPath("wood.png") == OrphanDeleteRefusal::NotAMetaName);
}

TEST_CASE("asset actions: \".meta\" alone -> NotAMetaName (AA4)") {
    CHECK(validateOrphanPath(".meta") == OrphanDeleteRefusal::NotAMetaName);
}

TEST_CASE("asset actions: an empty path -> NotAMetaName (AA5)") {
    CHECK(validateOrphanPath("") == OrphanDeleteRefusal::NotAMetaName);
}

TEST_CASE("asset actions: an absolute POSIX path -> EscapesRoot (AA6, E24, seed S19)") {
    CHECK(validateOrphanPath("/x/y.meta") == OrphanDeleteRefusal::EscapesRoot);
}

TEST_CASE("asset actions: a rooted Windows path -> EscapesRoot (AA7, E24)") {
    CHECK(validateOrphanPath("C:/x/y.meta") == OrphanDeleteRefusal::EscapesRoot);
}

TEST_CASE("asset actions: any \"..\" segment -> EscapesRoot (AA8, E24, seed S19)") {
    CHECK(validateOrphanPath("../x.meta") == OrphanDeleteRefusal::EscapesRoot);
    CHECK(validateOrphanPath("a/../x.meta") == OrphanDeleteRefusal::EscapesRoot);
    // A trailing ".." leaf ("a/..") is caught by the isMetaFileName check FIRST -- its leaf is "..",
    // which is not a sidecar name at all, so it is NotAMetaName, never reaching the EscapesRoot arm.
    // Not tested here for that reason: it would assert an unreachable code path.
}

TEST_CASE("asset actions: any backslash -> EscapesRoot (AA9)") {
    CHECK(validateOrphanPath("a\\b.meta") == OrphanDeleteRefusal::EscapesRoot);
}

TEST_CASE("asset actions: a .META suffix is accepted, case-insensitive (AA10, consistency)") {
    CHECK(validateOrphanPath("wood.png.META") == OrphanDeleteRefusal::None);
}

// ================================================================================================
// deleteOrphanMeta -- real, bounded disk I/O
// ================================================================================================

TEST_CASE("asset actions: a genuine orphan is deleted (AA11, AC-18/AC-19)") {
    const TempDir dir;
    GuidGenerator gen(11);
    REQUIRE(writeTextFileAtomic(dir.join("wood.png.meta"), writeMetaText(gen.next())).empty());
    REQUIRE(fileExists(dir.join("wood.png.meta")));

    const OrphanDeleteResult result = deleteOrphanMeta(dir.utf8(), "wood.png.meta");
    CHECK(result.deleted);
    CHECK(result.message.empty());
    CHECK_FALSE(fileExists(dir.join("wood.png.meta")));
}

TEST_CASE("asset actions: nothing ELSE in the directory changes (AA12, AC-19, seed S22)") {
    const TempDir dir;
    GuidGenerator gen(12);
    REQUIRE(writeTextFileAtomic(dir.join("wood.png.meta"), writeMetaText(gen.next())).empty());
    writeBytes(dir.join("sibling.txt"), "unrelated content");
    REQUIRE(writeTextFileAtomic(dir.join("sibling.txt.meta"), writeMetaText(gen.next())).empty());

    const OrphanDeleteResult result = deleteOrphanMeta(dir.utf8(), "wood.png.meta");
    REQUIRE(result.deleted);

    CHECK(fileExists(dir.join("sibling.txt")));
    CHECK(fileExists(dir.join("sibling.txt.meta")));
    std::error_code ec;
    const std::uintmax_t siblingSize = std::filesystem::file_size(pathOf(dir.join("sibling.txt")), ec);
    REQUIRE_FALSE(ec);
    CHECK(siblingSize == std::string("unrelated content").size());
}

TEST_CASE("asset actions: an asset that EXISTS again -> AssetPresent, file still on disk (AA13, E22, seed S20)") {
    const TempDir dir;
    GuidGenerator gen(13);
    REQUIRE(writeTextFileAtomic(dir.join("wood.png.meta"), writeMetaText(gen.next())).empty());
    writeBytes(dir.join("wood.png"), "not actually an orphan");

    const OrphanDeleteResult result = deleteOrphanMeta(dir.utf8(), "wood.png.meta");
    CHECK_FALSE(result.deleted);
    CHECK(result.refusal == OrphanDeleteRefusal::AssetPresent);
    CHECK(fileExists(dir.join("wood.png.meta")));
}

TEST_CASE("asset actions: a .meta that does not PARSE -> NotAMeta, file still on disk (AA14, E23, seed S21)") {
    const TempDir dir;
    writeBytes(dir.join("wood.png.meta"), "not json at all");

    const OrphanDeleteResult result = deleteOrphanMeta(dir.utf8(), "wood.png.meta");
    CHECK_FALSE(result.deleted);
    CHECK(result.refusal == OrphanDeleteRefusal::NotAMeta);
    CHECK(fileExists(dir.join("wood.png.meta")));
}

TEST_CASE("asset actions: a .meta with a NIL guid -> NotAMeta, still on disk (AA15, E23)") {
    const TempDir dir;
    REQUIRE(writeTextFileAtomic(dir.join("wood.png.meta"), writeMetaText(Guid{})).empty());

    const OrphanDeleteResult result = deleteOrphanMeta(dir.utf8(), "wood.png.meta");
    CHECK_FALSE(result.deleted);
    CHECK(result.refusal == OrphanDeleteRefusal::NotAMeta);
    CHECK(fileExists(dir.join("wood.png.meta")));
}

TEST_CASE("asset actions: a missing file -> Missing, no throw (AA16, E21)") {
    const TempDir dir;
    const OrphanDeleteResult result = deleteOrphanMeta(dir.utf8(), "nope.png.meta");
    CHECK_FALSE(result.deleted);
    CHECK(result.refusal == OrphanDeleteRefusal::Missing);
}

TEST_CASE("asset actions: a read-only directory -> RemoveFailed, file still on disk (AA17, E25)") {
#if defined(_WIN32)
    MESSAGE("skipped on Windows: POSIX permission semantics do not apply");
#else
    if (geteuid() == 0) {
        MESSAGE("skipped as root: the mode bits are ignored, so this case would pass vacuously");
    } else {
        const TempDir dir;
        GuidGenerator gen(17);
        REQUIRE(writeTextFileAtomic(dir.join("wood.png.meta"), writeMetaText(gen.next())).empty());

        std::error_code ec;
        std::filesystem::permissions(pathOf(dir.utf8()),
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::replace, ec);
        REQUIRE_FALSE(ec);

        const OrphanDeleteResult result = deleteOrphanMeta(dir.utf8(), "wood.png.meta");

        std::filesystem::permissions(pathOf(dir.utf8()), std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
        CHECK_FALSE(ec);  // restored BEFORE any assertion can fail and before ~TempDir runs

        CHECK_FALSE(result.deleted);
        CHECK(result.refusal == OrphanDeleteRefusal::RemoveFailed);
        CHECK_FALSE(result.message.empty());
    }
#endif
}

TEST_CASE("asset actions: after EVERY refusal branch, the file is still on disk (AA18, seed S22)") {
    const TempDir dir;
    GuidGenerator gen(18);

    // NotAMetaName -- deliberately never reaches disk, but the parameterised sweep is honest about it.
    CHECK(validateOrphanPath("wood.png") == OrphanDeleteRefusal::NotAMetaName);

    // EscapesRoot
    CHECK(validateOrphanPath("../wood.png.meta") == OrphanDeleteRefusal::EscapesRoot);

    // NotAMeta
    writeBytes(dir.join("bad.meta"), "not json");
    REQUIRE(deleteOrphanMeta(dir.utf8(), "bad.meta").refusal == OrphanDeleteRefusal::NotAMeta);
    CHECK(fileExists(dir.join("bad.meta")));

    // AssetPresent
    REQUIRE(writeTextFileAtomic(dir.join("present.png.meta"), writeMetaText(gen.next())).empty());
    writeBytes(dir.join("present.png"), "x");
    REQUIRE(deleteOrphanMeta(dir.utf8(), "present.png.meta").refusal == OrphanDeleteRefusal::AssetPresent);
    CHECK(fileExists(dir.join("present.png.meta")));
}

TEST_CASE("asset actions: deleteOrphanMeta never throws (AA19, no-exceptions rule)") {
    const TempDir dir;
    CHECK_NOTHROW((void)deleteOrphanMeta(dir.utf8(), "../escaped.meta"));
    CHECK_NOTHROW((void)deleteOrphanMeta(dir.utf8(), "nope.meta"));
    CHECK_NOTHROW((void)deleteOrphanMeta("", "x.meta"));
    CHECK_NOTHROW((void)deleteOrphanMeta(dir.utf8(), ""));
}

TEST_CASE("asset actions: a NESTED orphan checks the asset in the SAME directory (AA20, correctness)") {
    const TempDir dir;
    GuidGenerator gen(20);
    REQUIRE(ensureDirectory(dir.join("tex")).empty());
    REQUIRE(writeTextFileAtomic(dir.join("tex/wood.png.meta"), writeMetaText(gen.next())).empty());
    // A file NAMED "wood.png" existing at the ROOT (not in tex/) must NOT block the delete -- the
    // check must look at "tex/wood.png", not "wood.png".
    writeBytes(dir.join("wood.png"), "a decoy at the wrong level");

    const OrphanDeleteResult result = deleteOrphanMeta(dir.utf8(), "tex/wood.png.meta");
    CHECK(result.deleted);
}

TEST_CASE("asset actions: a directory named x.meta is refused, directory intact (AA21, robustness)") {
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directory(pathOf(dir.join("x.meta")), ec);
    REQUIRE_FALSE(ec);

    const OrphanDeleteResult result = deleteOrphanMeta(dir.utf8(), "x.meta");
    CHECK_FALSE(result.deleted);
    CHECK(result.refusal == OrphanDeleteRefusal::NotAMeta);  // readTextFile refuses a directory
    CHECK(std::filesystem::is_directory(pathOf(dir.join("x.meta")), ec));
}

TEST_CASE(
    "asset actions: an empty assetsRootUtf8 -> EscapesRoot, refused explicitly, nothing touched (AA22, "
    "E16, code-review finding 5)") {
    // code-review finding 5: the OLD verdict here was Missing, true only because "/wood.png.meta"
    // happened not to exist on this machine -- not because the code refused an unresolvable root. The
    // root is now validated BEFORE assetsRootUtf8 is ever concatenated into a path at all.
    const OrphanDeleteResult result = deleteOrphanMeta("", "wood.png.meta");
    CHECK_FALSE(result.deleted);
    CHECK(result.refusal == OrphanDeleteRefusal::EscapesRoot);
}

TEST_CASE(
    "asset actions: a RELATIVE (non-absolute) assetsRootUtf8 -> EscapesRoot, refused explicitly (AA23, "
    "code-review finding 5)") {
    const OrphanDeleteResult result = deleteOrphanMeta("relative/root", "wood.png.meta");
    CHECK_FALSE(result.deleted);
    CHECK(result.refusal == OrphanDeleteRefusal::EscapesRoot);
}

TEST_CASE(
    "asset actions: a real, absolute assetsRootUtf8 is accepted by the root guard -- a genuine orphan "
    "still deletes (AA24, code-review finding 5, no regression)") {
    const TempDir dir;
    GuidGenerator gen(24);
    REQUIRE(writeTextFileAtomic(dir.join("wood.png.meta"), writeMetaText(gen.next())).empty());
    const OrphanDeleteResult result = deleteOrphanMeta(dir.utf8(), "wood.png.meta");
    CHECK(result.deleted);
    CHECK(result.message.empty());
}

// ================================================================================================
// task E.4.3, commit 1 -- the validateRelativeAssetPath promotion
// ================================================================================================

TEST_CASE(
    "asset actions: validateRelativeAssetPath agrees with validateOrphanPath's path half on fourteen "
    "inputs (AA25, task E.4.3)") {
    // The promotion is behaviour-free, and AA1-AA24 passing UNEDITED is the primary proof. This is
    // the secondary one: for every shape, the shared validator and the delegate answer the same.
    // ".."-as-a-SEGMENT is the rule, never ".." as a prefix -- "..config" is a legal leaf name and
    // nothing pinned that before.
    struct Row {
        std::string_view path;
        bool safe;
    };
    constexpr std::array<Row, 14> ROWS{{
        {"", false},
        {"a", true},
        {"a/b", true},
        {"a\\b", false},
        {"/a", false},
        {"C:/a", false},
        {"c:a", false},
        {"../a", false},
        {"a/../b", false},
        {"a/..", false},
        {"..config", true},
        {"a/..config", true},
        {"a/b/..", false},
        {"a./b", true},
    }};
    std::size_t safeCount = 0;
    std::size_t refusedCount = 0;
    for (const Row& row : ROWS) {
        CAPTURE(row.path);
        const engine::editor::AssetOpRefusal shared = engine::editor::validateRelativeAssetPath(row.path);
        CHECK(((shared == engine::editor::AssetOpRefusal::None) == row.safe));
        if (row.safe) {
            ++safeCount;
        } else {
            ++refusedCount;
        }
    }
    // ANTI-VACUITY: a table that is all one way proves nothing about the other direction.
    REQUIRE(safeCount >= 4U);
    REQUIRE(refusedCount >= 4U);

    SUBCASE("and the delegate answers EscapesRoot on exactly the paths the shared validator refuses") {
        // A SEPARATE roster, spelled as whole sidecar paths rather than derived by appending ".meta"
        // to the rows above. Appending would be wrong in two distinct ways and both are silent:
        // "" + ".meta" is ".meta", which isMetaFileName REFUSES (5 bytes, not > 5), so the leaf test
        // wins and the path half is never reached; and "a/.." + ".meta" is "a/...meta", whose last
        // segment is no longer ".." at all, so the very rule under test disappears from the input.
        constexpr std::array<Row, 9> SIDECARS{{
            {"wood.png.meta", true},
            {"a/wood.png.meta", true},
            {"..config/wood.png.meta", true},
            {"a./wood.png.meta", true},
            {"a\\wood.png.meta", false},
            {"/wood.png.meta", false},
            {"C:/wood.png.meta", false},
            {"../wood.png.meta", false},
            {"a/../wood.png.meta", false},
        }};
        std::size_t agreements = 0;
        for (const Row& row : SIDECARS) {
            CAPTURE(row.path);
            const bool sharedSafe =
                engine::editor::validateRelativeAssetPath(row.path) == engine::editor::AssetOpRefusal::None;
            const bool delegateSafe = validateOrphanPath(row.path) != OrphanDeleteRefusal::EscapesRoot;
            CHECK(sharedSafe == row.safe);
            CHECK(delegateSafe == row.safe);
            ++agreements;
        }
        REQUIRE(agreements == SIDECARS.size());  // ANTI-VACUITY: the loop ran
    }
}

TEST_CASE("asset actions: the delegate still runs its LEAF test FIRST (AA26, task E.4.3)") {
    // The order is load-bearing and is why AA1-AA24 pass unedited: today's body checks the leaf
    // first and the path shape second. Had the promotion swapped them, an empty path would start
    // reporting EscapesRoot instead of NotAMetaName and at least two existing cases would flip.
    CHECK((validateOrphanPath("") == OrphanDeleteRefusal::NotAMetaName));
    // "../x.png" escapes AND is not a sidecar name; the leaf test wins because it runs first.
    CHECK((validateOrphanPath("../x.png") == OrphanDeleteRefusal::NotAMetaName));
    // The control: with a sidecar leaf, the SAME escaping path does reach the path test.
    CHECK((validateOrphanPath("../x.png.meta") == OrphanDeleteRefusal::EscapesRoot));
}

// ================================================================================================
// task E.4.3, commit 2 -- the pure planner. NO DISK anywhere in this block.
// ================================================================================================

namespace {

using engine::editor::AssetNameRefusal;
using engine::editor::AssetOpInputs;
using engine::editor::AssetOpKind;
using engine::editor::AssetOpPlan;
using engine::editor::AssetOpRefusal;
using engine::editor::AssetPathBase;
using engine::editor::DirectoryListing;
using engine::editor::FileEntry;
using engine::editor::ScanStatus;

// A complete, readable listing holding exactly these leaves. `listingIsComplete` needs status Ok,
// truncated false and skipped 0, which is this helper's whole point: a hand-built listing that
// forgets one of the three silently takes rung 8 instead of the rung under test.
[[nodiscard]] DirectoryListing completeListing(std::initializer_list<std::string_view> names) {
    DirectoryListing listing;
    listing.status = ScanStatus::Ok;
    for (const std::string_view name : names) {
        FileEntry entry;
        entry.name = std::string(name);
        listing.entries.push_back(entry);
    }
    return listing;
}

}  // namespace

TEST_CASE("asset actions: validateAssetName -- emptiness and the length boundary, both sides (AA27)") {
    using engine::editor::MAX_ASSET_NAME_BYTES;
    using engine::editor::validateAssetName;
    CHECK((validateAssetName("") == AssetNameRefusal::Empty));
    // Built, never spelled: a 255-byte literal in a test file is unreadable and easy to miscount.
    CHECK((validateAssetName(std::string(MAX_ASSET_NAME_BYTES, 'a')) == AssetNameRefusal::None));
    CHECK((validateAssetName(std::string(MAX_ASSET_NAME_BYTES + 1, 'a')) == AssetNameRefusal::TooLong));
    CHECK((validateAssetName("wood.png") == AssetNameRefusal::None));  // the accepting control
}

TEST_CASE("asset actions: validateAssetName -- the shape rules (AA28)") {
    using engine::editor::validateAssetName;
    CHECK((validateAssetName("a/b") == AssetNameRefusal::HasSeparator));
    CHECK((validateAssetName("a\\b") == AssetNameRefusal::HasSeparator));
    CHECK((validateAssetName(".") == AssetNameRefusal::DotOrDotDot));
    CHECK((validateAssetName("..") == AssetNameRefusal::DotOrDotDot));
    CHECK((validateAssetName(".hidden") == AssetNameRefusal::Hidden));
    CHECK((validateAssetName(std::string("a\x01") + "b") == AssetNameRefusal::ControlCharacter));

    SUBCASE("each reserved byte in its own right") {
        // Seven subcases, one per byte, so a table that lost one is a named failure rather than a
        // silently narrower check.
        for (const char c : std::string_view("*?\"<>|:")) {
            CAPTURE(c);
            CHECK((validateAssetName(std::string("a") + c + "b") == AssetNameRefusal::ReservedCharacter));
        }
    }

    SUBCASE("a UTF-8 multi-byte name is LEGAL") {
        // THE ONE THAT MATTERS. A validator walking `char` as SIGNED reads 0xc3 as negative, and a
        // naive `c < 0x20` control-character test then fires on every accented name in the world.
        CHECK((validateAssetName("caf\xc3\xa9.png") == AssetNameRefusal::None));
    }
}

TEST_CASE("asset actions: validateAssetName -- the Windows rules and the two suffixes (AA29)") {
    using engine::editor::validateAssetName;
    CHECK((validateAssetName("name.") == AssetNameRefusal::TrailingDotOrSpace));
    CHECK((validateAssetName("name ") == AssetNameRefusal::TrailingDotOrSpace));
    CHECK((validateAssetName("CON") == AssetNameRefusal::ReservedDeviceName));
    CHECK((validateAssetName("con") == AssetNameRefusal::ReservedDeviceName));
    CHECK((validateAssetName("CON.png") == AssetNameRefusal::ReservedDeviceName));
    CHECK((validateAssetName("COM1") == AssetNameRefusal::ReservedDeviceName));
    CHECK((validateAssetName("LPT9") == AssetNameRefusal::ReservedDeviceName));
    CHECK((validateAssetName("nul.tar.gz") == AssetNameRefusal::ReservedDeviceName));

    SUBCASE("the near-misses, which are what prove the matcher is not a prefix test") {
        CHECK((validateAssetName("CONS") == AssetNameRefusal::None));
        CHECK((validateAssetName("COM0") == AssetNameRefusal::None));
        CHECK((validateAssetName("COM10") == AssetNameRefusal::None));
    }

    CHECK((validateAssetName("x.meta") == AssetNameRefusal::MetaSuffix));
    CHECK((validateAssetName("x.aero-tmp") == AssetNameRefusal::TempSuffix));
}

TEST_CASE("asset actions: planAssetOp -- step composition, the mechanical statement of INV-A8 (AA30)") {
    using engine::editor::planAssetOp;

    SUBCASE("(a) a FILE rename plans exactly TWO steps, both assets-relative") {
        AssetOpInputs inputs;
        inputs.sourceRelative = "a/b.png";
        inputs.sourceIsDirectory = false;
        inputs.newLeaf = "c.png";
        const AssetOpPlan plan = planAssetOp(AssetOpKind::Rename, inputs, completeListing({"b.png", "b.png.meta"}));
        REQUIRE((plan.refusal == AssetOpRefusal::None));
        REQUIRE(plan.stepCount == 2U);
        CHECK(plan.steps[0].from.relative == "a/b.png");
        CHECK(plan.steps[0].to.relative == "a/c.png");
        CHECK(plan.steps[1].from.relative == "a/b.png.meta");
        CHECK(plan.steps[1].to.relative == "a/c.png.meta");
        CHECK((plan.steps[0].from.base == AssetPathBase::AssetsRoot));
        CHECK((plan.steps[0].to.base == AssetPathBase::AssetsRoot));
        CHECK((plan.steps[1].from.base == AssetPathBase::AssetsRoot));
        CHECK((plan.steps[1].to.base == AssetPathBase::AssetsRoot));
        CHECK(plan.resultingRelativePath == "a/c.png");
    }

    SUBCASE("(b) a FOLDER rename plans exactly ONE -- a folder has no sidecar") {
        AssetOpInputs inputs;
        inputs.sourceRelative = "a/tex";
        inputs.sourceIsDirectory = true;
        inputs.newLeaf = "textures";
        const AssetOpPlan plan = planAssetOp(AssetOpKind::Rename, inputs, completeListing({"tex"}));
        REQUIRE((plan.refusal == AssetOpRefusal::None));
        CHECK(plan.stepCount == 1U);
        CHECK(plan.steps[0].from.relative == "a/tex");
        CHECK(plan.steps[0].to.relative == "a/textures");
        CHECK(plan.resultingRelativePath == "a/textures");
    }

    SUBCASE("(c) a Move into the assets ROOT is legal") {
        AssetOpInputs inputs;
        inputs.sourceRelative = "a/b.png";
        inputs.destinationDirRelative = "";
        const AssetOpPlan plan = planAssetOp(AssetOpKind::Move, inputs, completeListing({"a"}));
        REQUIRE((plan.refusal == AssetOpRefusal::None));
        REQUIRE(plan.stepCount == 2U);
        CHECK(plan.steps[0].from.relative == "a/b.png");
        CHECK(plan.steps[0].to.relative == "b.png");
        CHECK(plan.steps[1].from.relative == "a/b.png.meta");
        CHECK(plan.steps[1].to.relative == "b.png.meta");
        CHECK(plan.resultingRelativePath == "b.png");
    }

    SUBCASE("(d) a Delete is MIXED-BASE, and that is what a single-base design gets silently wrong") {
        AssetOpInputs inputs;
        inputs.sourceRelative = "a/b.png";
        inputs.trashSequence = 7;
        const AssetOpPlan plan = planAssetOp(AssetOpKind::Delete, inputs, DirectoryListing{});
        REQUIRE((plan.refusal == AssetOpRefusal::None));
        REQUIRE(plan.stepCount == 2U);
        CHECK((plan.steps[0].from.base == AssetPathBase::AssetsRoot));
        CHECK(plan.steps[0].from.relative == "a/b.png");
        CHECK((plan.steps[0].to.base == AssetPathBase::ProjectRoot));
        CHECK(plan.steps[0].to.relative == "Library/Trash/0007/a/b.png");
        CHECK((plan.steps[1].to.base == AssetPathBase::ProjectRoot));
        CHECK(plan.steps[1].to.relative == "Library/Trash/0007/a/b.png.meta");
        CHECK((plan.directoryToCreate.base == AssetPathBase::ProjectRoot));
        CHECK(plan.directoryToCreate.relative == "Library/Trash/0007/a");
        CHECK(plan.resultingRelativePath.empty());  // ALWAYS "" for Delete
    }
}

TEST_CASE("asset actions: classifyAssetMove -- the ancestor battery, SEGMENT-WISE (AA31)") {
    using engine::editor::classifyAssetMove;
    CHECK((classifyAssetMove("textures", "textures") == AssetOpRefusal::DestinationInsideSource));
    CHECK((classifyAssetMove("textures", "textures/sub") == AssetOpRefusal::DestinationInsideSource));
    // THE PAIR A RAW starts_with FAILS. Neither is inside the other.
    CHECK((classifyAssetMove("textures", "tex") == AssetOpRefusal::None));
    CHECK((classifyAssetMove("tex", "textures/a") == AssetOpRefusal::None));
    CHECK((classifyAssetMove("a/b.png", "a") == AssetOpRefusal::AlreadyThere));
    CHECK((classifyAssetMove("b.png", "") == AssetOpRefusal::AlreadyThere));
    CHECK((classifyAssetMove("a/b.png", "") == AssetOpRefusal::None));
    CHECK((classifyAssetMove("", "a") == AssetOpRefusal::SourceIsRoot));
    CHECK((classifyAssetMove("../x", "a") == AssetOpRefusal::BadSourcePath));
    CHECK((classifyAssetMove("a", "../x") == AssetOpRefusal::BadDestination));
}

TEST_CASE("asset actions: rung 7 -- a missing destination is DestinationMissing, never ListingIncomplete (AA32)") {
    using engine::editor::planAssetOp;
    AssetOpInputs inputs;
    inputs.sourceRelative = "a/b.png";
    inputs.destinationDirRelative = "dst";

    SUBCASE("status Missing") {
        DirectoryListing listing;
        listing.status = ScanStatus::Missing;
        const AssetOpPlan plan = planAssetOp(AssetOpKind::Move, inputs, listing);
        CHECK((plan.refusal == AssetOpRefusal::DestinationMissing));
        // The WRONG answer this rung's position exists to prevent, stated explicitly.
        CHECK((plan.refusal != AssetOpRefusal::ListingIncomplete));
    }
    SUBCASE("status NotADirectory") {
        DirectoryListing listing;
        listing.status = ScanStatus::NotADirectory;
        const AssetOpPlan plan = planAssetOp(AssetOpKind::Move, inputs, listing);
        CHECK((plan.refusal == AssetOpRefusal::DestinationMissing));
        CHECK((plan.refusal != AssetOpRefusal::ListingIncomplete));
    }
}

TEST_CASE("asset actions: rung 8 -- an INCOMPLETE Ok listing is ListingIncomplete (AA33)") {
    using engine::editor::planAssetOp;
    AssetOpInputs inputs;
    inputs.sourceRelative = "a/b.png";
    inputs.destinationDirRelative = "dst";
    // BOTH subcases carry status == Ok, BECAUSE THAT IS THE TRAP: a case testing `status` alone
    // passes against an Ok PREFIX, which is the antivirus-lock / cloud-sync shape 3.1.4's D5 records
    // as real. Neither AA32 nor AA33 alone proves rung 7 sits above rung 8; the pair does.
    SUBCASE("truncated") {
        DirectoryListing listing;
        listing.status = ScanStatus::Ok;
        listing.truncated = true;
        const AssetOpPlan plan = planAssetOp(AssetOpKind::Move, inputs, listing);
        CHECK((plan.refusal == AssetOpRefusal::ListingIncomplete));
        CHECK((plan.refusal != AssetOpRefusal::DestinationMissing));
    }
    SUBCASE("skipped") {
        DirectoryListing listing;
        listing.status = ScanStatus::Ok;
        listing.truncated = false;
        listing.skipped = 3;
        const AssetOpPlan plan = planAssetOp(AssetOpKind::Move, inputs, listing);
        CHECK((plan.refusal == AssetOpRefusal::ListingIncomplete));
        CHECK((plan.refusal != AssetOpRefusal::DestinationMissing));
    }
}

TEST_CASE("asset actions: rung 9 -- NameTaken and the case-only carve-out (AA34)") {
    using engine::editor::planAssetOp;

    SUBCASE("an occupied name is refused") {
        AssetOpInputs inputs;
        inputs.sourceRelative = "a/wood.png";
        inputs.newLeaf = "other.png";
        const AssetOpPlan plan = planAssetOp(AssetOpKind::Rename, inputs, completeListing({"wood.png", "other.png"}));
        CHECK((plan.refusal == AssetOpRefusal::NameTaken));
    }
    SUBCASE("a case-ONLY rename is PERMITTED -- the carve-out") {
        AssetOpInputs inputs;
        inputs.sourceRelative = "a/wood.png";
        inputs.newLeaf = "Wood.png";
        const AssetOpPlan plan = planAssetOp(AssetOpKind::Rename, inputs, completeListing({"wood.png"}));
        CHECK((plan.refusal == AssetOpRefusal::None));
    }
    SUBCASE("the carve-out does NOT license overwriting a DIFFERENT file") {
        AssetOpInputs inputs;
        inputs.sourceRelative = "a/wood.png";
        inputs.newLeaf = "Wood.png";
        const AssetOpPlan plan = planAssetOp(AssetOpKind::Rename, inputs, completeListing({"wood.png", "Wood.png"}));
        CHECK((plan.refusal == AssetOpRefusal::NameTaken));
    }
    SUBCASE("the carve-out is RENAME-only: a Move into another directory has no such argument") {
        AssetOpInputs inputs;
        inputs.sourceRelative = "a/wood.png";
        inputs.destinationDirRelative = "b";
        const AssetOpPlan plan = planAssetOp(AssetOpKind::Move, inputs, completeListing({"Wood.png"}));
        CHECK((plan.refusal == AssetOpRefusal::NameTaken));
    }
}

TEST_CASE("asset actions: rung 10 -- SidecarBlocked, refused BEFORE any step is emitted (AA35)") {
    using engine::editor::planAssetOp;
    AssetOpInputs inputs;
    inputs.sourceRelative = "a/b.png";
    inputs.newLeaf = "c.png";
    // c.png.meta is occupied while c.png is free: rung 9 passes and rung 10 is what refuses.
    const AssetOpPlan plan = planAssetOp(AssetOpKind::Rename, inputs, completeListing({"b.png", "c.png.meta"}));
    CHECK((plan.refusal == AssetOpRefusal::SidecarBlocked));
    // THE REAL ASSERTION: this is the rung that stops a half-move at the one point where refusing is
    // still free, so nothing may be emitted.
    CHECK(plan.stepCount == 0U);
}

TEST_CASE("asset actions: assetDeletePromptFor -- the FOLDER body, byte for byte (AA38)") {
    using engine::editor::assetDeletePromptFor;
    const engine::editor::AssetDeletePrompt prompt = assetDeletePromptFor("textures", true, 34);
    CHECK(prompt.title == "Delete folder \"textures\"?");
    CHECK(prompt.detail == "It holds 34 indexed assets. Everything inside it moves to the project trash together.");
    CHECK(prompt.footer == "The files are not erased -- they move to Library/Trash/ inside this project.");
}

TEST_CASE("asset actions: assetDeletePromptFor -- the FILE body, and the count is IGNORED (AA39)") {
    using engine::editor::assetDeletePromptFor;
    const engine::editor::AssetDeletePrompt zero = assetDeletePromptFor("textures/wood.png", false, 0);
    CHECK(zero.title == "Delete \"textures/wood.png\"?");
    CHECK(zero.detail == "Its .meta sidecar moves with it, so its identity is preserved if you put it back.");
    CHECK(zero.footer == "The file is not erased -- it moves to Library/Trash/ inside this project.");
    // The half that proves indexedAssetCount is not leaking into the file body.
    const engine::editor::AssetDeletePrompt many = assetDeletePromptFor("textures/wood.png", false, 34);
    CHECK(many.title == zero.title);
    CHECK(many.detail == zero.detail);
    CHECK(many.footer == zero.footer);
}

TEST_CASE("asset actions: the folder body's plural agreement is arithmetic (AA40)") {
    using engine::editor::assetDeletePromptFor;
    CHECK(assetDeletePromptFor("f", true, 0).detail ==
          "It holds 0 indexed assets. Everything inside it moves to the project trash together.");
    CHECK(assetDeletePromptFor("f", true, 1).detail ==
          "It holds 1 indexed asset. Everything inside it moves to the project trash together.");
    CHECK(assetDeletePromptFor("f", true, 34).detail ==
          "It holds 34 indexed assets. Everything inside it moves to the project trash together.");
}

TEST_CASE("asset actions: assetNameRefusalMessage is TOTAL and None is empty (AA41)") {
    using engine::editor::assetNameRefusalMessage;
    // An explicit array of the twelve, never a cast-from-int loop, so a thirteenth enumerator is a
    // compile-visible edit HERE rather than a silently narrower sweep.
    constexpr std::array<AssetNameRefusal, 12> ALL{
        AssetNameRefusal::None,
        AssetNameRefusal::Empty,
        AssetNameRefusal::TooLong,
        AssetNameRefusal::HasSeparator,
        AssetNameRefusal::DotOrDotDot,
        AssetNameRefusal::Hidden,
        AssetNameRefusal::ControlCharacter,
        AssetNameRefusal::ReservedCharacter,
        AssetNameRefusal::TrailingDotOrSpace,
        AssetNameRefusal::ReservedDeviceName,
        AssetNameRefusal::MetaSuffix,
        AssetNameRefusal::TempSuffix,
    };
    CHECK(assetNameRefusalMessage(AssetNameRefusal::None).empty());
    std::size_t nonEmpty = 0;
    for (const AssetNameRefusal refusal : ALL) {
        CAPTURE(engine::editor::assetNameRefusalLabel(refusal));
        if (refusal == AssetNameRefusal::None) {
            continue;
        }
        CHECK_FALSE(assetNameRefusalMessage(refusal).empty());
        ++nonEmpty;
    }
    REQUIRE(nonEmpty == 11U);  // ANTI-VACUITY: the loop really ran over eleven
}

TEST_CASE("asset actions: no prompt or refusal string contains a '%' or a byte >= 0x80 (AA42)") {
    using engine::editor::assetDeletePromptFor;
    using engine::editor::assetNameRefusalMessage;
    const auto checkAscii = [](const std::string& text) {
        for (const char c : text) {
            CHECK(c != '%');  // so TextWrapped("%s", ...) is belt-and-braces rather than load-bearing
            CHECK(static_cast<unsigned char>(c) < 0x80U);  // so "--" is the ASCII double hyphen
        }
    };
    const engine::editor::AssetDeletePrompt folder = assetDeletePromptFor("textures", true, 34);
    const engine::editor::AssetDeletePrompt file = assetDeletePromptFor("textures/wood.png", false, 0);
    checkAscii(folder.title);
    checkAscii(folder.detail);
    checkAscii(folder.footer);
    checkAscii(file.title);
    checkAscii(file.detail);
    checkAscii(file.footer);
    constexpr std::array<AssetNameRefusal, 12> ALL{
        AssetNameRefusal::None,
        AssetNameRefusal::Empty,
        AssetNameRefusal::TooLong,
        AssetNameRefusal::HasSeparator,
        AssetNameRefusal::DotOrDotDot,
        AssetNameRefusal::Hidden,
        AssetNameRefusal::ControlCharacter,
        AssetNameRefusal::ReservedCharacter,
        AssetNameRefusal::TrailingDotOrSpace,
        AssetNameRefusal::ReservedDeviceName,
        AssetNameRefusal::MetaSuffix,
        AssetNameRefusal::TempSuffix,
    };
    for (const AssetNameRefusal refusal : ALL) {
        checkAscii(assetNameRefusalMessage(refusal));
    }
}

TEST_CASE("asset actions: the PEEK is a literal PREFIX of the ladder (AA43)") {
    using engine::editor::classifyAssetMove;
    using engine::editor::planAssetOp;
    struct Pair {
        std::string_view source;
        std::string_view destination;
    };
    constexpr std::array<Pair, 12> PAIRS{{
        {"textures", "textures"},
        {"textures", "textures/sub"},
        {"textures", "tex"},
        {"tex", "textures/a"},
        {"a/b.png", "a"},
        {"b.png", ""},
        {"a/b.png", ""},
        {"", "a"},
        {"../x", "a"},
        {"a", "../x"},
        {"a/b", "a/b/c"},
        {"a", "a"},
    }};
    std::set<AssetOpRefusal> distinct;
    for (const Pair& pair : PAIRS) {
        CAPTURE(pair.source);
        CAPTURE(pair.destination);
        const AssetOpRefusal peek = classifyAssetMove(pair.source, pair.destination);
        AssetOpInputs inputs;
        inputs.sourceRelative = pair.source;
        inputs.destinationDirRelative = pair.destination;
        // A complete, EMPTY Ok listing, so nothing below rung 8 can fire and the only refusals that
        // can differ are the ladder's own.
        const AssetOpPlan plan = planAssetOp(AssetOpKind::Move, inputs, completeListing({}));
        CHECK((peek == plan.refusal));
        distinct.insert(peek);
    }
    // ANTI-VACUITY: twelve pairs that all answered None would prove nothing.
    REQUIRE(distinct.size() >= 4U);
}

TEST_CASE("asset actions: trashRelativePathFor (AA44)") {
    using engine::editor::trashRelativePathFor;
    CHECK(trashRelativePathFor(1, "a.png") == "Library/Trash/0001/a.png");
    CHECK(trashRelativePathFor(7, "x/y/z.png") == "Library/Trash/0007/x/y/z.png");  // nesting preserved
    CHECK(trashRelativePathFor(9999, "a.png") == "Library/Trash/9999/a.png");
    CHECK(trashRelativePathFor(0, "a.png") == "Library/Trash/0000/a.png");
    CHECK(trashRelativePathFor(12, "") == "Library/Trash/0012");  // joinRelative's no-trailing-separator rule
}

TEST_CASE("asset actions: countRecordsUnder, and the trash's invisibility (AA45)") {
    using engine::editor::AssetRecord;
    using engine::editor::countRecordsUnder;

    SUBCASE("(a) an empty span is 0") { CHECK(countRecordsUnder({}, "textures") == 0U); }

    // records() is sorted byte-lexicographically by relativePath, which is what makes the lower_bound
    // valid; the fixture below is in that order deliberately.
    std::vector<AssetRecord> records;
    for (const char* path : {"other/z.png", "textures/a.png", "textures/b.png", "textures/sub/c.png", "z.png"}) {
        AssetRecord record;
        record.relativePath = path;
        records.push_back(record);
    }
    std::sort(records.begin(), records.end(),
              [](const AssetRecord& a, const AssetRecord& b) { return a.relativePath < b.relativePath; });

    SUBCASE("(b) a prefix matching nothing is 0") { CHECK(countRecordsUnder(records, "nothing") == 0U); }
    SUBCASE("(c) SEGMENT-WISE: \"tex\" does not count anything under \"textures/\"") {
        CHECK(countRecordsUnder(records, "tex") == 0U);
    }
    SUBCASE("(d) three records under textures, two elsewhere") { CHECK(countRecordsUnder(records, "textures") == 3U); }
    SUBCASE("(e) a nested folder counts its own, once") { CHECK(countRecordsUnder(records, "textures/sub") == 1U); }
    SUBCASE("(f) a trashed path is under ASSET_CACHE_DIR_NAME, so Library/'s EXISTING rules hide it") {
        // Asserted against the CONSTANT, never the literal "Library", so changing that constant moves
        // this claim with it -- which is the whole reason no new exclusion is added anywhere.
        const std::string trashed = engine::editor::trashRelativePathFor(1, "a.png");
        const std::string libraryPrefix = std::string(engine::editor::ASSET_CACHE_DIR_NAME) + "/";
        REQUIRE(trashed.size() > libraryPrefix.size());
        CHECK(trashed.compare(0, libraryPrefix.size(), libraryPrefix) == 0);
    }
}

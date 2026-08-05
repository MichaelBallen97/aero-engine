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

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

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

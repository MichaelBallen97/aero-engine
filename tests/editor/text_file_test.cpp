// tests/editor/text_file_test.cpp -- task 3.1.2 (plan A1): text_file.cpp's THREE PRE-EXISTING
// primitives (readTextFile/writeTextFileAtomic/fileExists) have never had a TU of their own -- they
// were exercised only incidentally through the scene and project batteries. This file gives them
// direct coverage (TF1-TF5) and adds the two new streaming primitives this task ships
// (hashFileContents/ensureDirectory, TF6-TF22).
//
// A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here. UNCONDITIONAL/UNGATED (project_test.cpp's precedent):
// text_file.hpp depends on neither reflection nor entt, so every case here must be PRESENT and
// PASSING in all three build configurations -- prove it with --list-test-cases. Tier-0: no GPU, no
// window, no ImGui context.
#include <aero/core/content_hash.hpp>
#include <aero/editor/text_file.hpp>

#include <doctest/doctest.h>

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

#if !defined(_WIN32)
    #include <unistd.h>  // geteuid -- TF15's vacuity guard
#endif

using engine::ContentHash;
using engine::editor::ensureDirectory;
using engine::editor::fileExists;
using engine::editor::FileHashResult;
using engine::editor::FileReadResult;
using engine::editor::HASH_CHUNK_BYTES;
using engine::editor::hashFileContents;
using engine::editor::readTextFile;
using engine::editor::writeTextFileAtomic;

namespace {

// The EIGHTH copy of this shape (project_files_test.cpp:41-86, asset_database_test.cpp's precedent):
// scaffolding is copied, the assertion is shared. A unique temp directory that removes itself (and
// its contents) on destruction.
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_text_file_test_" + std::to_string(++counter));
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

// A path built from UTF-8 BYTES, never from a narrow std::string (asset_database_test.cpp's pathOf
// precedent, :87-90) -- std::filesystem::path's narrow-char constructor assumes the ACTIVE CODE PAGE
// on Windows, not UTF-8.
[[nodiscard]] std::filesystem::path pathOf(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

void writeBytes(std::string_view absolutePathUtf8, std::string_view bytes) {
    std::ofstream out(pathOf(absolutePathUtf8), std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// The same deterministic pattern content_hash_test.cpp's makePattern uses: buf[i] = (i*31+7) & 0xFF --
// NOT periodic at any small stride, so it exercises the carry crossing a chunk boundary honestly.
std::string makePattern(std::size_t size) {
    std::string buf(size, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        buf[i] = static_cast<char>(static_cast<unsigned char>((i * 31U + 7U) & 0xFFU));
    }
    return buf;
}

[[nodiscard]] ContentHash hashOf(std::string_view bytes) {
    return engine::hashBytes(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())));
}

}  // namespace

// ---- retro coverage of the three pre-existing primitives (A1) ---------------------------------

TEST_CASE("text_file: readTextFile on a directory reports it, not a read error (TF1)") {
    const TempDir tmp;
    const FileReadResult result = readTextFile(tmp.utf8());
    CHECK_FALSE(result.text.has_value());
    CHECK(result.error == "path is a directory");
}

TEST_CASE("text_file: readTextFile on a missing path fails with a non-empty error (TF2)") {
    const TempDir tmp;
    const FileReadResult result = readTextFile(tmp.join("nope.txt"));
    CHECK_FALSE(result.text.has_value());
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("text_file: writeTextFileAtomic + readTextFile round-trips arbitrary bytes (TF3)") {
    const TempDir tmp;
    const std::string path = tmp.join("round.bin");
    const std::string content = std::string("before\0middle\r\nafter", 20);  // embeds a NUL and CRLF
    REQUIRE(content.size() == 20);

    const std::string writeError = writeTextFileAtomic(path, content);
    CHECK(writeError.empty());

    const FileReadResult result = readTextFile(path);
    REQUIRE(result.text.has_value());
    CHECK(result.error.empty());
    CHECK(*result.text == content);
}

TEST_CASE("text_file: writeTextFileAtomic leaves no .aero-tmp behind on success (TF4)") {
    const TempDir tmp;
    const std::string path = tmp.join("clean.txt");
    CHECK(writeTextFileAtomic(path, "hello").empty());
    CHECK_FALSE(std::filesystem::exists(pathOf(path + ".aero-tmp")));
}

TEST_CASE("text_file: fileExists is true/false (TF5)") {
    const TempDir tmp;
    const std::string path = tmp.join("maybe.txt");
    CHECK_FALSE(fileExists(path));
    writeBytes(path, "x");
    CHECK(fileExists(path));
}

// ---- hashFileContents (AC-8) --------------------------------------------------------------------

TEST_CASE("text_file: hashFileContents on a zero-byte file succeeds all-zero (TF6, AC-8, A4)") {
    const TempDir tmp;
    const std::string path = tmp.join("empty.bin");
    writeBytes(path, "");

    const FileHashResult result = hashFileContents(path);
    REQUIRE(result.hash.has_value());
    CHECK(result.error.empty());
    CHECK(result.bytesRead == 0);
    CHECK(result.hash->hi == 0);
    CHECK(result.hash->lo == 0);
    CHECK_FALSE(result.hash->valid());  // a legitimate value, not a "was this hashed?" flag (A4)
    CHECK(*result.hash == engine::hashBytes(std::span<const std::byte>{}));
}

TEST_CASE("text_file: hashFileContents on a small file matches hashBytes exactly (TF7, AC-8)") {
    const TempDir tmp;
    const std::string path = tmp.join("small.bin");
    const std::string content = "hello, aero";
    writeBytes(path, content);

    const FileHashResult result = hashFileContents(path);
    REQUIRE(result.hash.has_value());
    CHECK(result.error.empty());
    CHECK(result.bytesRead == content.size());
    CHECK(*result.hash == hashOf(content));
}

// TF8/TF9/TF10 pin that STREAMING a file agrees with hashing its bytes in one call at, one under and
// one over the read-chunk size. What they do NOT do -- and what an earlier reading of "seed S3" in
// these titles implied -- is discriminate a ContentHasher CARRY regression. HASH_CHUNK_BYTES is 1 MiB,
// a multiple of 16, so every read() but the last hands update() a whole number of blocks and leaves
// carryLength == 0; the carry top-up branch at the head of update() is therefore never entered at ANY
// file size reachable through hashFileContents. Measured: delete that whole branch and TF8-TF11 stay
// green. CH25's adversarial splits (tests/content_hash_test.cpp) are the only thing that catches it.
TEST_CASE("text_file: hashFileContents on exactly HASH_CHUNK_BYTES matches hashBytes (TF8, AC-8)") {
    const TempDir tmp;
    const std::string path = tmp.join("exact-chunk.bin");
    const std::string content = makePattern(HASH_CHUNK_BYTES);
    writeBytes(path, content);

    const FileHashResult result = hashFileContents(path);
    REQUIRE(result.hash.has_value());
    CHECK(result.bytesRead == HASH_CHUNK_BYTES);
    CHECK(*result.hash == hashOf(content));
}

TEST_CASE("text_file: hashFileContents one byte UNDER HASH_CHUNK_BYTES matches hashBytes (TF9, AC-8)") {
    const TempDir tmp;
    const std::string path = tmp.join("under-chunk.bin");
    const std::string content = makePattern(HASH_CHUNK_BYTES - 1U);
    writeBytes(path, content);

    const FileHashResult result = hashFileContents(path);
    REQUIRE(result.hash.has_value());
    CHECK(result.bytesRead == HASH_CHUNK_BYTES - 1U);
    CHECK(*result.hash == hashOf(content));
}

TEST_CASE("text_file: hashFileContents one byte OVER HASH_CHUNK_BYTES matches hashBytes (TF10, AC-8)") {
    // Two read()s: HASH_CHUNK_BYTES bytes fill exactly one, and the one extra byte forces a second,
    // short one. The carry is WRITTEN here (one leftover byte awaiting finish()) but never TOPPED UP,
    // since the first read handed update() a whole number of 16-byte blocks -- see the note above TF8.
    const TempDir tmp;
    const std::string path = tmp.join("over-chunk.bin");
    const std::string content = makePattern(HASH_CHUNK_BYTES + 1U);
    writeBytes(path, content);

    const FileHashResult result = hashFileContents(path);
    REQUIRE(result.hash.has_value());
    CHECK(result.bytesRead == HASH_CHUNK_BYTES + 1U);
    CHECK(*result.hash == hashOf(content));
}

TEST_CASE("text_file: hashFileContents over multiple chunks + tail matches hashBytes (TF11, AC-8)") {
    const TempDir tmp;
    const std::string path = tmp.join("multi-chunk.bin");
    const std::string content = makePattern(3U * HASH_CHUNK_BYTES + 5U);
    writeBytes(path, content);

    const FileHashResult result = hashFileContents(path);
    REQUIRE(result.hash.has_value());
    CHECK(result.bytesRead == 3U * HASH_CHUNK_BYTES + 5U);
    CHECK(*result.hash == hashOf(content));
}

TEST_CASE("text_file: hashFileContents on a directory fails with zero bytesRead (TF12, AC-8)") {
    const TempDir tmp;
    const FileHashResult result = hashFileContents(tmp.utf8());
    CHECK_FALSE(result.hash.has_value());
    CHECK(result.error == "path is a directory");
    CHECK(result.bytesRead == 0);
}

TEST_CASE("text_file: hashFileContents on a missing path fails (TF13, AC-8)") {
    const TempDir tmp;
    const FileHashResult result = hashFileContents(tmp.join("nope.bin"));
    CHECK_FALSE(result.hash.has_value());
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("text_file: hashFileContents on a broken symlink fails (TF14, AC-8, E31)") {
    // Guarded on create_symlink actually working -- Windows needs Developer Mode or admin privileges
    // (project_files_test.cpp's E6 precedent). A skip is recorded LOUDLY, in the test name via
    // --list-test-cases would not show it, so MESSAGE says so explicitly (V7's rule).
    const TempDir tmp;
    std::error_code ec;
    std::filesystem::create_symlink("aero-definitely-no-such-target-3.1.2", tmp.path() / "dangling.bin", ec);
    if (ec) {
        MESSAGE("skipped: this platform/filesystem refuses create_symlink (Windows needs Developer Mode)");
    } else {
        const FileHashResult result = hashFileContents(tmp.join("dangling.bin"));
        CHECK_FALSE(result.hash.has_value());
        CHECK_FALSE(result.error.empty());
    }
}

TEST_CASE("text_file: hashFileContents on an unreadable file fails (TF15, AC-8)") {
    // Guarded twice, deliberately (project_files_test.cpp's F20 precedent):
    //   * Windows -- POSIX permission semantics do not apply, and chmod is a no-op there.
    //   * root    -- root IGNORES the mode bits, so the case would PASS VACUOUSLY.
    // Assertions use CHECK, not REQUIRE: a REQUIRE throws past the permission restore below, and a
    // 0000 file left behind makes TempDir's remove_all(ec) fail silently.
#if defined(_WIN32)
    MESSAGE("skipped on Windows: POSIX permission semantics do not apply");
#else
    if (geteuid() == 0) {
        MESSAGE("skipped as root: the mode bits are ignored, so this case would pass vacuously");
    } else {
        const TempDir tmp;
        const std::string path = tmp.join("locked.bin");
        writeBytes(path, "secret");
        const std::filesystem::path fsPath = pathOf(path);

        std::error_code ec;
        std::filesystem::permissions(fsPath, std::filesystem::perms::none, std::filesystem::perm_options::replace, ec);
        REQUIRE_FALSE(ec);  // safe: nothing is unrestored yet

        const FileHashResult result = hashFileContents(path);
        CHECK_FALSE(result.hash.has_value());
        CHECK_FALSE(result.error.empty());

        std::filesystem::permissions(fsPath, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace,
                                     ec);
        CHECK_FALSE(ec);  // restored BEFORE ~TempDir, or the cleanup itself fails
    }
#endif
}

TEST_CASE("text_file: hashFileContents hashes two identical-content files identically (TF16, AC-8)") {
    const TempDir tmp;
    const std::string content = makePattern(4096);
    writeBytes(tmp.join("a.bin"), content);
    writeBytes(tmp.join("b.bin"), content);

    const FileHashResult a = hashFileContents(tmp.join("a.bin"));
    const FileHashResult b = hashFileContents(tmp.join("b.bin"));
    REQUIRE(a.hash.has_value());
    REQUIRE(b.hash.has_value());
    CHECK(*a.hash == *b.hash);
}

TEST_CASE("text_file: hashFileContents hashes the SAME bytes on every OS -- \\r\\n untouched (TF17, AC-8)") {
    // The binary-mode rule (F4, applied to the new primitive): text mode on Windows would rewrite
    // "\r\n", which would make the same file hash differently there. Writing the bytes with an
    // explicit binary ofstream (writeBytes above) keeps them literal on every OS.
    const TempDir tmp;
    const std::string content = "line one\r\nline two\r\n";
    const std::string path = tmp.join("crlf.bin");
    writeBytes(path, content);

    const FileHashResult result = hashFileContents(path);
    REQUIRE(result.hash.has_value());
    CHECK(result.bytesRead == content.size());
    CHECK(*result.hash == hashOf(content));
}

// ---- ensureDirectory (AC-9) ----------------------------------------------------------------------

TEST_CASE("text_file: ensureDirectory creates one level (TF18, AC-9)") {
    const TempDir tmp;
    const std::string path = tmp.join("one");
    CHECK_FALSE(std::filesystem::exists(pathOf(path)));
    CHECK(ensureDirectory(path).empty());
    CHECK(std::filesystem::is_directory(pathOf(path)));
}

TEST_CASE("text_file: ensureDirectory creates nested levels (TF19, AC-9)") {
    const TempDir tmp;
    const std::string path = tmp.join("a/b/c");
    CHECK(ensureDirectory(path).empty());
    CHECK(std::filesystem::is_directory(pathOf(path)));
}

TEST_CASE("text_file: ensureDirectory on an already-existing directory returns \"\" (TF20, AC-9, seed S7)") {
    const TempDir tmp;
    const std::string path = tmp.join("already");
    REQUIRE(ensureDirectory(path).empty());
    // create_directories' BOOL return is `false` with NO error_code set for an existing directory
    // (2.6.1's measured trap) -- deciding from the bool alone would misreport this as a failure.
    CHECK(ensureDirectory(path).empty());
    CHECK(std::filesystem::is_directory(pathOf(path)));
}

TEST_CASE("text_file: ensureDirectory whose parent is a FILE returns a non-empty reason (TF21, AC-9)") {
    const TempDir tmp;
    const std::string blockerPath = tmp.join("blocker");
    writeBytes(blockerPath, "not a directory");
    const std::string target = tmp.join("blocker/sub");

    CHECK_FALSE(ensureDirectory(target).empty());
    CHECK_FALSE(std::filesystem::is_directory(pathOf(target)));
}

TEST_CASE("text_file: ensureDirectory(\"\") returns a non-empty reason and creates nothing (TF22, AC-9)") {
    CHECK_FALSE(ensureDirectory("").empty());
}

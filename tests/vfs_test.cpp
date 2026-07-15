// Exercises engine::VirtualFileSystem + DirectoryBackend (task 0.2.6): path normalization, the ".."
// escape guard (the security-critical case), binary + text reads, overlay precedence/fall-through,
// and the empty/missing/directory/malformed edge cases. Uses a real temporary directory torn down by
// RAII, so a failing assertion that unwinds the test still cleans up.
#include <aero/core/vfs.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace {

// A unique temp directory that removes itself (and its contents) on destruction.
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_vfs_test_" + std::to_string(++counter));
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

    // The root as a UTF-8 string — what DirectoryBackend expects (path::string() is native-narrow /
    // ACP on Windows, so it would be wrong for non-ASCII paths).
    [[nodiscard]] std::string utf8() const {
        const std::u8string bytes = dirPath.u8string();
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    // Write a file at a UTF-8 relative path, creating parent directories.
    void write(std::string_view relativeUtf8, std::string_view contents) const {
        const std::u8string rel(reinterpret_cast<const char8_t*>(relativeUtf8.data()), relativeUtf8.size());
        const std::filesystem::path full = dirPath / std::filesystem::path(rel);
        std::error_code ec;
        std::filesystem::create_directories(full.parent_path(), ec);
        std::ofstream stream(full, std::ios::binary);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

private:
    std::filesystem::path dirPath;
};

std::string asString(const engine::ByteBuffer& bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const std::byte b : bytes) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

std::unique_ptr<engine::DirectoryBackend> backendFor(const TempDir& dir) {
    return std::make_unique<engine::DirectoryBackend>(dir.utf8());
}

}  // namespace

TEST_CASE("VFS: reads a mounted file's bytes, text, and size") {
    const TempDir dir;
    dir.write("textures/wood.png", "PNGDATA");
    engine::VirtualFileSystem vfs;
    vfs.mount(backendFor(dir));

    CHECK(vfs.exists("res://textures/wood.png"));

    const std::optional<engine::ByteBuffer> bytes = vfs.readFile("res://textures/wood.png");
    REQUIRE(bytes.has_value());
    CHECK(asString(*bytes) == "PNGDATA");

    const std::optional<std::string> text = vfs.readText("res://textures/wood.png");
    REQUIRE(text.has_value());
    CHECK(*text == "PNGDATA");

    const std::optional<std::uint64_t> size = vfs.fileSize("res://textures/wood.png");
    REQUIRE(size.has_value());
    CHECK(*size == 7);
}

TEST_CASE("VFS: a missing file is reported absent, not as an error") {
    const TempDir dir;
    engine::VirtualFileSystem vfs;
    vfs.mount(backendFor(dir));
    CHECK_FALSE(vfs.exists("res://nope.txt"));
    CHECK_FALSE(vfs.readFile("res://nope.txt").has_value());
    CHECK_FALSE(vfs.fileSize("res://nope.txt").has_value());
}

TEST_CASE("VFS: an empty file reads as an empty buffer/string and is present") {
    const TempDir dir;
    dir.write("empty.bin", "");
    engine::VirtualFileSystem vfs;
    vfs.mount(backendFor(dir));
    CHECK(vfs.exists("res://empty.bin"));

    const std::optional<engine::ByteBuffer> bytes = vfs.readFile("res://empty.bin");
    REQUIRE(bytes.has_value());
    CHECK(bytes->empty());

    const std::optional<std::string> text = vfs.readText("res://empty.bin");
    REQUIRE(text.has_value());
    CHECK(text->empty());
}

TEST_CASE("VFS: '..' cannot escape the mount root (security)") {
    const TempDir dir;
    dir.write("inside.txt", "ok");
    const std::filesystem::path secret = dir.path().parent_path() / "aero_vfs_secret.txt";
    {
        std::ofstream stream(secret, std::ios::binary);
        stream << "TOPSECRET";
    }

    engine::VirtualFileSystem vfs;
    vfs.mount(backendFor(dir));

    CHECK_FALSE(vfs.readFile("res://../aero_vfs_secret.txt").has_value());
    CHECK_FALSE(vfs.readFile("res://../../etc/passwd").has_value());
    CHECK_FALSE(vfs.exists("res://../aero_vfs_secret.txt"));
    CHECK(vfs.exists("res://inside.txt"));  // the legit file inside is still readable

    std::error_code ec;
    std::filesystem::remove(secret, ec);
}

TEST_CASE("VFS: an interior '..' that stays within the root resolves normally") {
    const TempDir dir;
    dir.write("a/keep.txt", "kept");
    engine::VirtualFileSystem vfs;
    vfs.mount(backendFor(dir));
    const std::optional<engine::ByteBuffer> bytes = vfs.readFile("res://a/sub/../keep.txt");
    REQUIRE(bytes.has_value());
    CHECK(asString(*bytes) == "kept");
}

TEST_CASE("VFS: a directory path is not a readable file") {
    const TempDir dir;
    dir.write("folder/child.txt", "x");
    engine::VirtualFileSystem vfs;
    vfs.mount(backendFor(dir));
    CHECK_FALSE(vfs.exists("res://folder"));
    CHECK_FALSE(vfs.readFile("res://folder").has_value());
    CHECK_FALSE(vfs.fileSize("res://folder").has_value());
}

TEST_CASE("VFS: malformed virtual paths are rejected") {
    using namespace std::string_view_literals;

    const TempDir dir;
    dir.write("f.txt", "x");
    engine::VirtualFileSystem vfs;
    vfs.mount(backendFor(dir));
    CHECK_FALSE(vfs.readFile("f.txt").has_value());           // no scheme
    CHECK_FALSE(vfs.readFile("file://f.txt").has_value());    // wrong scheme
    CHECK_FALSE(vfs.readFile("res://a\\b.txt").has_value());  // backslash
    CHECK_FALSE(vfs.exists("user://save.dat"));               // reserved, unimplemented
    // A literal preserves the embedded NUL; a bare C-string would truncate at it.
    CHECK_FALSE(vfs.readFile("res://a\0b"sv).has_value());  // embedded NUL
}

TEST_CASE("VFS: Windows drive-relative / colon paths are rejected") {
    const TempDir dir;
    dir.write("f.txt", "x");
    engine::VirtualFileSystem vfs;
    vfs.mount(backendFor(dir));
    CHECK_FALSE(vfs.readFile("res://C:Windows/win.ini").has_value());  // Windows drive-relative escape
    CHECK_FALSE(vfs.exists("res://C:secret"));
    CHECK_FALSE(vfs.readFile("res://a:b.txt").has_value());  // any ':' (drive / NTFS ADS)
}

#ifndef _WIN32
TEST_CASE("VFS: a colon path is rejected even when it names a real file (POSIX)") {
    const TempDir dir;
    dir.write("weird:name.txt", "nope");  // legal on POSIX, illegal on Windows
    engine::VirtualFileSystem vfs;
    vfs.mount(backendFor(dir));
    CHECK_FALSE(vfs.readFile("res://weird:name.txt").has_value());  // rejected by normalize, not read
}
#endif

TEST_CASE("VFS: mount(nullptr) is ignored; an unaddressable prefix mounts nothing") {
    const TempDir dir;
    dir.write("f.txt", "x");
    engine::VirtualFileSystem vfs;
    vfs.mount(nullptr);  // no crash, still absent
    CHECK_FALSE(vfs.exists("res://f.txt"));
    vfs.mount("bad\\prefix", backendFor(dir));  // an unaddressable prefix mounts nothing
    CHECK_FALSE(vfs.exists("res://f.txt"));
    CHECK_FALSE(vfs.exists("res://bad\\prefix/f.txt"));
}

TEST_CASE("VFS: later mounts overlay earlier ones, with fall-through") {
    const TempDir base;
    const TempDir patch;
    base.write("shared.txt", "base");
    base.write("only-base.txt", "base-only");
    patch.write("shared.txt", "patch");

    engine::VirtualFileSystem vfs;
    vfs.mount(backendFor(base));
    vfs.mount(backendFor(patch));  // mounted later -> higher priority

    const std::optional<std::string> shared = vfs.readText("res://shared.txt");
    REQUIRE(shared.has_value());
    CHECK(*shared == "patch");  // the later mount wins

    const std::optional<std::string> onlyBase = vfs.readText("res://only-base.txt");
    REQUIRE(onlyBase.has_value());
    CHECK(*onlyBase == "base-only");  // falls through to the earlier mount
}

TEST_CASE("VFS: clear() unmounts everything") {
    const TempDir dir;
    dir.write("f.txt", "x");
    engine::VirtualFileSystem vfs;
    vfs.mount(backendFor(dir));
    REQUIRE(vfs.exists("res://f.txt"));
    vfs.clear();
    CHECK_FALSE(vfs.exists("res://f.txt"));
}

TEST_CASE("VFS: with no mounts, every read is absent and nothing crashes") {
    const engine::VirtualFileSystem vfs;
    CHECK_FALSE(vfs.exists("res://anything"));
    CHECK_FALSE(vfs.readFile("res://anything").has_value());
    CHECK_FALSE(vfs.readText("res://anything").has_value());
}

TEST_CASE("VFS: a sub-prefix mount provides only its subtree") {
    const TempDir dlc;
    dlc.write("pack/hat.mesh", "HAT");
    engine::VirtualFileSystem vfs;
    vfs.mount("res://dlc", backendFor(dlc));
    const std::optional<engine::ByteBuffer> bytes = vfs.readFile("res://dlc/pack/hat.mesh");
    REQUIRE(bytes.has_value());
    CHECK(asString(*bytes) == "HAT");
    CHECK_FALSE(vfs.exists("res://pack/hat.mesh"));  // not at the root
}

TEST_CASE("VFS: a non-ASCII (UTF-8) filename round-trips") {
    const TempDir dir;
    dir.write("caf\xC3\xA9.txt", "espresso");  // "café.txt" in UTF-8
    engine::VirtualFileSystem vfs;
    vfs.mount(backendFor(dir));
    const std::optional<std::string> text = vfs.readText("res://caf\xC3\xA9.txt");
    REQUIRE(text.has_value());
    CHECK(*text == "espresso");
}

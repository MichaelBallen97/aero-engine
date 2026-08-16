// tests/editor/project_files_test.cpp — task 2.2.4: the asset browser's filesystem model, tier-0 and
// UNGATED. The third TU of aero_editor_shell_test (which supplies main() from shell_test.cpp -- do
// NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here). No GPU, no window, no ImGui context: it must
// pass identically with AERO_REQUIRE_GPU unset and set.
//
// It uses a real temporary directory torn down by RAII, so a failing assertion that unwinds still
// cleans up. The TempDir shape is COPIED from tests/vfs_test.cpp:20-60 rather than shared: keeping it
// TU-local costs ~30 lines and avoids a new header and a new target_include_directories (F27).
#include <aero/editor/project_files.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <span>  // code-review BLOCKING-3: findEntryByRelativePath
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
    #include <unistd.h>  // geteuid -- case 8b's vacuity guard
#endif

using engine::editor::DirectoryListing;
using engine::editor::FileEntry;
using engine::editor::findEntryByRelativePath;
using engine::editor::ScanStatus;
using engine::editor::TreeRow;

namespace {

// A unique temp directory that removes itself (and its contents) on destruction.
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_project_files_test_" + std::to_string(++counter));
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

    // The root as UTF-8 -- what listDirectory expects (path::string() is native-narrow / ACP on
    // Windows, so it would be wrong for non-ASCII paths).
    [[nodiscard]] std::string utf8() const {
        const std::u8string bytes = dirPath.u8string();
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    void write(std::string_view relativeUtf8, std::string_view contents) const {
        const std::u8string rel(reinterpret_cast<const char8_t*>(relativeUtf8.data()), relativeUtf8.size());
        const std::filesystem::path full = dirPath / std::filesystem::path(rel);
        std::error_code ec;
        std::filesystem::create_directories(full.parent_path(), ec);
        std::ofstream stream(full, std::ios::binary);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    void makeDir(std::string_view relativeUtf8) const {
        const std::u8string rel(reinterpret_cast<const char8_t*>(relativeUtf8.data()), relativeUtf8.size());
        std::error_code ec;
        std::filesystem::create_directories(dirPath / std::filesystem::path(rel), ec);
    }

private:
    std::filesystem::path dirPath;
};

// task 3.1.2: this file's TempDir (above) has no `join`, unlike asset_database_test.cpp's/
// text_file_test.cpp's copies -- a small local helper for the new canonicalDirectory cases below.
std::string joinPath(const TempDir& tmp, std::string_view leaf) {
    std::string result = tmp.utf8();
    result += '/';
    result += leaf;
    return result;
}

// task 3.1.2 (plan A2): DESIGNATED initializers, as defence in depth for the NEXT field addition --
// these used to be POSITIONAL, and inserting a field ahead of `isDirectory` would have silently
// re-mapped both (bool -> int64 is a promotion, not a narrowing, so nothing diagnoses it).
FileEntry dirEntry(std::string name) { return FileEntry{.name = std::move(name), .isDirectory = true}; }
FileEntry fileEntry(std::string name, std::uint64_t size = 0) {
    return FileEntry{.name = std::move(name), .size = size, .sizeKnown = true};
}

// Index a listing by name; -1 when absent. Keeps the assertions readable.
std::ptrdiff_t indexOf(const DirectoryListing& listing, std::string_view name) {
    const auto it = std::find_if(listing.entries.begin(), listing.entries.end(),
                                 [name](const FileEntry& e) { return e.name == name; });
    return it == listing.entries.end() ? -1 : std::distance(listing.entries.begin(), it);
}

// The cache shape buildVisibleTree walks: a plain map of relative path -> listing. `listingFor` must
// NEVER scan (D7), so this provider only ever looks up.
using Cache = std::map<std::string, DirectoryListing>;

std::function<const DirectoryListing*(const std::string&)> providerFor(const Cache& cache) {
    return [&cache](const std::string& rel) -> const DirectoryListing* {
        const auto it = cache.find(rel);
        return it == cache.end() ? nullptr : &it->second;
    };
}

DirectoryListing listingOf(std::vector<FileEntry> entries) {
    DirectoryListing out;
    out.entries = std::move(entries);
    std::sort(out.entries.begin(), out.entries.end(), engine::editor::entryOrderLess);
    return out;
}

std::vector<std::string> namesOf(const std::vector<FileEntry>& entries) {
    std::vector<std::string> out;
    out.reserve(entries.size());
    for (const FileEntry& e : entries) {
        out.push_back(e.name);
    }
    return out;
}

}  // namespace

TEST_CASE("editor: formatFileSize is integer, truncating and locale-free (D15)") {
    using engine::editor::formatFileSize;
    // std::array, never a C array: modernize-avoid-c-arrays is --warnings-as-errors in CI.
    // 3565158 is DELIBERATELY 0.4 bytes short of 3.4 MB (3.4 * 1048576 == 3565158.4), so it proves
    // the first decimal TRUNCATES rather than rounds -- the whole point of D15.
    static constexpr std::array<std::pair<std::uint64_t, std::string_view>, 15> CASES{{
        {0ULL, "0 B"},
        {1ULL, "1 B"},
        {842ULL, "842 B"},
        {1023ULL, "1023 B"},
        {1024ULL, "1.0 KB"},
        {1536ULL, "1.5 KB"},
        {1023ULL * 1024ULL, "1023.0 KB"},
        {1048575ULL, "1023.9 KB"},
        {1048576ULL, "1.0 MB"},
        {3565158ULL, "3.3 MB"},
        {1073741823ULL, "1023.9 MB"},
        {1073741824ULL, "1.0 GB"},
        {1099511627775ULL, "1023.9 GB"},
        {1099511627776ULL, "1.0 TB"},
        {1024ULL * 1099511627776ULL, "1024.0 TB"},  // stays TB -- total, never asserts
    }};
    for (const auto& [bytes, expected] : CASES) {
        CAPTURE(bytes);
        CHECK(formatFileSize(bytes) == std::string(expected));
    }
}

TEST_CASE("editor: joinRelative / parentOf / leafOf / depthOf / splitSegments (§3.2)") {
    using engine::editor::depthOf;
    using engine::editor::joinRelative;
    using engine::editor::leafOf;
    using engine::editor::parentOf;
    using engine::editor::splitSegments;

    CHECK(joinRelative("", "assets") == "assets");
    CHECK(joinRelative("assets", "textures") == "assets/textures");
    CHECK(joinRelative("a", "") == "a");
    CHECK(joinRelative("", "").empty());

    CHECK(parentOf("assets/textures") == "assets");
    CHECK(parentOf("assets").empty());
    CHECK(parentOf("").empty());
    CHECK(parentOf("a/b/c") == "a/b");

    CHECK(leafOf("assets/textures") == "textures");
    CHECK(leafOf("assets") == "assets");
    CHECK(leafOf("").empty());

    CHECK(depthOf("") == 0);
    CHECK(depthOf("assets") == 1);
    CHECK(depthOf("assets/textures") == 2);

    CHECK(splitSegments("").empty());
    CHECK(splitSegments("a") == std::vector<std::string>{"a"});
    CHECK(splitSegments("a/b/c") == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("editor: isHiddenName is a leading dot, nothing else (D10)") {
    using engine::editor::isHiddenName;
    CHECK(isHiddenName(".git"));
    CHECK(isHiddenName(".DS_Store"));
    CHECK(isHiddenName("..weird"));
    CHECK(isHiddenName("."));
    CHECK_FALSE(isHiddenName("a.txt"));
    CHECK_FALSE(isHiddenName("a.b"));
    CHECK_FALSE(isHiddenName(""));
    CHECK_FALSE(isHiddenName("a."));
}

TEST_CASE("editor: entryOrderLess is a strict weak ordering with a defined tie-break (D11)") {
    using engine::editor::entryOrderLess;

    // Directories precede files regardless of name.
    CHECK(entryOrderLess(dirEntry("zzz"), fileEntry("aaa")));
    CHECK_FALSE(entryOrderLess(fileEntry("aaa"), dirEntry("zzz")));

    // ASCII case folding: apple < Banana < cherry.
    CHECK(entryOrderLess(fileEntry("apple"), fileEntry("Banana")));
    CHECK(entryOrderLess(fileEntry("Banana"), fileEntry("cherry")));
    CHECK(entryOrderLess(fileEntry("apple"), fileEntry("cherry")));  // transitivity spot-check
    CHECK_FALSE(entryOrderLess(fileEntry("Banana"), fileEntry("apple")));
    CHECK_FALSE(entryOrderLess(fileEntry("cherry"), fileEntry("apple")));

    // The raw-byte tie-break makes a case-only difference DEFINED (E9): 'A' (65) < 'a' (97).
    CHECK(entryOrderLess(fileEntry("Apple"), fileEntry("apple")));
    CHECK_FALSE(entryOrderLess(fileEntry("apple"), fileEntry("Apple")));

    // Shorter prefixes sort first.
    CHECK(entryOrderLess(fileEntry("app"), fileEntry("apple")));

    const std::vector<FileEntry> sample{dirEntry("zzz"),      dirEntry("Assets"),  dirEntry("assets"),
                                        fileEntry("apple"),   fileEntry("Apple"),  fileEntry("APPLE"),
                                        fileEntry("Banana"),  fileEntry("cherry"), fileEntry("app"),
                                        fileEntry("aaa", 12), dirEntry("aaa")};
    for (const FileEntry& x : sample) {  // irreflexivity
        CHECK_FALSE(entryOrderLess(x, x));
    }
    for (const FileEntry& a : sample) {  // asymmetry
        for (const FileEntry& b : sample) {
            if (entryOrderLess(a, b)) {
                CHECK_FALSE(entryOrderLess(b, a));
            }
        }
    }
    for (const FileEntry& a : sample) {  // transitivity, exhaustively over the sample
        for (const FileEntry& b : sample) {
            for (const FileEntry& c : sample) {
                if (entryOrderLess(a, b) && entryOrderLess(b, c)) {
                    CHECK(entryOrderLess(a, c));
                }
            }
        }
    }

    // Determinism: two DIFFERENT shuffles must sort to the IDENTICAL name sequence (AC-7).
    std::vector<FileEntry> first = sample;
    std::mt19937 rngA(1234U);
    std::shuffle(first.begin(), first.end(), rngA);
    std::sort(first.begin(), first.end(), entryOrderLess);

    std::vector<FileEntry> second = sample;
    std::mt19937 rngB(98765U);
    std::shuffle(second.begin(), second.end(), rngB);
    std::sort(second.begin(), second.end(), entryOrderLess);

    CHECK(namesOf(first) == namesOf(second));
}

TEST_CASE("editor: rootDisplayName picks the last segment (§3.2)") {
    using engine::editor::rootDisplayName;
    CHECK(rootDisplayName("/a/b/MyGame") == "MyGame");
    CHECK(rootDisplayName("/a/b/MyGame/") == "MyGame");
    CHECK(rootDisplayName("/a/b/MyGame///") == "MyGame");
    CHECK(rootDisplayName("MyGame") == "MyGame");
    CHECK(rootDisplayName("/") == "/");
    CHECK(rootDisplayName("///") == "///");
    CHECK(rootDisplayName("") == "(no project)");
    CHECK(rootDisplayName("C:\\Users\\me\\MyGame") == "MyGame");
    CHECK(rootDisplayName("C:\\") == "C:");
}

TEST_CASE("editor: listDirectory lists a real tree, sorted, sized (AC-6/AC-7)") {
    const TempDir tmp;
    tmp.makeDir("assets");
    tmp.makeDir("scenes");
    tmp.write("readme.txt", "hello");
    tmp.write("Big.bin", std::string(2048, 'x'));
    tmp.write(".hidden", "x");

    const DirectoryListing listing = engine::editor::listDirectory(tmp.utf8(), "", false);
    CHECK(listing.status == ScanStatus::Ok);
    REQUIRE(listing.entries.size() == 4);
    CHECK(namesOf(listing.entries) == std::vector<std::string>{"assets", "scenes", "Big.bin", "readme.txt"});
    CHECK(listing.entries[0].isDirectory);
    CHECK_FALSE(listing.entries[0].sizeKnown);  // directories never carry a size (AC-6/E6)
    CHECK(listing.entries[1].isDirectory);

    const std::ptrdiff_t big = indexOf(listing, "Big.bin");
    REQUIRE(big >= 0);
    CHECK_FALSE(listing.entries[static_cast<std::size_t>(big)].isDirectory);
    CHECK(listing.entries[static_cast<std::size_t>(big)].sizeKnown);
    CHECK(listing.entries[static_cast<std::size_t>(big)].size == 2048);

    const std::ptrdiff_t readme = indexOf(listing, "readme.txt");
    REQUIRE(readme >= 0);
    CHECK(listing.entries[static_cast<std::size_t>(readme)].sizeKnown);
    CHECK(listing.entries[static_cast<std::size_t>(readme)].size == 5);

    CHECK(listing.skipped == 0);
    CHECK_FALSE(listing.truncated);
}

TEST_CASE("editor: listDirectory's hidden filter is not an error (D10/E11)") {
    const TempDir tmp;
    tmp.makeDir("assets");
    tmp.makeDir("scenes");
    tmp.makeDir(".git");
    tmp.write("readme.txt", "hello");
    tmp.write(".hidden", "x");

    const DirectoryListing visible = engine::editor::listDirectory(tmp.utf8(), "", false);
    CHECK(visible.status == ScanStatus::Ok);
    CHECK(indexOf(visible, ".hidden") == -1);
    CHECK(indexOf(visible, ".git") == -1);
    CHECK(visible.entries.size() == 3);
    CHECK(visible.skipped == 0);  // a FILTERED entry is not a SKIPPED entry

    const DirectoryListing all = engine::editor::listDirectory(tmp.utf8(), "", true);
    CHECK(all.status == ScanStatus::Ok);
    const std::ptrdiff_t git = indexOf(all, ".git");
    const std::ptrdiff_t hidden = indexOf(all, ".hidden");
    REQUIRE(git >= 0);
    REQUIRE(hidden >= 0);
    CHECK(all.entries.size() == 5);
    CHECK(all.skipped == 0);
    // '.' (46) folds below every letter, so .git leads the DIRECTORIES and .hidden leads the FILES.
    CHECK(all.entries[static_cast<std::size_t>(git)].isDirectory);
    CHECK(git < indexOf(all, "assets"));
    CHECK(git < indexOf(all, "scenes"));
    CHECK(hidden > indexOf(all, "scenes"));  // still after every directory
}

TEST_CASE("editor: listDirectory's status distinguishes missing, not-a-directory and empty (AC-8)") {
    const TempDir tmp;
    tmp.write("readme.txt", "hello");
    tmp.makeDir("empty");

    CHECK(engine::editor::listDirectory(tmp.utf8(), "nope", false).status == ScanStatus::Missing);
    CHECK(engine::editor::listDirectory(tmp.utf8(), "readme.txt", false).status == ScanStatus::NotADirectory);
    CHECK(engine::editor::listDirectory("", "", false).status == ScanStatus::Missing);  // E2

    const DirectoryListing empty = engine::editor::listDirectory(tmp.utf8(), "empty", false);
    CHECK(empty.status == ScanStatus::Ok);  // an EMPTY directory is Ok, NOT Missing
    CHECK(empty.entries.empty());
}

TEST_CASE("editor: listDirectory reports an unreadable directory rather than an empty one (F20)") {
    // The F20 property, and the ONLY case that discriminates sabotage S8
    // (directory_options::skip_permission_denied turns the error into an empty Ok listing).
    // Guarded twice, deliberately:
    //   * Windows -- POSIX permission semantics do not apply, and chmod is a no-op there.
    //   * root    -- root IGNORES the mode bits, so the case would PASS VACUOUSLY. That is exactly
    //                the 2.2.2-S5 failure mode this project has already paid to learn once.
    // Assertions use CHECK, not REQUIRE: a REQUIRE throws past the permission restore below, and a
    // 0000 directory left behind makes TempDir's remove_all(ec) fail silently.
#if defined(_WIN32)
    MESSAGE("skipped on Windows: POSIX permission semantics do not apply");
#else
    if (geteuid() == 0) {
        MESSAGE("skipped as root: the mode bits are ignored, so this case would pass vacuously");
    } else {
        const TempDir tmp;
        tmp.makeDir("locked");
        tmp.write("locked/inside.txt", "x");
        const std::filesystem::path locked = tmp.path() / "locked";

        std::error_code ec;
        std::filesystem::permissions(locked, std::filesystem::perms::none, std::filesystem::perm_options::replace, ec);
        REQUIRE_FALSE(ec);  // safe: nothing is unrestored yet

        const DirectoryListing listing = engine::editor::listDirectory(tmp.utf8(), "locked", false);
        CHECK(listing.status == ScanStatus::Unreadable);  // NOT ScanStatus::Ok
        CHECK(listing.entries.empty());

        std::filesystem::permissions(locked, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace,
                                     ec);
        CHECK_FALSE(ec);  // restored BEFORE ~TempDir, or the cleanup itself fails
    }
#endif
}

TEST_CASE("editor: listDirectory LISTS a broken symlink instead of skipping it (review gap 1, E6)") {
    // The spec's E6 premise was wrong: a dangling symlink never reaches file_size(), because
    // is_directory() fails FIRST ([fs.op.status] sets ec when an element of the path does not exist),
    // so the entry used to hit ++skipped and vanish from the listing. It must be LISTED as a
    // size-unknown file -- the panel then renders "—" (AC-6). A broken link you can see beats one
    // that silently disappears.
    //
    // Guarded on create_symlink actually working: Windows needs Developer Mode or admin privileges
    // for symlink creation, and some filesystems refuse it. A skip is recorded loudly rather than
    // letting the case pass vacuously (the 2.2.2-S5 lesson).
    const TempDir tmp;
    tmp.write("real.txt", "hello");
    std::error_code ec;
    std::filesystem::create_symlink("aero-definitely-no-such-target-2.2.4", tmp.path() / "dangling.txt", ec);
    if (ec) {
        MESSAGE("skipped: this platform/filesystem refuses create_symlink (Windows needs Developer Mode)");
    } else {
        const DirectoryListing listing = engine::editor::listDirectory(tmp.utf8(), "", false);
        CHECK(listing.status == ScanStatus::Ok);
        CHECK(listing.skipped == 0);  // a broken link is LISTED, not skipped
        REQUIRE(listing.entries.size() == 2);

        const std::ptrdiff_t dangling = indexOf(listing, "dangling.txt");
        REQUIRE(dangling >= 0);
        const FileEntry& broken = listing.entries[static_cast<std::size_t>(dangling)];
        CHECK_FALSE(broken.isDirectory);  // never offer to descend into it
        CHECK_FALSE(broken.sizeKnown);    // -> the panel renders "—", never a "0 B" lie
        CHECK(broken.size == 0);

        // The healthy sibling is unaffected: the fallback must not swallow real sizes.
        const std::ptrdiff_t real = indexOf(listing, "real.txt");
        REQUIRE(real >= 0);
        CHECK(listing.entries[static_cast<std::size_t>(real)].sizeKnown);
        CHECK(listing.entries[static_cast<std::size_t>(real)].size == 5);
    }
}

TEST_CASE("editor: listDirectory caps a huge directory and says so (D12/E8)") {
    // ~10 005 zero-byte files. The cap check sits at the TOP of the scan loop, so a directory holding
    // exactly MAX_ENTRIES_PER_DIRECTORY entries ends naturally with truncated == false, while MAX + 5
    // breaks on the MAX+1-th iteration with truncated == true.
    const TempDir tmp;
    const std::size_t total = engine::editor::MAX_ENTRIES_PER_DIRECTORY + 5;
    for (std::size_t i = 0; i < total; ++i) {
        tmp.write("f" + std::to_string(i), "");
    }
    const DirectoryListing listing = engine::editor::listDirectory(tmp.utf8(), "", false);
    CHECK(listing.status == ScanStatus::Ok);
    CHECK(listing.entries.size() == engine::editor::MAX_ENTRIES_PER_DIRECTORY);
    CHECK(listing.truncated);
}

TEST_CASE("editor: listDirectory caps entries EXAMINED, not just retained (review gap 2, D12)") {
    // The gap: hidden-filtered entries never grow `entries`, so MAX_ENTRIES_PER_DIRECTORY alone
    // bounded nothing for a directory of dotfiles browsed with `Show hidden` off -- the loop would
    // iterate every one of them inside a synchronous per-frame reconcile.
    // MAX_ENTRIES_EXAMINED + 5 HIDDEN files: with includeHidden=false NOTHING is retained, so the
    // retained cap can never fire and only the examined cap can stop this.
    const TempDir tmp;
    const std::size_t total = engine::editor::MAX_ENTRIES_EXAMINED + 5;
    for (std::size_t i = 0; i < total; ++i) {
        tmp.write(".h" + std::to_string(i), "");
    }

    const DirectoryListing filtered = engine::editor::listDirectory(tmp.utf8(), "", false);
    CHECK(filtered.status == ScanStatus::Ok);
    CHECK(filtered.entries.empty());                                             // every entry was hidden-filtered...
    CHECK(filtered.entries.size() < engine::editor::MAX_ENTRIES_PER_DIRECTORY);  // ...so the RETAINED
                                                                                 // cap cannot have fired
    CHECK(filtered.truncated);     // the EXAMINED cap did, and it is surfaced (D12) -- never silent
    CHECK(filtered.skipped == 0);  // a filtered entry is still not an error (case 7's rule holds)

    // And with includeHidden=true the same directory truncates on the retained cap instead, which is
    // what keeps the two bounds independent rather than one masking the other.
    const DirectoryListing all = engine::editor::listDirectory(tmp.utf8(), "", true);
    CHECK(all.status == ScanStatus::Ok);
    CHECK(all.entries.size() == engine::editor::MAX_ENTRIES_PER_DIRECTORY);
    CHECK(all.truncated);
}

TEST_CASE("editor: listDirectory resolves a nested relPath and a trailing separator (§3.3)") {
    const TempDir tmp;
    tmp.makeDir("a/b");
    tmp.write("a/b/leaf.txt", "z");

    const DirectoryListing nested = engine::editor::listDirectory(tmp.utf8(), "a/b", false);
    CHECK(nested.status == ScanStatus::Ok);
    REQUIRE(nested.entries.size() == 1);
    CHECK(nested.entries[0].name == "leaf.txt");
    CHECK(nested.entries[0].size == 1);

    const DirectoryListing trailing = engine::editor::listDirectory(tmp.utf8() + "/", "a/b", false);
    CHECK(trailing.status == ScanStatus::Ok);
    REQUIRE(trailing.entries.size() == nested.entries.size());
    for (std::size_t i = 0; i < nested.entries.size(); ++i) {
        CHECK(trailing.entries[i].name == nested.entries[i].name);
        CHECK(trailing.entries[i].size == nested.entries[i].size);
        CHECK(trailing.entries[i].isDirectory == nested.entries[i].isDirectory);
        CHECK(trailing.entries[i].sizeKnown == nested.entries[i].sizeKnown);
    }
}

TEST_CASE("editor: listDirectory round-trips a non-ASCII name byte-for-byte (F18)") {
    // U+00DF has NO canonical decomposition, so this name is immune to HFS+/APFS NFD normalisation.
    const TempDir tmp;
    tmp.write("straße.txt", "x");
    const DirectoryListing listing = engine::editor::listDirectory(tmp.utf8(), "", false);
    CHECK(listing.status == ScanStatus::Ok);
    REQUIRE(listing.entries.size() == 1);
    CHECK(listing.entries[0].name == std::string("straße.txt"));
}

TEST_CASE("editor: buildVisibleTree walks depth-first in sort order (D4)") {
    Cache cache;
    cache[""] = listingOf({dirEntry("a"), dirEntry("b"), fileEntry("file.txt", 3)});
    cache["a"] = listingOf({dirEntry("a1"), dirEntry("a2")});
    cache["a/a1"] = listingOf({dirEntry("deep")});
    cache["b"] = listingOf({});

    const std::set<std::string> openDirs{"a", "a/a1"};
    std::vector<TreeRow> out;
    engine::editor::buildVisibleTree(providerFor(cache), openDirs, out);

    REQUIRE(out.size() == 5);
    CHECK(out[0].path == "a");
    CHECK(out[0].depth == 0);
    CHECK(out[0].open);
    CHECK_FALSE(out[0].knownLeaf);
    CHECK(out[1].path == "a/a1");
    CHECK(out[1].depth == 1);
    CHECK(out[1].open);
    CHECK_FALSE(out[1].knownLeaf);
    CHECK(out[2].path == "a/a1/deep");
    CHECK(out[2].depth == 2);
    CHECK_FALSE(out[2].open);
    CHECK_FALSE(out[2].knownLeaf);  // uncached -> we cannot know it is a leaf (E12)
    CHECK(out[3].path == "a/a2");
    CHECK(out[3].depth == 1);
    CHECK_FALSE(out[3].open);
    CHECK(out[4].path == "b");
    CHECK(out[4].depth == 0);
    CHECK(out[4].knownLeaf);  // cached and subdirectory-free

    // No file ever appears, and every path is emitted exactly once.
    std::set<std::string> seen;
    for (const TreeRow& row : out) {
        CHECK(row.path.find("file.txt") == std::string::npos);
        CHECK(seen.insert(row.path).second);
    }
}

TEST_CASE("editor: buildVisibleTree stops at uncached and closed directories (E4/E12)") {
    Cache cache;
    cache[""] = listingOf({dirEntry("cached"), dirEntry("uncached")});
    cache["cached"] = listingOf({dirEntry("child")});
    // "uncached" is deliberately absent from the cache, and so is "cached/child".

    std::vector<TreeRow> out;

    // Both open, but only "cached" has a listing: it emits its child; "uncached" emits nothing below.
    engine::editor::buildVisibleTree(providerFor(cache), std::set<std::string>{"cached", "uncached"}, out);
    REQUIRE(out.size() == 3);
    CHECK(out[0].path == "cached");
    CHECK(out[0].open);
    CHECK_FALSE(out[0].knownLeaf);
    CHECK(out[1].path == "cached/child");
    CHECK(out[1].depth == 1);
    CHECK_FALSE(out[1].knownLeaf);  // uncached (E12) -- the arrow shows and opens to nothing
    CHECK(out[2].path == "uncached");
    CHECK(out[2].open);             // our open set says so...
    CHECK_FALSE(out[2].knownLeaf);  // ...but nothing is cached, so nothing is emitted below it

    // Nothing open: two rows, no children at all.
    engine::editor::buildVisibleTree(providerFor(cache), std::set<std::string>{}, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].path == "cached");
    CHECK_FALSE(out[0].open);
    CHECK(out[1].path == "uncached");
    CHECK_FALSE(out[1].open);

    // knownLeaf is true EXACTLY when the listing is cached and holds no subdirectory.
    cache["cached/child"] = listingOf({fileEntry("only-a-file.txt", 1)});
    engine::editor::buildVisibleTree(providerFor(cache), std::set<std::string>{"cached"}, out);
    REQUIRE(out.size() == 3);
    CHECK(out[1].path == "cached/child");
    CHECK(out[1].knownLeaf);
}

TEST_CASE("editor: buildVisibleTree terminates on a symlink cycle at MAX_TREE_DEPTH (D13/E7)") {
    // A provider that returns the SAME one-subdirectory listing for EVERY path -- the shape a
    // symlink cycle presents. Without the MAX_TREE_DEPTH conjunct this walk never terminates.
    const DirectoryListing cyclic = listingOf({dirEntry("sub")});
    const auto provider =
        std::function<const DirectoryListing*(const std::string&)>([&cyclic](const std::string&) { return &cyclic; });

    std::set<std::string> openDirs;
    std::string path;
    for (std::size_t i = 0; i < engine::editor::MAX_TREE_DEPTH + 8; ++i) {
        path = engine::editor::joinRelative(path, "sub");
        openDirs.insert(path);
    }

    std::vector<TreeRow> out;
    engine::editor::buildVisibleTree(provider, openDirs, out);
    CHECK(out.size() == engine::editor::MAX_TREE_DEPTH);
    REQUIRE_FALSE(out.empty());
    CHECK(out.back().depth == engine::editor::MAX_TREE_DEPTH - 1);
}

TEST_CASE("editor: listDirectory fills mtime for a real file, and mtimeKnown is true (plan A3)") {
    const TempDir tmp;
    tmp.write("file.txt", "hello");
    const DirectoryListing listing = engine::editor::listDirectory(tmp.utf8(), "", false);
    const std::ptrdiff_t idx = indexOf(listing, "file.txt");
    REQUIRE(idx >= 0);
    CHECK(listing.entries[static_cast<std::size_t>(idx)].mtimeKnown);
    CHECK(listing.entries[static_cast<std::size_t>(idx)].mtime != 0);
}

TEST_CASE("editor: listDirectory's mtime is LIVE -- it changes after a rewrite (plan A3)") {
    // Do NOT sleep past the filesystem's granularity: writing a DIFFERENT SIZE too means the case
    // cannot flake on a 1-second-granularity volume -- either the mtime or the size must differ.
    const TempDir tmp;
    tmp.write("file.txt", "hello");
    const DirectoryListing before = engine::editor::listDirectory(tmp.utf8(), "", false);
    const std::ptrdiff_t beforeIdx = indexOf(before, "file.txt");
    REQUIRE(beforeIdx >= 0);
    const std::int64_t beforeMtime = before.entries[static_cast<std::size_t>(beforeIdx)].mtime;
    const std::uint64_t beforeSize = before.entries[static_cast<std::size_t>(beforeIdx)].size;

    tmp.write("file.txt", "hello, much longer than before now");
    const DirectoryListing after = engine::editor::listDirectory(tmp.utf8(), "", false);
    const std::ptrdiff_t afterIdx = indexOf(after, "file.txt");
    REQUIRE(afterIdx >= 0);
    CHECK((after.entries[static_cast<std::size_t>(afterIdx)].mtime != beforeMtime ||
           after.entries[static_cast<std::size_t>(afterIdx)].size != beforeSize));
}

TEST_CASE(
    "editor: listDirectory's mtimeKnown is false and isSymlink is true for a dangling symlink "
    "(F3, symlink-capable hosts only)") {
    const TempDir tmp;
    std::error_code ec;
    std::filesystem::create_symlink("aero-definitely-no-such-target-3.1.2-mtime", tmp.path() / "dangling.txt", ec);
    if (ec) {
        MESSAGE("skipped: this platform/filesystem refuses create_symlink (Windows needs Developer Mode)");
    } else {
        const DirectoryListing listing = engine::editor::listDirectory(tmp.utf8(), "", false);
        const std::ptrdiff_t idx = indexOf(listing, "dangling.txt");
        REQUIRE(idx >= 0);
        const FileEntry& entry = listing.entries[static_cast<std::size_t>(idx)];
        CHECK_FALSE(entry.mtimeKnown);
        CHECK(entry.isSymlink);
    }
}

TEST_CASE(
    "editor: listDirectory's isSymlink is true for a symlinked directory, false for a real one "
    "(D9, symlink-capable hosts only)") {
    const TempDir tmp;
    tmp.makeDir("real");
    std::error_code ec;
    std::filesystem::create_directory_symlink(tmp.path() / "real", tmp.path() / "link", ec);
    if (ec) {
        MESSAGE("skipped: this platform/filesystem refuses create_directory_symlink (Windows needs Developer Mode)");
    } else {
        const DirectoryListing listing = engine::editor::listDirectory(tmp.utf8(), "", false);
        const std::ptrdiff_t realIdx = indexOf(listing, "real");
        const std::ptrdiff_t linkIdx = indexOf(listing, "link");
        REQUIRE(realIdx >= 0);
        REQUIRE(linkIdx >= 0);
        CHECK_FALSE(listing.entries[static_cast<std::size_t>(realIdx)].isSymlink);
        CHECK(listing.entries[static_cast<std::size_t>(linkIdx)].isSymlink);
    }
}

TEST_CASE("editor: listDirectory's isSymlink is false for an ordinary file (D9)") {
    const TempDir tmp;
    tmp.write("plain.txt", "x");
    const DirectoryListing listing = engine::editor::listDirectory(tmp.utf8(), "", false);
    const std::ptrdiff_t idx = indexOf(listing, "plain.txt");
    REQUIRE(idx >= 0);
    CHECK_FALSE(listing.entries[static_cast<std::size_t>(idx)].isSymlink);
}

TEST_CASE("editor: canonicalDirectory resolves the same real directory reached by two routes (D9, INV-C9)") {
    const TempDir tmp;
    tmp.makeDir("a/b");
    const std::string direct = engine::editor::canonicalDirectory(joinPath(tmp, "a/b"));
    const std::string indirect = engine::editor::canonicalDirectory(joinPath(tmp, "a/./b"));
    CHECK_FALSE(direct.empty());
    CHECK(direct == indirect);
}

TEST_CASE(
    "editor: canonicalDirectory on a symlinked directory returns the target's path "
    "(AC-31, D9, symlink-capable hosts only)") {
    const TempDir tmp;
    tmp.makeDir("real");
    std::error_code ec;
    std::filesystem::create_directory_symlink(tmp.path() / "real", tmp.path() / "link", ec);
    if (ec) {
        MESSAGE("skipped: this platform/filesystem refuses create_directory_symlink (Windows needs Developer Mode)");
    } else {
        const std::string viaLink = engine::editor::canonicalDirectory(joinPath(tmp, "link"));
        const std::string viaTarget = engine::editor::canonicalDirectory(joinPath(tmp, "real"));
        CHECK_FALSE(viaLink.empty());
        CHECK(viaLink == viaTarget);
    }
}

TEST_CASE("editor: canonicalDirectory returns \"\" for a missing path and for a file (D9)") {
    const TempDir tmp;
    tmp.write("plain.txt", "x");
    CHECK(engine::editor::canonicalDirectory(joinPath(tmp, "nope")).empty());
    CHECK(engine::editor::canonicalDirectory(joinPath(tmp, "plain.txt")).empty());
}

TEST_CASE("editor: canonicalDirectory returns \"\" for a broken symlink (D9, symlink-capable hosts only)") {
    const TempDir tmp;
    std::error_code ec;
    std::filesystem::create_symlink("aero-definitely-no-such-target-3.1.2-canon", tmp.path() / "dangling", ec);
    if (ec) {
        MESSAGE("skipped: this platform/filesystem refuses create_symlink (Windows needs Developer Mode)");
    } else {
        CHECK(engine::editor::canonicalDirectory(joinPath(tmp, "dangling")).empty());
    }
}

TEST_CASE("editor: canonicalDirectory(\"\") returns \"\" (D9)") {
    CHECK(engine::editor::canonicalDirectory("").empty());
}

TEST_CASE(
    "editor: canonicalDirectory never returns a backslash, even one that is part of a directory's OWN "
    "name (code-review finding 1, D9, INV-C9)") {
    // Code-review finding 1: canonicalDirectory's result is a DEDUP KEY, compared byte-for-byte against
    // a forward-slash-joined child path elsewhere -- it must always be in the GENERIC (forward-slash)
    // form. This machine's native separator is already '/', so it can never reproduce Windows's
    // backslash-native u8string() output directly; a backslash is nonetheless a perfectly legal POSIX
    // filename byte (only '/' and NUL are forbidden), so a directory literally NAMED with one exercises
    // the exact same normalization call inside canonicalDirectory, proving the MECHANISM rather than
    // relying on a platform this suite cannot run on.
    const TempDir tmp;
    std::error_code ec;
    std::filesystem::create_directory(tmp.path() / "a\\b", ec);
    if (ec) {
        MESSAGE("skipped: this filesystem refuses a backslash byte in a directory name");
    } else {
        const std::string result = engine::editor::canonicalDirectory(joinPath(tmp, "a\\b"));
        CHECK_FALSE(result.empty());
        CHECK(result.find('\\') == std::string::npos);
    }
}

TEST_CASE("editor: buildVisibleTree reuses `out` without leaving a survivor (D15)") {
    Cache large;
    std::vector<FileEntry> manyDirs;
    manyDirs.reserve(20);
    for (int i = 0; i < 20; ++i) {
        manyDirs.push_back(dirEntry("d" + std::to_string(i)));
    }
    large[""] = listingOf(std::move(manyDirs));

    std::vector<TreeRow> out;
    engine::editor::buildVisibleTree(providerFor(large), std::set<std::string>{}, out);
    REQUIRE(out.size() == 20);
    const std::size_t capacityAfterLarge = out.capacity();
    REQUIRE(capacityAfterLarge >= 20);

    Cache small;
    small[""] = listingOf({dirEntry("x"), dirEntry("y"), dirEntry("z")});
    engine::editor::buildVisibleTree(providerFor(small), std::set<std::string>{}, out);
    // (a) a clear()-less implementation reds here...
    CHECK(out.size() == 3);
    CHECK(out[0].path == "x");
    CHECK(out[2].path == "z");
    // (b) ...and a `out = std::vector<TreeRow>{}` / swap implementation reds here. BOTH assertions,
    // because either alone passes for the implementation the other forbids (the 2.2.2 D15 lesson).
    CHECK(out.capacity() == capacityAfterLarge);
}

// ---- findEntryByRelativePath: code-review BLOCKING-3 (the footer's leaf-name collision) ----------

TEST_CASE("editor: findEntryByRelativePath finds a member by its full relative path (code review BLOCKING-3)") {
    std::vector<FileEntry> entries(2);
    entries[0].name = "a.png";
    entries[1].name = "b.png";
    CHECK(findEntryByRelativePath(std::span<const FileEntry>(entries), "assets", "assets/b.png") == 1);
    CHECK(findEntryByRelativePath(std::span<const FileEntry>(entries), "assets", "assets/a.png") == 0);
}

TEST_CASE("editor: findEntryByRelativePath at the ROOT (currentDir == \"\") (code review BLOCKING-3)") {
    std::vector<FileEntry> entries(1);
    entries[0].name = "a.png";
    CHECK(findEntryByRelativePath(std::span<const FileEntry>(entries), "", "a.png") == 0);
}

TEST_CASE(
    "editor: findEntryByRelativePath returns entries.size() for a path outside currentDir -- NEVER "
    "a leaf-name match against a different file (code review BLOCKING-3, the actual bug)") {
    // The exact collision the finding describes: "wood.png" exists in BOTH "dirA" and "dirB". The
    // listing here is "dirA"'s own; the selection is "dirB/wood.png" -- a DIFFERENT file that merely
    // shares a leaf name. A leaf-only lookup would wrongly match index 0 and pair "dirB/wood.png"'s
    // identity with "dirA/wood.png"'s size -- this must find NOTHING instead.
    std::vector<FileEntry> entries(1);
    entries[0].name = "wood.png";
    const std::size_t result = findEntryByRelativePath(std::span<const FileEntry>(entries), "dirA", "dirB/wood.png");
    CHECK(result == entries.size());
}

TEST_CASE(
    "editor: findEntryByRelativePath returns entries.size() for a genuinely absent selection (code "
    "review BLOCKING-3, E10 preserved)") {
    std::vector<FileEntry> entries(1);
    entries[0].name = "a.png";
    const std::size_t result =
        findEntryByRelativePath(std::span<const FileEntry>(entries), "assets", "assets/vanished.png");
    CHECK(result == entries.size());
}

TEST_CASE("editor: findEntryByRelativePath over an empty span returns 0 == entries.size() (code review BLOCKING-3)") {
    const std::vector<FileEntry> entries;
    CHECK(findEntryByRelativePath(std::span<const FileEntry>(entries), "assets", "assets/a.png") == 0);
    CHECK(findEntryByRelativePath(std::span<const FileEntry>(entries), "assets", "assets/a.png") == entries.size());
}

TEST_CASE(
    "editor: findEntryByRelativePath matches a DIRECTORY entry the same way as a file (code review "
    "BLOCKING-3)") {
    std::vector<FileEntry> entries(1);
    entries[0].name = "tex";
    entries[0].isDirectory = true;
    CHECK(findEntryByRelativePath(std::span<const FileEntry>(entries), "assets", "assets/tex") == 0);
}

// ---- currentFileTimeTicks / fileTimeTicksFromMillis (task 3.1.4) ---------------------------------

TEST_CASE("editor: currentFileTimeTicks is monotone non-decreasing (PF-c1)") {
    const std::int64_t first = engine::editor::currentFileTimeTicks();
    const std::int64_t second = engine::editor::currentFileTimeTicks();
    CHECK(second >= first);  // NEVER >: a coarse clock can return the same tick twice
}

TEST_CASE("editor: fileTimeTicksFromMillis(0) is 0 (PF-c2)") { CHECK(engine::editor::fileTimeTicksFromMillis(0) == 0); }

TEST_CASE("editor: fileTimeTicksFromMillis(1000) is positive (PF-c3)") {
    CHECK(engine::editor::fileTimeTicksFromMillis(1000) > 0);
}

TEST_CASE("editor: fileTimeTicksFromMillis is not saturating (PF-c4)") {
    CHECK(engine::editor::fileTimeTicksFromMillis(2000) > engine::editor::fileTimeTicksFromMillis(1000));
}

TEST_CASE("editor: currentFileTimeTicks and FileEntry::mtime share the SAME domain (PF-c5)") {
    const TempDir tmp;
    tmp.write("file.txt", "hello");
    const DirectoryListing listing = engine::editor::listDirectory(tmp.utf8(), "", false);
    const std::ptrdiff_t idx = indexOf(listing, "file.txt");
    REQUIRE(idx >= 0);
    if (!listing.entries[static_cast<std::size_t>(idx)].mtimeKnown) {
        MESSAGE("skipped: mtime unavailable for this entry on this host");
        return;
    }
    const std::int64_t now = engine::editor::currentFileTimeTicks();
    CHECK(listing.entries[static_cast<std::size_t>(idx)].mtime <= now);
}

// ---- listingIsComplete (task 3.4.2, the code-review round's BLOCKING-2) --------------------------
//
// A listing that could not be enumerated IN FULL cannot prove a name is unused, and it says so three
// ways -- not one. `status` is the obvious signal; `truncated` and `skipped` both leave the status at
// Ok and hand back a PREFIX of the directory, which is exactly what makes them dangerous to a caller
// that is about to write a file at a name it believes to be free. Every arm below is driven from a
// DirectoryListing VALUE: the predicate is pure, so no directory of 10 001 files and no antivirus lock
// is needed to test the case that matters.

TEST_CASE("editor: a fully enumerated listing is complete (PF-c6)") {
    DirectoryListing listing;
    listing.status = ScanStatus::Ok;
    listing.entries.push_back(FileEntry{.name = "a.aeromat"});
    CHECK(engine::editor::listingIsComplete(listing));

    // An EMPTY directory is complete too -- "nothing is here" is a fact, not a failure.
    const DirectoryListing empty;
    CHECK(engine::editor::listingIsComplete(empty));
}

TEST_CASE("editor: every incompleteness signal refuses, INCLUDING the two that keep status Ok (PF-c7)") {
    // The three failing arms, each on its own, so a fix that checks only one of them reddens here.
    for (const ScanStatus status : {ScanStatus::Missing, ScanStatus::NotADirectory, ScanStatus::Unreadable}) {
        DirectoryListing bad;
        bad.status = status;
        CAPTURE(static_cast<int>(status));
        CHECK_FALSE(engine::editor::listingIsComplete(bad));
    }

    // TRUNCATED: hit MAX_ENTRIES_PER_DIRECTORY or MAX_ENTRIES_EXAMINED. Status Ok, entries a prefix.
    DirectoryListing truncated;
    truncated.status = ScanStatus::Ok;
    truncated.truncated = true;
    truncated.entries.push_back(FileEntry{.name = "a.aeromat"});
    CHECK_FALSE(engine::editor::listingIsComplete(truncated));

    // SKIPPED: an entry the OS refused to classify, or -- the case that matters -- an increment(ec)
    // failure that terminated the walk part way through (3.1.4's D5: an antivirus lock, a cloud-sync
    // pass, a permission change). Status Ok, entries a prefix.
    DirectoryListing skipped;
    skipped.status = ScanStatus::Ok;
    skipped.skipped = 1;
    skipped.entries.push_back(FileEntry{.name = "a.aeromat"});
    CHECK_FALSE(engine::editor::listingIsComplete(skipped));

    // And both at once, since nothing forbids it.
    DirectoryListing both;
    both.status = ScanStatus::Ok;
    both.truncated = true;
    both.skipped = 7;
    CHECK_FALSE(engine::editor::listingIsComplete(both));
}

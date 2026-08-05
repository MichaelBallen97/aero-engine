#pragma once
// Aero Engine — the asset browser's filesystem model (task 2.2.4). PUBLIC, ImGui-FREE and
// <filesystem>-FREE by rule: it speaks UTF-8 std::string, std::vector and std::uint64_t only, so the
// tier-0 aero_editor_shell_test can exercise every rule below with no ImGui context and no
// filesystem library on its compile line. ALL std::filesystem usage is confined to
// project_files.cpp -- vfs.hpp:14-19's hygiene rule, the same instinct. std::filesystem is stdlib,
// not third-party, so this is NOT project rule #3 and this file needs no boundary guard; the
// editor's "public headers stay ImGui-free" rule is held by FILE PLACEMENT, not enforcement (R12).
//
// READ-ONLY BY CONTRACT (D19/INV-6): nothing here creates, renames, moves, deletes or writes
// anything, and nothing here opens an output stream. Task 3.1.3 replaces the PANEL on top of this
// model and keeps these helpers; task 3.1.1 adds GUIDs/.meta beside them. Changing that must be a
// deliberate decision, not an accident. task 3.1.2's canonicalDirectory extends this rule: it
// RESOLVES a path; like everything else here it creates, renames, moves and writes nothing.
//
// NOTHING HERE LOGS. A scan runs from the panel's per-frame reconcile, so anything logged here would
// log every frame (E1). Status is RETURNED, never printed.
//
// Relative paths are '/'-separated on EVERY OS and are built only by joinRelative() from leaf names
// the OS gave us. "" means the root itself.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// Why a listing has no entries. Distinct states, because each needs a different message (AC-8).
enum class ScanStatus : std::uint8_t {
    Ok = 0,
    Missing,        // the path does not exist -- or no root is configured at all (E2)
    NotADirectory,  // it exists but is a file / device / socket
    Unreadable,     // it IS a directory and the OS refused to enumerate it (permissions, I/O)
};

struct FileEntry {
    std::string name;        // UTF-8 LEAF name. Never a path, never "." or ".." (F19).
    std::uint64_t size = 0;  // meaningful ONLY when sizeKnown && !isDirectory
    bool isDirectory = false;
    bool sizeKnown = false;  // false for directories AND for files whose size could not be read --
                             // which is why this is a flag and not a sentinel 0 (AC-6/E6). A BROKEN
                             // SYMLINK lands here too: isDirectory == false, sizeKnown == false, so it
                             // is LISTED and renders "—" rather than silently vanishing (review gap 1).
    // ---- task 3.1.2: APPENDED, never inserted. tests/editor/project_files_test.cpp:88-89 hold two
    // POSITIONAL aggregate initializers; inserting a field ahead of `isDirectory` re-maps them
    // silently (bool -> int64 is a promotion, not a narrowing, so nothing diagnoses it). Both helpers
    // were converted to designated initializers in the same commit as defence in depth for the NEXT
    // field addition.
    std::int64_t mtime = 0;   // OPAQUE file_time_type ticks (docs/09 §6.5). Never a date.
    bool mtimeKnown = false;  // false for a broken symlink and any entry the OS refused (F3)
    bool isSymlink = false;   // symlink_status, NOT the followed status (D9)
};

struct DirectoryListing {
    std::vector<FileEntry> entries;  // sorted by entryOrderLess (D11)
    ScanStatus status = ScanStatus::Ok;
    std::uint32_t skipped = 0;  // entries the OS refused to classify; the rest are still listed (E5)
    bool truncated = false;     // hit MAX_ENTRIES_PER_DIRECTORY -- surfaced, never silent (D12/E8)
};

// One row of the left-hand directory tree, produced by buildVisibleTree.
struct TreeRow {
    std::string path;        // relative to the root, '/'-separated; never empty
    std::size_t depth = 0;   // 0 == a direct child of the root
    bool open = false;       // this directory's children are expanded below it
    bool knownLeaf = false;  // its listing is CACHED and holds no subdirectory -> draw no arrow.
                             // Before a directory's first scan we cannot know without one opendir
                             // per sibling, so the arrow is shown and opens to nothing (E12).
};

inline constexpr std::size_t MAX_ENTRIES_PER_DIRECTORY = 10000;  // D12 -- entries RETAINED
inline constexpr std::size_t MAX_TREE_DEPTH = 32;                // D12/D13 -- bounds a symlink cycle
// D12's second bound (review gap 2). MAX_ENTRIES_PER_DIRECTORY caps what a listing KEEPS, and hidden-
// filtered and skipped entries never grow it -- so a directory of 500 000 dotfiles browsed with
// `Show hidden` off would iterate all 500 000 inside a synchronous per-frame reconcile, which is
// exactly the "never blocks on a large tree" requirement failing. This caps entries EXAMINED and sets
// `truncated` the same way, so the bound is surfaced and never silent.
// STRICTLY GREATER than MAX_ENTRIES_PER_DIRECTORY on purpose: a directory that legitimately holds
// 10 000 visible entries alongside 10 000 hidden ones still lists in full, and the retained cap stays
// the bound that fires for an ordinary huge directory. 2x, not more: the scan runs inside a frame.
inline constexpr std::size_t MAX_ENTRIES_EXAMINED = 2 * MAX_ENTRIES_PER_DIRECTORY;

// ---- pure helpers (no I/O; every one of them is a tier-0 test case) ----

// Hidden == a leading '.', which is what removes .git / .DS_Store / .vscode (D10). "" is not hidden.
// No Windows FILE_ATTRIBUTE_HIDDEN check: that needs a platform call in editor code for a rule the
// dot-prefix already covers on the files that matter.
[[nodiscard]] bool isHiddenName(std::string_view name) noexcept;

// The D11 ordering. A STRICT WEAK ORDERING BY CONSTRUCTION: the lexicographic composition of
// (isDirectory desc), (ASCII-case-folded name), (raw name) -- each a total preorder. That is
// std::sort's precondition, not a nicety. The raw-byte tie-break is what makes the order DEFINED for
// names differing only in case (E9), and therefore assertable in a test.
[[nodiscard]] bool entryOrderLess(const FileEntry& a, const FileEntry& b) noexcept;

// ("", "assets") -> "assets";  ("assets", "textures") -> "assets/textures". An empty `name` returns
// `parentRel` unchanged -- joining nothing must not produce a trailing separator.
[[nodiscard]] std::string joinRelative(std::string_view parentRel, std::string_view name);

// "assets/textures" -> "assets";  "assets" -> "";  "" -> "".
[[nodiscard]] std::string parentOf(std::string_view rel);

// "assets/textures" -> "textures";  "assets" -> "assets";  "" -> "".
// POINTS INTO `rel`: never call it on a temporary whose lifetime ends before the result is used.
[[nodiscard]] std::string_view leafOf(std::string_view rel) noexcept;

// "" -> 0; "assets" -> 1; "assets/textures" -> 2.
[[nodiscard]] std::size_t depthOf(std::string_view rel) noexcept;

// The '/'-separated segments of `rel`, in order, for the breadcrumb. "" -> empty vector.
[[nodiscard]] std::vector<std::string> splitSegments(std::string_view rel);

// Integer-only, locale-free, TRUNCATING (never rounding): 0 -> "0 B", 842 -> "842 B",
// 1023 -> "1023 B", 1024 -> "1.0 KB", 1536 -> "1.5 KB", 1048575 -> "1023.9 KB", 1048576 -> "1.0 MB"
// (D15). Units B/KB/MB/GB/TB, binary (1024) multiples; B carries no decimal. No floating point, no
// locale, no std::format -- which is what makes the boundary values assertable IDENTICALLY on three
// OSes. Values >= 1024 TB keep the TB unit and simply grow the whole part (total, never asserts).
[[nodiscard]] std::string formatFileSize(std::uint64_t bytes);

// The display label for the root itself: its last non-empty path segment, ignoring trailing
// separators ("/Users/me/MyGame/" -> "MyGame"). Falls back to the whole string when it has no
// segment ("/" -> "/"), and to "(no project)" when empty. THIS is the one helper that sees an
// OS-NATIVE path, so it treats BOTH '/' and '\\' as separators; every other helper above sees only
// our own '/'-separated relative paths.
[[nodiscard]] std::string rootDisplayName(std::string_view rootUtf8);

// ---- the visible-tree builder: PURE over the cache, no I/O, NO RECURSION (D4/F23) ----
//
// `listingFor` returns the CACHED listing for a relative path ("" == the root), or nullptr when that
// directory has not been scanned yet -- IT MUST NOT SCAN (D7). Scanning inside it would mutate the
// cache while this function holds a `const FileEntry&` cursor into it.
// Emits, depth-first in entryOrderLess order, one row per directory reachable from the root through
// `openDirs`, and nothing deeper than MAX_TREE_DEPTH. Files never appear.
// `out` is CLEARED first and its CAPACITY IS REUSED across calls (never reassigned or swapped).
void buildVisibleTree(const std::function<const DirectoryListing*(const std::string&)>& listingFor,
                      const std::set<std::string>& openDirs, std::vector<TreeRow>& out);

// ---- the two functions that actually touch the disk (project_files.cpp) ----

// Enumerate `rootUtf8 / relPath`. NEVER THROWS -- every call site uses the std::error_code overload
// (E20); returns a listing whose `status` says what happened. `relPath` is '/'-separated and
// root-relative; "" means the root itself. An EMPTY `rootUtf8` is ScanStatus::Missing (E2).
[[nodiscard]] DirectoryListing listDirectory(std::string_view rootUtf8, std::string_view relPath, bool includeHidden);

// task 3.1.2 (D9): the PHYSICAL path of a directory, symlinks resolved. "" on ANY failure -- a caller
// that cannot prove two paths are distinct must refuse to descend, never guess. READ-ONLY, like
// everything else here: it resolves; it does not create, move or normalize anything on disk.
// Deliberately NOT used to record or display a path anywhere -- project roots are `absolute`, not
// `weakly_canonical`, ON PURPOSE (a project reached through a symlink must not be silently recorded
// under its target). THIS VALUE IS A DEDUP KEY AND NOTHING ELSE (INV-C9).
[[nodiscard]] std::string canonicalDirectory(std::string_view absolutePathUtf8);

}  // namespace engine::editor

// Aero Engine — the asset browser's filesystem model (task 2.2.4). THE ONLY TU under /editor that
// includes <filesystem>, and it makes no ImGui call and includes no ImGui header (INV-2, both
// grep-asserted in §V6). READ-ONLY (D19/INV-6): no create_directory, no rename, no remove, no copy,
// no output stream anywhere below. NO RECURSION anywhere (INV-4/F23). NO LOGGING (E1).
#include <aero/editor/project_files.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// Mirrors engine/core/src/vfs.cpp:91-97 (whose comment carries the full rationale). Kept TU-LOCAL
// rather than promoted to core: the only sane home there would have to expose
// std::filesystem::path from a PUBLIC core header, which vfs.hpp:14-19 deliberately refuses to do.
// Construct the path from UTF-8 BYTES so non-ASCII names resolve correctly on Windows, where path's
// native encoding is UTF-16 and the narrow-char constructor assumes the active code page (NOT UTF-8).
std::filesystem::path pathFromUtf8(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

// path::u8string(), NEVER path::string(): the latter is native-narrow / ACP on Windows, so it would
// mangle every non-ASCII name (F18; tests/vfs_test.cpp:41-46 says the same thing). Sabotage S7.
std::string utf8FromPath(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// ASCII-only, locale-independent. NEVER std::tolower(char): UTF-8 continuation bytes are NEGATIVE as
// char on every target, which is UB and trips bugprone-signed-char-misuse, --warnings-as-errors in
// CI (F22; it already cost task 2.2.2 a fix).
constexpr unsigned char foldAscii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

// -1 / 0 / +1, comparing ASCII-case-folded bytes, then length. Total preorder -> safe as a
// std::sort key component (D11).
int compareFolded(std::string_view a, std::string_view b) noexcept {
    const std::size_t shared = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < shared; ++i) {
        const unsigned char ca = foldAscii(static_cast<unsigned char>(a[i]));
        const unsigned char cb = foldAscii(static_cast<unsigned char>(b[i]));
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
    }
    if (a.size() == b.size()) {
        return 0;
    }
    return a.size() < b.size() ? -1 : 1;
}

// rootDisplayName is the ONE helper that sees an OS-native path (a project's root, task 2.6.1's
// ProjectSession::root(), which is backslash-separated on Windows). Everything else here sees only
// our own '/'-separated relative paths.
constexpr bool isPathSeparator(char c) noexcept { return c == '/' || c == '\\'; }

bool hasSubdirectory(const DirectoryListing& listing) noexcept {
    // A linear scan, deliberately NOT `entries.front().isDirectory`: buildVisibleTree is PURE over
    // whatever the caller's cache holds, and must not silently depend on the sort having run.
    return std::any_of(listing.entries.begin(), listing.entries.end(),
                       [](const FileEntry& e) noexcept { return e.isDirectory; });
}

}  // namespace

bool isHiddenName(std::string_view name) noexcept { return !name.empty() && name.front() == '.'; }

bool entryOrderLess(const FileEntry& a, const FileEntry& b) noexcept {
    if (a.isDirectory != b.isDirectory) {
        return a.isDirectory;  // directories first
    }
    const int folded = compareFolded(a.name, b.name);
    if (folded != 0) {
        return folded < 0;
    }
    return a.name < b.name;  // raw-byte tie-break: makes case-only differences DETERMINISTIC (E9)
}

std::string joinRelative(std::string_view parentRel, std::string_view name) {
    if (name.empty()) {
        return std::string(parentRel);
    }
    if (parentRel.empty()) {
        return std::string(name);
    }
    std::string out;
    out.reserve(parentRel.size() + 1U + name.size());
    out.append(parentRel);
    out.push_back('/');
    out.append(name);
    return out;
}

std::string parentOf(std::string_view rel) {
    const std::size_t slash = rel.rfind('/');
    if (slash == std::string_view::npos) {
        return {};
    }
    return std::string(rel.substr(0, slash));
}

std::string_view leafOf(std::string_view rel) noexcept {
    const std::size_t slash = rel.rfind('/');
    if (slash == std::string_view::npos) {
        return rel;
    }
    // NOT substr(): it is specified to throw std::out_of_range, which would escape this noexcept
    // function (bugprone-exception-escape, --warnings-as-errors in CI). The pointer+size constructor
    // IS noexcept, and `slash + 1 <= rel.size()` holds by construction since rfind found a character.
    return std::string_view(rel.data() + slash + 1U, rel.size() - slash - 1U);
}

std::size_t depthOf(std::string_view rel) noexcept {
    if (rel.empty()) {
        return 0;
    }
    std::size_t count = 1;
    for (const char c : rel) {
        if (c == '/') {
            ++count;
        }
    }
    return count;
}

std::vector<std::string> splitSegments(std::string_view rel) {
    std::vector<std::string> out;
    std::size_t cursor = 0;
    while (cursor < rel.size()) {
        const std::size_t slash = rel.find('/', cursor);
        const std::size_t end = (slash == std::string_view::npos) ? rel.size() : slash;
        out.emplace_back(rel.substr(cursor, end - cursor));
        if (slash == std::string_view::npos) {
            break;
        }
        cursor = slash + 1U;
    }
    return out;
}

std::string formatFileSize(std::uint64_t bytes) {
    constexpr std::uint64_t UNIT = 1024;
    if (bytes < UNIT) {
        return std::to_string(bytes) + " B";  // B carries no decimal
    }
    // KB / MB / GB / TB. Everything >= 1024 TB stays in TB and simply grows the whole part.
    // std::array, not a C array: modernize-avoid-c-arrays is --warnings-as-errors in CI.
    static constexpr std::array<std::string_view, 4> SUFFIXES{" KB", " MB", " GB", " TB"};
    std::size_t unitIndex = 0;
    std::uint64_t divisor = UNIT;
    while (unitIndex + 1U < SUFFIXES.size() && bytes / divisor >= UNIT) {
        divisor *= UNIT;
        ++unitIndex;
    }
    const std::uint64_t whole = bytes / divisor;
    // TRUNCATING first decimal, integer-only. (bytes % divisor) < divisor <= 1024^4 == 2^40, so the
    // * 10 cannot overflow a uint64 (worst case ~2^43.3). No floating point, no locale (D15).
    const std::uint64_t tenths = ((bytes % divisor) * 10U) / divisor;
    return std::to_string(whole) + "." + std::to_string(tenths) + std::string(SUFFIXES[unitIndex]);
}

std::string rootDisplayName(std::string_view rootUtf8) {
    if (rootUtf8.empty()) {
        return "(no project)";
    }
    std::size_t end = rootUtf8.size();
    while (end > 0 && isPathSeparator(rootUtf8[end - 1U])) {
        --end;
    }
    if (end == 0) {
        return std::string(rootUtf8);  // "/" or "///" -- no segment; show it as-is
    }
    std::size_t begin = end;
    while (begin > 0 && !isPathSeparator(rootUtf8[begin - 1U])) {
        --begin;
    }
    return std::string(rootUtf8.substr(begin, end - begin));
}

void buildVisibleTree(const std::function<const DirectoryListing*(const std::string&)>& listingFor,
                      const std::set<std::string>& openDirs, std::vector<TreeRow>& out) {
    // clear(), never `out = {}` or a swap: the CAPACITY must survive across calls (D15's habit;
    // tier-0 case 15 asserts BOTH size correctness and capacity reuse, because either assertion
    // alone passes for the implementation the other forbids -- the 2.2.2 D15 review lesson).
    out.clear();

    struct Frame {
        std::string rel;
        std::size_t next = 0;
        std::size_t depth = 0;
    };
    std::vector<Frame> stack;  // EXPLICIT stack -- misc-no-recursion is --warnings-as-errors (F23)
    stack.push_back(Frame{std::string{}, 0, 0});

    while (!stack.empty()) {
        Frame& frame = stack.back();  // re-taken EVERY iteration; NEVER held across a push_back
        const DirectoryListing* const listing = listingFor(frame.rel);
        if (listing == nullptr || frame.next >= listing->entries.size()) {
            stack.pop_back();
            continue;  // not scanned yet -> emits itself and no children (E4/case 13)
        }
        // `entry` points INTO the caller's cache. That is safe ONLY because listingFor must not scan
        // (D7): a scan would insert into the cache and could relocate this vector's storage.
        const FileEntry& entry = listing->entries[frame.next];
        ++frame.next;
        if (!entry.isDirectory) {
            continue;  // deliberately sort-order independent: do not `break` on the first file
        }
        std::string childRel = joinRelative(frame.rel, entry.name);
        const std::size_t childDepth = frame.depth;  // COPIED OUT before any push_back below
        const bool open = childDepth + 1U < MAX_TREE_DEPTH && openDirs.contains(childRel);
        const DirectoryListing* const childListing = listingFor(childRel);
        const bool knownLeaf = childListing != nullptr && !hasSubdirectory(*childListing);
        out.push_back(TreeRow{childRel, childDepth, open, knownLeaf});
        if (open) {
            // INVALIDATES `frame` -- nothing below this line may touch it, and the loop re-takes it.
            stack.push_back(Frame{std::move(childRel), 0, childDepth + 1U});
        }
    }
}

DirectoryListing listDirectory(std::string_view rootUtf8, std::string_view relPath, bool includeHidden) {
    DirectoryListing out;
    if (rootUtf8.empty()) {
        out.status = ScanStatus::Missing;  // E2: no root was given, or no project is open
        return out;
    }

    std::filesystem::path dir = pathFromUtf8(rootUtf8);
    if (!relPath.empty()) {
        // operator/ join: a root with a trailing separator needs no normalisation (case 10).
        dir /= pathFromUtf8(relPath);
    }

    std::error_code ec;
    const std::filesystem::file_status st = std::filesystem::status(dir, ec);
    if (ec || !std::filesystem::exists(st)) {
        out.status = ScanStatus::Missing;
        return out;
    }
    if (!std::filesystem::is_directory(st)) {
        out.status = ScanStatus::NotADirectory;
        return out;
    }

    // directory_options::none, NOT skip_permission_denied (F20): that flag turns the error we want to
    // REPORT into an indistinguishable empty Ok listing. This is exactly what case 8b asserts and
    // sabotage S8 targets.
    std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::none, ec);
    if (ec) {
        out.status = ScanStatus::Unreadable;
        return out;
    }

    const std::filesystem::directory_iterator last;
    // TWO bounds, both surfaced by the footer and never silent (D12/E8). The first caps what the
    // listing RETAINS; the second caps what the loop EXAMINES, because hidden-filtered and skipped
    // entries never grow `entries` and would otherwise let one directory spin unbounded inside a
    // synchronous per-frame reconcile (review gap 2). Both checks sit at the TOP of the body, so a
    // directory holding EXACTLY either cap ends naturally with truncated == false, while cap + 1
    // breaks with truncated == true.
    std::size_t examined = 0;
    while (it != last) {
        if (out.entries.size() >= MAX_ENTRIES_PER_DIRECTORY || examined >= MAX_ENTRIES_EXAMINED) {
            out.truncated = true;
            break;
        }
        ++examined;
        const std::filesystem::directory_entry& entry = *it;

        std::string name = utf8FromPath(entry.path().filename());
        if (name.empty()) {
            ++out.skipped;
        } else if (!includeHidden && isHiddenName(name)) {
            // A FILTERED entry is not an error -- it must NOT count as skipped (case 7).
        } else {
            std::error_code entryEc;
            // is_directory FOLLOWS symlinks (D13), so a symlinked asset folder behaves like a real
            // one; MAX_TREE_DEPTH is what makes a cycle safe. directory_entry caches the attributes
            // populated during iteration (F21), so this is typically free.
            const bool isDir = entry.is_directory(entryEc);
            if (entryEc) {
                // The FOLLOWED status failed. E6 CORRECTED (review gap 1): the spec assumed a broken
                // symlink reached file_size() and rendered "—". It never does -- is_directory() fails
                // FIRST, because [fs.op.status] sets ec when an element of the path does not exist,
                // so the dangling link used to hit ++skipped and VANISH from the listing entirely.
                // Measured on this lane: a dangling symlink gives is_directory ec=ENOENT, file_size
                // ec=ENOENT, but symlink_status ec=0 type=symlink. That asymmetry IS the discriminator:
                // symlink_status does NOT follow the link, so it answers for the directory entry
                // itself. Succeeding there means the entry really is on disk and we simply cannot
                // classify what it points AT -> list it as a size-unknown FILE, which renders "—"
                // (AC-6). In an asset browser a broken link you can SEE beats one that silently
                // disappears. Failing there means the entry itself is gone or unreachable (a race, a
                // broken mount) -> ++skipped, and the footer says how many (E5).
                std::error_code linkEc;
                const std::filesystem::file_status linkStatus = entry.symlink_status(linkEc);
                if (linkEc || !std::filesystem::exists(linkStatus)) {
                    ++out.skipped;
                } else {
                    FileEntry brokenEntry;
                    brokenEntry.name = std::move(name);
                    // isDirectory and sizeKnown both stay false -- deliberately NOT a "0 B" lie, and
                    // deliberately not a directory (the tree pane must not offer to descend into it).
                    out.entries.push_back(std::move(brokenEntry));
                }
            } else {
                FileEntry fileEntry;
                fileEntry.name = std::move(name);
                fileEntry.isDirectory = isDir;
                if (!isDir) {
                    std::error_code sizeEc;
                    const std::uintmax_t bytes = entry.file_size(sizeEc);
                    if (!sizeEc) {
                        fileEntry.size = static_cast<std::uint64_t>(bytes);
                        fileEntry.sizeKnown = true;
                    }
                    // on failure both fields stay at their defaults -> the panel renders "—" (AC-6).
                    // NOT a "0 B" lie. Reachable for a file whose size the OS refuses even though its
                    // type resolved; the broken-symlink path is the entryEc branch above.
                }
                out.entries.push_back(std::move(fileEntry));
            }
        }

        // The EXPLICIT increment(ec) form: an enumeration failure mid-directory must TERMINATE the
        // loop, not spin on a stuck iterator (E19). The entries already read stay valid and listed.
        it.increment(ec);
        if (ec) {
            ++out.skipped;
            break;
        }
    }

    std::sort(out.entries.begin(), out.entries.end(), entryOrderLess);  // D11/F19
    return out;
}

}  // namespace engine::editor

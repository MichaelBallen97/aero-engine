#pragma once
// Aero Engine — the editor's text-file bytes (task 2.6.1, D12; the declarations promoted verbatim
// from scene_session.hpp, where they landed at task 2.5.1). PUBLIC, and free of ImGui, SDL, entt and
// <filesystem> -- ALL of the latter lives in text_file.cpp, exactly as it did in scene_file.cpp.
// Held by FILE PLACEMENT (R12), like every other header under editor/include.
//
// Promoted because the project writer needs all three and the atomic write is subtle enough (binary
// on BOTH sides, a SCOPED stream, `.aero-tmp` + rename, Windows's refusal to replace an open file)
// that a second hand-rolled copy in this tree would be a defect waiting to happen.
//
// task 3.1.2 adds hashFileContents and ensureDirectory here rather than a new file: both are the same
// class of primitive (byte-level, error_code-driven, never throws, never logs) that this TU already
// owns.
#include <aero/core/content_hash.hpp>  // FileHashResult::hash

#include <cstddef>  // HASH_CHUNK_BYTES
#include <cstdint>  // FileHashResult::bytesRead -- NOT transitive on libstdc++
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

struct FileReadResult {
    std::optional<std::string> text;  // engaged == success
    std::string error;                // OS reason; empty iff `text` is engaged
};
[[nodiscard]] FileReadResult readTextFile(std::string_view absolutePathUtf8);

// "" == success. ATOMIC: writes <path>.aero-tmp, CLOSES it, renames over `path`.
[[nodiscard]] std::string writeTextFileAtomic(std::string_view absolutePathUtf8, std::string_view text);
[[nodiscard]] bool fileExists(std::string_view absolutePathUtf8);

inline constexpr std::size_t HASH_CHUNK_BYTES = static_cast<std::size_t>(1024U) * 1024U;

struct FileHashResult {
    std::optional<ContentHash> hash;  // engaged == success. NOTE: a zero-byte file SUCCEEDS with the
                                      // ALL-ZERO digest (plan A4) -- engagement, never hash->valid(),
                                      // is what says "this was read".
    std::string error;                // OS reason; empty iff `hash` is engaged
    std::uint64_t bytesRead = 0;      // exact; what the scan's budget accounting consumes
};
// STREAMING, binary on BOTH sides, never throws, never logs. A 2 GB file is read in HASH_CHUNK_BYTES
// chunks and never exists in memory -- which is exactly why this is NOT readTextFile + hashBytes:
// readTextFile's istreambuf_iterator construction would materialise the whole file as a std::string.
[[nodiscard]] FileHashResult hashFileContents(std::string_view absolutePathUtf8);

// "" == success, INCLUDING when the directory already exists. Decided from the error_code and an
// is_directory check, NEVER from create_directories' bool return -- which is `false` with NO ec set
// for an existing directory (2.6.1's measured trap, its sabotage seed S22).
[[nodiscard]] std::string ensureDirectory(std::string_view absolutePathUtf8);

// task 3.1.3 (§D-4): the byte-level, capped, binary, never-throws-never-logs primitive the thumbnail
// loader needs. Refuses a file larger than `maxBytes` from std::filesystem::file_size ALONE -- it
// NEVER OPENS such a file. That is the whole difference from readTextFile above (which has no cap
// and materialises everything through an istreambuf_iterator). Binary on BOTH sides, for the same
// load-bearing reason text_file.cpp:57-60 states for readTextFile.
struct FileBytesResult {
    std::optional<std::string> bytes;  // engaged == success. std::string is a BYTE container here.
    std::string error;                 // OS reason, or "file is too large"; "" iff `bytes` engaged
    std::uint64_t size = 0;            // the observed size, filled EVEN WHEN REFUSED, so the caller
                                       // can report the number that tripped the cap (seed S33)
    // code-review finding 6: the DISCRIMINATED signal for "refused by the maxBytes cap specifically",
    // never a string comparison against `error`'s text. `thumbnail_store.cpp` used to test
    // `fileResult.error == "file is too large"` against a literal owned by THIS TU -- a message wording
    // change there would silently turn Skipped into Failed with no test able to catch it (TF26 only
    // asserted `error` was non-empty, never its exact text). Pin the outcome here instead, on a bool.
    bool refusedByCap = false;
};
// A directory, a missing file and an unreadable file each return a disengaged `bytes` with the OS
// reason, exactly like readTextFile.
[[nodiscard]] FileBytesResult readFileBytes(std::string_view absolutePathUtf8, std::uint64_t maxBytes);

// task 3.2.4. TRUE iff the path names an EXISTING REGULAR FILE (never a directory -- E3) and, when
// requireExecBit is true, at least one of owner/group/other execute permission is set.
//
// `requireExecBit` is FALSE on Windows, where an execute permission bit does not exist -- THE CALLER
// decides from HostOs, so THIS TU stays free of the preprocessor (AC-5/INV-B6). That is the whole
// reason it is a parameter rather than an internal branch.
//
// Decided from std::filesystem::status()'s error_code, NEVER from an exception and never from
// is_regular_file()'s throwing overload: a permission-denied parent directory must return false, not
// terminate (2.6.1's ensureDirectory lesson, a second application). NEVER THROWS. NEVER LOGS.
//
// A symlink is FOLLOWED deliberately (status, not symlink_status): the commonest macOS install is
// reached through a wrapper on PATH that is itself a symlink, and refusing symlinks would refuse it.
[[nodiscard]] bool isExecutableFile(std::string_view absolutePathUtf8, bool requireExecBit);

}  // namespace engine::editor

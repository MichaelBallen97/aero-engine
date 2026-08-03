#pragma once
// Aero Engine — AssetDatabase: the scan over a project's assets root (task 3.1.1). PUBLIC, and free
// of ImGui, SDL, entt and every build gate (D4, project.hpp's precedent). Still `<filesystem>`-free
// by file placement -- all disk access lives in asset_database.cpp, which composes 2.2.4's
// listDirectory and 2.5.1/2.6.1's text_file rather than touching std::filesystem directly.
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/project_files.hpp>  // ScanStatus -- reused, never redeclared

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace engine::editor {

struct AssetScanReport {
    ScanStatus status = ScanStatus::Ok;  // the ROOT's status; Missing when no project is open
    std::size_t filesSeen = 0;
    std::size_t created = 0;
    std::size_t repaired = 0;
    std::size_t invalid = 0;
    std::vector<std::string> invalidPaths;  // capped; "<path>" or "<path>: <reason>"
    std::vector<std::string> orphans;       // capped
    std::size_t orphanTotal = 0;
    std::vector<std::string> repairs;        // A9. capped; "<path>: <old guid> -> <new guid>"
    std::vector<std::string> writeFailures;  // capped; "<path>: <os reason>"
    std::size_t writeFailureTotal = 0;
    std::vector<std::string> unknownKeyWarnings;  // capped
    std::size_t unknownKeyTotal = 0;              // A9
    std::size_t skippedEntries = 0;               // A10 -- listDirectory::skipped, summed
    std::size_t unreadableDirs = 0;               // A10 -- a non-Ok listing BELOW the root
    bool truncated = false;                       // MAX_ASSETS or a listDirectory cap
    bool depthLimited = false;                    // MAX_TREE_DEPTH
    bool largeCreateNotice = false;               // A6 -- writeIndices > CREATE_NOTICE_THRESHOLD
};

// C++20 heterogeneous lookup (P0919R3): WITHOUT `is_transparent`, find(std::string_view) either
// fails to compile or silently constructs a std::string -- which ALLOCATES, and an allocation
// inside a noexcept function is a std::terminate path. Both accessors below are noexcept, so this
// is load-bearing, not a micro-optimisation (plan A1).
struct PathHash {
    // is_transparent's spelling is fixed by the standard library's heterogeneous-lookup protocol
    // (P0919R3) -- it cannot be renamed to fit this tree's naming convention.
    // NOLINTNEXTLINE(readability-identifier-naming)
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view path) const noexcept {
        return std::hash<std::string_view>{}(path);
    }
};

class AssetDatabase {
public:
    [[nodiscard]] const std::string& root() const noexcept;

    // The ONE mutating entry point. Never throws, never logs (INV-A3). An empty root clears the
    // database and returns ScanStatus::Missing with zero writes.
    AssetScanReport rescan(std::string newRootUtf8, GuidGenerator& generator);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const AssetRecord* findByPath(std::string_view relativePath) const noexcept;
    [[nodiscard]] const AssetRecord* findByGuid(Guid guid) const noexcept;  // nullptr for nil
    [[nodiscard]] std::optional<Guid> guidForPath(std::string_view relativePath) const noexcept;

private:
    std::string rootUtf8;
    std::vector<AssetRecord> records;  // sorted by relativePath
    std::unordered_map<std::string, std::size_t, PathHash, std::equal_to<>> byPath;
    std::unordered_map<Guid, std::size_t> byGuid;  // Invalid records ABSENT (INV-A7)
};

// F10 (editor_app.hpp): EditorApp's move is `noexcept = default`, so every value member must be
// noexcept-movable. A2/command_stack.hpp:189-194's precedent: aggregate asserts FIRST, then
// per-member ones, so a future regression NAMES the culprit instead of failing an opaque aggregate.
static_assert(std::is_nothrow_move_constructible_v<AssetDatabase>);
static_assert(std::is_nothrow_move_assignable_v<AssetDatabase>);
static_assert(std::is_nothrow_move_constructible_v<std::vector<AssetRecord>>);
static_assert(std::is_nothrow_move_assignable_v<std::vector<AssetRecord>>);
static_assert(
    std::is_nothrow_move_assignable_v<std::unordered_map<std::string, std::size_t, PathHash, std::equal_to<>>>);
static_assert(std::is_nothrow_move_assignable_v<std::unordered_map<Guid, std::size_t>>);

}  // namespace engine::editor

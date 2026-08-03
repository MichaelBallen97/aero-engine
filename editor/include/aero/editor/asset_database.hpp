#pragma once
// Aero Engine — AssetDatabase: the scan over a project's assets root (task 3.1.1). PUBLIC, and free
// of ImGui, SDL, entt and every build gate (D4, project.hpp's precedent). Still `<filesystem>`-free
// by file placement -- all disk access lives in asset_database.cpp, which composes 2.2.4's
// listDirectory and 2.5.1/2.6.1's text_file rather than touching std::filesystem directly.
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/project_files.hpp>  // ScanStatus -- reused, never redeclared

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
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
    std::vector<AssetRecord> records;  // sorted by relativePath (planAssetMetas' own contract) --
                                       // findByPath is a std::lower_bound over THIS vector directly.
    // MSVC's std::unordered_map move CONSTRUCTOR is not noexcept (measured in CI: C2607 on the
    // aggregate static_assert below, libc++/libstdc++ both hold, MSVC's STL does not) -- the
    // documented fallback (plan A2 part 2), applied for real rather than staying theoretical. A
    // sorted index vector is unconditionally nothrow-movable on all three standard libraries: no
    // hash table, no bucket array, no allocator-equality question. Guid has operator< (AC-2), so
    // std::lower_bound applies here exactly as it does to `records` above. Invalid records ABSENT
    // (INV-A7); sorted by Guid, NOT by index -- rebuilt (sorted) once per rescan, not maintained
    // incrementally.
    std::vector<std::pair<Guid, std::size_t>> byGuid;
};

// F10 (editor_app.hpp): EditorApp's move is `noexcept = default`, so every value member must be
// noexcept-movable. A2/command_stack.hpp:189-194's precedent: aggregate asserts FIRST, then
// per-member ones, so a future regression NAMES the culprit instead of failing an opaque aggregate.
static_assert(std::is_nothrow_move_constructible_v<AssetDatabase>);
static_assert(std::is_nothrow_move_assignable_v<AssetDatabase>);
static_assert(std::is_nothrow_move_constructible_v<std::vector<AssetRecord>>);
static_assert(std::is_nothrow_move_assignable_v<std::vector<AssetRecord>>);
static_assert(std::is_nothrow_move_constructible_v<std::vector<std::pair<Guid, std::size_t>>>);
static_assert(std::is_nothrow_move_assignable_v<std::vector<std::pair<Guid, std::size_t>>>);

}  // namespace engine::editor

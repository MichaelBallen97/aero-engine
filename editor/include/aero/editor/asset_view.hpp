#pragma once
// Aero Engine — the asset browser's presentation MODEL (task 3.1.3). PUBLIC, pure, and the
// project_files.hpp shape verbatim: free of ImGui, SDL, entt, <filesystem> and every build gate.
// NOTHING HERE LOGS (INV-V8): every rule below is provable from a std::vector or std::span literal
// with no context of any kind -- classification, icons, the filter, the whole-project search and the
// grid's column math all live here.
#include <aero/editor/asset_meta.hpp>     // AssetRecord
#include <aero/editor/project_files.hpp>  // FileEntry -- code-review BLOCKING-2 (filterEntriesByKind)

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

enum class AssetKind : std::uint8_t { Folder = 0, Texture, Model, Audio, Text, Unknown };

// Classification is on the LAST extension, ASCII-lowercased. A directory is ALWAYS Folder, whatever
// it is called ("textures.png/" is a folder). No extension, or a trailing dot, -> Unknown.
//   Texture: png jpg jpeg tga bmp gif hdr psd ktx2 dds
//   Model:   gltf glb fbx obj blend dae ply stl
//   Audio:   wav mp3 ogg flac
//   Text:    json txt md hlsl glsl ts js
[[nodiscard]] AssetKind classifyAssetKind(std::string_view fileName, bool isDirectory) noexcept;

// The stb_image-readable SUBSET of Texture, and deliberately NOT the same predicate: .ktx2 and .dds
// are textures that get an ICON, not a preview. Keeping the two tables separate is what stops a
// future container format being silently promoted to "decodable" by an edit to the kind table.
[[nodiscard]] bool isThumbnailDecodable(std::string_view fileName) noexcept;

[[nodiscard]] std::string_view assetKindLabel(AssetKind kind) noexcept;  // for the filter combo;
                                                                         // switch with NO default:

// "wood.png"->"PNG"; "scene.gltf"->"GLTF"; "model.blend1"->"BLE1" (4 max); "README"->"FILE".
// ASCII uppercase only; a non-ASCII byte anywhere in the extension falls back to "FILE" rather than
// mangling bytes. A trailing dot ("x.") is no extension -> "FILE".
[[nodiscard]] std::string iconLabelFor(std::string_view fileName);

struct IconColor {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};
[[nodiscard]] IconColor iconColorFor(AssetKind kind) noexcept;  // total over the enum, one fixed
                                                                // colour each, switch with no default:

enum class AssetViewMode : std::uint8_t { Grid = 0, List };
enum class TileSize : std::uint8_t { Small = 0, Medium, Large };
// DPI-proportional like everything in 2.2.4: 4 / 6 / 8 x ImGui::GetFontSize(), never a pixel count.
[[nodiscard]] float tileEdgeFontMultiple(TileSize size) noexcept;

inline constexpr std::size_t TILE_CAPTION_LINES = 2;            // A15
inline constexpr float TILE_CAPTION_PAD_FONT_MULTIPLE = 0.25F;  // A15

// max(1, floor((availWidth + spacing) / (tileWidth + spacing))). NaN-safe and division-safe: a
// non-finite or non-positive availWidth, or a (tileWidth + spacing) <= 0, yields 1 -- NEVER 0, which
// would be a division by zero at the call site (rows = (n + columns - 1) / columns).
[[nodiscard]] int gridColumnsFor(float availWidth, float tileWidth, float spacing) noexcept;

struct AssetFilter {
    std::string query;  // ASCII-case-insensitive SUBSTRING over the LEAF name only
    AssetKind kind = AssetKind::Unknown;
    bool anyKind = true;  // true == the `All` selection; `kind` is then ignored
};
// Takes the LEAF name, not the path: "assets/wood/plank.png" must NOT match the query "wood", or
// every asset in a well-named folder becomes a hit and the feature is useless. A directory only ever
// matches AssetKind::Folder.
[[nodiscard]] bool matchesFilter(std::string_view leafName, bool isDirectory, const AssetFilter& filter) noexcept;

// code-review BLOCKING-2 (AC-13's first clause: "Textures alone filters the current directory"): the
// WIRING matchesFilter needs to actually filter a directory listing when no query is set -- the panel
// had matchesFilter (and its own tests, AV37/AV38) but never called it for the non-search path, so
// choosing a kind with an empty search box changed nothing on screen. Pure over a span, provable from
// a std::vector<FileEntry> literal: returns the INDICES into `entries` whose (name, isDirectory)
// satisfies matchesFilter, in `entries`' own order (never reordered). filter.anyKind == true (the
// default) returns every index unfiltered -- the common, cheapest case.
[[nodiscard]] std::vector<std::size_t> filterEntriesByKind(std::span<const FileEntry> entries,
                                                            const AssetFilter& filter);

inline constexpr std::size_t MAX_SEARCH_RESULTS = 2000;

struct SearchHit {
    std::string relativePath;
    std::size_t recordIndex = 0;
};
struct SearchResult {
    std::vector<SearchHit> hits;  // in the input's order, already byte-lexicographic
    std::size_t total = 0;        // UNCAPPED match count -- NEVER capped (seed S15)
    bool truncated = false;       // total > hits.size()
};
// PURE over a span, never over the database: provable from a std::vector<AssetRecord> literal.
[[nodiscard]] SearchResult searchAssets(std::span<const AssetRecord> records, const AssetFilter& filter,
                                        std::size_t cap = MAX_SEARCH_RESULTS);

}  // namespace engine::editor

#pragma once
// Aero Engine — the asset browser's presentation MODEL (task 3.1.3). PUBLIC, pure, and the
// project_files.hpp shape verbatim: free of ImGui, SDL, entt, <filesystem> and every build gate.
// NOTHING HERE LOGS (INV-V8): every rule below is provable from a std::vector or std::span literal
// with no context of any kind -- classification, icons, the filter, the whole-project search and the
// grid's column math all live here.
#include <aero/editor/asset_meta.hpp>     // AssetRecord
#include <aero/editor/project_files.hpp>  // FileEntry -- code-review BLOCKING-2 (filterEntriesByKind)

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>  // task E.4.5 -- CaptionLineFits, the injected caption measurer
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// task 3.4.2 (D9): Material is INSERTED before Unknown, which stays last as the catch-all. Safe to
// insert rather than append because the kind is PRESENTATION-ONLY -- nothing anywhere persists its
// numeric value (one static_cast site, transient, in the browser's kind-filter action), so Unknown
// moving 5 -> 6 is a non-event.
enum class AssetKind : std::uint8_t { Folder = 0, Texture, Model, Audio, Text, Material, Unknown };

// Classification is on the LAST extension, ASCII-lowercased. A directory is ALWAYS Folder, whatever
// it is called ("textures.png/" is a folder). No extension, or a trailing dot, -> Unknown.
//   Texture:  png jpg jpeg tga bmp gif hdr psd ktx2 dds
//   Model:    gltf glb fbx obj blend dae ply stl
//   Audio:    wav mp3 ogg flac
//   Text:     json txt md hlsl glsl ts js
//   Material: aeromat
[[nodiscard]] AssetKind classifyAssetKind(std::string_view fileName, bool isDirectory) noexcept;

// The stb_image-readable SUBSET of Texture, and deliberately NOT the same predicate: .ktx2 and .dds
// are textures that get an ICON, not a preview. Keeping the two tables separate is what stops a
// future container format being silently promoted to "decodable" by an edit to the kind table.
[[nodiscard]] bool isThumbnailDecodable(std::string_view fileName) noexcept;

[[nodiscard]] std::string_view assetKindLabel(AssetKind kind) noexcept;  // for the filter combo;
                                                                         // switch with NO default:

// EVERY AssetKind, in enum order -- the Asset Browser's kind-filter combo iterates exactly this and
// owns no list of its own. It lives HERE, beside the enum, because a copy inside the panel's draw
// function is unreachable from every test tier in this tree: dropping an entry from one leaves that
// kind unfilterable with no error, no log and no red test anywhere (measured directly -- sabotage seed
// S2 stayed green through all 1570 + 119 cases before this constant existed). AV53 is the pin, and it
// works only because there is one list rather than two.
inline constexpr std::array<AssetKind, 7> ASSET_KIND_FILTER_OPTIONS{
    AssetKind::Folder, AssetKind::Texture,  AssetKind::Model,  AssetKind::Audio,
    AssetKind::Text,   AssetKind::Material, AssetKind::Unknown};

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

// task E.4.4: what the Asset Browser's directory listing KEEPS, decided at cache-fill time. It composes
// the ONE roster in asset_meta.hpp; it never restates it.
//
// `isDirectory` LEADS, and it is load-bearing: the roster is about FILES, and a folder the user named
// "backup.bak", "old~" or "Thumbs.db" is a folder they made. Hiding it would remove it from the grid AND
// from the left-hand tree -- a directory the editor could then never open -- with no error, no log, and
// nothing below the ImGui tier able to see it.
//
// The HIDDEN rule is deliberately ABSENT. listDirectory already applies it, gated on the panel's own
// `Show hidden` checkbox; restating it here would make that checkbox do nothing at all.
//
// Sidecars and ignored names are dropped UNCONDITIONALLY: both are classes the editor KNOWS are not
// content, so `Show hidden` reveals dotfiles and nothing else. "" answers true; listDirectory never yields
// an empty leaf (project_files.cpp skips one and counts it), so that answer is defined, not reachable.
[[nodiscard]] bool isBrowserVisibleName(std::string_view leafName, bool isDirectory) noexcept;

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

// ---- task E.4.4 (validation finding 2): the panel's vertical budget ----------------------------------
// The panel used to reserve ONE line for the footer and give the panes everything else, so the
// "Issues (N)" header -- and, opened, its whole list -- was drawn BELOW the panes, past the bottom of the
// panel, with the footer after it: with 40 orphans the macOS validation pass reached both only by
// scrolling the whole panel. The Issues region is now reserved BEFORE the panes are sized, and its body is
// a scrollable child whose height is capped, so the panes, the header, the body and the footer always
// share the panel's height rather than overflow it. .claude/rules/editor.md's "A panel with fixed
// regions" section is the pattern; this is its second instance.
//
// STARTING VALUES. The pane minimum is in FONT UNITS, the MATERIAL_PREVIEW_* precedent, so a DPI change
// moves it with the font; the cap is a fraction of the panel, so a taller panel shows more rows.
inline constexpr float ASSET_PANES_MIN_FONT = 4.0F;            // the panes' height before the body yields
inline constexpr float ASSET_ISSUES_BODY_MAX_FRACTION = 0.4F;  // the body's cap, of availHeight

struct AssetBrowserLayoutMetrics {
    float availHeight = 0.0F;     // GetContentRegionAvail().y right after the header row
    float fontSize = 0.0F;        // GetFontSize()
    float frameHeight = 0.0F;     // GetFrameHeight(): the Issues header's height, and the footer's line
    float textLineHeight = 0.0F;  // GetTextLineHeight(): one body row, which is the body's floor
    float itemSpacingY = 0.0F;    // style.ItemSpacing.y
    bool issuesShown = false;     // the scan reported at least one issue, so the header draws
    bool issuesOpen = false;      // the header is open, so the body child draws
    // The body's NATURAL height, measured inside the child on the last frame it drew (one frame late, the
    // shape of ImGui's own auto-resize). 0 before the first measurement; the body is then one row tall.
    float issuesContentHeight = 0.0F;
};

struct AssetBrowserLayout {
    float paneHeight = 1.0F;        // the two panes' shared height, >= 1: BeginChild reads 0 as "fill"
    float issuesHeight = 0.0F;      // header + spacing [+ body + spacing]; 0 when no issue is shown
    float issuesBodyHeight = 0.0F;  // the body child's height; 0 iff the body is not drawn, else >= 1
    float footerHeight = 0.0F;      // spacing below the region above + the footer's line (frame height)
};

// TOTAL: a metric that is not a finite, positive number counts as 0, so no NaN and no negative reaches
// BeginChild, and std::clamp is never used (it returns NaN for a NaN on libc++).
//
// THE BUDGET. footer and header are fixed. The body wants its measured content height, at least one text
// row and at most floor(ASSET_ISSUES_BODY_MAX_FRACTION * avail); it YIELDS first, so the panes keep
// ASSET_PANES_MIN_FONT * fontSize, but never below one row. The panes take what is left, never below 1.
// So paneHeight + issuesHeight + footerHeight == availHeight whenever availHeight is at least
// footer + header + one row + one spacing + 1, and only a panel shorter than that can scroll.
[[nodiscard]] AssetBrowserLayout assetBrowserLayout(const AssetBrowserLayoutMetrics& metrics) noexcept;

// ---- task E.4.5 (the code-review round): a caption LINE that keeps the file name -------------------------
// A tile carrying a document name gives its file name ONE line, and a search hit's caption source is
// "parent/leaf" -- so right-eliding it kept the FOLDER and dropped the very name the tile exists to show. The two
// rules below decide a line PURELY: the host injects its measurer (TRUE == `text` fits the line), which makes
// each rule a tier-0 table over a code-point budget and leaves the ImGui half one lambda (asset_tile.cpp).
// Neither ever cuts inside a UTF-8 sequence, and both spend the caption's own ellipsis, U+2026.
using CaptionLineFits = std::function<bool(std::string_view)>;

// `text` when it fits; else the LONGEST byte prefix whose (prefix + ellipsis) fits, stepped back to a UTF-8
// boundary; the ellipsis alone when not even one code point does. elideForCaption's rule, moved here so it can be
// tested without an ImGui context -- elideForCaption is now this plus its measurer.
[[nodiscard]] std::string elideCaptionRight(std::string_view text, const CaptionLineFits& fits);

// Line one of a SUBTITLED tile: `captionSource` when it fits; else the ellipsis + the LONGEST suffix that fits
// and still holds the WHOLE `leaf`, starting on a UTF-8 boundary; else -- not even ellipsis + leaf fits -- the
// leaf right-elided. `leaf` is the file name and must END `captionSource` (a hit's "parent/leaf"); a caption
// source that IS the leaf, or that does not end with it, is right-elided whole -- today's caption rule exactly.
[[nodiscard]] std::string subtitledTileCaptionLine(std::string_view captionSource, std::string_view leaf,
                                                   const CaptionLineFits& fits);

}  // namespace engine::editor

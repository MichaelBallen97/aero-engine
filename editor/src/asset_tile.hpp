#pragma once
// SRC-PRIVATE: it needs a live ImGui context (CalcTextSize, GetTextLineHeight, GetFont), so it cannot
// join asset_view.cpp, which is pure. task E.3.3 (D6): drawTile's FACE, extracted so the picker and
// the browser paint ONE tile rather than two that drift by a pixel of padding a year later.
#include <aero/editor/asset_view.hpp>  // AssetKind

#include <cstddef>  // task E.4.5 -- elideForCaption's maxLines
#include <imgui.h>
#include <optional>  // task E.4.5 -- AssetTileFace::tint
#include <string>
#include <string_view>

namespace engine::editor {

struct AssetTileFace {
    void* nativeTexture = nullptr;  // a Ready thumbnail, or nullptr -> the kind icon
    AssetKind kind = AssetKind::Unknown;
    bool isDirectory = false;
    std::string_view fileName;       // the leaf -- iconLabelFor's input
    std::string_view captionSource;  // what the caption elides: the leaf, or "parent/leaf" for a hit
    float tileW = 0.0F;
    float tileEdge = 0.0F;
    float pad = 0.0F;
    // task E.4.5, APPENDED (never inserted): a designated initialiser must follow DECLARATION order in C++20;
    // clang accepts a wrong order with a warning while GCC and MSVC reject it (E.2.2's finding 3). Both
    // default to today's behaviour EXACTLY.
    //
    // A SECOND, DIMMED caption line under the first. EMPTY == today's caption, wrapped to TILE_CAPTION_LINES;
    // NON-EMPTY == the caption elided to ONE line with this elided to one beneath it. The caller's reserved
    // caption height is ALREADY two lines (asset_view.hpp's TILE_CAPTION_LINES), so tileH, tileW, pad and the
    // clipper's rowHeight do not move -- which is the reason this is the layout. The viewed bytes must outlive
    // the call; a material card's name does (material_card.hpp's materialCardSubtitle).
    std::string_view subtitle;
    // The icon-rect fill when the caller has a better answer than iconColorFor(kind) -- today, one caller: a
    // material's own base colour. DISENGAGED == no swatch: a file with no thumbnail draws a hatched well (task E.6.3).
    std::optional<IconColor> tint;
    // task E.6.3 (D8), APPENDED LAST in declaration order (the rule above). `folderCount` is the folder's RECURSIVE
    // record count (FolderCountCache), drawn at the well's bottom-right; 0 draws none. The three states come off the
    // host's own Selectable -- its `selected` argument, IsItemHovered, IsItemActive -- because ImGui paints no tile
    // fill (every host clears the Header slots around that one call), so the face draws every state itself.
    std::size_t folderCount = 0;
    bool hovered = false;
    bool selected = false;
    bool held = false;
};

// Moved VERBATIM from asset_browser_panel.cpp's anonymous namespace: the LONGEST byte prefix whose
// (prefix + ellipsis) still fits `maxLines` lines at `wrapWidth`, landing on a UTF-8 boundary (never slicing
// a multi-byte sequence). Needs a live ImGui context, which is what puts it here.
// task E.4.5: `maxLines` APPENDED AND DEFAULTED to TILE_CAPTION_LINES, so the one existing call compiles and
// behaves byte-identically; 1 is what each half of a two-line caption gets.
// task E.4.5's code-review round: the RULE is asset_view.hpp's elideCaptionRight now, pure and tier-0 tested;
// this function is that rule plus its ImGui measurer. A subtitled tile's FIRST line is not this rule at all:
// it is subtitledTileCaptionLine, which keeps the file name of a search hit whose folder does not fit.
[[nodiscard]] std::string elideForCaption(const std::string& name, float wrapWidth,
                                          std::size_t maxLines = TILE_CAPTION_LINES);

// What the face handed to its draw calls, for the host's record (task E.6.3, D14). Colours are ImGui-packed.
struct AssetTileDrawn {
    bool ringed = false;         // a ring was drawn: every face does
    bool hatched = false;        // a file with no preview and no tint
    bool folderCounted = false;  // a folder count was drawn
    ImU32 ringColor = 0;         // hover at rest, active hovered or held, accentBorder selected
    ImU32 hatchLabelColor = 0;   // textSecondary, never a kind colour
    ImU32 folderTabColor = 0;    // folderTab, never folder's
};

// drawTile's face, drawn at `itemMin`: the WELL (the icon rect) -- filled `tile`, `raisedHeader` hovered or held,
// `selection` selected -- holding the thumbnail image, a material's own colour, the two-tone folder with its count, or
// a hatched well with the kind dot and the extension label; then the ring (hover / active / accentBorder,
// tileRingHeight() thick); then the caption, `textBright` when selected.
//
// IT SUBMITS NO ImGui ITEM -- it draws into `drawList` and nothing else, so it never disturbs the
// caller's last-item data. That is load-bearing for both hosts: the browser's Selectable and the
// picker's are submitted BEFORE the face, so IsItemHovered() afterwards still reads the right item,
// and a caller's drop target still attaches to the item it means to.
//
// `scratch` is the caller's own per-frame string, reused rather than allocated (the labelScratch
// idiom) -- it is clobbered. NOT [[nodiscard]]: the reference row draws an icon and records nothing.
AssetTileDrawn drawAssetTileFace(ImDrawList* drawList, ImVec2 itemMin, const AssetTileFace& face, std::string& scratch);

}  // namespace engine::editor

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
    // material's own base colour. DISENGAGED == iconColorFor(kind): every other tile in the editor, unchanged.
    std::optional<IconColor> tint;
};

// Moved VERBATIM from asset_browser_panel.cpp's anonymous namespace: the LONGEST byte prefix whose
// (prefix + ellipsis) still fits `maxLines` lines at `wrapWidth`, landing on a UTF-8 boundary (never slicing
// a multi-byte sequence). Needs a live ImGui context, which is what puts it here.
// task E.4.5: `maxLines` APPENDED AND DEFAULTED to TILE_CAPTION_LINES, so the one existing call compiles and
// behaves byte-identically; 1 is what each half of a two-line caption gets.
[[nodiscard]] std::string elideForCaption(const std::string& name, float wrapWidth,
                                          std::size_t maxLines = TILE_CAPTION_LINES);

// drawTile's face, drawn at `itemMin`: the thumbnail image or the kind-coloured icon rect with its
// folder tab or centred label, then the elided caption.
//
// IT SUBMITS NO ImGui ITEM -- it draws into `drawList` and nothing else, so it never disturbs the
// caller's last-item data. That is load-bearing for both hosts: the browser's Selectable and the
// picker's are submitted BEFORE the face, so IsItemHovered() afterwards still reads the right item,
// and a caller's drop target still attaches to the item it means to.
//
// `scratch` is the caller's own per-frame string, reused rather than allocated (the labelScratch
// idiom) -- it is clobbered.
void drawAssetTileFace(ImDrawList* drawList, ImVec2 itemMin, const AssetTileFace& face, std::string& scratch);

}  // namespace engine::editor

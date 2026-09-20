#pragma once
// SRC-PRIVATE: it needs a live ImGui context (CalcTextSize, GetTextLineHeight, GetFont), so it cannot
// join asset_view.cpp, which is pure. task E.3.3 (D6): drawTile's FACE, extracted so the picker and
// the browser paint ONE tile rather than two that drift by a pixel of padding a year later.
#include <aero/editor/asset_view.hpp>  // AssetKind

#include <imgui.h>
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
};

// Moved VERBATIM from asset_browser_panel.cpp's anonymous namespace: the LONGEST byte prefix whose
// (prefix + ellipsis) still fits TILE_CAPTION_LINES lines at `wrapWidth`, landing on a UTF-8 boundary
// (never slicing a multi-byte sequence). Needs a live ImGui context, which is what puts it here.
[[nodiscard]] std::string elideForCaption(const std::string& name, float wrapWidth);

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

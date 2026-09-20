// editor/src/asset_tile.cpp -- task E.3.3 (D6): the asset tile's FACE, moved out of
// AssetBrowserPanel::drawTile with exactly two substitutions -- the thumbnail pointer comes from the
// face rather than from a lookup, and the panel's `labelScratch` became the `scratch` parameter.
// Everything else, comments included, is drawTile's own body.
#include "asset_tile.hpp"

#include <aero/editor/asset_view.hpp>

#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <string>

namespace engine::editor {

// A15: ImGui's own text wrapping has no ellipsis and no "how many lines would this take" query for
// wrapped text, so this measures with ImGui::CalcTextSize and truncates by hand -- the LONGEST byte
// prefix whose (prefix + ellipsis) still fits TILE_CAPTION_LINES lines at `wrapWidth`, landing on a
// UTF-8 boundary (never slicing a multi-byte sequence).
std::string elideForCaption(const std::string& name, float wrapWidth) {
    const float twoLineHeight = static_cast<float>(TILE_CAPTION_LINES) * ImGui::GetTextLineHeight();
    const ImVec2 full = ImGui::CalcTextSize(name.c_str(), nullptr, false, wrapWidth);
    if (full.y <= twoLineHeight) {
        return name;
    }
    std::size_t lo = 0;
    std::size_t hi = name.size();
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo + 1) / 2);
        std::size_t cut = mid;
        while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0U) == 0x80U) {
            --cut;  // step back to a UTF-8 boundary
        }
        if (cut == 0) {
            hi = 0;
            break;
        }
        std::string candidate(name, 0, cut);
        candidate += "…";
        const ImVec2 size = ImGui::CalcTextSize(candidate.c_str(), nullptr, false, wrapWidth);
        if (size.y <= twoLineHeight) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    std::size_t finalCut = lo;
    while (finalCut > 0 && (static_cast<unsigned char>(name[finalCut]) & 0xC0U) == 0x80U) {
        --finalCut;
    }
    return name.substr(0, finalCut) + "…";
}

void drawAssetTileFace(ImDrawList* drawList, ImVec2 itemMin, const AssetTileFace& face, std::string& scratch) {
    const ImVec2 iconMin(itemMin.x + face.pad, itemMin.y + face.pad);
    const ImVec2 iconMax(iconMin.x + face.tileEdge, iconMin.y + face.tileEdge);
    const float rounding = ImGui::GetStyle().FrameRounding;

    if (face.nativeTexture != nullptr) {
        // F7 (A6): the SDL_GPU ImGui backend takes the native texture pointer directly as the id; a
        // thumbnail needs no sampler of its own. viewport_panel.cpp:201-204's exact idiom.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto texId = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(face.nativeTexture));
        drawList->AddImage(texId, iconMin, iconMax);
    } else {
        // The generated type icon -- what a Skipped/Failed/no-database entry, or an undecodable
        // extension, falls back to FOREVER.
        const IconColor color = iconColorFor(face.kind);
        const ImU32 fillColor = IM_COL32(color.r, color.g, color.b, color.a);
        drawList->AddRectFilled(iconMin, iconMax, fillColor, rounding);
        if (face.isDirectory) {
            // D6: a folder is a two-rect glyph in a fixed colour -- a small "tab" atop the body, both
            // in the SAME icon colour so it reads as one shape rather than two overlapping tiles.
            const float tabWidth = (iconMax.x - iconMin.x) * 0.45F;
            const float tabHeight = (iconMax.y - iconMin.y) * 0.18F;
            const ImU32 tabColor = IM_COL32(color.r, color.g, color.b, 255U);
            drawList->AddRectFilled(iconMin, ImVec2(iconMin.x + tabWidth, iconMin.y + tabHeight), tabColor, rounding);
        } else {
            scratch = iconLabelFor(face.fileName);
            const ImVec2 textSize = ImGui::CalcTextSize(scratch.c_str());
            const ImVec2 textPos((iconMin.x + iconMax.x - textSize.x) * 0.5F,
                                 (iconMin.y + iconMax.y - textSize.y) * 0.5F);
            drawList->AddText(textPos, IM_COL32_WHITE, scratch.c_str());
        }
    }

    // The caption: leaf name, wrapped to at most TILE_CAPTION_LINES and ellipsised beyond that (A15).
    // A15's 8-argument AddText overload -- imgui.h:3477 -- is the only one with a wrap width. A search
    // hit folds its containing folder into the SAME caption (the plan's own "subtitled" requirement),
    // so it stays identifiable outside the directory the user is currently browsing (AC-15).
    const float wrapWidth = face.tileW - (2.0F * face.pad);
    const std::string caption = elideForCaption(std::string(face.captionSource), wrapWidth);
    const ImVec2 captionPos(itemMin.x + face.pad, iconMax.y + face.pad);
    drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), captionPos, IM_COL32_WHITE, caption.c_str(), nullptr,
                      wrapWidth, nullptr);
}

}  // namespace engine::editor

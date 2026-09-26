// editor/src/asset_tile.cpp -- task E.3.3 (D6): the asset tile's FACE, moved out of
// AssetBrowserPanel::drawTile with exactly two substitutions -- the thumbnail pointer comes from the
// face rather than from a lookup, and the panel's `labelScratch` became the `scratch` parameter.
// Everything else, comments included, is drawTile's own body.
#include "asset_tile.hpp"

#include <aero/editor/asset_view.hpp>
#include <aero/editor/material_card.hpp>  // task E.4.5 -- materialSwatchWantsDarkLabel

#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <optional>
#include <string>

namespace engine::editor {

namespace {

// task E.4.5: the kind label's colour on a PALE swatch. THE ONLY COLOUR LITERAL THIS TASK STATES, and it is a
// contrast answer rather than a palette choice -- handed to E.6.1 with the theme: when EditorTheme lands this
// becomes a theme role and materialSwatchWantsDarkLabel's threshold a theme question.
constexpr ImU32 DARK_SWATCH_LABEL = IM_COL32(24, 24, 24, 255);

}  // namespace

// A15: ImGui's own text wrapping has no ellipsis and no "how many lines would this take" query for
// wrapped text, so this measures with ImGui::CalcTextSize and truncates by hand -- the LONGEST byte
// prefix whose (prefix + ellipsis) still fits `maxLines` lines at `wrapWidth` (TILE_CAPTION_LINES unless
// the caller asks otherwise -- task E.4.5), landing on a UTF-8 boundary (never slicing a multi-byte sequence).
std::string elideForCaption(const std::string& name, float wrapWidth, std::size_t maxLines) {
    const float budgetHeight = static_cast<float>(maxLines) * ImGui::GetTextLineHeight();
    const ImVec2 full = ImGui::CalcTextSize(name.c_str(), nullptr, false, wrapWidth);
    if (full.y <= budgetHeight) {
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
        if (size.y <= budgetHeight) {
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
        // task E.4.5: the caller's tint wins over the kind colour, and NOTHING ELSE about this branch changes: a
        // material with a parsed, finite base colour paints its OWN colour; every other tile paints its kind's.
        const IconColor color = face.tint.value_or(iconColorFor(face.kind));
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
            // task E.4.5: THE LABEL'S CONTRAST FOLLOWS THE FILL. A white "AERT" (iconLabelFor's label for
            // .aeromat, AV51) on a pale swatch is unreadable, and a fixed white is right only while every fill
            // happens to be dark -- the "true by accident" shape. Every UNtinted tile keeps white exactly.
            const bool darkLabel = face.tint.has_value() && materialSwatchWantsDarkLabel(color);
            drawList->AddText(textPos, darkLabel ? DARK_SWATCH_LABEL : IM_COL32_WHITE, scratch.c_str());
        }
    }

    // The caption: leaf name, wrapped to at most TILE_CAPTION_LINES and ellipsised beyond that (A15).
    // A15's 8-argument AddText overload -- imgui.h:3477 -- is the only one with a wrap width. A search
    // hit folds its containing folder into the SAME caption (the plan's own "subtitled" requirement),
    // so it stays identifiable outside the directory the user is currently browsing (AC-15).
    const float wrapWidth = face.tileW - (2.0F * face.pad);
    const ImVec2 captionPos(itemMin.x + face.pad, iconMax.y + face.pad);
    // task E.4.5: the font and its size, read once as named locals so each AddText below is a single line.
    ImFont* const font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    if (face.subtitle.empty()) {
        // TODAY'S CAPTION, argument for argument: the caption source wrapped to TILE_CAPTION_LINES.
        const std::string caption = elideForCaption(std::string(face.captionSource), wrapWidth);
        drawList->AddText(font, fontSize, captionPos, IM_COL32_WHITE, caption.c_str(), nullptr, wrapWidth, nullptr);
        return;
    }
    // task E.4.5: TWO one-line captions in exactly the space the wrapped one had -- the file name first, in
    // today's white, then the document's name beneath it in the theme's own disabled-text colour (a colour the
    // theme already owns, E.3.4's D6 precedent). Both are draw-list text, so no format string exists on this
    // path (D12), and still NO ImGui ITEM is submitted: GetColorU32, GetFont and GetTextLineHeight are reads.
    // GetColorU32 applies the current style alpha, so the subtitle's exact byte value is context-dependent and
    // is asserted nowhere as a constant.
    const std::string primary = elideForCaption(std::string(face.captionSource), wrapWidth, 1U);
    drawList->AddText(font, fontSize, captionPos, IM_COL32_WHITE, primary.c_str(), nullptr, wrapWidth, nullptr);
    const std::string secondary = elideForCaption(std::string(face.subtitle), wrapWidth, 1U);
    const ImVec2 subtitlePos(captionPos.x, captionPos.y + ImGui::GetTextLineHeight());
    const ImU32 dimmed = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    drawList->AddText(font, fontSize, subtitlePos, dimmed, secondary.c_str(), nullptr, wrapWidth, nullptr);
}

}  // namespace engine::editor

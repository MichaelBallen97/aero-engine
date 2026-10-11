// editor/src/asset_tile.cpp -- task E.3.3 (D6): the asset tile's FACE, moved out of
// AssetBrowserPanel::drawTile with exactly two substitutions -- the thumbnail pointer comes from the
// face rather than from a lookup, and the panel's `labelScratch` became the `scratch` parameter.
// Everything else, comments included, is drawTile's own body.
#include "asset_tile.hpp"

#include <aero/editor/asset_browser_chrome.hpp>  // task E.6.3: folderGlyphRects
#include <aero/editor/asset_view.hpp>
#include <aero/editor/editor_theme.hpp>   // task E.6.1 -- the swatch label pair (D17)
#include <aero/editor/material_card.hpp>  // task E.4.5 -- materialSwatchWantsDarkLabel
#include <aero/editor/status_bar.hpp>     // task E.6.3: groupThousands

#include "editor_theme_ui.hpp"  // task E.6.1: toImU32; task E.6.3: currentUiScale
#include "panel_chrome_ui.hpp"  // task E.6.3: roles, faces, the hatch

#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

namespace {

// task E.4.5: the kind label's colour on a PALE swatch. THE ONLY COLOUR LITERAL THIS TASK STATES, and it is a
// contrast answer rather than a palette choice -- handed to E.6.1 with the theme: when EditorTheme lands this
// becomes a theme role and materialSwatchWantsDarkLabel's threshold a theme question.
// task E.6.1: now EDITOR_THEME.swatchLabel (D17), both bytes unchanged.
// Built with ImGui's own IM_COL32 from the theme's bytes, so the packing is the library's.
[[nodiscard]] constexpr ImU32 labelColor(Srgb8 c) noexcept { return IM_COL32(c.r, c.g, c.b, c.a); }
constexpr ImU32 SWATCH_LABEL_ON_LIGHT = labelColor(EDITOR_THEME.swatchLabel.onLight);
constexpr ImU32 SWATCH_LABEL_ON_DARK = labelColor(EDITOR_THEME.swatchLabel.onDark);

// task E.4.5's code-review round: THE caption measurer -- TRUE when `text`, wrapped at `wrapWidth`, is no taller
// than `budgetHeight`. The explicit end pointer measures a view without copying it; an empty view measures the
// literal "", so ImGui is never handed a null pointer whatever the view's data() is.
[[nodiscard]] CaptionLineFits captionFitsWithin(float wrapWidth, float budgetHeight) {
    return [wrapWidth, budgetHeight](std::string_view text) {
        const char* const begin = text.empty() ? "" : text.data();
        return ImGui::CalcTextSize(begin, begin + text.size(), false, wrapWidth).y <= budgetHeight;
    };
}

// task E.6.3 (D8): the well's state, as roles. Selected wins over hovered, which wins over rest.
[[nodiscard]] ChromeRole wellFillRole(const AssetTileFace& face) noexcept {
    if (face.selected) {
        return ChromeRole::Selection;
    }
    return (face.hovered || face.held) ? ChromeRole::RaisedHeader : ChromeRole::Tile;
}
[[nodiscard]] ChromeRole wellRingRole(const AssetTileFace& face) noexcept {
    if (face.selected) {
        return ChromeRole::AccentBorder;
    }
    return (face.hovered || face.held) ? ChromeRole::Active : ChromeRole::Hover;  // D3: the resting ring IS hover
}

}  // namespace

// A15: ImGui's own text wrapping has no ellipsis and no "how many lines would this take" query for
// wrapped text, so this measures with ImGui::CalcTextSize and truncates by hand -- the LONGEST byte
// prefix whose (prefix + ellipsis) still fits `maxLines` lines at `wrapWidth` (TILE_CAPTION_LINES unless
// the caller asks otherwise -- task E.4.5), landing on a UTF-8 boundary (never slicing a multi-byte sequence).
std::string elideForCaption(const std::string& name, float wrapWidth, std::size_t maxLines) {
    // task E.4.5's code-review round: the rule is elideCaptionRight's now (asset_view.hpp), pure and tier-0 tested;
    // this is its measurer, byte for byte the old body's.
    const float budgetHeight = static_cast<float>(maxLines) * ImGui::GetTextLineHeight();
    return elideCaptionRight(name, captionFitsWithin(wrapWidth, budgetHeight));
}

AssetTileDrawn drawAssetTileFace(ImDrawList* drawList, ImVec2 itemMin, const AssetTileFace& face,
                                 std::string& scratch) {
    AssetTileDrawn drawn;
    const ImVec2 iconMin(itemMin.x + face.pad, itemMin.y + face.pad);
    const ImVec2 iconMax(iconMin.x + face.tileEdge, iconMin.y + face.tileEdge);
    const float s = currentUiScale();
    const float rounding = EDITOR_THEME.panels.wellRounding * s;  // task E.6.3: the well's corner (D8)

    if (face.nativeTexture != nullptr) {
        // F7 (A6): the SDL_GPU ImGui backend takes the native texture pointer directly as the id; a
        // thumbnail needs no sampler of its own. viewport_panel.cpp:201-204's exact idiom.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto texId = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(face.nativeTexture));
        drawList->AddImage(texId, iconMin, iconMax);
    } else if (face.isDirectory) {
        // task E.6.3 (D3, D8): the well carries the state, and on it the mock's two-tone folder -- a grey body and a
        // lighter tab ABOVE it. The tab used to sit INSIDE the body in the body's own colour, where it could not be
        // seen (seed S50 restores exactly that).
        roleFill(drawList, iconMin, iconMax, wellFillRole(face), rounding);
        const FolderGlyph glyph = folderGlyphRects(Vec2{iconMin.x, iconMin.y}, Vec2{iconMax.x, iconMax.y});
        const IconColor body = iconColorFor(AssetKind::Folder);
        const float glyphRounding = rounding * 0.5F;
        drawn.folderTabColor = toImU32(EDITOR_THEME.assetKind.folderTab);
        drawList->AddRectFilled(ImVec2(glyph.tabMin.x, glyph.tabMin.y), ImVec2(glyph.tabMax.x, glyph.tabMax.y),
                                drawn.folderTabColor, glyphRounding);
        drawList->AddRectFilled(ImVec2(glyph.bodyMin.x, glyph.bodyMin.y), ImVec2(glyph.bodyMax.x, glyph.bodyMax.y),
                                IM_COL32(body.r, body.g, body.b, body.a), glyphRounding);
        if (face.folderCount > 0U) {
            // The folder's RECURSIVE count at the well's bottom-right, Mono small textMuted (D8).
            scratch = groupThousands(face.folderCount);
            const float inset = EDITOR_THEME.panels.rowMetaInsetX * s;
            const ImVec2 at(iconMax.x - inset - faceTextWidth(PanelFace::MonoSmall, scratch),
                            iconMax.y - inset - panelFacePoints(PanelFace::MonoSmall));
            drawRoleText(drawList, PanelFace::MonoSmall, at, ChromeRole::TextMuted, scratch);
            drawn.folderCounted = true;
        }
    } else if (face.tint.has_value()) {
        // task E.4.5: a material with a parsed, finite base colour paints its OWN colour, unchanged by E.6.3 -- the
        // swatch IS its preview, so it is not hatched.
        const IconColor color = *face.tint;
        drawList->AddRectFilled(iconMin, iconMax, IM_COL32(color.r, color.g, color.b, color.a), rounding);
        scratch = iconLabelFor(face.fileName);
        const ImVec2 textSize = ImGui::CalcTextSize(scratch.c_str());
        const ImVec2 textPos((iconMin.x + iconMax.x - textSize.x) * 0.5F, (iconMin.y + iconMax.y - textSize.y) * 0.5F);
        // task E.4.5: THE LABEL'S CONTRAST FOLLOWS THE FILL -- a pale swatch takes the dark label.
        const ImU32 labelInk = materialSwatchWantsDarkLabel(color) ? SWATCH_LABEL_ON_LIGHT : SWATCH_LABEL_ON_DARK;
        drawList->AddText(textPos, labelInk, scratch.c_str());
    } else {
        // task E.6.3 (D8): a file with no preview -- a HATCHED well, its extension label in textSecondary (6.75:1 on
        // the stripe, 7.81:1 on `tile`, TH16), the kind colour kept on a non-text mark: a kindDotSize dot, the label
        // kindDotGap after it. A kind colour carries no contrast floor (`unknown` is 1.81:1 on the stripe), which is
        // why the label never draws in one.
        hatchedWell(drawList, iconMin, iconMax, rounding);
        scratch = iconLabelFor(face.fileName);
        const float dot = EDITOR_THEME.panels.kindDotSize * s;
        const float gap = EDITOR_THEME.panels.kindDotGap * s;
        const float line = panelFacePoints(PanelFace::BodySmall);
        const float left = (iconMin.x + iconMax.x - (dot + gap + faceTextWidth(PanelFace::BodySmall, scratch))) * 0.5F;
        const float top = (iconMin.y + iconMax.y - line) * 0.5F;
        const IconColor kind = iconColorFor(face.kind);
        drawList->AddCircleFilled(ImVec2(left + (dot * 0.5F), top + (line * 0.5F)), dot * 0.5F,
                                  IM_COL32(kind.r, kind.g, kind.b, kind.a));
        // Packed ONCE and handed to the draw, as the folder tab's is: the record is what the label RECEIVED.
        drawn.hatchLabelColor = roleU32(ChromeRole::TextSecondary);
        drawList->AddText(panelFaceFont(PanelFace::BodySmall), panelFacePoints(PanelFace::BodySmall),
                          ImVec2(left + dot + gap, top), drawn.hatchLabelColor, scratch.c_str());
        drawn.hatched = true;
    }
    // The ring, LAST over the well, so it frames whatever the well holds (D3: the resting ring IS `hover`).
    drawn.ringColor = roleU32(wellRingRole(face));
    drawList->AddRect(iconMin, iconMax, drawn.ringColor, rounding, 0, tileRingHeight());
    drawn.ringed = true;

    // The caption: leaf name, wrapped to at most TILE_CAPTION_LINES and ellipsised beyond that (A15).
    // A15's 8-argument AddText overload -- imgui.h:3477 -- is the only one with a wrap width. A search
    // hit folds its containing folder into the SAME caption (the plan's own "subtitled" requirement),
    // so it stays identifiable outside the directory the user is currently browsing (AC-15).
    const float wrapWidth = face.tileW - (2.0F * face.pad);
    const ImVec2 captionPos(itemMin.x + face.pad, iconMax.y + face.pad);
    // task E.4.5: the font and its size, read once as named locals so each AddText below is a single line.
    ImFont* const font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    // task E.6.3 (D8): textBright on a selected tile, `text` otherwise -- roles, never palette reads (I287).
    const ImU32 textInk = roleU32(face.selected ? ChromeRole::TextBright : ChromeRole::Text);
    if (face.subtitle.empty()) {
        // TODAY'S CAPTION: the caption source wrapped to TILE_CAPTION_LINES.
        const std::string caption = elideForCaption(std::string(face.captionSource), wrapWidth);
        drawList->AddText(font, fontSize, captionPos, textInk, caption.c_str(), nullptr, wrapWidth, nullptr);
        return drawn;
    }
    // task E.4.5: TWO one-line captions in exactly the space the wrapped one had -- the file name first, then the
    // document's name beneath it in textMuted (task E.6.1: de-emphasised but ENABLED, D16). Both are draw-list text,
    // so no format string exists on this path (D12), and still NO ImGui ITEM is submitted.
    // The code-review round: line one keeps the FILE NAME. A search hit's source is "parent/leaf", and eliding it
    // from the right kept the folder and dropped the name; subtitledTileCaptionLine drops the folder's front first.
    const CaptionLineFits oneLine = captionFitsWithin(wrapWidth, ImGui::GetTextLineHeight());
    const std::string primary = subtitledTileCaptionLine(face.captionSource, face.fileName, oneLine);
    drawList->AddText(font, fontSize, captionPos, textInk, primary.c_str(), nullptr, wrapWidth, nullptr);
    const std::string secondary = elideForCaption(std::string(face.subtitle), wrapWidth, 1U);
    const ImVec2 subtitlePos(captionPos.x, captionPos.y + ImGui::GetTextLineHeight());
    drawList->AddText(font, fontSize, subtitlePos, roleU32(ChromeRole::TextMuted), secondary.c_str(), nullptr,
                      wrapWidth, nullptr);
    return drawn;
}

}  // namespace engine::editor

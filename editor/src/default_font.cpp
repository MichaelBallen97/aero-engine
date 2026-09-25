// Aero Engine — the editor's UI font and its Windows-1252 remaps (task E.4.4's validation pass, finding 1).
// The ONLY ImGui half of default_font.hpp; the rationale for every choice is at the declarations there.
#include "default_font.hpp"

#include <imgui.h>
#include <span>
#include <string>

namespace engine::editor {

namespace {

// AddFontDefaultBitmap(), NEVER AddFontDefault(). The latter is a heuristic (imgui_draw.cpp:3166-3172): it
// returns ProggyForever once the context's expected font size -- FontSizeBase (13 while unset) times
// FontScaleMain times FontScaleDpi -- reaches 15, and the implicit first-frame add goes through that same
// heuristic. For this editor it has always answered 13, i.e. ProggyClean; calling the bitmap loader by
// name keeps that answer whatever a later style change does to the three factors, and the remap table is
// measured for ProggyClean alone.
ImFont* addProggyClean(ImFontAtlas& atlas) { return atlas.AddFontDefaultBitmap(); }

void applyCp1252Remaps(ImFont& font) {
    for (const DefaultFontRemap& remap : DEFAULT_FONT_CP1252_REMAPS) {
        font.AddRemapChar(static_cast<ImWchar>(remap.unicode), static_cast<ImWchar>(remap.cp1252Slot));
    }
}

}  // namespace

bool addEditorDefaultFont() {
    ImFont* const font = addProggyClean(*ImGui::GetIO().Fonts);
    if (font == nullptr) {
        return false;
    }
    applyCp1252Remaps(*font);
    return true;
}

DefaultFontMeasurement measureDefaultFont(DefaultFontSetup setup, std::span<const char32_t> points, float scale) {
    DefaultFontMeasurement result;
    ImGuiContext* const previous = ImGui::GetCurrentContext();
    ImGuiContext* const context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);  // CreateContext restores the previous one when there was one

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // NEVER write imgui.ini (or a log) into whatever the working directory is
    io.LogFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // glyphs bake on demand, CPU-side
    io.DisplaySize = ImVec2(64.0F, 64.0F);

    bool fontReady = true;
    switch (setup) {
        case DefaultFontSetup::ImGuiImplicit:
            break;  // ImGui adds its own default inside the first NewFrame, exactly as the editor used to
        case DefaultFontSetup::BitmapWithoutRemaps:
            fontReady = addProggyClean(*io.Fonts) != nullptr;
            break;
        case DefaultFontSetup::Editor:
            fontReady = addEditorDefaultFont();
            break;
    }

    if (fontReady) {
        ImGui::NewFrame();
        ImFont* const font = ImGui::GetFont();
        result.fontName = io.Fonts->Sources.empty() ? std::string() : std::string(io.Fonts->Sources[0].Name);
        result.referenceSize = font->LegacySize;
        result.bakedSize = font->LegacySize * scale;
        ImFontBaked* const baked = font->GetFontBaked(result.bakedSize);
        result.glyphs.reserve(points.size());
        for (const char32_t codepoint : points) {
            DefaultFontGlyph glyph;
            glyph.requested = codepoint;
            if (codepoint <= IM_UNICODE_CODEPOINT_MAX) {
                const auto c = static_cast<ImWchar>(codepoint);
                // BY VALUE, immediately: a later lookup may bake a glyph into ImFontBaked::Glyphs and
                // reallocate it, which dangles any ImFontGlyph* held across it (the header's ASan note).
                glyph.found = baked->FindGlyphNoFallback(c) != nullptr;
                const ImFontGlyph* const drawn = baked->FindGlyph(c);
                glyph.drawnCodepoint = static_cast<char32_t>(drawn->Codepoint);
                glyph.x0 = drawn->X0;
                glyph.x1 = drawn->X1;
                glyph.y0 = drawn->Y0;
                glyph.y1 = drawn->Y1;
                glyph.advanceX = drawn->AdvanceX;
                glyph.visible = drawn->Visible != 0U;
            }
            result.glyphs.push_back(glyph);
        }
        ImGui::EndFrame();
        result.ok = true;
    }

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
    return result;
}

}  // namespace engine::editor

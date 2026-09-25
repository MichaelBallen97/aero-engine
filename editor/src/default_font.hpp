#pragma once
// Aero Engine — the editor's one UI font, and the Windows-1252 punctuation it can actually draw (task E.4.4's
// macOS validation pass, finding 1).
//
// SRC-PRIVATE and ImGui-FREE by construction: every ImGui call lives in default_font.cpp. That is what lets
// tests/editor/imgui_layer_test.cpp -- ImGui-free at source (2.1.3 D9) -- measure what the font draws
// without including an ImGui header: the blender_process.hpp / viewport_panel.hpp reach, a third time.
//
// THE DEFECT THIS EXISTS FOR. The editor adds no font, so ImGui 1.92.8 draws everything in its embedded
// ProggyClean. ProggyClean carries the Windows-1252 punctuation at the CP1252 code points (0x85 is the
// ellipsis, 0x97 the em dash), NOT at their Unicode code points (U+2026, U+2014) -- ImGui itself configures
// `EllipsisChar = 0x0085` for this font (imgui_draw.cpp:3189, AddFontDefaultBitmap). Every ellipsis and em
// dash a UTF-8 string in this tree carries -- the Issues list's "and N more" line, the footer's elided GUID,
// UNKNOWN_SIZE, the tile caption's ellipsis, and every engine log line with an em dash, which reaches the
// Console -- therefore drew as '?'. The remaps below point each Unicode code point at the glyph the font
// already has, so NO string literal changes.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::editor {

// One remap: a UTF-8 string carries `unicode`; ProggyClean holds that glyph at `cp1252Slot`.
struct DefaultFontRemap {
    char32_t unicode = 0;
    char32_t cp1252Slot = 0;
};

// MEASURED, not transcribed: with every remap applied, each `unicode` resolves to a glyph whose geometry
// (X0/X1/Y0/Y1/AdvanceX/Visible) is IDENTICAL to its `cp1252Slot`'s, at the font's reference size and at
// twice it -- imgui_layer_test.cpp's I230 re-measures all 26 on every run. That is every Windows-1252
// punctuation code point in 0x80-0x9F whose slot the font carries.
//
// An earlier probe reported U+201A, U+201E and U+2021 as "differing in geometry". It was a heap-use-after-
// free in the PROBE, confirmed by ASan: it held an ImFontGlyph* from FindGlyphNoFallback(slot) across a
// FindGlyph(unicode) whose bake push_back'ed into ImFontBaked::Glyphs and reallocated it. Measured by value,
// all three match; measureDefaultFont() below copies every glyph the moment it is looked up for that reason.
inline constexpr std::array<DefaultFontRemap, 26> DEFAULT_FONT_CP1252_REMAPS{{
    {0x201A, 0x82},  // single low-9 quotation mark
    {0x0192, 0x83},  // latin small f with hook
    {0x201E, 0x84},  // double low-9 quotation mark
    {0x2026, 0x85},  // horizontal ellipsis -- the Issues list, the elided GUID, tile captions
    {0x2020, 0x86},  // dagger
    {0x2021, 0x87},  // double dagger
    {0x02C6, 0x88},  // modifier circumflex
    {0x2030, 0x89},  // per mille sign
    {0x0160, 0x8A},  // S with caron
    {0x2039, 0x8B},  // single left angle quotation mark
    {0x0152, 0x8C},  // OE ligature
    {0x017D, 0x8E},  // Z with caron
    {0x2018, 0x91},  // left single quotation mark
    {0x2019, 0x92},  // right single quotation mark
    {0x201C, 0x93},  // left double quotation mark
    {0x201D, 0x94},  // right double quotation mark
    {0x2022, 0x95},  // bullet
    {0x2013, 0x96},  // en dash
    {0x2014, 0x97},  // em dash -- UNKNOWN_SIZE and every engine log line that uses one
    {0x02DC, 0x98},  // small tilde
    {0x2122, 0x99},  // trade mark sign
    {0x0161, 0x9A},  // s with caron
    {0x203A, 0x9B},  // single right angle quotation mark
    {0x0153, 0x9C},  // oe ligature
    {0x017E, 0x9E},  // z with caron
    {0x0178, 0x9F},  // Y with diaeresis
}};

// Windows-1252 punctuation deliberately NOT remapped. U+20AC (the euro sign) is ProggyClean's ONE Unicode
// punctuation glyph -- it draws natively -- while its CP1252 slot 0x80 is ABSENT, so a remap would turn a
// working glyph into '?'. (0x81, 0x8D, 0x8F, 0x90 and 0x9D are unassigned in Windows-1252.)
inline constexpr std::array<char32_t, 1> DEFAULT_FONT_CP1252_NOT_REMAPPED{0x20AC};

// Adds the editor's UI font to the CURRENT ImGui context's atlas and applies every remap above. The font is
// ProggyClean at its reference size of 13 -- EXACTLY the font 1.92.8 chose implicitly for this editor, which
// I230 proves glyph for glyph -- added through AddFontDefaultBitmap(), never AddFontDefault(): the latter is
// a heuristic that returns ProggyForever once the context's expected font size reaches 15, and the remap
// table is measured for ProggyClean alone. Call once, after CreateContext and before the first NewFrame.
// Returns false iff the atlas refused the font.
[[nodiscard]] bool addEditorDefaultFont();

// ---- measurement, for tests -------------------------------------------------------------------------------

enum class DefaultFontSetup : std::uint8_t {
    ImGuiImplicit,        // no font added: ImGui's own first-frame choice, as the editor had before this fix
    BitmapWithoutRemaps,  // ProggyClean added explicitly, with no remap -- the control
    Editor,               // addEditorDefaultFont(), exactly as ImGuiLayer::create calls it
};

// What one code point DRAWS, copied by value the moment it is looked up.
struct DefaultFontGlyph {
    char32_t requested = 0;
    bool found = false;           // the font resolves it WITHOUT falling back (FindGlyphNoFallback)
    char32_t drawnCodepoint = 0;  // FindGlyph(requested)->Codepoint -- '?' when it falls back
    float x0 = 0.0F;
    float x1 = 0.0F;
    float y0 = 0.0F;
    float y1 = 0.0F;
    float advanceX = 0.0F;
    bool visible = false;
};

struct DefaultFontMeasurement {
    bool ok = false;       // false iff the font could not be set up
    std::string fontName;  // the atlas's first source, e.g. "ProggyClean.ttf"
    float referenceSize = 0.0F;
    float bakedSize = 0.0F;                // the size the glyphs below were baked at
    std::vector<DefaultFontGlyph> glyphs;  // one per requested code point, in request order
};

// Builds a PRIVATE ImGui context -- no window, no GPU, no ini or log file; RendererHasTextures makes glyphs
// bake on demand into a CPU-side atlas -- sets its font up per `setup`, runs one frame, and reports what each
// code point draws at `scale` times the font's reference size. Restores whichever context was current,
// so it is safe beside a live EditorApp. TEST SUPPORT: nothing in the editor calls it, and it shares
// addEditorDefaultFont() with ImGuiLayer::create so what a test measures is what the editor draws.
[[nodiscard]] DefaultFontMeasurement measureDefaultFont(DefaultFontSetup setup, std::span<const char32_t> points,
                                                        float scale);

}  // namespace engine::editor

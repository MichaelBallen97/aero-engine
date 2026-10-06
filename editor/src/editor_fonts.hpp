#pragma once
// Aero Engine — the editor's three faces and the icons merged into two of them (task E.6.1, spec D2-D7,
// D22).
//
// SRC-PRIVATE and ImGui-FREE at source: ImFont is only forward-declared, and every ImGui call lives in
// editor_fonts.cpp. That is what lets tests/editor/imgui_layer_test.cpp -- ImGui-free at source (2.1.3 D9)
// -- measure what the faces draw without including an ImGui header.
//
// THE FACES: Body (IBM Plex Sans Regular) is ImGui's default font; Strong (IBM Plex Sans SemiBold) is for
// headings; Mono (IBM Plex Mono Regular) is for log text, paths and numbers and carries NO icons -- an icon
// in log text has no reader. Lucide 1.49.0's icons are MERGED into Body and Strong, AFTER Plex in each merge:
// 1.92 asks a merge list in order, and Lucide maps '-', '0'-'9' and 'a'-'z' to glyphs of zero or icon width,
// so a Lucide-first merge would draw lowercase text as icons or nothing (I277's merge-order arm).
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ImFont;

namespace engine::editor {

struct EditorFonts {
    ImFont* body = nullptr;    // IBM Plex Sans Regular + Lucide -- ImGui's default font (added first)
    ImFont* strong = nullptr;  // IBM Plex Sans SemiBold + Lucide
    ImFont* mono = nullptr;    // IBM Plex Mono Regular, NO icons (an icon in log text has no reader)
};

inline constexpr std::string_view EDITOR_FONT_BODY_NAME = "IBM Plex Sans";
inline constexpr std::string_view EDITOR_FONT_STRONG_NAME = "IBM Plex Sans SemiBold";
inline constexpr std::string_view EDITOR_FONT_MONO_NAME = "IBM Plex Mono";
inline constexpr std::string_view EDITOR_FONT_ICONS_NAME = "Lucide";

// Adds Body (+ Lucide), Strong (+ Lucide), Mono to the CURRENT context's atlas, in that order, and publishes
// the set for editorFonts(). nullopt if any add fails, with `failedFace` naming it for the caller's one ERROR
// (D22). Call after CreateContext and before the first NewFrame.
[[nodiscard]] std::optional<EditorFonts> addEditorFonts(std::string_view& failedFace);
[[nodiscard]] const EditorFonts& editorFonts() noexcept;  // the published set; null before add, after clear
void clearEditorFonts() noexcept;                         // ImGuiLayer: BEFORE every ImGui::DestroyContext()

// ---- measurement, for tests (the E.4.4 font harness, generalised) ---------------------------------------
// SansWithoutIcons is the control: the same faces with both Lucide merges omitted.
enum class EditorFontSetup : std::uint8_t { Editor, SansWithoutIcons };
enum class EditorFontFace : std::uint8_t { Body, Strong, Mono };

// What one code point DRAWS, copied BY VALUE at lookup. Never hold an ImFontGlyph* across another lookup: a
// lookup can bake a glyph, which push_backs into ImFontBaked::Glyphs and may reallocate it.
struct MeasuredGlyph {
    char32_t requested = 0;
    bool found = false;           // FindGlyphNoFallback: the face resolves it without falling back
    char32_t drawnCodepoint = 0;  // FindGlyph(...)->Codepoint: 0xFFFD when it falls back
    float x0 = 0.0F;
    float x1 = 0.0F;
    float y0 = 0.0F;
    float y1 = 0.0F;
    float advanceX = 0.0F;
    bool visible = false;
    // The INK: the first and one-past-last rows of the baked bitmap holding any coverage, in the quad's units
    // (both equal to y0 when nothing is drawn). A quad is sized from the font's glyph BOX, which need not be
    // tight -- every Lucide 1.49.0 box reaches down to the baseline (glyf yMin = 0) whatever its outline
    // does -- so a claim about where a glyph SITS reads these, never y0/y1.
    float inkY0 = 0.0F;
    float inkY1 = 0.0F;
};

struct EditorFontMeasurement {
    bool ok = false;             // false iff the faces could not be set up
    std::string fontName;        // the face's first source Name -- set by this code, never derived by ImGui
    float referenceSize = 0.0F;  // LegacySize (16)
    float bakedSize = 0.0F;      // LegacySize * scale
    float ascent = 0.0F;         // the baked size's Ascent / Descent (the body's, for a merged face)
    float descent = 0.0F;
    char32_t ellipsisChar = 0;
    char32_t fallbackChar = 0;
    std::vector<MeasuredGlyph> glyphs;  // one per requested code point, in request order
};

// A PRIVATE context (no window, no GPU, no ini/log file; RendererHasTextures, so glyphs bake on demand into a
// CPU atlas), the faces set up per `setup` through the SAME per-face helpers addEditorFonts uses, one frame,
// every glyph copied by value; the previous context restored, so it is safe beside a live EditorApp. Never
// publishes into editorFonts(). A code point above IM_UNICODE_CODEPOINT_MAX (0xFFFF in this build) is
// measured as U+FFFD -- what ImTextCharFromUtf8 makes of it in text (imgui.cpp:2706, :2721).
[[nodiscard]] EditorFontMeasurement measureEditorFonts(EditorFontSetup setup, EditorFontFace face,
                                                       std::span<const char32_t> points, float scale);

}  // namespace engine::editor

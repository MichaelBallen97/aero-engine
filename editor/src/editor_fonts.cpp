// Aero Engine — the editor's three faces and the Lucide merges (task E.6.1). The ONLY editor TU that adds
// a font (I286); the rationale for every choice is at the declarations in editor_fonts.hpp.
#include "editor_fonts.hpp"

#include <aero/editor/editor_theme.hpp>

#include "editor_font_data.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

namespace {

// spec D7: where a merged icon sits against the capitals. 1.92.8 places a merged source's glyph at
// ImFloor(GlyphOffset.y * (bakedSize / Sources[0]->SizePixels) + 0.5) + the FIRST source's rounded Ascent
// (imgui_draw.cpp:4816-4826), so Plex and Lucide share one baseline and only this offset moves an icon,
// snapped to whole points at each baked size. At body 16, Plex's cap centre sits 698/2 x 16/1300 = 4.295
// points above the baseline and an 11-point Lucide em centre 500 x 11/1000 = 5.5, a gap of 1.205 -- so 1.0
// (any value in [0.5, 1.5) is one point at scale 1, and 1.0 is two points at scale 2).
// MEASURED by I278 on the INK of all 67 roster icons against 'H' (identical in Body and Strong): mean
// -0.022 and max |delta| 0.5 points at scale 1, mean -0.052 and max 1.0 at scale 2 (= 0.5 of the scale).
// The outline model gives the same deltas, and gives 0.5 a scale-2 mean of -1.05 and 1.5 a scale-1 mean of
// +0.98 -- both fail. Measured on the ink, never on the quad: every Lucide 1.49.0 glyph box reaches down to
// the baseline (glyf yMin = 0), so a quad's centre tracks only an icon's top edge.
constexpr float LUCIDE_GLYPH_OFFSET_Y = 1.0F;

// The published set. Mutable on purpose: ImGuiLayer clears it before every DestroyContext, so it never names
// a font in a destroyed atlas.
EditorFonts publishedFonts{};

// An explicit Name, so a test reads the name this code set rather than whatever ImGui derives for a memory
// font. ImFontConfig's constructor zero-fills it, and the copy is bounded so the terminator always fits.
void setConfigName(ImFontConfig& config, std::string_view name) {
    const std::size_t count = std::min(name.size(), sizeof(config.Name) - 1U);
    std::copy_n(name.data(), count, config.Name);
    config.Name[count] = '\0';
}

ImFont* addFace(ImFontAtlas& atlas, std::span<const std::uint8_t> data, float size, std::string_view name) {
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;  // static storage: the atlas must never free it (default: true)
    setConfigName(config, name);
    // The const_cast is sound because ImGui only READS a buffer it does not own. AddFontFromMemoryTTF takes
    // void* for the owning case alone: imgui.h:3702 and :3810 say the atlas frees the pointer only when
    // FontDataOwnedByAtlas is set, and its one free is gated on exactly that (imgui_draw.cpp:3743-3745);
    // AddFont stores the POINTER, never copying or writing the bytes (imgui_draw.cpp:3262 and the AddFont
    // body); the loader hands stb_truetype `(const unsigned char*)src->FontData` (imgui_draw.cpp:4689,
    // :4696), and stbtt_InitFont takes `const unsigned char*` (imstb_truetype.h:738). Static storage
    // outlives the atlas, which 1.92 requires of a non-owned buffer.
    auto* const bytes = const_cast<std::uint8_t*>(data.data());
    return atlas.AddFontFromMemoryTTF(bytes, static_cast<int>(data.size()), size, &config);
}

// Lucide merged into the font added just before it, at 11/16 of that face. No GlyphMinAdvanceX: every icon
// in the Private Use Area advances one em, so every icon is exactly 11 points wide at body 16. (The
// ASCII-mapped Lucide glyphs advance zero or one em, which is why the merge comes AFTER Plex.)
bool mergeIcons(ImFontAtlas& atlas, float faceSize) {
    ImFontConfig config;
    config.MergeMode = true;
    config.FontDataOwnedByAtlas = false;  // static storage, as above
    // A glyph offset needs a non-zero size (imgui_draw.cpp:3062-3063); the size below is 11.
    config.GlyphOffset = ImVec2(0.0F, LUCIDE_GLYPH_OFFSET_Y);
    setConfigName(config, EDITOR_FONT_ICONS_NAME);
    const std::span<const std::uint8_t> data = embedded::lucideIcons();
    auto* const bytes = const_cast<std::uint8_t*>(data.data());  // read-only, as in addFace
    const float sizePixels = faceSize * EDITOR_THEME.type.iconSizeRatio;
    return atlas.AddFontFromMemoryTTF(bytes, static_cast<int>(data.size()), sizePixels, &config) != nullptr;
}

// Body -> (icons) -> Strong -> (icons) -> Mono: a merge joins the font added immediately before it, so the
// order IS the assignment of icons to faces. Each failure names its face.
bool addAllFaces(ImFontAtlas& atlas, EditorFontSetup setup, EditorFonts& out, std::string_view& failedFace) {
    const bool withIcons = setup == EditorFontSetup::Editor;
    const ThemeTypeScale& type = EDITOR_THEME.type;
    out.body = addFace(atlas, embedded::ibmPlexSansRegular(), type.bodySize, EDITOR_FONT_BODY_NAME);
    if (out.body == nullptr) {
        failedFace = EDITOR_FONT_BODY_NAME;
        return false;
    }
    if (withIcons && !mergeIcons(atlas, type.bodySize)) {
        failedFace = EDITOR_FONT_ICONS_NAME;
        return false;
    }
    out.strong = addFace(atlas, embedded::ibmPlexSansSemiBold(), type.strongSize, EDITOR_FONT_STRONG_NAME);
    if (out.strong == nullptr) {
        failedFace = EDITOR_FONT_STRONG_NAME;
        return false;
    }
    if (withIcons && !mergeIcons(atlas, type.strongSize)) {
        failedFace = EDITOR_FONT_ICONS_NAME;
        return false;
    }
    out.mono = addFace(atlas, embedded::ibmPlexMonoRegular(), type.monoSize, EDITOR_FONT_MONO_NAME);
    if (out.mono == nullptr) {
        failedFace = EDITOR_FONT_MONO_NAME;
        return false;
    }
    return true;
}

// The rows of a baked glyph's bitmap that hold any coverage, mapped onto its quad (MeasuredGlyph::inkY0 and
// inkY1). GetCustomRect resolves a pack id against the CURRENT texture, so this runs after every lookup.
void measureInk(const ImFontAtlas& atlas, int packId, MeasuredGlyph& glyph) {
    glyph.inkY0 = glyph.y0;
    glyph.inkY1 = glyph.y0;
    ImFontAtlasRect rect;
    const ImTextureData* const texture = atlas.TexData;
    if (!glyph.visible || texture == nullptr || texture->Pixels == nullptr) {
        return;
    }
    if (!atlas.GetCustomRect(packId, &rect) || rect.w == 0 || rect.h == 0) {
        return;
    }
    const auto bytesPerPixel = static_cast<std::size_t>(texture->BytesPerPixel);
    const std::size_t alphaByte = texture->Format == ImTextureFormat_RGBA32 ? 3U : 0U;
    const auto pitch = static_cast<std::size_t>(texture->Width) * bytesPerPixel;
    int first = -1;
    int last = -1;
    for (int row = 0; row < rect.h; ++row) {
        const std::size_t rowStart = (static_cast<std::size_t>(rect.y + row) * pitch) +
                                     (static_cast<std::size_t>(rect.x) * bytesPerPixel) + alphaByte;
        for (int column = 0; column < rect.w; ++column) {
            if (texture->Pixels[rowStart + (static_cast<std::size_t>(column) * bytesPerPixel)] != 0U) {
                first = first < 0 ? row : first;
                last = row;
                break;
            }
        }
    }
    if (first < 0) {
        return;
    }
    const float rowHeight = (glyph.y1 - glyph.y0) / static_cast<float>(rect.h);
    glyph.inkY0 = glyph.y0 + (static_cast<float>(first) * rowHeight);
    glyph.inkY1 = glyph.y0 + (static_cast<float>(last + 1) * rowHeight);
}

[[nodiscard]] ImFont* fontFor(const EditorFonts& fonts, EditorFontFace face) noexcept {
    switch (face) {  // NO default: a new face without an arm is a clang-diagnostic-switch error (CI lint)
        case EditorFontFace::Body:
            return fonts.body;
        case EditorFontFace::Strong:
            return fonts.strong;
        case EditorFontFace::Mono:
            return fonts.mono;
    }
    return nullptr;
}

// What text does with a code point this build's 16-bit ImWchar cannot hold: U+FFFD.
[[nodiscard]] ImWchar asImWchar(char32_t codepoint) noexcept {
    constexpr auto INVALID = static_cast<char32_t>(IM_UNICODE_CODEPOINT_INVALID);
    return static_cast<ImWchar>(codepoint <= IM_UNICODE_CODEPOINT_MAX ? codepoint : INVALID);
}

}  // namespace

std::optional<EditorFonts> addEditorFonts(std::string_view& failedFace) {
    EditorFonts fonts;
    if (!addAllFaces(*ImGui::GetIO().Fonts, EditorFontSetup::Editor, fonts, failedFace)) {
        return std::nullopt;
    }
    publishedFonts = fonts;
    return fonts;
}

const EditorFonts& editorFonts() noexcept { return publishedFonts; }

void clearEditorFonts() noexcept { publishedFonts = EditorFonts{}; }

EditorFontMeasurement measureEditorFonts(EditorFontSetup setup, EditorFontFace face,  // one face, measured
                                         std::span<const char32_t> points, float scale) {
    EditorFontMeasurement result;
    ImGuiContext* const previous = ImGui::GetCurrentContext();
    ImGuiContext* const context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);  // CreateContext restores the previous one when there was one

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // NEVER write imgui.ini (or a log) into whatever the working directory is
    io.LogFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;  // glyphs bake on demand, CPU-side
    io.DisplaySize = ImVec2(64.0F, 64.0F);

    EditorFonts fonts;
    std::string_view failedFace;
    ImFont* const font = addAllFaces(*io.Fonts, setup, fonts, failedFace) ? fontFor(fonts, face) : nullptr;
    if (font != nullptr) {
        ImGui::NewFrame();
        result.fontName = font->Sources.empty() ? std::string() : std::string(font->Sources[0]->Name);
        result.referenceSize = font->LegacySize;
        result.bakedSize = font->LegacySize * scale;
        ImFontBaked* const baked = font->GetFontBaked(result.bakedSize);
        result.ascent = baked->Ascent;
        result.descent = baked->Descent;
        result.ellipsisChar = static_cast<char32_t>(font->EllipsisChar);
        result.fallbackChar = static_cast<char32_t>(font->FallbackChar);
        result.glyphs.reserve(points.size());
        std::vector<int> packIds;
        packIds.reserve(points.size());
        for (const char32_t codepoint : points) {
            MeasuredGlyph glyph;
            glyph.requested = codepoint;
            const ImWchar c = asImWchar(codepoint);
            // BY VALUE, immediately: a later lookup may bake a glyph into ImFontBaked::Glyphs and reallocate
            // it, which dangles any ImFontGlyph* held across it.
            glyph.found = baked->FindGlyphNoFallback(c) != nullptr;
            const ImFontGlyph* const drawn = baked->FindGlyph(c);
            glyph.drawnCodepoint = static_cast<char32_t>(drawn->Codepoint);
            glyph.x0 = drawn->X0;
            glyph.x1 = drawn->X1;
            glyph.y0 = drawn->Y0;
            glyph.y1 = drawn->Y1;
            glyph.advanceX = drawn->AdvanceX;
            glyph.visible = drawn->Visible != 0U;
            packIds.push_back(drawn->PackId);
            result.glyphs.push_back(glyph);
        }
        for (std::size_t i = 0; i < result.glyphs.size(); ++i) {
            measureInk(*io.Fonts, packIds[i], result.glyphs[i]);
        }
        ImGui::EndFrame();
        result.ok = true;
    }

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
    return result;
}

}  // namespace engine::editor

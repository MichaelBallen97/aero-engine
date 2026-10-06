#pragma once
// Aero Engine -- the editor's icon roster (task E.6.1, spec D3/D19). PUBLIC and PURE: <array> and
// <string_view> only, no ImGui.
//
// Each icon is a Lucide 1.49.0 glyph (editor/third_party/fonts/lucide/), named by its UPSTREAM Lucide
// name -- AERO_ICON_ + the name in upper snake case -- so the roster is a vocabulary of GLYPHS and what
// a glyph MEANS is the consumer's own table (createKindIcon is the first). Icons draw ONLY in the Body
// and Strong faces, which carry the merged icon font; Mono does not, and draws U+FFFD for them. Each
// macro is a string literal of hex escapes under the same policy as editor_glyphs.hpp. A new icon is
// one macro and one table row: GL2, GL3 and GL4 check the bytes, the names and the upstream code point.
#include <array>
#include <string_view>

// ---- Tools (E.6.2) ----
#define AERO_ICON_MOUSE_POINTER_2 "\xEE\x87\x83"  // U+E1C3 mouse-pointer-2
#define AERO_ICON_MOVE "\xEE\x84\xA1"             // U+E121 move
#define AERO_ICON_ROTATE_CW "\xEE\x85\x89"        // U+E149 rotate-cw
#define AERO_ICON_SCALING "\xEE\x8B\xAC"          // U+E2EC scaling
#define AERO_ICON_MAGNET "\xEE\x8A\xB5"           // U+E2B5 magnet
#define AERO_ICON_GLOBE "\xEE\x83\xA8"            // U+E0E8 globe
#define AERO_ICON_PLAY "\xEE\x84\xBC"             // U+E13C play
#define AERO_ICON_PAUSE "\xEE\x84\xAE"            // U+E12E pause
#define AERO_ICON_STEP_FORWARD "\xEE\x8F\xAA"     // U+E3EA step-forward
#define AERO_ICON_UNDO_2 "\xEE\x8A\xA1"           // U+E2A1 undo-2
#define AERO_ICON_REDO_2 "\xEE\x8A\xA0"           // U+E2A0 redo-2
// ---- Panels ----
#define AERO_ICON_APP_WINDOW "\xEE\x90\xA6"          // U+E426 app-window
#define AERO_ICON_LIST_TREE "\xEE\x90\x88"           // U+E408 list-tree
#define AERO_ICON_SLIDERS_HORIZONTAL "\xEE\x8A\x9A"  // U+E29A sliders-horizontal
#define AERO_ICON_SQUARE_TERMINAL "\xEE\x88\x8A"     // U+E20A square-terminal
// ---- Entities and components ----
#define AERO_ICON_CIRCLE_DASHED "\xEE\x92\xB0"  // U+E4B0 circle-dashed
#define AERO_ICON_BOX "\xEE\x81\xA1"            // U+E061 box
#define AERO_ICON_CIRCLE "\xEE\x81\xB6"         // U+E076 circle
#define AERO_ICON_SQUARE "\xEE\x85\xA7"         // U+E167 square
#define AERO_ICON_SHAPES "\xEE\x92\xB3"         // U+E4B3 shapes
#define AERO_ICON_SUN "\xEE\x85\xB8"            // U+E178 sun
#define AERO_ICON_LIGHTBULB "\xEE\x87\x82"      // U+E1C2 lightbulb
#define AERO_ICON_LAMP_CEILING "\xEE\x8B\x99"   // U+E2D9 lamp-ceiling
#define AERO_ICON_CAMERA "\xEE\x81\xA4"         // U+E064 camera
#define AERO_ICON_AXIS_3D "\xEE\x8B\xBE"        // U+E2FE axis-3d
#define AERO_ICON_CLOUD_SUN "\xEE\x88\x96"      // U+E216 cloud-sun
#define AERO_ICON_VOLUME_2 "\xEE\x86\xAB"       // U+E1AB volume-2
#define AERO_ICON_EAR "\xEE\x8E\x82"            // U+E382 ear
#define AERO_ICON_FILM "\xEE\x83\x90"           // U+E0D0 film
// ---- Assets ----
#define AERO_ICON_FOLDER "\xEE\x83\x97"        // U+E0D7 folder
#define AERO_ICON_FOLDER_OPEN "\xEE\x89\x87"   // U+E247 folder-open
#define AERO_ICON_FOLDER_PLUS "\xEE\x83\x99"   // U+E0D9 folder-plus
#define AERO_ICON_FILE "\xEE\x83\x80"          // U+E0C0 file
#define AERO_ICON_FILE_PLUS "\xEE\x83\x89"     // U+E0C9 file-plus
#define AERO_ICON_FILE_TEXT "\xEE\x83\x8C"     // U+E0CC file-text
#define AERO_ICON_IMAGE "\xEE\x83\xB6"         // U+E0F6 image
#define AERO_ICON_BOXES "\xEE\x8B\x90"         // U+E2D0 boxes
#define AERO_ICON_PALETTE "\xEE\x87\x9D"       // U+E1DD palette
#define AERO_ICON_MUSIC "\xEE\x84\xA2"         // U+E122 music
#define AERO_ICON_CLAPPERBOARD "\xEE\x8A\x9B"  // U+E29B clapperboard
// ---- Actions ----
#define AERO_ICON_SEARCH "\xEE\x85\x91"         // U+E151 search
#define AERO_ICON_X "\xEE\x86\xB2"              // U+E1B2 x
#define AERO_ICON_PLUS "\xEE\x84\xBD"           // U+E13D plus
#define AERO_ICON_TRASH_2 "\xEE\x86\x8E"        // U+E18E trash-2
#define AERO_ICON_PENCIL "\xEE\x87\xB9"         // U+E1F9 pencil
#define AERO_ICON_COPY "\xEE\x82\x9E"           // U+E09E copy
#define AERO_ICON_REFRESH_CW "\xEE\x85\x85"     // U+E145 refresh-cw
#define AERO_ICON_SAVE "\xEE\x85\x8D"           // U+E14D save
#define AERO_ICON_EXTERNAL_LINK "\xEE\x82\xB9"  // U+E0B9 external-link
#define AERO_ICON_CHECK "\xEE\x81\xAC"          // U+E06C check
#define AERO_ICON_SETTINGS "\xEE\x85\x94"       // U+E154 settings
// ---- Navigation and view ----
#define AERO_ICON_ELLIPSIS "\xEE\x82\xB6"        // U+E0B6 ellipsis
#define AERO_ICON_CHEVRON_RIGHT "\xEE\x81\xAF"   // U+E06F chevron-right
#define AERO_ICON_CHEVRON_DOWN "\xEE\x81\xAD"    // U+E06D chevron-down
#define AERO_ICON_CORNER_LEFT_UP "\xEE\x82\xA4"  // U+E0A4 corner-left-up
#define AERO_ICON_ARROW_LEFT "\xEE\x81\x88"      // U+E048 arrow-left
#define AERO_ICON_EYE "\xEE\x82\xBA"             // U+E0BA eye
#define AERO_ICON_EYE_OFF "\xEE\x82\xBB"         // U+E0BB eye-off
#define AERO_ICON_LOCK "\xEE\x84\x8B"            // U+E10B lock
#define AERO_ICON_LOCK_OPEN "\xEE\x84\x8C"       // U+E10C lock-open
#define AERO_ICON_GRID_3X3 "\xEE\x83\xA9"        // U+E0E9 grid-3x3
#define AERO_ICON_LIST "\xEE\x84\x86"            // U+E106 list
#define AERO_ICON_LAYOUT_GRID "\xEE\x83\xBF"     // U+E0FF layout-grid
// ---- Status ----
#define AERO_ICON_TRIANGLE_ALERT "\xEE\x86\x93"  // U+E193 triangle-alert
#define AERO_ICON_CIRCLE_X "\xEE\x82\x84"        // U+E084 circle-x
#define AERO_ICON_INFO "\xEE\x83\xB9"            // U+E0F9 info
#define AERO_ICON_BUG "\xEE\x88\x8C"             // U+E20C bug

namespace engine::editor {

// One row per line, in macro order, BUILT FROM the macro, with the code point spelled 0x + four upper-case
// hex digits: GL3 reads these rows as text.
struct EditorIcon {
    std::string_view name;   // the upstream Lucide name: "box", "folder-open", ...
    char32_t codepoint = 0;  // an integer literal, in the Private Use Area
    std::string_view utf8;   // the macro
};

inline constexpr std::array<EditorIcon, 67> EDITOR_ICONS{{
    {"mouse-pointer-2", 0xE1C3, AERO_ICON_MOUSE_POINTER_2},
    {"move", 0xE121, AERO_ICON_MOVE},
    {"rotate-cw", 0xE149, AERO_ICON_ROTATE_CW},
    {"scaling", 0xE2EC, AERO_ICON_SCALING},
    {"magnet", 0xE2B5, AERO_ICON_MAGNET},
    {"globe", 0xE0E8, AERO_ICON_GLOBE},
    {"play", 0xE13C, AERO_ICON_PLAY},
    {"pause", 0xE12E, AERO_ICON_PAUSE},
    {"step-forward", 0xE3EA, AERO_ICON_STEP_FORWARD},
    {"undo-2", 0xE2A1, AERO_ICON_UNDO_2},
    {"redo-2", 0xE2A0, AERO_ICON_REDO_2},
    {"app-window", 0xE426, AERO_ICON_APP_WINDOW},
    {"list-tree", 0xE408, AERO_ICON_LIST_TREE},
    {"sliders-horizontal", 0xE29A, AERO_ICON_SLIDERS_HORIZONTAL},
    {"square-terminal", 0xE20A, AERO_ICON_SQUARE_TERMINAL},
    {"circle-dashed", 0xE4B0, AERO_ICON_CIRCLE_DASHED},
    {"box", 0xE061, AERO_ICON_BOX},
    {"circle", 0xE076, AERO_ICON_CIRCLE},
    {"square", 0xE167, AERO_ICON_SQUARE},
    {"shapes", 0xE4B3, AERO_ICON_SHAPES},
    {"sun", 0xE178, AERO_ICON_SUN},
    {"lightbulb", 0xE1C2, AERO_ICON_LIGHTBULB},
    {"lamp-ceiling", 0xE2D9, AERO_ICON_LAMP_CEILING},
    {"camera", 0xE064, AERO_ICON_CAMERA},
    {"axis-3d", 0xE2FE, AERO_ICON_AXIS_3D},
    {"cloud-sun", 0xE216, AERO_ICON_CLOUD_SUN},
    {"volume-2", 0xE1AB, AERO_ICON_VOLUME_2},
    {"ear", 0xE382, AERO_ICON_EAR},
    {"film", 0xE0D0, AERO_ICON_FILM},
    {"folder", 0xE0D7, AERO_ICON_FOLDER},
    {"folder-open", 0xE247, AERO_ICON_FOLDER_OPEN},
    {"folder-plus", 0xE0D9, AERO_ICON_FOLDER_PLUS},
    {"file", 0xE0C0, AERO_ICON_FILE},
    {"file-plus", 0xE0C9, AERO_ICON_FILE_PLUS},
    {"file-text", 0xE0CC, AERO_ICON_FILE_TEXT},
    {"image", 0xE0F6, AERO_ICON_IMAGE},
    {"boxes", 0xE2D0, AERO_ICON_BOXES},
    {"palette", 0xE1DD, AERO_ICON_PALETTE},
    {"music", 0xE122, AERO_ICON_MUSIC},
    {"clapperboard", 0xE29B, AERO_ICON_CLAPPERBOARD},
    {"search", 0xE151, AERO_ICON_SEARCH},
    {"x", 0xE1B2, AERO_ICON_X},
    {"plus", 0xE13D, AERO_ICON_PLUS},
    {"trash-2", 0xE18E, AERO_ICON_TRASH_2},
    {"pencil", 0xE1F9, AERO_ICON_PENCIL},
    {"copy", 0xE09E, AERO_ICON_COPY},
    {"refresh-cw", 0xE145, AERO_ICON_REFRESH_CW},
    {"save", 0xE14D, AERO_ICON_SAVE},
    {"external-link", 0xE0B9, AERO_ICON_EXTERNAL_LINK},
    {"check", 0xE06C, AERO_ICON_CHECK},
    {"settings", 0xE154, AERO_ICON_SETTINGS},
    {"ellipsis", 0xE0B6, AERO_ICON_ELLIPSIS},
    {"chevron-right", 0xE06F, AERO_ICON_CHEVRON_RIGHT},
    {"chevron-down", 0xE06D, AERO_ICON_CHEVRON_DOWN},
    {"corner-left-up", 0xE0A4, AERO_ICON_CORNER_LEFT_UP},
    {"arrow-left", 0xE048, AERO_ICON_ARROW_LEFT},
    {"eye", 0xE0BA, AERO_ICON_EYE},
    {"eye-off", 0xE0BB, AERO_ICON_EYE_OFF},
    {"lock", 0xE10B, AERO_ICON_LOCK},
    {"lock-open", 0xE10C, AERO_ICON_LOCK_OPEN},
    {"grid-3x3", 0xE0E9, AERO_ICON_GRID_3X3},
    {"list", 0xE106, AERO_ICON_LIST},
    {"layout-grid", 0xE0FF, AERO_ICON_LAYOUT_GRID},
    {"triangle-alert", 0xE193, AERO_ICON_TRIANGLE_ALERT},
    {"circle-x", 0xE084, AERO_ICON_CIRCLE_X},
    {"info", 0xE0F9, AERO_ICON_INFO},
    {"bug", 0xE20C, AERO_ICON_BUG},
}};

}  // namespace engine::editor

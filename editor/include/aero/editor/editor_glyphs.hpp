#pragma once
// Aero Engine -- the editor's typographic glyphs (task E.6.1, spec D19). PUBLIC and PURE: <array> and
// <string_view> only, no ImGui.
//
// THE STRING-LITERAL POLICY, in four sentences. (1) Editor source stays 7-bit in every string and
// character literal: MSVC builds this tree without /utf-8, so it reads a BOM-less file as CP1252, and a
// raw non-ASCII byte survives that round trip only by accident. (2) A non-ASCII character in an editor
// string is spelled ONLY through a macro -- AERO_GLYPH_* here, AERO_ICON_* in editor_icons.hpp. (3) Each
// macro is a STRING LITERAL OF HEX ESCAPES, so adjacent-literal concatenation works
// (AERO_GLYPH_ELLIPSIS "and "), and an escape is NEVER spliced into a longer literal, because a hex escape
// is greedy: "\xA6and" would read \xA6a. (4) Every glyph here is drawn natively by Plex Sans AND Plex
// Mono; a mark Plex lacks (the command, shift, option and return symbols among them) is not added here.
// GL6 enforces the policy: a tier-0 lexer over every editor source and header (.claude/rules/editor.md).
#include <array>
#include <string_view>

#define AERO_GLYPH_ELLIPSIS "\xE2\x80\xA6"     // U+2026 horizontal ellipsis
#define AERO_GLYPH_EM_DASH "\xE2\x80\x94"      // U+2014 em dash
#define AERO_GLYPH_EN_DASH "\xE2\x80\x93"      // U+2013 en dash
#define AERO_GLYPH_MIDDLE_DOT "\xC2\xB7"       // U+00B7 middle dot
#define AERO_GLYPH_MULTIPLY "\xC3\x97"         // U+00D7 multiplication sign
#define AERO_GLYPH_DEGREE "\xC2\xB0"           // U+00B0 degree sign
#define AERO_GLYPH_BULLET "\xE2\x80\xA2"       // U+2022 bullet
#define AERO_GLYPH_ARROW_LEFT "\xE2\x86\x90"   // U+2190 leftwards arrow (0x90 is undefined in CP1252)
#define AERO_GLYPH_ARROW_UP "\xE2\x86\x91"     // U+2191 upwards arrow
#define AERO_GLYPH_ARROW_RIGHT "\xE2\x86\x92"  // U+2192 rightwards arrow
#define AERO_GLYPH_ARROW_DOWN "\xE2\x86\x93"   // U+2193 downwards arrow
#define AERO_GLYPH_CHECK "\xE2\x9C\x93"        // U+2713 check mark
#define AERO_GLYPH_MINUS "\xE2\x88\x92"        // U+2212 minus sign

namespace engine::editor {

// One row per macro, in macro order, BUILT FROM the macro, so a typo in the bytes and a typo in the code
// point disagree and GL1 reports it.
struct EditorGlyph {
    std::string_view name;   // lower-case, hyphenated: "ellipsis", "em-dash", ...
    char32_t codepoint = 0;  // an integer literal -- a universal-character-name escape is outside the policy
    std::string_view utf8;   // the macro
};

inline constexpr std::array<EditorGlyph, 13> EDITOR_GLYPHS{{
    {"ellipsis", 0x2026, AERO_GLYPH_ELLIPSIS},
    {"em-dash", 0x2014, AERO_GLYPH_EM_DASH},
    {"en-dash", 0x2013, AERO_GLYPH_EN_DASH},
    {"middle-dot", 0x00B7, AERO_GLYPH_MIDDLE_DOT},
    {"multiply", 0x00D7, AERO_GLYPH_MULTIPLY},
    {"degree", 0x00B0, AERO_GLYPH_DEGREE},
    {"bullet", 0x2022, AERO_GLYPH_BULLET},
    {"arrow-left", 0x2190, AERO_GLYPH_ARROW_LEFT},
    {"arrow-up", 0x2191, AERO_GLYPH_ARROW_UP},
    {"arrow-right", 0x2192, AERO_GLYPH_ARROW_RIGHT},
    {"arrow-down", 0x2193, AERO_GLYPH_ARROW_DOWN},
    {"check", 0x2713, AERO_GLYPH_CHECK},
    {"minus", 0x2212, AERO_GLYPH_MINUS},
}};

}  // namespace engine::editor

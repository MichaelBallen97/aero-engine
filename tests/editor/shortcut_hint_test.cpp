// tests/editor/shortcut_hint_test.cpp -- task E.6.2: the one chord spelling (HK1-HK3). A TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp. Tier 0: the host is an argument, so the
// three spellings are asserted from one process. No #if of any kind.
#include <aero/editor/shortcut_hint.hpp>

#include <doctest/doctest.h>

#include <array>
#include <ostream>  // MSVC: a CHECK over a std::string_view needs the complete std::ostream (the 0.4.1 trap)
#include <string>
#include <string_view>

using engine::editor::chordHint;
using engine::editor::ChordModifiers;
using engine::editor::HostOs;
using engine::editor::modifierName;

namespace {

constexpr ChordModifiers CTRL{.ctrl = true};
constexpr ChordModifiers CTRL_SHIFT{.ctrl = true, .shift = true};

}  // namespace

TEST_CASE("shortcut hint: every chord the editor binds reads Cmd on macOS (task E.6.2, HK1)") {
    constexpr HostOs MAC = HostOs::MacOs;
    CHECK(chordHint(MAC, CTRL, "Z") == "Cmd+Z");
    CHECK(chordHint(MAC, CTRL_SHIFT, "Z") == "Cmd+Shift+Z");
    CHECK(chordHint(MAC, CTRL, "N") == "Cmd+N");
    CHECK(chordHint(MAC, CTRL, "O") == "Cmd+O");
    CHECK(chordHint(MAC, CTRL, "S") == "Cmd+S");
    CHECK(chordHint(MAC, CTRL_SHIFT, "S") == "Cmd+Shift+S");
    CHECK(chordHint(MAC, CTRL, "Q") == "Cmd+Q");
    CHECK(chordHint(MAC, CTRL, "D") == "Cmd+D");
    CHECK(modifierName(MAC) == "Cmd");
}

TEST_CASE("shortcut hint: Windows and Linux read Ctrl (task E.6.2, HK2)") {
    for (const HostOs os : {HostOs::Windows, HostOs::Linux}) {
        CAPTURE(static_cast<int>(os));
        CHECK(chordHint(os, CTRL, "Z") == "Ctrl+Z");
        CHECK(chordHint(os, CTRL_SHIFT, "Z") == "Ctrl+Shift+Z");
        CHECK(chordHint(os, CTRL, "N") == "Ctrl+N");
        CHECK(chordHint(os, CTRL, "O") == "Ctrl+O");
        CHECK(chordHint(os, CTRL, "S") == "Ctrl+S");
        CHECK(chordHint(os, CTRL_SHIFT, "S") == "Ctrl+Shift+S");
        CHECK(chordHint(os, CTRL, "Q") == "Ctrl+Q");
        CHECK(chordHint(os, CTRL, "D") == "Ctrl+D");
        CHECK(modifierName(os) == "Ctrl");
    }
}

TEST_CASE("shortcut hint: 7-bit, a fixed order, and no modifier passes the key through (task E.6.2, HK3)") {
    for (const HostOs os : {HostOs::Windows, HostOs::MacOs, HostOs::Linux}) {
        for (const ChordModifiers mods : {ChordModifiers{}, CTRL, CTRL_SHIFT, ChordModifiers{.shift = true}}) {
            const std::string hint = chordHint(os, mods, "Z");
            CAPTURE(hint);
            for (const char c : hint) {
                CHECK(static_cast<unsigned char>(c) < 0x80U);  // Plex has no command or shift glyph
            }
            CHECK(hint.ends_with("Z"));
            if (mods.ctrl && mods.shift) {
                CHECK(hint.find(std::string(modifierName(os)) + "+") < hint.find("Shift+"));  // command first
            }
        }
    }
    CHECK(chordHint(HostOs::MacOs, ChordModifiers{}, "F2") == "F2");
    CHECK(chordHint(HostOs::Windows, ChordModifiers{.shift = true}, "Z") == "Shift+Z");
}

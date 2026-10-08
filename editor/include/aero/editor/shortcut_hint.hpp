#pragma once
// Aero Engine — the ONE spelling of a chord or a modifier for DISPLAY (task E.6.2, D10). PUBLIC and PURE: no
// ImGui, no SDL, no platform branch -- the host is an argument, so all three spellings are asserted from one test
// process (HK1-HK3). ImGuiMod_Ctrl binds Cmd on macOS (ImGui swaps the two at the event layer), and every hint the
// editor draws for such a chord comes from here: `Cmd+Z` on macOS, `Ctrl+Z` on Windows and Linux. ASCII only --
// Plex draws no command or shift symbol. This file's .cpp is the ONLY editor source whose string literals name
// either modifier (HK4 sweeps every editor literal for that).
#include <aero/editor/blender_tool.hpp>  // HostOs and currentHostOs() -- the editor's one host seam

#include <string>
#include <string_view>

namespace engine::editor {

// The modifiers of a chord bound through ImGui::Shortcut. `ctrl` is ImGuiMod_Ctrl -- Cmd on macOS.
struct ChordModifiers {
    bool ctrl = false;
    bool shift = false;
};

// `key` behind its modifiers in a fixed order -- the command modifier, then Shift -- each followed by '+';
// the key alone when there are none ("F2" stays "F2"). A call site passes currentHostOs(), never a literal host.
[[nodiscard]] std::string chordHint(HostOs os, ChordModifiers mods, std::string_view key);

// The command modifier's name for PROSE ("hold Cmd to invert"): "Cmd" on macOS, "Ctrl" elsewhere.
[[nodiscard]] std::string_view modifierName(HostOs os) noexcept;

}  // namespace engine::editor

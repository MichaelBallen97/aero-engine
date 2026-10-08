// Aero Engine — the one chord spelling (task E.6.2, D10). The ONLY editor TU whose literals name a modifier.
#include <aero/editor/shortcut_hint.hpp>

namespace engine::editor {

std::string_view modifierName(HostOs os) noexcept {
    switch (os) {  // NO default: -- a fourth host is a clang-diagnostic-switch failure on the lint lane
        case HostOs::MacOs:
            return "Cmd";
        case HostOs::Windows:
        case HostOs::Linux:
            return "Ctrl";
    }
    return "Ctrl";
}

std::string chordHint(HostOs os, ChordModifiers mods, std::string_view key) {
    std::string hint;
    if (mods.ctrl) {
        hint += modifierName(os);
        hint += '+';
    }
    if (mods.shift) {
        hint += "Shift+";
    }
    hint += key;
    return hint;
}

}  // namespace engine::editor

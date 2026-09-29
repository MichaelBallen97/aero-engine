// editor/src/create_menu_ui.cpp -- task E.5.2: the Create menu's ONE ImGui TU. It walks createMenuEntries()
// and names no CreateKind enumerator, so adding a kind is a table row, never an edit here (the
// asset_picker.cpp posture, I166(c)).
#include "create_menu_ui.hpp"

#include <aero/editor/create_menu.hpp>

#include <cstddef>
#include <imgui.h>
#include <optional>
#include <span>

namespace engine::editor {

std::optional<CreateKind> drawCreateMenuItems(bool enabled) {
    std::optional<CreateKind> chosen;
    const std::span<const CreateMenuEntry> entries = createMenuEntries();
    std::size_t i = 0;
    while (i < entries.size()) {
        const CreateMenuGroup group = entries[i].group;
        std::size_t runEnd = i;
        while (runEnd < entries.size() && entries[runEnd].group == group) {
            ++runEnd;
        }
        const char* const groupLabel = createMenuGroupLabel(group);
        if (groupLabel[0] == '\0') {
            // TopLevel: the entries sit directly in the caller's menu.
            for (std::size_t k = i; k < runEnd; ++k) {
                if (ImGui::MenuItem(createKindLabel(entries[k].kind), nullptr, false, enabled)) {
                    chosen = entries[k].kind;
                }
            }
        } else if (ImGui::BeginMenu(groupLabel, enabled)) {  // EndMenu ONLY when BeginMenu returned true
            for (std::size_t k = i; k < runEnd; ++k) {
                if (ImGui::MenuItem(createKindLabel(entries[k].kind), nullptr, false, enabled)) {
                    chosen = entries[k].kind;
                }
            }
            ImGui::EndMenu();
        }
        i = runEnd;
    }
    return chosen;
}

}  // namespace engine::editor

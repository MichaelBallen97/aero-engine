// editor/src/create_menu_ui.cpp -- task E.5.2: the Create menu's ONE ImGui TU. It walks createMenuEntries()
// and names no CreateKind enumerator, so adding a kind is a table row, never an edit here (the
// asset_picker.cpp posture, I166(c)).
#include "create_menu_ui.hpp"

#include <aero/editor/create_menu.hpp>

#include <cstddef>
#include <imgui.h>
#include <imgui_internal.h>  // task E.6.1: MenuItemEx / BeginMenuEx (imgui_internal.h:3598-3599) -- the third
                             // editor TU to include it, after shell_ui.cpp and viewport_panel.cpp
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
                const CreateKind kind = entries[k].kind;
                if (ImGui::MenuItemEx(createKindLabel(kind), createKindIcon(kind), nullptr, false, enabled)) {
                    chosen = kind;
                }
            }
        } else if (ImGui::BeginMenuEx(groupLabel, createMenuGroupIcon(group), enabled)) {
            for (std::size_t k = i; k < runEnd; ++k) {
                const CreateKind kind = entries[k].kind;
                if (ImGui::MenuItemEx(createKindLabel(kind), createKindIcon(kind), nullptr, false, enabled)) {
                    chosen = kind;
                }
            }
            ImGui::EndMenu();  // ONLY when BeginMenuEx returned true
        }
        i = runEnd;
    }
    return chosen;
}

bool drawCreateKindItem(const char* label, CreateKind kind, bool enabled) {
    return ImGui::MenuItemEx(label, createKindIcon(kind), nullptr, false, enabled);
}

}  // namespace engine::editor

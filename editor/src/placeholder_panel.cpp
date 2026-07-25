// Aero Engine — PlaceholderPanel (task 2.1.3, D8): the five 2.2.x stand-ins. title()/options() are
// NOT overridden — the Panel defaults (title() == id(), every PanelOptions flag false) are exactly
// right for a one-line placeholder.
#include "placeholder_panel.hpp"

#include <imgui.h>

namespace engine::editor {

PlaceholderPanel::PlaceholderPanel(const char* panelId, DockSlot dockSlot, const char* note) noexcept
    : panelId(panelId), dockSlot(dockSlot), note(note) {}

const char* PlaceholderPanel::id() const noexcept { return panelId; }

DockSlot PlaceholderPanel::defaultDockSlot() const noexcept { return dockSlot; }

void PlaceholderPanel::onDraw() { ImGui::TextUnformatted(note); }

}  // namespace engine::editor

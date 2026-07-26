#pragma once
// Aero Engine — src-private (F14/D9): the five 2.2.x stand-ins share this ONE class, parameterised
// by id/slot/note. Registered by EditorApp::create() (D8); replaced one at a time as 2.2.x lands the
// real panel content.
#include <aero/editor/panel.hpp>

namespace engine::editor {

class PlaceholderPanel final : public Panel {
public:
    // panelId/note MUST be string literals (or otherwise outlive this object) — D6/E19.
    PlaceholderPanel(const char* panelId, DockSlot dockSlot, const char* note) noexcept;

    [[nodiscard]] const char* id() const noexcept override;
    [[nodiscard]] DockSlot defaultDockSlot() const noexcept override;
    void onDraw(PanelContext& context) override;

private:
    const char* panelId;
    DockSlot dockSlot;
    const char* note;
};

}  // namespace engine::editor

#pragma once
// Aero Engine — the editor's panel interface (task 2.1.3). ImGui-FREE BY RULE: this header is the
// contract every 2.2.x panel implements and it must stay includable from ImGui-free TUs (the
// registry, EditorApp, the tier-0 shell test). ImGui appears only in the .cpp implementing a panel
// and in shell_ui.cpp, which owns the Begin/End around onDraw().

#include <cstdint>

namespace engine::editor {

// Where a panel asks to live in the first-run / reset dock layout (D3). The shell maps each USED
// slot to one DockBuilder node; panels sharing a slot become tabs in it. A slot no registered panel
// asks for is never split (an empty ImGui dock node renders as a dead rectangle — F10).
// Explicit uint8_t underlying type: performance-enum-size, like every engine enum.
enum class DockSlot : std::uint8_t {
    Center = 0,  // the big middle node — the viewport's home
    Left,
    Right,
    Bottom,
};

// Per-panel window preferences, translated to ImGui window flags / style vars by shell_ui.cpp.
// NAMED BOOLEANS, never a raw ImGui flag int — an ImGui type here would make every consumer of this
// header ImGui-aware and break D9.
struct PanelOptions {
    bool noScrollbar = false;  // -> ImGuiWindowFlags_NoScrollbar
    bool noPadding = false;    // -> WindowPadding pushed to (0,0) across Begin (the 2.2.3 viewport)
    bool hasMenuBar = false;   // -> ImGuiWindowFlags_MenuBar; the panel draws its own BeginMenuBar
};

// One dockable editor panel. Subclasses own their state; the REGISTRY owns the subclass and calls
// ImGui::Begin/End around onDraw(), so a panel can never unbalance them (the 2.1.1 review Gap 1).
// onDraw() runs INSIDE an already-open window: never Begin/End your own window in it.
class Panel {
public:
    Panel() = default;
    virtual ~Panel() = default;
    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;
    Panel(Panel&&) = delete;
    Panel& operator=(Panel&&) = delete;

    // STABLE, unique, non-empty, null-terminated; must outlive the panel (in practice a string
    // literal). Doubles as the ImGui window name AND the imgui.ini settings key (D6) — RENAMING AN
    // ID ORPHANS EVERY USER'S SAVED LAYOUT FOR THAT PANEL. Treat it as a persisted format.
    [[nodiscard]] virtual const char* id() const noexcept = 0;

    // The View-menu label; defaults to id(). Same lifetime contract.
    [[nodiscard]] virtual const char* title() const noexcept { return id(); }

    [[nodiscard]] virtual DockSlot defaultDockSlot() const noexcept { return DockSlot::Center; }
    [[nodiscard]] virtual PanelOptions options() const noexcept { return {}; }

    // Draw the contents. Called once per frame while visible, inside the open window. Takes no
    // arguments in 2.1.3 (D13) — panels capture what they need at construction; 2.2.1 introduces a
    // PanelContext when there is a World and a selection to pass.
    virtual void onDraw() = 0;
};

}  // namespace engine::editor

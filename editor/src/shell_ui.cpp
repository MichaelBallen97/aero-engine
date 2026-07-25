// Aero Engine — the ONE new ImGui TU this task adds besides placeholder_panel.cpp (task 2.1.3, D9):
// menu bar, dockspace, the data-driven default layout, and the balanced Begin/End around every
// Panel::onDraw(). editor_app.cpp includes this header but never ImGui itself.
#include "shell_ui.hpp"

#include <aero/editor/panel.hpp>

#include <array>
#include <cstddef>
#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder* (2.1.1 precedent)

namespace engine::editor {

namespace {

// One disabled stub item + its "owned by task N" tooltip (D5). A disabled MenuItem can never return
// true, so there is no unimplemented handler behind it.
void menuItemStub(const char* label, const char* shortcut, const char* owningTask) {
    ImGui::MenuItem(label, shortcut, false, /*enabled=*/false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", owningTask);  // e.g. "Not implemented yet — task 2.5.1"
    }
}

void drawMenuBar(PanelRegistry& panels, ShellUiState& state) {
    // Editor shortcuts go through ImGui's routing, NEVER ctx.input() (D7/E9): a focused InputText
    // must be able to swallow the chord. ImGuiMod_Ctrl is Cmd on macOS automatically (F8).
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Q, ImGuiInputFlags_RouteGlobal)) {
        state.quitRequested = true;
    }

    if (!ImGui::BeginMainMenuBar()) {
        return;  // F7: End only when Begin returned true
    }
    if (ImGui::BeginMenu("File")) {
        menuItemStub("New Scene", "Ctrl+N", "Not implemented yet — task 2.5.1");
        menuItemStub("Open Scene...", "Ctrl+O", "Not implemented yet — task 2.5.1");
        menuItemStub("Save Scene", "Ctrl+S", "Not implemented yet — task 2.5.1");
        menuItemStub("Save Scene As...", "Ctrl+Shift+S", "Not implemented yet — task 2.5.1");
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Ctrl+Q")) {
            state.quitRequested = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        menuItemStub("Undo", "Ctrl+Z", "Not implemented yet — task 2.4.1");
        menuItemStub("Redo", "Ctrl+Shift+Z", "Not implemented yet — task 2.4.1");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (std::size_t i = 0; i < panels.count(); ++i) {
            bool shown = panels.visibleAt(i);
            if (ImGui::MenuItem(panels.panelAt(i).title(), nullptr, &shown)) {
                panels.setVisibleAt(i, shown);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {
            state.applyDefaultLayout = true;
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

// The shortcut labels above are DISPLAY STRINGS ONLY — MenuItem's `shortcut` argument draws text and
// binds nothing; the sole live binding is the explicit ImGui::Shortcut above (the stubs deliberately
// bind nothing, so Ctrl+S does nothing until 2.5.1 wires it).

// The balanced-End core (E1). Hidden panels `continue` before Begin, so they never call End either;
// a visible panel's End() is unconditional — Begin's return value only gates onDraw().
void drawPanels(PanelRegistry& panels) {
    for (std::size_t i = 0; i < panels.count(); ++i) {
        bool open = panels.visibleAt(i);
        if (!open) {
            continue;  // hidden: no Begin, therefore no End (E1)
        }
        Panel& panel = panels.panelAt(i);
        const PanelOptions opts = panel.options();

        ImGuiWindowFlags flags = 0;
        if (opts.noScrollbar) {
            flags |= ImGuiWindowFlags_NoScrollbar;
        }
        if (opts.hasMenuBar) {
            flags |= ImGuiWindowFlags_MenuBar;
        }

        if (opts.noPadding) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        }
        const bool contentVisible = ImGui::Begin(panel.id(), &open, flags);
        if (opts.noPadding) {
            ImGui::PopStyleVar();  // pop right after Begin: applies to the window frame, not the
                                   // panel's widgets
        }
        if (contentVisible) {
            panel.onDraw();
        }
        ImGui::End();                  // ALWAYS — Begin's return value never gates it
        panels.setVisibleAt(i, open);  // the close-'X' wrote through `open` (E2)
    }
}

// C4: the DockSlot enumerator count, kept in lockstep with panel.hpp by the static_assert below —
// this is what keeps a future 5th slot from silently writing out of bounds into `used`.
constexpr std::size_t DOCK_SLOT_COUNT = 4;
static_assert(static_cast<std::size_t>(DockSlot::Bottom) + 1U == DOCK_SLOT_COUNT,
              "DockSlot gained an enumerator — widen DOCK_SLOT_COUNT and add its split below");

constexpr std::size_t slotIndex(DockSlot slot) noexcept { return static_cast<std::size_t>(slot); }

// D3 + F10: the default/reset dock layout. Only splits a slot at least one registered panel asks
// for — an empty ImGui dock node is not pruned, it renders as a dead grey rectangle.
void buildDefaultLayout(ImGuiID dockId, PanelRegistry& panels) {
    std::array<bool, DOCK_SLOT_COUNT> used{};  // value-initialised: all false
    for (std::size_t i = 0; i < panels.count(); ++i) {
        used[slotIndex(panels.panelAt(i).defaultDockSlot())] = true;
    }

    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);  // F9/F11

    ImGuiID center = dockId;
    ImGuiID left = 0;
    ImGuiID right = 0;
    ImGuiID bottom = 0;
    if (used[slotIndex(DockSlot::Left)]) {
        left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20F, nullptr, &center);
    }
    if (used[slotIndex(DockSlot::Right)]) {
        right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.25F, nullptr, &center);
    }
    if (used[slotIndex(DockSlot::Bottom)]) {
        bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25F, nullptr, &center);
    }

    for (std::size_t i = 0; i < panels.count(); ++i) {
        const Panel& panel = panels.panelAt(i);
        ImGuiID node = center;
        switch (panel.defaultDockSlot()) {
            case DockSlot::Left:
                node = left;
                break;
            case DockSlot::Right:
                node = right;
                break;
            case DockSlot::Bottom:
                node = bottom;
                break;
            case DockSlot::Center:
                node = center;
                break;
        }
        ImGui::DockBuilderDockWindow(panel.id(), node);  // by NAME — works for never-submitted and
                                                         // currently-hidden panels alike (F11/E13)
    }
    ImGui::DockBuilderFinish(dockId);
}

}  // namespace

void drawShellUi(PanelRegistry& panels, ShellUiState& state) {
    drawMenuBar(panels, state);  // FIRST: reserves the viewport work area (F9) and is where Reset
                                 // Layout can still affect THIS frame
    const ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    if (state.applyDefaultLayout) {
        state.applyDefaultLayout = false;
        buildDefaultLayout(dockId, panels);  // must run AFTER the dockspace exists this frame (E17)
    }
    drawPanels(panels);
}

}  // namespace engine::editor

#pragma once
// Aero Engine -- the Create menu's DRAWING helper (task E.5.2). SRC-PRIVATE, and this header is ImGui-free:
// every ImGui call lives in create_menu_ui.cpp. ONE helper for all three hosts (the menu bar and the
// Hierarchy's two context menus), so they cannot drift.
#include <aero/editor/create_menu.hpp>

#include <optional>

namespace engine::editor {

// Draws the TYPED Create entries into the menu or popup the caller ALREADY has open: one submenu per
// contiguous group run (BeginMenu(createMenuGroupLabel(g), enabled) -- EndMenu ONLY when it returned true),
// one MenuItem(createKindLabel(k), nullptr, false, enabled) per entry, TopLevel entries directly. Walks
// createMenuEntries() and states NO CreateKind literal (I253(b)). Returns the kind a click chose THIS
// frame; the caller records it and never applies it (EditorApp::applyCreate is the one place that does).
[[nodiscard]] std::optional<CreateKind> drawCreateMenuItems(bool enabled);

}  // namespace engine::editor

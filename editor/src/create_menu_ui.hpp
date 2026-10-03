#pragma once
// Aero Engine -- the Create menu's DRAWING helper (task E.5.2). SRC-PRIVATE, and this header is ImGui-free:
// every ImGui call lives in create_menu_ui.cpp. ONE helper for all three hosts (the menu bar and the
// Hierarchy's two context menus), so they cannot drift.
#include <aero/editor/create_menu.hpp>

#include <optional>

namespace engine::editor {

// Draws the TYPED Create entries into the menu or popup the caller ALREADY has open: one submenu per
// contiguous group run (BeginMenuEx(createMenuGroupLabel(g), createMenuGroupIcon(g), enabled) -- EndMenu ONLY
// when it returned true), one MenuItemEx(createKindLabel(k), createKindIcon(k), nullptr, false, enabled) per
// entry (task E.6.1: each with its Lucide icon), TopLevel entries directly. Walks
// createMenuEntries() and states NO CreateKind literal (I253(b)). Returns the kind a click chose THIS
// frame; the caller records it and never applies it (EditorApp::applyCreate is the one place that does).
[[nodiscard]] std::optional<CreateKind> drawCreateMenuItems(bool enabled);

// task E.6.1: one host-labelled item with `kind`'s icon -- MenuItemEx(label, createKindIcon(kind), nullptr,
// false, enabled) -- so a host that labels an entry itself (the Hierarchy's two Create Empty items) shows
// the same icon WITHOUT including imgui_internal.h (decision D-10). True on the frame it was clicked.
[[nodiscard]] bool drawCreateKindItem(const char* label, CreateKind kind, bool enabled);

}  // namespace engine::editor

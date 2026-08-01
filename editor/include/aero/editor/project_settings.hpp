#pragma once
// Aero Engine — the Project Settings panel's model: the open project's manifest turned into labelled,
// grouped, display-ready rows (task 2.6.2). PUBLIC, and free of ImGui, SDL, entt and <filesystem> --
// the project.hpp / console_model.hpp shape, so every rule below is reachable from the UNGATED tier-0
// aero_editor_shell_test with no window and no GPU. Held by FILE PLACEMENT (R12).
//
// FREE OF EVERY BUILD GATE (D4), inherited from project.hpp: this task reads and writes no scene, so
// tests/editor/project_settings_test.cpp is present and passing in ALL THREE build configurations.
//
// NOTHING HERE LOGS (INV-S3) and nothing here touches a file (INV-S2). The panel DISPLAYS the parsed,
// in-memory manifest -- it never re-reads project.json (D6). An external edit to that file while the
// editor is running is picked up by File > Open Project on the same root, which routes through
// adoptProject and therefore resets the scene; there is deliberately no Reload button, because a panel
// may not discard the user's scene.
#include <aero/editor/project.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// FROZEN (D12/INV-S1): this is the ImGui window name AND the imgui.ini settings key
// (panel.hpp:52-53). Renaming it orphans every existing user's saved layout for this panel. It lives
// in this PUBLIC header, not in the src-private panel header, because shell_ui.cpp's Edit menu item
// and the GPU-gated tests both name it and neither may see project_settings_panel.hpp.
inline constexpr const char* PROJECT_SETTINGS_PANEL_ID = "Project Settings";

// "TypeScript" / "C++" -- the HUMAN name, deliberately not languageKey()'s "ts"/"cpp" wire form (D9).
// switch-based with NO default: label, so a third language is a compile-time reminder.
[[nodiscard]] std::string_view languageDisplayName(ProjectLanguage language) noexcept;

struct ProjectSettingsRow {
    std::string label;
    std::string value;
};

// One SeparatorText + one table in the panel. A GROUP rather than a flat list with a separator flag
// because ImGui::Separator() only spans all columns for the LEGACY Columns() api
// (imgui_widgets.cpp:1736-1738); inside a table cell it spans that cell (F2/D10).
struct ProjectSettingsGroup {
    std::string title;
    std::vector<ProjectSettingsRow> rows;
};

// EMPTY vector <=> no project open (D11). There is no bool and no sentinel group.
//
// THE isOpen() GUARD IS LOAD-BEARING, and not for the obvious reason: ProjectSession::manifest() is
// NOT guarded by isOpen() (F6) and close() resets it to a DEFAULT ProjectManifest, whose assetsPath /
// scenesPath are "assets" / "scenes" -- NOT empty. A model that read manifest() unguarded would render
// a plausible, entirely fictional project rather than obviously breaking.
//
// `buildEngineVersion` is AERO_ENGINE_VERSION, captured by the panel at construction. EMPTY suppresses
// the mismatch suffix entirely (D8) -- the same rule docs/09 §4.7's WARN already follows.
[[nodiscard]] std::vector<ProjectSettingsGroup> projectSettingsGroups(const ProjectSession& session,
                                                                      std::string_view buildEngineVersion);

}  // namespace engine::editor

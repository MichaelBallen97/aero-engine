#pragma once
// Aero Engine — <prefPath>/editor_prefs.json (v1), the editor's per-user UI taste (task E.3.2).
// MACHINE-LOCAL, PER-USER, DERIVED FROM A CHOICE, DISPOSABLE -- one file per MACHINE, beside
// recent_projects.json and editor_tools.json (docs/09 section 8.5).
//
// WHY NOT editor_tools.json (D9): BlenderService::setOverridePath builds a FRESH ToolPrefs and writes
// the whole file (blender_service.cpp:172-181), so a second key in that struct is reset to its default
// by every Locate.../Re-detect. Sharing the file would mean turning that write into a
// read-modify-write -- a behaviour change inside 3.2.4's state machine, for no benefit here.
//
// WHY NOT project.json: it is committed and shared. docs/09 section 8.1 already makes this argument
// for the Blender path; a UI taste is a worse fit, not a better one -- it would hand every teammate a
// taste rather than a path.
//
// THE FILE IS THE HOME FOR EVERY FUTURE PER-USER EDITOR TASTE -- E.6.1's EditorTheme and the standing
// "persist the four viewport toggles and the tonemap params per user" handoff (3.6.3 / E.1.2 / E.2.3 /
// E.2.4). Append a key, bump nothing: an ABSENT key is its default, by this parser's own rule below.
//
// THE PATH IS NOT RESOLVED HERE. defaultEditorPrefsPath() lives in <aero/editor/project.hpp> beside
// defaultRecentProjectsPath and defaultToolPrefsPath, because project_file.cpp owns the editor's whole
// SDL pref-path chain and its free/do-not-free asymmetry, and is the one TU permitted a WARN on the
// CWD fallback. NOTHING IN THIS PAIR LOGS: editor_app.cpp turns `corrupt` into the one WARN, beside
// the tool-preferences one it already emits.
//
// PUBLIC, and therefore ImGui-free and entt-free BY FILE PLACEMENT (.claude/rules/editor.md).

#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

inline constexpr int EDITOR_PREFS_FORMAT_VERSION = 1;

struct EditorPrefs {
    // task E.3.2. TRUE is the shipping behaviour and therefore the default -- and it is also what a
    // MISSING FILE and an ABSENT KEY both mean, so a machine that has never opened the View menu
    // behaves exactly like one that ticked the box.
    bool focusFollowsSelection = true;
};

// nullopt on unparseable JSON, a non-object root, a wrong "version", or a PRESENT-but-non-bool value.
// An ABSENT "focusFollowsSelection" is the DEFAULT, not a miss -- the blenderPath rule
// (blender_tool.cpp's optionalString), which is what makes appending a key a non-breaking change.
[[nodiscard]] std::optional<EditorPrefs> parseEditorPrefs(std::string_view text);

// Deterministic: JsonWriter's DEFAULT config, fixed key order with "version" first, exactly one
// trailing '\n'. Re-parses equal.
[[nodiscard]] std::string writeEditorPrefsText(const EditorPrefs& prefs);

// A MISSING file is defaults, SILENTLY (corrupt = false). A file that EXISTS and does not parse, or
// carries a wrong version, is defaults PLUS corrupt = true. The two must never be conflated, or every
// machine that has never set a preference gets a warning on every launch (readBlenderEnv's own rule,
// blender_service.cpp:112-118). An EMPTY path reads nothing and sets corrupt = false.
//
// A file that EXISTS and cannot be READ -- a directory, or one the OS refuses -- is corrupt = true
// too, and that is a DISTINCT arm rather than a shade of "missing": readTextFile disengages its text
// for all three, so without a fileExists() discriminator an unreadable preferences file would silently
// reset the preference to its default with no diagnostic (EP13).
[[nodiscard]] EditorPrefs readEditorPrefs(std::string_view pathUtf8, bool& corrupt);

// "" on success, the OS reason otherwise. ONE writeTextFileAtomic call, with the path as a NAMED LOCAL
// on the preceding line (AC-25's rule, blender_service.cpp:175-176). An EMPTY path writes nothing and
// returns "" -- "this instance does not persist preferences" is not a failure.
[[nodiscard]] std::string writeEditorPrefs(std::string_view pathUtf8, const EditorPrefs& prefs);

}  // namespace engine::editor

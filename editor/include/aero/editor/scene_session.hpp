#pragma once
// Aero Engine — SceneSession: the document model, the pure file-flow guard, and the single scene-swap
// operation (task 2.5.1, D2/D3/INV-1). PUBLIC, and free of ImGui, SDL, entt, <filesystem> and
// scene_serialize -- every rule below is reachable from the ungated tier-0 aero_editor_shell_test, with
// no window and no GPU. Held by FILE PLACEMENT (R12), exactly like every other header under
// editor/include.
#include <aero/editor/command_stack.hpp>  // CommandContext + CommandStack; forward decls only inside

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

// EVERY enum below carries an explicit underlying type: performance-enum-size is
// --warnings-as-errors on the Linux lane.

// What the shell was asked to do.
enum class FileAction : std::uint8_t { None = 0, NewScene, OpenScene, SaveScene, SaveSceneAs, Quit };

// The modal's three answers. Esc == Cancel (D10).
enum class ConfirmChoice : std::uint8_t { Save = 0, Discard, Cancel };

// What the shell must do NEXT, as decided by a pure function. One enum for both decision points.
enum class FileStep : std::uint8_t {
    Nothing = 0,    // stop; the flow is over
    Confirm,        // raise the modal
    Perform,        // carry out the pending action now
    WriteNow,       // write to the existing path
    AskWhereToSave  // launch the Save dialog
};

// Moved here from file_dialog.hpp (plan A16) so a PUBLIC signature (FileFlow/FileDialogHost, task
// 2.5.1 step 5) can name it without seeing the src-private file_dialog.hpp.
enum class DialogKind : std::uint8_t { None = 0, Open, Save };

// Does `action` risk discarding work? NewScene/OpenScene/Quit do; the two saves never do.
[[nodiscard]] bool discardsWork(FileAction action) noexcept;

// The guard, in one pure function: Confirm iff the action discards work AND the document is dirty.
[[nodiscard]] FileStep guardFor(FileAction action, bool dirty) noexcept;

// Save (or the modal's "Save"): WriteNow with a path, AskWhereToSave without one.
[[nodiscard]] FileStep saveStep(bool untitled) noexcept;

// Resolving the modal. `untitled` decides whether "Save" writes or asks first.
[[nodiscard]] FileStep resolveConfirm(ConfirmChoice choice, bool untitled) noexcept;

// ---- the document ------------------------------------------------------------------------------
// Holds the current scene's absolute path and nothing else. Dirtiness is NOT stored here (D3): it is
// CommandStack::isClean(), and a second copy would be a second truth undo/redo could not keep in step.
class SceneSession {
public:
    [[nodiscard]] std::string_view path() const noexcept;  // "" == untitled
    [[nodiscard]] bool untitled() const noexcept;
    void setPath(std::string_view absolutePathUtf8);
    void clearPath() noexcept;

    // "level1.scene.json", or "Untitled".
    [[nodiscard]] std::string documentName() const;
    // The exact strings, character for character (SS7): "Untitled - Aero Editor",
    // "*Untitled - Aero Editor", "level1.scene.json - Aero Editor",
    // "*level1.scene.json - Aero Editor". Separator is " - " (space hyphen space), an ASCII hyphen,
    // NEVER an em dash -- this goes through SDL_SetWindowTitle and into a native title bar. The '*' is
    // the ONLY dirty affordance outside the Edit menu, so it leads.
    [[nodiscard]] std::string windowTitle(bool dirty) const;
    // Where a dialog should start: this scene's directory, else `projectRootUtf8`, else "" (D20).
    [[nodiscard]] std::string dialogDirectory(std::string_view projectRootUtf8) const;
    // The pre-filled Save target: the current path, else `<dialogDirectory>/Untitled.scene.json`.
    [[nodiscard]] std::string saveSuggestion(std::string_view projectRootUtf8) const;

private:
    std::string scenePath;
};

// ---- pure path helpers --------------------------------------------------------------------------
// Deliberately NOT project_files.hpp's leafOf/parentOf/depthOf/splitSegments (plan A25): those operate
// on OUR OWN '/'-separated RELATIVE paths (project_files.hpp:17-18, built only by joinRelative());
// these see OS-NATIVE ABSOLUTE paths coming back from a native dialog and must treat '\' as a
// separator too -- the rootDisplayName exception (project_files.hpp:115-119). D19/F17 forbid
// extending that read-only-by-contract module for this anyway.
inline constexpr std::string_view SCENE_EXTENSION = ".scene.json";
// POINTS INTO pathUtf8 -- never call on a temporary whose lifetime ends before the result is used
// (the project_files.hpp leafOf precedent).
[[nodiscard]] std::string_view fileNameOf(std::string_view pathUtf8) noexcept;
[[nodiscard]] std::string_view directoryOf(std::string_view pathUtf8) noexcept;  // "" when there is none
[[nodiscard]] bool hasExtension(std::string_view pathUtf8) noexcept;             // a '.' in the LAST segment only
[[nodiscard]] std::string withSceneExtension(std::string_view pathUtf8);         // D13's append rule

// ---- THE swap (D2/INV-6/INV-1) -------------------------------------------------------------------
// Clears the World, the Selection, the RootOrder and the CommandStack -- all four, in one call, with
// no way to perform half of it. THE ONLY CALLER OF World::clear() AND CommandStack::clear() UNDER
// editor/src/ (AC-30/AC-31). The order of the four calls does not matter; the COMPLETENESS does.
//
// An uncleared CommandStack here is not merely stale against a new World -- it is ACTIVELY HARMFUL:
// World::clear() bumps every version but never un-issues an index (measured, tests/scene_test.cpp W7),
// so World::recreate() of a pre-clear() handle SUCCEEDS. A history entry holding a SubtreeSnapshot
// from the previous scene, driven against a freshly loaded World, would therefore not fail -- it would
// resurrect a ghost entity from the old scene into the new one, with its old name, its old components
// and its old values, and then hand it a fresh identity that later commands keep editing. There is no
// assertion anywhere in the tree that would catch it (D2/F5).
//
// Deliberately NOT noexcept (plan A24): all four callees are (World::clear(), Selection::clear(),
// RootOrder::clear(), CommandStack::clear()), but a future diagnostic added here must not turn into a
// std::terminate.
void resetSceneState(CommandContext& context, CommandStack& commands);

// resetSceneState + seedDefaultScene (F8). Leaves the history empty and CLEAN (F7) -- commands.clear()
// already marks it so, and seedDefaultScene pushes nothing. Never the reverse order: seeding before
// clearing would leave six entities, not three.
void newScene(CommandContext& context, CommandStack& commands);

// ---- file bytes (scene_file.cpp; ALL <filesystem>/<fstream> for scenes lives there) --------------
struct FileReadResult {
    std::optional<std::string> text;  // engaged == success
    std::string error;                // OS reason; empty iff `text` is engaged
};
[[nodiscard]] FileReadResult readTextFile(std::string_view absolutePathUtf8);

// "" == success. ATOMIC (D12): writes <path>.aero-tmp, CLOSES it, renames over `path`.
[[nodiscard]] std::string writeTextFileAtomic(std::string_view absolutePathUtf8, std::string_view text);
[[nodiscard]] bool fileExists(std::string_view absolutePathUtf8);

// ---- serialization (scene_io.cpp; the ONE #if in this task, F9's AERO_REFLECT_TOOLS gate) --------
[[nodiscard]] bool sceneIoAvailable() noexcept;  // false without AERO_REFLECT_TOOLS

// Deliberately PLAIN INTEGERS, not an engine::SceneError -- even though F9 says the latter WOULD be
// legal here (aero::reflect is already a PUBLIC transitive dependency of aero_editor_core through
// aero::scene). Keeping this a value type with no <aero/reflect/...> include is what lets this whole
// header stay reachable in every build configuration with no #if of its own; do not "improve" it.
struct SceneOpenOutcome {
    bool ok = false;
    std::string message;     // the SceneError message when !ok; "" otherwise
    std::uint32_t line = 0;  // >0 only for a JSON-stage failure
    std::uint32_t column = 0;
    std::size_t entities = 0;  // SceneLoadReport, on success
    std::size_t components = 0;
    std::size_t skipped = 0;
    std::size_t failed = 0;
};

// PARSE FIRST, THEN SWAP (D5/E8): on a parse error nothing is touched, and `context`/`commands` are
// byte-for-byte what they were. On success calls resetSceneState, loads, and leaves the history clean.
[[nodiscard]] SceneOpenOutcome openSceneText(CommandContext& context, CommandStack& commands, std::string_view text);
// nullopt only when !sceneIoAvailable().
[[nodiscard]] std::optional<std::string> sceneToText(const World& world);

// ---- the async dialog boundary (file_dialog.{hpp,cpp}, src-private; D0/D6/D7/D8) -------------------

// The dialog result, drained on the MAIN thread. A plain value type, declared here rather than nested
// in the src-private DialogChannel so a PUBLIC signature (applyDialogResult) can name it without
// seeing file_dialog.hpp.
struct DialogResult {
    bool ready = false;      // false == nothing has arrived
    bool cancelled = false;  // *filelist == NULL -- the user cancelled; SILENT (AC-13)
    bool failed = false;     // filelist == NULL -- an SDL error (F4); exactly one ERROR
    std::string path;
};

// src-private (editor/src/file_dialog.hpp). Only the NAME is needed here -- the ViewportPanel /
// ConsolePanel precedent (editor_app.hpp), which keeps this PUBLIC header free of SDL.
class DialogChannel;

// Everything the flow needs to launch a native dialog. `channel == nullptr` means "no dialog is
// possible" -- the tier-0 test seam: a request that would need one is then a SILENT no-op (A17).
struct FileDialogHost {
    DialogChannel* channel = nullptr;
    void* parentWindow = nullptr;  // SDL_Window*, OPAQUE. No SDL type crosses this header (AC-32).
    std::string_view projectRoot;  // D20's fallback start directory
};

}  // namespace engine::editor

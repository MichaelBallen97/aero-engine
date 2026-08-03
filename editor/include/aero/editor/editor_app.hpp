#pragma once
// Aero Engine — EditorApp: the editor's application shell (task 2.1.3, D1/D4/D11). ImGui-FREE BY
// RULE (D9/AC-3) — this header exposes engine + std types only. Owns the ImGui host, the panel
// registry, and the frame clock, and turns the loop into a callable tick() (run() is
// `while (tick()) {}`), which is what makes it drivable N-frames-at-a-time from a test.

#include <aero/core/guid.hpp>  // task 3.1.1: assetGuidForPath()'s return type (AC-39)
#include <aero/core/time.hpp>
#include <aero/editor/asset_database.hpp>  // task 3.1.1: a VALUE member (assetDatabase) needs the
                                           // definition -- the command_stack.hpp/entity_ops.hpp
                                           // precedent below. Transitively brings project_files.hpp,
                                           // already a public editor header, ImGui/entt/<filesystem>-free.
#include <aero/editor/command_stack.hpp>   // a VALUE member needs the definition (the selection.hpp /
                                           // panel_registry.hpp precedent), unlike panel_context.hpp,
                                           // which holds a reference and forward-declares.
#include <aero/editor/entity_ops.hpp>      // a VALUE member (rootOrder) needs RootOrder's definition
#include <aero/editor/imgui_layer.hpp>
#include <aero/editor/panel_registry.hpp>
#include <aero/editor/scene_session.hpp>  // task 2.5.1: VALUE members SceneSession/FileFlow need the
                                          // definition, the command_stack.hpp precedent above.
#include <aero/editor/selection.hpp>
#include <aero/rhi/types.hpp>
#include <aero/scene/world.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace engine::platform {
class Window;  // task 2.5.1 (F14): a POINTER member needs only the name -- a precedent already set
               // below for ViewportPanel/ConsolePanel/EditorCamera. Keeps this header light (2.1.3 D9).
}  // namespace engine::platform

namespace engine::editor {

class ViewportPanel;      // task 2.2.3: src-private (editor/src/viewport_panel.hpp). Only the NAME is
                          // needed here, so this PUBLIC header stays free of viewport_panel.hpp's
                          // engine includes and stays ImGui/entt-free.
class ConsolePanel;       // task 2.2.5: src-private (editor/src/console_panel.hpp). Only the NAME is
                          // needed here, so this PUBLIC header stays free of console_panel.hpp and
                          // therefore of <aero/editor/console_model.hpp> as well.
class EditorCamera;       // task 2.3.1: PUBLIC (editor/include/aero/editor/editor_camera.hpp). Only the
                          // NAME is needed here -- viewportCamera() returns a pointer -- so this header
                          // keeps its include weight, exactly as it already does for ViewportPanel and
                          // ConsolePanel.
class DialogChannel;      // task 2.5.1: src-private (editor/src/file_dialog.hpp). Only the NAME is needed
                          // here for the shared_ptr member -- the ViewportPanel/ConsolePanel precedent.
class AssetBrowserPanel;  // task 2.2.4: src-private (editor/src/asset_browser_panel.hpp). Only the
                          // NAME is needed here for the reconcile's non-owning pointer -- the
                          // ViewportPanel/ConsolePanel precedent, applied a third time.

struct EditorAppConfig {
    rhi::Color clearColor{0.10F, 0.10F, 0.12F, 1.0F};  // unchanged from 2.1.1
    bool persistLayout = true;                         // -> ImGuiLayer (imgui.ini); false in tests
    bool registerDefaultPanels = true;                 // the five 2.2.x placeholders (D8)
    // The default new-scene contents (D9): Main Camera + Directional Light + Cube, so a freshly
    // launched editor is not an empty box and 2.2.3's viewport has something to render. Tests that
    // want a clean World set it false.
    bool seedDefaultScene = true;
    // Frame-rate ceiling applied ONLY while the window has no keyboard focus (D4). <= 0 disables the
    // throttle entirely — what the GPU smoke test uses so its frames stay unpaced + deterministic.
    float unfocusedFrameCapHz = 20.0F;
    // Task 2.6.1 (WAS `projectRoot`): a project DIRECTORY or a .../project.json path, as UTF-8.
    // EMPTY => the no-project state (D0), which is first-class and NOT an error: menus, panels,
    // docking, undo/redo and scene save/load all keep working; the Asset Browser simply has nothing
    // to show and a Welcome window offers the two ways out. NOT validated here (2.2.4's D17,
    // preserved): a bad path is one ERROR plus the no-project state, and the editor still opens,
    // docks and quits cleanly (AC-32). aero_editor's optional argv[1] writes this field.
    std::string projectPath;
    // Task 2.6.1 (D15): with no `projectPath`, open the first recents entry that HAS a project.json
    // on disk. TRUE is the shipping behaviour and therefore the default, matching
    // registerDefaultPanels / seedDefaultScene / persistLayout -- which makes a forgotten opt-out a
    // real footgun, so EVERY EditorAppConfig literal under tests/ sets it false and an AC-level grep
    // holds that line.
    bool restoreLastProject = true;
    // Task 2.6.1 (D15, the second defence): where the recents list lives. EMPTY =>
    // defaultRecentProjectsPath(). The one case that MUST exercise the restore path points this at a
    // TempDir file, so even with restore ENABLED no test ever touches the real pref directory.
    std::string recentProjectsPath;
    // Where imgui.ini lives. EMPTY => ImGuiLayer's own exe/pref-relative derivation, which is what
    // ships. This exists for the SAME reason recentProjectsPath above does, and it is the second
    // instance of the same lesson: a machine-wide file a test needs to drive is untestable until the
    // test can point it somewhere else. Any test that sets persistLayout TRUE must set this too, or
    // it writes the developer's real editor layout -- 2.6.1's BLOCKING-2 in a new costume.
    std::string layoutIniPath;
};

// Sleep applied when the window is not presentable (minimized) — inherited verbatim from 2.1.1.
inline constexpr std::uint32_t MINIMIZED_SLEEP_MS = 4;

// PURE pacing policy (D4), exposed for tier-0 testing. Order of precedence is load-bearing:
// minimized beats unfocused (E12).
//   !presented                         -> MINIMIZED_SLEEP_MS
//   focused, or capHz <= 0             -> 0        (vsync paces us)
//   otherwise                          -> max(0, round(1000 / capHz) - frameElapsedMs)
[[nodiscard]] std::uint32_t framePaceSleepMs(bool presented, bool focused, float unfocusedCapHz,
                                             float frameElapsedMs) noexcept;

class EditorApp {
public:
    // nullopt (+ ERROR, already logged by ImGuiLayer) when the ImGui host cannot be created. The
    // device, window and ctx MUST outlive the app (the ImGuiLayer contract, D1).
    [[nodiscard]] static std::optional<EditorApp> create(rhi::Device& device, platform::Window& window,
                                                         platform::Context& ctx, const EditorAppConfig& config = {});

    // task 2.5.1: declared here, DEFINED (= default) in editor_app.cpp -- the standard pimpl-adjacent
    // fix (2.4.2 §A13's scene_snapshot.hpp precedent, applied one layer up). A destructor/move
    // defaulted IN THIS HEADER would need DialogChannel complete at every point it could be
    // instantiated (including this header's own inclusion sites), which would force
    // "file_dialog.hpp" -- an SDL-adjacent src-private header -- onto every consumer's compile line
    // (AC-32). Out-of-line, where DialogChannel is complete, is the only acceptable spelling.
    ~EditorApp();
    EditorApp(EditorApp&&) noexcept;
    EditorApp& operator=(EditorApp&&) noexcept;
    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    // One full editor frame: input newFrame -> event drain -> clock tick -> ImGui frame (menu bar,
    // dockspace, panels) -> present -> pace. Returns false once a quit has been requested, and is
    // idempotent after that (further calls return false without drawing — E10).
    bool tick();

    // tick() until it returns false. Returns the process exit code (0).
    int run();

    [[nodiscard]] PanelRegistry& panels() noexcept;
    [[nodiscard]] const PanelRegistry& panels() const noexcept;
    // The edited scene. EditorApp OWNS it (D6): a World is DOCUMENT state, unlike the
    // Context/Window/Device that main.cpp injects because they are process-global (2.1.3 D1).
    [[nodiscard]] World& world() noexcept;
    [[nodiscard]] const World& world() const noexcept;
    // The shared entity selection: the hierarchy writes it, 2.2.2's inspector and 2.2.3's viewport
    // read it, 2.3.2's picking writes it. ONE object, never two.
    [[nodiscard]] Selection& selection() noexcept;
    [[nodiscard]] const Selection& selection() const noexcept;
    // The editor's undo/redo history (task 2.4.1). ONE object: the menu items, the two chords and
    // requestUndo()/requestRedo() all drive this same stack, and every panel reaches it through
    // PanelContext::commands. INV-6: it is only ever driven against the World this EditorApp owns --
    // 2.5.1's New/Open Scene must clear() it in the same operation that replaces that World.
    [[nodiscard]] CommandStack& commands() noexcept;
    [[nodiscard]] const CommandStack& commands() const noexcept;
    // The editor's ONE display order among root entities (task 2.4.2, D10). Moved off HierarchyPanel so
    // a structural command can restore a deleted root to the row it occupied: a command cannot reach a
    // panel's private member, and must not -- commands hold values and handles only. The Hierarchy
    // reconciles this same object every frame; with registerDefaultPanels == false nothing reconciles it
    // and it stays empty, which is a TESTED state (E5), not an assumption.
    [[nodiscard]] RootOrder& roots() noexcept;
    [[nodiscard]] const RootOrder& roots() const noexcept;
    [[nodiscard]] const FrameClock& clock() const noexcept;
    [[nodiscard]] bool focused() const noexcept;
    // True when the LAST tick() actually presented (false when the window was minimized). This is
    // what the GPU smoke test asserts — tick()'s own bool means "still running", not "presented".
    [[nodiscard]] bool presentedLastFrame() const noexcept;
    // How many log records the Console panel currently holds. 0 when no console panel is registered
    // (registerDefaultPanels == false, or registration was rejected). Exists for the same reason
    // presentedLastFrame() does (D16): without it, "records are captured while the panel is HIDDEN"
    // (AC-6) is mechanically unprovable and would fall entirely to the human pass.
    [[nodiscard]] std::size_t logRecordCount() const noexcept;
    // The Viewport's own camera (task 2.3.1, D6). NULL when no Viewport panel is registered
    // (registerDefaultPanels == false, or registration was rejected). Exists for the same reason
    // logRecordCount() does: without it, "the Viewport renders through the EDITOR camera and not
    // through the scene Camera" has no black-box signature at all and would fall entirely to the
    // human pass -- exactly the failure mode 2.2.3's S6 and S8 documented.
    [[nodiscard]] EditorCamera* viewportCamera() noexcept;
    [[nodiscard]] const EditorCamera* viewportCamera() const noexcept;
    // The current scene's path ("" == untitled, task 2.5.1). A VIEW into a member that lives as long
    // as the app -- a caller holding it ACROSS a scene swap would dangle; copy it first if that matters.
    [[nodiscard]] std::string_view scenePath() const noexcept;
    // == !commands().isClean() (D3): there is no second flag.
    [[nodiscard]] bool sceneDirty() const noexcept;

    // ---- task 2.6.1: the open project. The black-box signature the GPU cases need, exactly the
    // reason logRecordCount() (2.2.5 D16) and viewportCamera() (2.3.1 D6) exist. ----
    [[nodiscard]] bool projectIsOpen() const noexcept;
    [[nodiscard]] std::string_view projectRoot() const noexcept;  // "" when none
    [[nodiscard]] std::string_view projectName() const noexcept;  // "" when none
    // The Asset Browser panel's current root ("" when no Asset Browser panel is registered, i.e.
    // registerDefaultPanels == false or registration was rejected). A VIEW into a member that lives as
    // long as the app -- the scenePath() caveat applies. Exists for the same reason logRecordCount()
    // (2.2.5 D16) and viewportCamera() (2.3.1 D6) do: without it, D10's per-frame reconcile has no
    // black-box signature, AC-31 is unprovable and sabotage seed S14 would redden nothing at all.
    [[nodiscard]] std::string_view assetBrowserRoot() const noexcept;
    // How many entries the recents list currently holds. Exists for the same reason
    // assetBrowserRoot() (A9) does: without it, AC-34 ("restoreLastProject == false never reads the
    // recents file") is provable only as a downstream consequence (projectIsOpen()/projectRoot()
    // staying empty), never DIRECTLY -- and it is what makes BLOCKING-2's fix (every GPU case that can
    // dirty the recents list points recentProjectsPath at its own scratch file) testable at all: a
    // case that forgot the override would otherwise show no symptom in-process, only a polluted file
    // on disk after the run.
    [[nodiscard]] std::size_t recentProjectCount() const noexcept;

    // ---- task 3.1.1: the asset scan. The black-box signature the GPU cases need, exactly the reason
    // assetBrowserRoot() (2.6.1 A9) and recentProjectCount() (2.6.1 code review) exist. ----
    [[nodiscard]] std::size_t assetCount() const noexcept;
    [[nodiscard]] std::optional<Guid> assetGuidForPath(std::string_view relativePath) const noexcept;

    void requestQuit() noexcept;
    void requestLayoutReset() noexcept;  // same effect as View > Reset Layout, applied next frame
    // Same effect as Edit > Undo / Ctrl+Z (Cmd+Z on macOS), applied on the NEXT tick -- the
    // requestLayoutReset() shape, NOT requestQuit()'s (which takes effect immediately). This is the
    // only mechanically drivable entry point: aero_editor_imgui_test is ImGui-free at source and
    // cannot press a key (task 2.4.1 D11/F6), the same reason logRecordCount() (2.2.5 D16) and
    // viewportCamera() (2.3.1 D6) exist.
    void requestUndo() noexcept;
    void requestRedo() noexcept;

    // ---- task 2.5.1: the File menu's request hooks. Each is `fileFlow.requested = FileAction::X;`
    // (plus `.requestedPath` for the two path-taking ones), applied on the NEXT tick -- the
    // requestLayoutReset()/requestUndo() shape, not requestQuit()'s. The two path-taking hooks are
    // D15's test seam: aero_editor_imgui_test is ImGui-free at source and cannot click a native
    // dialog, so nothing about Open/Save As would be drivable through a real frame without them. ----
    void requestNewScene() noexcept;                 // guarded
    void requestOpenSceneDialog() noexcept;          // guarded, then launches the native dialog
    void requestOpenScene(std::string_view path);    // guarded, NO dialog (D15)
    void requestSaveScene() noexcept;                // Save, or Save As when untitled
    void requestSaveSceneAs(std::string_view path);  // NO dialog (D15)
    // D14: what the UI's quit paths (File > Exit, Ctrl+Q, the window [X]) call. requestQuit() below
    // stays UNCHANGED and unguarded -- 35 existing GPU-gated cases end
    // `app->requestQuit(); CHECK(app->tick() == false);` and this must not become a second thing.
    void requestGuardedQuit() noexcept;

    // ---- task 2.6.1: the project flow's request hooks. All `projectFlow`/`fileFlow` writes, applied
    // on the NEXT tick -- the requestUndo() shape, never requestQuit()'s. ----
    void requestNewProject() noexcept;               // guarded; opens the modal
    void requestOpenProjectDialog() noexcept;        // guarded; launches the folder dialog
    void requestOpenProject(std::string_view path);  // guarded, NO dialog -- the D15 test seam
    void requestClearRecentProjects() noexcept;

    // task 3.1.1 (AC-38): the requestUndo()/requestLayoutReset() shape -- applied on the NEXT tick,
    // drained in the SAME reconcile expression as AssetBrowserPanel::takeRescanRequest(). This is what
    // makes the request channel drivable through real frames from the ImGui-free
    // aero_editor_imgui_test, which cannot click the panel's own Refresh button (it is ImGui-free at
    // source).
    void requestAssetRescan() noexcept;

private:
    // BY VALUE + move (task 2.2.4): EditorAppConfig gained a std::string field, so it is no longer
    // trivially copyable and modernize-pass-by-value (--warnings-as-errors in CI) requires this shape.
    EditorApp(ImGuiLayer layer, platform::Context& ctx, EditorAppConfig config);

    ImGuiLayer layer;
    platform::Context* ctx;
    EditorAppConfig config;
    PanelRegistry registry;
    FrameClock frameClock;
    bool running = true;
    bool windowFocused = true;        // a freshly created+shown window normally has focus; a stale false
                                      // would only cost one throttled frame before the first focus event
    bool applyDefaultLayout = false;  // seeded from ImGuiLayer::wantsDefaultLayout(); also set by
                                      // View > Reset Layout / requestLayoutReset()
    // The EXACT COMPLEMENT of applyDefaultLayout, and seeded from the same call: when a layout was
    // RESTORED, buildDefaultLayout never runs, and it is the only reader of defaultDockSlot(). A panel
    // registered after the user's imgui.ini was written therefore has no settings entry at all and
    // ImGui free-floats it. This one-shot places exactly those panels on the first drawn frame.
    // Consumed like applyDefaultLayout (read back out of ShellUiState), never re-armed.
    bool placeUnplacedPanels = false;
    bool presented = false;
    bool undoRequested = false;  // consumed by the next tick(); never survives it (task 2.4.1)
    bool redoRequested = false;
    World sceneWorld;
    Selection sceneSelection;
    CommandStack commandStack;          // F15: noexcept-movable, so EditorApp's own `noexcept = default` move
                                        // stays valid. command_stack.hpp's two static_asserts hold that line.
    RootOrder rootOrder;                // task 2.4.2, D10 (accessor: roots()). entity_ops.hpp's two static_asserts
                                        // hold the same noexcept-move guarantee for this member.
    AssetDatabase assetDatabase;        // task 3.1.1 (D12) -- reconciled in tick(), never pushed. Own six
                                        // static_asserts (asset_database.hpp) hold the noexcept-move
                                        // guarantee this member needs.
    GuidGenerator assetGuids{0};        // task 3.1.1 -- replaced in create() by fromEntropy(); one uint64,
                                        // so this does not affect EditorApp's own `noexcept = default` move.
    bool assetRescanRequested = false;  // task 3.1.1 (AC-38) -- consumed by the next tick()'s reconcile
    // Non-owning; owned by `registry`, which holds panels through unique_ptr -- so the Panel object
    // is address-stable and this pointer survives an EditorApp move (F21). Null when
    // registerDefaultPanels == false (E13) or if registration was rejected (E14) -- ALWAYS null-check.
    ViewportPanel* viewportPanel = nullptr;
    // Non-owning; owned by `registry` (unique_ptr -> address-stable, survives an EditorApp move --
    // F17, the same reason viewportPanel above is legal). Null when registerDefaultPanels == false or
    // if registration was rejected -- ALWAYS null-check.
    ConsolePanel* consolePanel = nullptr;
    // task 2.6.1: non-owning, the SAME precedent as viewportPanel/consolePanel above -- owned by
    // `registry` through unique_ptr, therefore address-stable, null when registerDefaultPanels ==
    // false or registration was rejected (E26), ALWAYS null-checked. D10's per-frame reconcile target.
    AssetBrowserPanel* assetBrowserPanel = nullptr;

    // ---- task 2.5.1 ---------------------------------------------------------------------------
    // Non-owning; F14: setTitle() and the dialog's parent window. The caller contract (create()'s
    // doc comment above) already requires the window to outlive the app.
    platform::Window* window = nullptr;
    SceneSession session;  // the current scene's path (D2/D3: dirtiness itself is NOT stored here)
    FileFlow fileFlow;     // the File-menu state machine's data
    // task 2.6.1: the open project and its own flow state, plus the recent-projects list.
    ProjectSession project;
    ProjectFlow projectFlow;
    RecentProjects recents;
    std::string recentsPath;  // resolved ONCE in create(): config.recentProjectsPath, else D8's default
    // Created once in create(); NEVER null on a LIVE app. NULL only on a moved-from app (a defaulted
    // move leaves the source's shared_ptr empty), which is why the drain in tick() is null-guarded
    // (plan A18) rather than assumed non-null.
    std::shared_ptr<DialogChannel> dialogChannel;
    std::string lastTitle;  // D16: setTitle() only when this string CHANGES
};

}  // namespace engine::editor

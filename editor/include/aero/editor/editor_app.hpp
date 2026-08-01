#pragma once
// Aero Engine — EditorApp: the editor's application shell (task 2.1.3, D1/D4/D11). ImGui-FREE BY
// RULE (D9/AC-3) — this header exposes engine + std types only. Owns the ImGui host, the panel
// registry, and the frame clock, and turns the loop into a callable tick() (run() is
// `while (tick()) {}`), which is what makes it drivable N-frames-at-a-time from a test.

#include <aero/core/time.hpp>
#include <aero/editor/command_stack.hpp>  // a VALUE member needs the definition (the selection.hpp /
                                          // panel_registry.hpp precedent), unlike panel_context.hpp,
                                          // which holds a reference and forward-declares.
#include <aero/editor/entity_ops.hpp>     // a VALUE member (rootOrder) needs RootOrder's definition
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

class ViewportPanel;  // task 2.2.3: src-private (editor/src/viewport_panel.hpp). Only the NAME is
                      // needed here, so this PUBLIC header stays free of viewport_panel.hpp's
                      // engine includes and stays ImGui/entt-free.
class ConsolePanel;   // task 2.2.5: src-private (editor/src/console_panel.hpp). Only the NAME is
                      // needed here, so this PUBLIC header stays free of console_panel.hpp and
                      // therefore of <aero/editor/console_model.hpp> as well.
class EditorCamera;   // task 2.3.1: PUBLIC (editor/include/aero/editor/editor_camera.hpp). Only the
                      // NAME is needed here -- viewportCamera() returns a pointer -- so this header
                      // keeps its include weight, exactly as it already does for ViewportPanel and
                      // ConsolePanel.
class DialogChannel;  // task 2.5.1: src-private (editor/src/file_dialog.hpp). Only the NAME is needed
                      // here for the shared_ptr member -- the ViewportPanel/ConsolePanel precedent.

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
    // Task 2.2.4: the asset browser's root, as UTF-8. EMPTY -> the process working directory,
    // resolved ONCE at create() by resolveProjectRoot(). aero_editor's optional argv[1] writes this
    // field. A bad path is NOT validated here (D17): it flows to the panel, which reports it in-panel
    // and still docks and quits cleanly. Task 2.6.1 replaces this with the opened project's path and
    // calls AssetBrowserPanel::setRoot().
    std::string projectRoot;
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
    bool presented = false;
    bool undoRequested = false;  // consumed by the next tick(); never survives it (task 2.4.1)
    bool redoRequested = false;
    World sceneWorld;
    Selection sceneSelection;
    CommandStack commandStack;  // F15: noexcept-movable, so EditorApp's own `noexcept = default` move
                                // stays valid. command_stack.hpp's two static_asserts hold that line.
    RootOrder rootOrder;        // task 2.4.2, D10 (accessor: roots()). entity_ops.hpp's two static_asserts
                                // hold the same noexcept-move guarantee for this member.
    // Non-owning; owned by `registry`, which holds panels through unique_ptr -- so the Panel object
    // is address-stable and this pointer survives an EditorApp move (F21). Null when
    // registerDefaultPanels == false (E13) or if registration was rejected (E14) -- ALWAYS null-check.
    ViewportPanel* viewportPanel = nullptr;
    // Non-owning; owned by `registry` (unique_ptr -> address-stable, survives an EditorApp move --
    // F17, the same reason viewportPanel above is legal). Null when registerDefaultPanels == false or
    // if registration was rejected -- ALWAYS null-check.
    ConsolePanel* consolePanel = nullptr;

    // ---- task 2.5.1 ---------------------------------------------------------------------------
    // Non-owning; F14: setTitle() and the dialog's parent window. The caller contract (create()'s
    // doc comment above) already requires the window to outlive the app.
    platform::Window* window = nullptr;
    SceneSession session;  // the current scene's path (D2/D3: dirtiness itself is NOT stored here)
    FileFlow fileFlow;     // the File-menu state machine's data
    // task 2.6.1: the open project and its own flow state, plus the recent-projects list. Wired
    // fully in Step 6 (startup resolution, the reconcile, the request hooks); Step 5 only keeps the
    // signature changes above compiling.
    ProjectSession project;
    ProjectFlow projectFlow;
    RecentProjects recents;
    // Created once in create(); NEVER null on a LIVE app. NULL only on a moved-from app (a defaulted
    // move leaves the source's shared_ptr empty), which is why the drain in tick() is null-guarded
    // (plan A18) rather than assumed non-null.
    std::shared_ptr<DialogChannel> dialogChannel;
    std::string projectRootResolved;  // D20: resolved ONCE in create(), the dialog's start directory
    std::string lastTitle;            // D16: setTitle() only when this string CHANGES
};

}  // namespace engine::editor

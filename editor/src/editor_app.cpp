// Aero Engine — EditorApp: create/tick/run/pacing (task 2.1.3, D1/D4/D11). ImGui-FREE — includes
// shell_ui.hpp (src-private) but never ImGui itself; drawShellUi is the only ImGui-touching call.
#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/time.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/editor_app.hpp>
#include <aero/editor/editor_camera.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/scene_session.hpp>
#include <aero/platform/context.hpp>
#include <aero/platform/event.hpp>
#include <aero/platform/internal/native_window.hpp>  // task 2.5.1: the SECOND consumer F15 sanctions
                                                     // (the first is imgui_layer.cpp) -- no new
                                                     // accessor is added.
#include <aero/platform/window.hpp>                  // task 2.5.1: window->setTitle() (F14)

#include "asset_browser_panel.hpp"
#include "console_panel.hpp"
#include "editor_reflection.hpp"
#include "file_dialog.hpp"  // task 2.5.1: DialogChannel's definition -- the shared_ptr's deleter needs
                            // a complete type wherever it could run, including this TU's ~EditorApp
#include "hierarchy_panel.hpp"
#include "inspector_panel.hpp"
#include "shell_ui.hpp"
#include "viewport_panel.hpp"

#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#ifndef AERO_ENGINE_VERSION
    #define AERO_ENGINE_VERSION \
        "0.0.0"  // a fallback for a hand-rolled build with no CMake definition.
                 // NOT a build gate (AC-35/D4/INV-P5): no CODE PATH is
                 // conditional on it -- exactly one string constant is.
#endif

namespace engine::editor {

namespace {

// task 2.5.1: the dialog's parent window and setTitle()'s target both need the SDL_Window* the
// platform layer hides. This is the SECOND consumer of NativeWindowAccessor (imgui_layer.cpp is the
// first) -- no new accessor is added (F15). `window == nullptr` only on a moved-from app.
void* nativeWindowHandle(platform::Window* window) {
    return window != nullptr ? static_cast<void*>(platform::internal::NativeWindowAccessor::get(*window)) : nullptr;
}

}  // namespace

std::uint32_t framePaceSleepMs(bool presented, bool focused, float unfocusedCapHz, float frameElapsedMs) noexcept {
    if (!presented) {
        return MINIMIZED_SLEEP_MS;  // E12: minimized wins over everything else
    }
    if (focused || !(unfocusedCapHz > 0.0F)) {  // NaN-safe (E18): NaN fails every `>` comparison
        return 0;                               // vsync paces us
    }
    const float budgetMs = 1000.0F / unfocusedCapHz;
    const float remaining = budgetMs - frameElapsedMs;
    if (!(remaining > 0.0F)) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::lround(remaining));
}

EditorApp::EditorApp(ImGuiLayer layer, platform::Context& ctx, EditorAppConfig config)
    : layer(std::move(layer)), ctx(&ctx), config(std::move(config)) {}

// task 2.5.1: defined HERE, out-of-line, where DialogChannel is a complete type -- forced by
// std::shared_ptr<DialogChannel>'s deleter needing completeness wherever it could run, including this
// destructor and both moves (the scene_snapshot.hpp precedent, 2.4.2 §A13, applied one layer up).
EditorApp::~EditorApp() = default;
EditorApp::EditorApp(EditorApp&&) noexcept = default;
EditorApp& EditorApp::operator=(EditorApp&&) noexcept = default;

std::optional<EditorApp> EditorApp::create(rhi::Device& device, platform::Window& window, platform::Context& ctx,
                                           const EditorAppConfig& config) {
    // Task 2.2.5 (D5): install the console's log sink BEFORE ANYTHING LOGS, so registerEditorReflection()'s
    // tools-OFF WARN, the assets root and "shell ready" -- everything create() itself emits -- are all
    // already in the panel the first time it draws (AC-2). A local std::optional, so the
    // `return std::nullopt` below detaches it by RAII rather than by a cleanup branch. Moved into the
    // panel at registration.
    //
    // ViewportPanel's tools-OFF shader WARN is deliberately NOT in that list: it is emitted from
    // ensureInitialized() on the panel's FIRST DRAW (viewport_panel.cpp:95, called from onDraw), not
    // from its constructor. tick() pumps before the draw walk, so a record raised during frame N's draw
    // reaches the history at frame N+1 -- correct and unavoidable ordering, but it means the WARN is one
    // frame later than the create()-time lines, not present alongside them.
    std::optional<LogSinkScope> logScope;
    if (config.registerDefaultPanels) {
        logScope.emplace();
        AERO_LOG_INFO("editor: console log sink attached ({} staged / {} held)", logScope->sink()->stagingCapacity(),
                      DEFAULT_LOG_HISTORY_CAPACITY);
    }
    registerEditorReflection();  // task 2.2.2 -- unconditional, once-per-process, before ImGuiLayer
    std::optional<ImGuiLayer> layer = ImGuiLayer::create(device, window, ctx, config.persistLayout);
    if (!layer) {
        return std::nullopt;  // ImGuiLayer already logged the reason
    }

    EditorApp app(std::move(*layer), ctx, config);
    app.window = &window;                                   // task 2.5.1, F14/A19
    app.dialogChannel = std::make_shared<DialogChannel>();  // never null on a LIVE app
    // The default layout is built on the FIRST DRAWN FRAME, not here (E3) — so panels registered by
    // the caller between create() and the first tick() are included.
    app.applyDefaultLayout = app.layer.wantsDefaultLayout();
    // task 2.6.1: the recents list and the project are resolved HERE -- the same position the old
    // project-root resolution occupied -- so the "assets root" INFO below keeps its exact text AND
    // its exact position among the panel records, and so AssetBrowserPanel is BORN with the right
    // root (which makes D10's reconcile a startup no-op rather than a one-frame correction).
    app.recentsPath =
        config.recentProjectsPath.empty() ? defaultRecentProjectsPath() : std::string(config.recentProjectsPath);
    if (config.restoreLastProject) {
        app.recents = readRecentProjects(app.recentsPath);  // D15: NOT read at all when false (AC-34/E23)
    }
    // D0's launch resolution order: argv/config -> the FIRST recents entry that HAS a project.json ->
    // none. The startup restore tries only that ONE entry: if it fails to VALIDATE we stop and show
    // Welcome rather than cascade, because silently opening a DIFFERENT project than the one you last
    // used is worse than showing Welcome (E16/E17/AC-33).
    std::string resolved(config.projectPath);
    if (resolved.empty() && config.restoreLastProject) {
        for (const std::string& candidate : app.recents.paths) {
            if (fileExists(candidate + "/" + std::string(PROJECT_FILE_NAME))) {
                resolved = candidate;
                break;
            }
        }
    }
    if (!resolved.empty()) {
        CommandContext cmd{app.sceneWorld, app.sceneSelection, app.rootOrder};
        ProjectContext projectContext{app.project, app.projectFlow, app.recents, AERO_ENGINE_VERSION};
        (void)openProjectPath(cmd, app.commandStack, app.session, projectContext, resolved);
        // openProjectPath logs its own ERROR or INFO. A FAILURE leaves the no-project state, which IS
        // AC-32's "the editor still opens, docks and quits" property -- not an error path to handle here.
    }

    if (config.registerDefaultPanels) {
        app.registry.emplace<HierarchyPanel>();                           // task 2.2.1 -- was a PlaceholderPanel
        app.registry.emplace<InspectorPanel>();                           // task 2.2.2 -- was a PlaceholderPanel
        app.viewportPanel = app.registry.emplace<ViewportPanel>(device);  // task 2.2.3 -- was a PlaceholderPanel
        // task 2.2.5 -- was a PlaceholderPanel. logScope is engaged exactly when this branch runs (both
        // guards read the same const config field), but the has_value() test is NOT defensive
        // programming: bugprone-unchecked-optional-access is --warnings-as-errors on the Linux Debug lane
        // and cannot be relied on to correlate two separate ifs across an intervening move (plan C3).
        if (logScope.has_value()) {
            app.consolePanel = app.registry.emplace<ConsolePanel>(std::move(*logScope));
        }
        AERO_LOG_INFO("editor: assets root '{}'", app.project.assetsRoot());
        app.assetBrowserPanel = app.registry.emplace<AssetBrowserPanel>(app.project.assetsRoot());
    }

    // task 2.6.1: `&& !app.project.isOpen()` is MANDATORY, not defensive. Opening a project above went
    // through adoptProject -> newScene -> resetSceneState + seedDefaultScene, so the World already
    // holds the three seed entities; seeding again would produce SIX. This is 2.5.1's S5 trap in a new
    // costume. Corollary, and it is CORRECT: opening a project always yields three entities, even with
    // seedDefaultScene == false -- AC-18 says a project switch resets to a FRESH DEFAULT scene.
    if (config.seedDefaultScene && !app.project.isOpen()) {
        engine::editor::seedDefaultScene(app.sceneWorld);  // fully qualified: the config field shadows
    }

    // This is the evidence the non-interactive launch check greps for (§V5).
    AERO_LOG_INFO("editor: shell ready ({} panels, {} entities, layout: {})", app.registry.count(),
                  app.sceneWorld.entityCount(), app.applyDefaultLayout ? "default" : "restored");
    return app;
}

bool EditorApp::tick() {
    if (!running) {
        presented = false;  // nothing reached the screen this tick — keep presentedLastFrame() honest
        return false;       // idempotent after quit (E10) — calls no ImGui function
    }
    const double frameStart = monotonicSeconds();

    ctx->newFrame();
    platform::Event ev;
    while (ctx->pollEvent(ev)) {  // ImGui sees every raw event via the D5 sink
        switch (ev.type) {
            case platform::EventType::Quit:
            case platform::EventType::WindowClose:
                requestGuardedQuit();  // task 2.5.1, D1: route the OS quit / window [X] through the
                                       // guard instead of quitting immediately. Observable consequence:
                                       // closing a CLEAN editor now draws one more frame before exiting
                                       // -- deliberate (AC-28), not a regression.
                break;
            case platform::EventType::WindowFocusGained:
                windowFocused = true;
                break;
            case platform::EventType::WindowFocusLost:
                windowFocused = false;
                break;
            default:
                break;  // resize is the swapchain's business
        }
    }
    if (!running) {
        // Reachable only through a DIRECT requestQuit() call (D14) -- the window [X] / OS Quit path
        // above no longer flips `running` here, it only requests the guard. Kept for the same reason
        // it always existed: a caller that quits before this tick even begins must still balance
        // ImGui correctly (no NewFrame means nothing to balance).
        presented = false;
        return false;
    }
    frameClock.tick();

    // Task 2.2.5 (D14): drain the sink EVERY frame, visible or not -- shell_ui.cpp:74-79 never calls
    // onDraw for a hidden or tabbed-away panel, and Console shares its dock node with Assets. Not an
    // ImGui call, and deliberately BEFORE the draw walk so this frame's rows include everything logged
    // before it; a record emitted DURING a draw lands next frame, which is the only safe ordering (E3).
    if (consolePanel != nullptr) {
        consolePanel->pumpLog();
    }

    // task 2.5.1: the dialog result is taken BEFORE the frame, so THIS frame's menu, modal and panels
    // all see the post-load scene -- the F12 property (2.4.1 D19), one step earlier, because the
    // RESULT is not a UI event. Guarded: a moved-from EditorApp has a null channel and is deliberately
    // inert (plan A18) rather than crashing on a null dereference.
    // task 2.6.1: everything the project half of the flow needs, built fresh each frame -- the
    // PanelContext/FileMenuContext shape, and for the same reason (D7). AERO_ENGINE_VERSION is read
    // at this ONE call site (D14).
    ProjectContext projectContext{project, projectFlow, recents, AERO_ENGINE_VERSION};
    if (dialogChannel != nullptr) {
        if (const DialogResult result = dialogChannel->take(); result.ready) {
            CommandContext cmd{sceneWorld, sceneSelection, rootOrder};
            // task 2.6.1: FileDialogHost::projectRoot is a std::string_view. Binding it DIRECTLY to
            // project.scenesRoot() -- which returns BY VALUE -- leaves it dangling the instant the
            // full-expression ends. The named local is MANDATORY, not style.
            const std::string scenesRoot = project.scenesRoot();
            const FileDialogHost host{dialogChannel.get(), nativeWindowHandle(window), scenesRoot};
            applyDialogResult(cmd, commandStack, session, fileFlow, host, result, projectContext);
        }
    }

    // task 2.6.1 (D10/AC-31): RECONCILE, never push. The swap does not call setRoot() -- if it did,
    // the swap would be two things a future caller could perform half of, and a stale Asset Browser
    // root after a project change is exactly the kind of drift nobody notices until it is confusing.
    // One std::string comparison per frame, which cannot be half-performed and cannot drift, and
    // which works identically whether the project changed via the menu, the Welcome window, argv, the
    // startup restore, or a caller nobody has written yet. It is the RootOrder/Hierarchy precedent
    // (editor_app.hpp:120-124) applied to a second panel. Null-checked: registerDefaultPanels == false
    // leaves the pointer null and this a no-op (E26). ONE allocation per frame, deliberately -- the
    // title push below has allocated one on every frame since task 2.5.1.
    if (assetBrowserPanel != nullptr) {
        std::string wanted = project.assetsRoot();
        if (assetBrowserPanel->root() != wanted) {
            assetBrowserPanel->setRoot(std::move(wanted));
        }
    }
    if (window != nullptr) {
        std::string title = session.windowTitle(!commandStack.isClean(), project.name());
        if (title != lastTitle) {  // D16: push the title only when it CHANGES
            lastTitle = std::move(title);
            window->setTitle(lastTitle);
        }
    }
    // The recents list is flushed ONLY when it changed -- never per-frame file I/O, ever.
    if (projectFlow.recentsDirty) {
        projectFlow.recentsDirty = false;
        writeRecentProjects(recentsPath, recents);  // one WARN on failure; the editor keeps running
    }  // and the project stays open (AC-24)

    layer.beginFrame();
    ShellUiState ui{
        .applyDefaultLayout = applyDefaultLayout, .undoRequested = undoRequested, .redoRequested = redoRequested};
    // Consumed: a request never survives the tick that carried it. Unlike applyDefaultLayout below,
    // these are NOT read back out of `ui` -- drawShellUi clears them as it applies them, and reading
    // them back would re-arm the request every frame (task 2.4.1).
    undoRequested = false;
    redoRequested = false;
    // rebuilt per frame (D7); deltaSeconds is this frame's SPIKE-CLAMPED delta (task 2.3.1);
    // commandStack is the editor's ONE undo history (task 2.4.1 D7); rootOrder is the editor's ONE
    // display order among root entities (task 2.4.2 D10)
    PanelContext panelContext{sceneWorld, sceneSelection, commandStack, rootOrder, frameClock.deltaSeconds()};
    // task 2.5.1 (plan A14): everything the File menu needs that PanelContext deliberately does not
    // carry (D17), built fresh each frame exactly like panelContext above.
    // task 2.6.1: the SAME named-local requirement as the drain above -- FileDialogHost::projectRoot
    // is a string_view and project.scenesRoot() returns by value.
    const std::string scenesRootForMenu = project.scenesRoot();
    FileMenuContext fileMenu{session, fileFlow,
                             FileDialogHost{dialogChannel.get(), nativeWindowHandle(window), scenesRootForMenu},
                             projectContext};
    drawShellUi(registry, panelContext, ui, fileMenu);  // menu bar -> dockspace -> panels
    applyDefaultLayout = ui.applyDefaultLayout;         // drawShellUi clears it once consumed, and re-sets
                                                        // it for View > Reset Layout
    // D3: the offscreen scene pass runs AFTER the draw walk (only it knows this frame's panel size,
    // which is what removes the one-frame resize lag) and BEFORE endFrame (ImGui's command buffer is
    // acquired and submitted there; ours must be submitted first -- F8's ordering guarantee, and F7
    // leaves the colour texture sampler-readable the instant our pass ends). NOT an ImGui call.
    if (viewportPanel != nullptr) {
        viewportPanel->renderScene(sceneWorld);
    }
    presented = layer.endFrame(config.clearColor);
    if (fileFlow.quitConfirmed) {
        // File > Exit / Ctrl+Q / the window [X] -- all AFTER the guard said yes (task 2.5.1 D1). This
        // frame still completed, so Render stays balanced (AC-28).
        running = false;
    }
    AERO_PROFILE_FRAME_MARK;

    const auto elapsedMs = static_cast<float>((monotonicSeconds() - frameStart) * 1000.0);
    if (const std::uint32_t sleepMs = framePaceSleepMs(presented, windowFocused, config.unfocusedFrameCapHz, elapsedMs);
        sleepMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
    return running;
}

int EditorApp::run() {
    while (tick()) {
    }
    return 0;
}

PanelRegistry& EditorApp::panels() noexcept { return registry; }
const PanelRegistry& EditorApp::panels() const noexcept { return registry; }
World& EditorApp::world() noexcept { return sceneWorld; }
const World& EditorApp::world() const noexcept { return sceneWorld; }
Selection& EditorApp::selection() noexcept { return sceneSelection; }
const Selection& EditorApp::selection() const noexcept { return sceneSelection; }
CommandStack& EditorApp::commands() noexcept { return commandStack; }
const CommandStack& EditorApp::commands() const noexcept { return commandStack; }
RootOrder& EditorApp::roots() noexcept { return rootOrder; }
const RootOrder& EditorApp::roots() const noexcept { return rootOrder; }
const FrameClock& EditorApp::clock() const noexcept { return frameClock; }
bool EditorApp::focused() const noexcept { return windowFocused; }
bool EditorApp::presentedLastFrame() const noexcept { return presented; }
std::size_t EditorApp::logRecordCount() const noexcept {
    return consolePanel != nullptr ? consolePanel->history().size() : std::size_t{0};
}
EditorCamera* EditorApp::viewportCamera() noexcept {
    return viewportPanel != nullptr ? &viewportPanel->camera() : nullptr;
}
const EditorCamera* EditorApp::viewportCamera() const noexcept {
    return viewportPanel != nullptr ? &viewportPanel->camera() : nullptr;
}
std::string_view EditorApp::scenePath() const noexcept { return session.path(); }
bool EditorApp::sceneDirty() const noexcept { return !commandStack.isClean(); }

bool EditorApp::projectIsOpen() const noexcept { return project.isOpen(); }
std::string_view EditorApp::projectRoot() const noexcept { return project.root(); }
std::string_view EditorApp::projectName() const noexcept { return project.name(); }
std::string_view EditorApp::assetBrowserRoot() const noexcept {
    return assetBrowserPanel != nullptr ? std::string_view(assetBrowserPanel->root()) : std::string_view{};
}
std::size_t EditorApp::recentProjectCount() const noexcept { return recents.paths.size(); }

void EditorApp::requestQuit() noexcept { running = false; }
void EditorApp::requestLayoutReset() noexcept { applyDefaultLayout = true; }
void EditorApp::requestUndo() noexcept { undoRequested = true; }
void EditorApp::requestRedo() noexcept { redoRequested = true; }

// task 2.5.1: each hook is `fileFlow.requested = FileAction::X;` (plus `.requestedPath` for the two
// path-taking ones), applied on the NEXT tick -- the requestLayoutReset()/requestUndo() shape, not
// requestQuit()'s (D14/AC-29 -- requestQuit() above stays byte-identical; 35 GPU-gated cases depend
// on it, plan A4).
void EditorApp::requestNewScene() noexcept { fileFlow.requested = FileAction::NewScene; }
void EditorApp::requestOpenSceneDialog() noexcept { fileFlow.requested = FileAction::OpenScene; }
void EditorApp::requestOpenScene(std::string_view path) {
    fileFlow.requested = FileAction::OpenScene;
    fileFlow.requestedPath = path;
}
void EditorApp::requestSaveScene() noexcept { fileFlow.requested = FileAction::SaveScene; }
void EditorApp::requestSaveSceneAs(std::string_view path) {
    fileFlow.requested = FileAction::SaveSceneAs;
    fileFlow.requestedPath = path;
}
void EditorApp::requestGuardedQuit() noexcept { fileFlow.requested = FileAction::Quit; }

// task 2.6.1: the SAME requestUndo()/requestLayoutReset() shape -- applied on the NEXT tick, never
// immediately. requestOpenProject(path) is what makes the entire project flow drivable through real
// frames from the ImGui-free aero_editor_imgui_test, exactly as requestOpenScene(path) did for 2.5.1.
void EditorApp::requestNewProject() noexcept { fileFlow.requested = FileAction::NewProject; }
void EditorApp::requestOpenProjectDialog() noexcept { fileFlow.requested = FileAction::OpenProject; }
void EditorApp::requestOpenProject(std::string_view path) {
    fileFlow.requested = FileAction::OpenProject;
    projectFlow.requestedPath = path;
}
void EditorApp::requestClearRecentProjects() noexcept { projectFlow.clearRecentsRequested = true; }

}  // namespace engine::editor

// F15/2.4.1's precedent, applied to EditorApp itself: this type stays noexcept-movable, so a future
// member whose move can throw fails HERE, loudly, instead of silently degrading EditorApp's own
// defaulted... spelled-out move (task 2.5.1: no longer `= default` IN THE HEADER, so the assert
// cannot live there any more -- it moves here, where EditorApp is complete). AC-34's mechanical proof.
static_assert(std::is_nothrow_move_constructible_v<engine::editor::EditorApp>);
static_assert(std::is_nothrow_move_assignable_v<engine::editor::EditorApp>);

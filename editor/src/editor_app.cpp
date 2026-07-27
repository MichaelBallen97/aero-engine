// Aero Engine — EditorApp: create/tick/run/pacing (task 2.1.3, D1/D4/D11). ImGui-FREE — includes
// shell_ui.hpp (src-private) but never ImGui itself; drawShellUi is the only ImGui-touching call.
#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/time.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/editor_app.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/project_files.hpp>
#include <aero/platform/context.hpp>
#include <aero/platform/event.hpp>

#include "asset_browser_panel.hpp"
#include "console_panel.hpp"
#include "editor_reflection.hpp"
#include "hierarchy_panel.hpp"
#include "inspector_panel.hpp"
#include "shell_ui.hpp"
#include "viewport_panel.hpp"

#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace engine::editor {

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

std::optional<EditorApp> EditorApp::create(rhi::Device& device, platform::Window& window, platform::Context& ctx,
                                           const EditorAppConfig& config) {
    // Task 2.2.5 (D5): install the console's log sink BEFORE ANYTHING LOGS, so registerEditorReflection()'s
    // tools-OFF WARN, ViewportPanel's tools-OFF shader WARN, the assets root and "shell ready" are all
    // already in the panel the first time it draws (AC-2). A local std::optional, so the
    // `return std::nullopt` below detaches it by RAII rather than by a cleanup branch. Moved into the
    // panel at registration.
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
    // The default layout is built on the FIRST DRAWN FRAME, not here (E3) — so panels registered by
    // the caller between create() and the first tick() are included.
    app.applyDefaultLayout = app.layer.wantsDefaultLayout();

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
        std::string assetsRoot = resolveProjectRoot(config.projectRoot);
        AERO_LOG_INFO("editor: assets root '{}'", assetsRoot);
        app.registry.emplace<AssetBrowserPanel>(std::move(assetsRoot));  // task 2.2.4 -- was a PlaceholderPanel
    }

    if (config.seedDefaultScene) {
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
                running = false;
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
        presented = false;  // as above: no frame was presented, so don't report a stale true
        return false;       // quit before any ImGui call (D11) — no NewFrame means nothing to balance
    }
    frameClock.tick();

    // Task 2.2.5 (D14): drain the sink EVERY frame, visible or not -- shell_ui.cpp:74-79 never calls
    // onDraw for a hidden or tabbed-away panel, and Console shares its dock node with Assets. Not an
    // ImGui call, and deliberately BEFORE the draw walk so this frame's rows include everything logged
    // before it; a record emitted DURING a draw lands next frame, which is the only safe ordering (E3).
    if (consolePanel != nullptr) {
        consolePanel->pumpLog();
    }

    layer.beginFrame();
    ShellUiState ui{.applyDefaultLayout = applyDefaultLayout, .quitRequested = false};
    PanelContext panelContext{sceneWorld, sceneSelection};  // rebuilt per frame (D7)
    drawShellUi(registry, panelContext, ui);                // menu bar -> dockspace -> panels
    applyDefaultLayout = ui.applyDefaultLayout;             // drawShellUi clears it once consumed, and re-sets
                                                            // it for View > Reset Layout
    // D3: the offscreen scene pass runs AFTER the draw walk (only it knows this frame's panel size,
    // which is what removes the one-frame resize lag) and BEFORE endFrame (ImGui's command buffer is
    // acquired and submitted there; ours must be submitted first -- F8's ordering guarantee, and F7
    // leaves the colour texture sampler-readable the instant our pass ends). NOT an ImGui call.
    if (viewportPanel != nullptr) {
        viewportPanel->renderScene(sceneWorld);
    }
    presented = layer.endFrame(config.clearColor);
    if (ui.quitRequested) {
        running = false;  // File>Exit / Ctrl+Q: this frame still completed, so Render stays balanced
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
const FrameClock& EditorApp::clock() const noexcept { return frameClock; }
bool EditorApp::focused() const noexcept { return windowFocused; }
bool EditorApp::presentedLastFrame() const noexcept { return presented; }
std::size_t EditorApp::logRecordCount() const noexcept {
    return consolePanel != nullptr ? consolePanel->history().size() : std::size_t{0};
}

void EditorApp::requestQuit() noexcept { running = false; }
void EditorApp::requestLayoutReset() noexcept { applyDefaultLayout = true; }

}  // namespace engine::editor

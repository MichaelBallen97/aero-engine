// Aero Engine — EditorApp: create/tick/run/pacing (task 2.1.3, D1/D4/D11). ImGui-FREE — includes
// shell_ui.hpp (src-private) but never ImGui itself; drawShellUi is the only ImGui-touching call.
#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/time.hpp>
#include <aero/editor/editor_app.hpp>
#include <aero/platform/context.hpp>
#include <aero/platform/event.hpp>

#include "placeholder_panel.hpp"
#include "shell_ui.hpp"

#include <chrono>
#include <cmath>
#include <optional>
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

EditorApp::EditorApp(ImGuiLayer layer, platform::Context& ctx, const EditorAppConfig& config)
    : layer(std::move(layer)), ctx(&ctx), config(config) {}

std::optional<EditorApp> EditorApp::create(rhi::Device& device, platform::Window& window, platform::Context& ctx,
                                           const EditorAppConfig& config) {
    std::optional<ImGuiLayer> layer = ImGuiLayer::create(device, window, ctx, config.persistLayout);
    if (!layer) {
        return std::nullopt;  // ImGuiLayer already logged the reason
    }

    EditorApp app(std::move(*layer), ctx, config);
    // The default layout is built on the FIRST DRAWN FRAME, not here (E3) — so panels registered by
    // the caller between create() and the first tick() are included.
    app.applyDefaultLayout = app.layer.wantsDefaultLayout();

    if (config.registerDefaultPanels) {
        app.registry.emplace<PlaceholderPanel>("Hierarchy", DockSlot::Left, "Hierarchy — placeholder (task 2.2.1)");
        app.registry.emplace<PlaceholderPanel>("Inspector", DockSlot::Right, "Inspector — placeholder (task 2.2.2)");
        app.registry.emplace<PlaceholderPanel>("Viewport", DockSlot::Center, "Viewport — placeholder (task 2.2.3)");
        app.registry.emplace<PlaceholderPanel>("Console", DockSlot::Bottom, "Console — placeholder (task 2.2.5)");
        app.registry.emplace<PlaceholderPanel>("Assets", DockSlot::Bottom, "Assets — placeholder (task 2.2.4)");
    }

    // This is the evidence the non-interactive launch check greps for (§V5).
    AERO_LOG_INFO("editor: shell ready ({} panels, layout: {})", app.registry.count(),
                  app.applyDefaultLayout ? "default" : "restored");
    return app;
}

bool EditorApp::tick() {
    if (!running) {
        return false;  // idempotent after quit (E10) — calls no ImGui function
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
        return false;  // quit before any ImGui call (D11) — no NewFrame means nothing to balance
    }
    frameClock.tick();

    layer.beginFrame();
    ShellUiState ui{.applyDefaultLayout = applyDefaultLayout, .quitRequested = false};
    drawShellUi(registry, ui);                   // menu bar -> dockspace -> panels
    applyDefaultLayout = ui.applyDefaultLayout;  // drawShellUi clears it once consumed, and re-sets
                                                 // it for View > Reset Layout
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
const FrameClock& EditorApp::clock() const noexcept { return frameClock; }
bool EditorApp::focused() const noexcept { return windowFocused; }
bool EditorApp::presentedLastFrame() const noexcept { return presented; }

void EditorApp::requestQuit() noexcept { running = false; }
void EditorApp::requestLayoutReset() noexcept { applyDefaultLayout = true; }

}  // namespace engine::editor

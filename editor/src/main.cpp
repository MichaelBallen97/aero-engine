// Aero Engine — aero_editor: the editor executable (task 2.1.1, opening Phase 2 / the /editor
// layer). Thin and ImGui-free: all ImGui/SDL_GPU plumbing lives in aero::editor_core. Mirrors the
// samples' shape (a runEditor()/main() split; RAII order ctx -> window -> device -> layer).
#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/time.hpp>
#include <aero/editor/editor_ui.hpp>
#include <aero/editor/imgui_layer.hpp>
#include <aero/platform/platform.hpp>
#include <aero/rhi/device.hpp>

#include <chrono>
#include <exception>
#include <optional>
#include <thread>

namespace {

// The real editor logic, split out of main() (docs/04: no exceptions across a public API boundary —
// main() is this executable's outermost one).
int runEditor() {
    using namespace engine;  // exe TU (not a header) — docs/04 forbids this only in headers

    platform::Context ctx;  // real driver — GPU + a real window need it
    if (!ctx.valid()) {
        AERO_LOG_CRITICAL("editor: platform init failed");
        return 1;
    }

    std::optional<platform::Window> window = ctx.createWindow({.title = "Aero Editor", .width = 1280, .height = 720});
    if (!window) {
        return 1;
    }

    std::optional<rhi::Device> device = rhi::Device::create();
    if (!device) {
        AERO_LOG_CRITICAL("editor: no GPU device");
        return 1;
    }

    std::optional<editor::ImGuiLayer> layer = editor::ImGuiLayer::create(*device, *window, ctx, /*persistLayout=*/true);
    if (!layer) {
        AERO_LOG_CRITICAL("editor: ImGuiLayer::create failed");
        return 1;
    }

    editor::EditorUiState ui;
    ui.applyDefaultLayout = layer->wantsDefaultLayout();

    FrameClock clock;
    bool running = true;
    while (running) {
        ctx.newFrame();
        platform::Event ev;
        while (ctx.pollEvent(ev)) {  // ImGui gets every raw event via the D5 sink
            if (ev.type == platform::EventType::Quit || ev.type == platform::EventType::WindowClose) {
                running = false;
            }
        }
        if (ctx.input().keyDown(platform::Key::Escape)) {
            running = false;
        }
        clock.tick();

        layer->beginFrame();
        editor::drawEditorUi(ui);
        if (!layer->endFrame(/*clearColor=*/{0.10F, 0.10F, 0.12F, 1.0F})) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));  // minimized: don't spin
        }
        AERO_PROFILE_FRAME_MARK;
    }

    return 0;
}

}  // namespace

int main() {
    try {
        return runEditor();
    } catch (const std::exception& e) {
        AERO_LOG_CRITICAL("aero_editor: unexpected exception: {}", e.what());
        return 1;
    } catch (...) {
        AERO_LOG_CRITICAL("aero_editor: unexpected exception");
        return 1;
    }
}

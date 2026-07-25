// Aero Engine — aero_editor: the editor executable. Thin bootstrap over EditorApp (task 2.1.3):
// Context -> Window -> Device -> EditorApp::create()/run(). The frame loop, event handling and every
// ImGui call live in aero::editor_core (EditorApp::tick()) — this file is ImGui-free.
#include <aero/core/log.hpp>
#include <aero/editor/editor_app.hpp>
#include <aero/platform/platform.hpp>
#include <aero/rhi/device.hpp>

#include <exception>
#include <optional>

namespace {
// docs/04: no exceptions across a public API boundary — main() is this executable's outermost one.
int runEditor() {
    using namespace engine;  // exe TU (not a header) — docs/04 forbids this only in headers
    platform::Context ctx;
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
    // RAII order ctx -> window -> device -> app is load-bearing: ~EditorApp runs before ~Device/~Window/~Context.
    std::optional<editor::EditorApp> app = editor::EditorApp::create(*device, *window, ctx);
    if (!app) {
        AERO_LOG_CRITICAL("editor: EditorApp::create failed");
        return 1;
    }
    return app->run();
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

// aero_editor_imgui_test is a standalone single-TU target (no shared tests/test_main.cpp) -- it
// provides doctest's own main() here, unlike aero_tests' TEST_CASE files (the aero_reflect_meta_test
// precedent). GPU-gated at RUNTIME via AERO_REQUIRE_GPU (rhi_test_support.hpp): unset locally skips
// loudly; set (CI) a missing GPU/display is a hard failure. ImGui-free at source -- this TU drives
// aero::editor_core's engine-typed API only; imgui/SDL3 reach it purely transitively through
// editor_core's PRIVATE static archive (the glm-in-aero_tests precedent).
//
// G6 (window visibility): uses a small VISIBLE 320x180 window, matching the rhi_swapchain_test
// precedent that is proven to present on all three CI lanes (macOS Metal, Windows WARP, Linux
// lavapipe under xvfb). A hidden window presented fine on macOS Metal locally, but hidden-window
// presentation is unproven on the lavapipe/WARP lanes; since every endFrame() below asserts the
// frame presented, we take the proven visible path. The brief flash matches rhi_swapchain_test.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <aero/editor/editor_ui.hpp>
#include <aero/editor/imgui_layer.hpp>
#include <aero/platform/platform.hpp>
#include <aero/rhi/device.hpp>

#include "rhi_test_support.hpp"

#include <doctest/doctest.h>

TEST_CASE("editor: ImGuiLayer init -> frames -> shutdown (GPU-gated smoke test)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }

    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "editor smoke", .width = 320, .height = 180});
    REQUIRE(window.has_value());

    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    std::optional<engine::editor::ImGuiLayer> layer =
        engine::editor::ImGuiLayer::create(*device, *window, ctx, /*persistLayout=*/false);
    REQUIRE(layer.has_value());

    engine::editor::EditorUiState ui;
    ui.applyDefaultLayout = layer->wantsDefaultLayout();

    for (int i = 0; i < 3; ++i) {
        layer->beginFrame();
        engine::editor::drawEditorUi(ui);
        CHECK(layer->endFrame(engine::rhi::Color{0.1F, 0.1F, 0.12F, 1.0F}));
    }

    // Regression (code-review Gap 1): a panel's close-'X' sets its show flag false. An unbalanced
    // End() would then over-call ImGui::End() and abort (IM_ASSERT) in the Debug ImGui build. Drive
    // frames with panels closed -- first some, then all -- to prove End() stays balanced (no crash).
    ui.showHierarchy = false;
    ui.showConsole = false;
    layer->beginFrame();
    engine::editor::drawEditorUi(ui);
    CHECK(layer->endFrame(engine::rhi::Color{0.1F, 0.1F, 0.12F, 1.0F}));

    ui.showInspector = false;
    ui.showViewport = false;
    layer->beginFrame();
    engine::editor::drawEditorUi(ui);
    CHECK(layer->endFrame(engine::rhi::Color{0.1F, 0.1F, 0.12F, 1.0F}));

    layer.reset();  // teardown must not crash; LSan (Linux Debug) proves leak-free
}

// aero_editor_imgui_test is a standalone single-TU target (no shared tests/test_main.cpp) -- it
// provides doctest's own main() here, unlike aero_tests' TEST_CASE files (the aero_reflect_meta_test
// precedent). GPU-gated at RUNTIME via AERO_REQUIRE_GPU (rhi_test_support.hpp): unset locally skips
// loudly; set (CI) a missing GPU/display is a hard failure. ImGui-free at source -- this TU drives
// aero::editor_core's engine-typed API only (task 2.1.3: EditorApp::tick(), not a hand-rolled loop);
// imgui reaches it purely transitively through editor_core's PRIVATE static archive (the
// glm-in-aero_tests precedent). SDL3 reaches it BOTH transitively AND directly as of task 2.3.3's
// I5 case, which needs SDL_GetBasePath() to compute the SAME exe-relative aero_editor.ini path
// ImGuiLayer::create's deriveIniPath() does (imgui_layer.cpp) -- there is no test-supplied "scratch
// directory" option in EditorAppConfig, so this is the only way to find the file the real app wrote.
//
// G6 (window visibility): uses a small VISIBLE 320x180 window, matching the rhi_swapchain_test
// precedent that is proven to present on all three CI lanes (macOS Metal, Windows WARP, Linux
// lavapipe under xvfb). A hidden window presented fine on macOS Metal locally, but hidden-window
// presentation is unproven on the lavapipe/WARP lanes; since every tick() below asserts the frame
// presented, we take the proven visible path. The brief flash matches rhi_swapchain_test.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <aero/core/log.hpp>              // AERO_LOG_* + initLogging (cases B and C)
#include <aero/editor/command_stack.hpp>  // task 2.4.1
#include <aero/editor/component_ops.hpp>
#include <aero/editor/console_model.hpp>  // DEFAULT_LOG_HISTORY_CAPACITY (case C)
#include <aero/editor/editor_app.hpp>
#include <aero/editor/editor_camera.hpp>    // task 2.3.1
#include <aero/editor/entity_commands.hpp>  // task 2.4.2
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/panel_registry.hpp>
#include <aero/editor/picking.hpp>       // task 2.3.2
#include <aero/editor/scene_bounds.hpp>  // task 2.3.1
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/selection_overlay.hpp>  // task 2.3.2
#include <aero/editor/transform_command.hpp>  // task 2.4.1
#include <aero/editor/transform_ops.hpp>      // task 2.4.1
#include <aero/platform/platform.hpp>
#include <aero/rhi/device.hpp>
#include <aero/scene/scene.hpp>
#include <aero/scene/world.hpp>

#include "rhi_test_support.hpp"

#include <SDL3/SDL_filesystem.h>  // task 2.3.3 I5: SDL_GetBasePath(), matching imgui_layer.cpp:38
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>  // task 2.4.1: std::make_unique<TransformCommand>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>  // std::error_code -- the non-throwing filesystem::remove overload
#include <type_traits>   // task 2.4.2, I11: std::is_nothrow_move_constructible_v/assignable_v
#include <vector>

namespace {
// Case C emits 12 000 records. console=false keeps them out of the suite's output; the destructor
// restores the engine defaults. initLogging() does NOT clear the callback (log.cpp:81-88 touches only
// the logger and the level), so this cannot detach the console panel's own sink -- which is exactly
// what makes it usable here.
struct QuietTraceLogging {
    QuietTraceLogging() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~QuietTraceLogging() { engine::initLogging(); }
    QuietTraceLogging(const QuietTraceLogging&) = delete;
    QuietTraceLogging& operator=(const QuietTraceLogging&) = delete;
    QuietTraceLogging(QuietTraceLogging&&) = delete;
    QuietTraceLogging& operator=(QuietTraceLogging&&) = delete;
};

// task 2.5.1 (I12-I17): a fresh, never-yet-existing absolute path in the OS temp directory -- these
// cases save/open a single FILE, not a whole directory tree, so the TempDir class the tier-0 scene
// tests use would be overkill here. Never removed proactively: the OS temp directory is reclaimed by
// the OS, and every case here writes at most one small scene file.
[[nodiscard]] std::string uniqueScenePath(std::string_view suffix) {
    static int counter = 0;
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::filesystem::path file =
        dir / ("aero_imgui_layer_scene_" + std::to_string(++counter) + std::string(suffix));
    const std::u8string bytes = file.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
}  // namespace

TEST_CASE("editor: EditorApp create -> tick -> quit -> teardown (GPU-gated smoke test)") {
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

    // persistLayout = false: no ini written by this test. unfocusedFrameCapHz = 0: the pacing
    // throttle is disabled, so ticks stay unpaced and deterministic.
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    // The D8 registration -- the ONE absolute panel count in the tree.
    CHECK(app->panels().count() == 5);

    // wantsDefaultLayout() is true even with persistLayout = false (F1b), so frame 1 below exercises
    // buildDefaultLayout for real. Use REQUIRE on tick(), not CHECK: a spurious Quit/WindowClose from
    // the window manager would otherwise make presentedLastFrame() assert on a stale value and
    // produce a confusing failure -- REQUIRE fails at the right line.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    // The closed-panel regression (2.1.1 code-review Gap 1), restated through the registry: an
    // unbalanced End() would over-call ImGui::End() and abort (IM_ASSERT) in the Debug ImGui build.
    // Hide some panels, then all, and keep ticking -- no abort proves Begin/End stayed balanced.
    app->panels().setVisible("Hierarchy", false);
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    app->panels().setVisible("Inspector", false);
    app->panels().setVisible("Viewport", false);
    app->panels().setVisible("Assets", false);
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    // The DockBuilder path runs live on a real context without crashing (AC-7's mechanical half).
    app->requestLayoutReset();
    REQUIRE(app->tick());

    // E10: idempotent after quit -- and it must draw NOTHING, not merely return false. The return
    // value alone cannot tell the two apart: with both `if (!running) return false;` guards removed,
    // a full frame would draw, present, and still `return running;` == false. frameClock only
    // advances inside a drawn frame, so pinning frameCount() is what actually proves the early exit.
    const std::uint64_t framesBeforeQuit = app->clock().frameCount();
    app->requestQuit();
    CHECK(app->tick() == false);
    CHECK(app->tick() == false);
    CHECK(app->clock().frameCount() == framesBeforeQuit);
    CHECK_FALSE(app->presentedLastFrame());  // no frame reached the screen after the quit

    app.reset();  // teardown must not crash; LSan (Linux Debug) proves leak-free
}

TEST_CASE("editor: the Hierarchy panel draws a seeded scene and survives edits (task 2.2.1)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "hierarchy smoke", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    // AC-18: the default config seeds three entities.
    CHECK(app->world().entityCount() == 3);
    CHECK(app->panels().count() == 5);
    CHECK(app->selection().empty());

    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    // Build a two-level tree BEHIND the panel's back and keep ticking: phase 1 must reconcile it in
    // one frame (E27). `parent` draws COLLAPSED here -- nothing in this test ever opens it (no mouse,
    // no SetNextItemOpen/_DefaultOpen) -- which exercises the closed/non-leaf TreePop-skip path; it
    // does NOT draw `child` at all, so this does NOT reach the expanded/child-descent path. That pure
    // stack/arena traversal (exactly-once child visitation, back-to-front root order, the child arena
    // returning to its pre-call size) is instead proven at tier-0, with no GPU, by
    // hierarchy_test.cpp's `walkForest` cases (review round 2, Gap 1).
    engine::World& world = app->world();
    const engine::Entity parent = engine::editor::createEntity(world, {}, "Parent");
    const engine::Entity child = engine::editor::createEntity(world, parent, "Child");
    REQUIRE(parent.valid());
    REQUIRE(child.valid());
    app->selection().set(parent);  // `parent` DOES draw (a root) -- exercises _Selected on a real row
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    // Destroy the subtree behind its back: prune + reconcile must leave no dead handle anywhere.
    REQUIRE(world.destroy(parent));
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    CHECK(app->selection().empty());  // the pruned selection followed the deletion (I5)

    // Duplicate through the same seam the panel uses, then keep drawing.
    const std::vector<engine::Entity> copies = engine::editor::duplicateEntities(world, app->selection().entities());
    CHECK(copies.empty());  // empty selection -> no-op (E13)
    REQUIRE(app->tick());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: seedDefaultScene = false yields an empty tree (AC-18)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "hierarchy empty", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = false, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    CHECK(app->world().entityCount() == 0);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());  // an EMPTY tree must draw cleanly too
        CHECK(app->presentedLastFrame());
    }
    app.reset();
}

TEST_CASE("editor: the Inspector panel draws a seeded scene and survives structural edits (task 2.2.2, AC-9/AC-14)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "inspector smoke", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    CHECK(world.entityCount() == 3);

    // Find the seeded "Cube" entity (Transform + MeshRenderer) -- exercises every widget path:
    // Vec3, Quat (via Transform), a ranged uint32 selector and a colour Vec3 (via MeshRenderer).
    engine::Entity cube{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());

    app->selection().set(cube);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    // The "(N selected)" multi-select note: select every entity, one more tick.
    std::vector<engine::Entity> all;
    world.eachEntity([&](engine::Entity e) { all.push_back(e); });
    app->selection().setAll(all);
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    // Structural edits BEHIND the panel's back, between ticks (E3): the model is rebuilt fresh
    // every frame (D15), so removing then re-adding a component the panel just drew must not crash.
    app->selection().set(cube);
    REQUIRE(app->tick());
    const engine::ComponentTypeId meshRendererId = world.findComponentType("engine::MeshRenderer");
    REQUIRE(meshRendererId.valid());
    CHECK(engine::editor::removeComponent(world, cube, meshRendererId));
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    CHECK(engine::editor::addComponent(world, cube, meshRendererId));
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the Viewport panel drives resize, hide/show and no-camera without crashing (task 2.2.3)") {
    // Honest limit: this proves NO CRASH, NO ABORT, NO LEAK, and that frames present. It does NOT
    // prove the image is correct, crisp, or artifact-free -- no pixel readback exists in this
    // harness, and lavapipe would not settle the HiDPI question anyway. That half is
    // editor/VALIDATION.md's, exactly as 2.1.1/2.2.1/2.2.2 recorded for their own interactive halves.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "viewport smoke", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());
    CHECK(app->world().entityCount() == 3);

    // 1. Three plain ticks -- proves the offscreen pass, the submit ordering and ImGui's sample of
    // the texture all survive on real Metal/D3D12-WARP/lavapipe. An unbalanced ImGui call would
    // IM_ASSERT-abort in the Debug build; a bad ImTextureID would fault inside the backend.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    // 2. Resize -- the mechanical half of AC-4: reallocation, UV recomputation and the barrier path
    // in one shot.
    window->setSize(480, 300);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    // 3. Hide/show (AC-7): a hidden panel still presents (ImGui's own frame), but records no
    // offscreen work; re-showing resumes correctly.
    app->panels().setVisible("Viewport", false);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    app->panels().setVisible("Viewport", true);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    // 4. No camera (AC-8): destroy the seeded camera BEHIND the panel's back, between ticks -- no
    // crash, still presenting.
    engine::World& world = app->world();
    engine::Entity mainCamera{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Main Camera") {
            mainCamera = e;
        }
    });
    REQUIRE(mainCamera.valid());
    REQUIRE(world.destroy(mainCamera));
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    // 5. Teardown clean, no leak WARN (AC-12; LSan on the Linux Debug lane is the mechanism).
    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the Asset browser panel draws a real directory tree without unbalancing ImGui (task 2.2.4)") {
    // Honest limit: this proves NO CRASH, NO ABORT, NO LEAK, and that frames present. It does NOT
    // prove the listing is CORRECT, that navigation works, or that the tree expands -- no ImGui
    // input can be synthesised here. The expand/collapse/navigate half is proven at tier-0 by
    // project_files_test.cpp's buildVisibleTree cases (no ImGui at all) plus editor/VALIDATION.md's
    // human pass -- exactly as 2.2.1 recorded for walkForest.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "assets smoke", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    // projectRoot = "" -> resolveProjectRoot falls back to the process working directory, which for
    // ctest is the build tree: a real, non-empty directory with real subdirectories and real files.
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .projectRoot = ""});
    REQUIRE(app.has_value());
    CHECK(app->panels().count() == 5);

    // LOAD-BEARING (plan C5): "Assets" shares DockSlot::Bottom with "Console", and Console registers
    // FIRST, so Console is the selected tab and the Assets window is never drawn -- onDraw would
    // never run and this whole test would prove nothing. Hiding Console leaves Assets alone in that
    // node, so it becomes the selected tab.
    app->panels().setVisible("Console", false);

    // 1. Three plain ticks. An unbalanced EndChild, a wrongly-called EndTable or a leaked PushID is
    // an IM_ASSERT ABORT in the Debug ImGui build, so a green run IS the assertion (AC-11).
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    // 2. Hide / re-show. Re-showing makes IsWindowAppearing() true, which drops the cache and forces
    // a full rescan on that frame (F15/AC-4) -- the D9 refresh path, exercised for real.
    app->panels().setVisible("Assets", false);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    app->panels().setVisible("Assets", true);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    // 3. Shrink the window until the panes have almost no content area (E13/AC-11): BeginChild may
    // return false and BeginTable may return false. Both asymmetric rules are exercised here, and
    // this is the only arm on which sabotages S9/S10 have a chance to bite.
    window->setSize(200, 120);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    window->setSize(480, 300);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    // 4. Teardown clean, no leak WARN (LSan on the Linux Debug lane is the mechanism).
    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the Asset browser draws its error state for an unusable root (task 2.2.4, E1)") {
    // Proves the D17 degradation path is DRAWN, not merely returned: a missing root must still open,
    // dock and quit cleanly. A relative literal is used deliberately -- it keeps <filesystem> out of
    // this GPU TU, and no such directory exists under the ctest working directory (the build tree).
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "assets missing-root", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .seedDefaultScene = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectRoot = "aero-nonexistent-root-2.2.4"});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);  // plan C5, as above
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the Console panel draws the engine log stream (task 2.2.5)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "console smoke", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    // The D8 registration -- the ONE absolute panel count in the tree (plan C3's proof).
    CHECK(app->panels().count() == 5);

    // The create()-time records are staged in the sink; only the pump fills the history.
    CHECK(app->logRecordCount() == 0);

    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    // The records emitted during create() (D5) landed on the first pump -- AC-2's mechanical half.
    CHECK(app->logRecordCount() > 0);

    const std::size_t afterFirstTick = app->logRecordCount();
    AERO_LOG_WARN("console: GPU case A probe warn");
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    const std::size_t afterWarn = app->logRecordCount();
    CHECK(afterWarn > afterFirstTick);

    AERO_LOG_ERROR("console: GPU case A probe error");
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    CHECK(app->logRecordCount() > afterWarn);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the Console panel captures records while HIDDEN (task 2.2.5, AC-6)") {
    // The D14 discriminator: the pump must live in tick(), never in onDraw. Sabotage S11 moves it and
    // this test fails immediately.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "console hidden", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    // Warm-up tick, BEFORE the measurement window starts: the Viewport panel initializes LAZILY on
    // its own first draw (viewport_panel.cpp's ensureInitialized(), called from onDraw()), and under
    // -DAERO_SHADER_TOOLS=OFF that first draw logs a one-time degradation WARN. Without this tick that
    // WARN lands inside the very frame used to "settle" Console's visibility below, is queued in the
    // sink (not yet pumped), and only reaches logRecordCount() on the NEXT pump -- landing squarely
    // inside this test's own measurement window and reddening the exact-delta assertion on a
    // tools-OFF configure even though nothing about Console's own behaviour is wrong. One extra tick
    // here lets any such one-time, panel-order-dependent diagnostic land and get pumped BEFORE
    // `before` is captured, which is what actually makes the exact-delta assertion sound in every
    // build configuration, not only the tools-ON one this case happened to be authored against.
    REQUIRE(app->tick());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());  // settle: the panel stops drawing from here on
    const std::size_t before = app->logRecordCount();
    constexpr int PROBE_COUNT = 20;
    for (int i = 0; i < PROBE_COUNT; ++i) {
        AERO_LOG_WARN("console: hidden-capture probe {}", i);
    }
    REQUIRE(app->tick());
    // EXACT, not >=. Nothing in engine/ or editor/ logs per frame: there is not a single AERO_LOG_TRACE
    // or AERO_LOG_DEBUG call site in the first-party tree, and every remaining site is a failure path or
    // a once-per-lifetime notice. So an extra record here means a real error fired and this SHOULD be
    // red. With the pump in onDraw instead of tick(), the delta is 0 and this fails immediately.
    CHECK(app->logRecordCount() - before == static_cast<std::size_t>(PROBE_COUNT));
    app->panels().setVisible("Console", true);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a full Console ring stays balanced and clipped (task 2.2.5, AC-13)") {
    const QuietTraceLogging quiet;  // declared BEFORE the app so it outlives it
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "console full ring", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    constexpr int BATCH_COUNT = 4;
    constexpr int BATCH_SIZE = 3000;  // comfortably under DEFAULT_LOG_STAGING_CAPACITY (4096), so
                                      // NOTHING is dropped at the sink -- this arm is about EVICTION
    for (int b = 0; b < BATCH_COUNT; ++b) {
        for (int i = 0; i < BATCH_SIZE; ++i) {
            AERO_LOG_INFO("console: bulk {} {}", b, i);  // INFO, never TRACE: TRACE compiles out under
        }  // NDEBUG (log.hpp:137-143) and macos-release
        REQUIRE(app->tick());  // would then assert on an empty ring
        CHECK(app->presentedLastFrame());
    }
    CHECK(app->logRecordCount() == engine::editor::DEFAULT_LOG_HISTORY_CAPACITY);
    window->setSize(200, 120);  // BeginChild can return false; the header row clips
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    window->setSize(480, 300);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ==================================================================================================
// task 2.3.1 -- the real editor camera driven through a real EditorApp::tick(). Honest limit, stated
// once here for all four cases: this proves NO CRASH, NO ABORT, NO LEAK, and that frames present, and
// that the World/Selection stay untouched by camera motion. It does NOT prove the image is correct,
// that any gesture FEELS right, or that ImGui-side input routing works -- no ImGui input can be
// synthesised in this harness (the 2.2.1/2.2.2/2.2.3/2.2.5 precedent). That half is editor/VALIDATION.md's
// human pass.
// ==================================================================================================

TEST_CASE("editor: the editor camera drives the viewport with zero scene Cameras (task 2.3.1, AC-1)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "editor camera smoke", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    // Destroy the seeded "Main Camera" -- the World now holds zero Camera components.
    engine::World& world = app->world();
    engine::Entity mainCamera{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Main Camera") {
            mainCamera = e;
        }
    });
    REQUIRE(mainCamera.valid());
    REQUIRE(world.destroy(mainCamera));

    std::vector<std::string> warnMessages;
    engine::setLogCallback([&warnMessages](const engine::LogRecord& r) {
        if (r.level >= engine::LogLevel::Warn) {
            warnMessages.emplace_back(r.message);
        }
    });

    // Before this task this scene rendered nothing but the clear. This is what S5 also reds.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    engine::setLogCallback({});  // detach before any REQUIRE below could otherwise leak it
    const bool sawNoCameraWarn = std::any_of(warnMessages.begin(), warnMessages.end(), [](const std::string& m) {
        return m.find("no Camera in world") != std::string::npos;
    });
    CHECK_FALSE(sawNoCameraWarn);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: EditorApp::viewportCamera() (task 2.3.1, D6)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "viewport camera accessor", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::editor::EditorCamera* camera = app->viewportCamera();
    REQUIRE(camera != nullptr);
    const engine::editor::EditorCamera* constCamera =
        const_cast<const engine::editor::EditorApp*>(&*app)->viewportCamera();
    CHECK(constCamera == camera);  // the const/non-const overloads agree by address

    // The pose immediately after create() is D8's default.
    using namespace engine::editor;
    CHECK(camera->pivot() == DEFAULT_PIVOT);
    CHECK(camera->distance() == DEFAULT_DISTANCE);
    CHECK(camera->yaw() == DEFAULT_YAW_RADIANS);
    CHECK(camera->pitch() == DEFAULT_PITCH_RADIANS);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();

    // E23/E13: null when no Viewport panel is registered.
    std::optional<engine::editor::EditorApp> bareApp = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .registerDefaultPanels = false, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(bareApp.has_value());
    CHECK(bareApp->viewportCamera() == nullptr);
    bareApp->requestQuit();
    CHECK(bareApp->tick() == false);
    bareApp.reset();
}

TEST_CASE("editor: programmatic camera navigation mutates no World, no Selection (task 2.3.1, AC-19)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "camera nav survives a real frame", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    const engine::World& world = app->world();
    const std::size_t entityCountBefore = world.entityCount();
    const std::size_t transformCountBefore = world.componentCount<engine::Transform>();
    const std::size_t cameraCountBefore = world.componentCount<engine::Camera>();
    const std::size_t meshCountBefore = world.componentCount<engine::MeshRenderer>();
    const std::size_t dirLightCountBefore = world.componentCount<engine::DirectionalLight>();
    const std::size_t pointLightCountBefore = world.componentCount<engine::PointLight>();
    const std::size_t selectionCountBefore = app->selection().count();

    engine::editor::EditorCamera* camera = app->viewportCamera();
    REQUIRE(camera != nullptr);

    using engine::editor::CameraGesture;
    using engine::editor::CameraInput;
    camera->update(CameraInput{.dragDelta = {10.0F, 5.0F}, .gesture = CameraGesture::Orbit}, 1.0F / 60.0F);
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    camera->update(
        CameraInput{.dragDelta = {8.0F, 0.0F}, .viewportHeightPoints = 200.0F, .gesture = CameraGesture::Pan},
        1.0F / 60.0F);
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    camera->update(CameraInput{.wheelNotches = 2.0F}, 1.0F / 60.0F);
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    camera->update(CameraInput{.gesture = CameraGesture::Fly, .moveForward = true}, 1.0F / 60.0F);
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    camera->focusOn(engine::editor::sceneBounds(world), 1.5F);
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    CHECK(world.entityCount() == entityCountBefore);
    CHECK(world.componentCount<engine::Transform>() == transformCountBefore);
    CHECK(world.componentCount<engine::Camera>() == cameraCountBefore);
    CHECK(world.componentCount<engine::MeshRenderer>() == meshCountBefore);
    CHECK(world.componentCount<engine::DirectionalLight>() == dirLightCountBefore);
    CHECK(world.componentCount<engine::PointLight>() == pointLightCountBefore);
    CHECK(app->selection().count() == selectionCountBefore);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a hidden/tabbed-away Viewport does not reset or re-latch the camera pose (task 2.3.1, E2)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "camera hidden viewport", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    app->panels().setVisible("Viewport", false);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    engine::editor::EditorCamera* camera = app->viewportCamera();
    REQUIRE(camera != nullptr);
    camera->setPivot(engine::Vec3{3.0F, 4.0F, 5.0F});
    camera->setDistance(42.0F);

    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    app->panels().setVisible("Viewport", true);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    CHECK(camera->pivot() == engine::Vec3{3.0F, 4.0F, 5.0F});
    CHECK(camera->distance() == 42.0F);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ==================================================================================================
// task 2.3.2 -- the selection overlay driven through a real EditorApp::tick(). Honest limit, stated
// once here for all five cases: this proves NO CRASH, NO ABORT, NO LEAK, that frames present, and that
// a pick mutates nothing but the Selection. It does NOT prove that `onDraw` hands the pure pick
// functions the right ImGui values -- this harness cannot synthesise a click at all: SDL owns the
// mouse position and ImGui_ImplSDL3_NewFrame overwrites any injected value every frame, and there is
// no window under a real cursor in CI. What IS mechanically covered is the whole chain
// `mouse points -> NDC -> ray -> entity -> PickAction -> Selection`, because every link is a pure
// function tested in picking_test.cpp. That gap is three lines wide (updatePick's ARM/FIRE gates), it
// is named here, and editor/VALIDATION.md's rows 1-8 are what close it -- the same accounting 2.3.1
// gave its own AC-14/AC-15.
// ==================================================================================================

TEST_CASE("editor: the selection overlay path executes and stays balanced through real frames (task 2.3.2, AC-19)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "selection overlay smoke", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    engine::Entity cube{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());

    // An unbalanced PushClipRect/PopClipRect pair is an IM_ASSERT ABORT in the Debug ImGui build, so a
    // green Debug run through three real frames IS the balance proof.
    app->selection().set(cube);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a dead handle left in the selection does not crash the overlay (task 2.3.2, E2)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "selection overlay dead handle", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    engine::Entity cube{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());

    // Reachable in the shipped editor whenever the Hierarchy is hidden: nothing prunes the selection.
    app->selection().set(cube);
    REQUIRE(world.destroy(cube));  // destroyed WITHOUT pruning the selection first
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a non-mesh selection drives the point-marker path (task 2.3.2, D8)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "selection overlay point marker", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    engine::Entity light{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Directional Light") {
            light = e;
        }
    });
    REQUIRE(light.valid());

    app->selection().set(light);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the selection cap survives 300 real entities under load (task 2.3.2, E13)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "selection overlay cap load", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    std::vector<engine::Entity> all;
    all.reserve(300);
    for (int i = 0; i < 300; ++i) {
        const engine::Entity e = world.create();
        world.add<engine::Transform>(
            e, engine::Transform{.position = engine::Vec3{static_cast<float>(i) * 0.01F, 0.0F, 0.0F}});
        world.add<engine::MeshRenderer>(e, engine::MeshRenderer{});
        all.push_back(e);
    }
    app->selection().setAll(all);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: driving the Selection through real frames mutates nothing but the Selection (task 2.3.2, AC-19)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "selection overlay purity", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    const engine::World& world = app->world();
    const std::size_t entityCountBefore = world.entityCount();
    const std::size_t transformCountBefore = world.componentCount<engine::Transform>();
    const std::size_t cameraCountBefore = world.componentCount<engine::Camera>();
    const std::size_t meshCountBefore = world.componentCount<engine::MeshRenderer>();
    const std::size_t dirLightCountBefore = world.componentCount<engine::DirectionalLight>();
    const std::size_t pointLightCountBefore = world.componentCount<engine::PointLight>();

    engine::editor::EditorCamera* camera = app->viewportCamera();
    REQUIRE(camera != nullptr);
    // The FULL pose, all eight accessors -- comparing the whole pose is what distinguishes "unchanged"
    // from "reset to the default" (2.3.1's recorded lesson).
    const engine::Vec3 pivotBefore = camera->pivot();
    const float distanceBefore = camera->distance();
    const float yawBefore = camera->yaw();
    const float pitchBefore = camera->pitch();
    const float fovBefore = camera->fovYRadians();
    const float nearBefore = camera->nearPlane();
    const float farBefore = camera->farPlane();
    const float flySpeedBefore = camera->flySpeed();

    engine::Entity cube{};
    engine::Entity light{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
        if (world.name(e) == "Directional Light") {
            light = e;
        }
    });
    REQUIRE(cube.valid());
    REQUIRE(light.valid());

    app->selection().set(cube);
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    app->selection().toggle(light);
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    app->selection().clear();
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    CHECK(world.entityCount() == entityCountBefore);
    CHECK(world.componentCount<engine::Transform>() == transformCountBefore);
    CHECK(world.componentCount<engine::Camera>() == cameraCountBefore);
    CHECK(world.componentCount<engine::MeshRenderer>() == meshCountBefore);
    CHECK(world.componentCount<engine::DirectionalLight>() == dirLightCountBefore);
    CHECK(world.componentCount<engine::PointLight>() == pointLightCountBefore);
    CHECK(camera->pivot() == pivotBefore);
    CHECK(camera->distance() == distanceBefore);
    CHECK(camera->yaw() == yawBefore);
    CHECK(camera->pitch() == pitchBefore);
    CHECK(camera->fovYRadians() == fovBefore);
    CHECK(camera->nearPlane() == nearBefore);
    CHECK(camera->farPlane() == farBefore);
    CHECK(camera->flySpeed() == flySpeedBefore);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ==================================================================================================
// task 2.3.3 -- the transform gizmo path driven through a real EditorApp::tick(). Honest limit,
// stated once here for all five cases: this harness CANNOT synthesise a mouse click or drag --
// ImGui_ImplSDL3_NewFrame overwrites any injected mouse position from SDL every frame, and there is
// no window under a real cursor in CI. Every case below is therefore an EXECUTION / BALANCE /
// no-crash proof, not a behaviour proof: an unbalanced PushClipRect/PushID/PushStyleColor/
// BeginDisabled is an IM_ASSERT ABORT in the Debug build, so a green Debug run through real frames
// IS the balance proof. Behaviour lives in the tier-0 batteries (gizmo_test.cpp) plus the human pass
// (editor/VALIDATION.md).
// ==================================================================================================

TEST_CASE("editor: the gizmo path executes and stays balanced through real frames (task 2.3.3, I1)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "gizmo smoke", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    engine::Entity cube{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());

    app->selection().set(cube);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the gizmo origin behind the near plane skips Manipulate cleanly (task 2.3.3, I2/AC-12)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "gizmo behind camera", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    engine::Entity cube{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());
    app->selection().set(cube);

    engine::editor::EditorCamera* camera = app->viewportCamera();
    REQUIRE(camera != nullptr);
    // Fly the eye far along its OWN forward direction, past the origin (the seeded Cube's position,
    // DEFAULT_PIVOT) -- this puts the Cube behind the eye regardless of the default yaw/pitch,
    // without depending on any particular camera orientation.
    const engine::Vec3 farAhead = camera->position() + camera->forward() * 1000.0F;
    camera->setPivot(farAhead);

    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: an empty selection and a Transform-less primary skip the gizmo cleanly (task 2.3.3, I3/AC-14)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "gizmo no target", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();

    SUBCASE("empty selection") {
        app->selection().clear();
        for (int i = 0; i < 3; ++i) {
            REQUIRE(app->tick());
            CHECK(app->presentedLastFrame());
        }
    }

    SUBCASE("a primary with no Transform") {
        // world.create() -- the RAW World API, deliberately NOT entity_ops::createEntity, which
        // DOES add a Transform unconditionally (2.3.2 G1) and would defeat this case.
        const engine::Entity bare = world.create();
        REQUIRE_FALSE(world.has<engine::Transform>(bare));
        app->selection().set(bare);
        for (int i = 0; i < 3; ++i) {
            REQUIRE(app->tick());
            CHECK(app->presentedLastFrame());
        }
    }

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a hidden then re-shown Viewport survives the gizmo path (task 2.3.3, I4/E3)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "gizmo hidden viewport", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    engine::Entity cube{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());
    app->selection().set(cube);

    // This is also the proof that BeginFrame()-without-Manipulate is safe: the Viewport panel does
    // not draw at all while hidden, so `ImGuizmo::BeginFrame()` runs alone for two frames.
    app->panels().setVisible("Viewport", false);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    app->panels().setVisible("Viewport", true);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: the gizmo's transparent overlay window is invisible to the persisted layout (task 2.3.3, I5/AC-15)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "gizmo ini persistence", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    // The SAME exe-relative path ImGuiLayer::create's deriveIniPath() computes (imgui_layer.cpp) --
    // EditorAppConfig has no test-supplied "scratch directory" option (a plan assumption this task
    // corrects). SDL_GetBasePath() is SDL-cached (do NOT free), matching imgui_layer.cpp:38.
    const char* const base = SDL_GetBasePath();
    if (base == nullptr) {
        AERO_SKIP_OR_FAIL("no base path resolvable");
    }
    const std::string iniPath = std::string(base) + "aero_editor.ini";
    // NON-THROWING remove (the vfs_test / project_files_test idiom): a leftover file from an
    // aborted previous run can still be handle-locked on Windows, and a throwing remove would turn
    // that into a test ERROR rather than the REQUIRE below.
    std::error_code removeEc;
    std::filesystem::remove(iniPath, removeEc);  // start clean regardless of a leftover file
    REQUIRE_FALSE(std::filesystem::exists(iniPath));

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = true, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());
    const std::size_t panelCountBefore = app->panels().count();

    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    CHECK(app->panels().count() == panelCountBefore);  // the "gizmo" window is not a panel

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();  // ~ImGuiLayer -> ImGui::DestroyContext() -> SaveIniSettingsToDisk (imgui.cpp)

    REQUIRE(std::filesystem::exists(iniPath));
    std::string contents;
    {
        // SCOPED so the handle is CLOSED before the remove below. POSIX happily unlinks a file that
        // is still open (the inode survives until the last descriptor closes), so a trailing remove
        // with the stream still in scope passes on macOS and Linux and FAILS on Windows, which
        // refuses with a sharing violation. That is exactly how this reddened the MSVC lane.
        const std::ifstream in(iniPath);
        REQUIRE(in.is_open());
        std::ostringstream contentsStream;
        contentsStream << in.rdbuf();
        contents = contentsStream.str();
    }
    CHECK(contents.find("[Window][gizmo]") == std::string::npos);

    std::filesystem::remove(iniPath, removeEc);  // leave no state behind for the next run
}

// ==================================================================================================
// task 2.4.1 -- the undo/redo shell path driven through a real EditorApp::tick(). Honest limit,
// stated once here for all six cases: this harness CANNOT press a key -- aero_editor_imgui_test is
// ImGui-free at source (imgui::imgui is PRIVATE on aero_editor_core), so it cannot name ImGuiKey, and
// injecting a chord would additionally have to inject the platform-swapped modifier key
// (imgui.cpp:1894-1903) while ImGui_ImplSDL3_NewFrame overwrites injected input every frame anyway.
// EditorApp::requestUndo()/requestRedo() (D11) is the mechanically drivable substitute: it exercises
// the WHOLE shell path -- ShellUiState -> drawMenuBar -> applyHistoryRequests -> CommandStack ->
// TransformCommand -> writeTransform -- and an unbalanced ImGui call is an IM_ASSERT ABORT in the
// Debug build, so a green Debug run through real frames IS the balance proof. The physical key chord
// is human row 9's business and nothing else's.
// ==================================================================================================

TEST_CASE("editor: the history path executes and stays balanced (task 2.4.1, I1)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "history smoke", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    engine::Entity cube{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());
    app->selection().set(cube);

    const std::optional<engine::Transform> before = engine::editor::readTransform(app->world(), cube);
    REQUIRE(before.has_value());
    engine::Transform after = *before;
    after.position = before->position + engine::Vec3{1.0F, 2.0F, 3.0F};
    engine::editor::CommandContext cmd{app->world(), app->selection(), app->roots()};
    CHECK(app->commands().push(cmd, std::make_unique<engine::editor::TransformCommand>(cube, *before, after)));

    for (int i = 0; i < 5; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    app->requestUndo();
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    app->requestRedo();
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: undo through the shell mutates the World (task 2.4.1, I2/AC-22)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "history undo mutates", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    engine::Entity cube{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());
    app->selection().set(cube);

    const std::optional<engine::Transform> before = engine::editor::readTransform(app->world(), cube);
    REQUIRE(before.has_value());
    engine::Transform after = *before;
    after.position = before->position + engine::Vec3{4.0F, 0.0F, 0.0F};
    engine::editor::CommandContext cmd{app->world(), app->selection(), app->roots()};
    REQUIRE(app->commands().push(cmd, std::make_unique<engine::editor::TransformCommand>(cube, *before, after)));

    app->requestUndo();
    REQUIRE(app->tick());
    REQUIRE(engine::editor::readTransform(app->world(), cube).has_value());
    CHECK(*engine::editor::readTransform(app->world(), cube) == *before);

    app->requestRedo();
    REQUIRE(app->tick());
    REQUIRE(engine::editor::readTransform(app->world(), cube).has_value());
    CHECK(*engine::editor::readTransform(app->world(), cube) == after);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a request is consumed exactly once (task 2.4.1, I3)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "history request once", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    engine::Entity cube{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());
    app->selection().set(cube);

    const std::optional<engine::Transform> p0 = engine::editor::readTransform(app->world(), cube);
    REQUIRE(p0.has_value());
    engine::Transform p1 = *p0;
    p1.position = p0->position + engine::Vec3{1.0F, 0.0F, 0.0F};
    engine::editor::CommandContext cmd{app->world(), app->selection(), app->roots()};
    app->commands().breakMergeChain();
    REQUIRE(app->commands().push(cmd, std::make_unique<engine::editor::TransformCommand>(cube, *p0, p1)));
    app->commands().breakMergeChain();
    engine::Transform p2 = p1;
    p2.position = p1.position + engine::Vec3{1.0F, 0.0F, 0.0F};
    REQUIRE(app->commands().push(cmd, std::make_unique<engine::editor::TransformCommand>(cube, p1, p2)));

    const std::size_t appliedBefore = app->commands().appliedCount();
    app->requestUndo();
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
    }
    CHECK(appliedBefore - app->commands().appliedCount() == 1);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: undo and redo requested in one tick (task 2.4.1, I4/E12)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "history undo redo same tick", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    engine::Entity cube{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());
    app->selection().set(cube);

    const std::optional<engine::Transform> before = engine::editor::readTransform(app->world(), cube);
    REQUIRE(before.has_value());
    engine::Transform after = *before;
    after.position = before->position + engine::Vec3{2.0F, 0.0F, 0.0F};
    engine::editor::CommandContext cmd{app->world(), app->selection(), app->roots()};
    REQUIRE(app->commands().push(cmd, std::make_unique<engine::editor::TransformCommand>(cube, *before, after)));

    const std::size_t appliedBefore = app->commands().appliedCount();
    app->requestUndo();
    app->requestRedo();
    REQUIRE(app->tick());
    CHECK(app->commands().appliedCount() == appliedBefore);
    REQUIRE(engine::editor::readTransform(app->world(), cube).has_value());
    CHECK(*engine::editor::readTransform(app->world(), cube) == after);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: an empty history is inert through real frames (task 2.4.1, I5/E1)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "history empty inert", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    CHECK(app->commands().count() == 0);
    // TWO ticks first, so `before` is captured only once the sink has settled. create() itself already
    // logged three INFO lines (console sink attached, assets root, shell ready) that sit unpumped
    // until the FIRST tick()'s pumpLog() runs (editor_app.cpp D14); under -DAERO_SHADER_TOOLS=OFF the
    // Viewport ALSO logs a WARN on its first DRAW (viewport_panel.cpp, ensureInitialized), one frame
    // later still, so the second tick is what settles that too. Capturing `before` only after both
    // is what makes the loop below a clean "zero NEW records" measurement in every tools configuration.
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    const std::size_t before = app->logRecordCount();
    for (int i = 0; i < 10; ++i) {
        app->requestUndo();
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    CHECK(app->commands().count() == 0);
    CHECK_FALSE(app->commands().canUndo());
    CHECK(app->logRecordCount() == before);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: undo of a destroyed target through real frames (task 2.4.1, I6/E3)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "history destroyed target", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    engine::Entity cube{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());
    app->selection().set(cube);

    const std::optional<engine::Transform> before = engine::editor::readTransform(app->world(), cube);
    REQUIRE(before.has_value());
    engine::Transform after = *before;
    after.position = before->position + engine::Vec3{0.0F, 5.0F, 0.0F};
    engine::editor::CommandContext cmd{app->world(), app->selection(), app->roots()};
    REQUIRE(app->commands().push(cmd, std::make_unique<engine::editor::TransformCommand>(cube, *before, after)));
    REQUIRE(app->tick());

    engine::editor::destroyEntities(app->world(), std::vector<engine::Entity>{cube});
    REQUIRE(app->tick());

    const std::size_t before2 = app->logRecordCount();
    app->requestUndo();
    REQUIRE(app->tick());  // undo() runs INSIDE this tick's drawShellUi and logs the WARN into the
                           // sink, but pumpLog() already ran at the TOP of this same tick (D14) -- the
                           // record is not visible through logRecordCount() until the NEXT pump.
    REQUIRE(app->tick());
    CHECK(app->logRecordCount() - before2 == 1);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ==================================================================================================
// task 2.4.2 -- the structural/property-set commands driven through a real EditorApp::tick(). Honest
// limit, stated once here for all five cases: this harness CANNOT press a key or type into a widget,
// so it drives the commands directly against a real CommandStack/CommandContext and pumps them through
// requestUndo()/requestRedo() and real tick()s -- the same substitute task 2.4.1's own I1-I6 use. An
// unbalanced ImGui call is an IM_ASSERT ABORT in the Debug build, so a green Debug run through real
// frames IS the balance proof. Every case that reads logRecordCount() budgets a SETTLING TICK first
// (2.2.5 D14/2.4.1 I5-I6): a record raised during a tick's draw is only visible on the FOLLOWING tick,
// since EditorApp::tick() pumps the console sink at the TOP of the frame, before drawShellUi.
// ==================================================================================================

TEST_CASE("editor: a structural command executes through a real frame (task 2.4.2, I7)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "structural command smoke", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    engine::Entity cube{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());
    const std::size_t entityCountBefore = world.entityCount();

    engine::editor::CommandContext cmd{app->world(), app->selection(), app->roots()};
    REQUIRE(app->commands().push(cmd, std::make_unique<engine::editor::DeleteEntitiesCommand>(
                                          std::vector<engine::Entity>{cube}, std::vector<engine::Entity>{})));

    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    CHECK(world.entityCount() == entityCountBefore - 1);
    CHECK_FALSE(world.alive(cube));

    app->requestUndo();
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    CHECK(world.entityCount() == entityCountBefore);
    CHECK(world.alive(cube));  // the ORIGINAL handle, back (D2) -- not a lookalike under a new identity

    app->requestRedo();
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    CHECK(world.entityCount() == entityCountBefore - 1);
    CHECK_FALSE(world.alive(cube));

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: delete -> undo through the shell restores the ORIGINAL handle and the root order "
    "(task 2.4.2, I8)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "structural undo root order", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    engine::World& world = app->world();
    engine::Entity cube{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());

    // Settle first: RootOrder is reconciled by the Hierarchy panel's own phase 1, inside tick().
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    const std::vector<engine::Entity> rootsBefore(app->roots().entities().begin(), app->roots().entities().end());
    REQUIRE(rootsBefore.size() == 3);  // Main Camera, Directional Light, Cube -- all three are roots

    engine::editor::CommandContext cmd{app->world(), app->selection(), app->roots()};
    REQUIRE(app->commands().push(cmd, std::make_unique<engine::editor::DeleteEntitiesCommand>(
                                          std::vector<engine::Entity>{cube}, std::vector<engine::Entity>{})));
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    app->requestUndo();
    for (int i = 0; i < 2; ++i) {  // one to apply undo(), one to let the Hierarchy reconcile again
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    CHECK(world.alive(cube));
    const std::vector<engine::Entity> rootsAfter(app->roots().entities().begin(), app->roots().entities().end());
    CHECK(rootsAfter == rootsBefore);  // element-wise: the SAME order, Cube back in ITS OWN slot

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: EditorApp exposes exactly ONE RootOrder, shared by a command and the Hierarchy panel "
    "(task 2.4.2, I9)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "one root order", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    REQUIRE(app->tick());  // reconciles once, through the Hierarchy panel's own phase 1
    CHECK(app->presentedLastFrame());
    const engine::editor::RootOrder* firstTick = &app->roots();
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    const engine::editor::RootOrder* secondTick = &app->roots();
    CHECK(firstTick == secondTick);  // address-stable across frames -- exactly ONE object

    // And it is the SAME object a CommandContext built the way tick() builds one would see -- there is
    // no second RootOrder a structural command could be handed by mistake.
    const engine::editor::CommandContext cmd{app->world(), app->selection(), app->roots()};
    CHECK(&cmd.roots == firstTick);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: an empty history stays silent under the widened CommandContext (task 2.4.2, I10)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "history empty widened", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    CHECK(app->commands().count() == 0);
    // TWO settling ticks (§A30), matching 2.4.1's I5: create() itself already logged records that sit
    // unpumped until the FIRST tick()'s pumpLog() runs, and under -DAERO_SHADER_TOOLS=OFF the Viewport
    // logs a one-time WARN on its first draw, one frame later still.
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    const std::size_t before = app->logRecordCount();
    for (int i = 0; i < 10; ++i) {
        app->requestUndo();
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    CHECK(app->commands().count() == 0);
    CHECK_FALSE(app->commands().canUndo());
    CHECK(app->logRecordCount() == before);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: EditorApp stays noexcept-movable and drives after a move (task 2.4.2, I11/AC-32)") {
    // The COMPILE-TIME half of AC-32 -- entity_ops.hpp's two static_asserts on RootOrder are the other.
    static_assert(std::is_nothrow_move_constructible_v<engine::editor::EditorApp>);
    static_assert(std::is_nothrow_move_assignable_v<engine::editor::EditorApp>);

    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "editor app move", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    std::optional<engine::editor::EditorApp> holder = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(holder.has_value());

    engine::World& world = holder->world();
    engine::Entity cube{};
    world.eachEntity([&](engine::Entity e) {
        if (world.name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());

    // The RUNTIME half: move into a second optional<EditorApp> -- the exact shape a future scene-swap
    // (2.5.1) exercises -- then prove push/undo/tick still work against the MOVED object.
    std::optional<engine::editor::EditorApp> moved = std::move(holder);
    REQUIRE(moved.has_value());

    const engine::World& movedWorld = moved->world();
    const std::optional<engine::Transform> before = engine::editor::readTransform(movedWorld, cube);
    REQUIRE(before.has_value());
    engine::Transform after = *before;
    after.position = before->position + engine::Vec3{1.0F, 1.0F, 1.0F};
    engine::editor::CommandContext cmd{moved->world(), moved->selection(), moved->roots()};
    REQUIRE(moved->commands().push(cmd, std::make_unique<engine::editor::TransformCommand>(cube, *before, after)));

    REQUIRE(moved->tick());
    CHECK(moved->presentedLastFrame());

    moved->requestUndo();
    REQUIRE(moved->tick());
    REQUIRE(engine::editor::readTransform(movedWorld, cube).has_value());
    CHECK(*engine::editor::readTransform(movedWorld, cube) == *before);

    moved->requestQuit();
    CHECK(moved->tick() == false);
    moved.reset();
}

// ==================================================================================================
// task 2.5.1 -- New/Open/Save driven through a real EditorApp::tick(), using the D15 path-taking
// hooks (requestOpenScene(path)/requestSaveSceneAs(path)) as the mechanical substitute for a native
// dialog this harness cannot click. TWO mandatory rules for every case below (plan §S Step 8):
//   1. state in a comment WHY the document is CLEAN at the moment a guarded hook is called -- a
//      mutation must go through the World DIRECTLY, never through CommandStack::push(), unless the
//      case's own point IS the guard;
//   2. budget a SETTLING TICK before reading logRecordCount() -- tick() pumps the console sink at the
//      TOP of the frame, so a record raised during frame N's draw is visible only on frame N+1.
// ==================================================================================================

TEST_CASE("editor: requestNewScene on a clean app produces the seed contents through a real frame (task 2.5.1, I12)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "new scene smoke", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    app->requestNewScene();  // clean at create() -- nothing pushed to the stack yet
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    CHECK(app->world().entityCount() == 3);
    std::vector<std::string> names;
    app->world().eachEntity([&](engine::Entity e) { names.emplace_back(app->world().name(e)); });
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"Cube", "Directional Light", "Main Camera"});
    CHECK(app->selection().empty());
    CHECK(app->commands().count() == 0);
    CHECK(app->commands().isClean());
    CHECK(app->scenePath().empty());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: requestSaveSceneAs writes through a real frame (task 2.5.1, I13/AC-15/AC-20)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "save as smoke", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());
    if (!engine::editor::sceneIoAvailable()) {  // D18/AC-6: Save/Open need AERO_REFLECT_TOOLS; New Scene
        MESSAGE("scene I/O unavailable -- built without AERO_REFLECT_TOOLS");  // does not (untested here)
        app->requestQuit();
        CHECK(app->tick() == false);
        return;
    }

    const std::string tmp = uniqueScenePath(".scene.json");
    app->requestSaveSceneAs(tmp);
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    CHECK(engine::editor::fileExists(tmp));
    CHECK(app->scenePath() == tmp);
    CHECK_FALSE(app->sceneDirty());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: requestSaveScene on a titled scene writes with no guard involved (task 2.5.1, I14/AC-16)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "save titled smoke", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());
    if (!engine::editor::sceneIoAvailable()) {  // D18/AC-6
        MESSAGE("scene I/O unavailable -- built without AERO_REFLECT_TOOLS");
        app->requestQuit();
        CHECK(app->tick() == false);
        return;
    }

    const std::string tmp = uniqueScenePath(".scene.json");
    app->requestSaveSceneAs(tmp);
    REQUIRE(app->tick());
    REQUIRE(app->scenePath() == tmp);
    REQUIRE_FALSE(app->sceneDirty());

    engine::Entity cube{};
    app->world().eachEntity([&](engine::Entity e) {
        if (app->world().name(e) == "Cube") {
            cube = e;
        }
    });
    REQUIRE(cube.valid());
    const std::optional<engine::Transform> before = engine::editor::readTransform(app->world(), cube);
    REQUIRE(before.has_value());
    engine::Transform after = *before;
    after.position = before->position + engine::Vec3{0.0F, 1.0F, 0.0F};
    engine::editor::CommandContext cmd{app->world(), app->selection(), app->roots()};
    // Pushed through the STACK deliberately: this case's own point is that a mutation which ALREADY
    // exists in the history still goes through a plain, unguarded Save (SaveScene never discards
    // work, so guardFor returns Perform and the modal is never involved -- SS1 pins that).
    REQUIRE(app->commands().push(cmd, std::make_unique<engine::editor::TransformCommand>(cube, *before, after)));
    REQUIRE(app->tick());
    CHECK(app->sceneDirty());

    app->requestSaveScene();
    REQUIRE(app->tick());
    CHECK_FALSE(app->sceneDirty());
    CHECK(app->scenePath() == tmp);

    // Push again -- dirty once more, proving the clean flag is live history arithmetic, not a latch.
    engine::Transform again = after;
    again.position = after.position + engine::Vec3{0.0F, 1.0F, 0.0F};
    REQUIRE(app->commands().push(cmd, std::make_unique<engine::editor::TransformCommand>(cube, after, again)));
    REQUIRE(app->tick());
    CHECK(app->sceneDirty());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: requestOpenScene discards a direct World mutation in one tick (task 2.5.1, I15/AC-10)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "open discards mutation", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());
    if (!engine::editor::sceneIoAvailable()) {  // D18/AC-6
        MESSAGE("scene I/O unavailable -- built without AERO_REFLECT_TOOLS");
        app->requestQuit();
        CHECK(app->tick() == false);
        return;
    }

    const std::string tmp = uniqueScenePath(".scene.json");
    app->requestSaveSceneAs(tmp);
    REQUIRE(app->tick());
    REQUIRE(app->scenePath() == tmp);
    const std::size_t savedCount = app->world().entityCount();

    // Mutate the World DIRECTLY (§A11): requestOpenScene(path) is GUARDED, and a mutation pushed
    // through the CommandStack would make isClean() false, raising the modal instead of opening --
    // no test tier can answer a modal. Bypassing the stack keeps the document clean, so the guard's
    // Perform branch is what runs here; the guard's BLOCKING half is covered tier-0 by SS23-SS26.
    const engine::Entity extra = engine::editor::createEntity(app->world(), {}, "DirectMutation");
    REQUIRE(extra.valid());
    REQUIRE(app->world().entityCount() == savedCount + 1);
    REQUIRE(app->commands().isClean());  // still clean -- the guard will proceed, not raise the modal

    app->requestOpenScene(tmp);
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    CHECK(app->world().entityCount() == savedCount);  // the mutation is gone
    CHECK(app->commands().isClean());
    CHECK(app->commands().count() == 0);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: requestOpenScene on a missing file changes nothing and logs (task 2.5.1, I16/AC-11/AC-12)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "open missing file", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(app.has_value());

    // TWO settling ticks first (2.4.1 I5's precedent): create() itself already logged records that
    // sit unpumped until the first tick()'s pumpLog() runs, and under -DAERO_SHADER_TOOLS=OFF the
    // Viewport logs a one-time WARN on its first draw, one frame later still.
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    const std::size_t countBefore = app->world().entityCount();
    const std::string_view pathBefore = app->scenePath();
    const std::size_t before = app->logRecordCount();

    const std::string missing = uniqueScenePath("-definitely-missing.scene.json");
    app->requestOpenScene(missing);
    REQUIRE(app->tick());  // openSceneFile fails and logs an ERROR into the sink this frame
    REQUIRE(app->tick());  // the settling tick: pumpLog() at the TOP of THIS frame surfaces it

    CHECK(app->world().entityCount() == countBefore);
    CHECK(app->scenePath() == pathBefore);
    CHECK(app->logRecordCount() - before >= 1);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a MOVED EditorApp still drives Save As and Open through real frames (task 2.5.1, I17/AC-34)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "editor app move scene io", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::editor::EditorApp> holder = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F});
    REQUIRE(holder.has_value());

    std::optional<engine::editor::EditorApp> moved = std::move(holder);
    REQUIRE(moved.has_value());
    if (!engine::editor::sceneIoAvailable()) {  // D18/AC-6: the move-safety half of AC-34 is proven by
        MESSAGE("scene I/O unavailable -- built without AERO_REFLECT_TOOLS");  // the static_asserts above
        moved->requestQuit();                                                  // regardless of tools state
        CHECK(moved->tick() == false);
        return;
    }

    const std::string tmp = uniqueScenePath(".scene.json");
    moved->requestSaveSceneAs(tmp);
    REQUIRE(moved->tick());
    CHECK(moved->presentedLastFrame());
    CHECK(engine::editor::fileExists(tmp));
    CHECK(moved->scenePath() == tmp);
    CHECK_FALSE(moved->sceneDirty());

    // Direct mutation again (§A11) -- requestOpenScene is guarded and the document must stay clean.
    const engine::Entity extra = engine::editor::createEntity(moved->world(), {}, "MovedMutation");
    REQUIRE(extra.valid());
    REQUIRE(moved->commands().isClean());

    moved->requestOpenScene(tmp);
    REQUIRE(moved->tick());
    CHECK_FALSE(moved->world().alive(extra));
    CHECK(moved->commands().isClean());

    moved->requestQuit();
    CHECK(moved->tick() == false);
    moved.reset();
}

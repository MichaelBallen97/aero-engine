// aero_editor_imgui_test is a standalone single-TU target (no shared tests/test_main.cpp) -- it
// provides doctest's own main() here, unlike aero_tests' TEST_CASE files (the aero_reflect_meta_test
// precedent). GPU-gated at RUNTIME via AERO_REQUIRE_GPU (rhi_test_support.hpp): unset locally skips
// loudly; set (CI) a missing GPU/display is a hard failure. ImGui-free at source -- this TU drives
// aero::editor_core's engine-typed API only (task 2.1.3: EditorApp::tick(), not a hand-rolled loop);
// imgui/SDL3 reach it purely transitively through editor_core's PRIVATE static archive (the
// glm-in-aero_tests precedent).
//
// G6 (window visibility): uses a small VISIBLE 320x180 window, matching the rhi_swapchain_test
// precedent that is proven to present on all three CI lanes (macOS Metal, Windows WARP, Linux
// lavapipe under xvfb). A hidden window presented fine on macOS Metal locally, but hidden-window
// presentation is unproven on the lavapipe/WARP lanes; since every tick() below asserts the frame
// presented, we take the proven visible path. The brief flash matches rhi_swapchain_test.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <aero/core/log.hpp>  // AERO_LOG_* + initLogging (cases B and C)
#include <aero/editor/component_ops.hpp>
#include <aero/editor/console_model.hpp>  // DEFAULT_LOG_HISTORY_CAPACITY (case C)
#include <aero/editor/editor_app.hpp>
#include <aero/editor/editor_camera.hpp>  // task 2.3.1
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/panel_registry.hpp>
#include <aero/editor/scene_bounds.hpp>  // task 2.3.1
#include <aero/editor/selection.hpp>
#include <aero/platform/platform.hpp>
#include <aero/rhi/device.hpp>
#include <aero/scene/scene.hpp>
#include <aero/scene/world.hpp>

#include "rhi_test_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
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
    CHECK(camera->yaw() == DEFAULT_YAW);
    CHECK(camera->pitch() == DEFAULT_PITCH);

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

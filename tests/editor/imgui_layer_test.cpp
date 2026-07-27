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
#include <aero/editor/component_ops.hpp>
#include <aero/editor/editor_app.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/panel_registry.hpp>
#include <aero/editor/selection.hpp>
#include <aero/platform/platform.hpp>
#include <aero/rhi/device.hpp>
#include <aero/scene/world.hpp>

#include "rhi_test_support.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <vector>

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

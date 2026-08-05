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
#include <aero/editor/asset_cache.hpp>    // task 3.1.2: ImportChange, ASSET_CACHE_DIR_NAME/FILE_NAME/
                                          // GITIGNORE_NAME -- I31's index-path/gitignore-path assertions
#include <aero/editor/command_stack.hpp>  // task 2.4.1
#include <aero/editor/component_ops.hpp>
#include <aero/editor/console_model.hpp>  // DEFAULT_LOG_HISTORY_CAPACITY (case C)
#include <aero/editor/editor_app.hpp>
#include <aero/editor/editor_camera.hpp>    // task 2.3.1
#include <aero/editor/entity_commands.hpp>  // task 2.4.2
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/panel_registry.hpp>
#include <aero/editor/picking.hpp>       // task 2.3.2
#include <aero/editor/project.hpp>       // task 2.6.1
#include <aero/editor/scene_bounds.hpp>  // task 2.3.1
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/selection_overlay.hpp>  // task 2.3.2
#include <aero/editor/text_file.hpp>          // task 2.6.1: writeTextFileAtomic, for I23/I24's recents file
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
#include <array>  // the frozen panel-id roster; reached transitively on libc++, not on MSVC (813bc4d)
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>  // task 2.4.1: std::make_unique<TransformCommand>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>   // I30: AERO_EDITOR_SRC_DIR's literal-concatenation target
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

// task 2.6.1 (I18/I21-I24): a fresh DIRECTORY in the OS temp directory, CREATED (createProject
// requires its `location` argument to already exist) and GUARANTEED EMPTY -- unlike uniqueScenePath
// above, createProject REFUSES a non-empty target (TargetNotEmpty), so a per-PROCESS counter alone is
// not enough: two ctest invocations both start counting from 1, and the FIRST run's
// "aero_imgui_layer_project_1/MyGame" (a real, non-empty scaffolded project) collides with the
// SECOND run's attempt to create the identically-named project inside it. remove_all FIRST, exactly
// the tests/editor/project_files_test.cpp TempDir precedent, discharges that regardless of what an
// earlier run left behind.
[[nodiscard]] std::string uniqueProjectLocation() {
    static int counter = 0;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("aero_imgui_layer_project_" + std::to_string(++counter));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::u8string bytes = dir.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// task 2.6.1 (I23/I24): a fresh, never-yet-existing recents-file PATH -- the file itself is written
// by writeTextFileAtomic at each case's own call site, never here.
[[nodiscard]] std::string uniqueRecentsFile() {
    static int counter = 0;
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / ("aero_imgui_layer_recents_" + std::to_string(++counter) + ".json");
    const std::u8string bytes = file.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
}  // namespace

// task 2.6.1 (D15): EVERY EditorAppConfig literal below opts OUT of restoring the last project. The
// field defaults to TRUE -- the shipping behaviour -- so without the opt-out every case here would
// open whatever project the developer last used, swapping the World and seeding three entities. It
// would PASS on a CI runner (which has no recents file) and FAIL on a developer machine, which is
// the worst possible shape for a bug. The ONE case that must exercise the restore path (I23)
// enables it AND points `recentProjectsPath` at a TempDir file, so no test ever READS the real
// pref directory. §V7's grep asserts this count matches the total number of app-creation call sites.
//
// BLOCKING-2 (code review): `restoreLastProject == false` ONLY suppresses the READ. It does NOT
// suppress the WRITE -- `recentsPath` is resolved from `config.recentProjectsPath` unconditionally at
// create(), and ANY case that opens or creates a project during its run (I18, I21: `.projectPath` /
// `requestOpenProject()`) dirties the recents list and gets it FLUSHED on the very next tick(),
// falling through to the real `defaultRecentProjectsPath()` -- the real, machine-wide
// `~/Library/Application Support/AeroEngine/AeroEditor/recent_projects.json` on macOS -- the moment
// `recentProjectsPath` is left unset. This is not hypothetical: it happened on the machine this task
// was implemented on, confirmed by inspecting that file's contents after a run. EVERY case that can
// reach `adoptProject` (a real `.projectPath`, or `requestOpenProject`/`requestNewProject`/
// `requestClearRecentProjects`) MUST set `.recentProjectsPath` to `uniqueRecentsFile()`, exactly like
// I23/I24 already do for the READ side -- READ and WRITE are two independent footguns closed by the
// SAME field, and a case can dirty recents without ever setting `restoreLastProject = true`.

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
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
    REQUIRE(app.has_value());

    // The D8 registration -- FOUR absolute panel-count sites in the tree (this one plus :219, :447 and
    // :551), all of which moved from 5 to 6 the moment task 2.6.2 registered the sixth default panel,
    // "Project Settings". (`:1360`/`:1366`'s captured `panelCountBefore` is a fifth, count-agnostic
    // site by construction -- the "gizmo" window is not a panel -- and is NOT one of the four.)
    CHECK(app->panels().count() == 6);

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
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
    REQUIRE(app.has_value());

    // AC-18: the default config seeds three entities.
    CHECK(app->world().entityCount() == 3);
    CHECK(app->panels().count() == 6);
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = false, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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

TEST_CASE(
    "editor: the Asset browser panel handles the no-project state without unbalancing ImGui (task "
    "2.2.4/2.6.1, I19/AC-28/AC-30)") {
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
    // projectPath = "" now means NO PROJECT (D0) -- unlike 2.2.4's old premise (the CWD-fallback
    // helper this task deletes, D11), the Asset Browser's root is "" and the panel renders its no-root
    // state. This is exactly what AC-28/AC-30 require: no project is not a cage, and the panel must
    // still draw, resize and tear down cleanly through it.
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(*device, *window, ctx,
                                                                                     {.persistLayout = false,
                                                                                      .seedDefaultScene = true,
                                                                                      .unfocusedFrameCapHz = 0.0F,
                                                                                      .projectPath = "",
                                                                                      .restoreLastProject = false});
    REQUIRE(app.has_value());
    CHECK(app->panels().count() == 6);
    CHECK_FALSE(app->projectIsOpen());
    CHECK(app->assetBrowserRoot().empty());

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

TEST_CASE("editor: the Asset browser draws its error state for an unusable root (task 2.2.4/2.6.1, I20/AC-32)") {
    // Proves the D17 degradation path is DRAWN, not merely returned: a missing root must still open,
    // dock and quit cleanly. A relative literal is used deliberately -- it keeps <filesystem> out of
    // this GPU TU, and no such directory exists under the ctest working directory (the build tree).
    // task 2.6.1: this path is now a PROJECT path (AC-32) -- it has no project.json, so
    // openProjectPath logs one ERROR and the project stays CLOSED; the Asset Browser's root is then
    // "" (the no-project state, not the literal bad path), and its OBSERVABLE outcome -- an error/
    // empty-state render -- is unchanged from 2.2.4's own premise.
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
                                           .projectPath = "aero-nonexistent-root-2.2.4",
                                           .restoreLastProject = false});
    REQUIRE(app.has_value());
    CHECK_FALSE(app->projectIsOpen());
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
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
    REQUIRE(app.has_value());

    // The D8 registration -- the ONE absolute panel count in the tree (plan C3's proof).
    CHECK(app->panels().count() == 6);

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
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
    std::optional<engine::editor::EditorApp> bareApp =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .registerDefaultPanels = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .restoreLastProject = false});
    REQUIRE(bareApp.has_value());
    CHECK(bareApp->viewportCamera() == nullptr);
    // Task 2.6.2 AC-28's first half: with registerDefaultPanels == false NOTHING is registered, which
    // is what makes the Edit > Project Settings... item resolve to disabled rather than to a logged
    // no-op ERROR on every click (shell_ui.cpp gates it on panels.find(id) != nullptr). The count-0
    // half had no assertion anywhere in the tree before this line.
    CHECK(bareApp->panels().count() == 0);
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = true, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
    REQUIRE(app.has_value());

    // Finding 3 of the 2.5.1 code-review round: with `seedDefaultScene = true`, every assertion below
    // already held BEFORE `requestNewScene()` ran at all, so a no-op `requestNewScene()` (the same
    // defect class self-corrected for IO5 in 1ad4c93) would leave this case green. A DIRECT World
    // mutation (never through CommandStack::push()) keeps the document CLEAN, so the guard still
    // performs immediately (Perform, not Confirm) -- and gives requestNewScene() something real to
    // discard.
    const engine::Entity probe = engine::editor::createEntity(app->world(), {}, "Probe");
    REQUIRE(probe.valid());
    REQUIRE(app->world().entityCount() == 4);

    app->requestNewScene();  // clean at create() -- nothing pushed to the stack yet
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    CHECK(app->world().entityCount() == 3);  // NOT 4 -- "Probe" is gone; discriminates the no-op
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
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

// ---- I18, I21-I24: task 2.6.1's project flow, driven through real frames ---------------------------

TEST_CASE(
    "editor: a project passed at create() opens and drives the Asset Browser root (task 2.6.1, I18/AC-27/AC-31)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "project i18", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);

    // BLOCKING-2 (code review): opening a project here (via `.projectPath`) dirties the recents list
    // (adoptProject -> promoteRecent -> recentsDirty = true), which the very next tick() FLUSHES --
    // `.restoreLastProject = false` only ever suppressed the READ half; with no `.recentProjectsPath`
    // override, the WRITE fell through to `defaultRecentProjectsPath()`, the REAL machine-wide
    // `recent_projects.json`. This is the exact defect the reviewer found already landed on this
    // machine. `uniqueRecentsFile()` (I23/I24's own helper, :106-112) points the write at a scratch
    // path instead.
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    CHECK(app->projectIsOpen());
    CHECK(app->projectName() == "MyGame");

    // D10's reconcile is a startup NO-OP (the panel is BORN with the right root, 2.6.1's whole point)
    // but this is proven only AFTER a tick -- the reconcile runs inside tick(), never at create().
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    CHECK(app->assetBrowserRoot() == created.root + "/assets");

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: requestOpenProject swaps the project and resets the scene in one tick (task 2.6.1, "
    "I21/AC-18/AC-31)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "project i21", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);

    // BLOCKING-2 (code review): requestOpenProject() below dirties the recents list on the tick that
    // performs the swap, and the very next tick() flushes it -- `recentProjectsPath` MUST be set here
    // too, at create() time, or the flush falls through to the REAL machine-wide recents file exactly
    // as I18's did.
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .seedDefaultScene = true,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    CHECK_FALSE(app->projectIsOpen());
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    // Start from a MUTATED World -- a World already at 3 entities would pass whether or not newScene
    // actually ran underneath requestOpenProject (2.5.2's EG2 lesson), so an extra entity is created
    // first to make 3-afterwards a real assertion.
    //
    // PLAN DEVIATION, recorded here and in the engineering log: the plan's own §S Step 8 text says to
    // push a REAL command here and REQUIRE(app->sceneDirty()) before the swap -- tried first, and
    // measured to behave exactly as AC-19 specifies: OpenProject's discardsWork() is true, so a dirty
    // scene raises the unsaved-changes modal (confirmOpen) instead of swapping, and this GPU-gated TU
    // has no mechanism to answer that modal (FileFlow::choice has no public EditorApp accessor,
    // matching F6's "no ImGui input can be synthesised here"). That is I22's own scenario, not I21's:
    // "swaps... in one tick" and "a dirty scene guards... behind the modal" cannot both be true of the
    // same precondition. The World is mutated directly (bypassing the CommandStack, so
    // commands().isClean() stays true and the guard does not fire) rather than through a pushed
    // command, which is the only way to keep 2.5.2's EG2 discrimination (a real, non-vacuous reset)
    // without contradicting AC-19, already covered by I22 immediately below.
    const engine::Entity extra = app->world().create();
    (void)extra;

    app->requestOpenProject(created.root);
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    CHECK(app->projectIsOpen());
    CHECK(app->projectName() == "MyGame");
    CHECK(app->world().entityCount() == 3);  // the fresh default scene (AC-18)
    CHECK_FALSE(app->sceneDirty());
    CHECK(app->scenePath().empty());
    CHECK(app->commands().count() == 0);

    // MEASURED, not assumed: the reconcile runs at the TOP of tick() (mirroring the title push's own
    // placement), BEFORE drawShellUi()'s applyFileRequests() actually performs the swap -- so on the
    // very tick the swap happens, the reconcile has already run against the PRE-swap project state.
    // AC-31 promises the root is correct "after a project change from any entry point", not
    // necessarily within the exact tick the change is requested; one more plain tick is what makes it
    // observable, exactly as the window title already lags a swap by one tick for the same reason.
    REQUIRE(app->tick());
    CHECK(app->assetBrowserRoot() == created.root + "/assets");

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a dirty scene guards requestOpenProject behind the modal (task 2.6.1, I22/AC-19)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "project i22", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx,
        {.persistLayout = false, .seedDefaultScene = true, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
    REQUIRE(app.has_value());
    REQUIRE(app->tick());

    const engine::Entity extra = app->world().create();
    engine::editor::CommandContext cmd{app->world(), app->selection(), app->roots()};
    REQUIRE(app->commands().push(cmd, std::make_unique<engine::editor::DeleteEntitiesCommand>(
                                          std::vector<engine::Entity>{extra}, std::vector<engine::Entity>{})));
    REQUIRE(app->sceneDirty());

    app->requestOpenProject(created.root);
    REQUIRE(app->tick());  // the unsaved-changes modal is raised; the project has NOT changed
    CHECK(app->presentedLastFrame());
    CHECK_FALSE(app->projectIsOpen());

    // requestQuit() is the UNGUARDED direct hook (D14) -- it bypasses the guard entirely, exactly as
    // the other 35+ GPU-gated smoke tests in this file rely on for teardown regardless of a pending
    // modal.
    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: restoreLastProject opens the first recents entry at create() (task 2.6.1, I23/AC-33/D15)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "project i23", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);

    engine::editor::RecentProjects recents;
    recents.paths = {created.root};
    const std::string recentsFile = uniqueRecentsFile();
    REQUIRE(engine::editor::writeTextFileAtomic(recentsFile, engine::editor::writeRecentProjectsText(recents)).empty());

    // The ONE case in this file that ENABLES restoreLastProject -- and it ALSO points
    // recentProjectsPath at a TempDir-style file, so even with restore ON, the real pref path
    // (~/Library/Application Support/AeroEngine/AeroEditor/ on macOS) is never touched (D15).
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .restoreLastProject = true,
                                           .recentProjectsPath = recentsFile});
    REQUIRE(app.has_value());
    CHECK(app->projectIsOpen());
    CHECK(app->projectName() == "MyGame");
    CHECK(app->projectRoot() == created.root);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: restoreLastProject = false never reads the recents file (task 2.6.1, I24/AC-34/E23)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "project i24", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);

    engine::editor::RecentProjects recents;
    recents.paths = {created.root};
    const std::string recentsFile = uniqueRecentsFile();
    REQUIRE(engine::editor::writeTextFileAtomic(recentsFile, engine::editor::writeRecentProjectsText(recents)).empty());

    // The SAME file as I23, present on disk -- but restoreLastProject stays false (this file's own
    // default posture), so it must never be read at all, and no project opens.
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = recentsFile});
    REQUIRE(app.has_value());
    CHECK_FALSE(app->projectIsOpen());
    CHECK(app->projectRoot().empty());
    // S15/AC-34, directly: the file on disk has ONE entry, but restoreLastProject == false means it
    // was never read at all, so the in-memory recents list must still be EMPTY -- proving AC-34
    // directly instead of only through the downstream consequence (projectIsOpen()/projectRoot()
    // staying empty), and making sabotage seed S15 (dropping the `if (config.restoreLastProject)`
    // guard around `readRecentProjects`) redden this case specifically.
    CHECK(app->recentProjectCount() == 0);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- I25: task 2.6.2's Project Settings panel, driven through real frames ---------------------------

TEST_CASE(
    "editor: the Project Settings panel registers, orders and draws in both project states (task 2.6.2, "
    "I25/AC-26/AC-27)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "project settings i25", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);

    // ⛔ `.recentProjectsPath` is MANDATORY, not defensive: step 4 below opens a project through
    // requestOpenProject(), which dirties the recents list and gets it flushed on the very next
    // tick() -- `.restoreLastProject = false` only ever suppresses the READ half. Without this override
    // the flush falls through to the REAL machine-wide recents file, exactly 2.6.1's BLOCKING-2.
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());

    // 2. Registration and ORDER -- what makes sabotage seed S13 (registering before Inspector) redden.
    CHECK(app->panels().count() == 6);

    // EVERY id, in order (Phase 2 audit). Each panel's own header calls its id FROZEN because it is
    // the imgui.ini settings key -- renaming one orphans every user's saved layout for that panel --
    // but only "Project Settings" and "Inspector" were ever pinned, and a rename of the other four
    // passed the whole suite green. Worse than silent: the GPU cases reach these panels by
    // `setVisible("Console", …)`, and setVisible on an unknown id is a LOGGED NO-OP, so a renamed
    // panel would simply stop being hidden and the case that depends on hiding it would assert
    // against a panel that never draws -- 2.2.4's C5 trap, re-armed. This is a PERSISTED FORMAT, so
    // treat a diff here like a file-format change, not a test to update.
    const std::array<const char*, 6> frozenPanelIds{"Hierarchy", "Inspector", "Viewport",
                                                    "Console",   "Assets",    "Project Settings"};
    for (std::size_t i = 0; i < frozenPanelIds.size(); ++i) {
        CAPTURE(i);
        CHECK_EQ(std::string(app->panels().panelAt(i).id()), std::string(frozenPanelIds[i]));
    }

    // AC-15's other two clauses, asserted MECHANICALLY rather than left to human row 1. panelAt()
    // hands back a Panel& and both accessors are public on the base, so nothing here needs the
    // src-private panel header -- the same reach shell_test.cpp already uses on its MinimalPanel.
    // This is what makes sabotage seed S14 (defaultDockSlot() returning Center) discriminate; before
    // it, S14 reddened nothing anywhere in the tree.
    CHECK(app->panels().panelAt(5).defaultDockSlot() == engine::editor::DockSlot::Right);
    // options() is deliberately NOT overridden (AC-15): the panel's own window must keep scrolling,
    // because the two tables carry no ScrollY of their own.
    const engine::editor::PanelOptions settingsOpts = app->panels().panelAt(5).options();
    CHECK_FALSE(settingsOpts.noScrollbar);
    CHECK_FALSE(settingsOpts.hasMenuBar);
    CHECK_FALSE(settingsOpts.noPadding);
    CHECK_FALSE(settingsOpts.noScrollWithMouse);

    // R-1 (LOAD-BEARING): "Project Settings" shares DockSlot::Right with "Inspector", and Inspector
    // registers FIRST, so it is the selected tab in a fresh layout -- exactly 2.2.4's C5 finding, one
    // panel later. Without hiding Inspector, onDraw() never runs for this panel and every assertion
    // below (including the balance proof through the empty-state and eight-row branches) is vacuous.
    app->panels().setVisible("Inspector", false);

    // 3. Three ticks with NO project open -- the empty-state branch. An unbalanced BeginTable/EndTable
    // or a stray Begin/End is an IM_ASSERT ABORT in the Debug build, so a green run IS the assertion
    // (F9/F10) -- which is what makes seeds S16/S17 real.
    CHECK_FALSE(app->projectIsOpen());
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    // 4. Open the project through the D15 test seam and draw the eight-row branch. The scene is CLEAN
    // here, so discardsWork(OpenProject)'s guard does not fire and the swap completes in ONE tick
    // (G2-13). Because PanelContext::project is a REFERENCE and applyFileRequests runs before
    // drawPanels, the panel already renders the eight rows on the very tick of the swap (§A10) -- no
    // extra tick is needed for correctness, but the three extra ticks below exercise the table across
    // frames.
    app->requestOpenProject(created.root);
    REQUIRE(app->tick());
    CHECK(app->projectIsOpen());
    CHECK(app->projectName() == "MyGame");
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    // 5. Hidden/shown transitions -- where a stray End() or an unconditional EndTable() aborts.
    app->panels().setVisible("Project Settings", false);
    REQUIRE(app->tick());
    app->panels().setVisible("Project Settings", true);
    REQUIRE(app->tick());

    // 6. A layout reset: buildDefaultLayout with SIX panels and a two-panel Right node. This re-docks
    // "Project Settings" beside "Inspector"; the visibility set in step 3 is not restored by a layout
    // reset, since ImGui does not persist visibility.
    app->requestLayoutReset();
    REQUIRE(app->tick());

    // 7. Teardown clean under ASan/UBSan, and LSan on the Linux Debug lane.
    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();

    // What this case deliberately does NOT assert, because no ImGui-free test TU can observe it: that
    // the tab is SELECTED, that the text is legible, that the columns align, or that '%' renders
    // correctly. Those are validation rows 4, 5-7 and 11.
}

// I26 -- the layout-migration regression. Before this, buildDefaultLayout was the ONLY reader of
// defaultDockSlot() and it runs only when there is NO imgui.ini to restore, so any panel added after
// a user's layout was written had no settings entry and ImGui free-floated it. Shipping the Project
// Settings panel did exactly that to every existing install; without the fix, every panel a later
// task adds lands the same way.
//
// The .ini below carries BOTH populations at once, which is the point. "Project Settings" is present
// but FLOATING (an entry with no DockId -- the born-floating case, captured verbatim from the machine
// this was reported on), while the five older panels are docked, with Hierarchy and Inspector
// deliberately swapped out of their default nodes. So one run proves all three promises: a floating
// panel gets docked, a docked panel is never moved, and the new panel follows its slot-mate to
// wherever the user actually dragged it.
//
// This is a BLACK-BOX assertion on the saved file, not on ImGui state: the test TU is deliberately
// ImGui-free (no <imgui.h> anywhere in it), so the proof has to survive a round trip to disk. It also
// pins the behaviour that matters -- the new panel joins INSPECTOR'S node rather than merely being
// docked somewhere -- by comparing the two DockIds to each other rather than to a hardcoded id.
TEST_CASE("editor: a registered panel that is not DOCKED in a restored layout gets docked (I26)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "layout migration i26", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    static int iniCounter = 0;
    const std::filesystem::path iniFile =
        std::filesystem::temp_directory_path() / ("aero_layout_migration_" + std::to_string(++iniCounter) + ".ini");
    {
        std::ofstream out(iniFile, std::ios::binary | std::ios::trunc);
        REQUIRE(out.good());
        // ⛔ Hierarchy and Inspector are DELIBERATELY SWAPPED against what buildDefaultLayout produces
        // (it makes Left 0x1 and Right 0x4, in that order): this is a user who dragged Inspector to
        // the left and Hierarchy to the right. That swap is what gives this case its teeth. With the
        // node ids in their default arrangement, an implementation that simply ran buildDefaultLayout
        // on a RESTORED ini -- the exact damage this fix exists to prevent, and which additionally
        // zeroes every user DockId on the way through DockBuilderRemoveNodeDockedWindows -- would
        // regenerate bit-identical ids and satisfy every assertion below. Swapped, it cannot: it puts
        // Hierarchy back in 0x1 and reddens.
        // ⛔ "Project Settings" IS PRESENT and FLOATING -- Pos/Size/Collapsed with no DockId line,
        // which is byte-for-byte what ImGui writes for a panel that was born floating and then saved
        // on quit. This is the case the FIRST version of this fix got wrong: it asked "has the ini
        // heard of this panel", the answer here is yes, and it skipped the panel and preserved an
        // 81px sliver forever. Captured from the machine the bug was found on, Size and all.
        out << "[Window][Project Settings]\nPos=80,53\nSize=81,593\nCollapsed=0\n\n"
               "[Window][Hierarchy]\nPos=1025,25\nSize=255,695\nDockId=0x00000004,0\n\n"
               "[Window][Inspector]\nPos=0,25\nSize=255,695\nDockId=0x00000001,0\n\n"
               "[Window][Viewport]\nPos=259,25\nSize=762,518\nDockId=0x00000005,0\n\n"
               "[Window][Console]\nPos=259,547\nSize=762,173\nDockId=0x00000006,0\n\n"
               "[Window][Assets]\nPos=259,547\nSize=762,173\nDockId=0x00000006,1\n\n"
               "[Docking][Data]\n"
               "DockSpace       ID=0x08BD597D Pos=0,25 Size=1280,695 Split=X\n"
               "  DockNode      ID=0x00000001 Parent=0x08BD597D SizeRef=255,695\n"
               "  DockNode      ID=0x00000002 Parent=0x08BD597D SizeRef=1021,695 Split=X\n"
               "    DockNode    ID=0x00000003 Parent=0x00000002 SizeRef=762,695 Split=Y\n"
               "      DockNode  ID=0x00000005 Parent=0x00000003 SizeRef=762,518 CentralNode=1\n"
               "      DockNode  ID=0x00000006 Parent=0x00000003 SizeRef=762,173\n"
               "    DockNode    ID=0x00000004 Parent=0x00000002 SizeRef=255,695\n";
    }  // scoped: Windows cannot remove/rewrite a file whose stream is still open
    REQUIRE(std::filesystem::exists(iniFile));

    const std::u8string iniBytes = iniFile.u8string();
    const std::string iniPath(reinterpret_cast<const char*>(iniBytes.data()), iniBytes.size());

    {
        // persistLayout TRUE is the whole point -- it is what makes wantsDefaultLayout() false and
        // sends create() down the RESTORE path. layoutIniPath is therefore MANDATORY here: without it
        // this case would read and then overwrite the developer's real editor layout.
        std::optional<engine::editor::EditorApp> app =
            engine::editor::EditorApp::create(*device, *window, ctx,
                                              {.persistLayout = true,
                                               .unfocusedFrameCapHz = 0.0F,
                                               .restoreLastProject = false,
                                               .recentProjectsPath = uniqueRecentsFile(),
                                               .layoutIniPath = iniPath});
        REQUIRE(app.has_value());
        CHECK(app->panels().count() == 6);

        for (int i = 0; i < 3; ++i) {
            REQUIRE(app->tick());
        }
        app->requestQuit();
        CHECK(app->tick() == false);
    }  // ~EditorApp -> ~ImGuiLayer -> DestroyContext -> SaveIniSettingsToDisk

    std::ifstream in(iniFile, std::ios::binary);
    REQUIRE(in.good());
    const std::string saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // The panel the restored ini had never heard of now has an entry of its own...
    const std::size_t settingsAt = saved.find("[Window][Project Settings]");
    REQUIRE(settingsAt != std::string::npos);
    const std::size_t inspectorAt = saved.find("[Window][Inspector]");
    REQUIRE(inspectorAt != std::string::npos);

    // ...and its DockId is INSPECTOR'S, not 0 (floating) and not some other node. Both are read out of
    // the saved file and compared to each other, so the assertion survives ImGui renumbering nodes.
    // BOUNDED to the section, deliberately. An unbounded find("DockId=", sectionAt) would happily
    // return the NEXT section's DockId when this section has none, which is precisely the
    // not-docked case -- i.e. the bug would report the value that makes the test pass. Verified by
    // seeding: with the fix disabled this helper must fail to find one, not find someone else's.
    auto dockIdInSection = [&saved](std::size_t sectionAt) {
        const std::size_t sectionEnd = saved.find("\n[", sectionAt + 1);
        const std::size_t at = saved.find("DockId=", sectionAt);
        REQUIRE(at != std::string::npos);
        REQUIRE((sectionEnd == std::string::npos || at < sectionEnd));
        // Clamped to the LINE as well as the section: ImGui omits the ",order" suffix entirely when
        // DockOrder == -1, and DockBuilderDockWindow leaves exactly that on a freshly created entry.
        // Without the clamp the comma search would run into a later section's "Pos=x,y" and return a
        // multi-line string -- a spurious failure rather than a false pass, but a confusing one.
        const std::size_t lineEnd = saved.find('\n', at);
        REQUIRE(lineEnd != std::string::npos);
        const std::size_t end = std::min(saved.find(',', at), lineEnd);
        return saved.substr(at + 7, end - (at + 7));
    };
    const std::string settingsDock = dockIdInSection(settingsAt);
    const std::string inspectorDock = dockIdInSection(inspectorAt);
    CHECK_FALSE(settingsDock.empty());
    CHECK(settingsDock != "0x00000000");
    CHECK_EQ(settingsDock, inspectorDock);  // it joined its slot-mate's node (D12's stated intent)

    // The two assertions that give the swapped fixture its purpose, and the ONLY coverage anywhere of
    // this fix's headline promise -- "a panel the ini already knows is never touched, wherever the
    // user put it". Both are exact node ids, not a relative comparison, because the whole point is
    // that the SPECIFIC user arrangement survived rather than being regenerated into something
    // self-consistent. A rebuild-on-restore implementation returns Hierarchy to 0x00000001 and
    // Project Settings to 0x00000004, reddening both.
    const std::size_t hierarchyAt = saved.find("[Window][Hierarchy]");
    REQUIRE(hierarchyAt != std::string::npos);
    CHECK_EQ(dockIdInSection(hierarchyAt), "0x00000004");  // a KNOWN panel stayed where the user put it
    CHECK_EQ(settingsDock, "0x00000001");                  // and the NEW panel followed its slot-mate LEFT

    std::error_code ec;
    std::filesystem::remove(iniFile, ec);
}

// ---- I27-I29: task 3.1.1's asset scan, driven through real frames -------------------------------
// EVERY case below opts OUT of restoring the last project AND redirects the recents-file WRITE, the
// SAME two-part discipline I18/I21-I26 use -- opening a project during create() (I27/I28/I29) or
// swapping it at runtime (I28) both dirty the recents list, flushed on the very next tick(), and
// `restoreLastProject = false` alone only ever suppresses the READ (BLOCKING-2, 2.6.1 code review).

TEST_CASE("editor: the asset scan runs on open and the key space lines up (task 3.1.1, I27/AC-34/AC-35/AC-39/A16)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "asset scan i27", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);

    const std::string assetsRoot = created.root + "/assets";
    const std::array<std::string, 3> leaves{"a.txt", "b.txt", "c.txt"};
    for (const std::string& leaf : leaves) {
        std::string assetPath = assetsRoot;
        assetPath += "/";
        assetPath += leaf;
        REQUIRE(engine::editor::writeTextFileAtomic(assetPath, "hello").empty());
    }

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    CHECK(app->assetCount() == 3);
    // A16: the panel's root and the database's root are the SAME string by construction -- this is
    // the only reason `AssetBrowserPanel::selectedEntry` (relative to the panel's root) is a valid
    // `AssetDatabase::findByPath` key. Proven directly, in the same case as the GUIDs below.
    CHECK(app->assetBrowserRoot() == assetsRoot);
    for (const std::string& leaf : leaves) {
        std::string metaPath = assetsRoot;
        metaPath += "/";
        metaPath += leaf;
        metaPath += ".meta";
        CHECK(engine::editor::fileExists(metaPath));
        const std::optional<engine::Guid> guid = app->assetGuidForPath(leaf);
        REQUIRE(guid.has_value());
        CHECK(guid->valid());
    }

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the asset scan follows a runtime project swap, one tick later (task 3.1.1, I28/AC-34, seed S22)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "asset scan i28", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string locationA = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome createdA = engine::editor::createProject(locationA, "GameA", "0.1.0");
    REQUIRE(createdA.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(createdA.root + "/assets/only_in_a.txt", "a").empty());

    const std::string locationB = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome createdB = engine::editor::createProject(locationB, "GameB", "0.1.0");
    REQUIRE(createdB.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(createdB.root + "/assets/only_in_b.txt", "b").empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = createdA.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    REQUIRE(app->tick());
    CHECK(app->assetGuidForPath("only_in_a.txt").has_value());

    app->requestOpenProject(createdB.root);
    // A8/I21's identical one-tick lag, restated for the database: the reconcile runs at the TOP of
    // tick(), BEFORE drawShellUi()'s applyFileRequests() performs the swap -- so the FIRST tick after
    // the request still reconciles against the PRE-swap project. One more plain tick is what makes the
    // swap observable. This is the ONLY case a seed dropping the reconcile block reddens -- I27
    // constructs with the correct root already and never exercises the reconcile at all.
    REQUIRE(app->tick());
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    CHECK(app->assetGuidForPath("only_in_b.txt").has_value());
    CHECK_FALSE(app->assetGuidForPath("only_in_a.txt").has_value());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: requestAssetRescan drives a manual rescan the Refresh button cannot reach from here "
    "(task 3.1.1, I29/AC-38)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "asset scan i29", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/first.txt", "one").empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    REQUIRE(app->tick());
    CHECK(app->assetCount() == 1);

    // A file dropped on disk AFTER the scan is invisible until something asks for a rescan -- there is
    // no filesystem watcher yet (3.1.4's deliverable). The panel's own Refresh button cannot be
    // clicked from this ImGui-free-at-source TU, so requestAssetRescan() is the only mechanically
    // drivable channel -- the requestUndo()/requestLayoutReset() shape, consumed on the very next
    // tick() (no A8 lag: the flag is read at the TOP of tick(), before any swap-related processing).
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/second.txt", "two").empty());
    app->requestAssetRescan();
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    CHECK(app->assetCount() == 2);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- I30: code-review finding 4 -- the panel's OWN one-shot flag actually drains -----------------
//
// AssetBrowserPanel::takeRescanRequest() is only ever set by a real click on the Refresh button, and
// this target is ImGui-free at source (it cannot synthesize a widget click -- the AC-27/FileDialogHost
// precedent). There is therefore no way to drive the panel's flag to `true` through a real frame here
// and observe the drain from the outside -- the exact reason `requestAssetRescan()` (EditorApp's OWN
// flag) had to be invented for I29 above. What CAN be proven mechanically, the same way PU1
// (project_test.cpp) proves ImGui::CloseCurrentPopup() bookkeeping no live test can drive: that
// `takeRescanRequest()` is called and its result stored BEFORE it is combined with
// `assetRescanRequested`, never as the right operand of an `||` whose left operand is
// `assetRescanRequested` -- which is precisely the shape that let `||`'s short-circuit skip the call
// (and therefore never drain the panel's flag) whenever `assetRescanRequested` was already true.
TEST_CASE(
    "editor_app: the reconcile drains the panel's rescan flag unconditionally (task 3.1.1, I30, "
    "code-review finding 4)") {
    constexpr std::string_view SOURCE_PATH = AERO_EDITOR_SRC_DIR "/editor_app.cpp";
    const engine::editor::FileReadResult read = engine::editor::readTextFile(SOURCE_PATH);
    REQUIRE(read.text.has_value());
    const std::string& text = *read.text;
    REQUIRE_FALSE(text.empty());

    // The regressed shape this proof must reject: `takeRescanRequest()` appearing on the SAME line as
    // `assetRescanRequested ||` (the exact pattern the finding reported, whitespace notwithstanding).
    // A textual scan line by line, since the fixed shape spreads the drain and the combine across two
    // statements and the buggy shape fuses them into one.
    std::vector<std::string_view> lines;
    std::string_view remaining = text;
    while (true) {
        const std::size_t newline = remaining.find('\n');
        if (newline == std::string_view::npos) {
            lines.push_back(remaining);
            break;
        }
        lines.push_back(remaining.substr(0, newline));
        remaining.remove_prefix(newline + 1U);
    }

    std::size_t combineLine = lines.size();
    std::size_t drainLine = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const bool hasAssetRescanRequested = lines[i].find("assetRescanRequested") != std::string_view::npos;
        const bool hasOr = lines[i].find("||") != std::string_view::npos;
        const bool hasTake = lines[i].find("takeRescanRequest()") != std::string_view::npos;

        // The regression: a single line combining BOTH the flag and the call is the short-circuit
        // shape, regardless of which side it is written on -- `||` short-circuits its RIGHT operand.
        INFO("line ", i, ": ", lines[i]);
        REQUIRE_FALSE((hasAssetRescanRequested && hasOr && hasTake));

        if (hasTake && drainLine == lines.size()) {
            drainLine = i;  // FIRST occurrence -- the drain
        }
        if (hasAssetRescanRequested && hasOr && combineLine == lines.size()) {
            combineLine = i;  // FIRST occurrence -- the combine
        }
    }

    REQUIRE(drainLine != lines.size());
    REQUIRE(combineLine != lines.size());
    // The drain happens BEFORE the combine reads it. NOT merely belt-and-braces beside the negative
    // check above: it is the only half of this proof that survives clang-format WRAPPING the fused
    // shape. With a marginally longer right operand the formatter breaks the line after the `||`,
    // which puts `assetRescanRequested ||` and `takeRescanRequest()` on two different lines and slips
    // past every line-based check -- but never in that order. See I34 below, this case's sibling for
    // task 3.1.2's second one-shot, which states the same reasoning in full.
    CHECK(drainLine < combineLine);
}

// ---- I31-I33: task 3.1.2's import cache, driven through real frames -----------------------------
// The SAME two-part discipline I27-I29 use: opt OUT of restoring the last project AND redirect the
// recents-file WRITE (BLOCKING-2, restated a third time for a third task).

TEST_CASE("editor: the import cache exists after the first scan and the second scan is free (task 3.1.2, I31)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import cache i31", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);

    const std::string assetsRoot = created.root + "/assets";
    const std::array<std::string, 3> leaves{"a.txt", "b.txt", "c.txt"};
    for (const std::string& leaf : leaves) {
        std::string assetPath = assetsRoot;
        assetPath += "/";
        assetPath += leaf;
        REQUIRE(engine::editor::writeTextFileAtomic(assetPath, "hello").empty());
    }

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    // AC-35/D15: the first scan has nothing to compare against, so all 3 assets are New -- and the
    // index is committed once, unconditionally, at the end of that first scan.
    CHECK(app->assetCacheEntryCount() == 3);
    CHECK(app->assetImportJobCount() == 3);

    const std::filesystem::path indexPath = std::filesystem::path(created.root) /
                                            std::string(engine::editor::ASSET_CACHE_DIR_NAME) /
                                            std::string(engine::editor::ASSET_CACHE_FILE_NAME);
    const std::filesystem::path gitignorePath = std::filesystem::path(created.root) /
                                                std::string(engine::editor::ASSET_CACHE_DIR_NAME) /
                                                std::string(engine::editor::ASSET_CACHE_GITIGNORE_NAME);
    std::error_code existsEc;
    REQUIRE(std::filesystem::exists(indexPath, existsEc));
    REQUIRE_FALSE(existsEc);
    REQUIRE(std::filesystem::exists(gitignorePath, existsEc));
    REQUIRE_FALSE(existsEc);

    std::error_code mtimeEc;
    const std::filesystem::file_time_type mtimeBefore = std::filesystem::last_write_time(indexPath, mtimeEc);
    REQUIRE_FALSE(mtimeEc);

    // The panel's own Refresh button cannot be clicked from this ImGui-free-at-source TU -- I29's own
    // channel, reused here for a PLAIN rescan (not a reimport): the second scan should find every asset
    // already committed to the index and touch NOTHING on disk (D15's whole point).
    app->requestAssetRescan();
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    CHECK(app->assetImportJobCount() == 0);
    const std::optional<engine::editor::ImportChange> change = app->assetImportChangeForPath("a.txt");
    REQUIRE(change.has_value());
    CHECK(*change == engine::editor::ImportChange::UpToDate);

    const std::filesystem::file_time_type mtimeAfter = std::filesystem::last_write_time(indexPath, mtimeEc);
    REQUIRE_FALSE(mtimeEc);
    CHECK(mtimeBefore == mtimeAfter);  // D15: the second scan is FREE -- zero bytes written to the index

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: an edited asset is detected and the change is scoped to that file (task 3.1.2, I32)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import cache i32", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);

    const std::string assetsRoot = created.root + "/assets";
    const std::array<std::string, 3> leaves{"a.txt", "b.txt", "c.txt"};
    for (const std::string& leaf : leaves) {
        std::string assetPath = assetsRoot;
        assetPath += "/";
        assetPath += leaf;
        REQUIRE(engine::editor::writeTextFileAtomic(assetPath, "hello").empty());
    }

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    REQUIRE(app->tick());
    CHECK(app->assetCacheEntryCount() == 3);

    // Write a DIFFERENT LENGTH, never same-length bytes -- R-C1: on a volume with 1-second mtime
    // granularity, a same-length rewrite within that window could still be vouched for by the (size,
    // mtime) fast path and this case would flake.
    std::string editedPath = assetsRoot;
    editedPath += "/b.txt";
    REQUIRE(engine::editor::writeTextFileAtomic(editedPath, "hello, much longer than before").empty());

    app->requestAssetRescan();
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    CHECK(app->assetImportJobCount() == 1);
    const std::optional<engine::editor::ImportChange> editedChange = app->assetImportChangeForPath("b.txt");
    REQUIRE(editedChange.has_value());
    CHECK(*editedChange == engine::editor::ImportChange::SourceChanged);
    const std::optional<engine::editor::ImportChange> unchangedA = app->assetImportChangeForPath("a.txt");
    REQUIRE(unchangedA.has_value());
    CHECK(*unchangedA == engine::editor::ImportChange::UpToDate);
    const std::optional<engine::editor::ImportChange> unchangedC = app->assetImportChangeForPath("c.txt");
    REQUIRE(unchangedC.has_value());
    CHECK(*unchangedC == engine::editor::ImportChange::UpToDate);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: requestAssetReimport drives Reimport All without a button click (task 3.1.2, I33/AC-39, F9)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import cache i33", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);

    const std::string assetsRoot = created.root + "/assets";
    const std::array<std::string, 3> leaves{"a.txt", "b.txt", "c.txt"};
    for (const std::string& leaf : leaves) {
        std::string assetPath = assetsRoot;
        assetPath += "/";
        assetPath += leaf;
        REQUIRE(engine::editor::writeTextFileAtomic(assetPath, "hello").empty());
    }

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    REQUIRE(app->tick());  // the first scan: every asset is New (3 jobs), and the index is committed
    CHECK(app->assetImportJobCount() == 3);

    // A plain rescan settles into the steady state this case's precondition ("New AGAIN", below)
    // actually needs -- I31 already proves this transition on its own.
    app->requestAssetRescan();
    REQUIRE(app->tick());
    CHECK(app->assetImportJobCount() == 0);

    // No panel exists in this ImGui-free-at-source TU to click Reimport All (human row 12) --
    // requestAssetReimport() is the black-box channel, exactly as requestAssetRescan() was for I29.
    app->requestAssetReimport();
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());
    CHECK(app->assetImportJobCount() == 3);  // AC-39: every asset is New again -- the cache was discarded

    // The FOLLOWING PLAIN rescan (not a second reimport) returns the count to 0 -- proving the reimport's
    // own scan already committed a fresh index, not merely cleared the old one.
    app->requestAssetRescan();
    REQUIRE(app->tick());
    CHECK(app->assetImportJobCount() == 0);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- I34: task 3.1.2's SECOND one-shot -- Reimport All -- drains unconditionally too --------------
//
// I30's exact algorithm, retargeted at the `takeReimportRequest()` / `reimport` pair, and needed for a
// reason I33 above cannot cover: `assetReimportRequested` is the ONLY flag ever set in this TU (there
// is no panel here to click Reimport All), so seeding the fused shape
//     const bool reimport = assetReimportRequested || (assetBrowserPanel != nullptr && ...->take...());
// leaves I33 -- and every other case in this tree -- green. Confirmed by direct sabotage (seed S28
// reddens nothing), which is what makes a mechanical source-text proof the only available discriminator.
//
// BOTH halves below are load-bearing, and the ORDERING half is the one that carries the weight. The
// plan's §V6 grep gate and the single-line negative check each fire only while the fused expression
// fits on ONE line; with a marginally longer right operand clang-format breaks after the `||`, leaving
// `assetReimportRequested ||` on one line and `takeReimportRequest()` on the next -- past every
// line-based check. What survives that wrapping is `drainLine < combineLine`: a fused expression can
// only ever place the call ON or AFTER the combine, never before it.
TEST_CASE(
    "editor_app: the reconcile drains the panel's reimport flag unconditionally (task 3.1.2, I34, "
    "AC-39, seed S28)") {
    constexpr std::string_view SOURCE_PATH = AERO_EDITOR_SRC_DIR "/editor_app.cpp";
    const engine::editor::FileReadResult read = engine::editor::readTextFile(SOURCE_PATH);
    REQUIRE(read.text.has_value());
    const std::string& text = *read.text;
    REQUIRE_FALSE(text.empty());

    std::vector<std::string_view> lines;
    std::string_view remaining = text;
    while (true) {
        const std::size_t newline = remaining.find('\n');
        if (newline == std::string_view::npos) {
            lines.push_back(remaining);
            break;
        }
        lines.push_back(remaining.substr(0, newline));
        remaining.remove_prefix(newline + 1U);
    }

    std::size_t combineLine = lines.size();
    std::size_t drainLine = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const bool hasAssetReimportRequested = lines[i].find("assetReimportRequested") != std::string_view::npos;
        const bool hasOr = lines[i].find("||") != std::string_view::npos;
        const bool hasTake = lines[i].find("takeReimportRequest()") != std::string_view::npos;

        // Half 1, the negative check: a single line carrying BOTH the flag and the call is the
        // short-circuit shape, whichever side it is written on -- `||` short-circuits its RIGHT operand.
        INFO("line ", i, ": ", lines[i]);
        REQUIRE_FALSE((hasAssetReimportRequested && hasOr && hasTake));

        if (hasTake && drainLine == lines.size()) {
            drainLine = i;  // FIRST occurrence -- the drain
        }
        if (hasAssetReimportRequested && hasOr && combineLine == lines.size()) {
            combineLine = i;  // FIRST occurrence -- the combine
        }
    }

    REQUIRE(drainLine != lines.size());
    REQUIRE(combineLine != lines.size());
    // Half 2, the ordering check -- the half that survives a clang-format line break after the `||`.
    CHECK(drainLine < combineLine);
}

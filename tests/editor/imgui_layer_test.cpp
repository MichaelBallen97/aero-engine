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
#include <aero/core/content_hash.hpp>     // task 3.2.4, I78: the cache hit's own settings fingerprint
#include <aero/core/log.hpp>              // AERO_LOG_* + initLogging (cases B and C)
#include <aero/editor/asset_cache.hpp>    // task 3.1.2: ImportChange, ASSET_CACHE_DIR_NAME/FILE_NAME/
                                          // GITIGNORE_NAME -- I31's index-path/gitignore-path assertions
#include <aero/editor/asset_meta.hpp>     // task 3.1.3: writeMetaText, for I39/I41's orphan fixtures
#include <aero/editor/blender_tool.hpp>   // task 3.2.4: ExportProvenance + BLENDER_EXPORT_DIR_NAME (I78)
#include <aero/editor/command_stack.hpp>  // task 2.4.1
#include <aero/editor/component_ops.hpp>
#include <aero/editor/console_model.hpp>  // DEFAULT_LOG_HISTORY_CAPACITY (case C)
#include <aero/editor/editor_app.hpp>
#include <aero/editor/editor_camera.hpp>    // task 2.3.1
#include <aero/editor/entity_commands.hpp>  // task 2.4.2
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/model_import_session.hpp>  // task 3.2.1: SessionState, named directly (I52-I59)
#include <aero/editor/panel_registry.hpp>
#include <aero/editor/picking.hpp>       // task 2.3.2
#include <aero/editor/project.hpp>       // task 2.6.1
#include <aero/editor/scene_bounds.hpp>  // task 2.3.1
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/selection_overlay.hpp>  // task 2.3.2
#include <aero/editor/text_file.hpp>          // task 2.6.1: writeTextFileAtomic, for I23/I24's recents file
#include <aero/editor/thumbnail_cache.hpp>    // task 3.1.3: MAX_THUMBNAIL_DECODES_PER_TICK, MAX_THUMBNAILS_RESIDENT
#include <aero/editor/transform_command.hpp>  // task 2.4.1
#include <aero/editor/transform_ops.hpp>      // task 2.4.1
#include <aero/platform/platform.hpp>
#include <aero/reflect/material_format.hpp>  // task 3.4.2: MaterialDocument, named directly (I84)
#include <aero/rhi/device.hpp>
#include <aero/scene/scene.hpp>
#include <aero/scene/world.hpp>

// task 3.1.5 (SL1-SL10): the scene-asset loader is SRC-PRIVATE, so it is reached the way
// blender_service_test.cpp reaches blender_process.hpp -- by relative path into editor/src. It names
// scene_render::MeshBinding, which is why aero::scene_render is on this target's link line.
#include "../../editor/src/scene_asset_loader.hpp"
#include "../../editor/src/viewport_panel.hpp"  // task 3.6.3: ViewportPanel's three test seams --
                                                // postProcess(), tonemapParams(), requestTonemapParams().
                                                // The scene_asset_loader.hpp precedent, one file over.
#include "rhi_test_support.hpp"

#include <SDL3/SDL_filesystem.h>  // task 2.3.3 I5: SDL_GetBasePath(), matching imgui_layer.cpp:38
#include <doctest/doctest.h>

#include <algorithm>
#include <array>  // the frozen panel-id roster; reached transitively on libc++, not on MSVC (813bc4d)
#include <cstdint>
#include <filesystem>
#include <format>  // task 3.2.2, I65: truncatedFbxText()'s programmatic 257-node fixture
#include <fstream>
#include <memory>  // task 2.4.1: std::make_unique<TransformCommand>
#include <optional>
#include <ostream>  // MSVC alone needs the complete type to stringify a string_view inside a CHECK
#include <span>     // task 3.2.4, I78: std::as_bytes over the fingerprint's own text
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

// task 3.1.3 (I36-I38): four REAL, tiny (2x2 truecolor) PNGs, byte-for-byte from a literal array --
// generated once, offline, with Python's zlib (a standard, conformant PNG: signature + IHDR (color
// type 2, 8-bit) + one zlib-compressed IDAT with a filter-type-0 byte per scanline + IEND). Verified
// by round-tripping through zlib.decompress before being pasted here. Never generated at test time --
// this TU is ImGui-free at source and links no image encoder (INV-V10 stays true: stb_image itself is
// STILL confined to thumbnail_store.cpp; nothing here decodes or encodes anything).
constexpr std::array<unsigned char, 73> TINY_PNG_RED{
    137, 80, 78, 71, 13,  10,  26,  10,  0,  0,   0,  13, 73, 72, 68, 82, 0,   0,   0,  2,   0,   0,   0,  2,  8,
    2,   0,  0,  0,  253, 212, 154, 115, 0,  0,   0,  16, 73, 68, 65, 84, 120, 218, 99, 248, 207, 192, 0,  68, 12,
    16,  10, 0,  31, 238, 3,   253, 99,  94, 187, 91, 0,  0,  0,  0,  73, 69,  78,  68, 174, 66,  96,  130};
constexpr std::array<unsigned char, 72> TINY_PNG_GREEN{
    137, 80, 78, 71, 13, 10,  26,  10,  0,   0,  0, 13,  73, 72, 68, 82, 0,  0,   0,   2,  0,   0,   0,   2,
    8,   2,  0,  0,  0,  253, 212, 154, 115, 0,  0, 0,   15, 73, 68, 65, 84, 120, 218, 99, 96,  248, 207, 0,
    66,  16, 10, 0,  27, 242, 3,   253, 212, 47, 4, 128, 0,  0,  0,  0,  73, 69,  78,  68, 174, 66,  96,  130};
constexpr std::array<unsigned char, 72> TINY_PNG_BLUE{
    137, 80, 78, 71, 13, 10,  26,  10,  0,   0,   0,   13,  73, 72, 68, 82, 0,  0,   0,   2,  0,   0,  0,   2,
    8,   2,  0,  0,  0,  253, 212, 154, 115, 0,   0,   0,   15, 73, 68, 65, 84, 120, 218, 99, 96,  96, 248, 15,
    70,  96, 10, 0,  23, 246, 3,   253, 199, 144, 139, 180, 0,  0,  0,  0,  73, 69,  78,  68, 174, 66, 96,  130};
constexpr std::array<unsigned char, 73> TINY_PNG_YELLOW{
    137, 80, 78, 71, 13,  10,  26,  10,  0,   0,   0,  13, 73, 72, 68, 82, 0,   0,   0,  2,   0,   0,   0,  2,   8,
    2,   0,  0,  0,  253, 212, 154, 115, 0,   0,   0,  16, 73, 68, 65, 84, 120, 218, 99, 248, 255, 159, 1,  136, 24,
    32,  20, 0,  59, 210, 7,   249, 37,  110, 227, 55, 0,  0,  0,  0,  73, 69,  78,  68, 174, 66,  96,  130};

// A valid PNG name whose BYTES are ASCII text (R3's chosen corrupt-image shape): it fails at
// stbi_info_from_memory, whose byte reader is bounds-checked by construction, before any decode loop
// ever runs -- never a truncated real PNG, which risks the UBSan-abort surface R3 documents.
constexpr std::string_view CORRUPT_PNG_BYTES = "this is not a png file, just ascii text pretending to be one";

// task 3.1.3: writes raw bytes (never text) through writeTextFileAtomic, which is binary on both
// sides already (text_file.cpp) -- a byte array reinterpreted as a string_view is exactly what it
// expects.
[[nodiscard]] std::string writeBinaryFixture(const std::string& path, const unsigned char* data, std::size_t size) {
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    return engine::editor::writeTextFileAtomic(path, bytes);
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

    // The D8 registration -- SEVEN absolute panel-count sites in the tree, measured directly rather
    // than assumed (this one plus :273, :501, :606, :2606, :2776 and :4728 at the moment task 3.4.2
    // moved them), all of which moved from 7 to 8 the moment this task registered the eighth default
    // panel, "Material". The count this comment used to claim was SIX, with line numbers that had
    // drifted by about eight: task 3.2.1's own AC-50 site was added and never listed -- the identical
    // drift the comment boasts of having caught once, caught a second time by the task that had to
    // move all of them. (`:1419`/`:1425`'s captured `panelCountBefore` is a count-AGNOSTIC site by
    // construction -- the "gizmo" window is not a panel -- and is NOT one of the seven.)
    CHECK(app->panels().count() == 8);

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
    CHECK(app->panels().count() == 8);
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
    CHECK(app->panels().count() == 8);
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
    CHECK(app->panels().count() == 8);

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
    CHECK(app->panels().count() == 8);

    // EVERY id, in order (Phase 2 audit). Each panel's own header calls its id FROZEN because it is
    // the imgui.ini settings key -- renaming one orphans every user's saved layout for that panel --
    // but only "Project Settings" and "Inspector" were ever pinned, and a rename of the other four
    // passed the whole suite green. Worse than silent: the GPU cases reach these panels by
    // `setVisible("Console", …)`, and setVisible on an unknown id is a LOGGED NO-OP, so a renamed
    // panel would simply stop being hidden and the case that depends on hiding it would assert
    // against a panel that never draws -- 2.2.4's C5 trap, re-armed. This is a PERSISTED FORMAT, so
    // treat a diff here like a file-format change, not a test to update.
    // task 3.4.2: EIGHT, so the two ids added since the Phase 2 audit -- "Import Details" (3.2.1) and
    // "Material" (this task) -- carry the same id-freeze pin the other six already have. The array was
    // a PREFIX check covering indices 0-5 while its own comment claimed "EVERY id", so growing it is
    // new coverage rather than a rewording pass: it makes an accidental rename of a FROZEN imgui.ini
    // key a red test instead of a silently migrated layout.
    const std::array<const char*, 8> frozenPanelIds{"Hierarchy", "Inspector",        "Viewport",       "Console",
                                                    "Assets",    "Project Settings", "Import Details", "Material"};
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
        CHECK(app->panels().count() == 8);

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

TEST_CASE(
    "editor: a write-failed record's import state is not reported as up to date (task 3.1.2, code-review "
    "finding 3, I35)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import cache i35", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);

    const std::string assetsRoot = created.root + "/assets";
    const std::string assetPath = assetsRoot + "/a.txt";
    REQUIRE(engine::editor::writeTextFileAtomic(assetPath, "hello").empty());

    // AD25's own pattern: a read-only assets root makes phase 7's sidecar write fail while the
    // in-memory record (state Created, no identity written to disk) is kept.
    std::error_code ec;
    std::filesystem::permissions(assetsRoot, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    REQUIRE(app->tick());  // the first scan attempts to write a.txt.meta and fails
    CHECK(app->presentedLastFrame());

    std::error_code restoreEc;
    std::filesystem::permissions(assetsRoot, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace,
                                 restoreEc);

    std::error_code existsEc;
    const std::string metaPath = assetPath + ".meta";
    if (std::filesystem::exists(metaPath, existsEc) && !existsEc) {
        MESSAGE("running as a user for whom a read-only directory does not block file creation -- seed did not land");
        app->requestQuit();
        CHECK(app->tick() == false);
        app.reset();
        return;
    }

    const std::optional<engine::Guid> guid = app->assetGuidForPath("a.txt");
    REQUIRE(guid.has_value());
    CHECK(guid->valid());  // the in-memory identity survives the write failure (AD25's own rule)

    // The whole point of the finding: NOT "up to date" -- the sidecar never landed and nothing was
    // hashed under it, so the accessor must refuse rather than report a stale default.
    const std::optional<engine::editor::ImportChange> change = app->assetImportChangeForPath("a.txt");
    CHECK_FALSE(change.has_value());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- I36-I42: task 3.1.3's thumbnails, the grid, search and the orphan delete, through real frames -

TEST_CASE(
    "editor: real thumbnails decode within the per-tick budget and eventually become ready (task 3.1.3, "
    "I36, AC-6/AC-8, seeds S4/S8/S9)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "thumbnails i36", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string assetsRoot = created.root + "/assets";
    REQUIRE(writeBinaryFixture(assetsRoot + "/a.png", TINY_PNG_RED.data(), TINY_PNG_RED.size()).empty());
    REQUIRE(writeBinaryFixture(assetsRoot + "/b.png", TINY_PNG_GREEN.data(), TINY_PNG_GREEN.size()).empty());
    REQUIRE(writeBinaryFixture(assetsRoot + "/c.png", TINY_PNG_BLUE.data(), TINY_PNG_BLUE.size()).empty());
    REQUIRE(writeBinaryFixture(assetsRoot + "/d.png", TINY_PNG_YELLOW.data(), TINY_PNG_YELLOW.size()).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    // The baseline is read BEFORE the FIRST tick, not after -- the scan tick ALSO draws (drawShellUi
    // runs every tick unconditionally) and therefore ALSO calls serviceThumbnails() once, so all 4
    // assets are already visible and touched by the time that first tick() returns. Reading the
    // baseline afterward would hide a seed that decodes everything in that single first burst (found
    // directly: sabotage seed S4 stayed green against the ORIGINAL, later-baseline shape of this case).
    std::size_t previousAttempts = app->thumbnailLoadAttempts();  // 0 -- no tick has run yet
    REQUIRE(app->tick());                                         // the scan: 4 New assets
    CHECK(app->presentedLastFrame());
    CHECK(app->assetCount() == 4);
    {
        const std::size_t attempts = app->thumbnailLoadAttempts();
        CHECK(attempts - previousAttempts <= engine::editor::MAX_THUMBNAIL_DECODES_PER_TICK);
        previousAttempts = attempts;
    }

    for (int i = 0; i < 30; ++i) {
        REQUIRE(app->tick());
        const std::size_t attempts = app->thumbnailLoadAttempts();
        // AC-8/seed S4: never more than the budget between two consecutive ticks.
        CHECK(attempts - previousAttempts <= engine::editor::MAX_THUMBNAIL_DECODES_PER_TICK);
        previousAttempts = attempts;
    }
    CHECK(app->presentedLastFrame());
    CHECK(app->thumbnailReadyCount() > 0);  // AC-6 -- at least one real thumbnail became Ready

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: a corrupt image is counted unavailable and never retried (task 3.1.3, I37, INV-V4/AC-9, "
    "seed S1)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "thumbnails i37", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string assetsRoot = created.root + "/assets";
    // R3: chosen to fail at stbi_info_from_memory, never reaching a decode loop -- ASCII text named
    // ".png", not a truncated real PNG.
    REQUIRE(engine::editor::writeTextFileAtomic(assetsRoot + "/bad.png", CORRUPT_PNG_BYTES).empty());

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

    std::size_t attemptsAfterFirst = 0;
    for (int i = 0; i < 20; ++i) {
        REQUIRE(app->tick());
        CHECK(app->thumbnailUnavailableCount() == 1);  // INV-V4: sticky, never Ready
        if (i == 0) {
            attemptsAfterFirst = app->thumbnailLoadAttempts();
        } else {
            // AC-9: attempts PLATEAU -- a Failed key is read exactly once, ever.
            CHECK(app->thumbnailLoadAttempts() == attemptsAfterFirst);
        }
    }
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the resident thumbnail count never exceeds the cap (task 3.1.3, I38, INV-V5, E12, seeds S2/S3)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "thumbnails i38", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string assetsRoot = created.root + "/assets";
    // More files than the small 320x180 window can show at once -- the content itself is IDENTICAL
    // (only the GUID differs per file, which is all a ThumbnailKey needs) since this case tests the
    // CAP, not visual variety.
    constexpr int FILE_COUNT = 40;
    for (int i = 0; i < FILE_COUNT; ++i) {
        const std::string path = assetsRoot + "/img" + std::to_string(i) + ".png";
        REQUIRE(writeBinaryFixture(path, TINY_PNG_RED.data(), TINY_PNG_RED.size()).empty());
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
    CHECK(app->assetCount() == FILE_COUNT);

    bool everNonZero = false;
    for (int i = 0; i < 60; ++i) {
        REQUIRE(app->tick());
        CHECK(app->thumbnailResidentCount() <= engine::editor::MAX_THUMBNAILS_RESIDENT);
        if (app->thumbnailResidentCount() > 0) {
            everNonZero = true;
        }
    }
    CHECK(everNonZero);  // anti-vacuity -- the bound was actually exercised, not merely never reached

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: every asset browser draw path stays balanced (task 3.1.3, I39, AC-21)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "thumbnails i39", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string assetsRoot = created.root + "/assets";
    REQUIRE(writeBinaryFixture(assetsRoot + "/a.png", TINY_PNG_RED.data(), TINY_PNG_RED.size()).empty());
    // An orphan, so the Issues section actually draws content (both open and closed).
    REQUIRE(engine::editor::writeTextFileAtomic(assetsRoot + "/gone.png.meta",
                                                engine::editor::writeMetaText(engine::GuidGenerator(39).next()))
                .empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    // code-review finding 4: this case used to drive three plain Grid-view ticks and CONCEDE, in its
    // own comment, that it could not reach the search box -- which left drawContentsList()'s search
    // branch (an asymmetric BeginTable/EndTable pair plus an ImGuiListClipper), drawContentsGrid()'s
    // search branch, and the delete confirmation modal executed by NO test at all. Worse, defaulting
    // the view to Grid had silently REMOVED the table-path coverage every pre-3.1.3 GPU case gave the
    // List view for free. The four new EditorApp seams drive each of them for real.
    //
    // WHAT `presentedLastFrame()` DOES AND DOES NOT PROVE HERE -- measured, not assumed. ImGui 1.92.8
    // ships ConfigErrorRecovery = true, so an unbalanced BeginTable/EndTable is silently REPAIRED
    // rather than aborting: deleting this branch's EndTable() and re-running this case leaves it
    // 43/43 GREEN (verified directly, with a control seed proving the build was picking the edit up).
    // So these ticks prove the paths EXECUTE -- which is the coverage finding 4 asked for, and enough
    // for ASan/UBSan and any crash or bad read inside them to be caught -- but they are NOT a proof of
    // ImGui call balance on their own. AC-21's balance claim rests on the source and on human rows,
    // exactly as it does for the modal. Do not upgrade this comment to "balance proven" on a green run.
    //
    // `pending` holds exactly ONE action, so each request gets its own tick to be drained.
    REQUIRE(app->tick());  // 1: Grid, default, Issues populated by the orphan above
    CHECK(app->presentedLastFrame());
    REQUIRE(app->tick());  // 2: let the default dock layout settle before focusing anything
    CHECK(app->presentedLastFrame());

    // The Assets panel shares DockSlot::Bottom with the Console, and whichever tab wins the FIRST
    // frame stays active forever with no further signal. drawPanels() calls onDraw() ONLY when
    // ImGui::Begin() returns true (shell_ui.cpp:356), so a tabbed-behind panel never drains
    // `pending` -- every request below would be recorded into a panel that never draws, and this
    // case would pass while proving NOTHING.
    //
    // Found exactly that way, twice: a deliberately unbalanced EndTable in the List search branch
    // failed to redden this case, and the assertions below then showed why. The focus must also come
    // AFTER the layout exists -- requesting it before the first tick lands while buildDefaultLayout
    // is still running and does nothing.
    app->requestPanelFocus("Assets");
    REQUIRE(app->tick());  // 3: focus applied before DockSpaceOverViewport, so it lands this frame
    CHECK(app->presentedLastFrame());

    // --- the Issues section, OPEN (it draws closed by default) ---------------------------------
    // Issues opens via its own CollapsingHeader, which this TU cannot click; the modal request below
    // draws the section's body regardless, so the open path is covered there.

    // --- List view, no search: the table path pre-3.1.3 cases used to cover for free ------------
    app->requestAssetBrowserViewMode(engine::editor::AssetViewMode::List);
    REQUIRE(app->tick());  // 2: drains SetViewMode
    CHECK(app->presentedLastFrame());
    REQUIRE(app->tick());  // 3: first frame actually DRAWN as List
    CHECK(app->presentedLastFrame());

    // --- List view, WITH a search hit: BeginTable/EndTable + the clipper, previously uncovered ---
    app->requestAssetBrowserSearch("a");  // matches a.png
    REQUIRE(app->tick());                 // 4: drains SetQuery, rebuilds searchRows
    CHECK(app->presentedLastFrame());
    // LOAD-BEARING, not decorative: the table branch is entered ONLY when there is at least one hit,
    // so without this REQUIRE the whole case can drive every seam, draw the plain listing, and pass
    // while executing none of the code it names.
    REQUIRE(app->assetBrowserListViewActive());
    REQUIRE(app->assetBrowserSearchHitCount() > 0);
    REQUIRE(app->tick());  // 5: first frame drawn through drawContentsList's search branch
    CHECK(app->presentedLastFrame());

    // --- List view, search with ZERO results: the "No assets match" arm -------------------------
    app->requestAssetBrowserSearch("zzz-no-such-asset");
    REQUIRE(app->tick());  // 6
    CHECK(app->presentedLastFrame());
    REQUIRE(app->tick());  // 7
    CHECK(app->presentedLastFrame());

    // --- back to Grid, still searching: drawContentsGrid's search branch ------------------------
    app->requestAssetBrowserViewMode(engine::editor::AssetViewMode::Grid);
    REQUIRE(app->tick());  // 8
    CHECK(app->presentedLastFrame());
    app->requestAssetBrowserSearch("a");
    REQUIRE(app->tick());  // 9
    CHECK(app->presentedLastFrame());
    REQUIRE(app->tick());  // 10: first frame drawn through the grid's search branch
    CHECK(app->presentedLastFrame());

    // --- the kind filter, composed with the query (AC-13's second clause) -----------------------
    app->requestAssetBrowserKindFilter("all");
    REQUIRE(app->tick());  // 11
    CHECK(app->presentedLastFrame());

    // --- clear the search, back to the plain directory listing ----------------------------------
    app->requestAssetBrowserSearch("");
    REQUIRE(app->tick());  // 12: drains ClearSearch
    CHECK(app->presentedLastFrame());

    // --- the delete CONFIRMATION MODAL, previously executed by no test at all --------------------
    // I41 drives EditorApp::requestOrphanDelete(), which performs the delete directly and bypasses
    // this modal entirely. This drives the panel's OWN orphan Delete button instead, so the modal
    // actually opens and draws -- and, crucially, is DISMISSED without confirming, proving the
    // sidecar survives a modal that was opened and abandoned.
    app->requestAssetBrowserDeleteOrphanClick("gone.png.meta");
    REQUIRE(app->tick());  // 13: drains RequestDeleteOrphan, sets pendingOrphanDelete
    CHECK(app->presentedLastFrame());
    REQUIRE(app->tick());  // 14: the modal is open and drawing
    CHECK(app->presentedLastFrame());
    REQUIRE(app->tick());  // 15: still open, still balanced
    CHECK(app->presentedLastFrame());
    // Never confirmed, so the orphan is still on disk -- the delete half of AC-17/AC-20.
    CHECK(engine::editor::fileExists(assetsRoot + "/gone.png.meta"));

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: a project swap clears the thumbnail store and ledger (task 3.1.3, I40, INV-V6/E27, seed "
    "S26)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "thumbnails i40", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string locationA = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome createdA = engine::editor::createProject(locationA, "GameA", "0.1.0");
    REQUIRE(createdA.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(writeBinaryFixture(createdA.root + "/assets/a.png", TINY_PNG_RED.data(), TINY_PNG_RED.size()).empty());

    const std::string locationB = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome createdB = engine::editor::createProject(locationB, "GameB", "0.1.0");
    REQUIRE(createdB.problem == engine::editor::CreateProblem::Ok);

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = createdA.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    for (int i = 0; i < 5; ++i) {
        REQUIRE(app->tick());
    }
    CHECK(app->thumbnailReadyCount() > 0);

    app->requestOpenProject(createdB.root);
    // A8/I21's identical one-tick lag, restated for the thumbnail store: the reconcile runs at the TOP
    // of tick(), BEFORE drawShellUi()'s applyFileRequests() performs the swap.
    REQUIRE(app->tick());
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    CHECK(app->thumbnailResidentCount() == 0);
    CHECK(app->thumbnailReadyCount() == 0);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: the orphan-delete round trip via requestOrphanDelete (task 3.1.3, I41, AC-18/AC-19, seeds "
    "S27/S28)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "orphan delete i41", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string assetsRoot = created.root + "/assets";
    const std::string orphanMetaPath = assetsRoot + "/gone.png.meta";
    REQUIRE(engine::editor::writeTextFileAtomic(orphanMetaPath,
                                                engine::editor::writeMetaText(engine::GuidGenerator(41).next()))
                .empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    REQUIRE(app->tick());  // the first scan finds the orphan
    CHECK(app->assetOrphanCount() == 1);
    REQUIRE(engine::editor::fileExists(orphanMetaPath));

    // The GPU tier is ImGui-free at source and cannot click the modal's Delete button --
    // requestOrphanDelete() is the only channel (§Q Q1), the requestAssetRescan() shape verbatim.
    app->requestOrphanDelete("gone.png.meta");
    REQUIRE(app->tick());  // ONE tick: the delete and the rescan that observes it are one pass (S28)
    CHECK(app->presentedLastFrame());

    CHECK_FALSE(engine::editor::fileExists(orphanMetaPath));
    CHECK(app->assetOrphanCount() == 0);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- I42: mechanical source-text proof (A20, F9, seed S31), the I30/I34 shape's third instance -----
//
// This task's own drain does not use the `flag || take()` shape I30/I34 guard against (there is no
// boolean flag to OR against for the orphan-delete one-shot) -- it reads
// `assetBrowserPanel->takeOrphanDeleteRequest()` into a named local directly. The invariant that
// actually matters here is unchanged: the call must never be fused into a `||` expression by a future
// edit, which would let short-circuit evaluation skip the drain. This proof is broader than I30/I34's
// line-local check for exactly that reason -- it flags takeOrphanDeleteRequest() sharing ANY line with
// `||`, not merely one naming a specific flag.
TEST_CASE(
    "editor_app: takeOrphanDeleteRequest is drained as its own statement, never fused with || (task "
    "3.1.3, I42, A20, F9, seed S31)") {
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

    std::size_t drainLine = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        // Comment-stripped BEFORE matching (the shell guards' own load-bearing rule, applied here too)
        // -- this task's own prose explaining the invariant legitimately names both tokens on one
        // line, and a check that cannot tell code from comment is not a mechanical proof at all.
        const std::string_view line = lines[i];
        const std::size_t commentStart = line.find("//");
        const std::string_view code = commentStart == std::string_view::npos ? line : line.substr(0, commentStart);
        const bool hasTake = code.find("takeOrphanDeleteRequest()") != std::string_view::npos;
        const bool hasOr = code.find("||") != std::string_view::npos;
        INFO("line ", i, ": ", line);
        REQUIRE_FALSE((hasTake && hasOr));
        if (hasTake && drainLine == lines.size()) {
            drainLine = i;  // FIRST occurrence -- the drain
        }
    }
    REQUIRE(drainLine != lines.size());
}

// ---- I43: code-review BLOCKING-1 -- Reimport All must not free a texture this SAME frame's draw
// walk already wrote into the ImGui draw list --------------------------------------------------------
//
// The bug (before the fix): AssetBrowserPanel::applyPending()'s ReimportAll arm called
// `ledger.clear(); store.clear();` DIRECTLY, from INSIDE the draw walk, AFTER drawContentsGrid's
// drawTile() had already baked every Ready thumbnail's native texture pointer into THIS frame's ImGui
// draw list. EditorApp::tick()'s later `layer.endFrame()` then submitted that draw data referencing
// the freed pointer -- a use-after-free that is SYNCHRONOUS on Vulkan/D3D12 and only deferred (hence
// silent) on Metal, which is why this stayed green on macOS CI while it would abort under ASan on
// Linux/Windows. `EditorApp::requestAssetReimport()` (task 3.1.2, I33) cannot reproduce this: it drives
// a DIFFERENT path (the deep rescan itself) that never touches the panel's `pending` at all, so this
// arm never runs at all through that channel.
//
// The portable, platform-independent signature: with the OLD code, `ledger.clear()` ran synchronously
// inside applyPending(), wiping the just-touched key back to Absent -- and since the decode budget (2)
// covers this single asset, `serviceThumbnails()`'s OWN decode pass (later in the SAME tick) instantly
// re-decodes and re-uploads it, so `thumbnailReadyCount()` bounces back to 1 by the time the tick
// returns and does NOT discriminate on its own. `thumbnailLoadAttempts()` does: a redundant SAME-TICK
// decode is exactly the symptom of "the ledger forgot an entry it should not have", so the OLD code
// increments it on the triggering tick, while the FIX (which excludes anything touched THIS frame, the
// SAME protection normal cap eviction already relies on, E12) never wipes the entry at all and needs no
// redundant redecode. This reproduces the exact same-tick draw-then-clear race the finding describes,
// deterministically on every OS, with no dependency on ASan actually catching the freed pointer -- and
// it is the DIRECT symptom of the destroy the fix removes, not a mere proxy for it.
TEST_CASE(
    "editor: Reimport All does not free a thumbnail drawn on the SAME tick (task 3.1.3 code review, "
    "BLOCKING-1)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    // A LARGER window than this file's other GPU cases (I36-I42 use 320x180): the Bottom dock slot
    // "Assets" shares with Console (D3) is one of five competing slots, and at 320x180 it settles to a
    // sliver too short to show even one row -- ImGuiListClipper then legitimately reports zero visible
    // rows and drawTile() is never called, silently. Confirmed directly: at 320x180 the tile draws only
    // on the dockspace's very first frame and never again; at 1280x800 it draws every tick.
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "reimport all i43", .width = 1280, .height = 800});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string assetsRoot = created.root + "/assets";
    REQUIRE(writeBinaryFixture(assetsRoot + "/a.png", TINY_PNG_RED.data(), TINY_PNG_RED.size()).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());

    // "Assets" shares its dock slot with "Console" (D3); whichever wins the FIRST frame's tab stays
    // active forever afterward with no further per-frame signal, and it is NOT guaranteed to be
    // "Assets" -- requestPanelFocus keeps it the active tab (and therefore actually DRAWN, phase 4
    // included) on every tick this test needs it to be, the ONLY way this ImGui-free-at-source TU can
    // ask for that (code-review BLOCKING-1 test seam). Re-requested every tick, matching
    // requestPanelFocus's own one-shot-per-tick contract.
    //
    // Tick until the single thumbnail becomes Ready AND drawn (I36's own "tick until ready" shape),
    // then a few extra margin ticks confirming BOTH stay true -- the state the triggering tick needs.
    bool ready = false;
    for (int i = 0; i < 30 && !ready; ++i) {
        app->requestPanelFocus("Assets");
        REQUIRE(app->tick());
        ready = app->thumbnailReadyCount() > 0;
    }
    REQUIRE(ready);
    for (int i = 0; i < 3; ++i) {
        app->requestPanelFocus("Assets");
        REQUIRE(app->tick());
        REQUIRE(app->thumbnailReadyCount() > 0);
    }
    CHECK(app->presentedLastFrame());
    const std::size_t attemptsBeforeTrigger = app->thumbnailLoadAttempts();

    // Queue the panel's OWN ReimportAll arm BEFORE the next tick() -- record(ActionKind::ReimportAll)
    // is set immediately, so the VERY NEXT tick's onDraw() draws the still-Ready tile (phase 4, baking
    // its texture pointer into THIS frame's draw list) and only THEN drains `pending` in applyPending()
    // -- the exact same-tick ordering a real button click produces.
    app->requestPanelFocus("Assets");
    app->requestAssetBrowserReimportAll();
    REQUIRE(app->tick());  // the triggering tick: draw, then applyPending(), then serviceThumbnails()
    CHECK(app->presentedLastFrame());

    // THE discriminator (see the comment above the test): the fix never wipes a key touched this SAME
    // tick, so it needs no redundant redecode, and thumbnailLoadAttempts() stays UNCHANGED across the
    // triggering tick. thumbnailReadyCount() is kept as a sanity check -- true either way, since the
    // pre-fix code's redundant same-tick redecode also lands Ready, just via a NEW texture that leaves
    // the OLD one dangling in the draw list already built this frame.
    CHECK(app->thumbnailLoadAttempts() == attemptsBeforeTrigger);
    CHECK(app->thumbnailReadyCount() > 0);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- I44-I50: task 3.1.4's hot-reload assets-tree watcher, through real frames -------------------
//
// §A-1 is mandatory here: the FIRST scan of a fresh project writes .meta sidecars, which the next
// sweep legitimately sees as additions and acts on exactly once (a rescan that writes ZERO bytes).
// Every case below therefore reaches QUIESCENCE first and asserts trigger-count DELTAS, never an
// absolute value -- a case that asserted `assetWatchTriggerCount() == 0` or `== 1` on a fresh project
// would be asserting the wrong number, not flaky.
namespace {

// task 3.1.4: ticks until the watcher has completed `sweeps` MORE sweeps than it had on entry, with
// a hard tick ceiling so a wedged watcher FAILS the case instead of hanging the suite. Returns false
// on exhaustion so the caller can REQUIRE it.
[[nodiscard]] bool tickSweeps(engine::editor::EditorApp& app, std::uint64_t sweeps, int maxTicks = 400) {
    const std::uint64_t target = app.assetWatchSweepCount() + sweeps;
    for (int i = 0; i < maxTicks && app.assetWatchSweepCount() < target; ++i) {
        if (!app.tick()) {
            return false;
        }
    }
    return app.assetWatchSweepCount() >= target;
}

// task 3.1.4 (D4/E17): ticks until `quietSweeps` CONSECUTIVE sweeps complete with NO new trigger.
// This is MANDATORY before any assertion about trigger counts, and it is not defensive: the FIRST
// scan of a fresh project WRITES .meta sidecars, which the next sweep legitimately sees as additions
// and acts on exactly once (a rescan that writes ZERO bytes). Without reaching quiescence first, a
// case that asserts `triggerCount() == 0` or `== 1` is asserting the wrong number.
[[nodiscard]] bool tickToQuiescence(engine::editor::EditorApp& app, std::uint64_t quietSweeps = 3, int maxTicks = 800) {
    std::uint64_t quiet = 0;
    std::uint64_t lastTriggers = app.assetWatchTriggerCount();
    for (int i = 0; i < maxTicks; ++i) {
        const std::uint64_t sweepsBefore = app.assetWatchSweepCount();
        if (!app.tick()) {
            return false;
        }
        if (app.assetWatchSweepCount() == sweepsBefore) {
            continue;  // mid-sweep or in cooldown
        }
        if (app.assetWatchTriggerCount() != lastTriggers) {
            lastTriggers = app.assetWatchTriggerCount();
            quiet = 0;
            continue;
        }
        if (++quiet >= quietSweeps) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("editor: a created file is reflected with no requestAssetRescan() call (task 3.1.4, I44/AC-1)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "watcher i44", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/first.txt", "one").empty());

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx,
        {.persistLayout = false,
         .unfocusedFrameCapHz = 0.0F,
         .projectPath = created.root,
         .restoreLastProject = false,
         .recentProjectsPath = uniqueRecentsFile(),
         .assetWatch = {.enabled = true, .dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0}});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);

    REQUIRE(tickToQuiescence(*app));
    const std::uint64_t baseTriggers = app->assetWatchTriggerCount();
    const std::size_t baseCount = app->assetCount();

    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/second.txt", "two").empty());
    // A deviation from the plan's own literal prediction, found by running this exact case: a BRAND
    // NEW file (one with no prior .meta) produces TWO triggers, not one -- the SAME mechanism §A-1
    // documents for the first scan of a fresh project, but not limited to it. Sweep N detects
    // second.txt itself (settled immediately, settleMs == 0) and fires; the rescan that fire drives
    // writes second.txt.meta; sweep N+1 then detects THAT sidecar as a fresh Added and fires again.
    // Reaching quiescence a SECOND time, rather than a fixed tick count, is what makes the assertion
    // honest regardless of exactly how many sweeps the two fires land on.
    REQUIRE(tickToQuiescence(*app));

    CHECK(app->assetCount() == baseCount + 1);
    CHECK(app->assetWatchTriggerCount() == baseTriggers + 2);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a changed file gets a new ContentHash and SourceChanged (task 3.1.4, I45/AC-2)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "watcher i45", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/first.txt", "one").empty());

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx,
        {.persistLayout = false,
         .unfocusedFrameCapHz = 0.0F,
         .projectPath = created.root,
         .restoreLastProject = false,
         .recentProjectsPath = uniqueRecentsFile(),
         .assetWatch = {.enabled = true, .dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0}});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);

    REQUIRE(tickToQuiescence(*app));
    const std::optional<engine::ContentHash> baseHash = app->assetContentHashForPath("first.txt");
    REQUIRE(baseHash.has_value());

    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/first.txt", "one, but genuinely different now")
                .empty());
    REQUIRE(tickSweeps(*app, 4));

    const std::optional<engine::ContentHash> newHash = app->assetContentHashForPath("first.txt");
    REQUIRE(newHash.has_value());
    CHECK_FALSE(*newHash == *baseHash);
    CHECK(app->assetImportChangeForPath("first.txt") == engine::editor::ImportChange::SourceChanged);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a deleted file leaves the database with no user action (task 3.1.4, I46/AC-3)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "watcher i46", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/first.txt", "one").empty());
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/second.txt", "two").empty());

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx,
        {.persistLayout = false,
         .unfocusedFrameCapHz = 0.0F,
         .projectPath = created.root,
         .restoreLastProject = false,
         .recentProjectsPath = uniqueRecentsFile(),
         .assetWatch = {.enabled = true, .dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0}});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);

    REQUIRE(tickToQuiescence(*app));
    const std::size_t baseCount = app->assetCount();

    // R9: the stream writeTextFileAtomic used is already closed by the time this call happens --
    // there is no open handle on this file anywhere in this test.
    std::error_code removeEc;
    std::filesystem::remove(std::filesystem::path(created.root + "/assets/first.txt"), removeEc);
    REQUIRE_FALSE(removeEc);
    REQUIRE(tickSweeps(*app, 3));

    CHECK(app->assetCount() == baseCount - 1);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the steady state is silent -- the loop detector (task 3.1.4, I47/AC-19)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "watcher i47", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/first.txt", "one").empty());

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx,
        {.persistLayout = false,
         .unfocusedFrameCapHz = 0.0F,
         .projectPath = created.root,
         .restoreLastProject = false,
         .recentProjectsPath = uniqueRecentsFile(),
         .assetWatch = {.enabled = true, .dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0}});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);

    REQUIRE(tickToQuiescence(*app));
    const std::uint64_t baseTriggers = app->assetWatchTriggerCount();
    const std::uint64_t baseSweeps = app->assetWatchSweepCount();

    REQUIRE(tickSweeps(*app, 6));  // changing NOTHING

    CHECK(app->assetWatchTriggerCount() == baseTriggers);  // zero further triggers
    CHECK(app->assetWatchSweepCount() >= baseSweeps + 6);  // the sweeps really ran

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: enabled == false stops detection; re-enabling resumes it (task 3.1.4, I48/AC-28/AC-35/AC-38)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "watcher i48", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/first.txt", "one").empty());

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx,
        {.persistLayout = false,
         .unfocusedFrameCapHz = 0.0F,
         .projectPath = created.root,
         .restoreLastProject = false,
         .recentProjectsPath = uniqueRecentsFile(),
         .assetWatch = {.enabled = true, .dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0}});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);

    REQUIRE(tickToQuiescence(*app));
    const std::size_t baseCount = app->assetCount();
    const std::uint64_t sweepsWhileEnabled = app->assetWatchSweepCount();

    app->requestAssetWatchToggle(false);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/third.txt", "three").empty());
    // tickSweeps cannot advance while disabled (the target would never be reached) -- tick a FIXED
    // number of frames instead.
    for (int i = 0; i < 40; ++i) {
        REQUIRE(app->tick());
    }
    CHECK_FALSE(app->assetWatchEnabled());
    CHECK(app->assetCount() == baseCount);
    CHECK(app->assetWatchSweepCount() == sweepsWhileEnabled);

    app->requestAssetWatchToggle(true);
    REQUIRE(tickSweeps(*app, 3));
    CHECK(app->assetWatchEnabled());
    CHECK(app->assetCount() > baseCount);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: a superseded thumbnail is destroyed and forgotten -- resident count stays 1 "
    "(task 3.1.4, I49/AC-31)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    // A LARGER window than most of this file's GPU cases (I43's own precedent, code-review
    // BLOCKING-1): at 320x180 the Bottom dock slot settles to a sliver too short to show even one
    // row after the first frame, so ImGuiListClipper legitimately reports zero visible rows and
    // drawTile() -- and therefore the SECOND decode this case must observe -- is never called again,
    // silently. Confirmed directly: at 320x180 the tile draws only on the dockspace's very first
    // frame; at 1280x800 it draws every tick.
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "watcher i49", .width = 1280, .height = 800});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string assetsRoot = created.root + "/assets";
    REQUIRE(writeBinaryFixture(assetsRoot + "/tex.png", TINY_PNG_RED.data(), TINY_PNG_RED.size()).empty());

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx,
        {.persistLayout = false,
         .unfocusedFrameCapHz = 0.0F,
         .projectPath = created.root,
         .restoreLastProject = false,
         .recentProjectsPath = uniqueRecentsFile(),
         .assetWatch = {.enabled = true, .dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0}});
    REQUIRE(app.has_value());
    // "Assets" shares its dock slot with "Console" (D3); Console registers first and would otherwise
    // win every tick's tab, so drawTile() -- and therefore the thumbnail this case observes -- never
    // runs at all (2.2.4's C5).
    app->panels().setVisible("Console", false);

    REQUIRE(tickToQuiescence(*app));

    bool ready = false;
    for (int i = 0; i < 60 && !ready; ++i) {
        REQUIRE(app->tick());
        ready = app->thumbnailReadyCount() == 1;
    }
    REQUIRE(ready);
    REQUIRE(app->thumbnailResidentCount() == 1);

    REQUIRE(writeBinaryFixture(assetsRoot + "/tex.png", TINY_PNG_GREEN.data(), TINY_PNG_GREEN.size()).empty());
    REQUIRE(tickSweeps(*app, 4));

    ready = false;
    for (int i = 0; i < 60 && !ready; ++i) {
        REQUIRE(app->tick());
        ready = app->thumbnailReadyCount() == 1;
    }
    REQUIRE(ready);

    CHECK(app->thumbnailResidentCount() == 1);  // NOT 2 -- the old {guid, hash} texture was released

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a manual rescan is not re-reported by the watcher (task 3.1.4, I50/AC-27/AC-30)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "watcher i50", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/first.txt", "one").empty());

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx,
        {.persistLayout = false,
         .unfocusedFrameCapHz = 0.0F,
         .projectPath = created.root,
         .restoreLastProject = false,
         .recentProjectsPath = uniqueRecentsFile(),
         .assetWatch = {.enabled = true, .dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0, .maxDeferredSweeps = 3}});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);

    REQUIRE(tickToQuiescence(*app));

    // A deviation from the plan's own literal sequencing, found by running this exact case: with
    // dirsPerPoll == 64 / cooldownMs == 0, AssetWatcher::poll() runs FIRST in every tick's reconcile
    // and therefore ALWAYS sees a file written just before that tick, before any manual-rescan
    // request is even inspected -- a manual rescan against an ALREADY-quiescent tree can never
    // discriminate noteExternalScan()'s call site at all: `committed` and `lastProbe` are already
    // identical once quiescent, so adopting one into the other is an observable no-op either way.
    //
    // The genuine divergence needs a DEFERRED sweep (AC-11/D3): `lastProbe` is updated on EVERY
    // completed sweep, deferred or not, while `committed` is updated ONLY on a fire. A settled
    // addition (fourth.txt) alongside a PERMANENTLY unsettled one (poison.txt, forward-dated into the
    // future -- AW38's own trick) keeps the whole batch deferring, so `lastProbe` sees fourth.txt long
    // before `committed` ever would. A manual rescan issued WHILE deferred (watchFired == false, since
    // finishSweep never fires on a deferred sweep) is the one tick where the two genuinely differ on a
    // settled change. (Sabotage finding, recorded at the assertion below rather than here: even THIS
    // scenario does not end up discriminating the call site through EditorApp's black-box surface.)
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/fourth.txt", "four").empty());
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/poison.txt", "poison").empty());
    {
        std::error_code ec;
        const auto future = std::filesystem::file_time_type::clock::now() + std::chrono::hours(1);
        std::filesystem::last_write_time(std::filesystem::path(created.root + "/assets/poison.txt"), future, ec);
        REQUIRE_FALSE(ec);
    }

    REQUIRE(tickSweeps(*app, 1));  // the first deferred sweep -- lastProbe now has fourth.txt
    const std::uint64_t triggersBeforeManual = app->assetWatchTriggerCount();

    app->requestAssetRescan();  // the manual channel, issued WHILE the watcher is still deferring
    REQUIRE(app->tick());
    CHECK(app->assetCount() > 1);  // the manual rescan saw fourth.txt (and poison.txt) immediately
    CHECK(app->assetWatchTriggerCount() == triggersBeforeManual);
    // Only observable because invalidateListings()/setScanReport ran (AC-30's observable half).
    CHECK(app->assetOrphanCount() == 0);

    // The manual rescan's OWN write of two fresh .meta sidecars (fourth.txt.meta, poison.txt.meta) is
    // ITSELF legitimately new information (§A-1's echo, a second application) -- the watcher's very
    // next sweep settles and reports BOTH in ONE bundled trigger, so quiescence after the manual
    // rescan is reached at EXACTLY triggersBeforeManual + 1.
    //
    // STATED HONESTLY, confirmed by direct sabotage (seed S16, deleting the noteExternalScan() call
    // in editor_app.cpp's reconcile): this delta assertion does NOT discriminate that call site.
    // Without the call, `committed` stays without fourth.txt too, poison.txt's permanent
    // unsettledness eventually forces, and the SAME sweep that would have reported the two sidecars
    // ALSO re-reports fourth.txt -- but bundled into that SAME one trigger (three items instead of
    // two), landing at the IDENTICAL triggersBeforeManual + 1 this assertion checks, only delayed by a
    // few extra deferred sweeps and carrying a duplicate entry no accessor here can see. EditorApp
    // exposes trigger/sweep/entry COUNTS, never diff CONTENT, so no assertion reachable through this
    // black-box surface can tell "one legitimate bundled trigger" from "one bundled trigger that also
    // silently repeats an already-known path" -- AW42 cannot either, for the identical reason the plan
    // itself predicted (it drives the method directly, with nothing to diverge from).
    //
    // CORRECTED by the code-review round: the conclusion originally recorded here -- that closing this
    // needs a NEW EditorApp seam surfacing lastDiff() content -- was WRONG. The gap is closable through
    // the existing accessors; what defeated both attempts was this scenario's POISON FILE, not the
    // accessor surface. See I51 immediately below, which discriminates the call site with a
    // modification (no sidecar echo to hide a duplicate inside) and a sweep that provably cannot
    // complete on the manual-rescan tick. THIS case's assertion is still correct and still worth
    // keeping -- it just is not the one that covers the call site.
    REQUIRE(tickToQuiescence(*app));
    CHECK(app->assetWatchTriggerCount() == triggersBeforeManual + 1);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// I51 -- noteExternalScan()'s CALL SITE, discriminated. Added by the code-review round, which
// disagreed with I50's "needs a new EditorApp seam" conclusion and was right: the gap is closable
// through the existing black-box surface. I50's two designs both failed for one shared reason -- both
// used a PERMANENTLY UNSETTLED poison file, so the eventual forced fire produces a trigger whether or
// not the call ran, and the re-reported path merely rides along inside it. That collapses the signal
// from "0 vs 1 trigger" to "2 vs 3 items inside one trigger", which no count can see.
//
// Two changes make it discriminate:
//   * a MODIFICATION, not an addition. D6 says a valid .meta is never rewritten, and the content hash
//     lives in Library/asset-cache.json (excluded, D4), so a modification produces NO sidecar echo --
//     there is no other candidate change in the tree for a duplicate to hide inside.
//   * dirsPerPoll == 1 over a TWO-directory tree, so the tick carrying the manual rescan provably
//     cannot complete a sweep. That is what forces watchFired == false, which is the only branch that
//     reaches noteExternalScan() at all.
//
// WITH the call:    committed = lastProbe (new stamp) -> the next sweep sees nothing -> delta 0.
// WITHOUT the call: committed keeps the OLD stamp while lastProbe holds the new one -> that same
//                   sweep finds a STABLE Modified, settles it, and fires -> delta 1.
TEST_CASE("imgui_layer: a manual rescan while deferring is not re-reported (I51, AC-27)") {
    engine::platform::Context ctx;
    REQUIRE(ctx.valid());
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "watcher i51", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string target = created.root + "/assets/first.txt";
    REQUIRE(engine::editor::writeTextFileAtomic(target, "one").empty());
    {
        // A second directory, so dirsPerPoll == 1 guarantees a sweep spans at least two ticks.
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(created.root + "/assets/sub"), ec);
        REQUIRE_FALSE(ec);
    }

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx,
        {.persistLayout = false,
         .unfocusedFrameCapHz = 0.0F,
         .projectPath = created.root,
         .restoreLastProject = false,
         .recentProjectsPath = uniqueRecentsFile(),
         .assetWatch = {.enabled = true, .dirsPerPoll = 1, .cooldownMs = 0, .settleMs = 0}});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);

    REQUIRE(tickToQuiescence(*app));

    // Toggling around the write removes the only nondeterminism: a sweep already in flight would
    // otherwise observe the new bytes at an unpredictable point in its cursor walk. setEnabled(false)
    // abandons that sweep and leaves committed/lastProbe untouched; setEnabled(true) zeroes the
    // cooldown so the next poll starts a clean sweep.
    app->requestAssetWatchToggle(false);
    REQUIRE(engine::editor::writeTextFileAtomic(target, "one, but genuinely longer now").empty());
    app->requestAssetWatchToggle(true);
    REQUIRE(app->tick());  // drain the toggle request through the reconcile block

    // This sweep MUST defer: the probe carries the new stamp, lastProbe still the old one, so
    // settlement condition 1 (stability against the PREVIOUS COMPLETED SWEEP) refuses it.
    REQUIRE(tickSweeps(*app, 1));
    const std::uint64_t before = app->assetWatchTriggerCount();

    app->requestAssetRescan();
    REQUIRE(app->tick());  // one dir of two -> no sweep completes -> watchFired == false

    REQUIRE(tickToQuiescence(*app));
    CHECK(app->assetWatchTriggerCount() == before);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- I52-I60: task 3.2.1's on-demand model import panel, through real frames ----------------------
//
// The SAME two-part discipline every project-opening case in this file uses (BLOCKING-2): opt OUT of
// restoring the last project AND redirect the recents-file WRITE.
//
// None of I53-I55/I59 needs `requestPanelFocus`/`setVisible` at all: modelImportCount()/
// modelImportState()/modelImportTarget() read ModelImportSession directly, and the service() call
// (editor_app.cpp's post-draw slot) runs unconditionally every tick regardless of whether "Import
// Details" is the currently-selected tab in its shared DockSlot::Right node. Only I56-I58, which must
// observe the PANEL ITSELF draw without an ImGui assert, need the I39 precedent: "Import Details"
// shares DockSlot::Right with "Inspector" (registered first, so it wins the tab on a fresh layout),
// and drawPanels() calls onDraw() only for a panel whose ImGui::Begin() returns true -- so a
// tabbed-behind panel never draws at all without requestPanelFocus() bringing it forward.
namespace {

// A minimal, structurally empty but VALID glTF document (MS2's own fixture, reused): zero nodes, zero
// meshes -- imports with ImportStatus::Ok, which is all I53/I54/I55/I59 need to observe SessionState::
// Imported and a real, non-zero importCount().
constexpr std::string_view MINIMAL_GLTF_TEXT = R"({"asset":{"version":"2.0"}})";

// A depth-4 node CHAIN (Root -> Child -> Grandchild -> GreatGrandchild), no meshes -- what I57 needs to
// exercise the Hierarchy section's explicit-stack tree walk through a real drawn frame.
constexpr std::string_view HIERARCHY_GLTF_TEXT = R"({"asset":{"version":"2.0"},"nodes":[)"
                                                 R"({"name":"Root","children":[1]},)"
                                                 R"({"name":"Child","children":[2]},)"
                                                 R"({"name":"Grandchild","children":[3]},)"
                                                 R"({"name":"GreatGrandchild"}]})";

// Not JSON at all -- the MI34 shape ("truncated / not JSON / bad GLB" -> ParseFailed), reused here only
// to reach SessionState::Failed through a real frame; I58 does not care WHICH failure status lands.
constexpr std::string_view DAMAGED_GLTF_TEXT = "this is not a json document at all";

// ---- task 3.2.2: ASCII FBX fixtures for I62-I67, the §D-7/§G-10 template's shape (VERIFIED TO PARSE,
// fbx_import_test.cpp's own spike) -- a FOURTH independent copy of the template (fbx_import_test.cpp,
// asset_database_test.cpp and model_import_session_test.cpp each already keep their own), matching
// this file's own "each TU keeps its own" precedent. Deliberately NO `Creator` line: metadata.exporter
// stays UNKNOWN and metadata.creator stays empty, so SourceSpace::generator is EMPTY (A21) -- exactly
// what I67 needs to prove the panel renders no empty parenthetical.
constexpr std::string_view MINIMAL_FBX_TEXT =
    "; FBX 7.4.0 project file\n"
    "FBXHeaderExtension:  {\n"
    "    FBXHeaderVersion: 1003\n"
    "    FBXVersion: 7400\n"
    "}\n"
    "GlobalSettings:  {\n"
    "    Version: 1000\n"
    "    Properties70:  {\n"
    "        P: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
    "        P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
    "        P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",100\n"
    "    }\n"
    "}\n"
    "Objects:  {\n"
    "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
    "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
    "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
    "        GeometryVersion: 124\n"
    "    }\n"
    "    Model: 100, \"Model::box\", \"Mesh\" { Version: 232 }\n"
    "}\n"
    "Connections:  {\n"
    "    C: \"OO\",100,0\n"
    "    C: \"OO\",200,100\n"
    "}\n";

// A depth-4 authored chain (Root -> Child -> Grandchild -> GreatGrandchild), the HIERARCHY_GLTF_TEXT
// shape above -- but the LEAF carries a GeometricTranslation, which fbx_import.cpp's phase 3 (D7/D8,
// HELPER_NODES handling) turns into an additional "<geometry helper>" child node (FI23's own fixture
// shape). What I64 needs: a real Hierarchy section walk over BOTH an ordinary chain AND a helper node,
// whose name contains '<'/'>' -- characters ImGui treats literally in TextUnformatted but not in a
// format string (§D-8's "a dynamic string is never a format argument" rule).
constexpr std::string_view HIERARCHY_FBX_TEXT =
    "; FBX 7.4.0 project file\n"
    "FBXHeaderExtension:  {\n"
    "    FBXHeaderVersion: 1003\n"
    "    FBXVersion: 7400\n"
    "}\n"
    "GlobalSettings:  {\n"
    "    Version: 1000\n"
    "    Properties70:  {\n"
    "        P: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
    "        P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
    "        P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",100\n"
    "    }\n"
    "}\n"
    "Objects:  {\n"
    "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
    "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
    "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
    "        GeometryVersion: 124\n"
    "    }\n"
    "    Model: 100, \"Model::Root\", \"Null\" { Version: 232 }\n"
    "    Model: 101, \"Model::Child\", \"Null\" { Version: 232 }\n"
    "    Model: 102, \"Model::Grandchild\", \"Null\" { Version: 232 }\n"
    "    Model: 103, \"Model::GreatGrandchild\", \"Mesh\" {\n"
    "        Version: 232\n"
    "        Properties70:  {\n"
    "            P: \"GeometricTranslation\", \"Vector3D\", \"Vector\", \"\",1,0,0\n"
    "        }\n"
    "    }\n"
    "}\n"
    "Connections:  {\n"
    "    C: \"OO\",100,0\n"
    "    C: \"OO\",101,100\n"
    "    C: \"OO\",102,101\n"
    "    C: \"OO\",103,102\n"
    "    C: \"OO\",200,103\n"
    "}\n";

// PNG magic bytes -- fbx_import_test.cpp's own FI6 confirms this maps to ImportStatus::ParseFailed.
constexpr std::string_view DAMAGED_FBX_TEXT = "\x89PNG\r\n\x1a\n";

// A non-finite UnitScaleFactor -- fbx_import_test.cpp's own FI10 confirms this maps to
// ImportStatus::Malformed (a NaN geometry_scale multiplies through every position, and Aabb::expand()
// silently ignores a non-finite point, so summary.bounds never leaves Aabb::empty() while
// summary.vertexCount is real -- E5's own "internally inconsistent" trigger).
constexpr std::string_view MALFORMED_FBX_TEXT =
    "; FBX 7.4.0 project file\n"
    "FBXHeaderExtension:  {\n"
    "    FBXHeaderVersion: 1003\n"
    "    FBXVersion: 7400\n"
    "}\n"
    "GlobalSettings:  {\n"
    "    Version: 1000\n"
    "    Properties70:  {\n"
    "        P: \"UpAxis\", \"int\", \"Integer\", \"\",2\n"
    "        P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"FrontAxis\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",-1\n"
    "        P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
    "        P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "        P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",nan\n"
    "    }\n"
    "}\n"
    "Objects:  {\n"
    "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
    "        Vertices: *12 { a: 0,0,0,100,0,0,100,100,0,0,100,0 }\n"
    "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
    "        GeometryVersion: 124\n"
    "    }\n"
    "    Model: 100, \"Model::box\", \"Mesh\" { Version: 232 }\n"
    "}\n"
    "Connections:  {\n"
    "    C: \"OO\",100,0\n"
    "    C: \"OO\",200,100\n"
    "}\n";

// A chain of 257 nested Model blocks (depth 1..257), exceeding MAX_FBX_NODE_DEPTH (256) --
// fbx_import_test.cpp's own FI27 confirms this maps to ImportStatus::Truncated. Built programmatically
// like FI27's own fixture: 257 hand-written blocks would defeat the point, which is the CAP, not the
// specific hierarchy shape.
[[nodiscard]] std::string truncatedFbxText() {
    std::string objects;
    std::string connections;
    constexpr int DEPTH = 257;
    for (int i = 0; i < DEPTH; ++i) {
        objects += std::format("    Model: {}, \"Model::n{}\", \"Null\" {{ Version: 232 }}\n", 100 + i, i);
        const int parent = (i == 0) ? 0 : (100 + i - 1);
        connections += std::format("    C: \"OO\",{},{}\n", 100 + i, parent);
    }
    return std::format(
        "; FBX 7.4.0 project file\n"
        "FBXHeaderExtension:  {{\n"
        "    FBXHeaderVersion: 1003\n"
        "    FBXVersion: 7400\n"
        "}}\n"
        "GlobalSettings:  {{\n"
        "    Version: 1000\n"
        "    Properties70:  {{\n"
        "        P: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
        "        P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
        "        P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "        P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",100\n"
        "    }}\n"
        "}}\n"
        "Objects:  {{\n"
        "{}"
        "}}\n"
        "Connections:  {{\n"
        "{}"
        "}}\n",
        objects, connections);
}

// task 3.2.3: a one-triangle .obj naming its own .mtl, and that .mtl's own text -- a FIFTH independent
// copy of "each TU keeps its own fixture" (fbx_import_test.cpp, asset_database_test.cpp,
// model_import_session_test.cpp and this file's own FBX section each already do). What I68 needs: BOTH
// a Materials-section-populated .obj selection AND a .mtl selected standalone, each through a real
// drawn frame.
constexpr std::string_view MINIMAL_OBJ_TEXT =
    "mtllib obj_fixture.mtl\nusemtl Wood\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
constexpr std::string_view MINIMAL_MTL_TEXT = "newmtl Wood\nKd 0.5 0.3 0.1\n";

}  // namespace

TEST_CASE("editor: the Import Details panel is registered right of the Inspector (task 3.2.1, I52, AC-50)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i52", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
    REQUIRE(app.has_value());
    // AC-50: registered in create(), BEFORE the first tick() -- checked here, before any tick runs.
    CHECK(app->panels().count() == 8);

    const engine::editor::Panel* panel = app->panels().find("Import Details");
    REQUIRE(panel != nullptr);
    CHECK(std::string_view(panel->id()) == "Import Details");
    CHECK(panel->defaultDockSlot() == engine::editor::DockSlot::Right);

    // The black-box surface's own default state, before any selection has ever happened.
    CHECK(app->modelImportCount() == 0);
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Idle));
    CHECK(app->modelImportTarget().empty());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: selecting a model imports it exactly once (task 3.2.1, I53, AC-45)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i53", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/a.gltf", MINIMAL_GLTF_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    // "Assets" shares DockSlot::Bottom with "Console" (registered first, so it wins the tab by
    // default) -- without hiding Console, AssetBrowserPanel::onDraw() never runs, applyPending() never
    // drains the SelectEntry action below, and selection() never changes (2.2.4's C5 precedent).
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());  // 1: the initial scan -- selection() is still "" through this whole tick
    CHECK(app->modelImportCount() == 0);

    app->requestAssetBrowserSelectEntry("a.gltf");
    REQUIRE(app->tick());  // 2: drains SelectEntry -> AssetBrowserPanel::selection() == "a.gltf" by the
                           // end of THIS tick's onDraw (applyPending runs last)
    CHECK(app->presentedLastFrame());
    REQUIRE(app->tick());  // 3: the reconcile's fifth statement now sees the new selection() ->
                           // setTarget() -> the post-draw slot's service() call imports it
    CHECK(app->presentedLastFrame());
    CHECK(app->modelImportCount() == 1);
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));

    for (int i = 0; i < 10; ++i) {
        REQUIRE(app->tick());
    }
    CHECK(app->modelImportCount() == 1);  // AC-45: STRUCTURAL (service()'s own `serviced` guard), not a
                                          // call-site convention -- ten further ticks cost ten early
                                          // returns, exactly ModelImportSession::MS3's tier-0 proof.

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a rescan invalidates the cached result and re-imports exactly once (task 3.2.1, I54, AC-47)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i54", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/a.gltf", MINIMAL_GLTF_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);  // 2.2.4's C5 -- see I53's own comment
    REQUIRE(app->tick());                        // 1: the initial scan

    app->requestAssetBrowserSelectEntry("a.gltf");
    REQUIRE(app->tick());  // 2: drains SelectEntry
    REQUIRE(app->tick());  // 3: reconcile -> setTarget -> service() imports once
    REQUIRE(app->modelImportCount() == 1);

    // AssetDatabase::generation() is the FOURTH consumer here (2.6.1's panel root, 3.1.1's database,
    // 3.1.3's report, 3.1.4's watcher, now the import session) -- ANY rescan trigger invalidates the
    // cached result, not only a change to the model itself.
    app->requestAssetRescan();
    REQUIRE(app->tick());  // 4: rescan runs (bumps generation()) -> reconcile sees the mismatch ->
                           // setTarget() resets `serviced` -> service() re-imports, ALL in this ONE tick
    CHECK(app->modelImportCount() == 2);

    REQUIRE(app->tick());  // a further tick at the SAME generation costs nothing more (AC-45's rule,
                           // restated for AC-47's own trigger)
    CHECK(app->modelImportCount() == 2);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: a non-model selection never imports; clearing the selection returns to Idle (task 3.2.1, "
    "I55, AC-46)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i55", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/notes.txt", "hello").empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);  // 2.2.4's C5 -- see I53's own comment
    REQUIRE(app->tick());                        // 1: the initial scan

    app->requestAssetBrowserSelectEntry("notes.txt");
    REQUIRE(app->tick());  // 2: drains SelectEntry
    REQUIRE(app->tick());  // 3: reconcile -> setTarget -> service() -- NotImportable, NOTHING read
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::NotImportable));
    CHECK(app->modelImportCount() == 0);  // AC-46: NotImportable never increments importCount()

    app->requestAssetBrowserSelectEntry("");
    REQUIRE(app->tick());  // 4: drains SelectEntry("") -- exactly what Navigate's own clear does
    REQUIRE(app->tick());  // 5: reconcile -> setTarget("", gen) -> service() -- Idle
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Idle));
    CHECK(app->modelImportCount() == 0);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the Import Details panel draws its Idle state without crashing (task 3.2.1, I56, AC-49)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i56", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
    REQUIRE(app.has_value());

    REQUIRE(app->tick());  // 1
    CHECK(app->presentedLastFrame());
    REQUIRE(app->tick());  // 2: let the default dock layout settle before focusing anything (I39's precedent)
    CHECK(app->presentedLastFrame());
    app->requestPanelFocus("Import Details");
    REQUIRE(app->tick());  // 3: focus applied before DockSpaceOverViewport, so it lands this frame --
                           // "Import Details" draws its Idle state for real, tabbed in front of Inspector
    CHECK(app->presentedLastFrame());
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Idle));

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: the Import Details panel draws its Imported state with a four-deep hierarchy (task 3.2.1, "
    "I57, AC-49)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i57", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/chain.gltf", HIERARCHY_GLTF_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);  // 2.2.4's C5 -- see I53's own comment
    REQUIRE(app->tick());                        // 1: the initial scan

    app->requestAssetBrowserSelectEntry("chain.gltf");
    REQUIRE(app->tick());  // 2: drains SelectEntry
    REQUIRE(app->tick());  // 3: reconcile -> setTarget -> service() imports the four-deep chain
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));
    CHECK(app->modelImportCount() == 1);

    REQUIRE(app->tick());  // 4: let the default dock layout settle before focusing anything
    CHECK(app->presentedLastFrame());
    app->requestPanelFocus("Import Details");
    REQUIRE(app->tick());  // 5: focus applied -> draws the Imported state for real, including the
                           // Overview/Import Settings/Hierarchy/Meshes/Materials/Skeleton & Animation
                           // sections (ALL default-open) over the four-deep node chain
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the Import Details panel draws its Failed state without crashing (task 3.2.1, I58, AC-49)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i58", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/broken.gltf", DAMAGED_GLTF_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);  // 2.2.4's C5 -- see I53's own comment
    REQUIRE(app->tick());                        // 1: the initial scan

    app->requestAssetBrowserSelectEntry("broken.gltf");
    REQUIRE(app->tick());  // 2: drains SelectEntry
    REQUIRE(app->tick());  // 3: reconcile -> setTarget -> service() -- ParseFailed
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Failed));

    REQUIRE(app->tick());  // 4: let the default dock layout settle before focusing anything
    CHECK(app->presentedLastFrame());
    app->requestPanelFocus("Import Details");
    REQUIRE(app->tick());  // 5: focus applied -> draws the Failed state for real
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: the reconcile's fifth statement keeps the session's target in sync with the selection "
    "(task 3.2.1, I59, seed S19)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i59", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/a.gltf", MINIMAL_GLTF_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);  // 2.2.4's C5 -- see I53's own comment
    CHECK(app->modelImportTarget().empty());
    REQUIRE(app->tick());  // 1: the initial scan

    app->requestAssetBrowserSelectEntry("a.gltf");
    REQUIRE(app->tick());                     // 2: drains SelectEntry -> AssetBrowserPanel::selection() == "a.gltf"
    CHECK(app->modelImportTarget().empty());  // the reconcile has not run against the NEW selection yet
    REQUIRE(app->tick());                     // 3: the reconcile's fifth statement compares selection() against the
                           // session's OWN target and calls setTarget() on the mismatch -- a seed that
                           // removes this statement (S19) leaves modelImportTarget() stuck at ""
    CHECK(app->modelImportTarget() == "a.gltf");
    CHECK(app->modelImportCount() == 1);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- I60: AC-48's call-site proof, the I30/I34/I42 shape's fourth instance -------------------------
//
// This target is ImGui-free at source and cannot observe WHERE a call sits at runtime -- INV-M12's
// general-case violation (moving importSession.service() into onDraw()) has NO automated tier that can
// see it (3.1.3's BLOCKING-1 and 3.1.4's D9 are the identical shape). The mechanical proof available is
// textual: importSession.service( appears EXACTLY ONCE in editor_app.cpp, and it sits textually AFTER
// drawShellUi( -- the ONE call that invokes every panel's onDraw() -- so it runs OUTSIDE the draw walk
// by construction. This is the I30/I42 statement-ordering proof (a drain happens before a combine),
// applied to prove a call sits after a walk instead.
TEST_CASE("editor_app: importSession.service() runs outside the draw walk (task 3.2.1, I60, AC-48, INV-M12)") {
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

    std::size_t drawShellUiLine = lines.size();
    std::size_t serviceLine = lines.size();
    std::size_t serviceHits = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        // Comment-stripped BEFORE matching (I42's lesson): this task's own prose legitimately names
        // both tokens inside comments more than once, and a check that cannot tell code from comment
        // is not a mechanical proof at all.
        const std::string_view line = lines[i];
        const std::size_t commentStart = line.find("//");
        const std::string_view code = commentStart == std::string_view::npos ? line : line.substr(0, commentStart);
        if (code.find("drawShellUi(") != std::string_view::npos && drawShellUiLine == lines.size()) {
            drawShellUiLine = i;  // the ONE call that invokes every panel's onDraw()
        }
        if (code.find("importSession.service(") != std::string_view::npos) {
            ++serviceHits;
            if (serviceLine == lines.size()) {
                serviceLine = i;  // FIRST occurrence -- and, per the REQUIRE below, the ONLY one
            }
        }
    }
    REQUIRE(drawShellUiLine != lines.size());
    REQUIRE(serviceLine != lines.size());
    REQUIRE(serviceHits == 1);             // exactly ONE call site in this file (§V6's AC-48 grep, scoped here)
    CHECK(drawShellUiLine < serviceLine);  // textually AFTER drawShellUi(: runs OUTSIDE the draw walk
}

// I61's OWN caveat, stated plainly (verified directly, matching 3.1.3's BLOCKING-1 / 3.1.4's D9
// posture): the SHOULD-FIX 10 bug is a SILENT omission, never a crash -- excluding importFailureTotal
// from drawIssues()'s `total` merely made the whole Issues header (and this new sixth category) never
// draw for a project in exactly the state built below, and ImGui's own CollapsingHeader/TextUnformatted
// calls simply not running has no effect this test tier can observe (presentedLastFrame() stays true
// whether the header opened or not; no automated tier in this tree scrapes rendered ImGui text). Checked
// directly: this case passes with drawIssues()'s importFailureTotal fix reverted, identically. What it
// DOES prove, and is worth proving on its own: report.importFailureTotal reaches EditorApp's new
// black-box accessor correctly, and the draw path this fix adds executes without crashing or
// unbalancing an ImGui call. The rendering fix itself is verified by code inspection against the five
// existing categories' identical idiom, not by this case.
TEST_CASE(
    "editor: a scan-time model import failure reaches assetImportFailureCount() and the Issues panel "
    "draws without crashing (task 3.2.1, I61, code review SHOULD-FIX 10)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import failure issues i61", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    // The ONLY issue this project has -- no orphan, no invalid .meta, no hash/write failure -- so
    // drawIssues()'s `total` is driven ENTIRELY by report.importFailureTotal here. BEFORE this fix,
    // excluding it from `total` meant the header never even opened for a project in exactly this state.
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/broken.gltf", DAMAGED_GLTF_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    REQUIRE(app->tick());  // 1: the initial scan -- phase 7.5 probes "broken.gltf" and fails to parse it
    CHECK(app->assetImportFailureCount() > 0);
    CHECK(app->assetOrphanCount() == 0);  // confirms importFailureTotal is the ONLY populated category

    REQUIRE(app->tick());  // 2: let the default dock layout settle before focusing anything
    CHECK(app->presentedLastFrame());
    app->requestPanelFocus("Assets");
    REQUIRE(app->tick());  // 3: focus applied before DockSpaceOverViewport, so it lands this frame --
                           // drawIssues() runs for real, past the total==0 guard this fix corrected
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- I62-I67: task 3.2.2's FBX arm through the SAME real-editor surface I52-I60 already proved for
// glTF -- the GPU tier, through real frames. Renumbered from I61: task 3.2.1's own post-merge
// code-review round already claimed I61 (`assetImportFailureCount()`/the Issues panel, SHOULD-FIX 10)
// -- measured via `git show HEAD:tests/editor/imgui_layer_test.cpp | grep -oE 'I[0-9]+' | sort -u`,
// never assumed, the identical lesson this task's own AD-i/MS blocks already log. --------------------

TEST_CASE(
    "editor: selecting an .fbx imports it exactly once; a rescan re-imports exactly once more (task 3.2.2, I62, "
    "AC-58)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i61", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/a.fbx", MINIMAL_FBX_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);  // 2.2.4's C5 -- see I53's own comment
    REQUIRE(app->tick());                        // 1: the initial scan
    CHECK(app->modelImportCount() == 0);

    app->requestAssetBrowserSelectEntry("a.fbx");
    REQUIRE(app->tick());  // 2: drains SelectEntry
    REQUIRE(app->tick());  // 3: reconcile -> setTarget -> service() imports it -- the D5 gate's ONLY
                           // pass for FBX
    CHECK(app->modelImportCount() == 1);
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));

    for (int i = 0; i < 10; ++i) {
        REQUIRE(app->tick());
    }
    CHECK(app->modelImportCount() == 1);  // AC-45's rule, restated for a second format

    app->requestAssetRescan();
    REQUIRE(app->tick());  // rescan runs -> generation() bumps -> re-import, all in this ONE tick
    CHECK(app->modelImportCount() == 2);
    REQUIRE(app->tick());
    CHECK(app->modelImportCount() == 2);  // a further tick at the SAME generation costs nothing more

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: selecting .fbx -> .gltf -> .fbx drives exactly THREE imports and never mixes results "
    "(task 3.2.2, I63, E21)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i62", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/a.fbx", MINIMAL_FBX_TEXT).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/b.gltf", MINIMAL_GLTF_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());  // 1: the initial scan

    app->requestAssetBrowserSelectEntry("a.fbx");
    REQUIRE(app->tick());  // 2: drains SelectEntry
    REQUIRE(app->tick());  // 3: imports #1
    CHECK(app->modelImportCount() == 1);
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));

    app->requestAssetBrowserSelectEntry("b.gltf");
    REQUIRE(app->tick());  // 4: drains SelectEntry
    REQUIRE(app->tick());  // 5: imports #2
    CHECK(app->modelImportCount() == 2);
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));
    CHECK(app->modelImportTarget() == "b.gltf");

    app->requestAssetBrowserSelectEntry("a.fbx");
    REQUIRE(app->tick());  // 6: drains SelectEntry
    REQUIRE(app->tick());  // 7: imports #3
    CHECK(app->modelImportCount() == 3);
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));
    CHECK(app->modelImportTarget() == "a.fbx");

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: the Import Details panel draws an Ok FBX result with a four-deep hierarchy containing a "
    "helper node (task 3.2.2, I64, AC-59)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i63", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/chain.fbx", HIERARCHY_FBX_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());  // 1: the initial scan

    app->requestAssetBrowserSelectEntry("chain.fbx");
    REQUIRE(app->tick());  // 2: drains SelectEntry
    REQUIRE(app->tick());  // 3: reconcile -> setTarget -> service() imports the chain + its helper node
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));
    CHECK(app->modelImportCount() == 1);

    REQUIRE(app->tick());  // 4: let the default dock layout settle before focusing anything
    CHECK(app->presentedLastFrame());
    app->requestPanelFocus("Import Details");
    REQUIRE(app->tick());  // 5: focus applied -> draws the Imported state for real, including a
                           // Hierarchy walk over a node whose name contains '<'/'>' (the helper node)
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: the Import Details panel draws a Truncated FBX result -- the message row plus a coherent "
    "smaller model (task 3.2.2, I65, AC-59)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i64", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/deep.fbx", truncatedFbxText()).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());  // 1: the initial scan

    app->requestAssetBrowserSelectEntry("deep.fbx");
    REQUIRE(app->tick());  // 2: drains SelectEntry
    REQUIRE(app->tick());  // 3: reconcile -> setTarget -> service() -- MAX_FBX_NODE_DEPTH exceeded
    // Truncated is a SessionState::Imported result (D-11's own gate is unconditional on Pass 2, and a
    // Truncated status still counts as "shown" -- ModelImportSession::service()'s own dichotomy), never
    // Failed -- a panel that only handles Ok/Failed would assert drawing this.
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));

    REQUIRE(app->tick());  // 4: let the default dock layout settle before focusing anything
    CHECK(app->presentedLastFrame());
    app->requestPanelFocus("Import Details");
    REQUIRE(app->tick());  // 5: focus applied -> draws the Truncated result for real, including the
                           // message row naming MAX_FBX_NODE_DEPTH
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: the Import Details panel draws every failure status an .fbx CAN produce without an ImGui "
    "assert -- ParseFailed and Malformed (task 3.2.2, I66, AC-59)") {
    // Unsupported is DELIBERATELY NOT exercised here: fbx_import_test.cpp's own FI7 asserts it
    // structurally (`CHECK(result.status != ImportStatus::Unsupported);`) -- the dispatch itself claims
    // every ".fbx"-suffixed name before importFbx() ever runs, so no ufbx error can ever produce it for
    // this extension. A deviation from the spec's own AC-59 wording (which names Unsupported as one of
    // the three), logged rather than silently narrowed.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i65", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/broken.fbx", DAMAGED_FBX_TEXT).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/bad-scale.fbx", MALFORMED_FBX_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());  // 1: the initial scan
    REQUIRE(app->tick());  // 2: let the default dock layout settle before focusing anything
    CHECK(app->presentedLastFrame());
    app->requestPanelFocus("Import Details");

    app->requestAssetBrowserSelectEntry("broken.fbx");
    REQUIRE(app->tick());  // 3: drains SelectEntry
    REQUIRE(app->tick());  // 4: reconcile -> setTarget -> service() -- ParseFailed -> draws Failed
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Failed));
    CHECK(app->presentedLastFrame());

    app->requestAssetBrowserSelectEntry("bad-scale.fbx");
    REQUIRE(app->tick());  // 5: drains SelectEntry
    REQUIRE(app->tick());  // 6: reconcile -> setTarget -> service() -- Malformed -> draws Failed
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Failed));
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: the Overview Source space row is present for an .fbx and absent for a .gltf; the Importer "
    "line names each importer; an empty generator renders no empty parenthetical (task 3.2.2, I67, "
    "AC-60, A21)") {
    // This target is ImGui-free at source and cannot read a drawn frame's text -- what IS provable is
    // that BOTH selections draw a real frame without an ImGui assert, exactly as I56-I58 already
    // established for the other sections; the row's PRESENCE/ABSENCE and the exact importer-line text
    // are proven at the pure-function level by A21/A2's own tier-0 cases (fbx_import_test.cpp's FI19,
    // model_import_test.cpp's MI106/MI107) and read directly here from the black-box surface this
    // target DOES expose.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i66", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/a.fbx", MINIMAL_FBX_TEXT).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/b.gltf", MINIMAL_GLTF_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());  // 1: the initial scan
    REQUIRE(app->tick());  // 2: let the default dock layout settle before focusing anything
    app->requestPanelFocus("Import Details");

    app->requestAssetBrowserSelectEntry("a.fbx");
    REQUIRE(app->tick());  // 3: drains SelectEntry
    REQUIRE(app->tick());  // 4: reconcile -> setTarget -> service() imports -- draws the Source space
                           // row, MINIMAL_FBX_TEXT's empty generator rendering no empty parenthetical
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));
    CHECK(app->presentedLastFrame());

    app->requestAssetBrowserSelectEntry("b.gltf");
    REQUIRE(app->tick());  // 5: drains SelectEntry
    REQUIRE(app->tick());  // 6: reconcile -> setTarget -> service() imports -- Source space row is
                           // ABSENT (sourceSpace.declared == false, AC-60's glTF half)
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: a real frame for a claimed .obj AND for its .mtl selected standalone -- all six sections "
    "present and default-open, no ImGui assert (task 3.2.3, I68, AC-62/AC-64)") {
    // This target is ImGui-free at source and cannot read a drawn frame's text -- what IS provable is
    // that BOTH selections draw a real frame without an ImGui assert, exactly as I67 already established
    // for FBX/glTF. The Materials section's actual content is proven at the pure-function level by
    // obj_import_test.cpp's own OI-series (OI60-OI80).
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i68", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/a.obj", MINIMAL_OBJ_TEXT).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/obj_fixture.mtl", MINIMAL_MTL_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());  // 1: the initial scan
    REQUIRE(app->tick());  // 2: let the default dock layout settle before focusing anything
    app->requestPanelFocus("Import Details");

    app->requestAssetBrowserSelectEntry("a.obj");
    REQUIRE(app->tick());  // 3: drains SelectEntry
    REQUIRE(app->tick());  // 4: reconcile -> setTarget -> service() imports -- geometry AND materials
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));
    CHECK(app->presentedLastFrame());

    app->requestAssetBrowserSelectEntry("obj_fixture.mtl");
    REQUIRE(app->tick());  // 5: drains SelectEntry
    REQUIRE(app->tick());  // 6: reconcile -> setTarget -> service() imports -- materials only, "(no
                           // meshes)" / "(no nodes)" (D6's depth-independent .mtl arm)
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- I69-I76: task 3.2.4's Blender section and its editor wiring, through real frames --------------
//
// The SAME two-part discipline every project-opening case in this file uses (BLOCKING-2), plus a THIRD
// part this task adds: `.toolPrefsPath`. `editor_tools.json` is MACHINE-WIDE, exactly like
// `recent_projects.json` -- ANY case that can reach the Blender resolve path MUST redirect it, or it
// reads (and, through Locate.../Re-detect, WRITES) the developer's real file. That is 2.6.1's
// BLOCKING-2 in a third costume, and AC-47 is the rule stated as a criterion (seed S31).
namespace {

[[nodiscard]] std::string uniqueToolPrefsFile() {
    static int counter = 0;
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / ("aero_imgui_layer_tools_" + std::to_string(++counter) + ".json");
    const std::u8string bytes = file.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// NOTHING IN THIS TREE PARSES A .blend, EVER (ADR-003). These bytes are opaque to every line of code
// this task adds: they are hashed as a byte stream by the scan and handed to Blender as a PATH, and
// that is the whole of their interaction with the editor.
constexpr std::string_view OPAQUE_BLEND_TEXT = "not a real .blend, and nothing here ever parses one";

// model_import_test.cpp's own buildGlb, COPIED rather than shared (this suite's standing rule:
// scaffolding is copied, the ASSERTION is shared). AC-42: the GLB is assembled here, in memory, from a
// JSON string -- NO BINARY FILE IS COMMITTED TO THE REPOSITORY.
[[nodiscard]] std::string blenderTestGlb() {
    std::string paddedJson(MINIMAL_GLTF_TEXT);
    while (paddedJson.size() % 4U != 0U) {
        paddedJson += ' ';  // JSON chunk padding is SPACE, per the GLB container spec
    }
    const auto jsonChunkLength = static_cast<std::uint32_t>(paddedJson.size());
    const auto appendU32 = [](std::string& out, std::uint32_t value) {
        out.push_back(static_cast<char>(value & 0xFFU));
        out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
        out.push_back(static_cast<char>((value >> 16U) & 0xFFU));
        out.push_back(static_cast<char>((value >> 24U) & 0xFFU));
    };
    std::string glb;
    glb += "glTF";       // magic
    appendU32(glb, 2U);  // version
    appendU32(glb, 12U + 8U + jsonChunkLength);
    appendU32(glb, jsonChunkLength);
    appendU32(glb, 0x4E4F534AU);  // 'JSON'
    glb += paddedJson;
    return glb;
}

}  // namespace

TEST_CASE(
    "editor: the Blender section draws for a .blend and NOT for a .gltf, through real frames "
    "(task 3.2.4, I69, AC-37)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "blender section i69", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/statue.blend", OPAQUE_BLEND_TEXT).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/b.gltf", MINIMAL_GLTF_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile(),
                                           .toolPrefsPath = uniqueToolPrefsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());  // 1: the initial scan
    REQUIRE(app->tick());  // 2: let the default dock layout settle before focusing anything
    app->requestPanelFocus("Import Details");

    app->requestAssetBrowserSelectEntry("statue.blend");
    REQUIRE(app->tick());  // 3: drains SelectEntry
    REQUIRE(app->tick());  // 4: reconcile -> setTarget -> service() -> NeedsConversion, section drawn
    // NOT NotImportable, ever again -- that enumerator's branch renders one sentence and returns before
    // any section, so it could draw no button at all.
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::NeedsConversion));
    CHECK(app->modelImportState() != static_cast<int>(engine::editor::SessionState::NotImportable));
    CHECK(app->presentedLastFrame());
    REQUIRE(app->tick());  // 5: a second drawn frame in the same state -- no ImGui assert either time

    app->requestAssetBrowserSelectEntry("b.gltf");
    REQUIRE(app->tick());  // 6: drains SelectEntry
    REQUIRE(app->tick());  // 7: the six existing sections and NO Blender section
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: a real frame with Blender NOT FOUND renders the searched-path list without an ImGui "
    "assert (task 3.2.4, I70, AC-30)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "blender missing i70", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/statue.blend", OPAQUE_BLEND_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile(),
                                           .toolPrefsPath = uniqueToolPrefsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());
    REQUIRE(app->tick());
    app->requestPanelFocus("Import Details");

    app->requestAssetBrowserSelectEntry("statue.blend");
    REQUIRE(app->tick());  // drains SelectEntry
    REQUIRE(app->tick());  // NeedsConversion -> the lazy resolve() fires on the NEXT reconcile

    // An override naming a path that does not exist yields EXACTLY ONE candidate, which does not
    // resolve -> ToolMissing, with the searched list retained for the panel to render.
    app->requestBlenderLocate(created.root + "/no-such-blender");
    REQUIRE(app->tick());  // drains it: setOverridePath -> Unknown -> resolveBlender() -> ToolMissing
    CHECK(app->blenderState() == static_cast<int>(engine::editor::BlenderState::ToolMissing));
    REQUIRE(app->tick());  // a real frame IN ToolMissing: the BeginChild scroll region draws
    CHECK(app->presentedLastFrame());
    // AC-30: nothing was spawned to learn that.
    CHECK(app->blenderExportRunCount() == 0);
    CHECK(app->blenderProbeRunCount() == 0);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: a .blend with a nil GUID draws the section with the button DISABLED and spawns nothing "
    "(task 3.2.4, I71, AC-27, seed S29)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "blender nil guid i71", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/statue.blend", OPAQUE_BLEND_TEXT).empty());
    // An INVALID sidecar, which D7 forbids repairing -- so the record's GUID is permanently nil.
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/statue.blend.meta", "{ not json").empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile(),
                                           .toolPrefsPath = uniqueToolPrefsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());
    REQUIRE(app->tick());
    app->requestPanelFocus("Import Details");

    app->requestAssetBrowserSelectEntry("statue.blend");
    REQUIRE(app->tick());  // drains SelectEntry
    REQUIRE(app->tick());  // NeedsConversion with NO identity -- the section draws a DISABLED button
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::NeedsConversion));
    CHECK(app->presentedLastFrame());
    // The lazy resolve is gated on targetHasIdentity(), so it never even ran.
    CHECK(app->blenderState() == static_cast<int>(engine::editor::BlenderState::Unknown));

    // Even a hook-driven request -- which bypasses the disabled button entirely -- starts nothing.
    app->requestBlenderConvert();
    REQUIRE(app->tick());
    REQUIRE(app->tick());
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::NeedsConversion));
    CHECK(app->blenderExportRunCount() == 0);
    CHECK(app->blenderProbeRunCount() == 0);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: a real frame in ConversionFailed renders the message and the log node (task 3.2.4, I72, "
    "AC-36)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "blender failed i72", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/statue.blend", OPAQUE_BLEND_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile(),
                                           .toolPrefsPath = uniqueToolPrefsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());
    REQUIRE(app->tick());
    app->requestPanelFocus("Import Details");

    app->requestAssetBrowserSelectEntry("statue.blend");
    REQUIRE(app->tick());
    REQUIRE(app->tick());

    // `cmake` stands in for Blender on EVERY lane (it exists on every runner by definition): its
    // --version exits 0 and D14 then ATTEMPTS rather than refuses, and handed Blender's own argv it
    // exits non-zero without writing a status file -- the SourceRejected row, reached for real.
    app->requestBlenderLocate(AERO_TEST_CMAKE_COMMAND);
    REQUIRE(app->tick());
    for (int i = 0; i < 20000 && app->blenderState() == static_cast<int>(engine::editor::BlenderState::Probing); ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->blenderState() == static_cast<int>(engine::editor::BlenderState::Ready));
    CHECK(app->blenderProbeRunCount() == 1);

    app->requestBlenderConvert();
    for (int i = 0;
         i < 20000 && app->modelImportState() != static_cast<int>(engine::editor::SessionState::ConversionFailed);
         ++i) {
        REQUIRE(app->tick());
    }
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::ConversionFailed));
    CHECK(app->blenderExportRunCount() == 1);
    REQUIRE(app->tick());  // a real frame IN ConversionFailed: the message + the log TreeNode draw
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: each of the Blender panel's four channels is drained UNCONDITIONALLY, as its own "
    "statement, exactly once (task 3.2.4, I73, AC-39, F9)") {
    // WHY THIS IS A SOURCE-TEXT PROOF: this target is ImGui-free at source and cannot synthesize a
    // widget click, so the panel's own flags can never be SET from here -- constructing a panel and
    // observing four `false`s would be a case that only looks like proof. What IS mechanically
    // decidable, and what F9 exists for, is the DRAIN SHAPE: a `panelConvert || editorConvert`
    // expression short-circuits past the panel's drain and strands the request until the next frame.
    // This tree has shipped that bug once (I30 is its mechanical proof) and guarded against it five
    // times since; this is the sixth.
    constexpr std::string_view SOURCE_PATH = AERO_EDITOR_SRC_DIR "/editor_app.cpp";
    const engine::editor::FileReadResult read = engine::editor::readTextFile(SOURCE_PATH);
    REQUIRE(read.text.has_value());

    std::vector<std::string> code;
    std::string_view remaining = *read.text;
    while (true) {
        const std::size_t newline = remaining.find('\n');
        const std::string_view line = newline == std::string_view::npos ? remaining : remaining.substr(0, newline);
        const std::size_t commentStart = line.find("//");
        code.emplace_back(commentStart == std::string_view::npos ? line : line.substr(0, commentStart));
        if (newline == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(newline + 1U);
    }

    const std::array<std::string_view, 4> channels{"takeConvertRequest()", "takeCancelRequest()", "takeLocateRequest()",
                                                   "takeRedetectRequest()"};
    for (const std::string_view channel : channels) {
        std::size_t hits = 0;
        std::size_t hitLine = code.size();
        for (std::size_t i = 0; i < code.size(); ++i) {
            if (code[i].find(channel) != std::string::npos) {
                ++hits;
                hitLine = i;
            }
        }
        CAPTURE(channel);
        CHECK(hits == 1);  // drained in EXACTLY ONE place, so there is one thing to get right
        REQUIRE(hitLine != code.size());
        // ITS OWN STATEMENT: no `||` on the drain's line, so the call can never be short-circuited past.
        CHECK(code[hitLine].find("||") == std::string::npos);
        // and it initialises a named local rather than being consumed inline inside an `if`.
        CHECK(code[hitLine].find("const bool ") != std::string::npos);
    }

    // task 3.2.4, seed S30, CLOSED HERE and nowhere else. Launching Locate... while another dialog is
    // in flight is a real defect -- DialogChannel holds ONE slot, so the second result silently
    // overwrites the first's -- and it is UNREACHABLE by every runtime tier in this tree: no test can
    // synthesize a native dialog, so removing the guard reddened NOTHING across both test binaries
    // when it was seeded directly. This assertion is its only mechanical cover.
    std::size_t launchLine = code.size();
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i].find("launchLocateBlenderDialog(") != std::string::npos) {
            launchLine = i;
        }
    }
    REQUIRE(launchLine != code.size());
    // Walk BACK to the `if` that guards it and require BOTH conditions -- a live channel and no dialog
    // already in flight (the scene_session.cpp guard shape, reused verbatim).
    bool guarded = false;
    for (std::size_t i = launchLine; i > 0 && i + 6U > launchLine; --i) {
        if (code[i - 1U].find("fileFlow.dialog == DialogKind::None") != std::string::npos &&
            code[i - 1U].find("dialogChannel != nullptr") != std::string::npos) {
            guarded = true;
        }
    }
    CHECK(guarded);
}

TEST_CASE(
    "editor: editor_app.cpp still calls importSession.service( EXACTLY ONCE, textually after "
    "drawShellUi( (task 3.2.4, I74, AC-38a)") {
    // I60's own proof, RE-ASSERTED against this task's edits: the post-draw call gained one argument
    // and did NOT move, and tick() gained no fourth post-draw call. poll() reaches the service through
    // the session that owns it, so there is exactly one thing to get right.
    constexpr std::string_view SOURCE_PATH = AERO_EDITOR_SRC_DIR "/editor_app.cpp";
    const engine::editor::FileReadResult read = engine::editor::readTextFile(SOURCE_PATH);
    REQUIRE(read.text.has_value());

    std::string code;
    code.reserve(read.text->size());
    std::string_view remaining = *read.text;
    while (true) {
        const std::size_t newline = remaining.find('\n');
        const std::string_view line = newline == std::string_view::npos ? remaining : remaining.substr(0, newline);
        const std::size_t commentStart = line.find("//");
        code.append(commentStart == std::string_view::npos ? line : line.substr(0, commentStart));
        code.push_back('\n');
        if (newline == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(newline + 1U);
    }

    std::size_t serviceHits = 0;
    std::size_t serviceAt = std::string::npos;
    for (std::size_t at = code.find("importSession.service("); at != std::string::npos;
         at = code.find("importSession.service(", at + 1U)) {
        ++serviceHits;
        serviceAt = at;
    }
    REQUIRE(serviceHits == 1);
    const std::size_t drawAt = code.find("drawShellUi(");
    REQUIRE(drawAt != std::string::npos);
    CHECK(serviceAt > drawAt);
}

TEST_CASE(
    "editor: import_details_panel.cpp contains NO poll( call at all (task 3.2.4, I75, AC-38b, seed "
    "S20)") {
    // I60's shape applied to a SECOND file. BlenderService::poll() spawns processes and waits on them;
    // running it from a draw walk would put a syscall inside ImGui's frame and break the "record a
    // pending action, apply it after the walk" rule every panel in this tree follows. NO RUNTIME TIER
    // IN THIS TREE CAN SEE THAT VIOLATION -- this is the only mechanical cover it has.
    constexpr std::string_view SOURCE_PATH = AERO_EDITOR_SRC_DIR "/import_details_panel.cpp";
    const engine::editor::FileReadResult read = engine::editor::readTextFile(SOURCE_PATH);
    REQUIRE(read.text.has_value());

    std::string code;
    code.reserve(read.text->size());
    std::string_view remaining = *read.text;
    while (true) {
        const std::size_t newline = remaining.find('\n');
        const std::string_view line = newline == std::string_view::npos ? remaining : remaining.substr(0, newline);
        const std::size_t commentStart = line.find("//");
        code.append(commentStart == std::string_view::npos ? line : line.substr(0, commentStart));
        code.push_back('\n');
        if (newline == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(newline + 1U);
    }
    CHECK(code.find("poll(") == std::string::npos);
    // and no mutating member of the session or the service either (AC-39): the panel holds a
    // `const ModelImportSession*`, so these are compile-time impossible -- asserted anyway, because the
    // pointer's constness is one edit away from being widened.
    CHECK(code.find("requestConversion(") == std::string::npos);
    CHECK(code.find("cancelConversion(") == std::string::npos);
    CHECK(code.find("setOverridePath(") == std::string::npos);
    CHECK(code.find("noteArtifactUnusable(") == std::string::npos);
    CHECK(code.find("blenderMutable(") == std::string::npos);
}

TEST_CASE(
    "editor: requestBlenderLocate writes the tool preferences to the CONFIGURED path, never the real "
    "one (task 3.2.4, I76, AC-47, seed S31)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "blender prefs i76", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/statue.blend", OPAQUE_BLEND_TEXT).empty());

    const std::string prefsPath = uniqueToolPrefsFile();
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(prefsPath), ec);
    REQUIRE_FALSE(engine::editor::fileExists(prefsPath));  // it does not exist BEFORE

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile(),
                                           .toolPrefsPath = prefsPath});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());
    REQUIRE(app->tick());

    app->requestBlenderLocate(AERO_TEST_CMAKE_COMMAND);
    REQUIRE(app->tick());  // drains it: setOverridePath writes the prefs, then re-resolves

    // The file appeared AT THE CONFIGURED PATH, and it round-trips through the public parser.
    REQUIRE(engine::editor::fileExists(prefsPath));
    const engine::editor::FileReadResult prefs = engine::editor::readTextFile(prefsPath);
    REQUIRE(prefs.text.has_value());
    const std::optional<engine::editor::ToolPrefs> parsed = engine::editor::parseToolPrefs(*prefs.text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->blenderPath == std::string(AERO_TEST_CMAKE_COMMAND));
    // and the override is what resolution then found -- one candidate, alone (AC-3).
    CHECK(app->blenderBinaryPath() == std::string(AERO_TEST_CMAKE_COMMAND));

    // Re-detect CLEARS it, through the same one file.
    app->requestBlenderRedetect();
    REQUIRE(app->tick());
    const engine::editor::FileReadResult cleared = engine::editor::readTextFile(prefsPath);
    REQUIRE(cleared.text.has_value());
    const std::optional<engine::editor::ToolPrefs> reparsed = engine::editor::parseToolPrefs(*cleared.text);
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->blenderPath.empty());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
    std::filesystem::remove(std::filesystem::path(prefsPath), ec);
}

// ---- I77-I80: the code-review round's own cases ---------------------------------------------------
namespace {

// The comment-stripped code lines of a file under editor/src -- I73/I74/I75's own reader, lifted into
// one helper now that a fourth case needs it. Comments are stripped because every gate in this task
// reasons about CODE, and a citation in prose must never be able to satisfy or break one.
[[nodiscard]] std::vector<std::string> editorSourceCodeLines(std::string_view absolutePathUtf8) {
    const engine::editor::FileReadResult read = engine::editor::readTextFile(absolutePathUtf8);
    REQUIRE(read.text.has_value());
    std::vector<std::string> code;
    std::string_view remaining = *read.text;
    while (true) {
        const std::size_t newline = remaining.find('\n');
        const std::string_view line = newline == std::string_view::npos ? remaining : remaining.substr(0, newline);
        const std::size_t commentStart = line.find("//");
        code.emplace_back(commentStart == std::string_view::npos ? line : line.substr(0, commentStart));
        if (newline == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(newline + 1U);
    }
    return code;
}

[[nodiscard]] std::size_t soleLineContaining(const std::vector<std::string>& code, std::string_view needle) {
    std::size_t hits = 0;
    std::size_t at = code.size();
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i].find(needle) != std::string::npos) {
            ++hits;
            at = i;
        }
    }
    CAPTURE(needle);
    REQUIRE(hits == 1);
    return at;
}

// The first non-blank code line at or after `from` -- so an assertion about "what follows this case
// label" is not defeated by a blank line or a re-wrap.
[[nodiscard]] std::size_t nextCodeLine(const std::vector<std::string>& code, std::size_t from) {
    for (std::size_t i = from; i < code.size(); ++i) {
        if (code[i].find_first_not_of(" \t\r") != std::string::npos) {
            return i;
        }
    }
    return code.size();
}

}  // namespace

TEST_CASE(
    "editor: the Blender section treats Unknown as NOT PROBED -- it falls through to the controls "
    "(task 3.2.4, I77, code-review B1)") {
    // WHY A SOURCE-TEXT PROOF: no tier in this tree reads rendered ImGui text, so "the panel offered a
    // Re-import" is not observable at runtime -- I78 below can only prove the frame drew without an
    // assert. What IS mechanically decidable is the SHAPE of the decision, and the defect was exactly a
    // shape: `Unknown` shared `Probing`'s arm, which returns before every control. On a pure CACHE HIT
    // nothing ever resolves (§A-9's lazy resolve), so Unknown is the state for the WHOLE session and a
    // correctly imported .blend showed one false sentence and no buttons at all.
    constexpr std::string_view SOURCE_PATH = AERO_EDITOR_SRC_DIR "/import_details_panel.cpp";
    const std::vector<std::string> code = editorSourceCodeLines(SOURCE_PATH);

    // The Unknown label FALLS THROUGH to another case, and specifically NOT to Probing's.
    const std::size_t unknownAt = soleLineContaining(code, "case BlenderState::Unknown:");
    const std::size_t afterUnknown = nextCodeLine(code, unknownAt + 1U);
    REQUIRE(afterUnknown != code.size());
    CHECK(code[afterUnknown].find("case BlenderState::") != std::string::npos);
    CHECK(code[afterUnknown].find("case BlenderState::Probing:") == std::string::npos);

    // THE ASSERTION THAT ACTUALLY DISCRIMINATES, and the weaker "it falls through to some case" above
    // does not: collect the CONTIGUOUS case labels immediately above the probing message -- that is its
    // arm, precisely -- and require it to be Probing ALONE. Re-grouping Unknown with Probing (the shape
    // that shipped) puts two labels there and reddens here. Blank lines are skipped because the reader
    // above strips comments to empty ones.
    const std::size_t messageAt = soleLineContaining(code, "Checking the Blender version");
    std::vector<std::string> arm;
    for (std::size_t i = messageAt; i > 0; --i) {
        const std::string& line = code[i - 1U];
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        if (line.find("case BlenderState::") == std::string::npos) {
            break;
        }
        arm.push_back(line);
    }
    REQUIRE(arm.size() == 1);
    CHECK(arm[0].find("case BlenderState::Probing:") != std::string::npos);

    // code-review S4, the panel half: the "produced by Blender ..." line names the ARTIFACT's own
    // recorded producer, never blender().versionString() -- which is the currently INSTALLED Blender and
    // is either empty (nothing probed) or a different binary entirely. BS50 proves the session carries
    // the record's value; this proves the panel is the thing that reads it. Scanned as a WINDOW rather
    // than a line, because clang-format puts a format string and its argument on separate lines.
    const std::size_t importedAt = soleLineContaining(code, "session.state() == SessionState::Imported");
    const std::size_t reimportAt = soleLineContaining(code, R"(ImGui::Button("Re-import"))");
    REQUIRE(reimportAt > importedAt);
    bool namesTheRecord = false;
    bool namesTheInstalledBlender = false;
    for (std::size_t i = importedAt; i < reimportAt; ++i) {
        namesTheRecord = namesTheRecord || code[i].find("artifactBlenderVersion()") != std::string::npos;
        namesTheInstalledBlender = namesTheInstalledBlender || code[i].find("versionString()") != std::string::npos;
    }
    CHECK(namesTheRecord);
    CHECK_FALSE(namesTheInstalledBlender);

    // code-review NOTE 11: the Blender log node is DEFAULT-OPEN, and that is what makes anything inside
    // it reachable by a test at all -- no tier in this tree can click a TreeNode, which is this file's
    // own stated reason for the panel's six sections defaulting open. I80 drives the refused-by-cap
    // branch through a real frame; it can only do so while this flag is here, and it cannot itself tell
    // that the flag went away.
    const std::size_t logNodeAt = soleLineContaining(code, R"("Blender log")");
    CHECK(code[logNodeAt].find("ImGuiTreeNodeFlags_DefaultOpen") != std::string::npos);
}

TEST_CASE(
    "editor: a CACHE HIT on a .blend draws real frames with the service never resolved, and spawns "
    "nothing (task 3.2.4, I78, AC-22 through the panel, code-review B1)") {
    // The task's HEADLINE FLOW, driven through real frames for the first time: every prior GPU-tier
    // .blend case is a cache MISS. This is the state in which the panel used to say "Checking the
    // Blender version..." forever.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "blender cache hit i78", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string blendPath = created.root + "/assets/statue.blend";
    REQUIRE(engine::editor::writeTextFileAtomic(blendPath, OPAQUE_BLEND_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile(),
                                           .toolPrefsPath = uniqueToolPrefsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());  // the initial scan MINTS the sidecar, which is where the GUID comes from
    REQUIRE(app->tick());
    app->requestPanelFocus("Import Details");

    // Stage the cache hit from OUTSIDE the app, exactly as a previous session would have left it: the
    // GUID comes from the sidecar the scan just wrote, and the source hash from the same primitive the
    // scan itself uses.
    const engine::editor::FileReadResult metaText = engine::editor::readTextFile(blendPath + ".meta");
    REQUIRE(metaText.text.has_value());
    const engine::editor::MetaParseResult meta = engine::editor::parseMeta(*metaText.text);
    REQUIRE(meta.guid.has_value());
    const engine::editor::FileHashResult sourceHash = engine::editor::hashFileContents(blendPath);
    REQUIRE(sourceHash.hash.has_value());

    const std::string exportDir = created.root + '/' + std::string(engine::editor::ASSET_CACHE_DIR_NAME) + '/' +
                                  std::string(engine::editor::BLENDER_EXPORT_DIR_NAME);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(exportDir), ec);
    REQUIRE_FALSE(ec);
    engine::editor::ExportProvenance record;
    record.guid = *meta.guid;
    record.sourcePath = "assets/statue.blend";
    record.blenderPath = "/nonexistent/blender";
    record.blenderVersion = "4.2.1";
    record.scriptVersion = engine::editor::BLENDER_SCRIPT_VERSION;
    // The fingerprint RE-DERIVED from the two public primitives rather than by calling the production
    // helper, so a change to what goes into it reddens this case instead of moving with it.
    const std::string defaultSettingsMeta =
        engine::editor::writeMetaText(engine::Guid{}, engine::editor::ImportSettings{});
    record.settingsFingerprint =
        engine::formatContentHash(engine::hashBytes(std::as_bytes(std::span<const char>(defaultSettingsMeta))));
    record.sourceHash = *sourceHash.hash;
    const std::string guidText = engine::formatGuid(*meta.guid);
    REQUIRE(engine::editor::writeTextFileAtomic(exportDir + '/' + guidText + ".glb", blenderTestGlb()).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(exportDir + '/' + guidText + ".json",
                                                engine::editor::writeExportProvenanceText(record))
                .empty());

    app->requestAssetBrowserSelectEntry("statue.blend");
    REQUIRE(app->tick());  // drains SelectEntry
    REQUIRE(app->tick());  // reconcile -> setTarget -> service() -> the CACHE HIT
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));
    // Nothing resolved, nothing probed, nothing spawned -- AC-22, now through a real frame.
    CHECK(app->blenderState() == static_cast<int>(engine::editor::BlenderState::Unknown));
    CHECK(app->blenderExportRunCount() == 0);
    CHECK(app->blenderProbeRunCount() == 0);
    CHECK(app->presentedLastFrame());

    // Five more frames in the SAME state: the section draws every one of them, and the count of imports
    // and spawns is unchanged. Before the fix these frames rendered one false sentence and no controls.
    for (int i = 0; i < 5; ++i) {
        REQUIRE(app->tick());
    }
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));
    CHECK(app->blenderExportRunCount() == 0);
    CHECK(app->blenderProbeRunCount() == 0);
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: with NO project open, Locate... is remembered but resolution is DEFERRED (task 3.2.4, I79, "
    "code-review NOTE 6)") {
    // The path this closes is invisible on a machine that always has a project open: with none, the
    // asset database's root is EMPTY, and the export directory used to be built by concatenation
    // regardless -- "/Library/BlenderExports", an absolute path at the filesystem root that the version
    // probe's own directory creation would then attempt. It fails harmlessly on this machine and creates
    // a real drive-root directory on Windows, which no local run could ever have shown.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "blender no project i79", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string prefsPath = uniqueToolPrefsFile();
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(prefsPath), ec);

    // projectPath = "" is NO PROJECT (D0), and restoreLastProject = false keeps it that way.
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = "",
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile(),
                                           .toolPrefsPath = prefsPath});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());
    REQUIRE(app->tick());

    app->requestBlenderLocate(AERO_TEST_CMAKE_COMMAND);
    REQUIRE(app->tick());  // drains it: the preferences are written, the resolve is DEFERRED
    for (int i = 0; i < 5; ++i) {
        REQUIRE(app->tick());
    }

    // The CHOICE IS REMEMBERED -- the preferences file is written by setOverridePath, before any of
    // this -- so nothing is lost by deferring.
    REQUIRE(engine::editor::fileExists(prefsPath));
    const engine::editor::FileReadResult prefs = engine::editor::readTextFile(prefsPath);
    REQUIRE(prefs.text.has_value());
    const std::optional<engine::editor::ToolPrefs> parsed = engine::editor::parseToolPrefs(*prefs.text);
    REQUIRE(parsed.has_value());
    CHECK(parsed->blenderPath == std::string(AERO_TEST_CMAKE_COMMAND));

    // ...and NOTHING was resolved against a root that does not exist. Unknown is exactly the condition
    // tick()'s lazy resolve re-tests once a project is open.
    CHECK(app->blenderState() == static_cast<int>(engine::editor::BlenderState::Unknown));
    CHECK(app->blenderBinaryPath().empty());
    CHECK(app->blenderProbeRunCount() == 0);
    CHECK(app->blenderExportRunCount() == 0);
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
    std::filesystem::remove(std::filesystem::path(prefsPath), ec);
}

TEST_CASE(
    "editor: a real frame renders the REFUSED-BY-CAP log branch, byte count and path instead of "
    "contents (task 3.2.4, I80, AC-35, code-review NOTE 11)") {
#if defined(_WIN32)
    MESSAGE("skipped on Windows: no scripted fake can produce a chatty child for Blender's argv (see BS14)");
#else
    // The branch this drives had NEVER executed anywhere: the log node shipped default-CLOSED, and no
    // tier in this tree can synthesize a click, so its std::format over a byte count and an absolute
    // path was unreachable under every sanitizer on every lane. The node is default-open now, for the
    // same reason the panel's six sections are.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "blender big log i80", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/statue.blend", OPAQUE_BLEND_TEXT).empty());

    // A fake Blender that answers --version and then writes MORE THAN MAX_TOOL_LOG_BYTES to stdout,
    // which the redirect sends straight to <guid>.log. `dd` is the portable way to produce a known
    // number of bytes without a loop; the content is irrelevant, only the size is.
    const std::string fake = created.root + "/fake-blender";
    {
        std::ofstream out(std::filesystem::path(fake), std::ios::binary | std::ios::trunc);
        REQUIRE(static_cast<bool>(out));
        out << "#!/bin/sh\n"
               "if [ \"$1\" = \"--version\" ]; then printf '%s\\n' 'Blender 5.2.0 LTS'; exit 0; fi\n"
               "dd if=/dev/zero bs=1024 count=400 2>/dev/null\n"
               "exit 1\n";
    }
    std::error_code ec;
    std::filesystem::permissions(std::filesystem::path(fake), std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, ec);
    REQUIRE_FALSE(ec);

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile(),
                                           .toolPrefsPath = uniqueToolPrefsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());
    REQUIRE(app->tick());
    app->requestPanelFocus("Import Details");

    app->requestAssetBrowserSelectEntry("statue.blend");
    REQUIRE(app->tick());
    REQUIRE(app->tick());

    app->requestBlenderLocate(fake);
    REQUIRE(app->tick());
    for (int i = 0; i < 20000 && app->blenderState() == static_cast<int>(engine::editor::BlenderState::Probing); ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->blenderState() == static_cast<int>(engine::editor::BlenderState::Ready));

    app->requestBlenderConvert();
    for (int i = 0;
         i < 20000 && app->modelImportState() != static_cast<int>(engine::editor::SessionState::ConversionFailed);
         ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->modelImportState() == static_cast<int>(engine::editor::SessionState::ConversionFailed));
    REQUIRE(app->blenderExportRunCount() == 1);

    // THE ASSERTION THAT MAKES THE FRAMES BELOW NON-VACUOUS: the log really is over the cap, so the
    // branch that draws a byte count and a path -- rather than 400 KiB of contents -- is the one that
    // runs. Without it, "a frame drew" would say nothing about WHICH branch drew.
    CHECK(app->blenderLogRefusedByCap());
    REQUIRE(app->tick());  // a real frame IN that branch: no ImGui assert, no format fault, no overread
    CHECK(app->presentedLastFrame());
    REQUIRE(app->tick());
    CHECK(app->presentedLastFrame());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
#endif
}

// ---- task 3.2.5's fixtures for I81/I82, hoisted into NAMED constants (MSVC's legacy preprocessor
// breaks on a raw string literal containing an escaped quote passed straight into a doctest macro) ----
namespace {

constexpr std::string_view SKINNED_DAE_TEXT =
    R"(<?xml version="1.0"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><contributor><authoring_tool>Aero test</authoring_tool></contributor>
    <unit meter="0.01" name="centimeter"/><up_axis>Z_UP</up_axis></asset>
  <library_geometries><geometry id="g1" name="Tri"><mesh>
    <source id="p"><float_array id="pa" count="9">0 0 0 1 0 0 0 1 0</float_array>
      <technique_common><accessor source="#pa" count="3" stride="3">
        <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
      </accessor></technique_common></source>
    <vertices id="v"><input semantic="POSITION" source="#p"/></vertices>
    <triangles count="1"><input semantic="VERTEX" source="#v" offset="0"/><p>0 1 2</p></triangles>
  </mesh></geometry></library_geometries>
  <library_controllers><controller id="skin1" name="SkinCtrl"><skin source="#g1">
    <bind_shape_matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</bind_shape_matrix>
    <source id="jointNames"><Name_array id="jn" count="2">Bone1 Bone2</Name_array>
      <technique_common><accessor source="#jn" count="2" stride="1">
        <param name="JOINT" type="name"/></accessor></technique_common></source>
    <source id="invBind">
      <float_array id="ib" count="32">1 0 0 1 0 1 0 2 0 0 1 3 0 0 0 1 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</float_array>
      <technique_common><accessor source="#ib" count="2" stride="16">
        <param name="TRANSFORM" type="float4x4"/></accessor></technique_common></source>
    <source id="skinWeights"><float_array id="wa" count="3">1 0.5 0.5</float_array>
      <technique_common><accessor source="#wa" count="3" stride="1">
        <param name="WEIGHT" type="float"/></accessor></technique_common></source>
    <joints><input semantic="JOINT" source="#jointNames"/>
      <input semantic="INV_BIND_MATRIX" source="#invBind"/></joints>
    <vertex_weights count="3"><input semantic="JOINT" source="#jointNames" offset="0"/>
      <input semantic="WEIGHT" source="#skinWeights" offset="1"/>
      <vcount>1 2 1</vcount><v>0 0 0 1 1 2 1 0</v></vertex_weights>
  </skin></controller></library_controllers>
  <library_visual_scenes><visual_scene id="S" name="S">
    <node id="Bone1" sid="Bone1" name="Bone1" type="JOINT">
      <matrix sid="transform">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>
      <node id="Bone2" sid="Bone2" name="Bone2" type="JOINT">
        <matrix sid="transform">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>
      </node>
    </node>
    <node id="Mesh" name="Mesh"><instance_controller url="#skin1"><skeleton>#Bone1</skeleton></instance_controller></node>
  </visual_scene></library_visual_scenes>
  <scene><instance_visual_scene url="#S"/></scene>
</COLLADA>
)";

constexpr std::string_view TEXTURED_PLY_TEXT =
    "ply\nformat ascii 1.0\ncomment TextureFile scan.png\nelement vertex 3\nproperty float x\n"
    "property float y\nproperty float z\nelement face 1\nproperty list uchar int vertex_index\n"
    "end_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n";

constexpr std::string_view TRIANGLE_STL_TEXT =
    "solid Part\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\n"
    "endloop\nendfacet\nendsolid Part\n";

}  // namespace

TEST_CASE(
    "editor: the Import Details panel draws a real frame for a .dae, Source Space row included (task "
    "3.2.5, I81, AC-60)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i81", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/rig.dae", SKINNED_DAE_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);  // "Assets" shares DockSlot::Bottom with "Console"
    REQUIRE(app->tick());                        // 1: the initial scan

    app->requestAssetBrowserSelectEntry("rig.dae");
    REQUIRE(app->tick());  // 2: drains SelectEntry
    REQUIRE(app->tick());  // 3: reconcile -> setTarget -> service() imports the rig
    CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));
    CHECK(app->modelImportCount() == 1);

    REQUIRE(app->tick());  // 4: let the default dock layout settle before focusing anything
    app->requestPanelFocus("Import Details");
    REQUIRE(app->tick());  // 5: draws the Imported state for real -- all six sections default-open, the
                           // Hierarchy and Skins sections resolving localId through their existing map,
                           // and the Source Space row PRESENT because a .dae declares a unit and an axis
    CHECK(app->presentedLastFrame());
    REQUIRE(app->tick());  // 6: a second drawn frame, so a one-frame-only defect cannot hide

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE(
    "editor: the Import Details panel draws a real frame for a .ply and an .stl, with NO Source Space "
    "row (task 3.2.5, I82, AC-60)") {
    // The row means something precisely BECAUSE it is absent when the format declares nothing. Neither
    // PLY nor STL declares a unit or an axis, and inventing one was rejected -- so this case is the
    // negative half of I81 and breaks independently of it.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "import details i82", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/scan.ply", TEXTURED_PLY_TEXT).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/part.stl", TRIANGLE_STL_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    REQUIRE(app->tick());  // 1: the initial scan

    for (const std::string_view leaf : {std::string_view("scan.ply"), std::string_view("part.stl")}) {
        app->requestAssetBrowserSelectEntry(leaf);
        REQUIRE(app->tick());  // drains SelectEntry
        REQUIRE(app->tick());  // reconcile -> setTarget -> service()
        INFO("leaf: ", leaf);
        CHECK(app->modelImportState() == static_cast<int>(engine::editor::SessionState::Imported));
        app->requestPanelFocus("Import Details");
        REQUIRE(app->tick());  // draws the Imported state for real
        CHECK(app->presentedLastFrame());
    }

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- task 3.4.2: the Material panel's registration and its sticky target (I83-I85) ---------------

namespace {

// A canonical .aeromat, written into a temp project by the cases below. The GPU tier deliberately
// writes its OWN material text rather than reaching for AERO_MATERIAL_FIXTURES_DIR: that definition
// is on aero_editor_shell_test, and no fixture path belongs in this file (the "each TU keeps its own
// fixture" precedent, a sixth application).
constexpr std::string_view MINIMAL_AEROMAT_TEXT =
    "{\n"
    "  \"version\": 1,\n"
    "  \"name\": \"First\",\n"
    "  \"metallicFactor\": 0.25,\n"
    "  \"roughnessFactor\": 0.75\n"
    "}\n";
constexpr std::string_view SECOND_AEROMAT_TEXT =
    "{\n"
    "  \"version\": 1,\n"
    "  \"name\": \"Second\",\n"
    "  \"roughnessFactor\": 0.125\n"
    "}\n";
// version 2: refused by name, never partially read (docs/09 section 11.6).
constexpr std::string_view REJECT_AEROMAT_TEXT = "{\n  \"version\": 2\n}\n";

}  // namespace

TEST_CASE("editor: the Material panel is registered eighth, right of the Inspector (task 3.4.2, I83)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i83", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx, {.persistLayout = false, .unfocusedFrameCapHz = 0.0F, .restoreLastProject = false});
    REQUIRE(app.has_value());
    // Registered in create(), BEFORE the first tick() -- checked here, before any tick runs, which is
    // exactly the condition placeUnplacedPanels requires (the I52 shape).
    CHECK(app->panels().count() == 8);
    // ... and LAST, so no existing panel's registration index shifted and the Inspector keeps the
    // selected Right-dock tab.
    CHECK(std::string_view(app->panels().panelAt(7).id()) == "Material");

    const engine::editor::Panel* panel = app->panels().find("Material");
    REQUIRE(panel != nullptr);
    CHECK(std::string_view(panel->id()) == "Material");
    CHECK(std::string_view(panel->title()) == "Material");
    CHECK(panel->defaultDockSlot() == engine::editor::DockSlot::Right);
    // options() is deliberately NOT overridden: the panel's own window must keep scrolling.
    const engine::editor::PanelOptions opts = panel->options();
    CHECK_FALSE(opts.noScrollbar);
    CHECK_FALSE(opts.hasMenuBar);
    CHECK_FALSE(opts.noPadding);
    CHECK_FALSE(opts.noScrollWithMouse);

    // The black-box surface's own default state, before any selection has ever happened.
    CHECK(app->materialTargetPath().empty());
    CHECK_FALSE(app->materialParseOk());
    CHECK_FALSE(app->materialDirty());
    CHECK(app->materialDocument() == nullptr);

    // Three real frames with no project at all -- the untargeted branch draws, and an unbalanced
    // Begin/End there would be an IM_ASSERT abort in the Debug ImGui build.
    app->panels().setVisible("Inspector", false);  // Inspector registers first and wins the Right tab
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    CHECK(app->materialTargetPath().empty());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the material target is STICKY across selections (task 3.4.2, I84, AC-7/AC-8)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i84", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/first.aeromat", MINIMAL_AEROMAT_TEXT).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/second.aeromat", SECOND_AEROMAT_TEXT).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/notes.txt", "not a material").empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    // "Assets" shares DockSlot::Bottom with "Console", which registers first and wins the tab, so
    // AssetBrowserPanel::onDraw() would never run and the SelectEntry action would never drain
    // (2.2.4's C5 precedent). "Inspector" shares DockSlot::Right with "Material" for the same reason.
    app->panels().setVisible("Console", false);
    app->panels().setVisible("Inspector", false);
    REQUIRE(app->tick());  // 1: the initial scan
    CHECK(app->materialTargetPath().empty());

    app->requestAssetBrowserSelectEntry("first.aeromat");
    REQUIRE(app->tick());  // 2: drains SelectEntry -> selection() == "first.aeromat"
    REQUIRE(app->tick());  // 3: the reconcile's SIXTH statement sees it and targets the material
    CHECK(app->materialTargetPath() == "first.aeromat");
    CHECK(app->materialParseOk());
    REQUIRE(app->materialDocument() != nullptr);
    CHECK(app->materialDocument()->name == "First");
    CHECK_FALSE(app->materialDirty());

    // An edit through the request seam -- exactly what a widget records.
    engine::MaterialDocument edited = *app->materialDocument();
    edited.roughnessFactor = 0.5F;
    app->requestMaterialDocument(edited);
    REQUIRE(app->tick());
    CHECK(app->materialDirty());

    // STICKY (D3, seed S3's GPU half): selecting a NON-material leaves the target -- and the unapplied
    // edit -- exactly where they are. Without this, every click hunting for a texture to reference
    // would tear the edit session down.
    app->requestAssetBrowserSelectEntry("notes.txt");
    REQUIRE(app->tick());
    REQUIRE(app->tick());
    CHECK(app->materialTargetPath() == "first.aeromat");
    CHECK(app->materialDirty());

    // Clearing the selection is the same answer.
    app->requestAssetBrowserSelectEntry("");
    REQUIRE(app->tick());
    REQUIRE(app->tick());
    CHECK(app->materialTargetPath() == "first.aeromat");
    CHECK(app->materialDirty());

    // A DIFFERENT existing .aeromat DOES retarget, and the unapplied edit is discarded -- recorded as
    // accepted (D4/D5), and the one path that can lose an edit.
    app->requestAssetBrowserSelectEntry("second.aeromat");
    REQUIRE(app->tick());
    REQUIRE(app->tick());
    CHECK(app->materialTargetPath() == "second.aeromat");
    CHECK(app->materialParseOk());
    REQUIRE(app->materialDocument() != nullptr);
    CHECK(app->materialDocument()->name == "Second");
    CHECK_FALSE(app->materialDirty());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a rejected .aeromat draws its error and refuses Apply (task 3.4.2, I85, AC-9)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i85", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string badPath = created.root + "/assets/bad.aeromat";
    REQUIRE(engine::editor::writeTextFileAtomic(badPath, REJECT_AEROMAT_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    app->panels().setVisible("Inspector", false);
    REQUIRE(app->tick());

    app->requestAssetBrowserSelectEntry("bad.aeromat");
    REQUIRE(app->tick());
    REQUIRE(app->tick());
    CHECK(app->materialTargetPath() == "bad.aeromat");
    CHECK_FALSE(app->materialParseOk());  // no editor at all -- never a half-loaded document
    CHECK(app->materialDocument() == nullptr);
    CHECK_FALSE(app->materialDirty());

    // Apply and Revert are refused, and the file is NEVER "repaired": it may hold a hand-recoverable
    // value one `git checkout` away (the invalid-.meta D7 posture, applied to a second format).
    app->requestMaterialApply();
    REQUIRE(app->tick());
    app->requestMaterialRevert();
    REQUIRE(app->tick());
    const engine::editor::FileReadResult after = engine::editor::readTextFile(badPath);
    REQUIRE(after.text.has_value());
    CHECK(*after.text == std::string(REJECT_AEROMAT_TEXT));

    // The error branch drew on every one of those frames; an unbalanced Begin/End would have been an
    // IM_ASSERT abort in the Debug ImGui build, so a green run IS the assertion.
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- task 3.4.2: the Material panel's editing half (I86-I87) -------------------------------------

namespace {

// Every scalar docs/09 section 11 defines, at a distinct non-default value, plus one unknown key so
// the status strip's WARN list renders too. alphaMode is "mask", so the conditional alphaCutoff row
// (AC-19) is drawn from the very first frame rather than only after an edit.
constexpr std::string_view FULL_AEROMAT_TEXT =
    "{\n"
    "  \"version\": 1,\n"
    "  \"name\": \"Full\",\n"
    "  \"baseColorFactor\": [0.9, 0.8, 0.7, 1.0],\n"
    "  \"metallicFactor\": 0.5,\n"
    "  \"roughnessFactor\": 0.4,\n"
    "  \"emissiveFactor\": [0.0, 1.5, 0.0],\n"
    "  \"normalScale\": 1.25,\n"
    "  \"occlusionStrength\": 0.6,\n"
    "  \"alphaMode\": \"mask\",\n"
    "  \"alphaCutoff\": 0.875,\n"
    "  \"doubleSided\": true,\n"
    "  \"authoredBy\": \"a key no reader of this format knows\"\n"
    "}\n";

}  // namespace

TEST_CASE("editor: the Material panel draws every state and every slot arm (task 3.4.2, I86, AC-24)") {
    // THE BALANCE ORACLE. A green run IS the assertion: an unbalanced Begin/End, PushID/PopID,
    // PushStyleColor/PopStyleColor or BeginDisabled/EndDisabled is an IM_ASSERT ABORT in the Debug
    // ImGui build, not a wrong picture, so every branch this case reaches is a branch proven balanced.
    //
    // A STATED COVERAGE GAP, so nobody reads this case as more than it is: no tier in this tree can
    // click, so a BeginCombo's LIST BODY never executes here -- the picker's per-record loop, its
    // Selectable arms and the five token combos' bodies are drawn only by a hand on a mouse. That is
    // the same closed-node limitation the CollapsingHeader sections carry (3.2.4's recorded lesson),
    // which is exactly why every header below is DefaultOpen and why the manual pass owns the rest.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i86", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string assetsRoot = created.root + "/assets";
    REQUIRE(engine::editor::writeTextFileAtomic(assetsRoot + "/full.aeromat", FULL_AEROMAT_TEXT).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(assetsRoot + "/bad.aeromat", REJECT_AEROMAT_TEXT).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(assetsRoot + "/notes.txt", "not a texture").empty());
    REQUIRE(writeBinaryFixture(assetsRoot + "/wood.png", TINY_PNG_RED.data(), TINY_PNG_RED.size()).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);  // so Assets draws and SelectEntry drains
    // Hiding the Inspector is what lets Material win the Right-dock tab (2.2.4's C5 rule, an eighth
    // application). MEASURED, not assumed, because a balance oracle whose panel never draws is
    // vacuous and looks identical to a passing one: seeding an unbalanced PushID inside the AC-22
    // uvSet branch below -- a branch reachable only AFTER the bindings land, 38 assertions in -- makes
    // this case abort with SIGABRT. Import Details shares the same dock slot and does not need hiding.
    app->panels().setVisible("Inspector", false);

    // --- state 1: untargeted ----------------------------------------------------------------------
    for (int i = 0; i < 2; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    CHECK(app->materialTargetPath().empty());

    // --- state 2: the error document ----------------------------------------------------------------
    app->requestAssetBrowserSelectEntry("bad.aeromat");
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    CHECK(app->materialTargetPath() == "bad.aeromat");
    CHECK_FALSE(app->materialParseOk());

    // --- state 3: the full document, every scalar row + the WARN list -------------------------------
    app->requestAssetBrowserSelectEntry("full.aeromat");
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    REQUIRE(app->materialTargetPath() == "full.aeromat");
    REQUIRE(app->materialDocument() != nullptr);
    CHECK(app->materialDocument()->name == "Full");
    CHECK(app->materialDocument()->alphaCutoff == 0.875F);
    CHECK((app->materialDocument()->alphaMode == engine::MaterialAlphaMode::Mask));
    CHECK_FALSE(app->materialDirty());  // reading a non-canonical file is not an edit (D5)

    // --- every slot arm at once: resolved Texture, resolved NON-texture, unresolvable, unbound ------
    const std::optional<engine::Guid> textureGuid = app->assetGuidForPath("wood.png");
    REQUIRE(textureGuid.has_value());
    const std::optional<engine::Guid> notesGuid = app->assetGuidForPath("notes.txt");
    REQUIRE(notesGuid.has_value());

    engine::MaterialDocument bound = *app->materialDocument();
    bound.baseColor = engine::MaterialTextureSlot{.guid = *textureGuid,
                                                  .uvSet = 2,  // AC-22's "consumers honour set 0" note
                                                  .wrapU = engine::MaterialWrap::Clamp,
                                                  .wrapV = engine::MaterialWrap::Mirror,
                                                  .minFilter = engine::MaterialFilter::Nearest,
                                                  .magFilter = engine::MaterialFilter::Linear,
                                                  .mipFilter = engine::MaterialMipFilter::None};
    bound.metallicRoughness = engine::MaterialTextureSlot{.guid = *notesGuid};  // AC-21: not a texture
    // A non-nil GUID belonging to nothing: AC-21's "not in this project" arm, and Apply must stay
    // legal for it, so validateMaterial must still pass -- which is why it is non-nil.
    bound.normal = engine::MaterialTextureSlot{.guid = engine::Guid{.hi = 0xDEADBEEFU, .lo = 0xFEEDFACEU}};
    bound.occlusion.reset();  // the unbound arm
    bound.emissive.reset();
    app->requestMaterialDocument(bound);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    REQUIRE(app->materialDocument() != nullptr);
    REQUIRE(app->materialDocument()->baseColor.has_value());
    CHECK(app->materialDocument()->baseColor->uvSet == 2U);
    CHECK(app->materialDirty());

    // --- AC-19: the row hides, the value does NOT reset ---------------------------------------------
    engine::MaterialDocument opaque = *app->materialDocument();
    opaque.alphaMode = engine::MaterialAlphaMode::Opaque;
    app->requestMaterialDocument(opaque);
    for (int i = 0; i < 5; ++i) {  // several frames of drawing WITHOUT the cutoff row
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    REQUIRE(app->materialDocument() != nullptr);
    CHECK((app->materialDocument()->alphaMode == engine::MaterialAlphaMode::Opaque));
    CHECK(app->materialDocument()->alphaCutoff == 0.875F);  // preserved across the mode change

    // Back to mask: the row returns with the value it had.
    engine::MaterialDocument masked = *app->materialDocument();
    masked.alphaMode = engine::MaterialAlphaMode::Mask;
    app->requestMaterialDocument(masked);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->materialDocument() != nullptr);
    CHECK(app->materialDocument()->alphaCutoff == 0.875F);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: edit -> dirty -> Apply writes canonical bytes; Revert round-trips (task 3.4.2, I87)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i87", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string materialPath = created.root + "/assets/edit.aeromat";
    REQUIRE(engine::editor::writeTextFileAtomic(materialPath, MINIMAL_AEROMAT_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    app->panels().setVisible("Inspector", false);
    app->requestAssetBrowserSelectEntry("edit.aeromat");
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->materialTargetPath() == "edit.aeromat");
    REQUIRE(app->materialDocument() != nullptr);
    const engine::MaterialDocument onDisk = *app->materialDocument();
    CHECK_FALSE(app->materialDirty());

    // --- edit -> dirty --------------------------------------------------------------------------
    engine::MaterialDocument edited = onDisk;
    edited.name = "Edited";
    edited.roughnessFactor = 0.125F;
    edited.doubleSided = true;
    app->requestMaterialDocument(edited);
    REQUIRE(app->tick());
    CHECK(app->materialDirty());
    REQUIRE(app->materialDocument() != nullptr);
    CHECK(app->materialDocument()->name == "Edited");
    // Not one byte has moved yet: an edit is a session state, never a write (INV-2).
    const engine::editor::FileReadResult untouched = engine::editor::readTextFile(materialPath);
    REQUIRE(untouched.text.has_value());
    CHECK(*untouched.text == std::string(MINIMAL_AEROMAT_TEXT));

    // --- Apply -> the canonical writer's own bytes, exactly ---------------------------------------
    app->requestMaterialApply();
    REQUIRE(app->tick());
    CHECK_FALSE(app->materialDirty());  // the file copy ADOPTED the session copy
    const engine::editor::FileReadResult applied = engine::editor::readTextFile(materialPath);
    REQUIRE(applied.text.has_value());
    CHECK(*applied.text == engine::writeMaterialText(edited));

    // --- Revert -> back to what is on disk, discarding the session copy ---------------------------
    engine::MaterialDocument abandoned = edited;
    abandoned.metallicFactor = 0.03125F;
    app->requestMaterialDocument(abandoned);
    REQUIRE(app->tick());
    REQUIRE(app->materialDirty());
    app->requestMaterialRevert();
    REQUIRE(app->tick());
    CHECK_FALSE(app->materialDirty());
    REQUIRE(app->materialDocument() != nullptr);
    CHECK(app->materialDocument()->metallicFactor == edited.metallicFactor);
    CHECK(app->materialDocument()->name == "Edited");
    // Revert re-READS the file, so what it restored is what Apply wrote -- never a cached copy.
    const engine::editor::FileReadResult afterRevert = engine::editor::readTextFile(materialPath);
    REQUIRE(afterRevert.text.has_value());
    CHECK(*afterRevert.text == engine::writeMaterialText(edited));

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- task 3.4.2: the live preview (I88-I90) -------------------------------------------------------

TEST_CASE("editor: the preview and the viewport both render in ONE frame (task 3.4.2, I88, AC-28/AC-31)") {
    // P7's COEXISTENCE PROBE, and the FIRST case of this task's preview half by deliberate order.
    // NOTHING IN THIS TREE HAS EVER RUN TWO RenderTargets AND TWO ForwardRenderers ALIVE IN ONE EDITOR
    // FRAME. render_target.hpp's own synchronisation note argues it works -- endFrame() submits its own
    // command buffer and command buffers submitted earlier order before later ones, so ImGui's may
    // sample either colour texture with no explicit barrier -- and tests/render_material_test.cpp
    // already drives one target through two sequential frames. Neither proves TWO targets in one frame
    // with ImGui sampling both, which is exactly what the Material panel does beside the Viewport.
    //
    // It also runs the whole teardown chain -- texture cache, material, renderer, target, then the
    // device -- under ASan on the Debug lane, which is AC-31's clean-shutdown clause and the OTHER half
    // of what this case is for.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i88", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(
        engine::editor::writeTextFileAtomic(created.root + "/assets/preview.aeromat", MINIMAL_AEROMAT_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);    // so Assets draws and SelectEntry drains
    app->panels().setVisible("Inspector", false);  // so Material wins the Right dock tab and onDraw runs
    // The Viewport is CENTER-docked and visible throughout: every tick below submits its offscreen
    // scene pass as well as the preview's.
    REQUIRE(app->panels().find("Viewport") != nullptr);
    REQUIRE(app->viewportCamera() != nullptr);

    REQUIRE(app->tick());
    app->requestAssetBrowserSelectEntry("preview.aeromat");
    REQUIRE(app->tick());
    REQUIRE(app->tick());
    REQUIRE(app->materialTargetPath() == "preview.aeromat");
    REQUIRE(app->materialDocument() != nullptr);

    // Five frames with BOTH passes live. Each tick: the panel draws (recording a preview request), the
    // viewport's renderScene submits, the preview's service submits, and ImGui's own command buffer
    // samples both colour textures.
    for (int i = 0; i < 5; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());  // the ImGui frame reached the screen with both images in it
    }
#if AERO_SHADER_TOOLS_ENABLED
    CHECK(app->materialPreviewAvailable());
    // THE NON-VACUITY WITNESS. Without this number a green run above proves only that the editor did
    // not crash: it would look identical if the preview had never rendered a single frame.
    CHECK(app->materialPreviewFrameCount() >= 3);
#else
    // -DAERO_SHADER_TOOLS=OFF (AC-32). The coexistence this case exists to probe is not merely unproven
    // here, it CANNOT occur: with no cooked shaders MaterialPreview latches Unavailable in its
    // constructor and neither offscreen renderer is ever created. That portion -- and only that portion
    // -- is unobservable in this configuration, so the OFF CONTRACT is asserted in its place rather
    // than the case being skipped: the preview costs exactly nothing (no target, no renderer, no pass),
    // while the five frames above still ticked and still presented, and the session is untouched by its
    // absence. The other half of this case needs no arm at all -- app.reset() runs the whole teardown
    // chain under ASan in both configurations.
    CHECK_FALSE(app->materialPreviewAvailable());
    CHECK(app->materialPreviewFrameCount() == 0);
    CHECK(app->materialTargetPath() == "preview.aeromat");
    CHECK(app->materialDocument() != nullptr);
    CHECK_FALSE(app->materialDirty());
#endif
    // The viewport is still there and still answering -- it was not torn down or starved by the second
    // renderer, and the editor camera it draws through survived the whole run.
    CHECK(app->viewportCamera() != nullptr);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();  // texture cache -> material -> renderer -> target -> device, under ASan
}

TEST_CASE("editor: a blend material latches the preview renderer's opaque WARN (task 3.4.2, I89, AC-30)") {
    // AC-30's MECHANICAL half. `blend` is stored by the format, drawn OPAQUE by the renderer and
    // latched once per renderer lifetime (3.4.1's D9) -- so the latch is the only part of "the mode
    // reached the preview" that is observable without reading pixels. It is also this case's own
    // non-vacuity witness for the push: the latch cannot flip unless updateMaterial carried the edited
    // alphaMode into the registry AND a frame actually drew that material.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i89", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/blend.aeromat", MINIMAL_AEROMAT_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    app->panels().setVisible("Inspector", false);
    REQUIRE(app->tick());
    app->requestAssetBrowserSelectEntry("blend.aeromat");
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->materialTargetPath() == "blend.aeromat");
#if AERO_SHADER_TOOLS_ENABLED
    REQUIRE(app->materialPreviewAvailable());
    REQUIRE(app->materialPreviewFrameCount() >= 1);
    // The fixture is OPAQUE, and several frames of it have already drawn: the latch is still down.
    CHECK_FALSE(app->materialPreviewBlendDrawnOpaque());
#else
    // -DAERO_SHADER_TOOLS=OFF (AC-32). The latch this case reads lives on the preview's own
    // ForwardRenderer, and in this configuration there is no renderer to hold one -- so the POSITIVE
    // arm below is the one portion of this case that cannot be observed without cooked shaders. The
    // rest is asserted identically: the OFF contract here is no preview, no frames, and therefore a
    // latch that is down for a reason that has nothing to do with alphaMode.
    REQUIRE_FALSE(app->materialPreviewAvailable());
    REQUIRE(app->materialPreviewFrameCount() == 0);
    CHECK_FALSE(app->materialPreviewBlendDrawnOpaque());
#endif

    REQUIRE(app->materialDocument() != nullptr);
    engine::MaterialDocument blended = *app->materialDocument();
    blended.alphaMode = engine::MaterialAlphaMode::Blend;
    app->requestMaterialDocument(blended);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->materialDocument() != nullptr);
    CHECK((app->materialDocument()->alphaMode == engine::MaterialAlphaMode::Blend));
#if AERO_SHADER_TOOLS_ENABLED
    // The edit reached the GPU through updateMaterial and the next draw took the blend arm -- which
    // draws OPAQUE and says so once. A known-and-expected of this task, not a defect (3.4.1's own gap).
    CHECK(app->materialPreviewBlendDrawnOpaque());
#else
    // The same edit, the same four ticks, the same stored mode -- and still no latch, because there is
    // no renderer to take the blend arm. This is AC-32's "everything else works identically" as an
    // assertion rather than a claim: the edit reached the SESSION exactly as it does above, and the
    // preview's absence changed nothing about it.
    CHECK_FALSE(app->materialPreviewBlendDrawnOpaque());
    CHECK(app->materialPreviewFrameCount() == 0);
#endif
    // The edit is UNAPPLIED throughout: the preview shows the session copy, never the file (D6).
    CHECK(app->materialDirty());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a hidden Material panel renders nothing and still edits (task 3.4.2, I90, AC-32)") {
    // TWO properties in one case, because they are the same property seen from both sides.
    //   * S25's RUNTIME half: the preview renders only on frames the panel actually DREW, so a Material
    //     panel the user closed creates no GPU object and submits no pass at all. A seed that renders
    //     every tick regardless moves materialPreviewFrameCount() here.
    //   * AC-32's SHAPE: with no preview at all -- which is exactly what a tools-OFF build has
    //     permanently -- targeting, editing, validation and Apply are untouched. This is the closest a
    //     GPU-tier case can get to that build; §V.2's fresh tools-OFF configuration is the real proof.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i90", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string materialPath = created.root + "/assets/hidden.aeromat";
    REQUIRE(engine::editor::writeTextFileAtomic(materialPath, MINIMAL_AEROMAT_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    // Console hidden so the Assets panel draws and SelectEntry drains; Material hidden outright, which
    // is the ONE reliable way to keep onDraw from running -- shell_ui.cpp's walk `continue`s before
    // Begin for a hidden panel. MEASURED, correcting this case's first draft: hiding the *Inspector's*
    // rival instead does not work, because in the default layout the LAST panel docked into the shared
    // Right node is the selected tab, so Material draws even with the Inspector visible (the preview
    // rendered 5 frames in 6 ticks that way). The existing setVisible("Inspector", false) in I84/I86 is
    // therefore belt-and-braces on this machine rather than load-bearing -- left exactly as it is,
    // since a restored layout on someone else's machine can select any tab it likes.
    app->panels().setVisible("Console", false);
    app->panels().setVisible("Material", false);
    REQUIRE_FALSE(app->panels().visible("Material"));
    app->requestAssetBrowserSelectEntry("hidden.aeromat");
    for (int i = 0; i < 6; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    // Targeted and parsed -- the SESSION runs off tick(), not off the panel.
    REQUIRE(app->materialTargetPath() == "hidden.aeromat");
    REQUIRE(app->materialDocument() != nullptr);
    // ... and the preview cost exactly nothing: no target, no renderer, no pass.
    CHECK_FALSE(app->materialPreviewAvailable());
    CHECK(app->materialPreviewFrameCount() == 0);

    // Editing and Apply are completely unaffected by the preview's absence (AC-32's clause).
    engine::MaterialDocument edited = *app->materialDocument();
    edited.name = "EditedWhileHidden";
    edited.roughnessFactor = 0.5F;
    app->requestMaterialDocument(edited);
    REQUIRE(app->tick());
    CHECK(app->materialDirty());
    app->requestMaterialApply();
    REQUIRE(app->tick());
    CHECK_FALSE(app->materialDirty());
    const engine::editor::FileReadResult applied = engine::editor::readTextFile(materialPath);
    REQUIRE(applied.text.has_value());
    CHECK(*applied.text == engine::writeMaterialText(edited));
    CHECK(app->materialPreviewFrameCount() == 0);  // still nothing rendered, through an entire Apply

    app->panels().setVisible("Material", true);
#if AERO_SHADER_TOOLS_ENABLED
    // Show the panel again and the preview starts -- which is what makes every assertion above a
    // statement about VISIBILITY rather than about a preview that could never work here.
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    CHECK(app->materialPreviewAvailable());
    CHECK(app->materialPreviewFrameCount() >= 1);
#else
    // -DAERO_SHADER_TOOLS=OFF (AC-32), and this is the arm that states the difference between the two
    // configurations exactly: making the panel VISIBLE cannot conjure a preview in a build with no
    // cooked shaders. The panel draws its one "preview unavailable" line instead -- four ticks that all
    // present, with no IM_ASSERT -- while the counters stay at the same zeros the HIDDEN panel produced
    // above, and everything the case already asserted (targeting, the edit, Apply, the bytes on disk)
    // held identically on the way here. Only the "the preview starts" arm is unobservable without
    // cooked shaders; nothing else in this case is.
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
    CHECK_FALSE(app->materialPreviewAvailable());
    CHECK(app->materialPreviewFrameCount() == 0);
    CHECK(app->materialTargetPath() == "hidden.aeromat");
    CHECK_FALSE(app->materialDirty());
#endif

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- task 3.4.2: the preview's texture chain (I91, I92) -------------------------------------------

TEST_CASE("editor: a slot GUID loads through decode -> cook -> parse -> upload (task 3.4.2, I91, AC-31/D7)") {
    // The real pipeline in miniature, driven end to end by a real .png in a real project, and the
    // RUNTIME half of D7's cache key: the SAME source bound to baseColor AND occlusion loads TWICE,
    // because those two slots sample different colour spaces and a cooked artifact's colour space is
    // its format. ME48 is the tier-0 half of the same rule.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i91", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string assetsRoot = created.root + "/assets";
    REQUIRE(engine::editor::writeTextFileAtomic(assetsRoot + "/tex.aeromat", MINIMAL_AEROMAT_TEXT).empty());
    REQUIRE(writeBinaryFixture(assetsRoot + "/wood.png", TINY_PNG_RED.data(), TINY_PNG_RED.size()).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    app->panels().setVisible("Inspector", false);
    app->requestAssetBrowserSelectEntry("tex.aeromat");
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->materialTargetPath() == "tex.aeromat");
#if AERO_SHADER_TOOLS_ENABLED
    REQUIRE(app->materialPreviewAvailable());
#else
    // -DAERO_SHADER_TOOLS=OFF (AC-32): no preview exists, so MaterialPreview::service returns before
    // rebuildSlots and a bound GUID is never even looked up. What this case can still prove here -- and
    // does, arm by arm below -- is that NO LOAD IS ATTEMPTED (a strictly stronger statement than "no
    // texture is ready", which is why textureLoadAttempts() is asserted beside the count every time)
    // and that binding, rebinding and clearing a slot reach the session exactly as they do with the
    // preview live.
    REQUIRE_FALSE(app->materialPreviewAvailable());
#endif
    // Nothing referenced yet: no key, no entry, no attempt.
    CHECK(app->materialPreviewTextureCount() == 0);
    CHECK(app->materialPreviewTextureLoadAttempts() == 0);

    const std::optional<engine::Guid> textureGuid = app->assetGuidForPath("wood.png");
    REQUIRE(textureGuid.has_value());
    REQUIRE(app->materialDocument() != nullptr);
    engine::MaterialDocument bound = *app->materialDocument();
    bound.baseColor = engine::MaterialTextureSlot{.guid = *textureGuid};
    app->requestMaterialDocument(bound);
    // ONE LOAD PER TICK, so this is a budget assertion as much as a chain assertion.
    for (int i = 0; i < 5; ++i) {
        REQUIRE(app->tick());
    }
#if AERO_SHADER_TOOLS_ENABLED
    CHECK(app->materialPreviewTextureCount() == 1);
    CHECK(app->materialPreviewTextureLoadAttempts() == 1);  // decoded, cooked, parsed and uploaded ONCE
#else
    // Nothing was read, decoded, cooked or uploaded -- the five ticks above cost the texture chain
    // exactly zero work. The BINDING itself is unaffected: the session holds the slot and is dirty.
    CHECK(app->materialPreviewTextureCount() == 0);
    CHECK(app->materialPreviewTextureLoadAttempts() == 0);
    REQUIRE(app->materialDocument() != nullptr);
    REQUIRE(app->materialDocument()->baseColor.has_value());
    CHECK((app->materialDocument()->baseColor->guid == *textureGuid));
    CHECK(app->materialDirty());
#endif

    // The ORM-atlas shape: the same GUID in a LINEAR slot. Two entries, two uploads, one source.
    engine::MaterialDocument both = *app->materialDocument();
    both.occlusion = engine::MaterialTextureSlot{.guid = *textureGuid};
    app->requestMaterialDocument(both);
    for (int i = 0; i < 5; ++i) {
        REQUIRE(app->tick());
    }
#if AERO_SHADER_TOOLS_ENABLED
    CHECK(app->materialPreviewTextureCount() == 2);
    CHECK(app->materialPreviewTextureLoadAttempts() == 2);
#else
    // Still not attempted -- twice over, now from two slots naming one source. The colour-space rule
    // that makes those two DIFFERENT keys is tier-0's (ME48); what this arm pins is that neither slot
    // provoked a read here.
    CHECK(app->materialPreviewTextureCount() == 0);
    CHECK(app->materialPreviewTextureLoadAttempts() == 0);
    REQUIRE(app->materialDocument() != nullptr);
    CHECK(app->materialDocument()->baseColor.has_value());
    CHECK(app->materialDocument()->occlusion.has_value());
#endif

    // Clearing both slots orphans both uploads, and the service pass -- never the draw walk -- is what
    // releases them (INV-5). The count returning to zero is that pass having run.
    engine::MaterialDocument cleared = *app->materialDocument();
    cleared.baseColor.reset();
    cleared.occlusion.reset();
    app->requestMaterialDocument(cleared);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
#if AERO_SHADER_TOOLS_ENABLED
    CHECK(app->materialPreviewTextureCount() == 0);
    CHECK(app->materialPreviewTextureLoadAttempts() == 2);  // releasing is not a load
    CHECK(app->materialPreviewFrameCount() >= 3);           // and the preview kept drawing throughout
#else
    // Nothing was ever uploaded, so there is nothing to orphan and no service pass to release it in --
    // and no frame was ever drawn. The document round-tripped through bind, rebind and clear untouched
    // by any of that, which is the whole of AC-32's claim for this case.
    CHECK(app->materialPreviewTextureCount() == 0);
    CHECK(app->materialPreviewTextureLoadAttempts() == 0);
    CHECK(app->materialPreviewFrameCount() == 0);
    REQUIRE(app->materialDocument() != nullptr);
    CHECK_FALSE(app->materialDocument()->baseColor.has_value());
    CHECK_FALSE(app->materialDocument()->occlusion.has_value());
#endif

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a broken image fails ONCE and stays failed (task 3.4.2, I92, AC-31)") {
    // STICKY FAILURE, the ThumbnailLedger rule restated for a second cache. Without it a broken image
    // re-reads and re-fails every tick forever, which is a frame-rate defect rather than a wrong
    // picture -- and it is INVISIBLE to a count-only assertion, which is why textureLoadAttempts()
    // exists and is asserted here rather than "the count is still zero".
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i92", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string assetsRoot = created.root + "/assets";
    REQUIRE(engine::editor::writeTextFileAtomic(assetsRoot + "/broken.aeromat", MINIMAL_AEROMAT_TEXT).empty());
    // A valid texture NAME whose bytes are ASCII: it fails at stbi_info_from_memory, before any decode
    // loop, which is this tree's chosen corrupt-image shape (3.1.3's R3).
    REQUIRE(engine::editor::writeTextFileAtomic(assetsRoot + "/bad.png", CORRUPT_PNG_BYTES).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    app->panels().setVisible("Inspector", false);
    app->requestAssetBrowserSelectEntry("broken.aeromat");
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->materialTargetPath() == "broken.aeromat");
#if AERO_SHADER_TOOLS_ENABLED
    REQUIRE(app->materialPreviewAvailable());
#else
    // -DAERO_SHADER_TOOLS=OFF (AC-32). Stickiness is unobservable where nothing is ever attempted, so
    // the arms below assert the shape that IS true here and is the stronger one: the broken image is
    // not read once and remembered, it is never read at all, and thirteen ticks later that is still so.
    // The half of this case that is configuration-independent -- Apply stays legal over an unloadable
    // reference -- is asserted outside every arm, exactly as it was.
    REQUIRE_FALSE(app->materialPreviewAvailable());
#endif

    const std::optional<engine::Guid> badGuid = app->assetGuidForPath("bad.png");
    REQUIRE(badGuid.has_value());
    REQUIRE(app->materialDocument() != nullptr);
    engine::MaterialDocument bound = *app->materialDocument();
    bound.baseColor = engine::MaterialTextureSlot{.guid = *badGuid};
    app->requestMaterialDocument(bound);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
    }
    CHECK(app->materialPreviewTextureCount() == 0);
#if AERO_SHADER_TOOLS_ENABLED
    CHECK(app->materialPreviewTextureLoadAttempts() == 1);
#else
    CHECK(app->materialPreviewTextureLoadAttempts() == 0);  // not attempted, so not failed either
    REQUIRE(app->materialDocument() != nullptr);
    REQUIRE(app->materialDocument()->baseColor.has_value());
    CHECK((app->materialDocument()->baseColor->guid == *badGuid));
#endif

    // TEN more frames: still one attempt, ever. The slot draws its built-in default and the preview
    // keeps rendering -- a broken reference degrades the picture, it does not stop it.
    const std::size_t framesBefore = app->materialPreviewFrameCount();
    for (int i = 0; i < 10; ++i) {
        REQUIRE(app->tick());
        CHECK(app->presentedLastFrame());
    }
#if AERO_SHADER_TOOLS_ENABLED
    CHECK(app->materialPreviewTextureLoadAttempts() == 1);
    CHECK(app->materialPreviewTextureCount() == 0);
    CHECK(app->materialPreviewFrameCount() > framesBefore);
#else
    // Ten more frames, still zero attempts: a broken reference costs nothing per tick here for the same
    // reason a good one does -- the chain is never entered. The editor itself kept presenting through
    // all ten, which is the part of "it does not stop it" this configuration can still show.
    CHECK(app->materialPreviewTextureLoadAttempts() == 0);
    CHECK(app->materialPreviewTextureCount() == 0);
    CHECK(framesBefore == 0);
    CHECK(app->materialPreviewFrameCount() == framesBefore);
#endif
    // Apply stays legal with an unloadable reference: a material may name an asset that is not usable
    // yet, and the editor never blocks a save over it (AC-21).
    CHECK(app->materialDirty());

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- task 3.4.2: New Material, and the watcher interplay it shares with Apply (I93, I94) -----------

namespace {

// task 3.4.2 (AC-41/D15/INV-C5): every file under the assets tree and its mtime, sorted by path. The
// D15 assertion shape widened from ONE file to the WHOLE tree: "the rescan wrote zero bytes" is a
// statement about everything, and a per-file check can only ever prove it about the file it names.
// Directories are excluded -- a directory's own mtime moves when a `.aero-tmp` sibling appears and
// vanishes, which is writeTextFileAtomic working, not a byte written to an asset.
[[nodiscard]] std::vector<std::pair<std::string, std::filesystem::file_time_type>> assetsTreeMtimes(
    const std::string& assetsRootUtf8) {
    std::vector<std::pair<std::string, std::filesystem::file_time_type>> stamps;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(std::filesystem::path(assetsRootUtf8), ec), end;
         it != end && !ec; it.increment(ec)) {
        if (it->is_directory(ec)) {
            continue;
        }
        const std::filesystem::file_time_type when = std::filesystem::last_write_time(it->path(), ec);
        if (ec) {
            ec.clear();
            continue;
        }
        const std::u8string bytes = it->path().u8string();
        stamps.emplace_back(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), when);
    }
    std::sort(stamps.begin(), stamps.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return stamps;
}

}  // namespace

TEST_CASE("editor: New Material creates, mints and selects, all outside the draw walk (task 3.4.2, I93, AC-5)") {
    // The authoring loop closed: before this, the only way to get a first .aeromat into a project was a
    // text editor. The click records; tick() drains; the file appears with the canonical DEFAULT bytes;
    // the scan that same pass mints its .meta; the new entry is selected and the Material panel is
    // holding it. ME47 is the tier-0 half (the bytes); this is the whole path.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i93", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string assetsRoot = created.root + "/assets";

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx,
        {.persistLayout = false,
         .unfocusedFrameCapHz = 0.0F,
         .projectPath = created.root,
         .restoreLastProject = false,
         .recentProjectsPath = uniqueRecentsFile(),
         .assetWatch = {.enabled = true, .dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0}});
    REQUIRE(app.has_value());
    // The Assets panel must actually DRAW, or the recorded action is never drained through
    // applyPending() and the request never reaches tick() at all (2.2.4's C5 rule).
    app->panels().setVisible("Console", false);
    app->panels().setVisible("Inspector", false);

    // MANDATORY before any trigger arithmetic (3.1.4's §A-1): the first scan writes sidecars of its own.
    REQUIRE(tickToQuiescence(*app));
    const std::uint64_t baseTriggers = app->assetWatchTriggerCount();
    const std::size_t baseAssets = app->assetCount();

    app->requestAssetBrowserCreateMaterial();
    // Tick 1 drains the recorded action through the panel's own applyPending; tick 2's reconcile drains
    // the request, writes the file and rescans; tick 2's draw drains the selection the drain queued;
    // tick 3's reconcile is where the material session sees it. Four is one spare.
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }

    const std::string materialPath = assetsRoot + "/NewMaterial.aeromat";
    const engine::editor::FileReadResult written = engine::editor::readTextFile(materialPath);
    REQUIRE(written.text.has_value());
    // THE DEFAULT DOCUMENT, canonically written -- seed S20's runtime witness.
    CHECK(*written.text == engine::writeMaterialText(engine::MaterialDocument{}));

    // The scan minted its identity: the sidecar exists and the record carries a real GUID.
    std::error_code existsEc;
    CHECK(std::filesystem::exists(std::filesystem::path(materialPath + ".meta"), existsEc));
    CHECK_FALSE(existsEc);
    const std::optional<engine::Guid> guid = app->assetGuidForPath("NewMaterial.aeromat");
    REQUIRE(guid.has_value());
    CHECK(guid->valid());
    CHECK(app->assetCount() == baseAssets + 1);

    // AUTO-SELECTED: the Material panel is already holding the file the button just made, clean.
    CHECK(app->materialTargetPath() == "NewMaterial.aeromat");
    CHECK(app->materialParseOk());
    CHECK_FALSE(app->materialDirty());
    REQUIRE(app->materialDocument() != nullptr);
    CHECK((*app->materialDocument() == engine::MaterialDocument{}));

    // The watcher's own view of a brand-new file, asserted as a DELTA after a SECOND quiescence and
    // never as an absolute (3.1.4's rule). MEASURED, not predicted: the file and its freshly minted
    // sidecar both exist before the next sweep runs, because the drain rescans in its OWN pass -- so
    // one sweep sees both and fires ONCE. The +2 that an EXTERNALLY created file produces (I44) needs
    // the two to appear in different sweeps, which an internal create never does.
    REQUIRE(tickToQuiescence(*app));
    CHECK(app->assetWatchTriggerCount() == baseTriggers + 1);

    // A SECOND create in the same directory counts up rather than overwriting -- the unique-name rule
    // driven by a real listing, not by a literal (AC-5).
    app->requestAssetBrowserCreateMaterial();
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    const engine::editor::FileReadResult second = engine::editor::readTextFile(assetsRoot + "/NewMaterial-2.aeromat");
    REQUIRE(second.text.has_value());
    CHECK(*second.text == engine::writeMaterialText(engine::MaterialDocument{}));
    // And the FIRST one is untouched -- a create never overwrites (this is what the listing is for).
    const engine::editor::FileReadResult first = engine::editor::readTextFile(materialPath);
    REQUIRE(first.text.has_value());
    CHECK(*first.text == engine::writeMaterialText(engine::MaterialDocument{}));
    CHECK(app->materialTargetPath() == "NewMaterial-2.aeromat");

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: Apply echoes ONCE and the rescan behind it writes zero bytes (task 3.4.2, I94, AC-41)") {
    // D10's Apply half, mechanically. The watcher has no self-write suppression (3.1.4, confirmed in
    // code), so an Apply IS seen -- once. What must not happen is the scan that follows rewriting
    // anything in the assets tree: the .meta is valid and D6 never rewrites a valid one, so the whole
    // tree's mtimes must be identical across it. That is the D15/INV-C5 assertion, widened from the
    // cache index to every file a user owns.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i94", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string assetsRoot = created.root + "/assets";
    const std::string materialPath = assetsRoot + "/echo.aeromat";
    REQUIRE(engine::editor::writeTextFileAtomic(materialPath, MINIMAL_AEROMAT_TEXT).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(assetsRoot + "/notes.txt", "an ordinary neighbour").empty());

    std::optional<engine::editor::EditorApp> app = engine::editor::EditorApp::create(
        *device, *window, ctx,
        {.persistLayout = false,
         .unfocusedFrameCapHz = 0.0F,
         .projectPath = created.root,
         .restoreLastProject = false,
         .recentProjectsPath = uniqueRecentsFile(),
         .assetWatch = {.enabled = true, .dirsPerPoll = 64, .cooldownMs = 0, .settleMs = 0}});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    app->panels().setVisible("Inspector", false);
    app->requestAssetBrowserSelectEntry("echo.aeromat");

    REQUIRE(tickToQuiescence(*app));
    REQUIRE(app->materialTargetPath() == "echo.aeromat");
    REQUIRE(app->materialDocument() != nullptr);
    const std::uint64_t baseTriggers = app->assetWatchTriggerCount();

    engine::MaterialDocument edited = *app->materialDocument();
    edited.name = "Echoed";
    edited.roughnessFactor = 0.75F;
    app->requestMaterialDocument(edited);
    REQUIRE(app->tick());
    REQUIRE(app->materialDirty());
    // A pending edit costs the watcher NOTHING: nothing has touched the disk yet.
    CHECK(app->assetWatchTriggerCount() == baseTriggers);

    app->requestMaterialApply();
    REQUIRE(app->tick());
    REQUIRE_FALSE(app->materialDirty());
    const engine::editor::FileReadResult applied = engine::editor::readTextFile(materialPath);
    REQUIRE(applied.text.has_value());
    CHECK(*applied.text == engine::writeMaterialText(edited));

    // The snapshot is taken AFTER the write and BEFORE the rescan the watcher is about to drive, which
    // is exactly the window the assertion is about.
    const std::vector<std::pair<std::string, std::filesystem::file_time_type>> before = assetsTreeMtimes(assetsRoot);
    REQUIRE(before.size() >= 4U);  // echo.aeromat + notes.txt + both sidecars -- never a vacuous sweep

    REQUIRE(tickToQuiescence(*app));
    CHECK(app->assetWatchTriggerCount() == baseTriggers + 1);  // ONE echo, not two: no new sidecar
    const std::vector<std::pair<std::string, std::filesystem::file_time_type>> after = assetsTreeMtimes(assetsRoot);
    CHECK(after == before);  // ZERO bytes written to the assets tree by the rescan (D6/D15)

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

// ---- task 3.4.2: the two source-text pins the preview's lifetime rule needs (I95, I96) -------------
//
// WHY SOURCE TEXT: SDL_ReleaseGPUTexture frees SYNCHRONOUSLY on Vulkan and D3D12 and defers only on
// Metal, so a GPU destroy moved into the draw walk is deterministic corruption on two platforms and
// completely invisible on this one -- 3.1.3's BLOCKING-1 and 3.1.4's D9, both of which were confirmed
// by direct sabotage to redden NOTHING on Metal. There is no runtime tier in this tree that can see
// the violation, so the rule is pinned mechanically instead of trusted.

TEST_CASE("editor: the preview's service pass runs AFTER the draw walk, exactly once (task 3.4.2, I95)") {
    const std::vector<std::string> code = editorSourceCodeLines(AERO_EDITOR_SRC_DIR "/editor_app.cpp");
    const std::size_t drawWalkAt = soleLineContaining(code, "drawShellUi(");
    const std::size_t serviceAt = soleLineContaining(code, "servicePreview(");
    // ONE call site (soleLineContaining REQUIREs it), and it is textually AFTER the single call that
    // invokes every panel's onDraw -- I60's own proof shape, applied to this task's fourth occupant of
    // the post-draw slot. A proof about THIS file's current text, not a guarantee against a future
    // refactor that moves the call somewhere else; validation row 9's cost blank is the other cover.
    CHECK(serviceAt > drawWalkAt);
}

TEST_CASE("editor: every preview GPU lifetime call sits where ImGui's ordering allows (task 3.4.2, I96, INV-5)") {
    // REWRITTEN BY THE CODE-REVIEW ROUND, because the claim this case used to make was satisfied by the
    // defect it was supposed to catch. It asserted "a destroy lives in the service pass" and greped for
    // `destroyTexture(`/`destroyMaterial(` alone -- so `target->resize(...)`, which destroys BOTH
    // textures inside RenderTarget::allocate, was invisible to it, and a resize sitting in the service
    // pass PASSED the old rule while being exactly the use-after-free the rule exists to prevent.
    //
    // THE RULE, RESTATED AS ORDERING RATHER THAN MEMBERSHIP. ImGui records an ImTextureID during the
    // draw walk and binds it inside ImGuiLayer::endFrame, which runs AFTER tick()'s post-draw service
    // pass. So:
    //   * a call that RELEASES the texture ImGui is holding (resize -> allocate -> destroyTexture) must
    //     happen BEFORE the handle is read, i.e. in the draw walk -- ViewportPanel's own ordering;
    //   * a call that releases anything ImGui never sees (the slot upload cache) stays in the service
    //     pass, where a create-then-destroy cannot race a frame at all.
    // The two are not in tension: one is about the texture ImGui BINDS, the other about textures it
    // never touches.
    const std::vector<std::string> code = editorSourceCodeLines(AERO_EDITOR_SRC_DIR "/material_preview.cpp");

    // Walk UP to the nearest DEFINITION HEADER -- a line whose first non-blank character is in column 0
    // and which names MaterialPreview:: -- and read the function name out of it. Every definition in
    // that file is written that way (`void MaterialPreview::service(`,
    // `MaterialPreview::~MaterialPreview()`), and a member's body is always indented, so the first such
    // line above any statement is the function that statement belongs to.
    const auto ownerOf = [&code](std::size_t index) {
        std::string owner;
        for (std::size_t j = index + 1U; j > 0; --j) {
            const std::string& line = code[j - 1U];
            const std::size_t at = line.find("MaterialPreview::");
            if (at == std::string::npos || line.find_first_not_of(" \t") != 0) {
                continue;
            }
            const std::size_t nameStart = at + std::string_view("MaterialPreview::").size();
            const std::size_t paren = line.find('(', nameStart);
            owner = line.substr(nameStart, paren == std::string::npos ? std::string::npos : paren - nameStart);
            break;
        }
        return owner;
    };

    // The functions a CACHE destroy may appear in. THE DESTRUCTOR IS PERMITTED and the draw path is
    // not: ~MaterialPreview runs inside ~PanelRegistry inside ~EditorApp, which precedes ~Device, and it
    // is not a frame at all. `destroyOrphans` is permitted because it is private and service() is its
    // one caller -- which this list is the record of, since nothing else in the tree can state it.
    const std::array<std::string_view, 3> destroyPermitted{"~MaterialPreview", "service", "destroyOrphans"};
    std::size_t destroySites = 0;
    for (std::size_t i = 0; i < code.size(); ++i) {
        const bool destroys = code[i].find("destroyTexture(") != std::string::npos ||
                              code[i].find("destroyMaterial(") != std::string::npos;
        if (!destroys) {
            continue;
        }
        ++destroySites;
        const std::string owner = ownerOf(i);
        CAPTURE(i);
        CAPTURE(owner);
        CHECK(std::find(destroyPermitted.begin(), destroyPermitted.end(), std::string_view(owner)) !=
              destroyPermitted.end());
    }
    // ANTI-VACUITY: a rename of the destroy calls would make the loop above find nothing and pass
    // saying nothing at all.
    CHECK(destroySites >= 1);

    // THE NEEDLE THE OLD PIN LACKED. `resize(` releases the colour texture ImGui may be holding, so it
    // may appear in ONE function only: prepareFrame, which the draw walk calls and which reads the
    // handle immediately afterwards. In service(), renderFrame() or any helper they call, the release
    // lands between ImGui recording the id and ImGui binding it.
    std::size_t resizeSites = 0;
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i].find("->resize(") == std::string::npos) {
            continue;
        }
        ++resizeSites;
        const std::string owner = ownerOf(i);
        CAPTURE(i);
        CAPTURE(owner);
        CHECK(owner == "prepareFrame");
    }
    // TWO since task 3.6.3, and BOTH in prepareFrame: `post->resize(pixels)` sits on the line
    // immediately above `target->resize(pixels)`, which is what makes the tonemap resolve's 1:1 blit
    // true by construction rather than by two call sites that could drift. The per-site ownership
    // CHECK above is the invariant and is unchanged -- this count is the anti-vacuity guard, and it
    // stays EXACT so a third resize appearing anywhere reddens here. I106(c) pins the adjacency.
    CHECK(resizeSites == 2);

    // The ImGui TU touches no GPU lifetime AT ALL beyond that one call, and the ORDER of its three
    // preview statements is the property this whole case is about: settle the allocation, THEN read the
    // handle, THEN hand it to ImGui. soleLineContaining REQUIREs exactly one of each, so a second
    // Image, a second read or a second prepare reddens here too.
    const std::vector<std::string> panelCode = editorSourceCodeLines(AERO_EDITOR_SRC_DIR "/material_panel.cpp");
    const std::size_t prepareAt = soleLineContaining(panelCode, "prepareFrame(");
    const std::size_t handleAt = soleLineContaining(panelCode, "nativeColorTexture(");
    const std::size_t imageAt = soleLineContaining(panelCode, "ImGui::Image(");
    CHECK(prepareAt < handleAt);
    CHECK(handleAt < imageAt);

    const std::array<std::string_view, 8> forbiddenInDrawWalk{"destroyTexture(",
                                                              "destroyMaterial(",
                                                              "createMaterial(",
                                                              "updateMaterial(",
                                                              "RenderTarget::create",
                                                              "ForwardRenderer::create",
                                                              "createTextureFromCookedTexture(",
                                                              "beginFrame("};
    for (const std::string& line : panelCode) {
        for (const std::string_view needle : forbiddenInDrawWalk) {
            CAPTURE(needle);
            CHECK(line.find(needle) == std::string::npos);
        }
    }
}

TEST_CASE("editor: .aeromat bytes have exactly ONE physical write path (task 3.4.2, I97, D12, seed S8)") {
    // THE AMENDED INV-A1, as a grep rather than a habit. Apply and New Material are TWO LOGICAL writes
    // through ONE physical one: material_session.cpp's saveMaterialFile holds the single
    // writeTextFileAtomic call site, and every caller assembles the absolute path into a named local
    // first so the invariant stays decidable by reading rather than by reasoning. Duplicating the
    // helper into a second call site (seed S8) reddens here and nowhere else -- no runtime tier can
    // see a second write path that happens to write the same bytes.
    const std::vector<std::string> sessionCode = editorSourceCodeLines(AERO_EDITOR_SRC_DIR "/material_session.cpp");
    std::size_t writeSites = 0;
    for (const std::string& line : sessionCode) {
        if (line.find("writeTextFileAtomic(") != std::string::npos) {
            ++writeSites;
        }
    }
    CHECK(writeSites == 1);

    // And nowhere else in this task's four TUs. editor_app.cpp is deliberately NOT on this list: it is
    // full of unrelated writes (recents, project manifests) and its material arm is asserted the other
    // way round, below.
    // asset_browser_panel.cpp is on this list for a reason the S21 run made concrete: moving the whole
    // create into its applyPending arm did redden this case, but only through the `soleLineContaining`
    // REQUIRE above -- an ABSENCE in editor_app.cpp. A seed that ADDED a panel-side write while leaving
    // the drain in place would have slipped past, and that panel is read-only by contract (D19).
    const std::array<std::string_view, 4> mustNotWrite{"/material_panel.cpp", "/material_preview.cpp",
                                                       "/material_edit.cpp", "/asset_browser_panel.cpp"};
    for (const std::string_view leaf : mustNotWrite) {
        CAPTURE(leaf);
        std::string path = AERO_EDITOR_SRC_DIR;
        path += leaf;
        for (const std::string& line : editorSourceCodeLines(path)) {
            CHECK(line.find("writeTextFileAtomic(") == std::string::npos);
            CHECK(line.find("saveMaterialFile(") == std::string::npos);
        }
    }

    // The create drain routes through the SAME helper, exactly once, and writes no bytes of its own:
    // one saveMaterialFile call, zero writeMaterialText calls (the helper owns the serialisation) and
    // -- the load-bearing half -- the named local the invariant is read from.
    const std::vector<std::string> appCode = editorSourceCodeLines(AERO_EDITOR_SRC_DIR "/editor_app.cpp");
    (void)soleLineContaining(appCode, "saveMaterialFile(");
    (void)soleLineContaining(appCode, "const std::string materialAbsolutePath =");
    for (const std::string& line : appCode) {
        CHECK(line.find("writeMaterialText(") == std::string::npos);
    }
}

TEST_CASE("editor: every numeric material edit is CLAMPED IN C++, not by the widget (task 3.4.2, I98, AC-18)") {
    // THE CLOSURE FOR A MEASURED GAP. Seed S18 removed all twelve clamp calls from the Material
    // panel's scalar rows -- "trust the widget" -- and every one of the 1570 shell cases and 119 GPU
    // cases stayed green. That is structural, not an oversight in the cases: no tier in this tree can
    // Ctrl+Click an ImGui slider and type 40 into a [0,1] field, which is precisely the input the
    // clamp exists for. AC-18 says the widget is NEVER the enforcement, and this is the only place
    // that sentence can be checked at all.
    //
    // Apply's validateMaterial is the BELT (ME41 proves it), so the damage a missing clamp does is not
    // corruption -- it is an Apply button disabled by a value the user cannot see is out of range.
    const std::vector<std::string> code = editorSourceCodeLines(AERO_EDITOR_SRC_DIR "/material_panel.cpp");

    // The numeric fields docs/09 section 11.1 bounds. `name` and `doubleSided` are deliberately absent:
    // neither has a range, so neither is clamped, and listing them would demand a clamp that means
    // nothing.
    const std::array<std::string_view, 8> boundedFields{
        "form.baseColorFactor.", "form.metallicFactor",    "form.roughnessFactor", "form.emissiveFactor.",
        "form.normalScale",      "form.occlusionStrength", "form.alphaCutoff",     "slot->uvSet"};

    std::size_t assignments = 0;
    for (const std::string& line : code) {
        const std::size_t assignAt = line.find(" = ");
        if (assignAt == std::string::npos) {
            continue;
        }
        for (const std::string_view field : boundedFields) {
            const std::size_t fieldAt = line.find(field);
            // The field must be on the LEFT of the `=` for this to be a WRITE. `float metallic =
            // form.metallicFactor;` reads it and is not this case's business.
            if (fieldAt == std::string::npos || fieldAt > assignAt) {
                continue;
            }
            ++assignments;
            CAPTURE(field);
            CAPTURE(line);
            CHECK(line.find("clamp") != std::string::npos);
        }
    }
    // ANTI-VACUITY, and the number is deliberately a floor rather than an equality: a future row adds
    // an assignment, it does not remove one. Twelve is what the panel ships today.
    CHECK(assignments >= 12U);
}

// ---- task 3.4.2, the code-review round's closures (I99-I101) --------------------------------------

TEST_CASE("editor: a RESIZED Material panel never binds a released preview texture (task 3.4.2, I99)") {
    // THE CODE-REVIEW ROUND'S BLOCKING-1, AND THE COVERAGE GAP THAT LET IT SHIP GREEN. No case in this
    // suite ever changed the preview's requested extent, so RenderTarget::resize never reallocated
    // anywhere -- which is precisely why a resize sitting in the post-draw service pass, AFTER ImGui had
    // already recorded the old colour texture into this frame's draw list and BEFORE ImGuiLayer::endFrame
    // bound it, was invisible to all 133 tests.
    //
    // IT IS ALSO INVISIBLE TO ASan HERE, WHICH IS WHY THIS CASE COUNTS RATHER THAN CRASHES. SDL frees the
    // texture CONTAINER immediately on Vulkan and D3D12 and merely queues it on Metal, so the dangling
    // pointer is real on the Ubuntu and Windows lanes and harmless on this one. materialPreviewStaleImageCount()
    // is the deterministic, platform-independent witness: it counts frames whose handed-out colour
    // texture was no longer the target's live one by the time the next draw walk began.
    //
    // THE EXTENT IS DRIVEN BY A WINDOW RESIZE **PLUS** A LAYOUT RESET, and the second half is not
    // ceremony -- it was measured. Resizing the window alone moves the viewport and the centre node but
    // leaves the Right dock column at its absolute width (64 px of preview at 320x180 AND at 800x500,
    // observed directly while writing this case), so a window-only case reallocates nothing and proves
    // nothing. View > Reset Layout re-derives the dock split from the new work area, which is what
    // actually widens the column. Both reallocation branches are then exercised: the grow, and the
    // shrink, which only reallocates once the need at least halves (nextTargetExtent's hysteresis).
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i99", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/resize.aeromat", MINIMAL_AEROMAT_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    app->panels().setVisible("Inspector", false);  // so Material wins the Right dock tab and onDraw runs
    REQUIRE(app->tick());
    app->requestAssetBrowserSelectEntry("resize.aeromat");
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->materialTargetPath() == "resize.aeromat");

#if AERO_SHADER_TOOLS_ENABLED
    REQUIRE(app->materialPreviewAvailable());
    const std::uint32_t narrowWidth = app->materialPreviewTextureWidth();
    const std::size_t imagesBeforeGrow = app->materialPreviewImageCount();
    REQUIRE(narrowWidth > 0U);
    REQUIRE(imagesBeforeGrow > 0U);  // the panel really did hand ImGui a texture before the resize
    CHECK(app->materialPreviewStaleImageCount() == 0U);

    // --- grow: the dock column widens, the request crosses the 64-pixel quantum, allocate() runs ---
    window->setSize(800, 500);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->materialPreviewStaleImageCount() == 0U);
    }
    app->requestLayoutReset();
    for (int i = 0; i < 6; ++i) {
        REQUIRE(app->tick());
        CHECK(app->materialPreviewStaleImageCount() == 0U);
    }
    const std::uint32_t wideWidth = app->materialPreviewTextureWidth();
    CAPTURE(narrowWidth);
    CAPTURE(wideWidth);
    // NON-VACUITY, and it is a REQUIRE rather than a CHECK on purpose: textureExtent IS the allocation,
    // so an unchanged value means this case reallocated nothing and proved nothing. A window manager
    // that refuses the resize must fail loudly rather than pass silently.
    REQUIRE(wideWidth != narrowWidth);
    CHECK(app->materialPreviewImageCount() > imagesBeforeGrow);

    // --- shrink: back under half the allocation, so the hysteresis branch reallocates too ----------
    window->setSize(320, 180);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
        CHECK(app->materialPreviewStaleImageCount() == 0U);
    }
    app->requestLayoutReset();
    for (int i = 0; i < 6; ++i) {
        REQUIRE(app->tick());
        CHECK(app->materialPreviewStaleImageCount() == 0U);
    }
    const std::uint32_t shrunkWidth = app->materialPreviewTextureWidth();
    CAPTURE(shrunkWidth);
    CHECK(shrunkWidth < wideWidth);
    CHECK(app->materialPreviewFrameCount() >= 3U);
    CHECK(app->materialPreviewStaleImageCount() == 0U);
#else
    // -DAERO_SHADER_TOOLS=OFF (AC-32): there is no target to reallocate, so the ordering this case
    // exists to prove is not merely unobserved, it cannot occur. The OFF contract is asserted instead --
    // the window still resizes, the editor still ticks, and the preview still costs exactly nothing.
    CHECK_FALSE(app->materialPreviewAvailable());
    CHECK(app->materialPreviewTextureWidth() == 0U);
    window->setSize(800, 500);
    for (int i = 0; i < 6; ++i) {
        REQUIRE(app->tick());
    }
    window->setSize(320, 180);
    for (int i = 0; i < 6; ++i) {
        REQUIRE(app->tick());
    }
    CHECK(app->materialPreviewImageCount() == 0U);
    CHECK(app->materialPreviewStaleImageCount() == 0U);
#endif

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the error state shows NO picture, not the last material's (task 3.4.2, I100)") {
    // THE CODE-REVIEW ROUND'S FINDING 9. A render target keeps whatever was last rendered into it, and
    // MaterialPreview::service refuses to render with no document -- so drawing the preview strip's
    // ImGui::Image under an error message blits a stale frame OF A DIFFERENT MATERIAL and presents it as
    // if it belonged to the file that failed to parse. Selecting a good material first is what makes the
    // case discriminating: with nothing rendered beforehand there is no stale picture to show.
    //
    // materialPreviewImageCount() is the observable, and it is the only one there can be: a correct
    // picture and a stale one are the same bytes to every tier in this tree.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i100", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    const std::string assetsRoot = created.root + "/assets";
    REQUIRE(engine::editor::writeTextFileAtomic(assetsRoot + "/good.aeromat", MINIMAL_AEROMAT_TEXT).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(assetsRoot + "/bad.aeromat", REJECT_AEROMAT_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    app->panels().setVisible("Inspector", false);
    REQUIRE(app->tick());
    app->requestAssetBrowserSelectEntry("good.aeromat");
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->materialTargetPath() == "good.aeromat");
    const std::size_t imagesAfterGood = app->materialPreviewImageCount();
#if AERO_SHADER_TOOLS_ENABLED
    REQUIRE(app->materialPreviewAvailable());
    REQUIRE(imagesAfterGood > 0U);  // a real picture existed before the error state was entered
    REQUIRE(app->materialPreviewFrameCount() >= 1U);
#else
    REQUIRE(imagesAfterGood == 0U);  // AC-32: no target, so no picture to go stale in the first place
#endif

    app->requestAssetBrowserSelectEntry("bad.aeromat");
    for (int i = 0; i < 5; ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->materialTargetPath() == "bad.aeromat");
    REQUIRE_FALSE(app->materialParseOk());
    // The count is snapshotted HERE, once the error state is on screen, rather than before the request:
    // the browser applies a selection inside its own draw walk, while tick()'s reconcile has already
    // run, so exactly one further frame legitimately draws the STILL-TARGETED good material. That frame
    // is correct; every frame after it is the defect.
    const std::size_t imagesInError = app->materialPreviewImageCount();
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    // THE ASSERTION: not one further frame handed ImGui the target's texture while the error was on
    // screen. Before the fix this climbed by one per tick, blitting `good.aeromat`'s last frame under
    // `bad.aeromat`'s parse error.
    CHECK(app->materialPreviewImageCount() == imagesInError);
    CHECK(app->materialPreviewStaleImageCount() == 0U);
    CHECK(app->materialTargetPath() == "bad.aeromat");

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a non-zero uvSet latches ONE preview WARN (task 3.4.2, I101, AC-22)") {
    // AC-22 asks for a latched WARN "exactly as the sample does", and until the code-review round there
    // was none: the panel drew an inline note and nothing logged. Nothing downstream COULD log it --
    // render::MaterialTextureSlot carries no uvSet field at all -- so the preview is the only place the
    // sentence can come from, which is why it lives beside the push rather than in the renderer.
    //
    // The count, not a bool, is what makes LATCHED assertable: an unlatched implementation climbs past
    // one as further edits re-push the document.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "material i101", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    REQUIRE(engine::editor::writeTextFileAtomic(created.root + "/assets/uv.aeromat", MINIMAL_AEROMAT_TEXT).empty());

    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    app->panels().setVisible("Inspector", false);
    REQUIRE(app->tick());
    app->requestAssetBrowserSelectEntry("uv.aeromat");
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->materialTargetPath() == "uv.aeromat");
    REQUIRE(app->materialDocument() != nullptr);
    // A material with NO bound slot says nothing at all -- the WARN is about a bound slot's uvSet, never
    // about the field's mere existence.
    CHECK(app->materialPreviewUvSetWarnCount() == 0U);

    engine::MaterialDocument withUvSet = *app->materialDocument();
    engine::MaterialTextureSlot slot;
    // A GUID no asset in this project owns, deliberately. The WARN is about what the SLOT DECLARES, not
    // about a texture that loaded -- exactly as the sample's own resolveSlot warns before it resolves.
    slot.guid = engine::Guid{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    slot.uvSet = 1;
    withUvSet.baseColor = slot;
    app->requestMaterialDocument(withUvSet);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->materialDocument() != nullptr);
    REQUIRE(app->materialDocument()->baseColor.has_value());
#if AERO_SHADER_TOOLS_ENABLED
    CHECK(app->materialPreviewUvSetWarnCount() == 1U);
#else
    // AC-32: no renderer, so no push, so no WARN. The edit still reached the session identically.
    CHECK(app->materialPreviewUvSetWarnCount() == 0U);
#endif

    // Further edits re-push the document; the latch must hold across every one of them.
    for (int round = 0; round < 3; ++round) {
        engine::MaterialDocument nudged = *app->materialDocument();
        nudged.roughnessFactor = 0.1F + (0.2F * static_cast<float>(round));
        app->requestMaterialDocument(nudged);
        REQUIRE(app->tick());
        REQUIRE(app->tick());
    }
#if AERO_SHADER_TOOLS_ENABLED
    CHECK(app->materialPreviewUvSetWarnCount() == 1U);
#else
    CHECK(app->materialPreviewUvSetWarnCount() == 0U);
#endif

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: New Material refuses a directory it could not enumerate IN FULL (task 3.4.2, I102)") {
    // THE CODE-REVIEW ROUND'S BLOCKING-2, pinned where it can be. The drain's own comment already states
    // the rule -- "a directory we cannot enumerate is a directory in which we cannot prove a name is
    // unused. Refusing is the only safe answer" -- and the code enforced one third of it: `status != Ok`
    // alone, while listDirectory ALSO signals incompleteness through `truncated` (over
    // MAX_ENTRIES_PER_DIRECTORY / MAX_ENTRIES_EXAMINED) and `skipped` (an increment(ec) failure part way
    // through -- the antivirus-lock and cloud-sync case 3.1.4's D5 records as real). Both keep the
    // status at Ok and return a PREFIX, so uniqueMaterialFileName hands back a name that already exists
    // and the canonical default document is written over an authored material, with no warning and no
    // undo (D4 keeps materials off the CommandStack).
    //
    // NO RUNTIME TIER HERE CAN REACH EITHER ARM: one needs 10 001 files in a directory inside a GPU-tier
    // frame budget, the other needs the OS to fail an iterator mid-walk on demand. The predicate itself
    // is proven exhaustively at tier 0 (project_files_test.cpp PF-c6/PF-c7); this pins that the CALL
    // SITE asks it rather than re-deriving one third of it inline.
    const std::vector<std::string> appCode = editorSourceCodeLines(AERO_EDITOR_SRC_DIR "/editor_app.cpp");
    (void)soleLineContaining(appCode, "listingIsComplete(listing)");
    for (const std::string& line : appCode) {
        // The regressed shape this proof must reject, in either spelling.
        CHECK(line.find("listing.status != ScanStatus::Ok") == std::string::npos);
        CHECK(line.find("listing.status == ScanStatus::Ok") == std::string::npos);
    }
}

// ---- task 3.6.3, the tonemap/gamma pass (I103-I106) -----------------------------------------------

TEST_CASE("editor: the viewport draws through an HDR pass into a depth-free output (task 3.6.3, I103)") {
    // The whole structural claim of this task's editor half, in one place: the SceneRenderer now draws
    // into an RGBA16Float target that PostProcess owns, and the target ImGui samples has lost its depth
    // attachment because the only thing recorded into it is a depth-off fullscreen triangle.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "tonemap i103", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());

    REQUIRE(app->tick());  // the viewport's ensureInitialized runs on its FIRST DRAW, not at create
    REQUIRE(app->tick());

    auto* const viewport = dynamic_cast<engine::editor::ViewportPanel*>(app->panels().find("Viewport"));
    REQUIRE(viewport != nullptr);

#if AERO_SHADER_TOOLS_ENABLED
    const engine::render::PostProcess* const post = viewport->postProcess();
    REQUIRE(post != nullptr);
    // The HDR intermediate, and the depth the forward pass still needs -- on the SCENE target.
    CHECK((post->sceneColorFormat() == engine::rhi::TextureFormat::RGBA16Float));
    CHECK((post->sceneDepthFormat() != engine::rhi::TextureFormat::Invalid));
    CHECK(post->sceneDrawExtent().width > 0);
    CHECK(post->sceneDrawExtent().height > 0);
    // ...and NOT on the output target, which nothing depth-tests into any more. The two targets'
    // depth formats are the ONE pair a runtime tier can compare, and the contrast is the assertion:
    // reading Invalid on both would mean the forward pass had lost its depth buffer.
    const engine::render::RenderTarget* const output = viewport->outputTarget();
    REQUIRE(output != nullptr);
    CHECK((output->depthFormat() == engine::rhi::TextureFormat::Invalid));
    CHECK((output->colorFormat() == engine::rhi::TextureFormat::RGBA8Unorm));
    // Non-vacuity: a pass that exists but never resolved would satisfy every format assertion above.
    CHECK(post->resolveCount() >= 1);
    CHECK_FALSE(post->hasWarnedResolveBeforeEndScene());
#else
    // -DAERO_SHADER_TOOLS=OFF: the viewport latches Unavailable before anything is created, so the pass
    // is not merely unproven here, it CANNOT exist. Asserted rather than skipped (the 3.4.2 near-miss
    // rule) -- and tonemapParams() must STILL answer, which is I105's second arm.
    CHECK(viewport->postProcess() == nullptr);
#endif
}

TEST_CASE("editor: two viewport frames at different sizes resolve without a stretch (task 3.6.3, I104)") {
    // ONE SEQUENCE CASE, deliberately. `hasWarnedExtentMismatch() == false` alone is satisfied by a pass
    // that never resolved at all, and `resolveCount() == 2` alone is satisfied by a pass that resolved
    // twice into the wrong extent. Both halves have to hold in the SAME run, which is what makes this
    // the witness for the adjacency of the two resize calls.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "tonemap i104", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());

    REQUIRE(app->tick());
    // A DIFFERENT window size between the two drawn frames: the dock's content region follows it, so
    // both targets are asked for a new extent on the same line of onDraw's step 5.
    window->setSize(480, 300);
    REQUIRE(app->tick());
    REQUIRE(app->tick());

    auto* const viewport = dynamic_cast<engine::editor::ViewportPanel*>(app->panels().find("Viewport"));
    REQUIRE(viewport != nullptr);

#if AERO_SHADER_TOOLS_ENABLED
    const engine::render::PostProcess* const post = viewport->postProcess();
    REQUIRE(post != nullptr);
    CHECK(post->resolveCount() >= 2);              // it DREW, more than once
    CHECK_FALSE(post->hasWarnedExtentMismatch());  // ...at matching extents, every time
    CHECK_FALSE(post->hasWarnedResolveBeforeEndScene());
#else
    CHECK(viewport->postProcess() == nullptr);
#endif
}

TEST_CASE("editor: the viewport's tonemap params are sanitized on write and valid always (task 3.6.3, I105)") {
    // No tier in this tree can move an ImGui slider, so requestTonemapParams is the seam that makes the
    // clamp drivable at all -- and it calls the SAME sanitize the combo and the slider call, which is
    // what makes it a real witness rather than a second policy. Seed T28 must therefore break BOTH
    // sites to be honest about what this case sees.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "tonemap i105", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }

    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = created.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());

    auto* const viewport = dynamic_cast<engine::editor::ViewportPanel*>(app->panels().find("Viewport"));
    REQUIRE(viewport != nullptr);

    // BEFORE any frame at all: the member is default-constructed and already sanitized, which is what
    // makes EditorApp::tick's forward safe on the very first tick.
    CHECK(viewport->tonemapParams().exposure == 1.0F);
    CHECK(viewport->tonemapParams().curve == engine::render::TonemapOperator::AcesApprox);

    REQUIRE(app->tick());

    // An out-of-range exposure comes back CLAMPED, not stored raw. A slider with a v_min still lets a
    // Ctrl+Click type anything at all.
    viewport->requestTonemapParams({1.0e9F, engine::render::TonemapOperator::Reinhard});
    CHECK(viewport->tonemapParams().exposure == engine::render::MAX_EXPOSURE);
    CHECK(viewport->tonemapParams().curve == engine::render::TonemapOperator::Reinhard);

    viewport->requestTonemapParams({-4.0F, engine::render::TonemapOperator::None});
    CHECK(viewport->tonemapParams().exposure == engine::render::MIN_EXPOSURE);
    CHECK(viewport->tonemapParams().curve == engine::render::TonemapOperator::None);

    // An out-of-range CURVE falls back to the default rather than reaching a uniform as a raw integer
    // the fragment stage's chained ternary has no arm for.
    viewport->requestTonemapParams({2.0F, static_cast<engine::render::TonemapOperator>(200)});
    CHECK(viewport->tonemapParams().exposure == 2.0F);
    CHECK(viewport->tonemapParams().curve == engine::render::TonemapOperator::AcesApprox);

    // An in-range value survives BIT-IDENTICAL: sanitizing is a clamp, not a quantisation.
    viewport->requestTonemapParams({0.25F, engine::render::TonemapOperator::Reinhard});
    CHECK(viewport->tonemapParams().exposure == 0.25F);

// BOTH ARMS ASSERT, including the -DAERO_SHADER_TOOLS=OFF one, because the degradation path is the
// whole point: with no cooked shaders the viewport is Unavailable and has no PostProcess at all,
// and the params must STILL be a valid sanitized value, since EditorApp::tick forwards them to the
// preview unconditionally. A skip would leave that untested in the one configuration that can test
// it.
#if AERO_SHADER_TOOLS_ENABLED
    CHECK(viewport->postProcess() != nullptr);
#else
    CHECK(viewport->postProcess() == nullptr);
    CHECK(viewport->tonemapParams().exposure == 0.25F);  // unaffected by the panel being Unavailable
#endif
}

TEST_CASE("editor: the tonemap wiring's three source-text invariants hold (task 3.6.3, I106)") {
    // A SOURCE-TEXT PIN, the I95/I96 shape, because none of the three has a runtime witness anywhere in
    // this tree: nothing here reads a pixel, so two consumers grading differently is invisible; nothing
    // here can move an ImGui widget, so a greyed-out control is invisible; and nothing here can observe
    // one target resized without the other except through an extent latch that a matched pair never
    // trips. Comment-stripped, so prose about any of the three cannot satisfy it.
    const std::vector<std::string> appCode = editorSourceCodeLines(AERO_EDITOR_SRC_DIR "/editor_app.cpp");
    const std::vector<std::string> viewportCode = editorSourceCodeLines(AERO_EDITOR_SRC_DIR "/viewport_panel.cpp");
    const std::vector<std::string> previewCode = editorSourceCodeLines(AERO_EDITOR_SRC_DIR "/material_preview.cpp");

    // ANTI-VACUITY, BOTH DIRECTIONS. The scan found real text, and a needle that must be ABSENT is
    // absent -- without the second half a `find` that always succeeded would satisfy every clause here.
    REQUIRE_FALSE(appCode.empty());
    REQUIRE_FALSE(viewportCode.empty());
    REQUIRE_FALSE(previewCode.empty());
    const auto containsAnywhere = [](const std::vector<std::string>& code, std::string_view needle) {
        return std::any_of(code.begin(), code.end(),
                           [needle](const std::string& line) { return line.find(needle) != std::string::npos; });
    };
    CHECK_FALSE(containsAnywhere(viewportCode, "gammaEnabled"));
    CHECK_FALSE(containsAnywhere(viewportCode, "applyOetf"));

    SUBCASE("(a) EditorApp forwards the VIEWPORT's params into the preview") {
        // The T29 witness. Passing a default here instead would make the preview grade with ACES/1.0
        // while the viewport graded with whatever was chosen, and nothing at runtime could see it.
        const std::size_t serviceAt = soleLineContaining(appCode, "servicePreview(");
        bool forwardsViewportParams = false;
        for (std::size_t i = serviceAt; i < appCode.size() && i < serviceAt + 4U; ++i) {
            if (appCode[i].find("viewportPanel->tonemapParams()") != std::string::npos) {
                forwardsViewportParams = true;
            }
        }
        CHECK(forwardsViewportParams);
    }

    SUBCASE("(b) drawViewOptions is a SIBLING of drawGizmoBar and opens no disabled scope") {
        // A PARTIAL PIN, and it is recorded as one. The property that actually matters -- "the call is
        // outside drawGizmoBar's BeginDisabled scope" -- is not decidable from source text at the call
        // site, because the scope lives inside another function. What IS decidable is that
        // drawViewOptions is called AFTER drawGizmoBar rather than from inside it, and that its own
        // body opens no BeginDisabled. Validation row 2 (the controls are live with nothing selected)
        // is the real witness; claiming otherwise here would be a pin certifying what it is blind to.
        const std::size_t barCallAt = soleLineContaining(viewportCode, "drawGizmoBar();");
        const std::size_t optionsCallAt = soleLineContaining(viewportCode, "drawViewOptions();");
        CHECK(barCallAt < optionsCallAt);

        // The body: from `void ViewportPanel::drawViewOptions() {` to the next column-0 `}`.
        const std::size_t bodyStart = soleLineContaining(viewportCode, "void ViewportPanel::drawViewOptions()");
        std::size_t bodyEnd = viewportCode.size();
        for (std::size_t i = bodyStart + 1U; i < viewportCode.size(); ++i) {
            if (viewportCode[i].starts_with('}')) {
                bodyEnd = i;
                break;
            }
        }
        REQUIRE(bodyEnd > bodyStart);
        REQUIRE(bodyEnd < viewportCode.size());  // the body was actually delimited, not run off the end
        bool opensDisabled = false;
        bool pushesId = false;
        bool popsId = false;
        for (std::size_t i = bodyStart; i <= bodyEnd; ++i) {
            opensDisabled = opensDisabled || viewportCode[i].find("BeginDisabled") != std::string::npos;
            pushesId = pushesId || viewportCode[i].find("ImGui::PushID(") != std::string::npos;
            popsId = popsId || viewportCode[i].find("ImGui::PopID(") != std::string::npos;
        }
        CHECK_FALSE(opensDisabled);
        CHECK(pushesId);  // 1:1 with the pop below -- the editor rules' balance requirement
        CHECK(popsId);
    }

    SUBCASE("(c) both consumers resize the HDR target on the line IMMEDIATELY above the output's") {
        // The T26/T27 adjacency seed. Two resizes driven by the same value on adjacent lines is what
        // makes the resolve's 1:1 blit true BY CONSTRUCTION; two call sites a few statements apart
        // would work today and drift the first time either is edited.
        for (const std::vector<std::string>* code : {&viewportCode, &previewCode}) {
            const std::size_t postAt = soleLineContaining(*code, "post->resize(");
            const std::size_t targetAt = soleLineContaining(*code, "target->resize(");
            CHECK(postAt + 1U == targetAt);
        }
    }
}

// ================================================================================================
// task 3.1.5 (SL1-SL10) -- the scene-asset loader, end to end against a real device.
//
// COMPILED ONLY WHERE THE SHADER TOOLCHAIN BUILT THE ARTIFACTS ForwardRenderer::create LOADS. That is
// scene_render_bindings_test.cpp's own tier-1 posture and it is NOT the preview's: the tools-OFF arm
// of I88-I92 asserts a real, different claim (targeting, editing and Apply all work with no picture),
// whereas SceneAssetLoader takes a render::ForwardRenderer BY REFERENCE on every entry point and a
// build with no cooked shaders can construct none -- there is no second contract to state.
//
// NO WINDOW: a RenderTarget supplies both formats, exactly as BR21/BR22 do. These cases never build an
// EditorApp -- the tick wiring is step 16's, and asserting the loader through it would test two things
// at once.
// ================================================================================================
#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/core/vfs.hpp>
    #include <aero/render/render.hpp>

namespace {

// TWO meshes, ONE material, positions-only. Both primitives share one attribute mask, so the cook
// groups them into ONE section with TWO submeshes carrying sourceMeshIndex 0 and 1 -- the shape SL2
// and SL10 need. Mesh A's box is (0,0,0)..(1,1,0) and mesh B's is (0,0,0)..(2,2,0), deliberately
// different so a fold that ignored sourceMeshIndex would produce one box and redden SL10. Only mesh
// A's primitive names the material, so mesh B's submesh carries COOKED_INVALID_MATERIAL (SL4).
//
// The buffer is 84 bytes: positions A at 0, positions B at 36, indices A at 72, indices B at 78 --
// every offset a multiple of its own component size, as glTF requires.
constexpr std::string_view SL_TWO_MESH_GLTF_TEXT =
    R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],)"
    R"("nodes":[{"name":"A","mesh":0},{"name":"B","mesh":1}],)"
    R"("meshes":[{"name":"MeshA","primitives":[{"attributes":{"POSITION":0},"indices":2,"mode":4,"material":0}]},)"
    R"({"name":"MeshB","primitives":[{"attributes":{"POSITION":1},"indices":3,"mode":4}]}],)"
    R"("materials":[{"name":"Painted","pbrMetallicRoughness":{"baseColorFactor":[0.5,0.25,0.125,1.0],)"
    R"("baseColorTexture":{"index":0},"metallicFactor":0.25,"roughnessFactor":0.75}}],)"
    R"("textures":[{"source":0}],"images":[{"uri":"wood.png"}],)"
    R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
    R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[2,2,0]},)"
    R"({"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"},)"
    R"({"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],)"
    R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},)"
    R"({"buffer":0,"byteOffset":36,"byteLength":36,"target":34962},)"
    R"({"buffer":0,"byteOffset":72,"byteLength":6,"target":34963},)"
    R"({"buffer":0,"byteOffset":78,"byteLength":6,"target":34963}],)"
    R"("buffers":[{"byteLength":84,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAA)"
    R"(AAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAEAAAAAAAAABAAIAAAABAAIA"}]})";

// The loader's whole world, built once per case: a real project with a scanned AssetDatabase beside
// it. Never removed proactively -- the OS temp directory is the OS's to reclaim, exactly as
// uniqueProjectLocation's own note records.
struct LoaderProject {
    std::string root;
    std::string assetsRoot;
    engine::GuidGenerator generator{0x51AD5EEDULL};
    engine::editor::AssetDatabase database;

    [[nodiscard]] const engine::editor::AssetRecord& record(std::string_view relativePath) const {
        const engine::editor::AssetRecord* const found = database.findByPath(relativePath);
        REQUIRE(found != nullptr);
        return *found;
    }
};

[[nodiscard]] std::unique_ptr<LoaderProject> makeLoaderProject() {
    auto project = std::make_unique<LoaderProject>();
    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    project->root = created.root;
    project->assetsRoot = created.root + "/assets";
    return project;
}

void scanLoaderProject(LoaderProject& project) {
    const engine::editor::AssetScanReport report =
        project.database.rescan(project.root, project.assetsRoot, project.generator);
    REQUIRE(report.status == engine::editor::ScanStatus::Ok);
}

}  // namespace

TEST_CASE("editor: the loader takes a model from disk to a GPU mesh (task 3.1.5, SL1/SL2/SL3/SL4/SL10)") {
    // ONE case for five of the ten, because they are five assertions about ONE load and splitting them
    // would pay for five imports, five cooks and five uploads to observe one result. SL5's ASan arm is
    // its own case for the opposite reason: what it proves is a lifetime, and a case that also asserted
    // something else would leave "which half aborted" ambiguous.
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(*device, {64, 64});
    REQUIRE(target.has_value());
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    std::optional<engine::render::ForwardRenderer> renderer = engine::render::ForwardRenderer::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(renderer.has_value());

    const std::unique_ptr<LoaderProject> project = makeLoaderProject();
    REQUIRE(engine::editor::writeTextFileAtomic(project->assetsRoot + "/two.gltf", SL_TWO_MESH_GLTF_TEXT).empty());
    REQUIRE(writeBinaryFixture(project->assetsRoot + "/wood.png", TINY_PNG_RED.data(), TINY_PNG_RED.size()).empty());
    scanLoaderProject(*project);

    engine::editor::SceneAssetLoader loader(*device);
    CHECK(loader.importCount() == 0);
    CHECK(loader.meshUploadCount() == 0);
    const engine::editor::SceneAssetLoader::ModelLoadResult loaded =
        loader.loadModel(project->record("two.gltf"), project->assetsRoot, project->root, project->database, *renderer);

    // SL1 -- end to end, a valid MeshHandle.
    INFO("loader message: " << loaded.message);
    REQUIRE(loaded.ok);
    CHECK(loaded.message.empty());
    REQUIRE(loaded.handles.mesh.valid());
    // TWO importModel calls, and that is the two-pass driver rather than a repeat: glTF answers TRUE to
    // modelImporterNeedsExternalBuffers, so pass 1 runs at Structure depth to learn the URI set even
    // for a document whose buffers are all data: URIs. ONE createMesh, counted at the CALL rather than
    // at success, so a refusal reads as an upload that produced nothing.
    CHECK(loader.importCount() == 2);
    CHECK(loader.meshUploadCount() == 1);

    // SL3 -- the submesh count the registry recorded equals the cooked table's.
    CHECK(renderer->meshSubmeshCount(loaded.handles.mesh) == 2U);
    REQUIRE(loaded.binding.submeshes.size() == 2);
    CHECK((loaded.binding.mesh == loaded.handles.mesh));

    // SL2 -- the binding's sourceMeshIndex values match the cooked table, in cooked submesh order.
    CHECK(loaded.binding.submeshes[0].submesh == 0U);
    CHECK(loaded.binding.submeshes[1].submesh == 1U);
    CHECK(loaded.binding.submeshes[0].sourceMeshIndex == 0U);
    CHECK(loaded.binding.submeshes[1].sourceMeshIndex == 1U);

    // SL4 -- mesh A's primitive names material 0 and resolves; mesh B's names none, so its submesh
    // carries COOKED_INVALID_MATERIAL and yields an INVALID handle. That is the renderer-default path,
    // not an error, and nothing about it is logged.
    REQUIRE(loaded.handles.materials.size() == 1);
    REQUIRE(loaded.handles.materialStates.size() == loaded.handles.materials.size());
    CHECK(loaded.handles.materials[0].valid());
    CHECK((loaded.binding.submeshes[0].material == loaded.handles.materials[0]));
    CHECK_FALSE(loaded.binding.submeshes[1].material.valid());

    // The one bound slot became ONE texture request, in the material's own colour space -- baseColor is
    // slot 0, which materialSlotIsSrgb answers sRGB for. No slot texture is bound yet: default texels
    // show until the ledger dresses them, one directive per service pass (the 3.4.1 doctrine).
    REQUIRE(loaded.textureRequests.size() == 1);
    CHECK(loaded.textureRequests[0].materialIndex == 0);
    CHECK(loaded.textureRequests[0].slot == 0);
    CHECK(loaded.textureRequests[0].srgb);
    CHECK((loaded.textureRequests[0].guid == *project->database.guidForPath("wood.png")));
    CHECK_FALSE(loaded.handles.materialStates[0].slots.baseColor.texture.valid());

    // SL10 -- bounds folded per sourceMeshIndex, each the union of that mesh's cooked submesh boxes.
    // Mesh-LOCAL and node-independent: no node transform enters this, which is what makes it an
    // entity-local box.
    REQUIRE(loaded.handles.bounds.size() == 2);
    const auto boxOf = [&loaded](std::uint32_t meshIndex) -> engine::editor::Aabb {
        for (const std::pair<std::uint32_t, engine::editor::Aabb>& entry : loaded.handles.bounds) {
            if (entry.first == meshIndex) {
                return entry.second;
            }
        }
        return engine::editor::Aabb::empty();
    };
    const engine::editor::Aabb first = boxOf(0);
    const engine::editor::Aabb second = boxOf(1);
    REQUIRE(first.valid());
    REQUIRE(second.valid());
    CHECK(first.max.x == doctest::Approx(1.0F));
    CHECK(first.max.y == doctest::Approx(1.0F));
    CHECK(second.max.x == doctest::Approx(2.0F));
    CHECK(second.max.y == doctest::Approx(2.0F));
    CHECK(first.min.x == doctest::Approx(0.0F));
    CHECK(second.min.y == doctest::Approx(0.0F));

    renderer->destroyMesh(loaded.handles.mesh);
    for (const engine::render::MaterialHandle material : loaded.handles.materials) {
        renderer->destroyMaterial(material);
    }
}

TEST_CASE("editor: the parse buffer outlives createMesh (task 3.1.5, SL5, seed S22)") {
    // THE RETAINED-SPAN CONTRACT (docs/09 section 9), and ASAN IS THE ORACLE -- not this case's own
    // CHECKs. CookedMesh::bytes is a span into the MeshCookResult's vector, and createMesh reads
    // through it; a seed that clears or scopes that vector before the upload is a heap-use-after-free
    // inside createMesh, which the Debug lanes abort on and which no CHECK anywhere could see. What
    // this case contributes is a load that REACHES createMesh with real bulk data behind it, then a
    // read of the registry afterwards so the handle is genuinely usable rather than merely non-zero.
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(*device, {64, 64});
    REQUIRE(target.has_value());
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    std::optional<engine::render::ForwardRenderer> renderer = engine::render::ForwardRenderer::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(renderer.has_value());

    const std::unique_ptr<LoaderProject> project = makeLoaderProject();
    REQUIRE(engine::editor::writeTextFileAtomic(project->assetsRoot + "/two.gltf", SL_TWO_MESH_GLTF_TEXT).empty());
    scanLoaderProject(*project);

    engine::editor::SceneAssetLoader loader(*device);
    const engine::editor::SceneAssetLoader::ModelLoadResult loaded =
        loader.loadModel(project->record("two.gltf"), project->assetsRoot, project->root, project->database, *renderer);
    INFO("loader message: " << loaded.message);
    REQUIRE(loaded.ok);
    REQUIRE(loaded.handles.mesh.valid());
    CHECK(renderer->meshSubmeshCount(loaded.handles.mesh) == 2U);
    renderer->destroyMesh(loaded.handles.mesh);
    for (const engine::render::MaterialHandle material : loaded.handles.materials) {
        renderer->destroyMaterial(material);
    }
}

TEST_CASE("editor: an .aeromat loads and a slot texture rebinds into it (task 3.1.5, SL6/SL7)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(*device, {64, 64});
    REQUIRE(target.has_value());
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    std::optional<engine::render::ForwardRenderer> renderer = engine::render::ForwardRenderer::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(renderer.has_value());

    const std::unique_ptr<LoaderProject> project = makeLoaderProject();
    REQUIRE(engine::editor::writeTextFileAtomic(project->assetsRoot + "/one.aeromat", MINIMAL_AEROMAT_TEXT).empty());
    REQUIRE(writeBinaryFixture(project->assetsRoot + "/wood.png", TINY_PNG_RED.data(), TINY_PNG_RED.size()).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(project->assetsRoot + "/broken.png", CORRUPT_PNG_BYTES).empty());
    scanLoaderProject(*project);

    engine::editor::SceneAssetLoader loader(*device);

    // SL6 -- the short twin: read, parse, createMaterial. The canonical fixture binds no slot, so it
    // asks for no texture at all.
    engine::editor::SceneAssetLoader::MaterialLoadResult material =
        loader.loadMaterial(project->record("one.aeromat"), project->assetsRoot, *renderer);
    INFO("material message: " << material.message);
    REQUIRE(material.ok);
    CHECK(material.message.empty());
    REQUIRE(material.material.valid());
    CHECK(material.textureRequests.empty());
    CHECK(material.state.params.metallicFactor == doctest::Approx(0.25F));
    CHECK(material.state.params.roughnessFactor == doctest::Approx(0.75F));

    // SL7 -- one slot texture through the SHARED decode -> cook -> parse -> upload chain, then the
    // rebind. rebindSlot writes the arrived handle into the ledger's own copy of the slots and calls
    // updateMaterial; the copy is what makes a later sibling slot's rebind keep this one.
    const engine::editor::SceneAssetLoader::TextureLoadResult texture =
        loader.loadSlotTexture(project->record("wood.png"), project->assetsRoot, /*srgb=*/true);
    INFO("texture message: " << texture.message);
    REQUIRE(texture.ok);
    REQUIRE(texture.texture.valid());
    CHECK(loader.textureFailureCount() == 0);
    loader.rebindSlot(*renderer, material.material, material.state.params, material.state.slots, /*slot=*/0,
                      texture.texture);
    CHECK((material.state.slots.baseColor.texture == texture.texture));
    CHECK(renderer->updateMaterial(material.material, material.state.params, material.state.slots));

    // A BROKEN image fails ONCE and stays failed -- the ThumbnailLedger stickiness rule, applied to
    // this cache. The proof is not the count: it is that REPLACING the bytes on disk with a real PNG
    // does not change the answer, because the RECORD -- and therefore the key -- has not moved. A
    // re-decode would succeed here and redden the second REQUIRE.
    const engine::editor::SceneAssetLoader::TextureLoadResult failed =
        loader.loadSlotTexture(project->record("broken.png"), project->assetsRoot, /*srgb=*/true);
    CHECK_FALSE(failed.ok);
    CHECK_FALSE(failed.message.empty());
    CHECK(loader.textureFailureCount() == 1);
    REQUIRE(
        writeBinaryFixture(project->assetsRoot + "/broken.png", TINY_PNG_GREEN.data(), TINY_PNG_GREEN.size()).empty());
    const engine::editor::SceneAssetLoader::TextureLoadResult again =
        loader.loadSlotTexture(project->record("broken.png"), project->assetsRoot, /*srgb=*/true);
    CHECK_FALSE(again.ok);
    CHECK(again.message == failed.message);
    CHECK(loader.textureFailureCount() == 1);

    device->destroyTexture(texture.texture);
    renderer->destroyMaterial(material.material);
}

TEST_CASE("editor: an unconverted .blend refuses without spawning anything (task 3.1.5, SL8, AC-24)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(*device, {64, 64});
    REQUIRE(target.has_value());
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    std::optional<engine::render::ForwardRenderer> renderer = engine::render::ForwardRenderer::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(renderer.has_value());

    const std::unique_ptr<LoaderProject> project = makeLoaderProject();
    REQUIRE(engine::editor::writeTextFileAtomic(project->assetsRoot + "/scene.blend", "BLENDER-v420RENDH").empty());
    scanLoaderProject(*project);

    engine::editor::SceneAssetLoader loader(*device);
    const engine::editor::SceneAssetLoader::ModelLoadResult loaded = loader.loadModel(
        project->record("scene.blend"), project->assetsRoot, project->root, project->database, *renderer);
    CHECK_FALSE(loaded.ok);
    CHECK(loaded.message == std::string(engine::editor::BLEND_UNCONVERTED_MESSAGE));
    // NOTHING was imported and NOTHING was uploaded: the arm returns before either.
    CHECK(loader.importCount() == 0);
    CHECK(loader.meshUploadCount() == 0);
    // The cache-hit read on its own, at the same record: the same miss, and the same sentence.
    const engine::editor::BlendArtifactResult artifact =
        engine::editor::readBlendCacheArtifact(project->record("scene.blend"), project->root);
    CHECK_FALSE(artifact.ok);
    CHECK(artifact.bytes.empty());
    CHECK(artifact.message == std::string(engine::editor::BLEND_UNCONVERTED_MESSAGE));

    // AC-24's mechanical half, and the only tier that can see it: no runtime assertion can prove a
    // process was NOT spawned by code that was never reached, so this reads the TU's own source text.
    // A second BlenderService anywhere in this file would make a cache-hit evaluation cost a process.
    const std::vector<std::string> code = editorSourceCodeLines(AERO_EDITOR_SRC_DIR "/scene_asset_loader.cpp");
    for (const std::string& line : code) {
        CHECK(line.find("BlenderService") == std::string::npos);
        CHECK(line.find("SDL_Process") == std::string::npos);
    }
}

TEST_CASE("editor: a model with no drawable geometry is refused, not half-loaded (task 3.1.5, SL9)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(*device, {64, 64});
    REQUIRE(target.has_value());
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    std::optional<engine::render::ForwardRenderer> renderer = engine::render::ForwardRenderer::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(renderer.has_value());

    const std::unique_ptr<LoaderProject> project = makeLoaderProject();
    // A LEGAL glTF document carrying no mesh at all. It imports Ok, cooks to the valid zero-primitive
    // container docs/09 section 9 defines, parses Ok -- and createMesh refuses it, because there is
    // nothing to create a buffer for and a zero-size createBuffer is itself an rhi validation failure.
    REQUIRE(engine::editor::writeTextFileAtomic(project->assetsRoot + "/empty.gltf", MINIMAL_GLTF_TEXT).empty());
    scanLoaderProject(*project);

    engine::editor::SceneAssetLoader loader(*device);
    const engine::editor::SceneAssetLoader::ModelLoadResult loaded = loader.loadModel(
        project->record("empty.gltf"), project->assetsRoot, project->root, project->database, *renderer);
    CHECK_FALSE(loaded.ok);
    CHECK(loaded.message == "the GPU refused this mesh (see the Console for the reason)");
    CHECK_FALSE(loaded.handles.mesh.valid());
    // The attempt is counted at the CALL, so a refusal is visible as an upload that produced nothing
    // rather than as an upload that never happened. Two imports for the same reason as SL1's.
    CHECK(loader.importCount() == 2);
    CHECK(loader.meshUploadCount() == 1);
    // NOTHING was half-installed: no material, no binding, no bounds.
    CHECK(loaded.handles.materials.empty());
    CHECK(loaded.binding.submeshes.empty());
    CHECK(loaded.handles.bounds.empty());
    CHECK(loaded.textureRequests.empty());
}

// ================================================================================================
// task 3.1.5 (DP1-DP22) -- the three drop surfaces, end to end through EditorApp's own seams.
//
// Each seam records EXACTLY what the corresponding panel's accept records, so these cases exercise
// the real drain, the real command push and the real ledger service pass. What they cannot exercise
// is ImGui's own drag machinery: no tier in this tree can press a mouse button and move it, which is
// why the accept/refuse MATRIX is proven at tier 0 (asset_drag_test.cpp's DR*) and the GLUE is proven
// by source-text pins plus the manual validation pass.
//
// GPU-tier and, like the SL block above, compiled only where the shader toolchain built the artifacts:
// the ledger's whole execute half needs a ForwardRenderer, and the viewport has none in a build with
// no cooked shaders.
// ================================================================================================

namespace {

// A project with one model, one material and one texture, scanned by a real EditorApp. Returns the
// app; the caller drives it. Every literal here is already defined above -- SL_TWO_MESH_GLTF_TEXT,
// MINIMAL_AEROMAT_TEXT and TINY_PNG_RED -- so this block adds no fixture of its own.
struct DropFixture {
    std::string root;
    std::string assetsRoot;
};

[[nodiscard]] DropFixture makeDropProject() {
    const std::string location = uniqueProjectLocation();
    const engine::editor::ProjectCreateOutcome created = engine::editor::createProject(location, "MyGame", "0.1.0");
    REQUIRE(created.problem == engine::editor::CreateProblem::Ok);
    DropFixture fixture{.root = created.root, .assetsRoot = created.root + "/assets"};
    REQUIRE(engine::editor::writeTextFileAtomic(fixture.assetsRoot + "/two.gltf", SL_TWO_MESH_GLTF_TEXT).empty());
    REQUIRE(engine::editor::writeTextFileAtomic(fixture.assetsRoot + "/one.aeromat", MINIMAL_AEROMAT_TEXT).empty());
    REQUIRE(writeBinaryFixture(fixture.assetsRoot + "/wood.png", TINY_PNG_RED.data(), TINY_PNG_RED.size()).empty());
    return fixture;
}

// The kind byte a payload carries. It is a PEEK HINT -- every drain re-derives the real kind from the
// record -- so these cases spell it honestly rather than passing 0 everywhere.
constexpr std::uint8_t MODEL_KIND = static_cast<std::uint8_t>(engine::editor::AssetKind::Model);
constexpr std::uint8_t MATERIAL_KIND = static_cast<std::uint8_t>(engine::editor::AssetKind::Material);
constexpr std::uint8_t TEXTURE_KIND = static_cast<std::uint8_t>(engine::editor::AssetKind::Texture);

}  // namespace

TEST_CASE("editor: a model dropped on the Hierarchy is ONE undoable subtree (task 3.1.5, DP1/DP2/DP3)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "drop dp1", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    const DropFixture fixture = makeDropProject();
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = fixture.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
    }
    const std::optional<engine::Guid> model = app->assetGuidForPath("two.gltf");
    REQUIRE(model.has_value());

    const std::size_t before = app->world().entityCount();
    app->requestHierarchyAssetDrop(*model, MODEL_KIND, engine::Entity{});  // Entity{} == the VOID target
    REQUIRE(app->tick());

    // DP1 -- the subtree exists. The fixture has two nodes, so the plan is a synthetic root plus two
    // children: three new entities, created by ONE command.
    const std::size_t after = app->world().entityCount();
    CHECK(after == before + 3);
    CHECK(app->commands().canUndo());
    CHECK_FALSE(app->commands().canRedo());

    // DP2 -- ONE undo removes the WHOLE subtree, and redo puts it back with the same shape. That is
    // what "one drop is one command" means, and it is the property a per-entity loop would break.
    app->requestUndo();
    REQUIRE(app->tick());
    CHECK(app->world().entityCount() == before);
    app->requestRedo();
    REQUIRE(app->tick());
    CHECK(app->world().entityCount() == before + 3);

    // DP3 -- a ROW drop parents the new subtree under that row instead. The row is any existing
    // entity; the default scene's first root will do.
    engine::Entity row{};
    app->world().eachEntity([&row, &app](engine::Entity e) {
        if (!row.valid() && !app->world().parent(e).valid()) {
            row = e;
        }
    });
    REQUIRE(row.valid());
    const std::size_t beforeRow = app->world().entityCount();
    app->requestHierarchyAssetDrop(*model, MODEL_KIND, row);
    REQUIRE(app->tick());
    CHECK(app->world().entityCount() == beforeRow + 3);
    // Exactly one NEW child of `row` -- the plan's synthetic root, reparented under it.
    std::size_t children = 0;
    app->world().eachChild(row, [&children](engine::Entity /*child*/) { ++children; });
    CHECK(children >= 1);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: two material drops on one entity are two undo steps (task 3.1.5, DP23)") {
    // The code-review round: pushMaterialAssign pushed a SetFieldCommand without touching the merge
    // chain, and SetFieldCommand::mergeWith accepts any incoming command with the same entity, type and
    // field -- overwriting `afterValue` while deliberately KEEPING `beforeValue`. A merged pair would
    // read nil -> B, so one undo would jump straight past A and A would be unreachable from the history.
    //
    // MEASURED, AND IT DOES NOT REDDEN: removing BOTH breakMergeChain gates from pushMaterialAssign and
    // rebuilding leaves this case GREEN -- the two drops still make two entries (the run logged
    // undo 'MeshRenderer.material' (0 left) only on the SECOND undo, so `applied` went 2 -> 1 -> 0).
    // Something else on the tick path between two drained drops already closes the chain, so the
    // gates are defence in depth rather than the only thing standing between this and a merge. They
    // are kept because they make the property LOCAL to the discrete gesture that owns it instead of
    // incidental to whatever a neighbouring subsystem happens to do -- the same reason the inspector's
    // Guid row carries its own pair. This case therefore pins the observable CONTRACT (two drops are
    // two undo steps, and the first undo lands on A) and is honestly NOT a witness for the gates
    // themselves; do not cite it as one.
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "drop dp23", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    const DropFixture fixture = makeDropProject();
    // A SECOND material, so the two drops name different guids -- written here rather than into
    // makeDropProject, which other cases share.
    REQUIRE(engine::editor::writeTextFileAtomic(fixture.assetsRoot + "/two.aeromat", MINIMAL_AEROMAT_TEXT).empty());
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = fixture.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
    }
    const std::optional<engine::Guid> materialA = app->assetGuidForPath("one.aeromat");
    const std::optional<engine::Guid> materialB = app->assetGuidForPath("two.aeromat");
    REQUIRE(materialA.has_value());
    REQUIRE(materialB.has_value());
    REQUIRE_FALSE(*materialA == *materialB);

    engine::Entity withMesh{};
    app->world().eachEntity([&](engine::Entity e) {
        if (!withMesh.valid() && app->world().has<engine::MeshRenderer>(e)) {
            withMesh = e;
        }
    });
    REQUIRE(withMesh.valid());

    app->requestHierarchyAssetDrop(*materialA, MATERIAL_KIND, withMesh);
    REQUIRE(app->tick());
    app->requestHierarchyAssetDrop(*materialB, MATERIAL_KIND, withMesh);
    REQUIRE(app->tick());

    const engine::MeshRenderer* renderer = app->world().get<engine::MeshRenderer>(withMesh);
    REQUIRE(renderer != nullptr);
    // BOTH ARMS ASSERT, neither skips -- DP6's reasoning, and this case needs it for the same reason:
    // the assignment rides SetFieldCommand through the reflection seam, so with -DAERO_REFLECT_TOOLS=OFF
    // there is no entt::meta for engine::MeshRenderer and no drop can land at all. Undo depth is not a
    // meaningful question there, because nothing was ever pushed.
    #if AERO_REFLECT_TOOLS_ENABLED
    CHECK((renderer->material == *materialB));

    // ONE undo must land on A, not on nil. That is the whole finding: a merged pair skips A.
    app->requestUndo();
    REQUIRE(app->tick());
    renderer = app->world().get<engine::MeshRenderer>(withMesh);
    REQUIRE(renderer != nullptr);
    CHECK((renderer->material == *materialA));

    // ...and a SECOND undo reaches nil, so the two drops really are two entries.
    CHECK(app->commands().canUndo());
    app->requestUndo();
    REQUIRE(app->tick());
    renderer = app->world().get<engine::MeshRenderer>(withMesh);
    REQUIRE(renderer != nullptr);
    CHECK_FALSE(renderer->material.valid());
    #else
    // No meta: BOTH drops are inert. The field never moved and nothing entered the history, which is
    // the same quiet degradation componentFieldsAreReflected buys for the single-drop case.
    CHECK_FALSE(renderer->material.valid());
    CHECK_FALSE(app->commands().canUndo());
    #endif

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a material drop assigns only where a MeshRenderer is (task 3.1.5, DP5/DP6/DP7)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "drop dp5", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    const DropFixture fixture = makeDropProject();
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = fixture.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
    }
    const std::optional<engine::Guid> material = app->assetGuidForPath("one.aeromat");
    const std::optional<engine::Guid> texture = app->assetGuidForPath("wood.png");
    REQUIRE(material.has_value());
    REQUIRE(texture.has_value());

    // A row WITH a MeshRenderer, and a row WITHOUT one, in the same World.
    engine::Entity withMesh{};
    engine::Entity withoutMesh{};
    app->world().eachEntity([&](engine::Entity e) {
        if (app->world().has<engine::MeshRenderer>(e)) {
            if (!withMesh.valid()) {
                withMesh = e;
            }
        } else if (!withoutMesh.valid()) {
            withoutMesh = e;
        }
    });
    REQUIRE(withMesh.valid());
    REQUIRE(withoutMesh.valid());

    // DP5 -- refused on a row with no MeshRenderer: nothing is pushed, so the stack stays exactly
    // where it was. The drain re-derives that from the LIVE World, never from the payload.
    const bool couldUndoBefore = app->commands().canUndo();
    app->requestHierarchyAssetDrop(*material, MATERIAL_KIND, withoutMesh);
    REQUIRE(app->tick());
    CHECK(app->commands().canUndo() == couldUndoBefore);

    // DP7 -- a TEXTURE is refused on both Hierarchy surfaces: it has no meaning outside a material
    // slot, and the matrix says so for the row and for the void alike.
    app->requestHierarchyAssetDrop(*texture, TEXTURE_KIND, withMesh);
    REQUIRE(app->tick());
    app->requestHierarchyAssetDrop(*texture, TEXTURE_KIND, engine::Entity{});
    REQUIRE(app->tick());
    CHECK(app->commands().canUndo() == couldUndoBefore);
    // A MODEL on a row with a MeshRenderer is NOT a material assignment either -- it instantiates.
    // Asserted here so "the matrix is consulted" cannot be satisfied by a blanket refusal.

    // DP6 -- accepted on a row WITH a MeshRenderer: one SetFieldCommand, and undo restores the field.
    // No new command type; it rides the Guid arm the field seam gained at step 15.
    //
    // BOTH ARMS ASSERT, neither skips (the AC-32 shape from task 3.4.2, applied to the OTHER gate).
    // The assignment goes through SetFieldCommand, which reaches the field via entt::meta -- so with
    // -DAERO_REFLECT_TOOLS=OFF there is no meta for engine::MeshRenderer and the drop CANNOT land.
    // That is the correct behaviour there, not a defect: the whole inspector cannot edit any field in
    // that configuration either. What matters is that it degrades to "nothing happens" QUIETLY, with
    // no command pushed and no ERROR from the seam -- which is what componentFieldsAreReflected buys,
    // and this arm is its only witness anywhere.
    const engine::MeshRenderer* renderer = app->world().get<engine::MeshRenderer>(withMesh);
    REQUIRE(renderer != nullptr);
    CHECK_FALSE(renderer->material.valid());
    app->requestHierarchyAssetDrop(*material, MATERIAL_KIND, withMesh);
    REQUIRE(app->tick());
    renderer = app->world().get<engine::MeshRenderer>(withMesh);
    REQUIRE(renderer != nullptr);
    #if AERO_REFLECT_TOOLS_ENABLED
    CHECK((renderer->material == *material));
    CHECK(app->commands().canUndo());
    app->requestUndo();
    REQUIRE(app->tick());
    renderer = app->world().get<engine::MeshRenderer>(withMesh);
    REQUIRE(renderer != nullptr);
    CHECK_FALSE(renderer->material.valid());
    #else
    // No meta: the field is untouched and NOTHING was pushed -- the drop is inert, not half-applied.
    CHECK_FALSE(renderer->material.valid());
    CHECK(app->commands().canUndo() == couldUndoBefore);
    #endif

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a viewport drop places the entity on the ground plane (task 3.1.5, DP8/DP9)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "drop dp8", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    const DropFixture fixture = makeDropProject();
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = fixture.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
    }
    const std::optional<engine::Guid> model = app->assetGuidForPath("two.gltf");
    const std::optional<engine::Guid> material = app->assetGuidForPath("one.aeromat");
    REQUIRE(model.has_value());
    REQUIRE(material.has_value());
    REQUIRE(app->viewportCamera() != nullptr);

    // DP8 -- the drop's own ray meets y = 0, and dropPlacementPoint's contract is TOTAL: every input
    // yields a FINITE point, so the assertion below is finiteness plus "it is not the origin by
    // accident". The exact point is picking_test.cpp's (DR13-DR16); what belongs here is that the
    // placement reaches the entity's Transform at all rather than being dropped on the floor.
    const std::size_t before = app->world().entityCount();
    app->requestViewportAssetDrop(*model, MODEL_KIND, engine::Vec2{0.25F, -0.4F});
    REQUIRE(app->tick());
    REQUIRE(app->world().entityCount() == before + 3);
    // The newest root -- the plan's synthetic root -- carries the placement.
    engine::Entity newest{};
    app->world().eachEntity([&](engine::Entity e) {
        if (!app->world().parent(e).valid() && app->world().has<engine::Transform>(e)) {
            newest = e;  // eachEntity walks in creation order, so the LAST root wins
        }
    });
    REQUIRE(newest.valid());
    const engine::Transform* placed = app->world().get<engine::Transform>(newest);
    REQUIRE(placed != nullptr);
    CHECK(std::isfinite(placed->position.x));
    CHECK(std::isfinite(placed->position.y));
    CHECK(std::isfinite(placed->position.z));

    // DP9 -- a MATERIAL dropped over empty space is refused: there is no entity under the cursor, so
    // classifyAssetDrop answers None and nothing is pushed. The pick that decides it runs against the
    // LIVE World, through the panel's own camera.
    const bool couldUndo = app->commands().canUndo();
    app->requestViewportAssetDrop(*material, MATERIAL_KIND, engine::Vec2{-0.99F, 0.99F});
    REQUIRE(app->tick());
    CHECK(app->commands().canUndo() == couldUndo);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the ledger loads a dropped model exactly once (task 3.1.5, DP13/DP14/DP20)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "drop dp13", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    const DropFixture fixture = makeDropProject();
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = fixture.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
    }
    const std::optional<engine::Guid> model = app->assetGuidForPath("two.gltf");
    REQUIRE(model.has_value());
    CHECK(app->sceneAssetEntryCount() == 0);
    CHECK(app->sceneAssetDirectiveCount() == 0);

    app->requestHierarchyAssetDrop(*model, MODEL_KIND, engine::Entity{});
    for (int i = 0; i < 6; ++i) {
        REQUIRE(app->tick());
    }

    // DP13 -- the ledger holds the guid, it is Ready, and the binding table names it. Nothing here
    // needs a pixel: a resolved reference IS the binding table having an entry for that guid.
    INFO("ledger message: " << app->sceneAssetMessage(*model));
    CHECK(app->sceneAssetEntryCount() >= 1);
    CHECK(app->sceneAssetReadyCount() >= 1);
    CHECK(app->sceneAssetFailedCount() == 0);
    CHECK(app->sceneAssetMeshBindingCount() == 1);

    // DP14/DP20 -- THE DROP'S OWN Full import was handed straight to the cook -> upload half, so the
    // ledger never had to issue a MODEL directive for this guid at all. Its directive count is
    // therefore either zero or purely the material's/texture's -- never one that re-read and
    // re-imported the same bytes. A regression that dropped the seed would make the model's own
    // directive fire, and readyCount would arrive one whole extra import later.
    const std::size_t directivesAfterLoad = app->sceneAssetDirectiveCount();
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    // Steady state: a Ready entry with no pending textures issues nothing, forever.
    CHECK(app->sceneAssetDirectiveCount() == directivesAfterLoad);
    CHECK(app->sceneAssetReadyCount() >= 1);

    // DP16 -- once the mesh is bound, the bridge resolves it: the viewport's latched unresolved count
    // is back to zero. It is TRANSIENT by design between the drop and the upload, which is exactly why
    // it is counted rather than warned about.
    CHECK(app->viewportUnresolvedMeshes() == 0U);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a retired binding's handles die ONE PASS LATER (task 3.1.5, DP15/DP18/DP21)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "drop dp15", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    const DropFixture fixture = makeDropProject();
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = fixture.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
    }
    const std::optional<engine::Guid> model = app->assetGuidForPath("two.gltf");
    REQUIRE(model.has_value());
    app->requestHierarchyAssetDrop(*model, MODEL_KIND, engine::Entity{});
    for (int i = 0; i < 6; ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->sceneAssetMeshBindingCount() == 1);
    const std::size_t destroysBefore = app->sceneAssetDestroyCount();

    // UNDO removes the whole subtree, so nothing references the guid any more.
    app->requestUndo();
    REQUIRE(app->tick());
    // DP15/DP18 -- the pass that RETIRES destroys nothing: the binding table stopped naming those
    // handles this very pass, and a frame recorded before it could still hold them. The destroy list is
    // a LEDGER member, returned by the NEXT service call, which is what makes the deferral survive
    // across passes rather than collapse into one.
    CHECK(app->sceneAssetMeshBindingCount() == 0);
    CHECK(app->sceneAssetDestroyCount() == destroysBefore);
    REQUIRE(app->tick());
    CHECK(app->sceneAssetDestroyCount() > destroysBefore);
    const std::size_t destroysAfter = app->sceneAssetDestroyCount();
    // ...and exactly once: a third pass returns nothing, because the list was moved out, not copied.
    REQUIRE(app->tick());
    CHECK(app->sceneAssetDestroyCount() == destroysAfter);
    CHECK(app->sceneAssetEntryCount() == 0);

    // DP21 -- shutdown releases whatever is still live, through the renderer that minted it, while the
    // panels are still alive. Re-load first so there IS something to release; the destructor's own
    // drain is what ASan and the device's own leak WARN judge.
    app->requestRedo();
    for (int i = 0; i < 6; ++i) {
        REQUIRE(app->tick());
    }
    CHECK(app->sceneAssetMeshBindingCount() == 1);
    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();  // ~EditorApp drains the ledger through the viewport's renderer -- ASan is the oracle
}

TEST_CASE("editor: a vanished asset warns and does nothing (task 3.1.5, DP10)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "drop dp10", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    const DropFixture fixture = makeDropProject();
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = fixture.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
    }
    // INV-D8: the payload is a HINT and the database is the authority. A guid this project has never
    // heard of re-resolves to nothing, so the drain stops at (a) -- one WARN, no command, no entity.
    const std::size_t before = app->world().entityCount();
    const bool couldUndo = app->commands().canUndo();
    const engine::Guid stranger{0xFEEDFACECAFEBEEFULL, 0x0123456789ABCDEFULL};
    app->requestHierarchyAssetDrop(stranger, MODEL_KIND, engine::Entity{});
    app->requestViewportAssetDrop(stranger, MODEL_KIND, engine::Vec2{});
    REQUIRE(app->tick());
    CHECK(app->world().entityCount() == before);
    CHECK(app->commands().canUndo() == couldUndo);
    CHECK(app->sceneAssetEntryCount() == 0);

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: a texture dropped on a material slot binds it and dirties the session (task 3.1.5, DP11/DP12)") {
    engine::platform::Context ctx;
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no platform context");
    }
    std::optional<engine::platform::Window> window =
        ctx.createWindow({.title = "drop dp11", .width = 320, .height = 180});
    REQUIRE(window.has_value());
    std::optional<engine::rhi::Device> device = engine::rhi::Device::create();
    if (!device) {
        AERO_SKIP_OR_FAIL("no GPU device");
    }
    const DropFixture fixture = makeDropProject();
    std::optional<engine::editor::EditorApp> app =
        engine::editor::EditorApp::create(*device, *window, ctx,
                                          {.persistLayout = false,
                                           .unfocusedFrameCapHz = 0.0F,
                                           .projectPath = fixture.root,
                                           .restoreLastProject = false,
                                           .recentProjectsPath = uniqueRecentsFile()});
    REQUIRE(app.has_value());
    app->panels().setVisible("Console", false);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
    }
    const std::optional<engine::Guid> texture = app->assetGuidForPath("wood.png");
    REQUIRE(texture.has_value());

    // DP12 -- UNTARGETED: the slot section is not drawn at all when the session has no document, so a
    // driven drop folds into nothing and no pending edit is ever recorded. The session stays clean.
    CHECK(app->materialTargetPath().empty());
    app->requestMaterialSlotTextureDrop(0, *texture);
    for (int i = 0; i < 3; ++i) {
        REQUIRE(app->tick());
    }
    CHECK_FALSE(app->materialDirty());

    // DP11 -- TARGETED: the drop folds into the panel's own frame copy at exactly the point the picker
    // would have written it, so the existing pendingDocument -> session.edit -> dirty -> Apply river
    // does the rest. No new write path and no new session surface.
    app->requestAssetBrowserSelectEntry("one.aeromat");
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->materialTargetPath() == "one.aeromat");
    REQUIRE(app->materialDocument() != nullptr);
    CHECK_FALSE(app->materialDocument()->baseColor.has_value());
    app->requestMaterialSlotTextureDrop(0, *texture);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(app->tick());
    }
    REQUIRE(app->materialDocument() != nullptr);
    REQUIRE(app->materialDocument()->baseColor.has_value());
    CHECK((app->materialDocument()->baseColor->guid == *texture));
    CHECK(app->materialDirty());  // and Apply is what writes it -- the drop never touches a file

    app->requestQuit();
    CHECK(app->tick() == false);
    app.reset();
}

TEST_CASE("editor: the drop is not a PendingAction and never mutates in a draw walk (task 3.1.5, DP4/DP22)") {
    // DP4 -- the asset drop is deliberately NOT routed through HierarchyPanel's five-phase apply: that
    // switch cannot finish the job (instantiation needs the database, the importer and the ledger), so
    // an enumerator it would have to refuse is worse than no enumerator. A RECORDED deviation from
    // AC-26, and this is where it is pinned: `pendingAssetDrop` appears in the panel, `applyPending`
    // never touches it, and ActionKind gained nothing.
    const std::vector<std::string> hierarchy = editorSourceCodeLines(AERO_EDITOR_SRC_DIR "/hierarchy_panel.cpp");
    bool sawDropRecord = false;
    bool applyPendingSeen = false;
    bool applyPendingTouchesDrop = false;
    for (const std::string& line : hierarchy) {
        if (line.find("pendingAssetDrop = HierarchyAssetDrop") != std::string::npos) {
            sawDropRecord = true;
        }
        if (line.find("void HierarchyPanel::applyPending") != std::string::npos) {
            applyPendingSeen = true;
        }
        if (applyPendingSeen && line.find("pendingAssetDrop") != std::string::npos) {
            applyPendingTouchesDrop = true;
        }
    }
    CHECK(sawDropRecord);     // the accept really records
    CHECK(applyPendingSeen);  // and the scan really reached applyPending
    CHECK_FALSE(applyPendingTouchesDrop);

    // DP22 -- the ledger's service pass sits in the POST-DRAW slot: textually AFTER drawShellUi, which
    // is the one call that invokes every panel's onDraw. No runtime tier here can see the general-case
    // violation; this is I60's proof shape, applied to the fifth occupant of that slot.
    const std::vector<std::string> appCode = editorSourceCodeLines(AERO_EDITOR_SRC_DIR "/editor_app.cpp");
    const std::size_t drawWalk = soleLineContaining(appCode, "drawShellUi(registry, panelContext, ui, fileMenu)");
    const std::size_t service = soleLineContaining(appCode, "serviceSceneAssets();");
    const std::size_t endFrame = soleLineContaining(appCode, "presented = layer.endFrame(config.clearColor)");
    CHECK(service > drawWalk);
    CHECK(service < endFrame);
}

#endif  // AERO_SHADER_TOOLS_ENABLED

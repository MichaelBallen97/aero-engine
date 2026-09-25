// Aero Engine — EditorApp: create/tick/run/pacing (task 2.1.3, D1/D4/D11). ImGui-FREE — includes
// shell_ui.hpp (src-private) but never ImGui itself; drawShellUi is the only ImGui-touching call.
#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/time.hpp>
#include <aero/editor/asset_actions.hpp>         // task 3.1.3: deleteOrphanMeta/OrphanDeleteResult -- the
#include <aero/editor/asset_commands.hpp>        // task 3.1.5: InstantiateAssetCommand -- one drop, one command
#include <aero/editor/component_commands.hpp>    // task 3.1.5: SetFieldCommand -- the material assignment
#include <aero/editor/component_ops.hpp>         // task 3.1.5: readComponentField, for the assignment's before
#include <aero/editor/instantiate_plan.hpp>      // task 3.1.5: buildInstantiatePlan + its refusal enum
#include <aero/editor/material_from_import.hpp>  // task 3.1.5: reached through the loader; named for clarity
#include <aero/editor/material_preview_rig.hpp>  // task E.2.4: MaterialPreviewLighting
#include <aero/editor/picking.hpp>               // task 3.1.5: viewportRay + dropPlacementPoint
#include <aero/editor/text_file.hpp>             // task 3.1.5: readFileBytes, for the drop's own import
                                                 // reconcile block's third one-shot drain
#include <aero/editor/console_model.hpp>
#include <aero/editor/editor_app.hpp>
#include <aero/editor/editor_camera.hpp>
#include <aero/editor/editor_prefs.hpp>  // task E.3.2: EditorPrefs + readEditorPrefs/writeEditorPrefs.
                                         // context_router.hpp arrives through editor_app.hpp.
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/material_edit.hpp>  // task 3.4.2: uniqueMaterialFileName -- New Material's
                                          // one naming rule, PURE and shared with the ME tier
#include <aero/editor/project.hpp>        // task E.3.2: defaultEditorPrefsPath() -- named directly rather
                                          // than left to arrive transitively, beside the two sibling
                                          // resolvers create() has always called
#include <aero/editor/project_files.hpp>  // task 3.4.2: listDirectory/joinRelative, for the same drain
#include <aero/editor/project_state.hpp>  // task E.4.1: the per-project state file, its relative-path
                                          // arithmetic and its write decision
#include <aero/editor/scene_session.hpp>
#include <aero/platform/context.hpp>
#include <aero/platform/event.hpp>
#include <aero/platform/internal/native_window.hpp>  // task 2.5.1: the SECOND consumer F15 sanctions
                                                     // (the first is imgui_layer.cpp) -- no new
                                                     // accessor is added.
#include <aero/platform/window.hpp>                  // task 2.5.1: window->setTitle() (F14)
#include <aero/render/tonemap.hpp>                   // task 3.6.3: render::TonemapParams{} -- the null-viewport arm
#include <aero/rhi/device.hpp>                       // task 3.1.5: destroyTexture on a retired handle
#include <aero/scene_render/scene_renderer.hpp>      // task E.2.4 -- the two resolvers

#include "asset_browser_panel.hpp"
#include "asset_picker.hpp"  // task E.3.3 -- AssetPickerState, held through a unique_ptr on a public header
#include "console_panel.hpp"
#include "editor_reflection.hpp"
#include "file_dialog.hpp"  // task 2.5.1: DialogChannel's definition -- the shared_ptr's deleter needs
                            // a complete type wherever it could run, including this TU's ~EditorApp
#include "hierarchy_panel.hpp"
#include "import_details_panel.hpp"  // task 3.2.1 -- ImportDetailsPanel's definition, the src-private
                                     // shared_ptr/unique_ptr-completeness precedent above, applied to a
                                     // registry-owned panel instead
#include "inspector_panel.hpp"
#include "material_panel.hpp"  // task 3.4.2 -- MaterialPanel's definition, the ImportDetailsPanel
                               // precedent one panel over
#include "project_settings_panel.hpp"
#include "scene_asset_loader.hpp"  // task 3.1.5 -- the loader's definition, so the unique_ptr member
                                   // below can be created and destroyed in this TU
#include "shell_ui.hpp"
#include "thumbnail_service.hpp"  // task E.3.3 (D5): the SHARED thumbnail ledger/store/budget, held
                                  // through a unique_ptr on a public header -- this TU is where the
                                  // complete type is needed
#include "viewport_panel.hpp"

#include <chrono>
// std::sort / std::unique, for the referenced-guid set. INCLUDED EXPLICITLY because libc++ and
// libstdc++ both supply <algorithm> transitively here and MSVC's STL does not -- the Windows lane
// failed C2039 'sort': is not a member of 'std' on this exact line, and only after a Chocolatey
// outage stopped masking it by killing the job before it compiled anything.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>  // task E.3.3 -- std::abs over the picker seam's signed step count
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef AERO_ENGINE_VERSION
    #define AERO_ENGINE_VERSION \
        "0.0.0"  // a fallback for a hand-rolled build with no CMake definition.
                 // NOT a build gate (AC-35/D4/INV-P5): no CODE PATH is
                 // conditional on it -- exactly one string constant is.
#endif

namespace engine::editor {

namespace {

// task 3.1.5: the registered name of the component a material assignment writes into. ONE spelling,
// used by the id lookup and by the command's own label, so the two can never disagree.
constexpr std::string_view MESH_RENDERER_TYPE_NAME = "engine::MeshRenderer";

// task 3.1.5: the leaf name with its LAST extension removed -- what the instantiation plan names the
// synthetic root. A leaf with no dot, or a leading-dot name, keeps its whole leaf: a hidden file's
// name is not an extension.
[[nodiscard]] std::string_view assetStem(std::string_view relativePath) {
    const std::string_view leaf = leafOf(relativePath);
    const std::size_t dot = leaf.rfind('.');
    return (dot == std::string_view::npos || dot == 0) ? leaf : leaf.substr(0, dot);
}

// task 3.1.5: ONE retired handle, released. At most one of the three is valid per entry, so this is
// three ifs and no dispatch. Meshes and materials go back to the ForwardRenderer that minted them --
// a MeshHandle is per-renderer -- and a texture belongs to the device.
void destroyRetired(render::ForwardRenderer* renderer, rhi::Device* device, const LedgerDestroy& entry) {
    if (renderer != nullptr && entry.mesh.valid()) {
        renderer->destroyMesh(entry.mesh);
    }
    if (renderer != nullptr && entry.material.valid()) {
        renderer->destroyMaterial(entry.material);
    }
    if (device != nullptr && entry.texture.valid()) {
        device->destroyTexture(entry.texture);
    }
}

// task 2.5.1: the dialog's parent window and setTitle()'s target both need the SDL_Window* the
// platform layer hides. This is the SECOND consumer of NativeWindowAccessor (imgui_layer.cpp is the
// first) -- no new accessor is added (F15). `window == nullptr` only on a moved-from app.
void* nativeWindowHandle(platform::Window* window) {
    return window != nullptr ? static_cast<void*>(platform::internal::NativeWindowAccessor::get(*window)) : nullptr;
}

// task 2.6.2 (F11/D14): the ONE read of the build-version compile definition in the whole tree.
// TWO functions in this TU need it now -- create() for the settings panel and for ProjectContext,
// tick() for ProjectContext -- and a macro read three times is a macro that gets read a fourth
// somewhere worse.
constexpr std::string_view BUILD_ENGINE_VERSION = AERO_ENGINE_VERSION;

// task 3.1.1 (INV-A3): the asset scan never logs itself -- asset_database.cpp returns a report and
// this is the ONLY place that turns it into log records. One capped category, one WARN: the entries
// already present (up to MAX_REPORTED_PER_CATEGORY) joined by "; ", with a "…and N more" tail when
// `total` exceeds what the report kept. A `total == 0` category is silent -- this is what makes "a
// scan of a clean, fully-metaed project logs exactly one INFO line and nothing else" true.
void logCappedWarn(std::string_view root, std::string_view label, const std::vector<std::string>& entries,
                   std::size_t total) {
    if (total == 0) {
        return;
    }
    std::string body;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) {
            body += "; ";
        }
        body += entries[i];
    }
    if (total > entries.size()) {
        if (!entries.empty()) {
            body += "; ";
        }
        body += "…and ";
        body += std::to_string(total - entries.size());
        body += " more";
    }
    AERO_LOG_WARN("assets: '{}' -- {} {}: {}", root, total, label, body);
}

// task 3.1.1 (D14/AC-40). Called from tick()'s reconcile block, AFTER rescan() returns (A6: a
// pre-write announcement is unachievable through a function that returns a report, and buys nothing
// anyway -- the Console panel's pumpLog() runs at the top of the NEXT tick, so a line logged "before
// writing" would reach the screen at exactly the same frame as one logged after, since the scan
// itself is synchronous inside this tick).
void logAssetScan(std::string_view root, const AssetScanReport& report) {
    if (report.status != ScanStatus::Ok) {
        const char* reason = "unknown reason";  // enumerated so a new ScanStatus cannot be silent
        switch (report.status) {
            case ScanStatus::Missing:
                reason = "the assets root is missing";
                break;
            case ScanStatus::NotADirectory:
                reason = "the assets root is not a directory";
                break;
            case ScanStatus::Unreadable:
                reason = "the assets root could not be read (permissions or I/O error)";
                break;
            case ScanStatus::Ok:
                break;  // unreachable -- guarded by the enclosing `if`
        }
        AERO_LOG_WARN("assets: '{}' -- scan aborted: {}", root, reason);
        return;  // phase 1's guard means nothing else in the report is meaningful (E1-E4)
    }
    if (report.largeCreateNotice) {
        // A6: emitted FIRST among this scan's log lines, even though the writes themselves already
        // happened -- explaining a multi-second first open after the fact serves D14's intent exactly
        // as well as before it, since nothing reaches the screen until the frame after the scan ends.
        AERO_LOG_INFO("assets: '{}' -- writing {} .meta files (more than {}); this scan may take a moment", root,
                      report.created + report.repaired, CREATE_NOTICE_THRESHOLD);
    }
    AERO_LOG_INFO("assets: '{}' -- {} files, {} .meta created, {} repaired, {} orphans, {} invalid", root,
                  report.filesSeen, report.created, report.repaired, report.orphanTotal, report.invalid);
    // report.invalid counts EVERY Invalid-state record, including one a write conflict downgraded
    // (code-review finding 2) -- but that one is reported below, in its OWN category, never doubled
    // into this one. Every write conflict increments BOTH counters exactly once, so subtracting is
    // exact, not an estimate.
    logCappedWarn(root, "invalid asset meta file(s)", report.invalidPaths, report.invalid - report.writeConflictTotal);
    logCappedWarn(root, "orphaned .meta file(s)", report.orphans, report.orphanTotal);
    logCappedWarn(root, "repaired duplicate GUID(s)", report.repairs, report.repaired);
    logCappedWarn(root, "asset meta write failure(s)", report.writeFailures, report.writeFailureTotal);
    logCappedWarn(root, "write conflict(s) with an orphaned .meta file", report.writeConflicts,
                  report.writeConflictTotal);
    logCappedWarn(root, "unknown key warning(s)", report.unknownKeyWarnings, report.unknownKeyTotal);
    if (report.truncated) {
        AERO_LOG_WARN("assets: '{}' -- scan truncated at the {} assets-seen cap (MAX_ASSETS)", root, MAX_ASSETS);
    }
    if (report.depthLimited) {
        AERO_LOG_WARN("assets: '{}' -- one or more directory trees exceeded the {}-level depth cap (MAX_TREE_DEPTH)",
                      root, MAX_TREE_DEPTH);
    }
    if (report.skippedEntries > 0) {  // A10
        AERO_LOG_WARN("assets: '{}' -- {} entries the OS refused to classify were skipped", root,
                      report.skippedEntries);
    }
    if (report.unreadableDirs > 0) {  // A10
        AERO_LOG_WARN("assets: '{}' -- {} sub-directories below the root could not be read and were skipped", root,
                      report.unreadableDirs);
    }

    // task 3.1.2 (A16/§D-12): everything below is NEW, and every line of it sits AFTER the non-Ok early
    // return above -- an aborted scan has nothing meaningful left to report (E1-E4), so nothing below
    // may run for it. The 3.1.1 INFO line directly above this comment is BYTE-UNCHANGED (F10; 3.1.1's
    // human row 2 asserts its exact wording), which is why this task's own line is a SECOND, separate
    // INFO rather than an extension of the first. A scan of a clean, unchanged, fully-cached project
    // therefore logs exactly the two INFO lines and nothing else.
    AERO_LOG_INFO(
        "assets: '{}' -- import cache: {} up to date, {} new, {} changed, {} dependency ({} files hashed, "
        "{} B read, {} from cache)",
        root, report.upToDate, report.newAssets, report.changed, report.dependencyChanged, report.hashed,
        report.hashedBytes, report.fastPathHits);
    if (!report.cacheDiscardReason.empty()) {
        // D7/E20: an INFO, not a WARN -- a cache format-version bump is expected evolution, not
        // something the user must act on.
        AERO_LOG_INFO("assets: '{}' -- import cache discarded: {}", root, report.cacheDiscardReason);
    }
    if (report.largeHashNotice) {
        AERO_LOG_INFO("assets: '{}' -- hashing {} B this scan (more than {} B); this scan may take a moment", root,
                      report.hashedBytes, HASH_NOTICE_THRESHOLD_BYTES);
    }
    logCappedWarn(root, "re-attached orphan(s)", report.reattachments, report.reattachmentTotal);
    logCappedWarn(root, "asset hash failure(s)", report.hashFailures, report.hashFailureTotal);
    logCappedWarn(root, "aliased directory path(s)", report.aliasedDirs, report.aliasedDirTotal);
    if (!report.cacheWriteError.empty()) {
        AERO_LOG_WARN("assets: '{}' -- import cache write failed: {}", root, report.cacheWriteError);
    }
    if (report.hashBudgetExhausted) {
        AERO_LOG_WARN(
            "assets: '{}' -- scan stopped hashing at the {} B per-scan cap (MAX_HASH_BYTES_PER_SCAN); "
            "press Refresh to continue",
            root, MAX_HASH_BYTES_PER_SCAN);
    }
    if (report.cacheTruncated) {
        AERO_LOG_WARN("assets: '{}' -- import cache truncated at the {} entries cap (MAX_CACHE_ENTRIES)", root,
                      MAX_CACHE_ENTRIES);
    }
    if (report.cacheDepsDropped > 0) {
        AERO_LOG_WARN(
            "assets: '{}' -- {} dependency link(s) dropped past the {}-per-entry cap (MAX_DEPENDENCIES_PER_ENTRY)",
            root, report.cacheDepsDropped, MAX_DEPENDENCIES_PER_ENTRY);
    }

    // task 3.2.1 (phase 7.5): the model probe's own lines, appended after every 3.1.x category above.
    if (report.modelsProbed > 0) {
        AERO_LOG_INFO("assets: '{}' -- probed {} model(s), {} dependency edge(s) recorded", root, report.modelsProbed,
                      report.dependenciesRecorded);
    }
    logCappedWarn(root, "model import failure(s)", report.importFailures, report.importFailureTotal);
    if (report.probeBudgetExhausted) {
        AERO_LOG_WARN(
            "assets: '{}' -- the per-scan model probe budget was exhausted; some models keep their "
            "previous importer record until the next scan",
            root);
    }
}

}  // namespace

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

// task 2.5.1: defined HERE, out-of-line, where DialogChannel is a complete type -- forced by
// std::shared_ptr<DialogChannel>'s deleter needing completeness wherever it could run, including this
// destructor and both moves (the scene_snapshot.hpp precedent, 2.4.2 §A13, applied one layer up).
EditorApp::~EditorApp() {
    // task 3.1.5 (§0.26): shutdown ordering is solved EXPLICITLY here, not by declaration order. The
    // ledger cannot release a handle -- it holds no renderer and no device by design -- so the drain
    // must run while `registry` still owns the ViewportPanel that minted every MeshHandle. Member
    // destruction has not started yet at this point, so every panel is alive.
    //
    // `sceneAssetLoader` is the LIVE-APP signal: a defaulted move leaves the source's unique_ptr null,
    // exactly as it leaves dialogChannel's shared_ptr empty, so a moved-from app -- whose panel
    // pointers dangle -- takes no branch here.
    if (sceneAssetLoader) {
        releaseSceneAssets();
    }
}
EditorApp::EditorApp(EditorApp&&) noexcept = default;
EditorApp& EditorApp::operator=(EditorApp&&) noexcept = default;

std::optional<EditorApp> EditorApp::create(rhi::Device& device, platform::Window& window, platform::Context& ctx,
                                           const EditorAppConfig& config) {
    // Task 2.2.5 (D5): install the console's log sink BEFORE ANYTHING LOGS, so registerEditorReflection()'s
    // tools-OFF WARN, the assets root and "shell ready" -- everything create() itself emits -- are all
    // already in the panel the first time it draws (AC-2). A local std::optional, so the
    // `return std::nullopt` below detaches it by RAII rather than by a cleanup branch. Moved into the
    // panel at registration.
    //
    // ViewportPanel's tools-OFF shader WARN is deliberately NOT in that list: it is emitted from
    // ensureInitialized() on the panel's FIRST DRAW (viewport_panel.cpp:95, called from onDraw), not
    // from its constructor. tick() pumps before the draw walk, so a record raised during frame N's draw
    // reaches the history at frame N+1 -- correct and unavoidable ordering, but it means the WARN is one
    // frame later than the create()-time lines, not present alongside them.
    std::optional<LogSinkScope> logScope;
    if (config.registerDefaultPanels) {
        logScope.emplace();
        AERO_LOG_INFO("editor: console log sink attached ({} staged / {} held)", logScope->sink()->stagingCapacity(),
                      DEFAULT_LOG_HISTORY_CAPACITY);
    }
    registerEditorReflection();  // task 2.2.2 -- unconditional, once-per-process, before ImGuiLayer
    std::optional<ImGuiLayer> layer =
        ImGuiLayer::create(device, window, ctx, config.persistLayout, config.layoutIniPath);
    if (!layer) {
        return std::nullopt;  // ImGuiLayer already logged the reason
    }

    EditorApp app(std::move(*layer), ctx, config);
    app.window = &window;                                   // task 2.5.1, F14/A19
    app.dialogChannel = std::make_shared<DialogChannel>();  // never null on a LIVE app
    // task 3.1.5: the loader holds the device and nothing else that outlives a call -- no GPU object,
    // by design (§0.26), which is what makes its position in the teardown order a non-question. The
    // renderer is NOT stored: it is passed per call, because it belongs to the ViewportPanel and may
    // not exist yet.
    app.sceneAssetLoader = std::make_unique<SceneAssetLoader>(device);
    // task E.3.3 (D5): created BEFORE the panel block below, because every consumer is handed the
    // pointer immediately after its own emplace -- once, never reconciled (the viewportPanel posture).
    app.thumbnails = std::make_unique<ThumbnailService>(&device);
    // task E.3.3: ONE picker state for the whole editor, because ImGui allows one popup at a time.
    app.assetPicker = std::make_unique<AssetPickerState>();
    // ...and the SAME device is what the service pass destroys retired TEXTURES through. The code-review
    // round found this line missing: the member kept its nullptr initialiser, so destroyRetired's third
    // branch was dead and every texture the ledger adopted through reportSlotTexture survived every
    // retire, reload, Apply nudge and project swap, reaching ~Device as a leak WARN. Nothing observed it
    // because sceneAssetDestroyCount() counts entries HANDED to destroyRetired, not handles released.
    app.sceneAssetDevice = &device;
    // task 3.1.1: a real seed, before the project-open block below -- so the FIRST tick()'s scan (the
    // reconcile block, triggered by AssetDatabase::root() starting empty) already draws real GUIDs
    // rather than the placeholder seed-0 generator's pinned sequence. Nothing scans in create() itself
    // (D12) -- the reconcile is tick()'s job alone.
    app.assetGuids = GuidGenerator::fromEntropy();
    // task 3.1.4: configure() writes the tunables AND establishes the enabled state -- it mirrors
    // cfg.enabled into the status itself. A setEnabled(config.assetWatch.enabled) beside it was here
    // originally, described in three places as "what normalises the phase"; it was provably DEAD,
    // because setEnabled early-returns on `cfg.enabled == on` and configure() has just made that true
    // by construction. Removed rather than left as a statement a future reader would trust. Nothing
    // sweeps in create() -- the first poll() is tick()'s job alone, exactly as the first scan is
    // (3.1.1 D12), and that first poll() sets the phase regardless.
    app.assetWatcher.configure(config.assetWatch);
    // The default layout is built on the FIRST DRAWN FRAME, not here (E3) — so panels registered by
    // the caller between create() and the first tick() are included.
    app.applyDefaultLayout = app.layer.wantsDefaultLayout();
    // The complement: a RESTORED layout is exactly the case buildDefaultLayout does not cover, and so
    // the case where a newly-registered panel would otherwise free-float. Never both.
    app.placeUnplacedPanels = !app.applyDefaultLayout;
    // task 2.6.1: the recents list and the project are resolved HERE -- the same position the old
    // project-root resolution occupied -- so the "assets root" INFO below keeps its exact text AND
    // its exact position among the panel records, and so AssetBrowserPanel is BORN with the right
    // root (which makes D10's reconcile a startup no-op rather than a one-frame correction).
    app.recentsPath =
        config.recentProjectsPath.empty() ? defaultRecentProjectsPath() : std::string(config.recentProjectsPath);
    // task 3.2.4 (D13): the SAME shape, one line down, and resolved here for the same reason -- one
    // resolution, one member, no call site able to fall through to the real machine-wide file. Nothing
    // CONSUMES it yet; that is deliberate, so the seam exists before anything can accidentally bypass
    // it (§S step 3).
    app.toolPrefsPath = config.toolPrefsPath.empty() ? defaultToolPrefsPath() : std::string(config.toolPrefsPath);
    // task E.3.2 (D14): the SAME shape, one line down, and resolved here for the same reason -- one
    // resolution, one member, no call site able to fall through to the real machine-wide file. The
    // DIFFERENCE from its three siblings is the persistLayout gate, and it is deliberate:
    // `persistLayout` already means "this instance owns the user's persisted editor UI state" (its own
    // comment reads "false in tests"), and a focus-routing preference IS layout-adjacent UI state. An
    // EMPTY result means read nothing and write nothing: the preference is session-only and starts at
    // its default. That is what keeps 150 of the 152 EditorApp::create call sites under tests/ from
    // touching a developer's real preference file without one of them being edited. The two that set
    // persistLayout = true must set .editorPrefsPath, exactly as they already must set .layoutIniPath.
    app.editorPrefsPath = config.editorPrefsPath.empty()
                              ? (config.persistLayout ? defaultEditorPrefsPath() : std::string{})
                              : std::string(config.editorPrefsPath);
    if (!app.editorPrefsPath.empty()) {
        bool prefsCorrupt = false;
        const EditorPrefs prefs = readEditorPrefs(app.editorPrefsPath, prefsCorrupt);
        if (prefsCorrupt) {
            // A MISSING file is defaults SILENTLY; only a file that EXISTS and does not parse gets
            // this line, or every machine that has never opened the View menu is warned on every
            // launch. The wording mirrors the tool-preferences WARN at :1834.
            AERO_LOG_WARN("editor: preferences '{}' are corrupt or unsupported; using defaults", app.editorPrefsPath);
        }
        app.contextRouter.setEnabled(prefs.focusFollowsSelection);
    }
    if (config.restoreLastProject) {
        app.recents = readRecentProjects(app.recentsPath);  // D15: NOT read at all when false (AC-34/E23)
    }
    // D0's launch resolution order: argv/config -> the FIRST recents entry that HAS a project.json ->
    // none. The startup restore tries only that ONE entry: if it fails to VALIDATE we stop and show
    // Welcome rather than cascade, because silently opening a DIFFERENT project than the one you last
    // used is worse than showing Welcome (E16/E17/AC-33).
    std::string resolved(config.projectPath);
    if (resolved.empty() && config.restoreLastProject) {
        for (const std::string& candidate : app.recents.paths) {
            if (fileExists(candidate + "/" + std::string(PROJECT_FILE_NAME))) {
                resolved = candidate;
                break;
            }
        }
    }
    if (!resolved.empty()) {
        CommandContext cmd{app.sceneWorld, app.sceneSelection, app.rootOrder};
        ProjectContext projectContext{app.project, app.projectFlow, app.recents, BUILD_ENGINE_VERSION};
        (void)openProjectPath(cmd, app.commandStack, app.session, projectContext, resolved);
        // openProjectPath logs its own ERROR or INFO. A FAILURE leaves the no-project state, which IS
        // AC-32's "the editor still opens, docks and quits" property -- not an error path to handle here.
    }

    if (config.registerDefaultPanels) {
        // task 3.1.5: both return values are now KEPT. The Hierarchy's is drained every tick for its
        // asset-drop one-shot; the Inspector's is reconciled with the database beside the Material
        // panel's. Both are non-owning and address-stable (the registry holds unique_ptrs), exactly
        // like viewportPanel below.
        app.hierarchyPanel = app.registry.emplace<HierarchyPanel>();  // task 2.2.1 -- was a PlaceholderPanel
        app.inspectorPanel = app.registry.emplace<InspectorPanel>();  // task 2.2.2 -- was a PlaceholderPanel
        // task E.3.3: SET ONCE, right after the emplace -- both point at heap objects this app holds
        // through a unique_ptr, so the addresses survive the app's own move and there is nothing to
        // reconcile (the viewportPanel posture, not the per-tick setDatabase one).
        app.inspectorPanel->setAssetPicker(app.assetPicker.get());
        app.inspectorPanel->setThumbnails(app.thumbnails.get());
        app.viewportPanel = app.registry.emplace<ViewportPanel>(device);  // task 2.2.3 -- was a PlaceholderPanel
        // task 2.2.5 -- was a PlaceholderPanel. logScope is engaged exactly when this branch runs (both
        // guards read the same const config field), but the has_value() test is NOT defensive
        // programming: bugprone-unchecked-optional-access is --warnings-as-errors on the Linux Debug lane
        // and cannot be relied on to correlate two separate ifs across an intervening move (plan C3).
        if (logScope.has_value()) {
            app.consolePanel = app.registry.emplace<ConsolePanel>(std::move(*logScope));
        }
        AERO_LOG_INFO("editor: assets root '{}'", app.project.assetsRoot());
        // task E.3.3: the device is gone from this constructor -- the store it used to build lives in
        // ThumbnailService now, lent through setThumbnails() on the next line. ONCE, not reconciled:
        // the service is a heap object whose address survives this app's own move.
        app.assetBrowserPanel = app.registry.emplace<AssetBrowserPanel>(app.project.assetsRoot());
        app.assetBrowserPanel->setThumbnails(app.thumbnails.get());
        // task 2.6.2 (D12): LAST. Inspector registers before it and therefore stays the selected tab
        // in the shared Right dock node (the Console-before-Assets property), and no existing panel's
        // index shifts, so every index-based assertion in the tree keeps its meaning. Its return value
        // is deliberately DISCARDED: nothing in tick() reaches this panel -- no pump, no render, no
        // reconcile -- and a non-owning pointer nobody dereferences is a trap.
        app.registry.emplace<ProjectSettingsPanel>(std::string(BUILD_ENGINE_VERSION));
        // task 3.2.1 (D17/F9/AC-50): registered in create(), BEFORE the first tick() -- which is
        // exactly the condition placeUnplacedPanels requires. It works off ImGui::FindWindowSettingsByID
        // + DockBuilderDockWindow, both of which act on the window NAME, so a panel that has never been
        // drawn is still docked beside the Inspector on the first frame of a RESTORED layout instead of
        // free-floating. Default VISIBLE: a default-hidden panel makes this task's whole deliverable
        // something a user must hunt for in the View menu.
        app.importDetailsPanel = app.registry.emplace<ImportDetailsPanel>();
        // task 3.4.2 (D2): the EIGHTH panel, registered LAST so no existing panel's registration index
        // shifts and the Inspector keeps the selected Right-dock tab by default. Registered in
        // create(), BEFORE the first tick() -- exactly the condition placeUnplacedPanels requires, so a
        // restored layout docks it beside the Inspector instead of free-floating (the 3.2.1
        // precedent). Default VISIBLE, for 3.2.1's reason: a default-hidden panel makes this task's
        // whole deliverable something a user must hunt for in the View menu.
        // The device is passed AT CONSTRUCTION (3.1.3's A17, a third application): the preview owns a
        // RenderTarget and a ForwardRenderer of its own, and unlike the project root a device can never
        // change during a session, so there is nothing to reconcile. Nothing is CREATED here -- the
        // preview is lazy and latched, so a session that never opens a material allocates no GPU
        // object at all (A-9/R2).
        app.materialPanel = app.registry.emplace<MaterialPanel>(device);
        // task E.3.3: SET ONCE, right after the emplace -- the Inspector's own posture, one panel over.
        app.materialPanel->setAssetPicker(app.assetPicker.get());
        app.materialPanel->setThumbnails(app.thumbnails.get());
    }

    // task 2.6.1: `&& !app.project.isOpen()` is MANDATORY, not defensive. Opening a project above went
    // through adoptProject -> newScene -> resetSceneState + seedDefaultScene, so the World already
    // holds the four seed entities; seeding again would produce EIGHT. This is 2.5.1's S5 trap in a new
    // costume. Corollary, and it is CORRECT: opening a project yields a FRESH DEFAULT scene even with
    // seedDefaultScene == false -- AC-18 says a project switch resets to one. Task E.4.1: it no longer
    // always yields FOUR entities, because openProjectPath may now restore the project's last scene on
    // top of that fresh default; the guard above is unaffected, because what it prevents is a SECOND
    // seed, not a particular count.
    if (config.seedDefaultScene && !app.project.isOpen()) {
        engine::editor::seedDefaultScene(app.sceneWorld);  // fully qualified: the config field shadows
    }

    // This is the evidence the non-interactive launch check greps for (§V5).
    AERO_LOG_INFO("editor: shell ready ({} panels, {} entities, layout: {})", app.registry.count(),
                  app.sceneWorld.entityCount(), app.applyDefaultLayout ? "default" : "restored");
    return app;
}

void EditorApp::persistProjectState() {
    // ---- THE DRAIN, FIRST (task E.4.1, code review) -----------------------------------------------
    // The OUTGOING project's pair, published by openProjectPath immediately before the adopt. It is
    // the only way a scene change made INSIDE a tick that also swapped the project can still be
    // recorded: a guarded Save on an untitled scene chains through applyDialogResult into the pending
    // OpenProject, so the save lands in the old project and the swap lands in the same call, and the
    // reconcile below sees only a ROOT change. Drained BEFORE that reconcile, so it reads the baseline
    // the pair belongs to rather than the one this tick is about to adopt.
    //
    // THE SAME DECIDER, ASKED ABOUT A DIFFERENT PAIR (PJ56). `Write` here means exactly what it means
    // below -- the baseline still names this root, and the scene against it moved -- and the two arms
    // it does not take are the drop conditions: a pending root the baseline does not name (two opens
    // in one tick) and no pending root at all (every ordinary tick). A hand-written condition beside
    // this one could drift from the reconcile's idea of what a change IS; this cannot.
    if (projectStateStep(projectFlow.outgoingStateRoot, projectStateRoot, projectFlow.outgoingStateScene,
                         projectStateScene) == ProjectStateStep::Write) {
        const std::string reason =
            writeProjectState(projectFlow.outgoingStateRoot,
                              ProjectState{.lastScene = projectFlow.outgoingStateScene, .lastSceneRecorded = true});
        if (reason.empty()) {
            ++projectStateWrites;
        } else {
            AERO_LOG_WARN("editor: could not record the last scene for project '{}' -- {}",
                          projectFlow.outgoingStateRoot, reason);
        }
    }
    // CLEARED WHETHER OR NOT ANYTHING WAS WRITTEN, and whatever the arm above decided -- the pair
    // describes ONE tick's swap and must never be reconsidered against a later tick's baseline. The
    // `recentsDirty` rule, one field over: consumed once per tick, not left to be drained twice.
    projectFlow.outgoingStateRoot.clear();
    projectFlow.outgoingStateScene.clear();

    // NAMED LOCALS, FIRST: ProjectSession::root() and SceneSession::path() both return a
    // std::string_view into a member of a live object, and both `root` and `scene` outlive the
    // full-expressions below.
    const std::string root(project.root());
    const std::string scene = projectRelativeScenePath(root, session.path());
    switch (projectStateStep(root, projectStateRoot, scene, projectStateScene)) {
        case ProjectStateStep::Nothing:
            return;
        case ProjectStateStep::AdoptBaseline:
            break;  // fall through to the adopt below, WITHOUT writing (D5)
        case ProjectStateStep::Write: {
            const std::string reason =
                writeProjectState(root, ProjectState{.lastScene = scene, .lastSceneRecorded = true});
            if (reason.empty()) {
                ++projectStateWrites;
            } else {
                AERO_LOG_WARN("editor: could not record the last scene for project '{}' -- {}", root, reason);
            }
            break;
        }
    }
    // The baseline advances WHETHER OR NOT the write succeeded (D13). Otherwise a read-only Library/
    // produces a write attempt AND a WARN on every frame, forever -- asset_database.cpp's INV-C11
    // ("in-memory regardless of whether the write succeeded"), one file over. A failure therefore costs
    // exactly ONE WARN per scene change (I182), and projectStateWriteCount() counts only successes,
    // which is what keeps it an assertion about the disk rather than about the attempt.
    projectStateRoot = root;
    projectStateScene = scene;
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
                requestGuardedQuit();  // task 2.5.1, D1: route the OS quit / window [X] through the
                                       // guard instead of quitting immediately. Observable consequence:
                                       // closing a CLEAN editor now draws one more frame before exiting
                                       // -- deliberate (AC-28), not a regression.
                break;
            case platform::EventType::WindowFocusGained:
                windowFocused = true;
                // task 3.1.4 (D6/AC-25): the alt-tab-back path, and the whole of it. Sweeps continue
                // (slower) while unfocused, so a change made in another application is usually
                // already SETTLED by the time focus returns and the first post-focus sweep fires it.
                assetWatcher.requestImmediateSweep();
                break;
            case platform::EventType::WindowFocusLost:
                windowFocused = false;
                break;
            default:
                break;  // resize is the swapchain's business
        }
    }
    if (!running) {
        // Reachable only through a DIRECT requestQuit() call (D14) -- the window [X] / OS Quit path
        // above no longer flips `running` here, it only requests the guard. Kept for the same reason
        // it always existed: a caller that quits before this tick even begins must still balance
        // ImGui correctly (no NewFrame means nothing to balance).
        presented = false;
        return false;
    }
    frameClock.tick();

    // Task 2.2.5 (D14): drain the sink EVERY frame, visible or not -- shell_ui.cpp:74-79 never calls
    // onDraw for a hidden or tabbed-away panel, and Console shares its dock node with Assets. Not an
    // ImGui call, and deliberately BEFORE the draw walk so this frame's rows include everything logged
    // before it; a record emitted DURING a draw lands next frame, which is the only safe ordering (E3).
    if (consolePanel != nullptr) {
        consolePanel->pumpLog();
    }

    // task 2.5.1: the dialog result is taken BEFORE the frame, so THIS frame's menu, modal and panels
    // all see the post-load scene -- the F12 property (2.4.1 D19), one step earlier, because the
    // RESULT is not a UI event. Guarded: a moved-from EditorApp has a null channel and is deliberately
    // inert (plan A18) rather than crashing on a null dereference.
    // task 2.6.1: everything the project half of the flow needs, built fresh each frame -- the
    // PanelContext/FileMenuContext shape, and for the same reason (D7). task 2.6.2: reads the hoisted
    // build-version constant, not the macro directly -- see its definition above (F11/D14).
    ProjectContext projectContext{project, projectFlow, recents, BUILD_ENGINE_VERSION};
    if (dialogChannel != nullptr) {
        if (const DialogResult result = dialogChannel->take(); result.ready) {
            CommandContext cmd{sceneWorld, sceneSelection, rootOrder};
            // task 2.6.1: FileDialogHost::projectRoot is a std::string_view. Binding it DIRECTLY to
            // project.scenesRoot() -- which returns BY VALUE -- leaves it dangling the instant the
            // full-expression ends. The named local is MANDATORY, not style.
            const std::string scenesRoot = project.scenesRoot();
            const FileDialogHost host{dialogChannel.get(), nativeWindowHandle(window), scenesRoot};
            applyDialogResult(cmd, commandStack, session, fileFlow, host, result, projectContext);
        }
    }

    // task 2.6.1 (D10/AC-31): RECONCILE, never push. The swap does not call setRoot() -- if it did,
    // the swap would be two things a future caller could perform half of, and a stale Asset Browser
    // root after a project change is exactly the kind of drift nobody notices until it is confusing.
    // One std::string comparison per frame, which cannot be half-performed and cannot drift, and
    // which works identically whether the project changed via the menu, the Welcome window, argv, the
    // startup restore, or a caller nobody has written yet. It is the RootOrder/Hierarchy precedent
    // (editor_app.hpp:120-124) applied to a second panel. Null-checked: registerDefaultPanels == false
    // leaves the pointer null and this a no-op (E26). ONE allocation per frame, deliberately -- the
    // title push below has allocated one on every frame since task 2.5.1.
    //
    // task 3.1.1 (D12): extended with the database's own reconcile, which runs FIRST -- the database
    // rescans before the panel's root is pushed, so the first frame after a project opens already has
    // GUIDs to show rather than displaying "no .meta" for one frame and correcting itself the next.
    // Like the project swap above (A8), this whole block LAGS a runtime project swap by exactly one
    // tick: this reconcile runs at the TOP of tick(), the swap happens inside drawShellUi ->
    // applyFileRequests, called LATER in the same tick (imgui_layer_test.cpp's I21 records the
    // identical one-tick lag for the panel-only case; I28 below re-proves it for the database).
    // `refresh` combines the panel's one-shot Refresh-button flag with EditorApp::requestAssetRescan()
    // (AC-38) -- the only way the ImGui-free GPU tier can drive a rescan without a real Refresh click.
    // The panel's DATABASE POINTER is reconciled EVERY tick, unconditionally, decoupled from the root
    // mismatch gate below: a raw pointer write has no side effects (unlike setRoot(), which clears the
    // panel's whole UI state), and gating it on the SAME mismatch as setRoot would leave it null
    // forever whenever the panel is already born with the correct root (the common case -- a project
    // opened during create()), since that mismatch never fires on tick 1.
    //
    // task 3.1.2 (§D-12/F9): the SAME block gains a SECOND one-shot, Reimport All, alongside Refresh --
    // pressing it (or its GPU-tier equivalent, requestAssetReimport()) discards the committed import
    // cache BEFORE this scan runs (AC-39: AssetDatabase::invalidateCache() clears both the in-memory
    // index and D15's own write comparand), so every asset re-hashes from scratch this pass instead of
    // taking its cached (size, mtime) fast path.
    {
        std::string wanted = project.assetsRoot();
        // task 3.1.4: HOISTED out of the rescan branch below, because setRoot() needs it first. The
        // named local is MANDATORY, not style: project.root() returns a std::string_view bound to the
        // live ProjectSession (2.6.1's FileDialogHost::projectRoot lesson).
        const std::string projectRootForScan = std::string(project.root());
        // task 3.4.2 (AC-15/the INV-6 family): captured HERE, before `wanted` is moved into rescan()
        // below, so the material session resets in the SAME operation that reconciles the browser root
        // rather than through a second path that half-performs a swap. True exactly when the assets
        // root the database is holding is not the one the open project names -- which is a project
        // swap, and, on the very first tick with a project open, a reset of an already-empty session.
        const bool assetsRootChanged = assetDatabase.root() != wanted;
        // task 3.1.4: the watcher's roots are reconciled from the SAME `wanted` string, in the SAME
        // block -- the FOURTH occupant (2.6.1's panel root, 3.1.1's database, 3.1.3's report, 3.1.4's
        // watcher). 3.1.2's D9 two-parameter rule applies here exactly as it does to rescan():
        // <assetsRoot>/.. is wrong the moment paths.assets is nested or ".", and getting it wrong
        // computes the wrong Library/ path -- the ONE exclusion no normal project can test (D4/AC-16).
        if (assetWatcher.root() != wanted) {
            assetWatcher.setRoot(projectRootForScan, wanted);
        }
        // task 3.1.4: the poll runs BEFORE the drains below, so a change it detects becomes THIS tick's
        // rescan rather than next tick's. It is the ONLY statement in this block that touches the disk
        // outside rescan() itself, it opens at most cfg.dirsPerPoll directories, and it reads no file
        // (AC-23).
        const bool watchFired = assetWatcher.poll(frameClock.deltaSeconds());

        // F9 (3.1.2), extending code-review finding 4 (3.1.1): BOTH one-shots are drained FIRST,
        // unconditionally, each as its OWN statement -- never on the right of a `flagAlreadyKnown ||
        // ...` expression, whose short-circuit would then skip the drain call entirely and leave that
        // one-shot set, un-consumed, to trigger a SECOND, redundant scan next frame. This tree has
        // shipped that exact bug once already (I30 is its mechanical proof).
        const bool panelRefresh = assetBrowserPanel != nullptr && assetBrowserPanel->takeRescanRequest();
        const bool panelReimport = assetBrowserPanel != nullptr && assetBrowserPanel->takeReimportRequest();
        // task 3.1.3 (D12): a THIRD one-shot, drained as its OWN statement for the identical F9 reason
        // -- putting takeOrphanDeleteRequest() on the right of a `||` would skip the drain whenever an
        // earlier term was already true, stranding the request until the next frame. I42 is its
        // mechanical proof. TWO sources feed it (the panel's own confirmed modal, and EditorApp's own
        // requestOrphanDelete() -- the GPU tier's only channel, §Q Q1), each drained here as its own
        // statement, unconditionally, before either is inspected.
        const std::string panelOrphanToDelete =
            assetBrowserPanel != nullptr ? assetBrowserPanel->takeOrphanDeleteRequest() : std::string{};
        const std::string editorOrphanToDelete = std::move(requestedOrphanDelete);
        requestedOrphanDelete.clear();
        const std::string& orphanToDelete = !panelOrphanToDelete.empty() ? panelOrphanToDelete : editorOrphanToDelete;
        bool orphanHandled = false;
        if (!orphanToDelete.empty()) {
            const OrphanDeleteResult result = deleteOrphanMeta(assetDatabase.root(), orphanToDelete);
            if (result.deleted) {
                AERO_LOG_INFO("editor: deleted orphaned sidecar '{}'", orphanToDelete);
            } else {
                AERO_LOG_WARN("editor: refused to delete '{}' -- {}", orphanToDelete, result.message);
            }
            // A REFUSAL still rescans: every refusal reason (Missing, AssetPresent, NotAMeta) means
            // "the tree changed under us", so the Issues list the user is looking at is stale either
            // way. The deletion therefore happens BEFORE the refresh/reimport test below, so the
            // delete and the rescan that observes it are ONE pass, not two ticks apart (seed S28).
            orphanHandled = true;
        }
        // task 3.4.2 (D9/AC-5): a FOURTH one-shot in this block, drained as its OWN statement for the
        // identical F9 reason -- an engaged optional carrying "" (the assets root) is a legitimate
        // request, which is exactly why the channel is an optional and not a string.
        std::optional<std::string> createMaterialDir;
        if (assetBrowserPanel != nullptr) {
            createMaterialDir = assetBrowserPanel->takeCreateMaterialRequest();
        }
        bool materialCreated = false;
        if (createMaterialDir.has_value()) {
            // The orphan-delete posture verbatim: the write happens BEFORE the refresh test below, so
            // the new file and the scan that mints its `.meta` are ONE pass rather than two ticks apart.
            materialCreated = createMaterialAsset(*createMaterialDir);
        }
        // task E.4.3: four MORE one-shots in this block, each drained as its OWN statement for the
        // identical F9 reason -- and all four inside ONE `if (assetBrowserPanel != nullptr)` block,
        // none on the right of a `&&`. The existing panelRefresh/panelReimport lines use
        // `!= nullptr &&` because their takers return bool, and short-circuiting a bool taker is the
        // exact bug F9 names; those two are pre-existing and untouched, but the new four must not
        // copy that shape. A moved-from optional is still ENGAGED, so every taker resets afterwards.
        std::optional<std::string> newFolderDir;
        std::optional<AssetRenameRequest> renameReq;
        std::optional<AssetMoveRequest> moveReq;
        std::string deleteRel;
        if (assetBrowserPanel != nullptr) {
            newFolderDir = assetBrowserPanel->takeNewFolderRequest();
            renameReq = assetBrowserPanel->takeRenameRequest();
            moveReq = assetBrowserPanel->takeMoveRequest();
            deleteRel = assetBrowserPanel->takeDeleteRequest();
        }
        bool fileOpPerformed = false;
        if (newFolderDir.has_value()) {
            (void)createAssetFolder(*newFolderDir);
            fileOpPerformed = true;
        }
        if (renameReq.has_value()) {
            (void)renameAssetEntry(renameReq->path, renameReq->newLeaf);
            fileOpPerformed = true;
        }
        if (moveReq.has_value()) {
            (void)moveAssetEntry(moveReq->path, moveReq->destinationDir);
            fileOpPerformed = true;
        }
        if (!deleteRel.empty()) {
            (void)deleteAssetEntry(deleteRel);
            fileOpPerformed = true;
        }
        // task E.4.3: TRUE when any of the four operations above was ATTEMPTED -- performed OR
        // refused -- on orphanHandled's rule (a refusal means the tree changed under us, so the
        // listing is stale either way). materialCreated above is deliberately NOT this shape: it is
        // 3.4.2's return value and its refusal-does-not-rescan behaviour is that task's contract,
        // pinned by that task's cases.
        const bool refresh = assetRescanRequested || panelRefresh || orphanHandled || watchFired ||
                             materialCreated      // task 3.1.4
                             || fileOpPerformed;  // task E.4.3
        const bool reimport = assetReimportRequested || panelReimport;
        assetRescanRequested = false;
        assetReimportRequested = false;
        if (reimport) {
            // AC-39/AC-35: discard the committed index BEFORE the scan below runs, so every asset
            // re-hashes from scratch this pass rather than taking its cached (size, mtime) fast path.
            // What makes that work is the one-shot invalidateCache() arms, which the next scan's phase
            // 3 consults to skip its reload -- without it the file on disk would simply be read back
            // in before phase 4 ever ran (AssetDatabase's own comments on the method and the flag).
            assetDatabase.invalidateCache();
        }
        if (assetsRootChanged) {
            materialSession.resetForProjectSwap();  // task 3.4.2 -- see the capture above
            // task 3.1.5 (§0.26): the SAME swap point, and it runs while the viewport panel is alive --
            // the renderer that minted a MeshHandle is the only thing that can release it.
            releaseSceneAssets();
        }
        if (assetsRootChanged || refresh || reimport) {
            // task 3.1.2: rescan now takes the project root and the assets root as two SEPARATE
            // parameters (D-9) -- <assetsRoot>/.. is wrong the moment paths.assets is nested or ".",
            // and deriving it would put the cache inside the user's own asset tree (A7/AC-38). The
            // project root is the named local hoisted to the top of this block, above (task 3.1.4).
            // task 3.1.3: RETAINED, not discarded -- the Issues section reads this same report through
            // the panel's own reconciled pointer, below.
            lastAssetReport = assetDatabase.rescan(projectRootForScan, std::move(wanted), assetGuids);
            logAssetScan(assetDatabase.root(), lastAssetReport);  // INV-A3: the ONLY logging site
            if (watchFired) {
                // task 3.1.4 (AC-37): the watcher's ONE line, and the ONLY place anything about the
                // watcher is ever logged. COUNTS, not paths -- a 500-file batch must not print 500
                // lines -- plus the FIRST changed path as an anchor, which is what makes the report
                // actionable without being a dump.
                // d.changes.front() is safe ONLY because watchFired is true, and poll() returns true
                // only when diff.changes is non-empty (finishSweep returns false on an empty change
                // set). This coupling is exactly what a later edit breaks silently.
                const WatchDiff& d = assetWatcher.lastDiff();
                AERO_LOG_INFO("assets: watcher detected {} change(s) (first: '{}') -- rescanning", d.changes.size(),
                              d.changes.front().path);
            } else {
                // task 3.1.4: a rescan we did NOT cause -- adopt the last completed sweep as the
                // baseline so the watcher does not report those same changes again (Q7/AC-27).
                assetWatcher.noteExternalScan();
            }
        }
        if (assetBrowserPanel != nullptr) {
            if (assetBrowserPanel->root() != assetDatabase.root()) {
                assetBrowserPanel->setRoot(assetDatabase.root());
                // task E.3.3: the two lines setRoot() used to perform itself, at the SAME point, so
                // I40's "resident 0 after a project swap" keeps its exact timing. The service is
                // shared now, so the panel is no longer the right place to decide this.
                thumbnails->clear();
            }
            assetBrowserPanel->setDatabase(&assetDatabase);
            // task 3.1.3: unconditional, same block, same reasoning as setDatabase() above (F14).
            assetBrowserPanel->setScanReport(&lastAssetReport);
            assetBrowserPanel->setWatchStatus(&assetWatcher.status());  // task 3.1.4 (D10)
            // task 3.1.4 (D8/AC-30): ONE signal drives BOTH downstream refreshes, for EVERY trigger --
            // the watcher, both buttons, requestAssetRescan(), an orphan delete, and a root change
            // alike.
            if (assetDatabase.generation() != lastAssetGeneration) {
                lastAssetGeneration = assetDatabase.generation();
                assetBrowserPanel->invalidateListings();
                thumbnails->noteDatabaseRescanned();  // task E.3.3: was the panel's own flag
            }
            // task 3.1.4: F9 -- drained as its OWN statement, unconditionally, BEFORE it is inspected.
            // This tree has shipped the `||`-short-circuit bug once (I30 is its mechanical proof) and
            // guarded against it three times since; this is the fourth.
            const std::optional<bool> watchToggle = assetBrowserPanel->takeWatchToggleRequest();
            if (watchToggle.has_value()) {
                assetWatcher.setEnabled(*watchToggle);
            }
        }
        // task 3.2.1 (D17/F10): the FIFTH occupant of this block -- 2.6.1's panel root, 3.1.1's
        // database, 3.1.3's report, 3.1.4's watcher, and now the import session. EXTEND, NEVER TWIN:
        // 3.1.4's seed S15 reddened TWELVE tier-0 cases by skipping ONE statement in here.
        //
        // A RECONCILE, never a push (2.6.1 D10, a third application): EditorApp COMPARES and calls
        // setTarget only on a mismatch. generation() is AssetDatabase's FOURTH consumer -- a rescan
        // from ANY trigger invalidates the cached result, so the next tick re-imports exactly once
        // (AC-47).
        if (assetBrowserPanel != nullptr) {
            const std::string& selected = assetBrowserPanel->selection();
            const std::uint64_t gen = assetDatabase.generation();
            if (importSession.target() != selected || importSession.generation() != gen) {
                importSession.setTarget(selected, gen);
            }
        }
        // F9, a FIFTH application: EVERY one-shot is drained as its OWN statement, unconditionally,
        // BEFORE it is inspected. A `panelApply || editorApply` expression would short-circuit past
        // the panel's drain and strand the request until the next frame. I30 is that bug's mechanical
        // proof and this tree has shipped it once.
        const bool panelApply = importDetailsPanel != nullptr && importDetailsPanel->takeApplyRequest();
        const bool panelRevert = importDetailsPanel != nullptr && importDetailsPanel->takeRevertRequest();
        std::optional<ImportSettings> panelSettings;
        if (importDetailsPanel != nullptr) {
            panelSettings = importDetailsPanel->takePendingSettings();
        }
        // DEVIATION from the plan's own literal §D-9 text (logged): `ImportSettings` (float + 3 bools,
        // no user-provided special members) is trivially copyable, so is `std::optional<ImportSettings>`
        // -- clang-tidy's performance-move-const-arg (--warnings-as-errors on the Linux lane) flags
        // `std::move` here as having no effect. A plain copy, followed by the reset below, is IDENTICAL
        // at runtime.
        std::optional<ImportSettings> editorSettings = requestedImportSettings;
        requestedImportSettings.reset();
        const bool editorApply = requestedImportApply;
        requestedImportApply = false;
        if (panelSettings.has_value()) {
            importSession.setPendingSettings(*panelSettings);
        }
        if (editorSettings.has_value()) {
            importSession.setPendingSettings(*editorSettings);
        }
        if (panelRevert) {
            importSession.revertSettings();
        }
        if (panelApply || editorApply) {
            const std::string error = importSession.applySettings(project.assetsRoot());
            if (error.empty()) {
                AERO_LOG_INFO("assets: saved import settings for '{}'", importSession.target());
                assetRescanRequested = true;  // AC-51/AC-52: MetaChanged next scan, ONE re-import
            } else {
                AERO_LOG_WARN("assets: could not save import settings for '{}' -- {}", importSession.target(), error);
            }
        }
        // task 3.2.4: the SIXTH occupant of this block. F9, a SIXTH application: EVERY one-shot is
        // drained as its OWN STATEMENT, unconditionally, BEFORE it is inspected. A `panelX || editorX`
        // expression would short-circuit past the panel's drain and strand the request until the next
        // frame -- I30 is that bug's mechanical proof and this tree has shipped it once.
        const bool panelConvert = importDetailsPanel != nullptr && importDetailsPanel->takeConvertRequest();
        const bool panelCancel = importDetailsPanel != nullptr && importDetailsPanel->takeCancelRequest();
        const bool panelLocate = importDetailsPanel != nullptr && importDetailsPanel->takeLocateRequest();
        const bool panelRedetect = importDetailsPanel != nullptr && importDetailsPanel->takeRedetectRequest();
        const bool editorConvert = requestedBlenderConvert;
        requestedBlenderConvert = false;
        const bool editorCancel = requestedBlenderCancel;
        requestedBlenderCancel = false;
        const bool editorRedetect = requestedBlenderRedetect;
        requestedBlenderRedetect = false;
        const std::string editorLocate = std::move(requestedBlenderLocate);
        requestedBlenderLocate.clear();
        // The DIALOG's own answer, delivered by applyDialogResult EARLIER IN THIS SAME TICK -- so a
        // picked path is applied in the tick it arrives, never one frame late.
        const std::string dialogLocate = std::move(fileFlow.pickedBlenderPath);
        fileFlow.pickedBlenderPath.clear();

        if (panelConvert || editorConvert) {
            importSession.requestConversion();
        }
        if (panelCancel || editorCancel) {
            importSession.cancelConversion();
        }
        if (!editorLocate.empty()) {
            applyBlenderOverride(editorLocate);
        }
        if (!dialogLocate.empty()) {
            applyBlenderOverride(dialogLocate);
        }
        if (panelRedetect || editorRedetect) {
            applyBlenderOverride("");  // clears the override and re-resolves from scratch
        }
        if (panelLocate && dialogChannel != nullptr && fileFlow.dialog == DialogKind::None) {
            // The scene_session.cpp guard shape verbatim: DialogChannel holds ONE slot, so a second
            // dialog launched while one is in flight would have its result silently overwrite the
            // first's.
            fileFlow.dialog = DialogKind::BlenderBinary;
            launchLocateBlenderDialog(dialogChannel, nativeWindowHandle(window), importSession.blender().binaryPath());
        }
        // §A-9: LAZY, and on NEED rather than on selection. A pure cache hit never reaches here, so it
        // reads no environment variable, stats no candidate path and spawns nothing (AC-22).
        if (importSession.state() == SessionState::NeedsConversion &&
            importSession.blender().state() == BlenderState::Unknown && importSession.targetHasIdentity()) {
            resolveBlender();
        }
        // The two remaining log lines, on a TRANSITION rather than per frame -- a Converting session
        // logging every frame would be a hundred lines a second. A cache HIT logs nothing at all: it
        // must be as silent as a steady-state scan (INV-C5's posture, applied here).
        const int blenderSessionState = static_cast<int>(importSession.state());
        if (blenderSessionState != lastBlenderSessionState) {
            if (importSession.state() == SessionState::Converting) {
                AERO_LOG_INFO("assets: converting '{}' with Blender {}", importSession.target(),
                              importSession.blender().versionString().empty()
                                  ? std::string("(version unknown)")
                                  : importSession.blender().versionString());
            } else if (importSession.state() == SessionState::ConversionFailed) {
                AERO_LOG_WARN("assets: the Blender conversion of '{}' failed -- {} (log: '{}')", importSession.target(),
                              importSession.blender().message(), importSession.blender().logPath());
            }
            lastBlenderSessionState = blenderSessionState;
        }
        // task 3.4.2 (D3/A-4): the SIXTH occupant of this block -- 2.6.1's panel root, 3.1.1's
        // database, 3.1.3's report, 3.1.4's watcher, 3.2.1's import session, and now the material
        // session. EXTEND, NEVER TWIN: 3.1.4's seed S15 reddened TWELVE tier-0 cases by skipping ONE
        // statement in here, and this block is now the whole of the editor's per-frame reconciliation.
        //
        // STICKY, unlike the import session above, and the asymmetry is D3 rather than an oversight:
        // MaterialSession::reconcile retargets ONLY on a different, existing *.aeromat, so clicking
        // through the browser to find a texture to reference cannot tear down an edit session.
        //
        // assetDatabase.root() rather than project.assetsRoot(): the two are the SAME string by
        // construction (INV-A9/A16 -- both are reconciled from `wanted` in this very block), and the
        // database's accessor returns a const reference, so nothing here can bind a string_view to a
        // by-value temporary (2.6.1's FileDialogHost::projectRoot lesson). It is also the more correct
        // of the two: the session resolves paths that are relative to the root the RECORDS are
        // relative to.
        if (assetBrowserPanel != nullptr) {
            materialSession.reconcile(assetBrowserPanel->selection(), assetDatabase.generation(), assetDatabase,
                                      assetDatabase.root());
        }
        // F9, a SEVENTH application: EVERY one-shot is drained as its OWN statement, unconditionally,
        // BEFORE it is inspected. A `panelX || editorX` expression would short-circuit past a drain
        // and strand the request until the next frame -- I30 is that bug's mechanical proof.
        std::optional<MaterialDocument> panelMaterialEdit;
        bool panelMaterialApply = false;
        bool panelMaterialRevert = false;
        if (materialPanel != nullptr) {
            panelMaterialEdit = materialPanel->takePendingDocument();
            panelMaterialApply = materialPanel->takeApplyRequest();
            panelMaterialRevert = materialPanel->takeRevertRequest();
        }
        std::optional<MaterialDocument> materialEdit = std::move(requestedMaterialDocument);
        requestedMaterialDocument.reset();
        const bool materialApply = requestedMaterialApply;
        requestedMaterialApply = false;
        const bool materialRevert = requestedMaterialRevert;
        requestedMaterialRevert = false;
        // The PANEL's edit first, then the request seam's: one pending slot, last writer wins, and the
        // seam is what a test drives, so it must be able to override what a widget happened to record.
        if (panelMaterialEdit.has_value()) {
            materialSession.edit(*panelMaterialEdit);
        }
        if (materialEdit.has_value()) {
            materialSession.edit(*materialEdit);
        }
        if (panelMaterialApply || materialApply) {
            materialSession.requestApply();
        }
        if (panelMaterialRevert || materialRevert) {
            materialSession.requestRevert();
        }
        materialSession.service(assetDatabase, assetDatabase.root());
        // Reconciled BEFORE drawShellUi, unlike ImportDetailsPanel's own setSession (which runs in the
        // post-draw slot): the material panel has no service pass of its own to run afterwards, so
        // there is nothing to be gained by making its first drawn frame see a null pointer. The
        // database is reconciled the same way and for the 3.1.1 D13 reason -- a POINTER, refreshed
        // every tick, never a reference member bound to a pre-move address.
        if (materialPanel != nullptr) {
            materialPanel->setSession(&materialSession);
            materialPanel->setDatabase(&assetDatabase);
        }
        // task 3.1.5: ONE line beside the Material panel's, not a new occupant. The Inspector's Guid
        // row resolves a reference to a record; a null pointer is a legal state it has a sentence for.
        if (inspectorPanel != nullptr) {
            inspectorPanel->setDatabase(&assetDatabase);
        }

        // task 3.1.5 (D12/§0.7): the SEVENTH occupant of this block -- 2.6.1's panel root, 3.1.1's
        // database, 3.1.3's report, 3.1.4's watcher, 3.2.1's import session, 3.4.2's material session,
        // and now the drop drain. EXTEND, NEVER TWIN. It sits at the END of the block, AFTER everything
        // that can retarget the material session and BEFORE the draw walk -- so a texture drop's effect
        // is judged against this frame's session, and an entity created by a model drop appears in the
        // Hierarchy in the same frame the drop landed.
        //
        // task E.3.2: the drop drain is no longer the last statement in this block -- the context
        // router's three observations follow it, because they must see the session state every arm
        // above has finished writing. It is still the last thing that CHANGES anything.
        //
        // F9's rule, an EIGHTH application: every one-shot is drained as its OWN statement,
        // unconditionally, BEFORE it is inspected. A `panelX || editorX` expression would short-circuit
        // past a drain and strand the request until the next frame.
        std::optional<HierarchyAssetDrop> panelHierarchyDrop;
        std::optional<ViewportAssetDrop> panelViewportDrop;
        std::optional<MaterialSlotTextureDrop> panelSlotDrop;
        if (hierarchyPanel != nullptr) {
            panelHierarchyDrop = hierarchyPanel->takeAssetDropRequest();
        }
        if (viewportPanel != nullptr) {
            panelViewportDrop = viewportPanel->takeAssetDropRequest();
        }
        if (materialPanel != nullptr) {
            panelSlotDrop = materialPanel->takeAssetDropRequest();
        }
        // Copied rather than moved: both structs are trivially copyable, so a move is a copy with a
        // misleading name (performance-move-const-arg is --warnings-as-errors on the Linux lane). The
        // reset is what drains, and it is its own statement either way.
        const std::optional<HierarchyAssetDrop> seamHierarchyDrop = requestedHierarchyDrop;
        requestedHierarchyDrop.reset();
        const std::optional<ViewportAssetDrop> seamViewportDrop = requestedViewportDrop;
        requestedViewportDrop.reset();
        // The PANEL's record first, then the seam's: one pending slot, last writer wins, and the seam is
        // what a test drives, so it must be able to override what a widget happened to record (the
        // 3.4.2 rule). Surface order Hierarchy -> Viewport -> Material throughout.
        if (panelHierarchyDrop.has_value()) {
            applyHierarchyDrop(*panelHierarchyDrop);
        }
        if (seamHierarchyDrop.has_value()) {
            applyHierarchyDrop(*seamHierarchyDrop);
        }
        if (panelViewportDrop.has_value()) {
            applyViewportDrop(*panelViewportDrop);
        }
        if (seamViewportDrop.has_value()) {
            applyViewportDrop(*seamViewportDrop);
        }
        // The Material surface has NO EditorApp-side seam local, and that is §0.4's stated asymmetry
        // rather than an omission: requestMaterialSlotTextureDrop forwards STRAIGHT to the panel,
        // because the seam must reach the panel's own frame copy and no one-shot held here can. So the
        // one drain below sees a driven drop and a real one identically.
        if (panelSlotDrop.has_value()) {
            applySlotDrop(*panelSlotDrop);
        }

        // task E.3.2 (D1/D4): the EIGHTH occupant of this block -- 2.6.1's panel root, 3.1.1's
        // database, 3.1.3's report, 3.1.4's watcher, 3.2.1's import session, 3.4.2's material session,
        // 3.1.5's drop drain, and now the context router. EXTEND, NEVER TWIN: 3.1.4's seed S15
        // reddened TWELVE tier-0 cases by skipping ONE statement in here.
        //
        // LAST, after every session it reads, so each observation sees THIS tick's settled answer --
        // MaterialSession::reconcile above is synchronous, so targetPath() is current the moment it
        // returns. The three calls are ORDER-INDEPENDENT by construction (RouteSource's numeric order
        // is the priority, applied inside latch()); they are written in priority order for reading
        // only, and RT22/RT23 drive both orders and assert the same winner.
        //
        // NOTHING HERE PREDICTS. The material arm asks MaterialSession's own sticky retarget; the
        // import arm reads the SessionState the session itself wrote, and deliberately does nothing at
        // all on the Idle tick that setTarget leaves behind (model_import_session.cpp:88) -- which is
        // why a browser click takes two ticks to raise Import Details and zero duplicated predicates.
        contextRouter.observeEntitySelection(sceneSelection.revision(), !sceneSelection.empty());
        contextRouter.observeMaterialTarget(materialSession.targetPath());
        contextRouter.observeImportTarget(importSession.target(), importSession.state() != SessionState::Idle,
                                          importSession.state() != SessionState::NotImportable);
    }
    if (window != nullptr) {
        std::string title = session.windowTitle(!commandStack.isClean(), project.name());
        if (title != lastTitle) {  // D16: push the title only when it CHANGES
            lastTitle = std::move(title);
            window->setTitle(lastTitle);
        }
    }
    // The recents list is flushed ONLY when it changed -- never per-frame file I/O, ever.
    if (projectFlow.recentsDirty) {
        projectFlow.recentsDirty = false;
        writeRecentProjects(recentsPath, recents);  // one WARN on failure; the editor keeps running
    }  // and the project stays open (AC-24)
    // task E.3.2: written ONLY when the value changed -- never per-frame file I/O, ever (the
    // recentsDirty idiom above, a second instance). An EMPTY editorPrefsPath means this instance does
    // not persist preferences at all (D14): config.persistLayout was false, which is every test in the
    // tree but two. The dirty flag is cleared whether or not a file is written, so a non-persisting
    // instance does not re-attempt a write it will never perform.
    if (editorPrefsDirty) {
        editorPrefsDirty = false;
        if (!editorPrefsPath.empty()) {
            const EditorPrefs prefs{.focusFollowsSelection = contextRouter.enabled()};
            if (const std::string reason = writeEditorPrefs(editorPrefsPath, prefs); !reason.empty()) {
                AERO_LOG_WARN("editor: could not write preferences '{}' -- {}", editorPrefsPath, reason);
            }
        }
    }

    layer.beginFrame();
    ShellUiState ui{.applyDefaultLayout = applyDefaultLayout,
                    .placeUnplacedPanels = placeUnplacedPanels,
                    .undoRequested = undoRequested,
                    .redoRequested = redoRequested,
                    .focusPanelId = requestedPanelFocus,
                    // task E.3.2: the reconcile block above has already run, so a route latched THIS
                    // tick is carried into THIS tick's frame. routeToggleRequested and routeOutcome
                    // are OUT-only and take their defaults.
                    .routeSource = contextRouter.pending(),
                    .routeEnabled = contextRouter.enabled()};
    // Consumed: a request never survives the tick that carried it. Unlike applyDefaultLayout below,
    // these are NOT read back out of `ui` -- drawShellUi clears them as it applies them, and reading
    // them back would re-arm the request every frame (task 2.4.1).
    undoRequested = false;
    redoRequested = false;
    requestedPanelFocus.clear();  // code-review BLOCKING-1 test seam -- the SAME "never re-armed" rule
    // rebuilt per frame (D7); deltaSeconds is this frame's SPIKE-CLAMPED delta (task 2.3.1);
    // commandStack is the editor's ONE undo history (task 2.4.1 D7); rootOrder is the editor's ONE
    // display order among root entities (task 2.4.2 D10); project is the open project (task 2.6.2
    // D1), CONST so no panel can swap it
    PanelContext panelContext{sceneWorld, sceneSelection, commandStack, rootOrder, project, frameClock.deltaSeconds()};
    // task 2.5.1 (plan A14): everything the File menu needs that PanelContext deliberately does not
    // carry (D17), built fresh each frame exactly like panelContext above.
    // task 2.6.1: the SAME named-local requirement as the drain above -- FileDialogHost::projectRoot
    // is a string_view and project.scenesRoot() returns by value.
    const std::string scenesRootForMenu = project.scenesRoot();
    FileMenuContext fileMenu{session, fileFlow,
                             FileDialogHost{dialogChannel.get(), nativeWindowHandle(window), scenesRootForMenu},
                             projectContext};
    drawShellUi(registry, panelContext, ui, fileMenu);  // menu bar -> dockspace -> panels
    applyDefaultLayout = ui.applyDefaultLayout;         // drawShellUi clears it once consumed, and re-sets
                                                        // it for View > Reset Layout
    placeUnplacedPanels = ui.placeUnplacedPanels;       // cleared once consumed; nothing ever re-arms it
    // task E.3.2. The toggle is adopted ONLY when the checkbox was actually flipped -- routeEnabled is
    // an in/out field, and adopting it unconditionally would be harmless today and a silent
    // re-introduction of a stale value the moment anything else writes it.
    if (ui.routeToggleRequested) {
        contextRouter.setEnabled(ui.routeEnabled);
        editorPrefsDirty = true;
    }
    // Counters FIRST, and gated on a real source, so a Drop of None -- the common case, on every
    // single frame -- counts nothing and focusRouteDropCount() means "how many real routes were
    // refused" rather than "how many frames ran".
    if (ui.routeSource != RouteSource::None) {
        switch (ui.routeOutcome) {
            case RouteOutcome::Apply:
                ++focusRouteApplies;
                lastRoutedPanel = routedPanelId(ui.routeSource);
                break;
            case RouteOutcome::Hold:
                ++focusRouteHolds;
                break;
            case RouteOutcome::Drop:
                ++focusRouteDrops;
                break;
        }
    }
    // Hold KEEPS the latch for the next tick; Apply and Drop are both terminal. The check is on the
    // OUTCOME, not on the source, so a Drop of None -- which never entered the switch above -- still
    // clears a latch that is already None. Idempotent, and one statement.
    if (ui.routeOutcome != RouteOutcome::Hold) {
        contextRouter.clearPending();
    }
    // D3: the offscreen scene pass runs AFTER the draw walk (only it knows this frame's panel size,
    // which is what removes the one-frame resize lag) and BEFORE endFrame (ImGui's command buffer is
    // acquired and submitted there; ours must be submitted first -- F8's ordering guarantee, and F7
    // leaves the colour texture sampler-readable the instant our pass ends). NOT an ImGui call.
    if (viewportPanel != nullptr) {
        viewportPanel->renderScene(sceneWorld);
    }
    // task 3.1.3 (D8): the thumbnail service pass is the ONLY thumbnail mutator, and it runs here --
    // the SECOND occupant of the slot between drawShellUi and endFrame, the renderScene precedent.
    // task E.3.3 moved the body into ThumbnailService and kept this statement's position exactly,
    // because THREE consumers share it now rather than one panel owning it.
    if (thumbnails != nullptr) {
        thumbnails->service(assetDatabase);
    }
    // task E.3.3: THE TICK IN WHICH NO FIELD CLAIMED THE POPUP. Every asset field writes the
    // observables OWNER-ONLY, so a tick in which the owning row was not drawn at all -- the selection
    // moved to an entity without that component, the panel was tabbed away or hidden -- would otherwise
    // leave them frozen at the last frame that DID draw. ImGui has already closed the popup by then
    // (its window stopped being submitted), so assetPickerOpen() would keep reporting an open popup
    // that is gone, against editor_app.hpp's "the LAST DRAWN frame's answer", and the four live
    // one-shots would never be dropped again because `openFieldKey` stays set forever.
    //
    // It belongs HERE and not in the widget: only a slot that runs after the WHOLE draw walk can know
    // that nobody claimed it, and this is that slot.
    if (assetPicker != nullptr && !std::exchange(assetPicker->ownerDrewThisTick, false)) {
        assetPicker->openValue = false;
        assetPicker->candidateCountValue = 0;
        assetPicker->cursorValue = 0;
        assetPicker->openHostId.clear();
        assetPicker->openFieldKey.clear();
    }
    // task 3.2.1 (D16/AC-48/INV-M12): OUTSIDE the ImGui draw walk, in the SAME SLOT as renderScene()
    // and serviceThumbnails(). A Full import is SYNCHRONOUS and may visibly hitch on a large model --
    // accepted, and what every editor in this class does on a deliberate click. It runs AT MOST ONCE
    // per (selection, generation) pair; ten further ticks cost ten early returns.
    //
    // NEVER CALL THIS FROM onDraw(). Like 3.1.3's BLOCKING-1 and 3.1.4's D9, NO AUTOMATED TIER CAN SEE
    // THE GENERAL-CASE VIOLATION -- I60 reads this file's own source text, and manual validation row 8
    // is the only behavioural cover. GET IT RIGHT BY CONSTRUCTION.
    // task 3.2.4: ONE new argument, and this call does NOT move. `frameClock.deltaSeconds()` is this
    // frame's spike-clamped delta -- the same value PanelContext already carries -- and it is the only
    // clock the Blender state machine ever sees: its timeout and its force-kill escalation are both
    // driven by injected time, never by a wall clock inside the service (AC-18).
    importSession.service(project.assetsRoot(), assetDatabase, frameClock.deltaSeconds());
    if (importDetailsPanel != nullptr) {
        importDetailsPanel->setSession(&importSession);
    }
    // task 3.4.2 (D6/INV-5): the FOURTH occupant of this slot -- renderScene, serviceThumbnails, the
    // import session, and now the material preview. OUTSIDE the ImGui draw walk and BEFORE endFrame,
    // so our command buffer is submitted before ImGui's and the preview's colour texture is
    // sampler-ready by the time ImGui samples it (render_target.hpp's own synchronisation note).
    //
    // NEVER CALL THIS FROM onDraw(). Every preview GPU create and every preview GPU destroy lives
    // inside it -- with ONE deliberate exception the code-review round established: the colour target's
    // own reallocation, which must happen in the draw walk, before the handle ImGui records is read
    // (MaterialPreview::prepareFrame states why). SDL_ReleaseGPUTexture frees SYNCHRONOUSLY on Vulkan
    // and D3D12 while deferring only on Metal -- the 3.1.3 BLOCKING-1 class, deterministic on two
    // platforms and invisible on the one with a completed validation pass. I95 pins this call site's
    // position in this file's own source text; no runtime tier here can see the general-case violation.
    //
    // assetDatabase.root() rather than project.assetsRoot(), for the reconcile block's own reason: the
    // two are the same string by construction and the accessor returns a const reference, so nothing
    // binds a string_view to a by-value temporary.
    //
    // task 3.6.3: ONE new argument, and this call does NOT move. The params come from the VIEWPORT,
    // which owns the UI that mutates them, so the viewport and the preview can never grade the same
    // material differently. Nothing at runtime can see the two diverge -- I106's source-text pin is
    // this line's only mechanical witness. The nullptr arm matters: viewportPanel is a Panel* looked up
    // from the registry and is legally null in a test that registers only the Material panel, and
    // render::TonemapParams{} is {1.0F, AcesApprox} -- already sanitized by construction.
    if (materialPanel != nullptr) {
        // task E.2.4 (D2/D8): the preview's lighting is the BRIDGE's resolution -- the same two
        // functions buildRenderView calls -- run over the World the viewport just rendered THIS frame,
        // post-undo (drawShellUi applied undo/redo above renderScene). Resolved HERE and handed down
        // as a VALUE, so the panel and the preview stay World-free: resolving inside the service pass
        // would hand a non-const World to code that has been scene-free since 3.4.2, to save one
        // struct. Neither resolver logs on any path; the viewport's SceneRenderer owns the latched
        // "multiple Environments" / "multiple DirectionalLights" WARNs, so this second resolution per
        // frame produces no second WARN.
        const scene_render::ResolvedEnvironment environment = scene_render::resolveEnvironment(sceneWorld);
        const scene_render::ResolvedDirectionalLight sun = scene_render::resolveDirectionalLight(sceneWorld);
        const MaterialPreviewLighting lighting{
            .environment = environment.data, .sun = sun.data, .hasSun = sun.entity.valid()};
        materialPanel->servicePreview(
            materialSession, assetDatabase, assetDatabase.root(), frameClock.deltaSeconds(),
            viewportPanel != nullptr ? viewportPanel->tonemapParams() : render::TonemapParams{}, lighting);
    }
    // task 3.1.5 (D8/D12): the FIFTH occupant of this slot -- renderScene, serviceThumbnails, the
    // import session, the material preview, and now the scene-asset ledger. OUTSIDE the ImGui draw walk
    // and BEFORE endFrame. EVERY GPU create and EVERY GPU destroy this task performs happens here.
    //
    // NEVER CALL THIS FROM onDraw(). No texture created here is ever handed to ImGui, so this has no
    // draw-walk exception of the kind the material preview's colour target needs -- but the destroy
    // ordering inside it is what makes that true, and it is not optional: out.destroy holds handles the
    // binding table stopped naming ONE SERVICE PASS AGO, so nothing this frame or the last can have
    // recorded them. Destroy FIRST, retire SECOND, execute THIRD.
    //
    // It runs AFTER the preview rather than before it, for the preview's own reason inverted: an
    // unbounded model import between the draw walk and the preview's frame would buy nothing, and the
    // ledger touches no ImGui-sampled texture, so appending after it is safe.
    serviceSceneAssets();
    presented = layer.endFrame(config.clearColor);
    // task E.4.1 (D4): the END of the tick, never the top. applyFileRequests runs INSIDE drawShellUi
    // (shell_ui.cpp:552) earlier in THIS tick, so a scene opened, saved or cleared by this frame's menu
    // action is already in session.path() by the time the draw walk returns. And the unsaved-changes
    // modal's "Save" answer performs the save AND the pending Quit in ONE tick -- fileFlow.quitConfirmed
    // is read on the next line and tick() returns false for ever after -- so a top-of-tick reconcile
    // would lose exactly that frame, which is the most common quit path there is.
    // It touches no ImGui and no GPU, so it has no draw-walk constraint of its own: the placement is
    // about the QUIT, not about ImGui.
    //
    // task E.4.2: MONOTONIC per process, and the SECOND occupant of E.4.1's end-of-tick slot, ahead of
    // persistProjectState(). Extend the slot; never twin it. It is HERE rather than in tick()'s
    // top-of-tick reconcile block for E.4.1's own D4 reason above -- the refusal is raised inside
    // applyFileRequests, which runs within drawShellUi EARLIER IN THIS TICK, so a top-of-tick mirror
    // would lag every refusal by a frame.
    //
    // AN ABSOLUTE MIRROR, NEVER A DELTA (the code-review round). The flow's serial is monotonic for
    // the FileFlow's lifetime -- scene_session.cpp's clearContainmentOffer preserves it across every
    // drain -- so there is nothing to accumulate and no `>` guard to get wrong. The delta form this
    // replaced was defeated by the drain resetting the serial: a dismiss plus a fresh refusal inside
    // ONE applyFileRequests call ran 1 -> 0 -> 1, `1 > 1` is false, and the counter did not move for a
    // refusal the user could see on screen. I191 drives exactly that tick.
    containmentRefusals = fileFlow.containmentOffer.refusalSerial;
    persistProjectState();
    if (fileFlow.quitConfirmed) {
        // File > Exit / Ctrl+Q / the window [X] -- all AFTER the guard said yes (task 2.5.1 D1). This
        // frame still completed, so Render stays balanced (AC-28).
        running = false;
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
CommandStack& EditorApp::commands() noexcept { return commandStack; }
const CommandStack& EditorApp::commands() const noexcept { return commandStack; }
RootOrder& EditorApp::roots() noexcept { return rootOrder; }
const RootOrder& EditorApp::roots() const noexcept { return rootOrder; }
const FrameClock& EditorApp::clock() const noexcept { return frameClock; }
bool EditorApp::focused() const noexcept { return windowFocused; }
bool EditorApp::presentedLastFrame() const noexcept { return presented; }
std::size_t EditorApp::logRecordCount() const noexcept {
    return consolePanel != nullptr ? consolePanel->history().size() : std::size_t{0};
}
EditorCamera* EditorApp::viewportCamera() noexcept {
    return viewportPanel != nullptr ? &viewportPanel->camera() : nullptr;
}
const EditorCamera* EditorApp::viewportCamera() const noexcept {
    return viewportPanel != nullptr ? &viewportPanel->camera() : nullptr;
}
std::string_view EditorApp::scenePath() const noexcept { return session.path(); }
bool EditorApp::sceneDirty() const noexcept { return !commandStack.isClean(); }

bool EditorApp::projectIsOpen() const noexcept { return project.isOpen(); }
std::string_view EditorApp::projectRoot() const noexcept { return project.root(); }
std::string_view EditorApp::projectName() const noexcept { return project.name(); }
std::string_view EditorApp::assetBrowserRoot() const noexcept {
    return assetBrowserPanel != nullptr ? std::string_view(assetBrowserPanel->root()) : std::string_view{};
}
std::size_t EditorApp::recentProjectCount() const noexcept { return recents.paths.size(); }

std::size_t EditorApp::assetCount() const noexcept { return assetDatabase.size(); }
std::optional<Guid> EditorApp::assetGuidForPath(std::string_view relativePath) const noexcept {
    return assetDatabase.guidForPath(relativePath);
}

// task 3.1.2 (§D-12): findByPath's record already carries both fields (asset_meta.hpp); std::optional
// on the return, not the record's own value, distinguishes "no record for this path" from a real,
// legitimately zero/UpToDate value (plan A4).
std::optional<ImportChange> EditorApp::assetImportChangeForPath(std::string_view relativePath) const noexcept {
    const AssetRecord* const record = assetDatabase.findByPath(relativePath);
    // Code-review finding 3: `change` is never assigned for an Invalid or write-failed record (phase 8
    // excludes both from its inputs) -- it stays at its ImportChange::UpToDate default either way, so
    // reading it unguarded for either would misreport a file with no sidecar and no cache entry as "up
    // to date". Mirrors the Asset Browser footer's own guard, extended to cover the write-failed case
    // the footer did not.
    if (record == nullptr || record->state == AssetMetaState::Invalid || record->metaWriteFailed) {
        return std::nullopt;
    }
    return record->change;
}
std::optional<ContentHash> EditorApp::assetContentHashForPath(std::string_view relativePath) const noexcept {
    const AssetRecord* const record = assetDatabase.findByPath(relativePath);
    return record != nullptr ? std::optional<ContentHash>(record->contentHash) : std::nullopt;
}
std::size_t EditorApp::assetCacheEntryCount() const noexcept { return assetDatabase.cacheSize(); }
std::size_t EditorApp::assetImportJobCount() const noexcept { return assetDatabase.importPlan().jobIndices.size(); }

// task 3.1.3 (A12): forwarded, 0 when no Asset Browser panel is registered -- the
// assetCacheEntryCount() shape verbatim.
// code-review finding 4: the search half's observability, same null-checked shape.
std::size_t EditorApp::assetBrowserSearchHitCount() const noexcept {
    return assetBrowserPanel != nullptr ? assetBrowserPanel->searchHitCount() : std::size_t{0};
}
bool EditorApp::assetBrowserListViewActive() const noexcept {
    return assetBrowserPanel != nullptr && assetBrowserPanel->listViewActive();
}
bool EditorApp::assetBrowserDeleteModalPending() const noexcept {
    return assetBrowserPanel != nullptr && assetBrowserPanel->deleteModalPending();
}
// task E.4.4: the same null-checked forward, twice more.
std::size_t EditorApp::assetBrowserVisibleEntryCount() const noexcept {
    return assetBrowserPanel != nullptr ? assetBrowserPanel->cachedEntryCount() : std::size_t{0};
}
bool EditorApp::assetBrowserListingContains(std::string_view leafName) const noexcept {
    return assetBrowserPanel != nullptr && assetBrowserPanel->cachedListingContains(leafName);
}

// task E.3.3: re-pointed at the SHARED service. A moved-from app holds a null pointer, exactly as it
// holds a null sceneAssetLoader, so all four are null-guarded like the drains.
std::size_t EditorApp::thumbnailReadyCount() const noexcept {
    return thumbnails != nullptr ? thumbnails->readyCount() : std::size_t{0};
}
std::size_t EditorApp::thumbnailUnavailableCount() const noexcept {
    return thumbnails != nullptr ? thumbnails->unavailableCount() : std::size_t{0};
}
std::size_t EditorApp::thumbnailResidentCount() const noexcept {
    return thumbnails != nullptr ? thumbnails->residentCount() : std::size_t{0};
}
std::size_t EditorApp::thumbnailLoadAttempts() const noexcept {
    return thumbnails != nullptr ? thumbnails->loadAttempts() : std::size_t{0};
}
std::size_t EditorApp::assetOrphanCount() const noexcept { return lastAssetReport.orphanTotal; }
// code-review SHOULD-FIX 10: the assetOrphanCount() shape verbatim, applied to phase 7.5's own capped
// category -- without this, a GPU-tier case has no black-box signature for report.importFailureTotal at
// all, since AssetScanReport is retained inside EditorApp (private), reachable through
// AssetBrowserPanel::setScanReport() only (src-private).
std::size_t EditorApp::assetImportFailureCount() const noexcept { return lastAssetReport.importFailureTotal; }

// task 3.1.4 (AC-38): the assetCount()/thumbnailReadyCount() shape verbatim. AssetWatcher is a
// private member, so without these the whole detect -> rescan -> refresh loop has no black-box
// signature at all and every GPU-tier case would be unwritable.
bool EditorApp::assetWatchEnabled() const noexcept { return assetWatcher.enabled(); }
std::uint64_t EditorApp::assetWatchSweepCount() const noexcept { return assetWatcher.status().sweepsCompleted; }
std::uint64_t EditorApp::assetWatchTriggerCount() const noexcept { return assetWatcher.status().triggers; }
std::size_t EditorApp::assetWatchEntryCount() const noexcept { return assetWatcher.status().entriesSeen; }

// task 3.2.1 (AC-45/AC-46/AC-47): the assetWatchEnabled()/assetWatchSweepCount() shape verbatim --
// ModelImportSession is reachable from no test target otherwise, so without these the whole
// select -> reconcile -> service loop has no black-box signature at all.
std::size_t EditorApp::modelImportCount() const noexcept { return importSession.importCount(); }
int EditorApp::modelImportState() const noexcept { return static_cast<int>(importSession.state()); }
std::string_view EditorApp::modelImportTarget() const noexcept { return importSession.target(); }

// task 3.2.4: the same black-box shape a second time -- an int, so BlenderState itself never reaches
// editor_app.hpp's surface.
int EditorApp::blenderState() const noexcept { return static_cast<int>(importSession.blender().state()); }
std::size_t EditorApp::blenderExportRunCount() const noexcept { return importSession.blender().exportRunCount(); }
std::size_t EditorApp::blenderProbeRunCount() const noexcept { return importSession.blender().probeRunCount(); }
std::string_view EditorApp::blenderBinaryPath() const noexcept { return importSession.blender().binaryPath(); }
bool EditorApp::blenderLogRefusedByCap() const noexcept { return importSession.blender().logRefusedByCap(); }

// task 3.4.2: the request seams and the black-box reads. Each seam records EXACTLY what the panel's
// own control records and is drained by the next tick()'s reconcile block -- never applied here.
// task 3.4.2 (D9/AC-5): the requestAssetBrowserReimportAll() forward verbatim -- it queues the SAME
// ActionKind::CreateMaterial the New Material button queues, so the request picks up the panel's own
// committed currentDir in applyPending() and the tick() drain cannot tell a seam from a click.
void EditorApp::requestAssetBrowserCreateMaterial() noexcept {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestCreateMaterial();
    }
}

// ---- task E.4.3: the seven forwarders ------------------------------------------------------------
// Each takes requestAssetBrowserCreateMaterial's exact shape: null-check the panel, forward, return
// void. Rename and Delete each need TWO, one that OPENS the modal and one that COMMITS it: a single
// commit-forwarder would set the one-shot and prove nothing about whether the modal ever drew, and a
// forwarder for the commit alone would make the modal unobservable. Both are needed on both sides,
// so the two counts are equal by construction.
void EditorApp::requestAssetBrowserContextMenu(std::string path) {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestContextMenu(std::move(path));
    }
}
void EditorApp::requestAssetBrowserNewFolder() noexcept {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestNewFolder();
    }
}
void EditorApp::requestAssetBrowserRename(std::string path) {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestRename(std::move(path));
    }
}
void EditorApp::requestAssetBrowserRenameCommit(std::string newLeaf) {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestRenameCommit(std::move(newLeaf));
    }
}
void EditorApp::requestAssetBrowserDelete(std::string path) {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestDelete(std::move(path));
    }
}
void EditorApp::requestAssetBrowserDeleteConfirm() noexcept {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestDeleteConfirm();
    }
}
void EditorApp::requestAssetBrowserMove(std::string path, std::string destinationDir) {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestMove(std::move(path), std::move(destinationDir));
    }
}
// The EIGHTH seam (see the panel's own comment): it models a PAYLOAD STATE, not a gesture, and has
// no applyPending arm -- so §1.4.3's count of seven deliberately does not include it.
void EditorApp::requestAssetBrowserDropPeek(std::string sourcePath, std::string destinationDir, bool asMovePayload) {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestDropPeek(std::move(sourcePath), std::move(destinationDir), asMovePayload);
    }
}

// ---- task E.4.3: the black-box observables the GPU tier asserts through -------------------------
bool EditorApp::assetBrowserRenameModalPending() const noexcept {
    return assetBrowserPanel != nullptr && assetBrowserPanel->renameModalPending();
}
bool EditorApp::assetBrowserAssetDeleteModalPending() const noexcept {
    return assetBrowserPanel != nullptr && assetBrowserPanel->assetDeleteModalPending();
}
std::size_t EditorApp::assetBrowserContextMenuItemsDrawn() const noexcept {
    return assetBrowserPanel != nullptr ? assetBrowserPanel->contextMenuItemsDrawn() : 0;
}
std::size_t EditorApp::assetBrowserRenameModalDrawnCount() const noexcept {
    return assetBrowserPanel != nullptr ? assetBrowserPanel->renameModalDrawnCount() : 0;
}
std::size_t EditorApp::assetBrowserDeleteModalDrawnCount() const noexcept {
    return assetBrowserPanel != nullptr ? assetBrowserPanel->assetDeleteModalDrawnCount() : 0;
}
std::size_t EditorApp::assetBrowserDropTargetsAccepted() const noexcept {
    return assetBrowserPanel != nullptr ? assetBrowserPanel->dropTargetsAccepted() : 0;
}
int EditorApp::assetBrowserLastDropPeekRefusal() const noexcept {
    // An INT, so AssetOpRefusal itself never reaches editor_app.hpp's surface -- modelImportState()'s
    // own posture, applied a second time.
    return assetBrowserPanel != nullptr ? static_cast<int>(assetBrowserPanel->lastDropPeekRefusal()) : 0;
}
std::string_view EditorApp::assetBrowserContextMenuTarget() const noexcept {
    return assetBrowserPanel != nullptr ? std::string_view(assetBrowserPanel->contextMenuTarget()) : std::string_view{};
}

void EditorApp::requestMaterialDocument(MaterialDocument document) { requestedMaterialDocument = std::move(document); }
void EditorApp::requestMaterialApply() noexcept { requestedMaterialApply = true; }
void EditorApp::requestMaterialRevert() noexcept { requestedMaterialRevert = true; }
std::string_view EditorApp::materialTargetPath() const noexcept { return materialSession.targetPath(); }
bool EditorApp::materialParseOk() const noexcept { return materialSession.document() != nullptr; }
bool EditorApp::materialDirty() const noexcept { return materialSession.dirty(); }
const MaterialDocument* EditorApp::materialDocument() const noexcept { return materialSession.document(); }
bool EditorApp::materialPreviewAvailable() const noexcept {
    return materialPanel != nullptr && materialPanel->previewAvailable();
}
std::size_t EditorApp::materialPreviewFrameCount() const noexcept {
    return materialPanel != nullptr ? materialPanel->previewFrameCount() : 0;
}
bool EditorApp::materialPreviewBlendDrawnOpaque() const noexcept {
    return materialPanel != nullptr && materialPanel->previewBlendDrawnOpaque();
}
std::size_t EditorApp::materialPreviewTextureCount() const noexcept {
    return materialPanel != nullptr ? materialPanel->previewTextureCount() : 0;
}
std::size_t EditorApp::materialPreviewTextureLoadAttempts() const noexcept {
    return materialPanel != nullptr ? materialPanel->previewTextureLoadAttempts() : 0;
}
std::size_t EditorApp::materialPreviewImageCount() const noexcept {
    return materialPanel != nullptr ? materialPanel->previewImageCount() : 0;
}
std::size_t EditorApp::materialPreviewStaleImageCount() const noexcept {
    return materialPanel != nullptr ? materialPanel->previewStaleImageCount() : 0;
}
std::size_t EditorApp::materialPreviewUvSetWarnCount() const noexcept {
    return materialPanel != nullptr ? materialPanel->previewUvSetWarnCount() : 0;
}
std::uint32_t EditorApp::materialPreviewTextureWidth() const noexcept {
    return materialPanel != nullptr ? materialPanel->previewTextureWidth() : 0;
}
std::uint32_t EditorApp::materialPreviewTextureHeight() const noexcept {
    return materialPanel != nullptr ? materialPanel->previewTextureHeight() : 0;
}
std::size_t EditorApp::materialPreviewSkyDrawCount() const noexcept {
    return materialPanel != nullptr ? materialPanel->previewSkyDrawCount() : 0;
}
bool EditorApp::materialPreviewHasSun() const noexcept {
    return materialPanel != nullptr && materialPanel->previewHasSun();
}

// ---- task E.3.2: the eight context-routing seams -------------------------------------------------
void EditorApp::setFocusRoutingEnabled(bool on) noexcept {
    // The SAME two effects the View-menu checkbox has (drawMenuBar + tick()'s read-back), so a test
    // and a click are indistinguishable downstream -- the requestAssetBrowserSelectEntry posture.
    // MARKS THE FILE DIRTY UNCONDITIONALLY, even when the value did not change: that is one write per
    // call from a seam only tests and the menu use, and an equality gate would make the "toggle, then
    // construct a second app" arm depend on the PREVIOUS value rather than on the call.
    contextRouter.setEnabled(on);
    editorPrefsDirty = true;
}

bool EditorApp::focusRoutingEnabled() const noexcept { return contextRouter.enabled(); }

int EditorApp::pendingFocusRoute() const noexcept { return static_cast<int>(contextRouter.pending()); }

std::string_view EditorApp::lastRoutedPanelId() const noexcept { return lastRoutedPanel; }

std::size_t EditorApp::focusRouteApplyCount() const noexcept { return focusRouteApplies; }
std::size_t EditorApp::focusRouteHoldCount() const noexcept { return focusRouteHolds; }
std::size_t EditorApp::focusRouteDropCount() const noexcept { return focusRouteDrops; }

std::uint64_t EditorApp::panelDrawnCount(const char* id) const noexcept { return registry.drawnCount(id); }

std::size_t EditorApp::projectStateWriteCount() const noexcept { return projectStateWrites; }

// ---- task E.4.2: the containment offer's three black-box accessors -------------------------------
bool EditorApp::sceneContainmentOfferOpen() const noexcept { return fileFlow.containmentOffer.open; }
std::string_view EditorApp::sceneContainmentOfferProject() const noexcept {
    return fileFlow.containmentOffer.projectRoot;
}
std::size_t EditorApp::sceneContainmentRefusalCount() const noexcept { return containmentRefusals; }

// ---- task 3.1.5: the three request hooks (the EIGHTH application of the request shape) ------------
// Each records EXACTLY what the corresponding panel's accept records. The first two land in an
// EditorApp one-shot the next tick's reconcile drains; the third forwards STRAIGHT to the panel,
// because the Material drop rides that panel's own per-frame document copy and nothing held here can
// reach it (§0.4's stated asymmetry).
void EditorApp::requestHierarchyAssetDrop(Guid guid, std::uint8_t kind, Entity targetRow) {
    requestedHierarchyDrop =
        HierarchyAssetDrop{.payload = AssetDragPayload{.guid = guid, .kind = kind}, .targetRow = targetRow};
}
void EditorApp::requestViewportAssetDrop(Guid guid, std::uint8_t kind, Vec2 ndc) {
    requestedViewportDrop = ViewportAssetDrop{.payload = AssetDragPayload{.guid = guid, .kind = kind}, .ndc = ndc};
}
void EditorApp::requestMaterialSlotTextureDrop(std::size_t slot, Guid textureGuid) {
    if (materialPanel != nullptr) {
        materialPanel->requestSlotTextureDrop(slot, textureGuid);
    }
}

// ---- task 3.1.5: the ledger, as numbers -----------------------------------------------------------
std::size_t EditorApp::sceneAssetEntryCount() const noexcept { return sceneAssetLedger.entryCount(); }
std::size_t EditorApp::sceneAssetReadyCount() const noexcept { return sceneAssetLedger.readyCount(); }
std::size_t EditorApp::sceneAssetFailedCount() const noexcept { return sceneAssetLedger.failedCount(); }
std::size_t EditorApp::sceneAssetLoadAttempts() const noexcept { return sceneAssetLedger.loadAttempts(); }
std::size_t EditorApp::sceneAssetDirectiveCount() const noexcept { return sceneAssetDirectives; }
std::size_t EditorApp::sceneAssetDestroyCount() const noexcept { return sceneAssetDestroys; }
std::size_t EditorApp::sceneAssetMeshBindingCount() const noexcept {
    const scene_render::AssetBindingTable* const table =
        viewportPanel != nullptr ? viewportPanel->sceneAssetBindings() : nullptr;
    return table != nullptr ? table->meshCount() : 0;
}
std::size_t EditorApp::sceneAssetMaterialBindingCount() const noexcept {
    const scene_render::AssetBindingTable* const table =
        viewportPanel != nullptr ? viewportPanel->sceneAssetBindings() : nullptr;
    return table != nullptr ? table->materialCount() : 0;
}
std::string_view EditorApp::sceneAssetMessage(Guid guid) const noexcept { return sceneAssetLedger.messageOf(guid); }
std::uint32_t EditorApp::viewportUnresolvedMeshes() const noexcept {
    return viewportPanel != nullptr ? viewportPanel->lastUnresolvedMeshes() : 0U;
}
std::uint32_t EditorApp::viewportUnresolvedMaterials() const noexcept {
    return viewportPanel != nullptr ? viewportPanel->lastUnresolvedMaterials() : 0U;
}

// ---- task 3.1.5: the three drop drains ------------------------------------------------------------
// Every one of them RE-RESOLVES the guid against the live database (INV-D8) and RE-DERIVES the action
// from the live World. The payload's `kind` byte was a peek hint and is never authoritative: a record
// can vanish, or gain a MeshRenderer, between the accept and this drain.
void EditorApp::applyHierarchyDrop(const HierarchyAssetDrop& drop) {
    const AssetRecord* const record = assetDatabase.findByGuid(drop.payload.guid);
    if (record == nullptr) {
        AERO_LOG_WARN("assets: the dropped asset is no longer in this project");
        return;
    }
    const AssetKind kind = classifyAssetKind(leafOf(record->relativePath), /*isDirectory=*/false);
    const bool hasMesh = drop.targetRow.valid() && sceneWorld.has<MeshRenderer>(drop.targetRow);
    const DropSurface surface = drop.targetRow.valid() ? DropSurface::HierarchyRow : DropSurface::HierarchyVoid;
    switch (classifyAssetDrop(kind, surface, hasMesh, /*fieldKind=*/std::nullopt)) {
        case DropAction::InstantiateModel:
            instantiateModelDrop(*record, drop.targetRow, Transform{});  // LOCAL identity under the row
            break;
        case DropAction::AssignMaterial:
            pushMaterialAssign(drop.targetRow, record->guid);
            break;
        case DropAction::BindTextureSlot:
        // task E.3.3: AssignAssetReference is the ASSET FIELD's row and cannot reach this drain -- the
        // surface here is only ever HierarchyRow or HierarchyVoid. Enumerated rather than defaulted,
        // because that is what makes the NEXT enumerator a -Wswitch error instead of a silent refusal.
        case DropAction::AssignAssetReference:
        case DropAction::None:
            break;  // a refusal that reached the drain is a STALE refusal, not an illegal one: peek
                    // already blocked the illegal ones, so this logs nothing beyond the null above
    }
}

void EditorApp::applyViewportDrop(const ViewportAssetDrop& drop) {
    const AssetRecord* const record = assetDatabase.findByGuid(drop.payload.guid);
    if (record == nullptr) {
        AERO_LOG_WARN("assets: the dropped asset is no longer in this project");
        return;
    }
    if (viewportPanel == nullptr) {
        return;  // no viewport, no camera, no placement -- and no way this drop was ever offered
    }
    const AssetKind kind = classifyAssetKind(leafOf(record->relativePath), /*isDirectory=*/false);
    const Entity target = viewportPanel->pickAt(sceneWorld, drop.ndc);
    const bool hasMesh = target.valid() && sceneWorld.has<MeshRenderer>(target);
    switch (classifyAssetDrop(kind, DropSurface::Viewport, hasMesh, /*fieldKind=*/std::nullopt)) {
        case DropAction::InstantiateModel: {
            // Placement is resolved HERE, not at accept time, so the entity lands where the camera is
            // NOW rather than where it was a frame ago. dropPlacementPoint is total: every ray yields
            // a finite point, including the parallel, behind-the-eye and unbuildable cases.
            const Ray ray = viewportRay(viewportPanel->camera(), drop.ndc, viewportPanel->aspect());
            instantiateModelDrop(*record, Entity{}, Transform{.position = dropPlacementPoint(ray)});
            break;
        }
        case DropAction::AssignMaterial:
            pushMaterialAssign(target, record->guid);
            break;
        case DropAction::BindTextureSlot:
        case DropAction::AssignAssetReference:  // task E.3.3: the asset FIELD's row -- never this drain's
        case DropAction::None:
            break;
    }
}

void EditorApp::applySlotDrop(const MaterialSlotTextureDrop& drop) {
    // The panel has ALREADY folded this into its own document copy -- that is §0.4's asymmetry. This
    // drain exists for exactly one thing: saying so when the guid vanished between the accept and now.
    if (assetDatabase.findByGuid(drop.textureGuid) == nullptr) {
        AERO_LOG_WARN("assets: the dropped asset is no longer in this project");
    }
}

void EditorApp::pushMaterialAssign(Entity entity, Guid material) {
    // findComponentType, never componentTypeId<T>(): an id looked up BY NAME is invalid rather than
    // merely unregistered in a -DAERO_REFLECT_TOOLS=OFF build, which is the one configuration where
    // this whole path must degrade quietly instead of logging an ERROR from the seam.
    const ComponentTypeId meshRendererId = sceneWorld.findComponentType(MESH_RENDERER_TYPE_NAME);
    if (!entity.valid() || !meshRendererId.valid()) {
        AERO_LOG_WARN("assets: this entity can no longer take a material");
        return;
    }
    // ...and findComponentType is NOT sufficient on its own, which the comment above used to assume.
    // MeshRenderer is a hand-registered built-in, so the World resolves it by NAME even in a
    // -DAERO_REFLECT_TOOLS=OFF build where no generated entt::meta exists at all; without this second
    // guard the drain reaches readComponentField and logs an ERROR from the seam in a configuration
    // where the correct behaviour is simply to do nothing. Measured: the reflect-off lane reddened
    // DP6 with exactly that ERROR before this line existed.
    if (!componentFieldsAreReflected(sceneWorld, meshRendererId)) {
        AERO_LOG_WARN("assets: this entity can no longer take a material");
        return;
    }
    const std::optional<FieldValue> before = readComponentField(sceneWorld, entity, meshRendererId, "material");
    if (!before.has_value()) {
        // The entity lost its MeshRenderer between the peek and this drain, or the component type is
        // not registered (a -DAERO_REFLECT_TOOLS=OFF build). Nothing is pushed and nothing is written.
        AERO_LOG_WARN("assets: this entity can no longer take a material");
        return;
    }
    CommandContext cmd{sceneWorld, sceneSelection, rootOrder};
    // A drop is a DISCRETE one-shot gesture, never a continuous one, so the chain is broken on BOTH
    // sides of the push -- the inspector's own Guid row already does exactly this (inspector_panel.cpp's
    // gateForLastItem arms). Without it, SetFieldCommand::mergeWith accepts the next drop on the same
    // entity and field, overwrites `afterValue` and KEEPS `beforeValue`, so dropping material A then
    // material B on one entity collapses to a single history entry reading nil -> B: one undo jumps
    // past A entirely and A is unreachable. Found by the code-review round; DP5-DP7 dropped only once
    // per entity, so nothing exercised it.
    commandStack.breakMergeChain();  // BEFORE the push
    (void)commandStack.push(
        cmd, std::make_unique<SetFieldCommand>(entity, meshRendererId, "material", std::string(MESH_RENDERER_TYPE_NAME),
                                               *before, FieldValue{material}));
    commandStack.breakMergeChain();  // AFTER the push
}

void EditorApp::instantiateModelDrop(const AssetRecord& record, Entity parent, const Transform& placement) {
    const std::string_view leaf = leafOf(record.relativePath);
    ImportedModel fullModel;
    bool haveFull = false;
    InstantiatePlan plan;

    if (isBlendFileName(leaf)) {
        // The cache-HIT arm, shared with the loader rather than restated: two copies of a
        // cache-validity rule is how a cache silently stops invalidating. A miss names Import Details.
        const BlendArtifactResult artifact = readBlendCacheArtifact(record, assetDatabase.projectRoot());
        if (!artifact.ok) {
            AERO_LOG_WARN("assets: {}", artifact.message);
            return;
        }
        const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(artifact.bytes.data()),  // NOLINT
                                               artifact.bytes.size());
        ImportResult imported = importModel(artifact.artifactLeaf, /*assetRelativeDir=*/"", bytes,
                                            record.importSettings, ImportDepth::Full, {});
        if (imported.status != ImportStatus::Ok && imported.status != ImportStatus::Truncated) {
            AERO_LOG_WARN("assets: this model could not be imported -- {}", imported.message);
            return;
        }
        assignImageGuids(imported.model.images, assetDatabase);
        fullModel = std::move(imported.model);
        haveFull = true;
        plan = buildInstantiatePlan(fullModel, assetStem(record.relativePath), record.guid);
    } else {
        const std::string modelPath = std::string(assetDatabase.root()) + '/' + record.relativePath;
        const FileBytesResult modelBytes = readFileBytes(modelPath, MAX_MODEL_FILE_BYTES);
        if (!modelBytes.bytes.has_value()) {
            AERO_LOG_WARN(
                "assets: this model could not be read -- {}",
                modelBytes.refusedByCap ? "it is larger than this importer's 256 MiB limit" : modelBytes.error);
            return;
        }
        const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(modelBytes.bytes->data()),  // NOLINT
                                               modelBytes.bytes->size());
        const std::string dir = parentOf(record.relativePath);
        // PASS 1 -- Structure. Cheap, and enough to plan a node tree for every importer that reports
        // one at this depth.
        ImportResult structure = importModel(leaf, dir, bytes, record.importSettings, ImportDepth::Structure, {});
        if (structure.status != ImportStatus::Ok && structure.status != ImportStatus::Truncated) {
            AERO_LOG_WARN("assets: this model could not be imported -- {}", structure.message);
            return;
        }
        plan = buildInstantiatePlan(structure.model, assetStem(record.relativePath), record.guid);
        // THE TWO-ARM FALLBACK (D5 as amended by §0.17). ONLY NoNodes retries: .obj, .stl and .ply
        // report no hierarchy at Structure depth by design, so their node tree exists only at Full. A
        // Cycle or a TooDeep will not be fixed by re-reading up to 256 MiB at a different depth.
        if (!plan.ok && plan.refusal == InstantiatePlanRefusal::NoNodes) {
            std::vector<ExternalBuffer> externals;
            if (modelImporterNeedsExternalBuffers(leaf)) {
                std::uint64_t total = 0;
                for (const std::string& rel : structure.externalUris) {
                    FileBytesResult buffer =
                        readFileBytes(std::string(assetDatabase.root()) + '/' + rel, MAX_EXTERNAL_BYTES_PER_MODEL);
                    if (!buffer.bytes.has_value()) {
                        continue;  // an unreadable buffer becomes MissingBuffer in pass 2
                    }
                    if (buffer.bytes->size() > MAX_EXTERNAL_BYTES_PER_MODEL - total) {
                        break;  // over budget: plan from what we have rather than from a partial Full
                    }
                    total += buffer.bytes->size();
                    externals.push_back(ExternalBuffer{rel, std::move(*buffer.bytes)});
                }
            }
            ImportResult full = importModel(leaf, dir, bytes, record.importSettings, ImportDepth::Full, externals);
            if (full.status == ImportStatus::Ok || full.status == ImportStatus::Truncated) {
                assignImageGuids(full.model.images, assetDatabase);
                fullModel = std::move(full.model);
                haveFull = true;
                plan = buildInstantiatePlan(fullModel, assetStem(record.relativePath), record.guid);
            }
        }
    }

    if (!plan.ok) {
        AERO_LOG_WARN("assets: this model cannot be added to the scene -- {}", plan.error);
        return;  // NOTHING is created
    }
    for (const std::string& warning : plan.warnings) {
        AERO_LOG_WARN("assets: {}", warning);
    }
    CommandContext cmd{sceneWorld, sceneSelection, rootOrder};
    (void)commandStack.push(
        cmd, std::make_unique<InstantiateAssetCommand>(std::move(plan), parent, placement, sceneSelection.entities()));
    // The drop already paid for a Full import, so hand it straight to the cook -> upload half rather
    // than letting the ledger re-import the same bytes next service pass. If the viewport renderer is
    // unavailable, skip silently: the ledger issues an ordinary directive later.
    if (haveFull) {
        loadModelIntoScene(record, &fullModel);
    }
}

// ---- task 3.1.5: executing ONE directive ----------------------------------------------------------
// Both of these run ONLY from tick()'s post-draw slot -- from serviceSceneAssets, or from the drop
// drain in the reconcile block, which is also outside every draw walk. Every GPU create this task
// performs is inside one of them.
void EditorApp::loadModelIntoScene(const AssetRecord& record, const ImportedModel* preImported) {
    render::ForwardRenderer* const renderer =
        viewportPanel != nullptr ? viewportPanel->sceneForwardRenderer() : nullptr;
    scene_render::AssetBindingTable* const bindings =
        viewportPanel != nullptr ? viewportPanel->sceneAssetBindings() : nullptr;
    if (renderer == nullptr || bindings == nullptr || !sceneAssetLoader) {
        return;  // no renderer yet: the ledger simply issues the directive again on a later pass
    }
    const SceneAssetLoader::ModelLoadResult loaded =
        preImported != nullptr ? sceneAssetLoader->loadFromImportedModel(*preImported, record, *renderer)
                               : sceneAssetLoader->loadModel(record, assetDatabase.root(), assetDatabase.projectRoot(),
                                                             assetDatabase, *renderer);
    for (const std::string& warning : loaded.warnings) {
        AERO_LOG_WARN("assets: {}", warning);
    }
    if (!loaded.ok) {
        AERO_LOG_WARN("assets: '{}' could not be loaded -- {}", record.relativePath, loaded.message);
        sceneAssetLedger.reportFailed(record.guid, loaded.message);
        // reportFailed retires whatever this entry already held onto the deferred destroy list, so the
        // binding must stop naming those handles here -- a guid reaches out.retire only when its entry
        // is UNREFERENCED, and a referenced Failed entry is deliberately kept, so nothing downstream
        // would ever unbind it. Without this the table names a destroyed MeshHandle from the next frame
        // on: the draw latches one stale-handle WARN, the entity renders nothing, and unresolvedMeshes
        // UNDER-reports, because the binding still exists so the counting arm is never taken. Reachable
        // when an already-Ready model's bytes change and the reload then fails. The code-review round.
        bindings->removeMesh(record.guid);
        return;
    }
    // The ledger ADOPTS the handles, folds the bounds into its own lookup and registers one pending
    // texture directive per bound slot. The binding table is the caller's to install: the ledger is
    // pure and has never heard of it.
    sceneAssetLedger.reportLoaded(record.guid, record.contentHash, loaded.handles, loaded.textureRequests,
                                  LedgerAssetClass::Model);
    bindings->setMesh(record.guid, loaded.binding);
}

void EditorApp::loadMaterialIntoScene(const AssetRecord& record) {
    render::ForwardRenderer* const renderer =
        viewportPanel != nullptr ? viewportPanel->sceneForwardRenderer() : nullptr;
    scene_render::AssetBindingTable* const bindings =
        viewportPanel != nullptr ? viewportPanel->sceneAssetBindings() : nullptr;
    if (renderer == nullptr || bindings == nullptr || !sceneAssetLoader) {
        return;
    }
    const SceneAssetLoader::MaterialLoadResult loaded =
        sceneAssetLoader->loadMaterial(record, assetDatabase.root(), *renderer);
    for (const std::string& warning : loaded.warnings) {
        AERO_LOG_WARN("assets: {}", warning);
    }
    if (!loaded.ok) {
        AERO_LOG_WARN("assets: '{}' could not be loaded -- {}", record.relativePath, loaded.message);
        sceneAssetLedger.reportFailed(record.guid, loaded.message);
        bindings->removeMaterial(record.guid);  // the model arm's reasoning, one asset class over
        return;
    }
    LedgerHandles handles;
    handles.materials.push_back(loaded.material);
    handles.materialStates.push_back(loaded.state);
    sceneAssetLedger.reportLoaded(record.guid, record.contentHash, handles, loaded.textureRequests,
                                  LedgerAssetClass::Material);
    bindings->setMaterial(record.guid, loaded.material);
}

void EditorApp::serviceSceneAssets() {
    scene_render::AssetBindingTable* const bindings =
        viewportPanel != nullptr ? viewportPanel->sceneAssetBindings() : nullptr;
    render::ForwardRenderer* const renderer =
        viewportPanel != nullptr ? viewportPanel->sceneForwardRenderer() : nullptr;

    // 1. THE REFERENCED SET, from the World. One deduplicated, sorted vector, classified against the
    //    database by the record's own path: a guid that is neither a material nor an importable model
    //    is not loadable and gets no entry at all, which is what stops a retargeted texture guid from
    //    ever entering the ledger.
    std::vector<Guid> referencedGuids;
    sceneWorld.each<MeshRenderer>([&referencedGuids](Entity /*entity*/, MeshRenderer& renderer) {
        if (renderer.mesh.valid()) {
            referencedGuids.push_back(renderer.mesh);
        }
        if (renderer.material.valid()) {
            referencedGuids.push_back(renderer.material);
        }
    });
    std::sort(referencedGuids.begin(), referencedGuids.end());
    referencedGuids.erase(std::unique(referencedGuids.begin(), referencedGuids.end()), referencedGuids.end());
    std::vector<LedgerAssetFacts> referenced;
    referenced.reserve(referencedGuids.size());
    for (const Guid guid : referencedGuids) {
        const AssetRecord* const record = assetDatabase.findByGuid(guid);
        if (record == nullptr) {
            // Present but RECORDLESS: the entry must still exist so the ledger can retire whatever it
            // is holding for that guid. recordPresent = false is exactly that signal.
            referenced.push_back(LedgerAssetFacts{.guid = guid});
            continue;
        }
        const bool material = isMaterialFileName(record->relativePath);
        if (!material && !isImportableModelName(leafOf(record->relativePath)) &&
            !isBlendFileName(leafOf(record->relativePath))) {
            continue;  // not loadable at all -- skipped WITHOUT an entry
        }
        referenced.push_back(LedgerAssetFacts{.guid = guid,
                                              .isMaterial = material,
                                              .recordPresent = true,
                                              .hashUsable = assetContentHashUsable(*record),
                                              .hash = record->contentHash});
    }

    // 2. THE APPLY NUDGE: an .aeromat written this session is stale in the ledger the instant it lands,
    //    and no generation bump reports that on its own -- the file's bytes changed, the database's
    //    view of them has not been rescanned yet.
    std::vector<Guid> nudged;
    if (materialSession.writeCount() != lastMaterialWriteCount) {
        lastMaterialWriteCount = materialSession.writeCount();
        if (const AssetRecord* const target = assetDatabase.findByPath(materialSession.targetPath());
            target != nullptr && target->guid.valid()) {
            nudged.push_back(target->guid);
        }
    }

    // 3. ONE service call: facts in, a directive plus a retire list plus a destroy list out.
    const LedgerServiceOutput out = sceneAssetLedger.service(
        LedgerServiceInput{.referenced = referenced, .generation = assetDatabase.generation(), .nudged = nudged});

    // 4. DESTROY FIRST. These handles were retired ONE PASS AGO: the table stopped naming them a whole
    //    service pass back, so no recorded frame can still hold them. Destroying THIS pass's
    //    retirements immediately is seed S20, and the deferral is the ledger's, not a local's.
    for (const LedgerDestroy& entry : out.destroy) {
        destroyRetired(renderer, sceneAssetDevice, entry);
        ++sceneAssetDestroys;
    }
    // 5. RETIRE SECOND. The handles these bindings named go onto the ledger's NEXT pass's destroy list.
    if (bindings != nullptr) {
        for (const Guid guid : out.retire) {
            bindings->removeMesh(guid);
            bindings->removeMaterial(guid);
        }
    }
    // 6. EXECUTE THIRD, at most one directive per pass.
    // Bound to a NAMED LOCAL before the has_value() test and the dereference alike:
    // bugprone-unchecked-optional-access is --warnings-as-errors on the Linux Debug lane and does not
    // track a test made through a member access expression -- material_preview.cpp records the same
    // trap one std::array subscript over.
    const std::optional<LedgerDirective>& pending = out.directive;
    if (pending.has_value()) {
        ++sceneAssetDirectives;
        const LedgerDirective directive = *pending;
        switch (directive.assetClass) {
            case LedgerAssetClass::Model: {
                if (const AssetRecord* const record = assetDatabase.findByGuid(directive.guid); record != nullptr) {
                    loadModelIntoScene(*record, nullptr);
                }
                break;
            }
            case LedgerAssetClass::Material: {
                if (const AssetRecord* const record = assetDatabase.findByGuid(directive.guid); record != nullptr) {
                    loadMaterialIntoScene(*record);
                }
                break;
            }
            case LedgerAssetClass::Texture: {
                const AssetRecord* const record = assetDatabase.findByGuid(directive.guid);
                rhi::TextureHandle texture{};
                if (record != nullptr && sceneAssetLoader) {
                    const SceneAssetLoader::TextureLoadResult loaded =
                        sceneAssetLoader->loadSlotTexture(*record, assetDatabase.root(), directive.srgb);
                    if (!loaded.ok) {
                        AERO_LOG_WARN("assets: a material slot texture could not be loaded -- {}", loaded.message);
                    }
                    texture = loaded.texture;
                }
                // Reported EITHER WAY: the pending slot is cleared whether or not the upload succeeded,
                // so a broken image costs one attempt per session rather than one per pass.
                const LedgerSlotBinding binding = sceneAssetLedger.reportSlotTexture(directive, texture);
                if (binding.state != nullptr && renderer != nullptr && sceneAssetLoader) {
                    sceneAssetLoader->rebindSlot(*renderer, binding.material, binding.state->params,
                                                 binding.state->slots, directive.slot, texture);
                }
                break;
            }
        }
    }
    // 7. PUBLISH the bounds lookup. A pointer to the LEDGER's own member: both are EditorApp members,
    //    so the ledger outlives the panel and EditorApp owns the ordering.
    if (viewportPanel != nullptr) {
        viewportPanel->setMeshBounds(&sceneAssetLedger.boundsLookup());
    }
}

void EditorApp::releaseSceneAssets() {
    // ONE call, so a caller cannot half-reset: the ledger moves every live handle onto its destroy
    // list -- including anything already deferred -- and ends empty. Runs while the panels are STILL
    // ALIVE, on a project swap and at shutdown alike, because the renderer that minted a MeshHandle is
    // the only thing that can release it.
    render::ForwardRenderer* const renderer =
        viewportPanel != nullptr ? viewportPanel->sceneForwardRenderer() : nullptr;
    for (const LedgerDestroy& entry : sceneAssetLedger.resetForProjectSwap()) {
        destroyRetired(renderer, sceneAssetDevice, entry);
        ++sceneAssetDestroys;
    }
    if (scene_render::AssetBindingTable* const bindings =
            viewportPanel != nullptr ? viewportPanel->sceneAssetBindings() : nullptr;
        bindings != nullptr) {
        bindings->clear();
    }
    if (viewportPanel != nullptr) {
        viewportPanel->setMeshBounds(&sceneAssetLedger.boundsLookup());
    }
}

void EditorApp::requestQuit() noexcept { running = false; }
void EditorApp::requestLayoutReset() noexcept { applyDefaultLayout = true; }
void EditorApp::requestUndo() noexcept { undoRequested = true; }
void EditorApp::requestRedo() noexcept { redoRequested = true; }

// task 2.5.1: each hook is `fileFlow.requested = FileAction::X;` (plus `.requestedPath` for the two
// path-taking ones), applied on the NEXT tick -- the requestLayoutReset()/requestUndo() shape, not
// requestQuit()'s (D14/AC-29 -- requestQuit() above stays byte-identical; 35 GPU-gated cases depend
// on it, plan A4).
void EditorApp::requestNewScene() noexcept { fileFlow.requested = FileAction::NewScene; }
void EditorApp::requestOpenSceneDialog() noexcept { fileFlow.requested = FileAction::OpenScene; }
void EditorApp::requestOpenScene(std::string_view path) {
    fileFlow.requested = FileAction::OpenScene;
    fileFlow.requestedPath = path;
}
void EditorApp::requestSaveScene() noexcept { fileFlow.requested = FileAction::SaveScene; }
void EditorApp::requestSaveSceneAs(std::string_view path) {
    fileFlow.requested = FileAction::SaveSceneAs;
    fileFlow.requestedPath = path;
}
void EditorApp::requestGuardedQuit() noexcept { fileFlow.requested = FileAction::Quit; }

// task 2.6.1: the SAME requestUndo()/requestLayoutReset() shape -- applied on the NEXT tick, never
// immediately. requestOpenProject(path) is what makes the entire project flow drivable through real
// frames from the ImGui-free aero_editor_imgui_test, exactly as requestOpenScene(path) did for 2.5.1.
void EditorApp::requestNewProject() noexcept { fileFlow.requested = FileAction::NewProject; }
void EditorApp::requestOpenProjectDialog() noexcept { fileFlow.requested = FileAction::OpenProject; }
void EditorApp::requestOpenProject(std::string_view path) {
    fileFlow.requested = FileAction::OpenProject;
    projectFlow.requestedPath = path;
}
void EditorApp::requestClearRecentProjects() noexcept { projectFlow.clearRecentsRequested = true; }

// task E.4.2: the requestUndo()/requestLayoutReset()/requestAssetBrowserViewMode shape -- each records
// EXACTLY what the modal's own button records, and is applied on the NEXT tick's step-0 drain. Never
// immediately: a request that took effect inside the hook would bypass the drain's own re-test, which
// is the ONLY thing standing between a save-shaped offer and adoptProject -> newScene -> World::clear().
void EditorApp::requestSceneContainmentAccept() noexcept { fileFlow.containmentOffer.acceptRequested = true; }
void EditorApp::requestSceneContainmentDismiss() noexcept { fileFlow.containmentOffer.dismissRequested = true; }

// task 3.1.1 (AC-38): the requestUndo()/requestLayoutReset() shape, drained in the SAME reconcile
// expression as AssetBrowserPanel::takeRescanRequest() -- see tick()'s reconcile block above.
void EditorApp::requestAssetRescan() noexcept { assetRescanRequested = true; }

// task 3.1.2 (AC-39): the requestAssetRescan() shape verbatim, drained in the SAME reconcile expression
// as AssetBrowserPanel::takeReimportRequest() (F9) -- see tick()'s reconcile block above.
void EditorApp::requestAssetReimport() noexcept { assetReimportRequested = true; }

// task 3.1.3 (§Q Q1): the requestAssetRescan()/requestAssetReimport() shape, a third instance -- see
// tick()'s reconcile block above for the drain.
void EditorApp::requestOrphanDelete(std::string_view relativeMetaPath) { requestedOrphanDelete = relativeMetaPath; }

// code-review BLOCKING-1: forwards DIRECTLY to the panel's own requestReimportAll() -- there is no
// one-shot flag to drain here (unlike requestAssetRescan()/requestAssetReimport()/requestOrphanDelete()
// above): AssetBrowserPanel::requestReimportAll() itself already just sets `pending`, the identical
// thing a real button click does, and that is exactly what needs to happen before the NEXT tick()'s
// onDraw() runs. assetBrowserPanel is non-owning, owned by `registry` (F17's precedent) and always
// null-checked.
void EditorApp::requestAssetBrowserReimportAll() noexcept {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestReimportAll();
    }
}

// code-review finding 4: the same null-checked forward, four more times. Each is a no-op when no Asset
// Browser panel is registered, exactly as requestAssetBrowserReimportAll() above.
void EditorApp::requestAssetBrowserViewMode(AssetViewMode mode) noexcept {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestViewMode(mode);
    }
}
void EditorApp::requestAssetBrowserSearch(std::string_view query) {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestSearchQuery(std::string(query));
    }
}
void EditorApp::requestAssetBrowserKindFilter(std::string_view kind) {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestKindFilter(std::string(kind));
    }
}
void EditorApp::requestAssetBrowserDeleteOrphanClick(std::string_view relativeMetaPath) {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestDeleteOrphanClick(std::string(relativeMetaPath));
    }
}

// DEVIATION (task 3.2.1, logged in the final report): the code-review finding 4 shape, a fifth
// application -- no existing seam reaches AssetBrowserPanel::selectedEntry, and without one I53-I59
// (AC-45/AC-46/AC-47/AC-50) would be unwritable or vacuous from the ImGui-free-at-source GPU tier.
void EditorApp::requestAssetBrowserSelectEntry(std::string_view relativePath) {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestSelectEntry(std::string(relativePath));
    }
}

// task E.4.4: the requestAssetBrowserCreateMaterial() forward, for the `Show hidden` checkbox.
void EditorApp::requestAssetBrowserToggleHidden() noexcept {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestToggleHidden();
    }
}

// task E.4.4 (validation finding 2): the Issues header's seam; the panel applies it where a click lands.
void EditorApp::requestAssetBrowserIssuesOpen(bool open) noexcept {
    if (assetBrowserPanel != nullptr) {
        assetBrowserPanel->requestIssuesOpen(open);
    }
}
// task E.4.4 (validation finding 2): the read-only forwards the vertical-fit case (I231) asserts through --
// kept beside the seam, below every line of this file other files cite by number.
std::size_t EditorApp::assetBrowserIssueRowsDrawn() const noexcept {
    return assetBrowserPanel != nullptr ? assetBrowserPanel->issueRowsDrawn() : std::size_t{0};
}
float EditorApp::assetBrowserIssuesBodyHeight() const noexcept {
    return assetBrowserPanel != nullptr ? assetBrowserPanel->issuesBodyHeightDrawn() : 0.0F;
}
float EditorApp::assetBrowserIssuesRowHeight() const noexcept {
    return assetBrowserPanel != nullptr ? assetBrowserPanel->issuesRowHeight() : 0.0F;
}
float EditorApp::assetBrowserScrollMaxY() const noexcept {
    return assetBrowserPanel != nullptr ? assetBrowserPanel->scrollMaxY() : 0.0F;
}

// ---- task E.3.3: the picker's six seams and three observables ------------------------------------
// "Inspector" and "Material" are written as LITERALS here, matching InspectorPanel::id() and
// MaterialPanel::id() -- the same restatement context_router.cpp makes for the three routed ids. A
// wrong literal is not a silent failure: the popup simply never opens, which I162 and I164 fail on.
void EditorApp::requestInspectorAssetPicker(std::string_view componentName, std::string_view fieldName) {
    if (assetPicker != nullptr) {
        assetPicker->pendingOpen =
            AssetPickerOpenRequest{.hostId = "Inspector", .fieldKey = inspectorAssetFieldKey(componentName, fieldName)};
    }
}

void EditorApp::requestMaterialSlotPicker(std::size_t slot) {
    if (assetPicker != nullptr) {
        assetPicker->pendingOpen = AssetPickerOpenRequest{.hostId = "Material", .fieldKey = materialSlotFieldKey(slot)};
    }
}

void EditorApp::requestMaterialSlotDetails(std::size_t slot, bool open) noexcept {
    if (materialPanel != nullptr) {
        materialPanel->setSlotDetailsOpen(slot, open);
    }
}

bool EditorApp::materialSlotDetailsOpen(std::size_t slot) const noexcept {
    return materialPanel != nullptr && materialPanel->slotDetailsOpen(slot);
}

std::size_t EditorApp::materialSamplerRowsDrawn() const noexcept {
    return materialPanel != nullptr ? materialPanel->samplerRowsDrawn() : 0;
}

void EditorApp::requestMaterialSectionOpen(std::size_t section, bool open) noexcept {
    if (materialPanel != nullptr) {
        materialPanel->setSectionOpen(section, open);
    }
}

bool EditorApp::materialSectionOpen(std::size_t section) const noexcept {
    return materialPanel != nullptr && materialPanel->sectionIsOpen(section);
}

void EditorApp::requestAssetPickerSearch(std::string_view query) {
    if (assetPicker != nullptr) {
        assetPicker->pendingSearch = std::string(query);
    }
}

void EditorApp::requestAssetPickerMove(int delta) {
    if (assetPicker != nullptr && delta != 0) {
        assetPicker->pendingMove = delta > 0 ? AssetPickerMove::Next : AssetPickerMove::Prev;
        assetPicker->pendingMoveSteps = static_cast<std::size_t>(std::abs(delta));
    }
}

void EditorApp::requestAssetPickerCommit() noexcept {
    if (assetPicker != nullptr) {
        assetPicker->pendingCommit = true;
    }
}

void EditorApp::requestAssetPickerClose() noexcept {
    if (assetPicker != nullptr) {
        assetPicker->pendingClose = true;
    }
}

bool EditorApp::assetPickerOpen() const noexcept { return assetPicker != nullptr && assetPicker->openValue; }

std::size_t EditorApp::assetPickerCandidateCount() const noexcept {
    return assetPicker != nullptr ? assetPicker->candidateCountValue : std::size_t{0};
}

std::size_t EditorApp::assetPickerCursor() const noexcept {
    return assetPicker != nullptr ? assetPicker->cursorValue : std::size_t{0};
}

// task 3.1.4 (D10): applied IMMEDIATELY -- there is no one-shot to drain, because this writes the
// watcher EditorApp itself owns (unlike the panel-facing request hooks above, which must queue a
// `pending` action for the next onDraw()). The requestAssetBrowserReimportAll() posture, one layer up.
void EditorApp::requestAssetWatchToggle(bool on) noexcept { assetWatcher.setEnabled(on); }

// task 3.2.1: the requestAssetBrowserViewMode()/requestAssetWatchToggle() shape -- each queues a
// one-shot for the NEXT tick()'s reconcile to drain (F9), never mutates importSession directly (D17:
// no write may happen from inside onDraw()).
void EditorApp::requestModelImportSettings(ImportSettings s) noexcept { requestedImportSettings = s; }
void EditorApp::requestModelImportApply() noexcept { requestedImportApply = true; }

// task 3.2.4, the requestAssetBrowserSelectEntry shape a sixth time: each records EXACTLY what the
// corresponding panel button records, and nothing more. Applied on the NEXT tick's reconcile.
void EditorApp::requestBlenderConvert() noexcept { requestedBlenderConvert = true; }
void EditorApp::requestBlenderCancel() noexcept { requestedBlenderCancel = true; }
void EditorApp::requestBlenderRedetect() noexcept { requestedBlenderRedetect = true; }
void EditorApp::requestBlenderLocate(std::string_view absolutePathUtf8) { requestedBlenderLocate = absolutePathUtf8; }

// THE ONLY PLACE THIS TASK LOGS (INV-B10, a ninth application). blender_tool.cpp, blender_process.cpp
// and blender_service.cpp are all log-free by construction; every status they hold is RETURNED or
// exposed as an accessor, and the three lines this task adds all originate here.
void EditorApp::resolveBlender() {
    // code-review NOTE 6: with NO project open the database's root is EMPTY, and this path used to be
    // built by concatenation regardless -- yielding "/Library/BlenderExports", an absolute path at the
    // filesystem root that the probe's own directory creation would later try to create. Harmless on a
    // POSIX machine, a real drive-root directory on Windows. Deferring is the honest answer: there is
    // nowhere to derive anything yet, the state stays Unknown, and Unknown is EXACTLY the condition
    // tick()'s lazy resolve re-tests once a project is open. The caller has already written the
    // tool-preferences file when this came from Locate..., so the user's choice is still remembered --
    // it is applied by the resolve that follows the next project open.
    const std::string exportDir = blenderExportDir(assetDatabase.projectRoot());
    if (exportDir.empty()) {
        return;
    }
    bool prefsCorrupt = false;
    const HostOs host = currentHostOs();
    const BlenderEnv env = readBlenderEnv(host, toolPrefsPath, prefsCorrupt);
    if (prefsCorrupt) {
        // A MISSING preferences file is empty preferences SILENTLY (AC-8); only a file that EXISTS and
        // does not parse gets this line, or every machine with no chosen Blender would be warned on
        // every resolve.
        AERO_LOG_WARN("editor: tool preferences '{}' are corrupt or unsupported; ignoring them", toolPrefsPath);
    }
    importSession.blenderMutable().resolve(host, env, exportDir);
}

void EditorApp::applyBlenderOverride(std::string_view absolutePathUtf8) {
    // "" CLEARS the override -- that is exactly what `Re-detect` does, and it is why this is one
    // function rather than two.
    if (const std::string reason = importSession.blenderMutable().setOverridePath(absolutePathUtf8, toolPrefsPath);
        !reason.empty()) {
        AERO_LOG_WARN("editor: could not save the Blender path to '{}' -- {}", toolPrefsPath, reason);
    }
    resolveBlender();  // setOverridePath resets the service to Unknown; re-resolve against the new value
}

// task 3.4.2 (D9/AC-5/AC-6): New Material's whole drain. Called from tick()'s reconcile block and
// nowhere else, so nothing here runs inside a draw walk. Every failure arm returns false having logged
// exactly ONE WARN and written nothing -- and "written nothing" is literal: writeTextFileAtomic writes
// through a `.aero-tmp` sibling and removes it on either failure path, so there is no partial state and
// nothing is ever deleted (the D7/INV-P4 family).
bool EditorApp::createMaterialAsset(std::string_view directoryRel) {
    if (assetBrowserPanel == nullptr) {
        return false;
    }
    // The PANEL's root, not project.assetsRoot(): `directoryRel` is relative to the root the panel was
    // showing when the button was pressed, and the two can differ for exactly one tick after a project
    // swap (the panel's root is reconciled later in this same block). An empty root is the
    // blenderExportDir() lesson one subsystem over -- concatenating onto it would name
    // "/NewMaterial.aeromat", an absolute path at the filesystem root.
    const std::string& assetsRoot = assetBrowserPanel->root();
    if (assetsRoot.empty()) {
        AERO_LOG_WARN("editor: cannot create a material -- no project is open");
        return false;
    }
    // The listing is REQUIRED, not an optimisation: saveMaterialFile overwrites whatever is at the path
    // it is given, so a directory we cannot enumerate is a directory in which we cannot prove a name is
    // unused. Refusing is the only safe answer. Hidden entries are included -- a hidden file still owns
    // its name.
    //
    // listingIsComplete, NOT `status == Ok` -- the code-review round's BLOCKING-2. A listing that hit
    // MAX_ENTRIES_PER_DIRECTORY / MAX_ENTRIES_EXAMINED, or whose iterator failed part way through (the
    // antivirus-lock and cloud-sync case 3.1.4's D5 records as real), keeps ScanStatus::Ok and hands
    // back a PREFIX. Trusting that prefix means uniqueMaterialFileName returns a name that already
    // exists and writeTextFileAtomic renames the canonical default document over an authored material:
    // no warning, no undo, since D4 keeps materials off the CommandStack. The refusal is the same
    // one-WARN-no-file arm either way, because "we could not read the folder" and "we could not read
    // ALL of the folder" have the same consequence here.
    const DirectoryListing listing = listDirectory(assetsRoot, directoryRel, true);
    if (!listingIsComplete(listing)) {
        AERO_LOG_WARN("editor: cannot create a material in '{}' -- the folder could not be read in full",
                      directoryRel.empty() ? std::string_view("<assets root>") : directoryRel);
        return false;
    }
    std::vector<std::string_view> taken;
    taken.reserve(listing.entries.size());
    for (const FileEntry& entry : listing.entries) {
        taken.emplace_back(entry.name);
    }
    const std::string fileName = uniqueMaterialFileName("NewMaterial", taken);
    if (fileName.empty()) {
        AERO_LOG_WARN("editor: cannot create a material in '{}' -- no unused name after {} attempts",
                      directoryRel.empty() ? std::string_view("<assets root>") : directoryRel,
                      MAX_NEW_MATERIAL_ATTEMPTS);
        return false;
    }
    const std::string relativePath = joinRelative(directoryRel, fileName);
    // A NAMED LOCAL, exactly as the amended INV-A1 requires of every path handed to the one write path.
    const std::string materialAbsolutePath = assetsRoot + "/" + relativePath;
    // material_session.cpp's saveMaterialFile -- the ONE function in this tree that writes .aeromat
    // bytes (D12). NOT a second writeTextFileAtomic call site: Apply and Create are two logical writes
    // through ONE physical one, which is what keeps the invariant a grep.
    if (const std::string error = saveMaterialFile(materialAbsolutePath, MaterialDocument{}); !error.empty()) {
        AERO_LOG_WARN("editor: could not create '{}' -- {}", relativePath, error);
        return false;
    }
    AERO_LOG_INFO("editor: created material '{}'", relativePath);
    // The SAME action a real click on the new row records, so the material session's own sticky
    // reconcile retargets to it next tick and the Material panel opens on it (AC-5's last clause).
    assetBrowserPanel->requestSelectEntry(relativePath);
    return true;
}

// ---- task E.4.3: the five orchestrating methods -------------------------------------------------
// Each is createMaterialAsset's shape verbatim. Called from tick()'s reconcile block and nowhere
// else, so nothing here runs inside a draw walk.

bool EditorApp::assetOpBlockedByDirtyMaterial(std::string_view rel) const {
    // Belt-and-braces: "" never reaches here today -- rung 1 refuses it as SourceIsRoot before the
    // planner, and all three callers check this block AFTER the root refusals -- but an empty `rel`
    // would make EVERY target "contained", so a future caller that reorders the checks would
    // otherwise block everything.
    if (rel.empty()) {
        return false;
    }
    if (!materialSession.dirty()) {
        return false;
    }
    const std::string_view target = materialSession.targetPath();  // "" when Untargeted
    if (target.empty()) {
        return false;
    }
    if (target == rel) {
        return true;
    }
    // A FOLDER containing it. SEGMENT-WISE: "tex" must not contain "textures/a.aeromat".
    return target.size() > rel.size() && target.compare(0, rel.size(), rel) == 0 && target[rel.size()] == '/';
}

namespace {

// task E.4.3: the one place the five methods agree about what a missing root means. Returns false and
// leaves both strings untouched when either root is unusable, which the caller reports with one WARN.
[[nodiscard]] bool assetOpRoots(const AssetBrowserPanel* panel, const ProjectSession& project, std::string& assetsRoot,
                                std::string& projectRoot) {
    if (panel == nullptr) {
        return false;
    }
    // The PANEL's root, not project.assetsRoot(): every path these methods receive is relative to
    // the root the panel was showing, and the two can differ for exactly one tick after a project
    // swap (createMaterialAsset's own reasoning, unchanged).
    assetsRoot = panel->root();
    projectRoot = project.root();
    if (assetsRoot.empty() || projectRoot.empty()) {
        return false;
    }
    // THE TWO ROOTS MUST BELONG TO ONE PROJECT (code-review G7). createMaterialAsset's precedent only
    // ever uses the ASSETS root, so it has no cross-root exposure; deleteAssetEntry renames FROM
    // <assetsRoot>/... INTO <projectRoot>/Library/Trash/..., and nothing else asserts the two are
    // related at all. The panel's root is reconciled at the END of this same asset block, so a delete
    // pending across a project swap reads the OUTGOING project's assets root against the INCOMING
    // project's root -- and renames a file out of one project into the other's trash. Low likelihood,
    // irreversible path, one comparison.
    //
    // A LEXICAL containment test, deliberately, and it is the right kind here: both strings are
    // produced by this editor from one manifest (project.hpp's join rule makes assetsRoot exactly
    // `root() + '/' + paths.assets`), never typed by a user and never returned by a file dialog, so
    // the absolute/symlink question E.4.2's directoryWithin exists for does not arise. `paths.assets`
    // may legitimately be "." -- which makes the two roots EQUAL -- so equality is accepted, not just
    // a proper prefix.
    if (assetsRoot != projectRoot &&
        !(assetsRoot.size() > projectRoot.size() && assetsRoot.compare(0, projectRoot.size(), projectRoot) == 0 &&
          assetsRoot[projectRoot.size()] == '/')) {
        return false;
    }
    return true;
}

// task E.4.3: New Folder's unique name. NOT uniqueMaterialFileName, which APPENDS ".aeromat" -- a
// folder has no extension, and reusing it created "NewFolder.aeromat" as a directory (caught by
// I216). Same shape otherwise: "NewFolder", "NewFolder-2", ..., "" on exhaustion, compared
// ASCII-case-insensitively so two names differing only in case cannot collide on a case-insensitive
// filesystem.
[[nodiscard]] std::string uniqueFolderName(std::string_view stem, std::span<const std::string_view> taken,
                                           std::size_t maxAttempts) {
    const auto foldedEqual = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) {
            return false;
        }
        const auto fold = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; };
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (fold(a[i]) != fold(b[i])) {
                return false;
            }
        }
        return true;
    };
    for (std::size_t attempt = 1; attempt <= maxAttempts; ++attempt) {
        std::string candidate(stem);
        if (attempt > 1) {
            candidate += "-" + std::to_string(attempt);
        }
        bool collides = false;
        for (const std::string_view name : taken) {
            if (foldedEqual(name, candidate)) {
                collides = true;
                break;
            }
        }
        if (!collides) {
            return candidate;
        }
    }
    return {};
}

// task E.4.3: one WARN per refusal, in the "{}"-formatted form. AERO_LOG_WARN's FIRST argument is the
// FORMAT STRING, and a two-argument call silently discards the second (E.2.4's finding).
void logAssetOpRefusal(std::string_view verb, std::string_view rel, const AssetOpResult& result) {
    AERO_LOG_WARN("assets: refused to {} '{}' -- {} ({})", verb, rel, result.message,
                  assetOpRefusalLabel(result.refusal));
}

// task E.4.3: the LISTING's own answer for "is this entry a folder", never classifyAssetKind -- which
// cannot tell a folder from an extension-less file, and whose isDirectory argument only ever FORCES
// Folder. The one-shots carry a path and nothing else (the panel's own request structs), so this is
// where EditorApp recovers the fact; `listing` must be the listing of rel's PARENT directory.
[[nodiscard]] bool entryIsDirectory(const DirectoryListing& listing, std::string_view rel) {
    const std::string_view leaf = leafOf(rel);
    for (const FileEntry& entry : listing.entries) {
        if (entry.name == leaf) {
            return entry.isDirectory;
        }
    }
    return false;  // not in the listing: the executor's step 3 refuses it as SourceMissing anyway
}

}  // namespace

bool EditorApp::createAssetFolder(std::string_view parentRel) {
    std::string assetsRoot;
    std::string projectRoot;
    if (!assetOpRoots(assetBrowserPanel, project, assetsRoot, projectRoot)) {
        AERO_LOG_WARN("assets: cannot create a folder -- no project is open");
        return false;
    }
    // Hidden entries INCLUDED: a hidden file still owns its name.
    const DirectoryListing listing = listDirectory(assetsRoot, parentRel, /*includeHidden=*/true);
    std::vector<std::string_view> taken;
    taken.reserve(listing.entries.size());
    for (const FileEntry& entry : listing.entries) {
        taken.emplace_back(entry.name);
    }
    const std::string leaf = uniqueFolderName("NewFolder", taken, MAX_NEW_MATERIAL_ATTEMPTS);
    if (leaf.empty()) {
        AERO_LOG_WARN("assets: cannot create a folder in '{}' -- no unused name after {} attempts",
                      parentRel.empty() ? std::string_view("<assets root>") : parentRel, MAX_NEW_MATERIAL_ATTEMPTS);
        return false;
    }
    AssetOpInputs inputs;
    inputs.sourceRelative = parentRel;
    inputs.newLeaf = leaf;
    const AssetOpResult result =
        executeAssetOpPlan(planAssetOp(AssetOpKind::CreateFolder, inputs, listing), projectRoot, assetsRoot);
    if (!result.performed) {
        logAssetOpRefusal("create a folder in", parentRel.empty() ? std::string_view("<assets root>") : parentRel,
                          result);
        return false;
    }
    AERO_LOG_INFO("assets: created folder '{}'", result.resultingPath);
    assetBrowserPanel->requestSelectEntry(result.resultingPath);
    return true;
}

bool EditorApp::renameAssetEntry(std::string_view rel, std::string_view newLeaf) {
    std::string assetsRoot;
    std::string projectRoot;
    if (!assetOpRoots(assetBrowserPanel, project, assetsRoot, projectRoot)) {
        AERO_LOG_WARN("assets: cannot rename '{}' -- no project is open", rel);
        return false;
    }
    if (assetOpBlockedByDirtyMaterial(rel)) {
        // THE ONE REFUSAL IN THIS TASK THAT DOES NOT RESCAN, because nothing changed: the tree is
        // exactly as the user is looking at it.
        AERO_LOG_WARN(
            "assets: refused to rename '{}' -- the Material panel has unsaved changes to it. "
            "Apply or revert your changes first.",
            rel);
        return false;
    }
    // ONE listing serves both purposes here: a rename's destination directory IS the source's parent.
    const DirectoryListing listing = listDirectory(assetsRoot, parentOf(rel), /*includeHidden=*/true);
    AssetOpInputs inputs;
    inputs.sourceRelative = rel;
    inputs.sourceIsDirectory = entryIsDirectory(listing, rel);
    inputs.newLeaf = newLeaf;
    const AssetOpResult result =
        executeAssetOpPlan(planAssetOp(AssetOpKind::Rename, inputs, listing), projectRoot, assetsRoot);
    if (!result.performed) {
        logAssetOpRefusal("rename", rel, result);
        if (result.torn) {
            AERO_LOG_WARN("assets: '{}' is TORN -- {}. The next scan will attempt re-attachment by content hash.", rel,
                          result.message);
        }
        return false;
    }
    AERO_LOG_INFO("assets: renamed '{}' -> '{}'", rel, result.resultingPath);
    assetBrowserPanel->requestSelectEntry(result.resultingPath);
    return true;
}

bool EditorApp::moveAssetEntry(std::string_view rel, std::string_view destinationDirRel) {
    std::string assetsRoot;
    std::string projectRoot;
    if (!assetOpRoots(assetBrowserPanel, project, assetsRoot, projectRoot)) {
        AERO_LOG_WARN("assets: cannot move '{}' -- no project is open", rel);
        return false;
    }
    if (assetOpBlockedByDirtyMaterial(rel)) {
        AERO_LOG_WARN(
            "assets: refused to move '{}' -- the Material panel has unsaved changes to it. "
            "Apply or revert your changes first.",
            rel);
        return false;
    }
    // TWO listings, because a move's source parent and destination are different directories: the
    // planner needs the DESTINATION's, and sourceIsDirectory is the SOURCE parent's answer.
    const DirectoryListing sourceListing = listDirectory(assetsRoot, parentOf(rel), /*includeHidden=*/true);
    const DirectoryListing listing = listDirectory(assetsRoot, destinationDirRel, /*includeHidden=*/true);
    AssetOpInputs inputs;
    inputs.sourceRelative = rel;
    inputs.sourceIsDirectory = entryIsDirectory(sourceListing, rel);
    inputs.destinationDirRelative = destinationDirRel;
    const AssetOpResult result =
        executeAssetOpPlan(planAssetOp(AssetOpKind::Move, inputs, listing), projectRoot, assetsRoot);
    if (!result.performed) {
        logAssetOpRefusal("move", rel, result);
        if (result.torn) {
            AERO_LOG_WARN("assets: '{}' is TORN -- {}. The next scan will attempt re-attachment by content hash.", rel,
                          result.message);
        }
        return false;
    }
    AERO_LOG_INFO("assets: moved '{}' -> '{}'", rel, result.resultingPath);
    assetBrowserPanel->requestSelectEntry(result.resultingPath);
    return true;
}

bool EditorApp::deleteAssetEntry(std::string_view rel) {
    std::string assetsRoot;
    std::string projectRoot;
    if (!assetOpRoots(assetBrowserPanel, project, assetsRoot, projectRoot)) {
        AERO_LOG_WARN("assets: cannot delete '{}' -- no project is open", rel);
        return false;
    }
    if (assetOpBlockedByDirtyMaterial(rel)) {
        AERO_LOG_WARN(
            "assets: refused to delete '{}' -- the Material panel has unsaved changes to it. "
            "Apply or revert your changes first.",
            rel);
        return false;
    }
    const std::optional<std::uint32_t> sequence = allocateTrashSequence(projectRoot);
    if (!sequence.has_value()) {
        // The bound is SURFACED, never silent, and this is the only place in this task that mentions
        // emptying the trash -- it does not offer to do it.
        AERO_LOG_WARN(
            "assets: cannot delete '{}' -- the project trash at '{}/Library/Trash' is full "
            "({} sequences used). Empty it by hand.",
            rel, projectRoot, MAX_TRASH_SEQUENCE);
        return false;
    }
    // The source parent's listing is read for sourceIsDirectory ALONE: the planner gets an empty one.
    const DirectoryListing sourceListing = listDirectory(assetsRoot, parentOf(rel), /*includeHidden=*/true);
    AssetOpInputs inputs;
    inputs.sourceRelative = rel;
    inputs.sourceIsDirectory = entryIsDirectory(sourceListing, rel);
    inputs.trashSequence = *sequence;
    // Delete passes an EMPTY listing rather than paying a listDirectory on a directory it is about to
    // create: the trash sequence directory is fresh by construction, so rung 8 is skipped for it.
    const AssetOpResult result =
        executeAssetOpPlan(planAssetOp(AssetOpKind::Delete, inputs, DirectoryListing{}), projectRoot, assetsRoot);
    if (!result.performed) {
        logAssetOpRefusal("delete", rel, result);
        if (result.torn) {
            AERO_LOG_WARN("assets: '{}' is TORN -- {}. The next scan will attempt re-attachment by content hash.", rel,
                          result.message);
        }
        return false;
    }
    AERO_LOG_INFO("assets: moved '{}' to the project trash (Library/Trash/{})", rel, *sequence);
    // Step j is SKIPPED for Delete: resultingPath is always "", and requestSelectEntry("") would
    // select the assets root, which is a surprising thing to do after a delete.
    return true;
}

// code-review BLOCKING-1 test seam: stores the id for tick()'s ShellUiState construction to carry --
// see ShellUiState::focusPanelId's own comment for why this exists and drawShellUi's own new block for
// where it is applied (BEFORE DockSpaceOverViewport, the "Edit > Project Settings..." click's own
// timing requirement, generalised).
void EditorApp::requestPanelFocus(std::string_view panelId) { requestedPanelFocus = panelId; }

}  // namespace engine::editor

// F15/2.4.1's precedent, applied to EditorApp itself: this type stays noexcept-movable, so a future
// member whose move can throw fails HERE, loudly, instead of silently degrading EditorApp's own
// defaulted... spelled-out move (task 2.5.1: no longer `= default` IN THE HEADER, so the assert
// cannot live there any more -- it moves here, where EditorApp is complete). AC-34's mechanical proof.
static_assert(std::is_nothrow_move_constructible_v<engine::editor::EditorApp>);
static_assert(std::is_nothrow_move_assignable_v<engine::editor::EditorApp>);

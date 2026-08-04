#pragma once
// Aero Engine — src-private: the read-only Asset browser panel (task 2.2.4). This HEADER is
// ImGui-free (editor_app.cpp registers the class and never sees ImGui); every ImGui call lives in
// asset_browser_panel.cpp, which in turn includes NO <filesystem> -- all disk access goes through
// <aero/editor/project_files.hpp> (INV-2, grep-asserted in §V6).
//
// FRAME SHAPE (D8, the hierarchy_panel.hpp:6-16 discipline) -- onDraw is exactly five phases:
//   1. reconcile  IsWindowAppearing -> drop the cache; fill the cache for "" / currentDir / openDirs;
//                 rebuild visibleRows when dirty. THE ONLY PLACE I/O HAPPENS (D7).
//   2. header     Refresh, Show hidden, breadcrumb -- RECORDED only
//   3. left pane  the directory tree, strictly READ-ONLY
//   4. right pane the current directory's contents, strictly READ-ONLY
//   5. footer     the selection / status line, strictly READ-ONLY
//   apply         one switch over `pending` -- the ONLY place currentDir, openDirs, cache,
//                 showHidden or selectedEntry is written (INV-5)
// Phases 1 and apply bracket the ImGui work so that scanning mid-draw -- which would rehash `cache`
// while buildVisibleTree holds a reference into it -- is impossible by construction.
// EVERY walk is an explicit stack, never a recursive function (INV-4/F23).
//
// READ-ONLY BY CONTRACT (D19): this panel creates, renames, moves, deletes and opens nothing.
#include <aero/editor/panel.hpp>
#include <aero/editor/project_files.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::editor {

// Forward-declared, never #included here: the header only needs the NAME for a pointer member (D13
// of task 3.1.1's plan) -- the ViewportPanel/ConsolePanel precedent, applied a fourth time. The .cpp
// includes <aero/editor/asset_database.hpp>.
class AssetDatabase;

class AssetBrowserPanel final : public Panel {
public:
    explicit AssetBrowserPanel(std::string rootPath);

    // FROZEN (D20/INV-1): "Assets" is the ImGui window name AND the imgui.ini settings key
    // (panel.hpp:46-47), and it has been written into every aero_editor.ini since 2.1.3. Renaming it
    // orphans every existing user's saved layout for this panel.
    [[nodiscard]] const char* id() const noexcept override { return "Assets"; }
    // Bottom, shared as a TAB with "Console" (2.2.5). shell_ui.cpp:137-139 splits that node exactly
    // once for both -- zero layout-code change.
    [[nodiscard]] DockSlot defaultDockSlot() const noexcept override { return DockSlot::Bottom; }
    // `context` is IGNORED (D18): no World read, no Selection read or write, no Entity stored.
    // Drag-into-scene is 3.1.3's, and shipping a dead coupling now would be exactly the dead code
    // shell_ui.cpp:19-24 refuses to ship for menu items. options() is deliberately NOT overridden.
    void onDraw(PanelContext& context) override;

    // Task 2.6.1's ENTIRE integration point: called from EditorApp::tick()'s per-frame reconcile
    // (D10) whenever this root disagrees with ProjectSession::assetsRoot(). Clears the cache, the
    // open set, the current directory and the selection, so no stale row can survive it (E18).
    void setRoot(std::string rootPath);
    [[nodiscard]] const std::string& root() const noexcept { return rootUtf8; }

    // task 3.1.1 -- the reconcile block's other half (D12). A non-owning, NEVER-owning pointer: this
    // panel does not scan disk (D7 stays true), EditorApp does, in tick(), outside the draw walk.
    // NEVER a reference (D13): EditorApp is movable and create() returns std::optional<EditorApp>, so
    // a reference bound at construction would dangle the moment the returned optional is moved from.
    // The member is `databasePtr`, not `database` -- a data member and a member function cannot share
    // a name (the `RenderTarget::depthFormatValue` / `depthFormat()` precedent, ci-portability.md's
    // "distinct name on accessor collision" rule).
    void setDatabase(const AssetDatabase* db) noexcept { databasePtr = db; }
    [[nodiscard]] const AssetDatabase* database() const noexcept { return databasePtr; }

    // One-shot, read-and-clear (AC-38): the Refresh button's ActionKind::Refresh arm sets it; the
    // reconcile block drains it in the same expression as the database's own root comparison.
    [[nodiscard]] bool takeRescanRequest() noexcept {
        const bool requested = rescanRequested;
        rescanRequested = false;
        return requested;
    }

private:
    // performance-enum-size: the explicit underlying type is mandatory, like every engine enum.
    enum class ActionKind : std::uint8_t {
        None = 0,
        Navigate,      // path: the directory to make current
        ToggleDir,     // path: the directory whose expansion flips
        SelectEntry,   // path: the entry to select (relative to the root)
        Refresh,       // path: unused
        ToggleHidden,  // path: unused
    };
    struct PendingAction {
        ActionKind kind = ActionKind::None;
        std::string path;
    };

    void record(ActionKind kind, std::string path);  // last writer wins; at most one click per frame
    bool ensureCached(const std::string& rel);       // returns true if it actually scanned
    void reconcile();                                // phase 1 -- the ONLY place I/O happens
    void drawHeader();                               // phase 2
    void drawTreePane(float paneHeight);             // phase 3
    void drawContentsPane(float paneHeight);         // phase 4
    void drawFooter();                               // phase 5
    void applyPending();                             // the ONE mutating switch
    void openAncestors(const std::string& path);     // iterative; never opens `path` itself (C9)
    [[nodiscard]] const DirectoryListing* cached(const std::string& rel) const;

    std::string rootUtf8;
    std::string currentDir;     // relative; "" == the root
    std::string selectedEntry;  // relative; "" == nothing selected
    std::set<std::string> openDirs;
    std::unordered_map<std::string, DirectoryListing> cache;
    std::vector<TreeRow> visibleRows;     // rebuilt ONLY when treeDirty; capacity reused (D15)
    std::vector<std::string> breadcrumb;  // per-frame scratch, NOT model state
    std::string labelScratch;             // per-frame scratch, NOT model state (the 2.2.1 idiom)
    PendingAction pending;
    bool showHidden = false;
    bool treeDirty = true;
    const AssetDatabase* databasePtr = nullptr;  // task 3.1.1 -- reconciled, never owned (D13)
    bool rescanRequested = false;                // task 3.1.1 -- one-shot, drained by takeRescanRequest()
};

}  // namespace engine::editor

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
#include <aero/editor/asset_view.hpp>  // task 3.1.3 -- AssetViewMode, TileSize (pure enums, ImGui-free)
#include <aero/editor/panel.hpp>
#include <aero/editor/project_files.hpp>
#include <aero/editor/thumbnail_cache.hpp>  // task 3.1.3 -- ThumbnailKey/ThumbnailLedger (pure, GPU-free)

#include "thumbnail_store.hpp"  // task 3.1.3 -- src-private: the ONLY stb/GPU TU for thumbnails

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::rhi {
class Device;  // forward-declared, never #included here (A17) -- viewport_panel.hpp:19-21's shape
}  // namespace engine::rhi

namespace engine::editor {

// Forward-declared, never #included here: the header only needs the NAME for a pointer member (D13
// of task 3.1.1's plan) -- the ViewportPanel/ConsolePanel precedent, applied a fourth time. The .cpp
// includes <aero/editor/asset_database.hpp>.
class AssetDatabase;
struct AssetScanReport;  // task 3.1.3 -- the SAME forward-declaration-only precedent, applied to a
                         // pointer member's element type.

class AssetBrowserPanel final : public Panel {
public:
    // task 3.1.3 (A17): `device` defaults to nullptr, which keeps thumbnails deliberately unavailable
    // (E13/AC-11) -- store.available() stays false, drawTile falls back to the icon path forever, and
    // nothing crashes. EditorApp::create() always passes a real device.
    explicit AssetBrowserPanel(std::string rootPath, rhi::Device* device = nullptr);

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

    // task 3.1.3 (D11): the SAME reconcile shape as setDatabase() above -- a non-owning, NEVER-owning
    // pointer, reconciled unconditionally every tick, never a reference (the identical D13 reasoning).
    // "The report IS the issues list": no second computation, no second source of truth.
    void setScanReport(const AssetScanReport* report) noexcept { reportPtr = report; }

    // One-shot, read-and-clear (AC-38): the Refresh button's ActionKind::Refresh arm sets it; the
    // reconcile block drains it in the same expression as the database's own root comparison.
    [[nodiscard]] bool takeRescanRequest() noexcept {
        const bool requested = rescanRequested;
        rescanRequested = false;
        return requested;
    }

    // task 3.1.2 (§D-11/A15): the `takeRescanRequest()` shape verbatim, for the Reimport All button's
    // OWN one-shot -- a strict superset of Refresh (it also discards the committed import cache, AC-39).
    [[nodiscard]] bool takeReimportRequest() noexcept {
        const bool requested = reimportRequested;
        reimportRequested = false;
        return requested;
    }

    // task 3.1.3 (D8): the ONLY thumbnail mutator, called from EditorApp::tick() OUTSIDE the draw
    // walk (the ViewportPanel::renderScene precedent) -- touches visible keys, evicts beyond the cap
    // BEFORE decoding (so the bound stated in the footer is exactly true), then decodes at most
    // MAX_THUMBNAIL_DECODES_PER_TICK Absent keys.
    void serviceThumbnails();

    // code-review BLOCKING-1: the ImGui-free-at-source GPU tier cannot click the Reimport All button,
    // and EditorApp::requestAssetReimport() (task 3.1.2) drives a DIFFERENT path (the deep rescan
    // itself) that never touches `pending` at all -- it cannot reproduce the same-tick draw-then-clear
    // race this finding fixed. This is the identical effect a real click on the button has: it queues
    // ActionKind::ReimportAll exactly as record() would from drawHeader(), so the NEXT onDraw() drains
    // it through the SAME applyPending() arm a click would.
    void requestReimportAll() noexcept;

    // code-review finding 4: the same seam, three more times. The GPU tier is ImGui-free at source, so
    // it cannot flip the Grid/List radio, type in the search box, or click an orphan's Delete button --
    // which left drawContentsList()'s search branch (an asymmetric BeginTable/EndTable pair plus a
    // clipper), drawContentsGrid()'s search branch, and the confirmation modal executed by NO test at
    // all. Each records exactly the action a real widget records, so the next onDraw() drains it
    // through the SAME applyPending() arm the widget would. One action per frame (`pending` is a single
    // slot), so a caller ticks between them.
    void requestViewMode(AssetViewMode mode) noexcept;
    void requestSearchQuery(std::string query);
    void requestKindFilter(std::string kind);
    void requestDeleteOrphanClick(std::string relativeMetaPath);

    // task 3.1.3, Step 11: one-shot, read-and-clear -- the takeRescanRequest() shape, a third
    // instance (F9). Set only by the modal's Delete button confirming; drained by EditorApp::tick()'s
    // reconcile block, as its OWN statement (I42's mechanical proof).
    [[nodiscard]] std::string takeOrphanDeleteRequest() noexcept {
        std::string requested = std::move(confirmedOrphanDelete);
        confirmedOrphanDelete.clear();
        return requested;
    }

    // task 3.1.3 (A12): black-box observability for the GPU tier, forwarded by EditorApp -- the
    // assetCacheEntryCount() shape verbatim.
    [[nodiscard]] std::size_t thumbnailReadyCount() const noexcept { return ledger.readyCount(); }
    [[nodiscard]] std::size_t thumbnailUnavailableCount() const noexcept { return ledger.unavailableCount(); }
    [[nodiscard]] std::size_t thumbnailResidentCount() const noexcept { return store.residentCount(); }
    [[nodiscard]] std::size_t thumbnailLoadAttempts() const noexcept { return store.loadAttempts(); }

    // code-review finding 4: the same black-box observability, for the SEARCH half. Without these a
    // GPU-tier case can drive the seams above and still prove nothing -- the branch it means to cover
    // is entered only when `searchRows.hits` is non-empty, so a case that silently searched zero
    // records would pass while executing none of the code it names. (Exactly that happened once: a
    // deliberately unbalanced EndTable failed to redden I39 because the panel was tabbed behind the
    // Console and never drew at all.)
    [[nodiscard]] std::size_t searchHitCount() const noexcept { return searchRows.hits.size(); }
    [[nodiscard]] bool listViewActive() const noexcept { return viewMode == AssetViewMode::List; }
    [[nodiscard]] bool deleteModalPending() const noexcept { return !pendingOrphanDelete.empty(); }

private:
    // performance-enum-size: the explicit underlying type is mandatory, like every engine enum.
    enum class ActionKind : std::uint8_t {
        None = 0,
        Navigate,       // path: the directory to make current
        ToggleDir,      // path: the directory whose expansion flips
        SelectEntry,    // path: the entry to select (relative to the root)
        Refresh,        // path: unused
        ToggleHidden,   // path: unused
        ReimportAll,    // path: unused -- task 3.1.2, APPENDED (never inserted -- performance-enum-size)
        SetViewMode,    // path: "grid" or "list" -- task 3.1.3, Step 6, APPENDED
        SetTileSize,    // path: "small"/"medium"/"large" -- task 3.1.3, Step 6, APPENDED
        SetQuery,       // path: the new query text -- task 3.1.3, Step 8, APPENDED
        ClearSearch,    // path: unused -- task 3.1.3, Step 8, APPENDED
        SetKindFilter,  // path: "all" or a single digit (static_cast<int>(AssetKind)) -- Step 8, APPENDED
        RevealPath,     // path: the full relative path a double-clicked search hit named -- Step 8, APPENDED
        // path: the orphan's project-relative .meta path -- task 3.1.3, Step 9, APPENDED. Sets
        // pendingOrphanDelete and NOTHING else; the modal that turns this into a real delete is Step 11.
        RequestDeleteOrphan,
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
    // phase 4 -- task 3.1.3, Step 6: ONE child (##contents), TWO bodies. drawContentsList is today's
    // drawContentsPane, renamed and otherwise BYTE-IDENTICAL (D3/AC-2); drawContentsGrid is new.
    void drawContentsList(float paneHeight);
    void drawContentsGrid(float paneHeight);
    // task 3.1.3, Step 8: `isSearchHit` changes the double-click semantics (RevealPath instead of
    // Navigate -- a search hit is always a file, never a folder) and folds the parent folder into the
    // caption so a project-wide result stays identifiable outside its own directory.
    void drawTile(const FileEntry& entry, const std::string& rel, float tileW, float tileH, float tileEdge, float pad,
                  bool isSearchHit);
    void drawIssues();    // phase 4b -- task 3.1.3, Step 9 (D11)
    void drawFooter();    // phase 5
    void applyPending();  // the ONE mutating switch
    // task 3.1.3, Step 8: rebuilds `searchRows` from the COMMITTED filter -- called from applyPending
    // ONLY (SetQuery/SetKindFilter), never from the draw walk, so the draw walk never searches.
    void refreshSearchRows();
    void openAncestors(const std::string& path);  // iterative; never opens `path` itself (C9)
    [[nodiscard]] const DirectoryListing* cached(const std::string& rel) const;

    // task 3.1.3 (INV-V3): std::nullopt unless ALL SEVEN conditions hold -- in ONE function so no
    // call site can forget one. See the .cpp for the full list.
    [[nodiscard]] std::optional<ThumbnailKey> thumbnailKeyFor(const FileEntry& entry, const std::string& rel) const;
    // "" when the record behind `key` has vanished (a rescan raced the decode) -- the caller treats
    // an empty path as Failed, never as "try again".
    [[nodiscard]] std::string absolutePathFor(const ThumbnailKey& key) const;

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
    bool reimportRequested = false;              // task 3.1.2 -- one-shot, drained by takeReimportRequest() (A15)

    // task 3.1.3, Step 6 -- D3/D4: SESSION state, deliberately not persisted (AC-34/D4's documented
    // "resets to Grid/Medium on relaunch" limitation).
    AssetViewMode viewMode = AssetViewMode::Grid;
    TileSize tileSize = TileSize::Medium;

    // task 3.1.3, Step 7 -- the two-phase thumbnail wiring (D8). `store`/`ledger` are member-named,
    // not `databasePtr`-style pointers: ThumbnailStore/ThumbnailLedger are OWNED here, one per panel.
    ThumbnailLedger ledger;
    ThumbnailStore store;
    std::vector<ThumbnailKey> visibleThumbnailKeys;  // per-frame scratch, cleared in phase 1
    std::uint64_t frameCounter = 0;                  // the LRU's clock; monotonic, NEVER wall time
    // code-review BLOCKING-1: set by applyPending()'s ReimportAll arm, consumed by the NEXT
    // serviceThumbnails() call -- NEVER an immediate ledger.clear()/store.clear() from inside the draw
    // walk (that destroyed a texture this SAME frame's drawTile() had already written into the ImGui
    // draw list, a use-after-free that is synchronous on Vulkan/D3D12 and merely deferred on Metal).
    // serviceThumbnails() drains this AFTER its own touch loop, so anything drawn (and therefore
    // touched) THIS frame is excluded by the SAME evictions()-vs-`currentFrame` rule that already
    // protects normal cap eviction (E12) -- it survives one more tick, harmlessly, rather than being
    // destroyed before this frame's endFrame() has consumed the draw data that references it.
    bool pendingThumbnailReimportClear = false;

    // task 3.1.3, Step 8 -- the whole-project search (D5: AssetDatabase::records() IS the index).
    AssetFilter filter;        // COMMITTED; the search box edits a SCRATCH copy (queryScratch)
    std::string queryScratch;  // per-frame scratch, NOT model state
    SearchResult searchRows;   // rebuilt in applyPending() when the query/kind filter changes

    // task 3.1.3, Step 9 -- the retained scan report (D11) and the delete confirmation's own state.
    const AssetScanReport* reportPtr = nullptr;  // reconciled, never owned (the databasePtr precedent)
    std::string pendingOrphanDelete;             // "" == no modal open
    std::string confirmedOrphanDelete;           // task 3.1.3, Step 11 -- one-shot, drained by
                                                 // takeOrphanDeleteRequest()
    // code-review finding 10: authoritative over ImGui's own per-id CollapsingHeader persistence,
    // which the "Issues (N)" label's changing id would otherwise silently defeat (drawIssues()'s
    // own comment has the full reasoning) -- the D5 precedent drawTreePane's `row.open` already sets.
    bool issuesOpen = false;
};

}  // namespace engine::editor

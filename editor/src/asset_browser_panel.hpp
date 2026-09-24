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
#include <aero/core/guid.hpp>             // task 3.1.4 -- abstainingScratch's element type, used DIRECTLY
#include <aero/editor/asset_actions.hpp>  // task E.4.3 -- AssetOpRefusal, AssetNameRefusal (pure)
#include <aero/editor/asset_view.hpp>     // task 3.1.3 -- AssetViewMode, TileSize (pure enums, ImGui-free)
#include <aero/editor/panel.hpp>
#include <aero/editor/project_files.hpp>
#include <aero/editor/thumbnail_cache.hpp>  // task 3.1.3 -- ThumbnailKey/ThumbnailLedger (pure, GPU-free)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::editor {

// Forward-declared, never #included here: the header only needs the NAME for a pointer member (D13
// of task 3.1.1's plan) -- the ViewportPanel/ConsolePanel precedent, applied a fourth time. The .cpp
// includes <aero/editor/asset_database.hpp>.
class AssetDatabase;
struct AssetScanReport;  // task 3.1.3 -- the SAME forward-declaration-only precedent, applied to a
                         // pointer member's element type.
struct WatchStatus;      // task 3.1.4 -- the SAME forward-declaration-only precedent as AssetScanReport:
                         // the header needs only the NAME for a pointer member, so it does not pull in
                         // <aero/editor/asset_watcher.hpp>. The .cpp includes it.
class ThumbnailService;  // task E.3.3 -- the SAME precedent again; the .cpp includes thumbnail_service.hpp

// ---- task E.4.3: the panel's one-shot payloads ---------------------------------------------------
// They live HERE, on the src-private header, not on a public one: nothing outside the panel/EditorApp
// pair names them, and editor_app.cpp already includes this file.
struct AssetRenameRequest {
    std::string path;     // the entry's assets-relative path
    std::string newLeaf;  // the typed leaf, UNVALIDATED -- validateAssetName runs in EditorApp
};
struct AssetMoveRequest {
    std::string path;            // the source
    std::string destinationDir;  // "" == the assets root, and is LEGAL
};

class AssetBrowserPanel final : public Panel {
public:
    // task E.3.3: the rhi::Device* parameter is GONE -- the store it used to construct now lives in
    // ThumbnailService, which EditorApp owns and lends through setThumbnails(). A panel with no service
    // lent keeps 3.1.3's E13/AC-11 behaviour exactly: every tile falls back to the icon path forever
    // and nothing crashes.
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

    // task 3.2.1 (D17/F11): the ONE line this task adds to the Asset Browser. A data member and a
    // member function may not share a name (the databasePtr/database() and recordList/records()
    // precedent, which 3.1.3 hit as a real compile error), so the accessor is `selection()` while the
    // member stays `selectedEntry`. onDraw, the five draw phases and every existing test are untouched.
    [[nodiscard]] const std::string& selection() const noexcept { return selectedEntry; }

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

    // task E.3.3 (D5): the service is RECONCILED, never owned -- set ONCE in EditorApp::create(), right
    // after this panel's own emplace, because it points at a heap object EditorApp holds through a
    // unique_ptr and that address survives the app's own move. NULL is a legal state.
    void setThumbnails(ThumbnailService* service) noexcept { thumbnailsPtr = service; }

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

    // DEVIATION (task 3.2.1, logged in the final report): the code-review finding 4 shape, a FIFTH
    // instance -- records EXACTLY what a real single click on a row/tile records (ActionKind::
    // SelectEntry), so ModelImportSession's reconcile (EditorApp::tick(), which reads selection()
    // every frame) is drivable from the ImGui-free-at-source GPU tier. "" selects nothing, exactly
    // like Navigate clearing it. Without this, AC-45/AC-46/AC-47/AC-50's own GPU-tier proof (I53-I59)
    // would be unwritable or vacuous -- no other seam reaches `selectedEntry`.
    void requestSelectEntry(std::string relativePath);

    // task 3.1.3, Step 11: one-shot, read-and-clear -- the takeRescanRequest() shape, a third
    // instance (F9). Set only by the modal's Delete button confirming; drained by EditorApp::tick()'s
    // reconcile block, as its OWN statement (I42's mechanical proof).
    [[nodiscard]] std::string takeOrphanDeleteRequest() noexcept {
        std::string requested = std::move(confirmedOrphanDelete);
        confirmedOrphanDelete.clear();
        return requested;
    }

    // task 3.1.4: 2.2.4's watcher seam, made callable. It is EXACTLY `cache.clear(); treeDirty = true;`
    // and nothing else -- it must NOT touch currentDir, openDirs, selectedEntry, the search rows, the
    // ledger or the store. Called from EditorApp's reconcile whenever AssetDatabase::generation()
    // changed, which makes listing invalidation uniform across EVERY rescan trigger (D8) and closes a
    // pre-existing gap: requestAssetRescan() rescanned the database but never dropped these listings,
    // so a test-driven rescan could leave rows on screen for files that no longer existed.
    void invalidateListings();

    // task 3.1.4 (D10): reconciled every tick, never owned -- the setDatabase()/setScanReport()
    // precedent, a THIRD instance. EditorApp owns the watcher; the panel only renders its state.
    void setWatchStatus(const WatchStatus* status) noexcept { watchStatusPtr = status; }

    // task 3.1.4 (D10): one-shot, read-and-clear -- the takeOrphanDeleteRequest() shape, a FOURTH
    // instance. std::nullopt == the checkbox was not touched this frame. Drained by EditorApp's
    // reconcile as its OWN statement (F9).
    [[nodiscard]] std::optional<bool> takeWatchToggleRequest() noexcept {
        const std::optional<bool> requested = watchToggleRequest;
        watchToggleRequest.reset();
        return requested;
    }

    // task 3.4.2 (D9/AC-5): the New Material button's one-shot -- the takeWatchToggleRequest() shape,
    // a FIFTH instance, and an OPTIONAL rather than a bare string for a reason: the payload is the
    // directory to create in, and "" is the legitimate value for the ROOT, so an empty string cannot
    // also mean "nothing was requested". Drained by EditorApp's reconcile as its OWN statement (F9);
    // THIS PANEL NEVER TOUCHES DISK (D7/D19 -- it is read-only by contract, and creation happens
    // outside the draw walk in tick(), exactly like Refresh's rescan).
    [[nodiscard]] std::optional<std::string> takeCreateMaterialRequest() noexcept {
        std::optional<std::string> requested = std::move(createMaterialRequest);
        createMaterialRequest.reset();  // a moved-from optional is still ENGAGED -- the move is not a drain
        return requested;
    }

    // task 3.4.2: the requestReimportAll() shape verbatim -- identical in effect to a real click on the
    // New Material button, because it queues the SAME ActionKind the button queues, so the next
    // onDraw() drains it through the SAME applyPending() arm. The ImGui-free-at-source GPU tier has no
    // other way to press a widget.
    void requestCreateMaterial() noexcept;

    // task 3.1.3 (A12): black-box observability for the GPU tier, forwarded by EditorApp -- the
    // assetCacheEntryCount() shape verbatim.
    // code-review finding 4: the same black-box observability, for the SEARCH half. Without these a
    // GPU-tier case can drive the seams above and still prove nothing -- the branch it means to cover
    // is entered only when `searchRows.hits` is non-empty, so a case that silently searched zero
    // records would pass while executing none of the code it names. (Exactly that happened once: a
    // deliberately unbalanced EndTable failed to redden I39 because the panel was tabbed behind the
    // Console and never drew at all.)
    [[nodiscard]] std::size_t searchHitCount() const noexcept { return searchRows.hits.size(); }
    [[nodiscard]] bool listViewActive() const noexcept { return viewMode == AssetViewMode::List; }
    [[nodiscard]] bool deleteModalPending() const noexcept { return !pendingOrphanDelete.empty(); }

    // ---- task E.4.3: the four one-shots ----------------------------------------------------------
    // Three are OPTIONAL for takeCreateMaterialRequest's stated reason -- "" is the LEGITIMATE value
    // for the assets root, so an empty string cannot also mean "nothing was requested". Each is
    // drained by EditorApp::tick()'s reconcile block as its OWN statement (F9).
    //
    // EVERY TAKER THAT MOVES OUT OF AN OPTIONAL CALLS .reset() AFTERWARDS: a moved-from optional is
    // still ENGAGED -- the move is not a drain.
    [[nodiscard]] std::optional<std::string> takeNewFolderRequest() noexcept {
        std::optional<std::string> requested = std::move(newFolderRequest);
        newFolderRequest.reset();
        return requested;
    }
    [[nodiscard]] std::optional<AssetRenameRequest> takeRenameRequest() noexcept {
        std::optional<AssetRenameRequest> requested = std::move(renameRequest);
        renameRequest.reset();
        return requested;
    }
    [[nodiscard]] std::optional<AssetMoveRequest> takeMoveRequest() noexcept {
        std::optional<AssetMoveRequest> requested = std::move(moveRequest);
        moveRequest.reset();
        return requested;
    }
    // A PLAIN string on takeOrphanDeleteRequest's precedent: "" is the assets root and the root is
    // never deletable (rung 1 -> SourceIsRoot), so "" is unambiguous here in a way it is not for the
    // other three.
    [[nodiscard]] std::string takeDeleteRequest() noexcept {
        std::string requested = std::move(deleteRequest);
        deleteRequest.clear();
        return requested;
    }

    // ---- task E.4.3: the seven GPU-tier seams ----------------------------------------------------
    // Each records EXACTLY what a real widget records, so the next onDraw() drains it through the
    // SAME applyPending() arm -- the code-review-finding-4 shape, applications six through twelve.
    //
    // Rename and Delete each need TWO seams, one that OPENS the modal and one that COMMITS it,
    // because a single commit-seam would set the one-shot and prove nothing about whether the modal
    // ever drew (E.3.4's S26, applied rather than rediscovered). requestRenameCommit and
    // requestDeleteConfirm do NOT go through applyPending: they model the modal's BUTTON, which sets
    // the one-shot directly, exactly as the orphan modal's Delete button sets confirmedOrphanDelete
    // inside the popup body.
    void requestContextMenu(std::string path);
    void requestNewFolder() noexcept;
    void requestRename(std::string path);
    void requestRenameCommit(std::string newLeaf);
    void requestDelete(std::string path);
    void requestDeleteConfirm() noexcept;
    void requestMove(std::string path, std::string destinationDir);

    // An EIGHTH seam, and §1.4.3's count of seven deliberately does NOT include it: seven is the
    // count of seams that model a USER GESTURE through applyPending. This one models a PAYLOAD STATE
    // and has no applyPending arm at all. Nothing in tests/ can perform a real drag -- the backend
    // rewrites io.MousePos every NewFrame -- so the payload state is injected and the counters below
    // report what the target really did.
    void requestDropPeek(std::string sourcePath, std::string destinationDir, bool asMovePayload);

    // ---- task E.4.3: the observables --------------------------------------------------------------
    // The first two report what was REQUESTED. The rest are EFFECT counters, incremented inside the
    // body that actually ran, and they exist because a seam's own accessor is a round trip: it says
    // what was asked for, never whether ImGui obeyed. E.3.4's seed S26 walked through 149 green
    // assertions for exactly the missing form of this. All reset at the TOP of every onDraw.
    [[nodiscard]] bool renameModalPending() const noexcept { return !pendingRename.empty(); }
    // A DISTINCT NAME from the pre-existing deleteModalPending() (the ORPHAN modal), so no existing
    // test silently changes meaning.
    [[nodiscard]] bool assetDeleteModalPending() const noexcept { return !pendingDelete.empty(); }
    [[nodiscard]] std::size_t contextMenuItemsDrawn() const noexcept { return contextMenuItemsDrawnCount; }
    [[nodiscard]] std::size_t renameModalDrawnCount() const noexcept { return renameModalDrawn; }
    [[nodiscard]] std::size_t assetDeleteModalDrawnCount() const noexcept { return assetDeleteModalDrawn; }
    [[nodiscard]] AssetOpRefusal lastDropPeekRefusal() const noexcept { return dropPeekRefusal; }
    // Counts AcceptDragDropPayload calls the drop-target body ACTUALLY made this frame.
    [[nodiscard]] std::size_t dropTargetsAccepted() const noexcept { return dropTargetsAcceptedCount; }
    [[nodiscard]] const std::string& contextMenuTarget() const noexcept { return contextTarget; }

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
        // path: "1" to enable, "0" to disable -- task 3.1.4 (D10), APPENDED (never inserted --
        // performance-enum-size, and this enum's own existing comment rule).
        SetAutoRefresh,
        // path: unused -- task 3.4.2 (D9/AC-5), APPENDED (this enum's own rule, again). The arm sets
        // createMaterialRequest to the CURRENT DIRECTORY and nothing else; the file is written by
        // EditorApp::tick(), outside the draw walk.
        CreateMaterial,
        // ---- task E.4.3, APPENDED (this enum's own performance-enum-size rule, again) ------------
        // path: the entry right-clicked; "" is the pane BACKGROUND, which is a legal target (New
        // Folder and New Asset apply there). ONE action, THREE effects -- selectedEntry, contextTarget
        // and contextMenuRequested -- because `pending` is one last-writer-wins slot (F1), so
        // recording a separate SelectEntry would clobber this one and the menu would open on a stale
        // target.
        OpenContextMenu,
        // path: unused. The arm reads `currentDir` -- applyPending is the one place that sees
        // committed state -- and sets newFolderRequest. CreateMaterial's arm verbatim.
        NewFolder,
        // path: the entry to rename. Sets pendingRename and NOTHING else; the modal that turns this
        // into a real rename draws in phase 4b.
        RequestRename,
        // path: the entry to delete. Sets pendingDelete and NOTHING else; same shape.
        RequestDelete,
        // path: the source; destination: the destination DIRECTORY ("" == the assets root, legal).
        // The ONLY kind that reads PendingAction::destination.
        MoveEntry,
    };
    struct PendingAction {
        ActionKind kind = ActionKind::None;
        std::string path;
        // task E.4.3: read by MoveEntry ALONE. Stated here so a future kind does not quietly acquire
        // a second meaning for it.
        std::string destination;
    };

    void record(ActionKind kind, std::string path);  // last writer wins; at most one click per frame
    // task E.4.3: the one-path form DELEGATES to this with destination = "", so no existing call site
    // changes -- which is what keeps the I-tier's existing browser cases passing unedited.
    void record(ActionKind kind, std::string path, std::string destination);
    bool ensureCached(const std::string& rel);  // returns true if it actually scanned
    void reconcile();                           // phase 1 -- the ONLY place I/O happens
    void drawHeader();                          // phase 2
    void drawTreePane(float paneHeight);        // phase 3
    // phase 4 -- task 3.1.3, Step 6: ONE child (##contents), TWO bodies. drawContentsList is today's
    // drawContentsPane, renamed and otherwise BYTE-IDENTICAL (D3/AC-2); drawContentsGrid is new.
    void drawContentsList(float paneHeight);
    void drawContentsGrid(float paneHeight);
    // task 3.1.3, Step 8: `isSearchHit` changes the double-click semantics (RevealPath instead of
    // Navigate -- a search hit is always a file, never a folder) and folds the parent folder into the
    // caption so a project-wide result stays identifiable outside its own directory.
    void drawTile(const FileEntry& entry, const std::string& rel, float tileW, float tileH, float tileEdge, float pad,
                  bool isSearchHit);
    // task 3.1.5 (§0.11/§D-21): attaches the AERO_ASSET drag source to the item JUST SUBMITTED. Call
    // IMMEDIATELY after the Selectable and BEFORE anything that reads g.LastItemData or the window
    // draw list. Starts no drag at all for a folder, a non-draggable kind, or a path with no database
    // record / no valid guid -- the SOURCE-side twin of the peek rule (D11): refusal happens here, so
    // an illegal payload never exists. ONE helper, THREE call sites; it is 1:1 Begin/End internally
    // and contains no return between the pair.
    // task E.4.3: `isDirectory` is NON-DEFAULTED on purpose. The function CANNOT derive it -- it has
    // only a path, and classifyAssetKind's own isDirectory argument was hardcoded false here because
    // until E.4.3 a folder and an extension-less file took the same early return. They now take
    // DIFFERENT arms (a folder has no sidecar, so its plan has one step and not two), so a default
    // would let a future call site silently encode a folder as a file, with no error and no failing
    // test. Non-defaulted makes every unconverted site a compile error, which is what makes this
    // change atomic.
    void beginAssetDragSource(const std::string& relativePath, const char* previewText, bool isDirectory);
    // task E.4.3: attaches a folder drop target to the item JUST SUBMITTED. Call IMMEDIATELY after the
    // item and BEFORE anything reads g.LastItemData. EndDragDropTarget runs ONLY when
    // BeginDragDropTarget returned true -- F18's asymmetry, the opposite of EndChild's rule, and
    // getting it backwards is an IM_ASSERT abort rather than a wrong picture.
    void attachFolderDropTarget(const std::string& folderRelative);
    // task E.4.3: the move arm of the drag source, as its OWN function rather than a second branch,
    // so each stays 1:1 Begin/End with no return between the pair.
    void beginAssetMoveBranch(const std::string& relativePath, const char* previewText, AssetKind kind,
                              bool isDirectory);
    // task E.4.3: the drop DECISION, shared by the real ImGui target and the injected seam, so the
    // two cannot diverge. It IS classifyAssetMove, which is itself a call to assetOpPathLadder.
    [[nodiscard]] static AssetOpRefusal folderDropVerdict(const std::string& source, const std::string& folderRelative);
    void applyInjectedDropPeek();
    void drawIssues();  // phase 4b -- task 3.1.3, Step 9 (D11)
    // task E.4.3 -- also phase 4b, drawn AFTER the orphan modal. The two modals are gated on
    // pendingOrphanDelete.empty() by their caller, so the pre-existing one always wins.
    void drawContextMenu();
    void drawRenameModal();
    void drawAssetDeleteModal();
    void drawFooter();    // phase 5
    void applyPending();  // the ONE mutating switch
    // task 3.1.3, Step 8: rebuilds `searchRows` from the COMMITTED filter -- called from applyPending
    // ONLY (SetQuery/SetKindFilter), never from the draw walk, so the draw walk never searches.
    void refreshSearchRows();
    void openAncestors(const std::string& path);  // iterative; never opens `path` itself (C9)
    [[nodiscard]] const DirectoryListing* cached(const std::string& rel) const;

    // task 3.1.3 (INV-V3): std::nullopt unless ALL SEVEN conditions hold -- in ONE function so no
    // call site can forget one. task E.3.3 split it: guards 1 (a folder) and 3 (no database) stay
    // here, and the other five moved into the pure thumbnailKeyForRecord, which a record is all that
    // is needed for -- so the picker's tile asks the IDENTICAL question rather than a similar one.
    [[nodiscard]] std::optional<ThumbnailKey> thumbnailKeyFor(const FileEntry& entry, const std::string& rel) const;

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

    // task E.3.3 (D5): the ledger, the store, the LRU clock, the per-frame scratch and the two pending
    // flags all moved into ThumbnailService, which EditorApp owns and three consumers share. What is
    // left here is a borrowed pointer -- the databasePtr/reportPtr posture, a fourth instance.
    ThumbnailService* thumbnailsPtr = nullptr;

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

    // ---- task 3.1.4 ---------------------------------------------------------------------------
    // Reconciled, never owned (the databasePtr/reportPtr precedent, a third instance). NULL until
    // EditorApp has pushed one, which the header and footer both handle honestly rather than lying.
    const WatchStatus* watchStatusPtr = nullptr;
    std::optional<bool> watchToggleRequest;  // one-shot, drained by takeWatchToggleRequest()
    // ---- task 3.4.2 ------------------------------------------------------------------------------
    // One-shot, drained by takeCreateMaterialRequest(). Engaged == "create a material in this
    // directory" ("" == the root); disengaged == nothing requested.
    std::optional<std::string> createMaterialRequest;

    // ---- task E.4.3 --------------------------------------------------------------------------------
    // The right-click target, PANEL state rather than a per-row popup id: the tree's rows are
    // PushID(static_cast<int>(i)) (an INDEX, asset_browser_panel.cpp:369), so an item-keyed popup is
    // keyed on an id that changes the moment a directory above it opens -- the popup would close or,
    // worse, RETARGET.
    std::string contextTarget;                   // "" == the pane background, a LEGAL target
    bool contextMenuRequested = false;           // set by the OpenContextMenu arm; consumed by drawIssues
    std::size_t contextMenuItemsDrawnCount = 0;  // reset at the TOP of every onDraw
    std::size_t renameModalDrawn = 0;            // reset at the TOP of every onDraw
    std::size_t assetDeleteModalDrawn = 0;       // reset at the TOP of every onDraw
    std::size_t dropTargetsAcceptedCount = 0;    // reset at the TOP of every onDraw

    std::string pendingRename;  // != "" while the rename modal is up
    std::string renameBuffer;   // the InputText's live text; seeded from leafOf(pendingRename)
    AssetNameRefusal renameRefusal = AssetNameRefusal::None;  // recomputed every frame the modal draws
    bool renameFocusPending = false;                          // SetKeyboardFocusHere on the first frame only
    std::string pendingDelete;                                // != "" while the delete modal is up

    // The four one-shots (§1.4.2). Three optional, one plain string -- see the takers above.
    std::optional<std::string> newFolderRequest;
    std::optional<AssetRenameRequest> renameRequest;
    std::optional<AssetMoveRequest> moveRequest;
    std::string deleteRequest;

    // The injected drag payload the GPU tier's drop cases drive, and the peek verdict it produced.
    // `dropPeekActive` is what distinguishes "no payload" from "a payload naming the assets root".
    bool dropPeekActive = false;
    bool dropPeekIsMovePayload = false;
    std::string dropPeekSource;
    std::string dropPeekDestination;
    AssetOpRefusal dropPeekRefusal = AssetOpRefusal::None;
};

}  // namespace engine::editor

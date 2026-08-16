// Aero Engine — the read-only Asset browser panel (task 2.2.4). THE only new ImGui TU, and it
// includes NO <filesystem>: every disk touch goes through <aero/editor/project_files.hpp> (INV-2).
//
// FOUR ImGui rules this file lives or dies by -- three of them are ASYMMETRIC, and getting one wrong
// is an IM_ASSERT ABORT in the Debug build, not a visual glitch:
//   * BeginChild/EndChild are 1:1 like Begin/End -- EndChild() ALWAYS runs, whatever BeginChild
//     returned (imgui.h:457-463).
//   * BeginTable/EndTable are the OPPOSITE -- EndTable() ONLY when BeginTable() returned true
//     (imgui.h:913). The asymmetry below is deliberate; do not "tidy" it into symmetry.
//   * PushID/PopID are 1:1 -- and there is NO continue, break or return between any pair here
//     (duplicate ImGui ids silently MERGE widgets; 2.2.2 shipped exactly that bug once).
//   * ImGuiTreeNodeFlags_NoTreePushOnOpen means NO TreePop is owed, on either return path
//     (verified in imgui_widgets.cpp's TreeNodeBehavior). That is what makes this tree balance-free.
#include "asset_browser_panel.hpp"

#include <aero/core/guid.hpp>
#include <aero/editor/asset_cache.hpp>  // task 3.1.2: ImportChange, importChangeLabel() -- used directly below
#include <aero/editor/asset_database.hpp>
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/asset_watcher.hpp>  // task 3.1.4 -- WatchStatus, read through the reconciled pointer
#include <aero/editor/panel_context.hpp>
#include <aero/editor/project_files.hpp>

#include "text_input.hpp"  // task 3.1.3 (A1): inputTextString -- NEVER imgui_stdlib (Windows Debug LNK2038)

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// DPI-proportional, never a pixel constant. ImGuiChildFlags_ResizeX persists the user's own width in
// the ini for free (F11), so this is only the first-run default.
constexpr float TREE_PANE_FONT_MULTIPLE = 14.0F;
constexpr float SIZE_COLUMN_FONT_MULTIPLE = 6.0F;
constexpr float INDENT_FONT_MULTIPLE = 0.9F;
// Shown for a file whose size the OS refused (AC-6/E6). NEVER "0 B" -- that is a lie, not a blank.
constexpr const char* UNKNOWN_SIZE = "—";

// C7: every dynamic string goes through this or TextUnformatted -- NEVER as a printf format. A file
// named "%s.txt" passed as the format would read the varargs stack (UB, and the Debug lanes run
// ASan/UBSan).
void textWrappedSafe(const std::string& text) { ImGui::TextWrapped("%s", text.c_str()); }

// task 3.1.1 (§D-7): the footer's GUID segment is elided to keep the status line short -- the first 8
// and last 4 of the 32-char canonical text, joined by a literal ellipsis (U+2026, three UTF-8 bytes),
// matching this file's existing non-ASCII status glyph, UNKNOWN_SIZE.
constexpr std::size_t GUID_PREFIX_LENGTH = 8;
constexpr std::size_t GUID_SUFFIX_LENGTH = 4;
std::string elideGuid(Guid guid) {
    const std::string full = formatGuid(guid);
    return full.substr(0, GUID_PREFIX_LENGTH) + "…" + full.substr(full.size() - GUID_SUFFIX_LENGTH);
}

// task 3.1.3 (A15): the tile caption's own truncation policy. ImGui offers no ellipsis for draw-list
// text, so this measures with ImGui::CalcTextSize and truncates by hand -- the LONGEST byte prefix
// whose (prefix + ellipsis) still fits TILE_CAPTION_LINES lines at `wrapWidth`, landing on a UTF-8
// boundary (never slicing a multi-byte sequence). Needs a live ImGui context (CalcTextSize/
// GetTextLineHeight), so it lives here, not in asset_view.cpp (which stays ImGui-free).
std::string elideForCaption(const std::string& name, float wrapWidth) {
    const float twoLineHeight = static_cast<float>(TILE_CAPTION_LINES) * ImGui::GetTextLineHeight();
    const ImVec2 full = ImGui::CalcTextSize(name.c_str(), nullptr, false, wrapWidth);
    if (full.y <= twoLineHeight) {
        return name;
    }
    std::size_t lo = 0;
    std::size_t hi = name.size();
    while (lo < hi) {
        const std::size_t mid = lo + ((hi - lo + 1) / 2);
        std::size_t cut = mid;
        while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0U) == 0x80U) {
            --cut;  // step back to a UTF-8 boundary
        }
        if (cut == 0) {
            hi = 0;
            break;
        }
        std::string candidate(name, 0, cut);
        candidate += "…";
        const ImVec2 size = ImGui::CalcTextSize(candidate.c_str(), nullptr, false, wrapWidth);
        if (size.y <= twoLineHeight) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    std::size_t finalCut = lo;
    while (finalCut > 0 && (static_cast<unsigned char>(name[finalCut]) & 0xC0U) == 0x80U) {
        --finalCut;
    }
    return name.substr(0, finalCut) + "…";
}

}  // namespace

AssetBrowserPanel::AssetBrowserPanel(std::string rootPath, rhi::Device* device)
    : rootUtf8(std::move(rootPath)), store(device) {}

void AssetBrowserPanel::setRoot(std::string rootPath) {
    rootUtf8 = std::move(rootPath);
    currentDir.clear();
    selectedEntry.clear();
    openDirs.clear();
    cache.clear();
    visibleRows.clear();
    pending = PendingAction{};
    treeDirty = true;
    // task 3.1.1: a rescan request recorded against the OLD root is meaningless against the new one --
    // EditorApp::tick() already rescans unconditionally on a root mismatch.
    rescanRequested = false;
    // task 3.1.2 (A15): the SAME reasoning applies to a Reimport All request against the old root --
    // 3.1.1's own easy-to-miss line, a second instance.
    reimportRequested = false;
    // task 3.1.3 (E27, seed S26): a texture keyed by a GUID from the OLD project is meaningless
    // against the new one -- both are cleared together, exactly like the cache/tree state above.
    ledger.clear();
    store.clear();
    // task 3.1.3, Step 8: a project-wide search against the OLD project's records is meaningless
    // against the new one -- the SAME reasoning, a third time.
    filter = AssetFilter{};
    queryScratch.clear();
    searchRows = SearchResult{};
    // task 3.1.3, Step 11: an orphan path named against the OLD root is meaningless against the new
    // one -- the SAME reasoning, a fourth time. A modal left open across a project swap would confirm
    // a delete against the wrong tree.
    pendingOrphanDelete.clear();
    confirmedOrphanDelete.clear();
}

void AssetBrowserPanel::record(ActionKind kind, std::string path) { pending = PendingAction{kind, std::move(path)}; }

const DirectoryListing* AssetBrowserPanel::cached(const std::string& rel) const {
    const auto it = cache.find(rel);
    return it == cache.end() ? nullptr : &it->second;
}

bool AssetBrowserPanel::ensureCached(const std::string& rel) {
    if (cache.contains(rel)) {
        return false;
    }
    DirectoryListing listing = listDirectory(rootUtf8, rel, showHidden);
    // task 3.1.1 (§D-7): filtered at CACHE-FILL time, not at draw time -- the footer count, the tree
    // builder and the selection lookup then all agree, with no second filtered view to keep in sync.
    // listDirectory itself is unchanged; only this cached copy drops sidecar rows.
    std::erase_if(listing.entries,
                  [](const FileEntry& entry) { return !entry.isDirectory && isMetaFileName(entry.name); });
    cache.emplace(rel, std::move(listing));
    return true;
}

// ---- phase 1: reconcile. THE ONLY PLACE I/O HAPPENS (D7) --------------------------------------
void AssetBrowserPanel::reconcile() {
    // task 3.1.3 (D8): the LRU clock and the per-frame scratch, first -- serviceThumbnails() (called
    // OUTSIDE the draw walk, after renderScene) reads frameCounter and drawTile below repopulates
    // visibleThumbnailKeys from scratch every frame.
    ++frameCounter;
    visibleThumbnailKeys.clear();

    // F15: true on the first frame the panel becomes visible -- INCLUDING the frame its docked tab is
    // selected, or View > Assets re-checks it. That is D9's whole "manual Refresh plus one line"
    // bargain: a filesystem watcher is 3.1.4's deliverable (AC-4).
    if (ImGui::IsWindowAppearing()) {
        cache.clear();
        treeDirty = true;
    }

    bool scanned = ensureCached(std::string{});  // the root -- always needed by the tree
    if (ensureCached(currentDir)) {              // a no-op when currentDir is "" (same key)
        scanned = true;
    }
    for (const std::string& dir : openDirs) {  // inserting into `cache` cannot disturb this set
        if (ensureCached(dir)) {
            scanned = true;
        }
    }
    if (scanned) {
        treeDirty = true;  // a newly scanned directory changes what the tree can show
    }
    if (treeDirty) {
        buildVisibleTree([this](const std::string& rel) { return cached(rel); }, openDirs, visibleRows);
        treeDirty = false;
    }
}

// ---- phase 2: header (Refresh, Show hidden, breadcrumb) ----------------------------------------
void AssetBrowserPanel::drawHeader() {
    if (ImGui::Button("Refresh")) {
        record(ActionKind::Refresh, {});  // D9: manual. A watcher is 3.1.4's deliverable.
    }
    // task 3.1.2 (§D-11): a second button, beside Refresh -- Reimport All is a strict superset (it also
    // discards the committed import cache, AC-39). The panel still performs NO I/O (D7): the rescan
    // itself runs in EditorApp::tick(), outside this draw walk, exactly like Refresh.
    ImGui::SameLine();
    if (ImGui::Button("Reimport All")) {
        record(ActionKind::ReimportAll, {});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Discard the import cache and re-hash every asset. Use this if the editor missed a change.");
    }
    // task 3.4.2 (D9/AC-5): a THIRD button on the same row. The click RECORDS and nothing else -- this
    // panel is read-only by contract (D19) and performs no I/O (D7), so the file is written by
    // EditorApp::tick(), outside the draw walk, exactly like Refresh's rescan. The orphan-delete drain
    // shape, a second application: a create inside the draw walk would write a file, request a scan and
    // mutate the selection while ImGui holds this frame's tree open.
    ImGui::SameLine();
    if (ImGui::Button("New Material")) {
        record(ActionKind::CreateMaterial, {});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Create a new material in this folder and select it.");
    }
    // task 3.1.4 (D10/AC-35): SESSION state, deliberately not persisted -- 3.1.3's D4 posture
    // verbatim (view mode and tile size take the same one, with the same documented "resets on
    // relaunch" limitation). EditorAppConfig is per-launch; project.json is a shared, committed,
    // per-PROJECT file and the wrong home for a per-MACHINE performance escape hatch; and there is no
    // editor preference store to put it in.
    ImGui::SameLine();
    // A LOCAL copy, never a member: INV-5 says only applyPending() writes model state. The TRUTH
    // lives on EditorApp's AssetWatcher; this reads it through the reconciled pointer and, when that
    // pointer is null (no watcher pushed yet), renders a DISABLED, unchecked box rather than lying.
    bool autoUi = watchStatusPtr != nullptr && watchStatusPtr->enabled;
    ImGui::BeginDisabled(watchStatusPtr == nullptr);  // 1:1 with EndDisabled; no continue/return between
    if (ImGui::Checkbox("Auto-refresh", &autoUi)) {
        record(ActionKind::SetAutoRefresh, autoUi ? "1" : "0");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        // A DISABLED item is not hovered without ImGuiHoveredFlags_AllowWhenDisabled, so this tooltip
        // correctly does not appear before a watcher has been reconciled.
        ImGui::SetTooltip(
            "Watch the assets folder and rescan automatically when files change.\n"
            "Turn this off on very large projects or network drives.");
    }
    ImGui::SameLine();
    // A LOCAL copy, never the member: INV-5 says only applyPending() writes showHidden.
    bool hiddenUi = showHidden;
    if (ImGui::Checkbox("Show hidden", &hiddenUi)) {
        record(ActionKind::ToggleHidden, {});
    }

    // task 3.1.3, Step 6: the view toggle and the tile-size combo, on the SAME wrapping row (§D-7).
    // Each records ONE action; applyPending() is the only writer (INV-5 unchanged).
    ImGui::SameLine();
    if (ImGui::RadioButton("Grid", viewMode == AssetViewMode::Grid)) {
        record(ActionKind::SetViewMode, "grid");
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("List", viewMode == AssetViewMode::List)) {
        record(ActionKind::SetViewMode, "list");
    }
    if (viewMode == AssetViewMode::Grid) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0F);
        constexpr std::array<const char*, 3> SIZE_LABELS{"Small", "Medium", "Large"};
        int sizeIndex = static_cast<int>(tileSize);
        if (ImGui::Combo("##tileSize", &sizeIndex, SIZE_LABELS.data(), static_cast<int>(SIZE_LABELS.size()))) {
            switch (static_cast<TileSize>(sizeIndex)) {
                case TileSize::Small:
                    record(ActionKind::SetTileSize, "small");
                    break;
                case TileSize::Medium:
                    record(ActionKind::SetTileSize, "medium");
                    break;
                case TileSize::Large:
                    record(ActionKind::SetTileSize, "large");
                    break;
            }
        }
    }

    // task 3.1.3, Step 8: the search box and the kind filter, on the SAME wrapping row (§D-7).
    // `queryScratch` is a local copy the InputText widget edits directly (A1's inputTextString) --
    // reseeded from the COMMITTED value first, so a query cleared/changed by anything OTHER than
    // typing (ClearSearch, RevealPath) is reflected here too. A SetQuery action is recorded only when
    // the buffer diverges from the committed value, the same "record, never write" discipline every
    // other control on this row already follows (INV-5).
    ImGui::SameLine();
    queryScratch = filter.query;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0F);
    engine::editor::inputTextString("##search", queryScratch, ImGuiInputTextFlags_None);
    if (queryScratch != filter.query) {
        record(ActionKind::SetQuery, queryScratch);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
        record(ActionKind::ClearSearch, {});
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Clear the search");
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0F);
    // task 3.4.2: the list lives in asset_view.hpp beside the enum, NOT here. A copy in this function
    // is invisible to every test tier -- seed S2 dropped Material from the local copy this line used
    // to hold and the whole suite stayed green, because no tier can read a combo's contents. AV53
    // pins the shared constant instead, which only works while this is the sole reader.
    labelScratch = filter.anyKind ? std::string("All") : std::string(assetKindLabel(filter.kind));
    if (ImGui::BeginCombo("##kindFilter", labelScratch.c_str())) {
        if (ImGui::Selectable("All", filter.anyKind)) {
            record(ActionKind::SetKindFilter, "all");
        }
        for (const AssetKind kind : ASSET_KIND_FILTER_OPTIONS) {
            const bool selected = !filter.anyKind && filter.kind == kind;
            labelScratch = std::string(assetKindLabel(kind));
            ImGui::PushID(static_cast<int>(kind));
            if (ImGui::Selectable(labelScratch.c_str(), selected)) {
                record(ActionKind::SetKindFilter, std::to_string(static_cast<int>(kind)));
            }
            ImGui::PopID();  // no continue/break/return between Push and Pop
        }
        ImGui::EndCombo();
    }

    // The breadcrumb: root / a / b, every segment clickable (AC-3).
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    breadcrumb = splitSegments(currentDir);
    labelScratch = rootDisplayName(rootUtf8);
    ImGui::SameLine();
    ImGui::PushID(-1);  // the root crumb; the segment crumbs use 0..n-1, so this cannot collide
    if (ImGui::SmallButton(labelScratch.c_str())) {
        record(ActionKind::Navigate, {});
    }
    ImGui::PopID();
    std::string accumulated;
    for (std::size_t i = 0; i < breadcrumb.size(); ++i) {
        accumulated = joinRelative(accumulated, breadcrumb[i]);
        ImGui::SameLine();
        ImGui::TextUnformatted("/");
        ImGui::SameLine();
        ImGui::PushID(static_cast<int>(i));  // two segments CAN share a name ("a/x/x") -- F26/E9
        // KNOWN AND RECORDED, not an oversight (review gap 4): SmallButton/Button have NO format
        // overload, so a crumb literally named "sprites##old" displays as "sprites" -- Button runs its
        // label through FindRenderedTextEnd(). The tree and the contents table, which are what a user
        // actually reads a name from, ARE fixed; navigation here is unaffected because the id comes
        // from PushID(i), not the label. Closing it needs an InvisibleButton plus hand-placed
        // draw-list text with its own hover/active styling -- disproportionate for a stub, and new
        // ImGui surface with its own bug potential. Recorded in editor/VALIDATION.md instead.
        if (ImGui::SmallButton(breadcrumb[i].c_str())) {
            record(ActionKind::Navigate, accumulated);
        }
        ImGui::PopID();  // no continue/break/return between Push and Pop
    }
    ImGui::Separator();
}

// ---- phase 3: the left pane (the directory tree) ------------------------------------------------
void AssetBrowserPanel::drawTreePane(float paneHeight) {
    const float indentStep = ImGui::GetFontSize() * INDENT_FONT_MULTIPLE;
    const float treeWidth = ImGui::GetFontSize() * TREE_PANE_FONT_MULTIPLE;
    // ImGuiChildFlags_ResizeX enables .ini saving for this child (F11), so the splitter position
    // persists across restarts with NO splitter code and NO state of our own (AC-13).
    const ImVec2 treeSize(treeWidth, paneHeight);
    const ImGuiChildFlags treeFlags = ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX;
    const bool treeVisible = ImGui::BeginChild("##dirs", treeSize, treeFlags);
    // C2 -- LOAD-BEARING, not an optimisation. A collapsed or fully-clipped child sets SkipItems, and
    // TreeNodeBehavior's FIRST statement is `if (window->SkipItems) return false;`. Submitting rows
    // here would make every OPEN directory report "closed" and record a ToggleDir, silently
    // collapsing the user's whole tree. imgui.h:457-459 documents this early-out; here it is required.
    if (treeVisible) {
        // The root row. _Leaf draws no arrow (its children are the rows below); its RETURN VALUE IS
        // DELIBERATELY UNUSED -- _Leaf always returns true and ignores SetNextItemOpen entirely
        // (imgui_widgets.cpp, TreeNodeUpdateNextOpen). We read IsItemClicked() instead.
        labelScratch = rootDisplayName(rootUtf8);
        ImGui::PushID(-1);
        ImGuiTreeNodeFlags rootFlags =
            ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_SpanFullWidth;
        if (currentDir.empty()) {
            rootFlags |= ImGuiTreeNodeFlags_Selected;
        }
        ImGui::TreeNodeEx(labelScratch.c_str(), rootFlags);
        if (ImGui::IsItemClicked()) {
            record(ActionKind::Navigate, {});
        }
        ImGui::PopID();

        for (std::size_t i = 0; i < visibleRows.size(); ++i) {
            const TreeRow& row = visibleRows[i];
            ImGui::PushID(static_cast<int>(i));  // two directories CAN share a leaf name (E9/S11)
            // C3 -- Indent(0.0f) indents by the DEFAULT IndentSpacing, not by zero
            // (imgui.cpp: `(indent_w != 0.0f) ? indent_w : g.Style.IndentSpacing`). Compute the
            // amount ONCE so the Indent/Unindent pair can never desync.
            const float indentAmount = indentStep * static_cast<float>(row.depth);
            const bool indented = indentAmount > 0.0F;
            if (indented) {
                ImGui::Indent(indentAmount);
            }
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_NoTreePushOnOpen |  // F12: NO TreePop owed
                                       ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
            if (row.knownLeaf) {
                flags |= ImGuiTreeNodeFlags_Leaf;
            }
            if (row.path == currentDir) {
                flags |= ImGuiTreeNodeFlags_Selected;
            }
            labelScratch = std::string(leafOf(row.path));
            // D5: our openDirs is AUTHORITATIVE over ImGui's storage. ImGuiCond_Always means the two
            // can never desync -- except for a _Leaf node, which ignores this entirely (see below).
            ImGui::SetNextItemOpen(row.open, ImGuiCond_Always);
            // The (str_id, flags, fmt, ...) OVERLOAD, never (label, flags) -- review gap 4, the same
            // class C7 closed for printf formats. A directory literally named "sprites##old" would
            // otherwise display as "sprites": the single-argument form runs the label through
            // FindRenderedTextEnd(), which stops at the first "##". Verified at upstream
            // v1.92.8-docking: TreeNodeExV(str_id, ...) builds the label with
            // ImFormatStringToTempBufferV and hands TreeNodeBehavior an EXPLICIT label_end, and
            // TreeNodeBehavior only falls back to FindRenderedTextEnd when label_end is null -- so the
            // "##" renders literally. "%s" also keeps it printf-safe, exactly as C7 requires.
            // The id now comes from the constant str_id, which is unique per row via PushID(i) above,
            // and is stable frame to frame; ImGui's own open state is irrelevant anyway (D5).
            const bool nowOpen = ImGui::TreeNodeEx("##dir", flags, "%s", labelScratch.c_str());
            // C1 -- LOAD-BEARING. _Leaf makes TreeNodeEx return TRUE unconditionally and skips the
            // whole toggle block (`if (!is_leaf)` in TreeNodeBehavior), so a leaf's return value
            // carries NO open/closed information. Comparing it would record a spurious ToggleDir on
            // every leaf row every frame -- which would also CLOBBER a genuine click recorded by an
            // earlier row, because `pending` is one last-writer-wins slot.
            if (!row.knownLeaf && nowOpen != row.open) {
                record(ActionKind::ToggleDir, row.path);
            } else if (ImGui::IsItemClicked()) {
                // With _OpenOnArrow, a click on the LABEL never toggles (verified at source), so this
                // branch is unambiguously "make me the current directory".
                record(ActionKind::Navigate, row.path);
            }
            if (indented) {
                ImGui::Unindent(indentAmount);
            }
            ImGui::PopID();  // no continue/break/return anywhere between Push and Pop
        }
    }
    ImGui::EndChild();  // UNCONDITIONAL -- F9, and the opposite of the table rule below
}

// ---- phase 4: the right pane (the current directory's contents) ---------------------------------
// task 3.1.3, Step 6: renamed from drawContentsPane -- BYTE-IDENTICAL otherwise (D3/AC-2). The
// caller now chooses between this and drawContentsGrid below.
void AssetBrowserPanel::drawContentsList(float paneHeight) {
    ImGui::BeginChild("##contents", ImVec2(0.0F, paneHeight), ImGuiChildFlags_Borders);
    if (!filter.query.empty()) {
        // task 3.1.3, Step 8 (AC-14/E15/E16): the SAME "No assets match" / two-column shape as the
        // directory listing below, over searchRows instead. The Name column shows the FULL relative
        // path (there is no single directory context to omit it against); the Size column is always
        // "—" -- a search hit carries no size (AssetRecord tracks identity and content, not bytes).
        if (searchRows.hits.empty()) {
            ImGui::TextUnformatted("No assets match");
        } else {
            const ImVec2 tableSize(0.0F, ImGui::GetContentRegionAvail().y);
            const ImGuiTableFlags tableFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                               ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("##search-entries", 2, tableFlags, tableSize)) {  // F10: End ONLY if true
                const float sizeColumnWidth = ImGui::GetFontSize() * SIZE_COLUMN_FONT_MULTIPLE;
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, sizeColumnWidth);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(searchRows.hits.size()));
                while (clipper.Step()) {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                        const SearchHit& hit = searchRows.hits[static_cast<std::size_t>(i)];
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::PushID(i);
                        const ImGuiSelectableFlags selFlags =
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick;
                        if (ImGui::Selectable("##row", hit.relativePath == selectedEntry, selFlags)) {
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                record(ActionKind::RevealPath, hit.relativePath);
                            } else {
                                record(ActionKind::SelectEntry, hit.relativePath);
                            }
                        }
                        ImGui::SameLine(0.0F, 0.0F);
                        ImGui::TextUnformatted(hit.relativePath.c_str());
                        ImGui::PopID();  // no continue/break/return between Push and Pop
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(UNKNOWN_SIZE);
                    }
                }
                ImGui::EndTable();  // ONLY because BeginTable returned true (F10)
            }
        }
        ImGui::EndChild();  // UNCONDITIONAL -- F9
        return;
    }
    // DELIBERATELY NOT guarded by the return value, unlike ##dirs above (plan §Approaches C): the
    // table path infers no state from a widget's return, so submitting into a clipped child is
    // harmless -- and leaving it live is what keeps BeginTable's asymmetric conditional EndTable a
    // REAL branch rather than dead code. Do not "tidy" this into symmetry with ##dirs.
    const DirectoryListing* const listing = cached(currentDir);
    if (listing == nullptr) {
        ImGui::TextUnformatted("Scanning...");  // one frame at most (D7's latency)
    } else if (rootUtf8.empty()) {
        ImGui::TextUnformatted("No project directory");  // E2
    } else if (listing->status != ScanStatus::Ok) {
        const std::string full = currentDir.empty() ? rootUtf8 : rootUtf8 + "/" + currentDir;
        switch (listing->status) {
            case ScanStatus::Missing:
                textWrappedSafe("Directory not found:\n" + full);  // E1/E3
                break;
            case ScanStatus::NotADirectory:
                textWrappedSafe("Not a directory:\n" + full);
                break;
            case ScanStatus::Unreadable:
                textWrappedSafe("Cannot read this directory (permission denied or I/O error):\n" + full);
                break;
            case ScanStatus::Ok:
                break;  // unreachable; enumerated so a new status cannot be added silently
        }
    } else {
        // F10b: ScrollY REQUIRES an explicit outer_size -- the default ImVec2(0,0) auto-fits the
        // contents, which silently kills BOTH the scrolling and TableSetupScrollFreeze's sticky
        // header, and would make ImGuiListClipper clip against a container that grows without bound.
        const ImVec2 tableSize(0.0F, ImGui::GetContentRegionAvail().y);
        const ImGuiTableFlags tableFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##entries", 2, tableFlags, tableSize)) {  // F10: End ONLY if true
            const float sizeColumnWidth = ImGui::GetFontSize() * SIZE_COLUMN_FONT_MULTIPLE;
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, sizeColumnWidth);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            // The ".." row, OUTSIDE the clipper: it is always visible and always first (AC-3).
            if (!currentDir.empty()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(-1);  // clipper rows use 0..n-1
                if (ImGui::Selectable("..", false, ImGuiSelectableFlags_SpanAllColumns)) {
                    record(ActionKind::Navigate, parentOf(currentDir));
                }
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);  // the size cell stays empty
            }

            // code-review BLOCKING-2 (AC-13): filters by the kind combo ALONE -- `filter.query` is
            // guaranteed empty here (the branch above returns for a non-empty query), so
            // matchesFilter's own substring clause is always a no-op and this call is exactly "apply
            // the kind filter". `filter.anyKind == true` (the default) returns every index, so this
            // costs nothing extra in the common case beyond one small per-frame scratch vector -- the
            // SAME "not persisted, rebuilt every frame" posture `breadcrumb` already has above.
            const std::vector<std::size_t> kindFiltered = filterEntriesByKind(listing->entries, filter);
            ImGuiListClipper clipper;  // F14 -- only the visible rows are submitted (AC-12)
            clipper.Begin(static_cast<int>(kindFiltered.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const FileEntry& entry = listing->entries[kindFiltered[static_cast<std::size_t>(i)]];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);  // a TableNextRow with no column index draws
                                                    // NOTHING, silently (imgui.h:910)
                    ImGui::PushID(i);
                    const std::string rel = joinRelative(currentDir, entry.name);
                    // C4: _AllowDoubleClick adds PressedOnDoubleClick, so the Selectable ALSO fires
                    // on the double-click DOWN frame -- the only frame on which
                    // IsMouseDoubleClicked() can be true (io.MouseClickedCount is reset every frame
                    // and filled only on the DOWN transition). Without this flag the double-click
                    // branch below is UNREACHABLE and AC-3's "double-click enters it" never works.
                    const ImGuiSelectableFlags selFlags =
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick;
                    // An EMPTY label plus a separate TextUnformatted -- review gap 4. Selectable has
                    // no format overload, and it runs its label through FindRenderedTextEnd(), so a
                    // file named "readme##v2.txt" would display as "readme". TextUnformatted goes
                    // through TextEx, which never calls FindRenderedTextEnd and renders "##"
                    // literally (both verified at upstream v1.92.8-docking).
                    // The layout is exact, not approximate: Selectable calls ItemSize() with the
                    // label-derived size BEFORE the SpanAllColumns widening, and CalcTextSize returns
                    // (0, fontSize) for an empty display range -- so the row keeps full height while
                    // CursorPosPrevLine.x stays at the cell origin, and SameLine(0, 0) puts the name
                    // exactly where Selectable would have drawn it.
                    if (ImGui::Selectable("##row", rel == selectedEntry, selFlags)) {
                        if (entry.isDirectory && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            record(ActionKind::Navigate, rel);
                        } else {
                            record(ActionKind::SelectEntry, rel);
                        }
                    }
                    ImGui::SameLine(0.0F, 0.0F);
                    ImGui::TextUnformatted(entry.name.c_str());
                    ImGui::PopID();  // no continue/break/return between Push and Pop
                    ImGui::TableSetColumnIndex(1);
                    if (!entry.isDirectory) {
                        if (entry.sizeKnown) {
                            labelScratch = formatFileSize(entry.size);
                            ImGui::TextUnformatted(labelScratch.c_str());
                        } else {
                            ImGui::TextUnformatted(UNKNOWN_SIZE);  // AC-6 -- never "0 B"
                        }
                    }
                }
            }
            ImGui::EndTable();  // ONLY because BeginTable returned true (F10)
        }
    }
    ImGui::EndChild();  // UNCONDITIONAL -- F9
}

// ---- phase 4 (grid): task 3.1.3, Step 6 -----------------------------------------------------------
void AssetBrowserPanel::drawTile(const FileEntry& entry, const std::string& rel, float tileW, float tileH,
                                 float tileEdge, float pad, bool isSearchHit) {
    const ImVec2 tileDims(tileW, tileH);
    const ImVec2 itemMin = ImGui::GetCursorScreenPos();
    // A single Selectable over the WHOLE tile (2.2.4's ##row idiom, applied to a 2-D item): keeps a
    // file named "readme##v2.png" hit-testable as one item with one id, while the caption below is
    // drawn on the draw list (never through Selectable's own label), so "##" renders literally (E19).
    const ImGuiSelectableFlags selFlags = ImGuiSelectableFlags_AllowDoubleClick;
    if (ImGui::Selectable("##tile", rel == selectedEntry, selFlags, tileDims)) {
        // task 3.1.3 (AC-15): a search hit is ALWAYS a file (searchAssets never matches a folder), so
        // its double-click semantics are RevealPath, not Navigate.
        if (isSearchHit) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                record(ActionKind::RevealPath, rel);
            } else {
                record(ActionKind::SelectEntry, rel);
            }
        } else if (entry.isDirectory && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            record(ActionKind::Navigate, rel);
        } else {
            record(ActionKind::SelectEntry, rel);
        }
    }

    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    const ImVec2 iconMin(itemMin.x + pad, itemMin.y + pad);
    const ImVec2 iconMax(iconMin.x + tileEdge, iconMin.y + tileEdge);
    const float rounding = ImGui::GetStyle().FrameRounding;

    // task 3.1.3, Step 7: three lines, none of which mutate (§D-7) -- the ONLY thumbnail participation
    // in the draw walk. `thumbnailKeyFor` returns nullopt for a folder, an undecodable extension, or
    // any of INV-V3's other six guards; `visibleThumbnailKeys` is per-frame scratch cleared in phase 1,
    // never model state, and `nativeTextureFor` is a const read that answers nullptr until Ready.
    void* texture = nullptr;
    if (const std::optional<ThumbnailKey> key = thumbnailKeyFor(entry, rel); key.has_value()) {
        visibleThumbnailKeys.push_back(*key);
        texture = store.nativeTextureFor(*key);
    }

    if (texture != nullptr) {
        // F7 (A6): the SDL_GPU ImGui backend takes the native texture pointer directly as the id; a
        // thumbnail needs no sampler of its own. viewport_panel.cpp:201-204's exact idiom.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto texId = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture));
        drawList->AddImage(texId, iconMin, iconMax);
    } else {
        // The generated type icon -- what a Skipped/Failed/no-database entry, or an undecodable
        // extension, falls back to FOREVER.
        const AssetKind kind = classifyAssetKind(entry.name, entry.isDirectory);
        const IconColor color = iconColorFor(kind);
        const ImU32 fillColor = IM_COL32(color.r, color.g, color.b, color.a);
        drawList->AddRectFilled(iconMin, iconMax, fillColor, rounding);
        if (entry.isDirectory) {
            // D6: a folder is a two-rect glyph in a fixed colour -- a small "tab" atop the body, both
            // in the SAME icon colour so it reads as one shape rather than two overlapping tiles.
            const float tabWidth = (iconMax.x - iconMin.x) * 0.45F;
            const float tabHeight = (iconMax.y - iconMin.y) * 0.18F;
            const ImU32 tabColor = IM_COL32(color.r, color.g, color.b, 255U);
            drawList->AddRectFilled(iconMin, ImVec2(iconMin.x + tabWidth, iconMin.y + tabHeight), tabColor, rounding);
        } else {
            labelScratch = iconLabelFor(entry.name);
            const ImVec2 textSize = ImGui::CalcTextSize(labelScratch.c_str());
            const ImVec2 textPos((iconMin.x + iconMax.x - textSize.x) * 0.5F,
                                 (iconMin.y + iconMax.y - textSize.y) * 0.5F);
            drawList->AddText(textPos, IM_COL32_WHITE, labelScratch.c_str());
        }
    }

    // The caption: leaf name, wrapped to at most TILE_CAPTION_LINES and ellipsised beyond that (A15).
    // A15's 8-argument AddText overload -- imgui.h:3477 -- is the only one with a wrap width. A search
    // hit folds its containing folder into the SAME caption (the plan's own "subtitled" requirement),
    // so it stays identifiable outside the directory the user is currently browsing (AC-15).
    std::string captionSource = entry.name;
    if (isSearchHit) {
        const std::string parent = parentOf(rel);
        if (!parent.empty()) {
            captionSource = parent + "/" + entry.name;
        }
    }
    const float wrapWidth = tileW - (2.0F * pad);
    const std::string caption = elideForCaption(captionSource, wrapWidth);
    const ImVec2 captionPos(itemMin.x + pad, iconMax.y + pad);
    drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), captionPos, IM_COL32_WHITE, caption.c_str(), nullptr,
                      wrapWidth, nullptr);
}

void AssetBrowserPanel::drawContentsGrid(float paneHeight) {
    ImGui::BeginChild("##contents", ImVec2(0.0F, paneHeight), ImGuiChildFlags_Borders);
    const bool searching = !filter.query.empty();
    const DirectoryListing* const listing = searching ? nullptr : cached(currentDir);
    if (searching) {
        // task 3.1.3, Step 8 (AC-14/E15/E16): a project-wide search routes here instead of the cached
        // DirectoryListing -- the status/`Scanning…`/`No project directory` early-outs below are for
        // the directory path only; a search's own empty state is "No assets match", never a blank pane.
        if (searchRows.hits.empty()) {
            ImGui::TextUnformatted("No assets match");
        } else {
            const float tileEdge = ImGui::GetFontSize() * tileEdgeFontMultiple(tileSize);
            const float pad = ImGui::GetFontSize() * TILE_CAPTION_PAD_FONT_MULTIPLE;
            const float captionH = static_cast<float>(TILE_CAPTION_LINES) * ImGui::GetTextLineHeight();
            const float tileW = tileEdge + (2.0F * pad);
            const float tileH = tileEdge + captionH + (3.0F * pad);
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float rowHeight = tileH + ImGui::GetStyle().ItemSpacing.y;
            const int columns = gridColumnsFor(ImGui::GetContentRegionAvail().x, tileW, spacing);
            const int rows = (static_cast<int>(searchRows.hits.size()) + columns - 1) / columns;
            ImGuiListClipper clipper;
            clipper.Begin(rows, rowHeight);
            while (clipper.Step()) {
                for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                    for (int c = 0; c < columns; ++c) {
                        const int index = (r * columns) + c;
                        if (index >= static_cast<int>(searchRows.hits.size())) {
                            break;
                        }
                        if (c > 0) {
                            ImGui::SameLine();
                        }
                        const SearchHit& hit = searchRows.hits[static_cast<std::size_t>(index)];
                        FileEntry synthetic;  // a search hit is ALWAYS a file (D5) -- no size is tracked
                        synthetic.name = std::string(leafOf(hit.relativePath));
                        ImGui::PushID(index);
                        drawTile(synthetic, hit.relativePath, tileW, tileH, tileEdge, pad, /*isSearchHit=*/true);
                        ImGui::PopID();
                    }
                }
            }
        }
        ImGui::EndChild();  // UNCONDITIONAL -- F9
        return;
    }
    if (listing == nullptr) {
        ImGui::TextUnformatted("Scanning...");
    } else if (rootUtf8.empty()) {
        ImGui::TextUnformatted("No project directory");
    } else if (listing->status != ScanStatus::Ok) {
        const std::string full = currentDir.empty() ? rootUtf8 : rootUtf8 + "/" + currentDir;
        switch (listing->status) {
            case ScanStatus::Missing:
                textWrappedSafe("Directory not found:\n" + full);
                break;
            case ScanStatus::NotADirectory:
                textWrappedSafe("Not a directory:\n" + full);
                break;
            case ScanStatus::Unreadable:
                textWrappedSafe("Cannot read this directory (permission denied or I/O error):\n" + full);
                break;
            case ScanStatus::Ok:
                break;  // unreachable; enumerated so a new status cannot be added silently
        }
    } else {
        // A16 -- the exact geometry, computed once per call, all DPI-proportional.
        const float tileEdge = ImGui::GetFontSize() * tileEdgeFontMultiple(tileSize);
        const float pad = ImGui::GetFontSize() * TILE_CAPTION_PAD_FONT_MULTIPLE;
        const float captionH = static_cast<float>(TILE_CAPTION_LINES) * ImGui::GetTextLineHeight();
        const float tileW = tileEdge + (2.0F * pad);
        const float tileH = tileEdge + captionH + (3.0F * pad);
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float rowHeight = tileH + ImGui::GetStyle().ItemSpacing.y;  // A16 -- EXACT, not a guess
        const int columns = gridColumnsFor(ImGui::GetContentRegionAvail().x, tileW, spacing);

        // The parent-directory affordance, when not at the root -- outside the clipper, always visible,
        // always first. It is a FULL-WIDTH BAR ONE TEXT LINE TALL, not a tile: as a tile it was a bare
        // Selectable sized tileW x tileH with ImGui drawing the label at its top-left corner, so it
        // rendered as a large empty rectangle (a solid accent-coloured block once hovered) with the
        // rest of its row blank -- visibly unlike every real tile beside it, which draws an icon rect
        // and a centred caption. Reported from the 3.1.3 human pass.
        //
        // "<" and ".." are DELIBERATELY ASCII. This editor still loads no font of its own (the Unicode
        // font is unowned, five tasks on from 2.2.4), so an arrow glyph like U+2190 is far outside
        // ImGui's default range and would render as a missing-glyph box -- a worse regression than the
        // block it replaces.
        //
        // Keeping it off the grid flow is UNCHANGED and still load-bearing: sharing a row with the
        // clipper-driven grid below would make the clipper's row math account for one leading cell.
        // A full-width bar is off that flow just as the old full-row tile was, so `rowHeight` and
        // `rows` below are untouched by this change.
        if (!currentDir.empty()) {
            ImGui::PushID(-1);
            const ImVec2 barSize(ImGui::GetContentRegionAvail().x, 0.0F);  // y == 0 -> one text line
            if (ImGui::Selectable("<  ..", false, ImGuiSelectableFlags_None, barSize)) {
                record(ActionKind::Navigate, parentOf(currentDir));
            }
            ImGui::PopID();
        }

        // code-review BLOCKING-2 (AC-13): the drawContentsList precedent immediately above, applied
        // here too -- `filter.query` is guaranteed empty in this branch, so filterEntriesByKind is
        // exactly "apply the kind filter alone", and costs nothing extra when filter.anyKind (default).
        const std::vector<std::size_t> kindFiltered = filterEntriesByKind(listing->entries, filter);
        const std::vector<FileEntry>& items = listing->entries;
        const int rows = (static_cast<int>(kindFiltered.size()) + columns - 1) / columns;
        ImGuiListClipper clipper;  // clips ROWS OF TILES, not tiles
        clipper.Begin(rows, rowHeight);
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                for (int c = 0; c < columns; ++c) {
                    const int index = (r * columns) + c;
                    if (index >= static_cast<int>(kindFiltered.size())) {
                        break;  // BEFORE any PushID -- there is NEVER a break between a Push/Pop pair
                    }
                    if (c > 0) {
                        ImGui::SameLine();  // restores the PREVIOUS line's Y (why one row == rowHeight)
                    }
                    const FileEntry& entry = items[kindFiltered[static_cast<std::size_t>(index)]];
                    const std::string rel = joinRelative(currentDir, entry.name);
                    ImGui::PushID(index);
                    drawTile(entry, rel, tileW, tileH, tileEdge, pad, /*isSearchHit=*/false);
                    ImGui::PopID();  // no continue/break/return inside drawTile
                }
            }
        }
    }
    ImGui::EndChild();  // UNCONDITIONAL -- F9
}

// ---- phase 4b: issues (task 3.1.3, Step 9, D11) -------------------------------------------------
// "The report IS the issues list" -- no new computation, no second source of truth. Shown ONLY when
// the total is non-zero (no ride-along empty header on a clean project).
void AssetBrowserPanel::drawIssues() {
    // task 3.1.3, Step 11: the delete-confirmation modal. Opened by applyPending() setting
    // pendingOrphanDelete (never from inside this draw walk); 2.5.1's shell_ui.cpp:247-302 shape
    // verbatim, retargeted at this action.
    constexpr const char* DELETE_MODAL_ID = "Delete orphaned .meta?";
    if (!pendingOrphanDelete.empty()) {
        if (!ImGui::IsPopupOpen(DELETE_MODAL_ID)) {
            ImGui::OpenPopup(DELETE_MODAL_ID);
        }
        // F13: EndPopup ONLY when BeginPopupModal returned true -- the BeginMenu family, not the Begin one.
        if (ImGui::BeginPopupModal(DELETE_MODAL_ID, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            // C7/E20: the exact project-relative path through TextWrapped's "%s" form -- NEVER as a
            // bare format string. A path containing '%' would otherwise be a format bug.
            ImGui::TextWrapped("%s", pendingOrphanDelete.c_str());
            ImGui::TextDisabled("The asset it described no longer exists; the file itself is not touched.");
            ImGui::Separator();
            if (ImGui::Button("Delete")) {
                confirmedOrphanDelete = pendingOrphanDelete;  // nothing touches disk here (D9)
                pendingOrphanDelete.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();  // Enter == Delete
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                pendingOrphanDelete.clear();
                ImGui::CloseCurrentPopup();
            }
            // ImGui CANNOT dismiss a MODAL with Escape: NavUpdateCancelRequest's popup branch excludes
            // ImGuiWindowFlags_Modal (imgui.cpp:15032) and BeginPopupModal always sets it
            // (imgui.cpp:13232) -- and the editor does not enable ImGuiConfigFlags_NavEnableKeyboard at
            // all (imgui_layer.cpp:79), so that path is doubly dead. Esc is the universal DISMISS key
            // (.claude/rules/editor.md), so we bind it OURSELVES, HERE, inside the body -- repeat=false,
            // one press = one Cancel. Deliberately NOT the global-route chord mechanism used elsewhere:
            // the editor-chord rule exists so a focused InputText can win a chord back, but a modal
            // already blocks every other window, and a global Escape route would also fire on the
            // frames the modal is NOT up.
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                pendingOrphanDelete.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        } else {
            // A SAFETY NET, not the Esc mechanism above: in 1.92.8 the only thing that can reach here
            // is a PROGRAMMATIC close, because a modal also swallows outside clicks. Treating it as
            // Cancel keeps the flow from wedging with pendingOrphanDelete stuck non-empty.
            pendingOrphanDelete.clear();
        }
    }

    if (reportPtr == nullptr) {
        return;
    }
    const AssetScanReport& report = *reportPtr;
    // Code-review finding 2 (3.1.1): report.invalid counts EVERY Invalid-state record, INCLUDING one
    // a write conflict downgraded -- report.invalidPaths already excludes those, so the count shown
    // here must subtract writeConflictTotal too, or the two would silently disagree (logAssetScan's
    // own identical subtraction, editor_app.cpp).
    const std::size_t invalidOnly = report.invalid - report.writeConflictTotal;
    // code-review SHOULD-FIX 10: `importFailureTotal` (task 3.2.1's phase 7.5) was missing from this
    // sum entirely, so a model-only import failure never even opened the header -- total stayed 0 and
    // this function returned before ImGui::CollapsingHeader was ever called.
    const std::size_t total = report.orphanTotal + invalidOnly + report.aliasedDirTotal + report.writeFailureTotal +
                              report.writeConflictTotal + report.hashFailureTotal + report.importFailureTotal;
    if (total == 0) {
        return;
    }

    labelScratch = "Issues (" + std::to_string(total) + ")";
    // code-review finding 10: `issuesOpen` is OUR OWN state, authoritative over ImGui's -- the SAME
    // D5 reasoning drawTreePane already applies (`ImGui::SetNextItemOpen(row.open, ImGuiCond_Always)`).
    // It is load-bearing here for a reason the tree pane does not have: this header's label EMBEDS
    // `total`, so its ImGui id changes every time the issue count does (a Refresh, an orphan delete)
    // -- WITHOUT this, ImGui's own per-id open/closed persistence would silently reset to closed on
    // every such change, because a new id has no memory of the old one's state.
    ImGui::SetNextItemOpen(issuesOpen, ImGuiCond_Always);
    issuesOpen = ImGui::CollapsingHeader(labelScratch.c_str());
    if (issuesOpen) {
        if (report.orphanTotal > 0) {
            ImGui::TextUnformatted("Orphaned .meta files:");
            for (std::size_t i = 0; i < report.orphans.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::TextUnformatted(report.orphans[i].c_str());  // E20 -- never a format string
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete .meta")) {
                    record(ActionKind::RequestDeleteOrphan, report.orphans[i]);
                }
                ImGui::PopID();  // no continue/break/return between Push and Pop
            }
            if (report.orphanTotal > report.orphans.size()) {
                labelScratch = "…and " + std::to_string(report.orphanTotal - report.orphans.size()) + " more";
                ImGui::TextUnformatted(labelScratch.c_str());
            }
        }

        // Spec A9: an invalid .meta still holds a real GUID one `git checkout --theirs` away -- text
        // only, deliberately NO action offered.
        const auto drawCategory = [this](const char* heading, const std::vector<std::string>& entries,
                                         std::size_t totalCount) {
            if (totalCount == 0) {
                return;
            }
            ImGui::TextUnformatted(heading);
            for (const std::string& line : entries) {
                ImGui::TextUnformatted(line.c_str());  // E20 -- never a format string
            }
            if (totalCount > entries.size()) {
                labelScratch = "…and " + std::to_string(totalCount - entries.size()) + " more";
                ImGui::TextUnformatted(labelScratch.c_str());
            }
        };
        drawCategory("Invalid .meta files (no action offered):", report.invalidPaths, invalidOnly);
        drawCategory("Aliased directories:", report.aliasedDirs, report.aliasedDirTotal);
        drawCategory("Sidecar write failures:", report.writeFailures, report.writeFailureTotal);
        drawCategory("Write conflicts with an orphaned .meta:", report.writeConflicts, report.writeConflictTotal);
        drawCategory("Asset hash failures:", report.hashFailures, report.hashFailureTotal);
        // code-review SHOULD-FIX 10: the sixth category, following the identical capped-list +
        // uncapped-total idiom as the five above -- previously never drawn at all.
        drawCategory("Model import failures:", report.importFailures, report.importFailureTotal);
    }
}

// ---- phase 5: footer ---------------------------------------------------------------------------
void AssetBrowserPanel::drawFooter() {
    const DirectoryListing* const listing = cached(currentDir);
    if (listing == nullptr) {
        return;
    }
    // The COUNTS ARE BUILT FIRST AND ALWAYS (review gap 3 / AC-8). They used to live behind an early
    // return that a selection pre-empted, so clicking any file in a >10 000-entry directory made the
    // truncation notice VANISH -- which is precisely the "a silent cap reads as 'this is everything'"
    // failure D12 exists to prevent. A selection now APPENDS to this line; it never replaces it.
    labelScratch.clear();
    if (listing->status == ScanStatus::Ok) {
        labelScratch = std::to_string(listing->entries.size()) + " items";
        if (listing->skipped > 0) {
            labelScratch += "  -  " + std::to_string(listing->skipped) + " skipped";  // AC-8/E5
        }
        if (listing->truncated) {
            // AC-8/D12/E8 -- surfaced, never silent. BOTH caps are named because either can fire
            // (review gap 2), and "showing the first 10000" would be a LIE when the scan cap is what
            // stopped us: a directory of dotfiles browsed with `Show hidden` off can be truncated
            // with far fewer than 10 000 entries listed.
            labelScratch += "  -  truncated (caps: " + std::to_string(MAX_ENTRIES_PER_DIRECTORY) + " listed, " +
                            std::to_string(MAX_ENTRIES_EXAMINED) + " scanned)";
        }
    }
    // A non-Ok status contributes NOTHING here: the right pane already explains it, and duplicating
    // the message in the footer was never wanted.
    if (!selectedEntry.empty()) {
        // code-review BLOCKING-3: matched by the FULL relative path, never by leaf name alone -- a
        // leaf-only lookup either misses every search hit (selectedEntry is a path outside currentDir)
        // or, on a same-leaf-name collision, silently pairs one file's identity with a DIFFERENT
        // file's size. `rec` is looked up unconditionally (when a database exists) so a search hit
        // whose parent directory was never navigated to can still resolve its identity even though it
        // is not a member of THIS listing.
        const std::size_t idx = findEntryByRelativePath(listing->entries, currentDir, selectedEntry);
        const bool inCurrentListing = idx < listing->entries.size();
        const AssetRecord* const rec = databasePtr != nullptr ? databasePtr->findByPath(selectedEntry) : nullptr;
        // E10, preserved: a selection that is NEITHER a member of the current listing NOR known to the
        // database has simply vanished (deleted outside the editor, then rescanned) -- nothing renders,
        // exactly as before. AC-15 is what ADDS the `rec != nullptr` half: a search hit's record still
        // exists in the database even though its directory was never navigated to.
        if (inCurrentListing || rec != nullptr) {
            if (!labelScratch.empty()) {
                labelScratch += "   |   ";
            }
            labelScratch += selectedEntry;
            // D5: a record NEVER describes a directory (AssetDatabase records only files), so
            // `rec != nullptr` alone already implies a file; `selectedIsDirectory` only matters for the
            // inCurrentListing branch.
            const bool selectedIsDirectory = inCurrentListing && listing->entries[idx].isDirectory;
            if (!selectedIsDirectory) {
                if (inCurrentListing) {
                    const FileEntry& entry = listing->entries[idx];
                    labelScratch +=
                        entry.sizeKnown ? ("  -  " + formatFileSize(entry.size)) : std::string("  -  ") + UNKNOWN_SIZE;
                } else {
                    // code-review BLOCKING-3: NEVER another file's size -- the exact bug this fixes. A
                    // search hit's size is simply unknown from here; AssetRecord tracks identity and
                    // content, never bytes (Step 8's own rule for the search table).
                    labelScratch += std::string("  -  ") + UNKNOWN_SIZE;
                }
                // task 3.1.1 (§D-7): the selected file's identity, appended after the size segment.
                // databasePtr is reconciled by EditorApp::tick() (D12/D13) -- nullptr before the first
                // scan, never a dangling reference.
                if (databasePtr != nullptr) {
                    labelScratch += "  -  ";
                    if (rec == nullptr) {
                        labelScratch += "no .meta";
                    } else if (rec->state == AssetMetaState::Invalid) {
                        labelScratch += "invalid .meta";
                    } else {
                        labelScratch += elideGuid(rec->guid);
                    }
                    // task 3.1.2 (§D-11): the import state, appended after the GUID segment -- STILL
                    // inside this SAME `databasePtr != nullptr` block (a null database appends neither
                    // segment, exactly as before). An invalid record has no identity, so it has no
                    // import state either -- the footer already says "invalid .meta" for it above.
                    // Code-review finding 3 (task 3.1.2): a write-failed record (state still
                    // Created/Repaired/Reattached) never got a `change` assigned either -- its default
                    // UpToDate would otherwise render "up to date" for a file with no sidecar on disk,
                    // so the segment is omitted for it exactly as it already is for Invalid.
                    if (rec != nullptr && rec->state != AssetMetaState::Invalid && !rec->metaWriteFailed) {
                        labelScratch += "  -  ";
                        labelScratch += importChangeLabel(rec->change);
                    }
                }
            }
        }
    }
    // task 3.1.3, Step 8 (AC-14/AC-28): APPENDED, never replacing -- the search summary, shown only
    // while a query is committed. `total` is the UNCAPPED match count (seed S15); `truncated` names
    // the cap explicitly rather than letting "showing the first 2000" read as "this is everything".
    if (!filter.query.empty()) {
        if (!labelScratch.empty()) {
            labelScratch += "   |   ";
        }
        labelScratch += std::to_string(searchRows.total) + " matches";
        if (searchRows.truncated) {
            labelScratch += "  -  truncated (cap: " + std::to_string(MAX_SEARCH_RESULTS) + ")";
        }
    }
    // task 3.1.3 (AC-28): APPENDED, never replacing -- the thumbnail summary, shown only when
    // non-zero. `readyCount`/`unavailableCount` forward straight to the ledger (thumbnailReadyCount()/
    // thumbnailUnavailableCount() are the SAME two calls, exposed for the GPU tier).
    const std::size_t readyThumbnails = ledger.readyCount();
    const std::size_t unavailableThumbnails = ledger.unavailableCount();
    if (readyThumbnails > 0 || unavailableThumbnails > 0) {
        if (!labelScratch.empty()) {
            labelScratch += "   |   ";
        }
        labelScratch += std::to_string(readyThumbnails) + " thumbnails";
        if (unavailableThumbnails > 0) {
            labelScratch += ", " + std::to_string(unavailableThumbnails) + " unavailable";
        }
    }

    // task 3.1.4 (AC-36): APPENDED, never replacing -- the watcher's condition, in a fixed precedence
    // order so the most ACTIONABLE condition wins. Omitted entirely when no watcher has been
    // reconciled yet, which is honest: the panel does not know.
    // ASCII ONLY. This editor loads no font of its own -- its one font is ImGui's ProggyClean (Basic
    // + Extended Latin) -- so a non-ASCII glyph renders as a missing-glyph box. That is 3.1.3's own
    // post-merge fix, applied here as a rule rather than rediscovered.
    if (watchStatusPtr != nullptr) {
        if (!labelScratch.empty()) {
            labelScratch += "   |   ";
        }
        if (!watchStatusPtr->enabled) {
            labelScratch += "Auto-refresh off";
        } else if (watchStatusPtr->rootUnreadable) {
            labelScratch += "Watch paused -- assets folder unreadable";
        } else if (watchStatusPtr->truncated) {
            labelScratch += "Watching (partial -- tree exceeds the scan limit)";
        } else if (watchStatusPtr->deferredSweeps > 0) {
            labelScratch += "Watching -- settling (" + std::to_string(watchStatusPtr->deferredSweeps) + ")";
        } else if (watchStatusPtr->unreadableDirs > 0) {
            labelScratch += "Watching (" + std::to_string(watchStatusPtr->unreadableDirs) + " folder(s) unreadable)";
        } else {
            labelScratch += "Watching";
        }
    }

    if (labelScratch.empty()) {
        return;  // an unusable directory with nothing selected -- the right pane carries the message
    }
    ImGui::TextUnformatted(labelScratch.c_str());
}

// ---- apply: the ONE mutating switch (D8/INV-5) --------------------------------------------------
void AssetBrowserPanel::openAncestors(const std::string& path) {
    // C9: STRICT ancestors only. Expanding `path` itself stays the arrow's job, so a label click and
    // an arrow click remain distinguishable. Iterative -- misc-no-recursion (F23).
    std::string cursor = parentOf(path);
    while (!cursor.empty()) {
        openDirs.insert(cursor);
        cursor = parentOf(cursor);
    }
}

// task 3.1.3, Step 8: called from applyPending() ONLY -- searchAssets is a real O(records) walk, and
// running it from the draw walk (once per frame, regardless of whether the query changed) would be
// the exact "the panel performs I/O in the draw walk" mistake this task's own gates grep for. An
// empty query or a null database both clear the result, never leaving a stale one behind.
void AssetBrowserPanel::refreshSearchRows() {
    if (filter.query.empty() || databasePtr == nullptr) {
        searchRows = SearchResult{};
        return;
    }
    searchRows = searchAssets(databasePtr->records(), filter);
}

void AssetBrowserPanel::applyPending() {
    const PendingAction action = std::move(pending);
    pending = PendingAction{};
    switch (action.kind) {
        case ActionKind::None:
            break;
        case ActionKind::Navigate:
            currentDir = action.path;
            selectedEntry.clear();
            openAncestors(currentDir);
            treeDirty = true;
            break;
        case ActionKind::ToggleDir:
            if (const auto it = openDirs.find(action.path); it != openDirs.end()) {
                openDirs.erase(it);
            } else {
                openDirs.insert(action.path);
            }
            treeDirty = true;
            break;
        case ActionKind::SelectEntry:
            selectedEntry = action.path;
            break;
        case ActionKind::Refresh:
            cache.clear();           // 3.1.4's watcher seam: `cache.clear(); treeDirty = true;` IS the whole
            treeDirty = true;        // invalidation
            rescanRequested = true;  // task 3.1.1 (AC-38): drained by EditorApp::tick()'s reconcile
            break;
        case ActionKind::ToggleHidden:
            showHidden = !showHidden;
            cache.clear();     // listings were filtered at SCAN time (D10), so the flag and the cache
            treeDirty = true;  // are ONE unit -- E11
            break;
        case ActionKind::ReimportAll:
            // task 3.1.2 (A15): Reimport All is a strict superset of Refresh -- clear THIS panel's own
            // listing cache (or it would render stale rows after the rescan) AND request the deeper,
            // cache-discarding rescan via `reimportRequested`, NOT `rescanRequested`: EditorApp derives
            // an ordinary refresh from a reimport request, never the other way around.
            cache.clear();
            treeDirty = true;
            reimportRequested = true;
            // task 3.1.3 (E26): every asset re-hashes, so every existing ContentHash -- and therefore
            // every existing ThumbnailKey -- is about to become stale. code-review BLOCKING-1: this
            // used to be an IMMEDIATE `ledger.clear(); store.clear();` right here -- INSIDE the draw
            // walk, AFTER drawContentsGrid/drawTile (above, this SAME onDraw() call) had already
            // written every ready thumbnail's native texture pointer into THIS frame's ImGui draw
            // list. Destroying it here freed that pointer before EditorApp::tick()'s endFrame() ever
            // consumed it -- synchronous on Vulkan/D3D12, only deferred (and therefore silent) on
            // Metal. Set a flag instead; serviceThumbnails() (which runs OUTSIDE the draw walk, D8's
            // own rule) drains it safely.
            pendingThumbnailReimportClear = true;
            break;
        case ActionKind::SetViewMode:
            // task 3.1.3, Step 6 -- the path carries "grid" or "list" (§D-7's PendingAction shape).
            viewMode = action.path == "grid" ? AssetViewMode::Grid : AssetViewMode::List;
            break;
        case ActionKind::SetTileSize:
            if (action.path == "small") {
                tileSize = TileSize::Small;
            } else if (action.path == "large") {
                tileSize = TileSize::Large;
            } else {
                tileSize = TileSize::Medium;
            }
            break;
        case ActionKind::SetQuery:
            filter.query = action.path;
            refreshSearchRows();
            break;
        case ActionKind::ClearSearch:
            filter.query.clear();
            queryScratch.clear();
            searchRows = SearchResult{};
            break;
        case ActionKind::SetKindFilter:
            if (action.path == "all") {
                filter.anyKind = true;
            } else if (!action.path.empty()) {
                // A single digit, 0-6 -- static_cast<int>(AssetKind) (§D-7's PendingAction shape).
                // No std::stoi: the no-exceptions rule (docs/04) extends to this control-flow path too.
                //
                // THE CEILING, written down at task 3.4.2 rather than discovered at kind eleven: this
                // encoding BREAKS SILENTLY at a TENTH enumerator. std::to_string(10) is "10" and the
                // line below reads path[0] only, so kind 10 would decode as kind 1 -- a wrong filter,
                // no error, no red test. A tenth AssetKind must widen both halves together.
                filter.anyKind = false;
                filter.kind = static_cast<AssetKind>(action.path[0] - '0');
            }
            refreshSearchRows();
            break;
        case ActionKind::RevealPath:
            // task 3.1.3 (AC-15): a double-clicked search hit navigates to its containing folder,
            // selects it, and expands the tree to reveal it -- in ONE arm, because `pending` is a
            // single last-writer-wins slot and this needs Navigate's effect AND SelectEntry's.
            currentDir = parentOf(action.path);
            selectedEntry = action.path;
            openAncestors(action.path);
            treeDirty = true;
            filter.query.clear();
            queryScratch.clear();
            searchRows = SearchResult{};
            break;
        case ActionKind::RequestDeleteOrphan:
            // task 3.1.3, Step 9: sets pendingOrphanDelete and NOTHING else -- the modal that opens on
            // it, confirms it, and turns it into a real delete is Step 11 (§D-9).
            pendingOrphanDelete = action.path;
            break;
        case ActionKind::SetAutoRefresh:
            // task 3.1.4 (D10): RECORDS a request and NOTHING else -- no direct call into the
            // watcher, which this panel cannot reach and must not (2.6.1's AC-46 rule: a control
            // records a request; consumption happens outside the draw walk, in EditorApp's
            // reconcile). The one mutation path INV-5 names is unchanged.
            watchToggleRequest = action.path == "1";
            break;
        case ActionKind::CreateMaterial:
            // task 3.4.2 (D9/AC-5): RECORDS the target directory and NOTHING else -- no listing, no
            // write, no selection change. `currentDir` is read HERE rather than at the record() call
            // site because applyPending() is the one place that sees committed model state: a Navigate
            // recorded in the same frame is resolved by this same switch, and the last writer wins.
            createMaterialRequest = currentDir;
            break;
    }
}

// ---- task 3.1.3: thumbnails, the two-phase wiring (D8) -----------------------------------------

// INV-V3: nullopt unless ALL SEVEN conditions hold, in ONE function so no call site can forget one.
std::optional<ThumbnailKey> AssetBrowserPanel::thumbnailKeyFor(const FileEntry& entry, const std::string& rel) const {
    if (entry.isDirectory) {  // 1: a folder is never a thumbnail candidate
        return std::nullopt;
    }
    if (!isThumbnailDecodable(entry.name)) {  // 2: .ktx2/.dds are Texture but not decodable (D7)
        return std::nullopt;
    }
    if (databasePtr == nullptr) {  // 3: no scan has ever completed
        return std::nullopt;
    }
    const AssetRecord* const record = databasePtr->findByPath(rel);
    if (record == nullptr) {  // 4: no identity for this file
        return std::nullopt;
    }
    if (record->state == AssetMetaState::Invalid) {  // 5: no identity this session (D7's posture)
        return std::nullopt;
    }
    if (record->metaWriteFailed) {  // 6: the sidecar never landed on disk (code-review finding 3)
        return std::nullopt;
    }
    // 7: 3.1.2's A4 trap made operational -- an all-zero contentHash is the EMPTY FILE's real digest,
    // not a sentinel, so the only "was this hashed?" test is the `change` enum.
    if (record->change == ImportChange::Unhashable || record->change == ImportChange::NotHashed) {
        return std::nullopt;
    }
    return ThumbnailKey{.guid = record->guid, .hash = record->contentHash};
}

std::string AssetBrowserPanel::absolutePathFor(const ThumbnailKey& key) const {
    if (databasePtr == nullptr) {
        return {};
    }
    const AssetRecord* const record = databasePtr->findByGuid(key.guid);
    if (record == nullptr) {
        return {};  // the record vanished (a rescan raced the decode) -- treated as Failed, never retried
    }
    return rootUtf8 + "/" + record->relativePath;
}

void AssetBrowserPanel::serviceThumbnails() {
    if (!store.available()) {  // E13/AC-11: no device -- thumbnails stay unavailable forever
        visibleThumbnailKeys.clear();
        return;
    }
    for (const ThumbnailKey& key : visibleThumbnailKeys) {
        ledger.touch(key, frameCounter);
    }
    visibleThumbnailKeys.clear();

    // code-review BLOCKING-1: the ReimportAll flag, drained HERE -- after the touch loop above, so
    // anything drawn (and therefore touched) THIS SAME frame is already marked at `frameCounter` and
    // is excluded by evictions()'s own "never evict a key touched at currentFrame" rule (E12). A key
    // still visible next tick is touched again and survives again, forever, at zero cost beyond
    // holding a possibly-stale (but never dangling) texture one tick longer; a key whose content
    // actually changed gets a brand-new ThumbnailKey once EditorApp's own rescan (reimportRequested,
    // set above) completes and is decoded fresh regardless. `evictions(0, frameCounter)` reads as
    // "every Ready key beyond a cap of zero" -- i.e. every Ready key that eviction's own protection
    // does not shield.
    if (pendingThumbnailReimportClear) {
        pendingThumbnailReimportClear = false;
        for (const ThumbnailKey& key : ledger.evictions(0, frameCounter)) {
            store.destroy(key);
            ledger.forget(key);
        }
    }

    // task 3.1.4 (D9/AC-31): drained HERE -- after the touch loop above, so every key drawn this
    // frame is already marked at `frameCounter` and is excluded by supersededBy's own currentFrame
    // rule, the same protection E12 gives ordinary eviction. NEVER from onDraw(): 3.1.3's BLOCKING-1,
    // where SDL_ReleaseGPUTexture frees SYNCHRONOUSLY on Vulkan (SDL_gpu_vulkan.c:7070-7073) and
    // D3D12 (SDL_gpu_d3d12.c:1460-1463) and only DEFERS on Metal (SDL_gpu_metal.m:936-944).
    //
    // What this exists for: ThumbnailKey is {Guid, ContentHash}, so an edited texture already gets a
    // FRESH key and re-decodes for free. What needs code is the OLD key -- its GPU texture is now
    // unreachable forever, and under LRU alone it survives until 256 residents push it out.
    // Iterating in Photoshop therefore strands one dead 128x128 RGBA8 texture per save.
    if (pendingSupersededSweep) {
        pendingSupersededSweep = false;
        if (databasePtr != nullptr) {
            liveKeyScratch.clear();
            abstainingScratch.clear();
            for (const AssetRecord& assetRecord : databasePtr->records()) {
                // metaWriteFailed FIRST -- a failed sidecar write leaves `change` at its default
                // UpToDate for a file whose bytes on disk are not what was hashed (3.1.2's own
                // code-review finding 3). An all-zero contentHash is the EMPTY FILE's real digest,
                // not a sentinel, so the ONLY "was this hashed?" test is the `change` enum (3.1.2 A4).
                const bool hashUsable = !assetRecord.metaWriteFailed && assetRecord.guid.valid() &&
                                        assetRecord.change != ImportChange::NotHashed &&
                                        assetRecord.change != ImportChange::Unhashable;
                if (hashUsable) {
                    liveKeyScratch.push_back(ThumbnailKey{.guid = assetRecord.guid, .hash = assetRecord.contentHash});
                } else if (assetRecord.guid.valid()) {
                    abstainingScratch.push_back(assetRecord.guid);  // AC-32: NO OPINION about its keys
                }
            }
            std::sort(liveKeyScratch.begin(), liveKeyScratch.end());  // supersededBy's precondition
            std::sort(abstainingScratch.begin(), abstainingScratch.end());
            for (const ThumbnailKey& key : ledger.supersededBy(liveKeyScratch, abstainingScratch, frameCounter)) {
                store.destroy(key);
                ledger.forget(key);
            }
        }
    }

    // EVICTION RUNS BEFORE DECODING, DELIBERATELY (INV-V5, seed S3): the other order lets the
    // resident count exceed the cap by up to MAX_THUMBNAIL_DECODES_PER_TICK for a tick, which makes
    // the bound this task states in a FOOTER a lie.
    for (const ThumbnailKey& key : ledger.evictions(MAX_THUMBNAILS_RESIDENT, frameCounter)) {
        store.destroy(key);
        ledger.forget(key);
    }
    for (const ThumbnailKey& key : ledger.nextDecodes(MAX_THUMBNAIL_DECODES_PER_TICK)) {
        const std::string absolute = absolutePathFor(key);
        const ThumbnailState state = absolute.empty() ? ThumbnailState::Failed : store.load(key, absolute);
        switch (state) {  // NO default: -- a new state is a -Wswitch warning, not a silent fallthrough
            case ThumbnailState::Ready:
                ledger.markReady(key);
                break;
            case ThumbnailState::Failed:
                ledger.markFailed(key);
                break;
            case ThumbnailState::Skipped:
                ledger.markSkipped(key);
                break;
            case ThumbnailState::Absent:
                ledger.markFailed(key);  // load() never returns it; defensive
                break;
        }
    }
}

// code-review BLOCKING-1: identical in effect to a real click on the Reimport All button --
// record(ActionKind::ReimportAll, {}) is exactly what drawHeader() calls when the button returns
// true. EditorApp forwards to this from a new public hook (requestAssetBrowserReimportAll()) because
// the ImGui-free-at-source GPU tier has no other way to press a widget.
void AssetBrowserPanel::requestReimportAll() noexcept { record(ActionKind::ReimportAll, {}); }

// task 3.1.4: 2.2.4's watcher seam, made callable. EXACTLY the two statements ActionKind::Refresh's
// arm performs, and deliberately NOT the third (`rescanRequested = true`): the caller is EditorApp,
// which has just FINISHED a rescan -- asking for another one would be a loop.
void AssetBrowserPanel::invalidateListings() {
    cache.clear();
    treeDirty = true;
}

// code-review finding 4: each records EXACTLY what the corresponding widget records -- the radio
// (drawHeader's "grid"/"list"), the search box (SetQuery, or ClearSearch for an empty string, which
// is what the Clear button does), the kind combo, and an orphan row's Delete button. Nothing here
// bypasses applyPending(); the seam ends at record(), so every one of these still travels the one
// mutation path INV-5 names.
void AssetBrowserPanel::requestViewMode(AssetViewMode mode) noexcept {
    record(ActionKind::SetViewMode, mode == AssetViewMode::Grid ? "grid" : "list");
}
void AssetBrowserPanel::requestSearchQuery(std::string query) {
    if (query.empty()) {
        record(ActionKind::ClearSearch, {});
        return;
    }
    record(ActionKind::SetQuery, std::move(query));
}
void AssetBrowserPanel::requestKindFilter(std::string kind) { record(ActionKind::SetKindFilter, std::move(kind)); }
void AssetBrowserPanel::requestDeleteOrphanClick(std::string relativeMetaPath) {
    record(ActionKind::RequestDeleteOrphan, std::move(relativeMetaPath));
}
// DEVIATION (task 3.2.1): the requestDeleteOrphanClick() shape verbatim -- record(ActionKind::
// SelectEntry, ...) is exactly what a real single click on a row/tile records (asset_browser_panel.cpp's
// own drawContentsList/drawContentsGrid/drawTile call sites).
void AssetBrowserPanel::requestSelectEntry(std::string relativePath) {
    record(ActionKind::SelectEntry, std::move(relativePath));
}
// task 3.4.2: the requestReimportAll() shape verbatim -- record(ActionKind::CreateMaterial, {}) is
// exactly what drawHeader() calls when the New Material button returns true, so the request travels
// the SAME applyPending() arm and picks up the SAME currentDir a click would.
void AssetBrowserPanel::requestCreateMaterial() noexcept { record(ActionKind::CreateMaterial, {}); }

// ---- the frame ---------------------------------------------------------------------------------
void AssetBrowserPanel::onDraw(PanelContext& /*context*/) {  // D18: the context is IGNORED
    reconcile();                                             // 1 -- the only I/O
    drawHeader();                                            // 2
    // Reserve one line for the footer. std::max keeps a very short panel from passing a NEGATIVE
    // height to BeginChild, which ImGui reads as "bottom-align at N from the edge", not as zero.
    const float footerHeight = ImGui::GetFrameHeightWithSpacing();
    const float paneHeight = std::max(ImGui::GetContentRegionAvail().y - footerHeight, 1.0F);
    drawTreePane(paneHeight);  // 3
    ImGui::SameLine();
    if (viewMode == AssetViewMode::Grid) {  // task 3.1.3, Step 6 -- one child, two bodies (§D-7)
        drawContentsGrid(paneHeight);
    } else {
        drawContentsList(paneHeight);
    }
    drawIssues();    // 4b -- task 3.1.3, Step 9
    drawFooter();    // 5
    applyPending();  // the ONLY place anything mutates
}

}  // namespace engine::editor

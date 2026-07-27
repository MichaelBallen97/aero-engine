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

#include <aero/editor/panel_context.hpp>
#include <aero/editor/project_files.hpp>

#include <algorithm>
#include <cstddef>
#include <imgui.h>
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

}  // namespace

AssetBrowserPanel::AssetBrowserPanel(std::string rootPath) : rootUtf8(std::move(rootPath)) {}

void AssetBrowserPanel::setRoot(std::string rootPath) {
    rootUtf8 = std::move(rootPath);
    currentDir.clear();
    selectedEntry.clear();
    openDirs.clear();
    cache.clear();
    visibleRows.clear();
    pending = PendingAction{};
    treeDirty = true;
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
    cache.emplace(rel, listDirectory(rootUtf8, rel, showHidden));
    return true;
}

// ---- phase 1: reconcile. THE ONLY PLACE I/O HAPPENS (D7) --------------------------------------
void AssetBrowserPanel::reconcile() {
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
    ImGui::SameLine();
    // A LOCAL copy, never the member: INV-5 says only applyPending() writes showHidden.
    bool hiddenUi = showHidden;
    if (ImGui::Checkbox("Show hidden", &hiddenUi)) {
        record(ActionKind::ToggleHidden, {});
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
            const bool nowOpen = ImGui::TreeNodeEx(labelScratch.c_str(), flags);
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
void AssetBrowserPanel::drawContentsPane(float paneHeight) {
    ImGui::BeginChild("##contents", ImVec2(0.0F, paneHeight), ImGuiChildFlags_Borders);
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

            ImGuiListClipper clipper;  // F14 -- only the visible rows are submitted (AC-12)
            clipper.Begin(static_cast<int>(listing->entries.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const FileEntry& entry = listing->entries[static_cast<std::size_t>(i)];
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
                    if (ImGui::Selectable(entry.name.c_str(), rel == selectedEntry, selFlags)) {
                        if (entry.isDirectory && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            record(ActionKind::Navigate, rel);
                        } else {
                            record(ActionKind::SelectEntry, rel);
                        }
                    }
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

// ---- phase 5: footer ---------------------------------------------------------------------------
void AssetBrowserPanel::drawFooter() {
    const DirectoryListing* const listing = cached(currentDir);
    if (listing == nullptr) {
        return;
    }
    if (!selectedEntry.empty()) {
        // E10: looked up in the CURRENT listing every frame, so a selection whose file disappeared
        // simply stops rendering. No invalidation pass, no dangling state.
        const std::string_view leaf = leafOf(selectedEntry);
        const auto it = std::find_if(listing->entries.begin(), listing->entries.end(),
                                     [leaf](const FileEntry& e) { return e.name == leaf; });
        if (it != listing->entries.end()) {
            labelScratch = selectedEntry;
            if (!it->isDirectory) {
                labelScratch +=
                    it->sizeKnown ? ("  -  " + formatFileSize(it->size)) : std::string("  -  ") + UNKNOWN_SIZE;
            }
            ImGui::TextUnformatted(labelScratch.c_str());
            return;
        }
    }
    if (listing->status != ScanStatus::Ok) {
        return;  // the right pane already explains it -- no duplicate message
    }
    labelScratch = std::to_string(listing->entries.size()) + " items";
    if (listing->skipped > 0) {
        labelScratch += "  -  " + std::to_string(listing->skipped) + " skipped";  // AC-8/E5
    }
    if (listing->truncated) {
        // AC-8/D12/E8 -- surfaced, never silent.
        labelScratch += "  -  showing the first " + std::to_string(MAX_ENTRIES_PER_DIRECTORY) + " (truncated)";
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
            cache.clear();     // 3.1.4's watcher seam: `cache.clear(); treeDirty = true;` IS the whole
            treeDirty = true;  // invalidation
            break;
        case ActionKind::ToggleHidden:
            showHidden = !showHidden;
            cache.clear();     // listings were filtered at SCAN time (D10), so the flag and the cache
            treeDirty = true;  // are ONE unit -- E11
            break;
    }
}

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
    drawContentsPane(paneHeight);  // 4
    drawFooter();                  // 5
    applyPending();                // the ONLY place anything mutates
}

}  // namespace engine::editor

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
#include <aero/editor/asset_drag.hpp>  // task 3.1.5: the payload, its type string and the draggable rule
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/asset_watcher.hpp>  // task 3.1.4 -- WatchStatus, read through the reconciled pointer
#include <aero/editor/editor_glyphs.hpp>
#include <aero/editor/editor_icons.hpp>   // task E.6.3: AERO_ICON_ELLIPSIS, the header's overflow
#include <aero/editor/material_card.hpp>  // task E.4.5 -- the card's subtitle and tint rules, and the separator
#include <aero/editor/panel_context.hpp>
#include <aero/editor/project_files.hpp>
#include <aero/editor/status_bar.hpp>  // task E.6.2: classifyWatchState + assetFooterWatchText

#include "asset_tile.hpp"         // task E.3.3 -- the tile FACE, shared with the picker
#include "editor_theme_ui.hpp"    // task E.6.3: currentUiScale
#include "panel_chrome_ui.hpp"    // task E.6.3: roles, faces, the ghost and segment buttons, the clear scopes
#include "text_input.hpp"         // task 3.1.3 (A1): inputTextString -- NEVER imgui_stdlib (Windows Debug LNK2038)
#include "thumbnail_service.hpp"  // task E.3.3 -- the SHARED ledger/store, borrowed through thumbnailsPtr

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>  // task 3.1.5: std::memset over the payload's tail padding
#include <imgui.h>
#include <imgui_internal.h>  // task E.6.3: GetTopMostPopupModal, the navigate request's modal gate (I337)
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// task E.6.3 (D1): the tree pane's and the List size column's first-run widths moved to asset_browser_chrome.hpp
// (ASSET_TREE_PANE_FONT_MULTIPLE, ASSET_SIZE_COLUMN_FONT_MULTIPLE); the tree's indent is the theme's IndentSpacing.
// The header's three field widths are named there too (ASSET_HEADER_*_FONT_MULTIPLE); this TU defines none.
constexpr const char* HEADER_OVERFLOW_POPUP = "##assetsOverflow";
constexpr std::string_view CRUMB_SEPARATOR = "/";
// Shown for a file whose size the OS refused (AC-6/E6). NEVER "0 B" -- that is a lie, not a blank.
constexpr const char* UNKNOWN_SIZE = AERO_GLYPH_EM_DASH;

// task E.6.3 (the code-review round, round 2): the pure Assets layout sees no ImGui, so it spells ImGui's shortest
// child as a literal 4 -- asset_browser_chrome.cpp's CONTENTS_MIN_HEIGHT and asset_view.cpp's CHILD_MIN_HEIGHT --
// and these make a port bump or a theme edit that moves the real minimum a compile error, never a pane silently
// shorter than its group. CalcWindowMinSize (imgui.cpp:7007-7029) holds a child without ImGuiChildFlags_ResizeY to at
// least IMGUI_WINDOW_HARD_MIN_SIZE (:7017) ...
static_assert(IMGUI_WINDOW_HARD_MIN_SIZE == 4.0F,
              "CONTENTS_MIN_HEIGHT (asset_browser_chrome.cpp) and CHILD_MIN_HEIGHT (asset_view.cpp) mirror it");
// ... and to at least max(0, style.WindowRounding - 1) (:7027, the WINDOW rounding even for a child), so 4 holds only
// while that term stays within it at the largest UI scale (WindowRounding is the theme's, x the scale, truncated).
constexpr float LARGEST_ROUNDING_TERM = std::max(0.0F, (EDITOR_THEME.metrics.windowRounding * UI_SCALE_MAX) - 1.0F);
static_assert(LARGEST_ROUNDING_TERM <= IMGUI_WINDOW_HARD_MIN_SIZE,
              "the theme's windowRounding would raise ImGui's shortest child above the Assets layout's 4");

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
    const std::string suffix = full.substr(full.size() - GUID_SUFFIX_LENGTH);
    return full.substr(0, GUID_PREFIX_LENGTH) + AERO_GLYPH_ELLIPSIS + suffix;
}

// task E.4.5: a list row's DIMMED document name, on the same line as the file name -- ONE body for both list
// arms, so the push/pop discipline lives in one place. Rule 4 runs against `leaf`, the file's own name, even
// where the row shows a full path. Composes exactly materialCardRowText's sentence (file name, then
// MATERIAL_CARD_SEPARATOR and the name), drawn in two colours.
//
// mutedText (a TextUnformatted under a pushed role), NEVER ImGui::TextDisabled: that is a variadic FORMAT function, and
// a document name is arbitrary user-authored JSON text, so a name containing "%s" would read an absent vararg (D12;
// seed S9). TextUnformatted takes no format string at all, so the class is unreachable rather than merely absent.
// PushStyleColor/PopStyleColor are 1:1 with no return between them: an unbalanced style stack is an IM_ASSERT abort in
// the Debug build (seed S30).
void drawMaterialRowSuffix(ThumbnailService* thumbnails, const AssetDatabase* database, const std::string& rel,
                           std::string_view leaf, std::string& scratch) {
    if (thumbnails == nullptr || database == nullptr) {
        return;
    }
    const AssetRecord* const record = database->findByPath(rel);  // nullptr for a folder
    if (record == nullptr) {
        return;
    }
    const std::optional<ThumbnailKey> key = thumbnailKeyForRecord(*record);
    if (!key.has_value()) {
        return;
    }
    thumbnails->noteCardWanted(*key);  // its OWN queue: a list row notes no thumbnail and never has
    const std::string_view subtitle = materialCardSubtitle(thumbnails->cardFor(*key), leaf);
    if (subtitle.empty()) {
        return;  // a nameless material, and every non-material: the row stays byte-identical to today's
    }
    scratch = MATERIAL_CARD_SEPARATOR;
    scratch += subtitle;
    ImGui::SameLine(0.0F, 0.0F);
    mutedText(scratch);  // task E.6.3 (D11): TextUnformatted under the textMuted role, still no format string
}

}  // namespace

AssetBrowserPanel::AssetBrowserPanel(std::string rootPath) : rootUtf8(std::move(rootPath)) {}

void AssetBrowserPanel::setRoot(std::string rootPath) {
    rootUtf8 = std::move(rootPath);
    currentDir.clear();
    selectedEntry.clear();
    openDirs.clear();
    cache.clear();
    visibleRows.clear();
    navigateRequest.reset();  // task E.6.3: a queued navigation belongs to the old root
    pending = PendingAction{};
    treeDirty = true;
    // task 3.1.1: a rescan request recorded against the OLD root is meaningless against the new one --
    // EditorApp::tick() already rescans unconditionally on a root mismatch.
    rescanRequested = false;
    // task 3.1.2 (A15): the SAME reasoning applies to a Reimport All request against the old root --
    // 3.1.1's own easy-to-miss line, a second instance.
    reimportRequested = false;
    // task 3.1.3 (E27, seed S26): a texture keyed by a GUID from the OLD project is meaningless
    // against the new one. task E.3.3 moved those two lines into ThumbnailService::clear(), which
    // EditorApp calls at the SAME point -- immediately after this setRoot() -- so I40's "resident 0
    // after a project swap" keeps its exact timing. The service is shared, so the panel is no longer
    // the right place to decide that.
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

void AssetBrowserPanel::record(ActionKind kind, std::string path) {
    // task E.4.3: DELEGATES, so no existing call site changes.
    record(kind, std::move(path), std::string{});
}

void AssetBrowserPanel::record(ActionKind kind, std::string path, std::string destination) {
    pending = PendingAction{kind, std::move(path), std::move(destination)};
}

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
    // listDirectory itself is unchanged; only this cached copy drops rows.
    //
    // task E.4.4: the predicate is isBrowserVisibleName, which composes the ONE roster in asset_meta.hpp.
    // Before this task it was isMetaFileName alone, so a .blend1, a leftover .aero-tmp, a Thumbs.db and a
    // desktop.ini each kept a tile the SCAN had already refused -- the one consumer that did not follow
    // isScannableAssetName. It follows by COMPOSITION now, never by a second list, and stays at cache-fill
    // time for §D-7's reason. `Show hidden` still reaches listDirectory above and nothing else.
    std::erase_if(listing.entries,
                  [](const FileEntry& entry) { return !isBrowserVisibleName(entry.name, entry.isDirectory); });
    cache.emplace(rel, std::move(listing));
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

// ---- phase 2: the header row -- RECORDED only (task E.6.3, D8) --------------------------------------------------
// UNFILLED, on `panel` (ImGui's tab bar already paints the chrome band), over a 1-dp divider rule. New Material at the
// left; the cluster right-aligned by assetHeaderLayout, which folds Refresh and Auto-refresh (Compact) and the kind
// filter (Minimal) into the overflow; Reimport All, Show hidden and the tile size live there always. Every control
// records the ActionKind it recorded before; applyPending() is still the only writer (INV-5).
void AssetBrowserPanel::drawHeader() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = currentUiScale();
    const float frame = ImGui::GetFrameHeight();
    const float padY = EDITOR_THEME.panels.panelHeaderPaddingY * s;
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    const float rowY = rowMin.y + padY;
    // MEASURED at the live font, exactly as each widget sizes itself: a button is its label (to the "##") plus two
    // frame paddings (ImGui::ButtonEx), a checkbox its square, ItemInnerSpacing.x and its label (ImGui::Checkbox).
    const auto buttonWidth = [&style](const char* label) {
        return ImGui::CalcTextSize(label, nullptr, true).x + (2.0F * style.FramePadding.x);
    };
    AssetHeaderWidths widths;
    widths.newMaterial = buttonWidth("New Material");
    widths.search = ImGui::GetFontSize() * ASSET_HEADER_SEARCH_FONT_MULTIPLE;
    widths.searchMin = ImGui::GetFontSize() * ASSET_HEADER_SEARCH_MIN_FONT_MULTIPLE;
    widths.kind = ImGui::GetFontSize() * ASSET_HEADER_KIND_FONT_MULTIPLE;
    widths.viewToggle = buttonWidth("Grid") + buttonWidth("List");
    widths.refresh = buttonWidth("Refresh");
    widths.autoRefresh = frame + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Auto-refresh").x;
    widths.overflow = frame;
    const AssetHeaderLayout layout = assetHeaderLayout(rowMin.x, rowWidth, widths, s);
    chrome.headerMode = layout.mode;
    chrome.headerLayout = layout;
    chrome.headerRowMinX = rowMin.x;
    chrome.headerRowWidth = rowWidth;
    chrome.headerWidths = widths;
    chrome.headerUiScale = s;

    // New Material, at the row's left. DISABLED WITH NO PROJECT (plan D-6, the code-review round's finding 5): the
    // drain refuses an empty root with one WARN, and the disabled state is what says "not yet" before the click.
    ImGui::SetCursorScreenPos(ImVec2(layout.newMaterialX, rowY));
    const bool canCreate = !rootUtf8.empty();
    ImGui::BeginDisabled(!canCreate);
    if (ImGui::Button("New Material")) {
        record(ActionKind::CreateMaterial, {});
    }
    ImGui::EndDisabled();
    noteHeaderRect(AssetHeaderControl::NewMaterial);
    // AllowWhenDisabled, or the tooltip that explains the disabled state is the one nobody can see. The enabled text
    // NAMES THE TARGET DIRECTORY -- a NAMED LOCAL passed as a "%s" argument, never as the format string itself.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (canCreate) {
            labelScratch = "Create a new material in '";
            labelScratch += currentDir.empty() ? rootDisplayName(rootUtf8) : currentDir;
            labelScratch += "' and select it.";
        } else {
            labelScratch = "Open a project first -- there is no assets folder to create a material in.";
        }
        ImGui::SetTooltip("%s", labelScratch.c_str());
    }

    // The search, with ImGui's own placeholder (the hint overload) -- reseeded from the COMMITTED value first, so a
    // query cleared by anything other than typing is reflected; a SetQuery is recorded only when the buffer diverges.
    // Escape clears it (EscapeClearsAll), which replaces the old "x" button (D8 lists no clear control).
    ImGui::SetCursorScreenPos(ImVec2(layout.searchX, rowY));
    queryScratch = filter.query;
    ImGui::SetNextItemWidth(layout.searchWidth);
    inputTextString("##search", "Search assets", queryScratch, ImGuiInputTextFlags_EscapeClearsAll);
    if (queryScratch != filter.query) {
        record(ActionKind::SetQuery, queryScratch);
    }
    noteHeaderRect(AssetHeaderControl::Search);

    // The kind filter, inline unless Minimal folded it. ASSET_KIND_FILTER_OPTIONS is the sole list (AV53).
    if (layout.kindInline) {
        ImGui::SetCursorScreenPos(ImVec2(layout.kindX, rowY));
        ImGui::SetNextItemWidth(widths.kind);
        labelScratch = filter.anyKind ? std::string("All") : std::string(assetKindLabel(filter.kind));
        if (ImGui::BeginCombo("##kindFilter", labelScratch.c_str())) {  // EndCombo ONLY when this returned true
            if (ImGui::Selectable("All", filter.anyKind)) {
                record(ActionKind::SetKindFilter, "all");
            }
            for (const AssetKind kind : ASSET_KIND_FILTER_OPTIONS) {
                labelScratch = std::string(assetKindLabel(kind));
                ImGui::PushID(static_cast<int>(kind));
                if (ImGui::Selectable(labelScratch.c_str(), !filter.anyKind && filter.kind == kind)) {
                    record(ActionKind::SetKindFilter, std::to_string(static_cast<int>(kind)));
                }
                ImGui::PopID();  // no continue/break/return between Push and Pop
            }
            ImGui::EndCombo();
        }
        noteHeaderRect(AssetHeaderControl::Kind);  // EndCombo hands LastItemData back to the combo (imgui.cpp:8848)
    } else {
        chrome.overflowDrawn.push_back(AssetHeaderControl::Kind);
    }

    // Grid | List: the hoisted segmented group (task E.6.2's, panel_chrome_ui), its `raised` body drawn first.
    ImDrawList* const list = ImGui::GetWindowDrawList();
    segmentGroupBackground(list, ImVec2(layout.viewToggleX, rowY),
                           ImVec2(layout.viewToggleX + widths.viewToggle, rowY + frame));
    ImGui::SetCursorScreenPos(ImVec2(layout.viewToggleX, rowY));
    const bool grid = viewMode == AssetViewMode::Grid;
    if (segmentButton("Grid", grid, ChromeRole::Active, ChromeRole::TextBright, ChromeRole::TextSecondary,
                      segmentScratch)) {
        record(ActionKind::SetViewMode, "grid");
    }
    const Vec2 viewMin = segmentScratch.min;
    ImGui::SameLine(0.0F, 0.0F);
    if (segmentButton("List", !grid, ChromeRole::Active, ChromeRole::TextBright, ChromeRole::TextSecondary,
                      segmentScratch)) {
        record(ActionKind::SetViewMode, "list");
    }
    AssetDrawnRect& viewRect = chrome.headerRects[static_cast<std::size_t>(AssetHeaderControl::ViewToggle)];
    viewRect = AssetDrawnRect{true, viewMin, segmentScratch.max};

    if (layout.refreshInline) {
        ImGui::SetCursorScreenPos(ImVec2(layout.refreshX, rowY));
        if (ghostButton("Refresh")) {
            record(ActionKind::Refresh, {});  // D9: manual; the watcher is 3.1.4's
        }
        noteHeaderRect(AssetHeaderControl::Refresh);
    } else {
        chrome.overflowDrawn.push_back(AssetHeaderControl::Refresh);
    }

    // Auto-refresh: a LOCAL copy, never a member (INV-5). The truth lives on EditorApp's AssetWatcher; a null pointer
    // (no watcher pushed yet) renders a DISABLED, unchecked box rather than lying.
    bool autoUi = watchStatusPtr != nullptr && watchStatusPtr->enabled;
    if (layout.autoRefreshInline) {
        ImGui::SetCursorScreenPos(ImVec2(layout.autoRefreshX, rowY));
        ImGui::BeginDisabled(watchStatusPtr == nullptr);  // 1:1 with EndDisabled; no continue/return between
        if (ImGui::Checkbox("Auto-refresh", &autoUi)) {
            record(ActionKind::SetAutoRefresh, autoUi ? "1" : "0");
        }
        ImGui::EndDisabled();
        noteHeaderRect(AssetHeaderControl::AutoRefresh);
        if (ImGui::IsItemHovered()) {  // a DISABLED item is not hovered without AllowWhenDisabled -- correct here
            ImGui::SetTooltip(
                "Watch the assets folder and rescan automatically when files change.\n"
                "Turn this off on very large projects or network drives.");
        }
    } else {
        chrome.overflowDrawn.push_back(AssetHeaderControl::AutoRefresh);
    }

    // The overflow: the folded controls first, in header order, then the three that always live here.
    ImGui::SetCursorScreenPos(ImVec2(layout.overflowX, rowY));
    if (ghostButton(AERO_ICON_ELLIPSIS "##assetsMore", ImVec2(widths.overflow, frame))) {
        ImGui::OpenPopup(HEADER_OVERFLOW_POPUP);
    }
    noteHeaderRect(AssetHeaderControl::Overflow);
    if (ImGui::BeginPopup(HEADER_OVERFLOW_POPUP)) {  // EndPopup ONLY when this returned true
        if (!layout.kindInline && ImGui::BeginMenu("Kind")) {
            if (ImGui::MenuItem("All", nullptr, filter.anyKind)) {
                record(ActionKind::SetKindFilter, "all");
            }
            for (const AssetKind kind : ASSET_KIND_FILTER_OPTIONS) {
                labelScratch = std::string(assetKindLabel(kind));
                ImGui::PushID(static_cast<int>(kind));
                if (ImGui::MenuItem(labelScratch.c_str(), nullptr, !filter.anyKind && filter.kind == kind)) {
                    record(ActionKind::SetKindFilter, std::to_string(static_cast<int>(kind)));
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        if (!layout.refreshInline && ImGui::MenuItem("Refresh")) {
            record(ActionKind::Refresh, {});
        }
        if (!layout.autoRefreshInline && ImGui::MenuItem("Auto-refresh", nullptr, autoUi, watchStatusPtr != nullptr)) {
            record(ActionKind::SetAutoRefresh, autoUi ? "0" : "1");  // the item shows the state; a click flips it
        }
        if (!chrome.overflowDrawn.empty()) {
            ImGui::Separator();
        }
        // task 3.1.2 (§D-11): a strict superset of Refresh -- it also discards the committed import cache (AC-39).
        if (ImGui::MenuItem("Reimport All")) {
            record(ActionKind::ReimportAll, {});
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Discard the import cache and re-hash every asset. Use this if the editor missed a change.");
        }
        if (ImGui::MenuItem("Show hidden", nullptr, showHidden)) {
            record(ActionKind::ToggleHidden, {});
        }
        if (viewMode == AssetViewMode::Grid) {  // the tile size, as the old combo: Grid only
            ImGui::Separator();
            if (ImGui::MenuItem("Small tiles", nullptr, tileSize == TileSize::Small)) {
                record(ActionKind::SetTileSize, "small");
            }
            if (ImGui::MenuItem("Medium tiles", nullptr, tileSize == TileSize::Medium)) {
                record(ActionKind::SetTileSize, "medium");
            }
            if (ImGui::MenuItem("Large tiles", nullptr, tileSize == TileSize::Large)) {
                record(ActionKind::SetTileSize, "large");
            }
        }
        ImGui::EndPopup();
    }

    // The divider rule under the row, across the full content width; the panes start on the row below it.
    const float ruleY = rowMin.y + frame + (2.0F * padY);
    const int ruleVertices = list->VtxBuffer.Size;  // what the list RECEIVES, never a flag set beside the call
    horizontalRule(list, rowMin.x, rowMin.x + rowWidth, ruleY, ChromeRole::Divider);
    chrome.headerRuleDrawn = list->VtxBuffer.Size > ruleVertices;       // AddRectFilled adds none at alpha 0
    ImGui::SetCursorScreenPos(ImVec2(rowMin.x, ruleY + ruleHeight()));  // the tree pane is the item that follows
}

void AssetBrowserPanel::noteHeaderRect(AssetHeaderControl control) {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    AssetDrawnRect& rect = chrome.headerRects[static_cast<std::size_t>(control)];
    rect = AssetDrawnRect{true, Vec2{min.x, min.y}, Vec2{max.x, max.y}};
}

// ---- phase 3: the left pane (the directory tree) -- task E.6.3 (D8): drawn the Hierarchy way -------------------
void AssetBrowserPanel::drawTreePane(float paneHeight) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float s = currentUiScale();
    const float indentStep = style.IndentSpacing;  // D1: the theme's indent, never a font multiple
    const float treeWidth = ImGui::GetFontSize() * ASSET_TREE_PANE_FONT_MULTIPLE;
    // ImGuiChildFlags_ResizeX enables .ini saving for this child (F11), so the splitter position persists across
    // restarts with NO splitter code and NO state of our own (AC-13). D8: NO border -- a divider rule on its right.
    const ImVec2 treeSize(treeWidth, paneHeight);
    const bool treeVisible = ImGui::BeginChild("##dirs", treeSize, ImGuiChildFlags_ResizeX);
    // C2 -- LOAD-BEARING, not an optimisation. A collapsed or fully-clipped child sets SkipItems, and
    // TreeNodeBehavior's FIRST statement is `if (window->SkipItems) return false;`. Submitting rows here would make
    // every OPEN directory report "closed" and record a ToggleDir, silently collapsing the user's whole tree.
    if (treeVisible) {
        ImDrawList* const list = ImGui::GetWindowDrawList();
        list->ChannelsSplit(2);  // 0: the rounded fills, 1: ImGui's arrows and the hand-drawn text. Merged below.
        list->ChannelsSetCurrent(1);
        const float rounding = EDITOR_THEME.metrics.frameRounding * s;
        const std::span<const AssetRecord> records =
            databasePtr != nullptr ? databasePtr->records() : std::span<const AssetRecord>{};
        const std::uint64_t generation = databasePtr != nullptr ? databasePtr->generation() : 0U;
        // One row, drawn after ImGui submitted its node: the fill (channel 0), then the label and the count.
        const auto drawRowChrome = [&](ImVec2 rowCursor, ImVec2 rowMin, ImVec2 rowMax, bool hovered, bool current,
                                       std::string_view label, std::string_view folder) {
            if (current || hovered) {
                list->ChannelsSetCurrent(0);
                roleFill(list, rowMin, rowMax, current ? ChromeRole::Selection : ChromeRole::Hover, rounding);
                list->ChannelsSetCurrent(1);
            }
            // The label at the x ImGui would have drawn it: the arrow's width plus two frame paddings
            // (imgui_widgets.cpp:6894, text_offset_x for an unframed node).
            const float labelX = rowCursor.x + ImGui::GetFontSize() + (2.0F * style.FramePadding.x);
            countScratch.clear();
            if (databasePtr != nullptr) {
                countScratch = groupThousands(folderCounts.countUnder(records, generation, folder));
            }
            const AssetTreeRowLayout row =
                assetTreeRowLayout(labelX, rowMax.x, faceTextWidth(PanelFace::MonoSmall, countScratch), s);
            const float budget = row.labelMaxX - labelX;
            labelScratch = elideCaptionRight(
                label, [budget](std::string_view t) { return faceTextWidth(PanelFace::Body, t) <= budget; });
            // Packed ONCE and handed to the draw, so the record below is the colour the label RECEIVED (I324).
            const ImU32 labelColor = roleU32(current ? ChromeRole::Accent : ChromeRole::Text);
            const ImVec2 labelAt(labelX, rowMin.y);
            list->AddText(panelFaceFont(PanelFace::Body), panelFacePoints(PanelFace::Body), labelAt, labelColor,
                          labelScratch.c_str());
            if (row.countShown) {
                const float countY = rowMin.y + ((ImGui::GetFontSize() - panelFacePoints(PanelFace::MonoSmall)) * 0.5F);
                const ImVec2 countAt(row.countX, countY);
                drawRoleText(list, PanelFace::MonoSmall, countAt, ChromeRole::TextFaint, countScratch);
            }
            if (current) {
                chrome.treeCurrentLabelColor = labelColor;
                chrome.treeSelectionRounding = rounding;
            }
            ++chrome.treeRowsDrawn;
        };

        // The root row. _Leaf draws no arrow (its children are the rows below); its RETURN VALUE IS DELIBERATELY
        // UNUSED -- _Leaf always returns true and ignores SetNextItemOpen entirely (TreeNodeUpdateNextOpen). We read
        // IsItemClicked() instead.
        ImGui::PushID(-1);
        ImGuiTreeNodeFlags rootFlags =
            ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_SpanFullWidth;
        if (currentDir.empty()) {
            rootFlags |= ImGuiTreeNodeFlags_Selected;  // kept for its nav semantics; ImGui's fill never shows
        }
        const ImVec2 rootCursor = ImGui::GetCursorScreenPos();
        {
            const ClearHeaderStyle clearRowFill(ChromeRole::Panel);
            ImGui::PushStyleColor(ImGuiCol_Text, roleVec4(ChromeRole::TextFaint));  // ImGui draws only the arrow
            chrome.treeArrowTextColor = ImGui::GetColorU32(ImGuiCol_Text);          // the live slot the arrow reads
            ImGui::TreeNodeEx("##dir", rootFlags, "%s", "");
            ImGui::PopStyleColor();
        }
        const ImVec2 rootMin = ImGui::GetItemRectMin();
        const ImVec2 rootMax = ImGui::GetItemRectMax();
        const bool rootHovered = ImGui::IsItemHovered();
        // task E.4.3 (code-review G1) -- site 6: the root TREE row, the same destination reached the other way.
        // Attached before IsItemClicked reads the item, like every other site.
        attachFolderDropTarget(std::string{});
        if (ImGui::IsItemClicked()) {
            record(ActionKind::Navigate, {});
        }
        ImGui::PopID();
        drawRowChrome(rootCursor, rootMin, rootMax, rootHovered, currentDir.empty(), rootDisplayName(rootUtf8), {});

        for (std::size_t i = 0; i < visibleRows.size(); ++i) {
            const TreeRow& row = visibleRows[i];
            ImGui::PushID(static_cast<int>(i));  // two directories CAN share a leaf name (E9/S11)
            // C3 -- Indent(0.0f) indents by the DEFAULT IndentSpacing, not by zero. Compute the amount ONCE so the
            // Indent/Unindent pair can never desync.
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
            const bool current = row.path == currentDir;
            if (current) {
                flags |= ImGuiTreeNodeFlags_Selected;
            }
            // D5: our openDirs is AUTHORITATIVE over ImGui's storage (ImGuiCond_Always), except for a _Leaf node.
            ImGui::SetNextItemOpen(row.open, ImGuiCond_Always);
            // The constant str_id "##dir", unique per row via PushID(i), and an EMPTY display label (task E.6.3): the
            // name is drawn by hand below, so a "##" in it renders literally there -- review gap 4 stays closed.
            const ImVec2 rowCursor = ImGui::GetCursorScreenPos();
            bool nowOpen = false;
            {
                const ClearHeaderStyle clearRowFill(ChromeRole::Panel);
                ImGui::PushStyleColor(ImGuiCol_Text, roleVec4(ChromeRole::TextFaint));  // ImGui draws only the arrow
                chrome.treeArrowTextColor = ImGui::GetColorU32(ImGuiCol_Text);          // the live slot the arrow reads
                nowOpen = ImGui::TreeNodeEx("##dir", flags, "%s", "");
                ImGui::PopStyleColor();
            }
            const ImVec2 rowMin = ImGui::GetItemRectMin();
            const ImVec2 rowMax = ImGui::GetItemRectMax();
            const bool rowHovered = ImGui::IsItemHovered();
            // C1 -- LOAD-BEARING. _Leaf makes TreeNodeEx return TRUE unconditionally, so a leaf's return value
            // carries NO open/closed information; comparing it would record a spurious ToggleDir every frame.
            // task E.4.3 -- site 1: the tree row, a folder by construction.
            attachFolderDropTarget(row.path);
            // task E.4.3: the right-click recorder, IMMEDIATELY after the item and inside the PushID/PopID pair. It is
            // IsItemClicked(Right), never BeginPopupContextItem: PushID(i) is an INDEX that changes as rows open.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                record(ActionKind::OpenContextMenu, row.path);
            }
            if (!row.knownLeaf && nowOpen != row.open) {
                record(ActionKind::ToggleDir, row.path);
            } else if (ImGui::IsItemClicked()) {
                // With _OpenOnArrow, a click on the LABEL never toggles, so this is "make me the current directory".
                record(ActionKind::Navigate, row.path);
            }
            drawRowChrome(rowCursor, rowMin, rowMax, rowHovered, current, leafOf(row.path), row.path);
            if (indented) {
                ImGui::Unindent(indentAmount);
            }
            ImGui::PopID();  // no continue/break/return anywhere between Push and Pop
        }
        list->ChannelsMerge();  // BEFORE EndChild, on every path through this block
    }
    ImGui::EndChild();  // UNCONDITIONAL -- F9, and the opposite of the table rule below
    const ImVec2 treeMin = ImGui::GetItemRectMin();
    const ImVec2 treeMax = ImGui::GetItemRectMax();
    chrome.treeChildMin = Vec2{treeMin.x, treeMin.y};
    chrome.treeChildMax = Vec2{treeMax.x, treeMax.y};
    // D8: the divider rule on the pane's right, in place of the child border.
    roleFill(ImGui::GetWindowDrawList(), ImVec2(treeMax.x, treeMin.y), ImVec2(treeMax.x + ruleHeight(), treeMax.y),
             ChromeRole::Divider, 0.0F);
}

// ---- phase 4: the right pane -- task E.6.3 (D8): ONE group, so the SameLine after the tree carries all of it -----
// SameLine carries over to exactly ONE item (ItemSize puts the cursor back at the indent, imgui.cpp:12128): a
// crumb row submitted on its own would sit beside the tree and push the grid below it. The group's height is
// crumbRowHeight + the rule + ItemSpacing.y + the child's, which assetContentsPaneLayout makes paneHeight exactly --
// for every pane the row shows in, since onDraw records that chrome and assetBrowserLayout shows the row only in a pane
// of at least it + one frame of contents (ImGui draws no child shorter than 4 points). Elsewhere the row FOLDS (task
// E.6.3's code-review round, rounds 2 and 3 -- decided for both Issues states alike): no crumb, no crumb drop target,
// no rule -- the group is the ##contents child alone, the whole pane, and the tree beside it still navigates.
void AssetBrowserPanel::drawContentsPane(float paneHeight, bool crumbRowShown) {
    const AssetContentsPaneLayout pane =
        assetContentsPaneLayout(paneHeight, ImGui::GetFrameHeight(), ruleHeight(), ImGui::GetStyle().ItemSpacing.y,
                                currentUiScale(), crumbRowShown);
    chrome.crumbRowShown = crumbRowShown;
    chrome.crumbRowHeight = pane.crumbRowHeight;
    ImGui::BeginGroup();
    if (crumbRowShown) {
        drawCrumbRow(pane.crumbRowHeight);
    }
    if (viewMode == AssetViewMode::Grid) {  // task 3.1.3, Step 6 -- one child, two bodies (§D-7)
        drawContentsGrid(pane.contentsHeight);
    } else {
        drawContentsList(pane.contentsHeight);
    }
    const ImVec2 contentsMin = ImGui::GetItemRectMin();  // EndChild's item: the ##contents child
    const ImVec2 contentsMax = ImGui::GetItemRectMax();
    ImGui::EndGroup();
    chrome.contentsChildMin = Vec2{contentsMin.x, contentsMin.y};
    chrome.contentsChildMax = Vec2{contentsMax.x, contentsMax.y};
    chrome.contentsGroupMaxY = ImGui::GetItemRectMax().y;  // EndGroup's item: the whole group
}

// The breadcrumb, at the top of the group: InvisibleButton crumbs with hand-drawn labels (so a "##" in a folder name
// draws, closing the old SmallButton gap), every crumb a folder drop target, the RECURSIVE item count at the right.
void AssetBrowserPanel::drawCrumbRow(float rowHeight) {
    const float s = currentUiScale();
    const float frame = ImGui::GetFrameHeight();
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    ImDrawList* const list = ImGui::GetWindowDrawList();
    assetBreadcrumbCrumbs(rootDisplayName(rootUtf8), currentDir, crumbs);
    // N is the RECURSIVE record count under the current folder -- the tree's number for it, and its folder tile's --
    // whatever the search, the kind filter or Show hidden do (D8; seed S66 hands it the listing's size instead).
    std::size_t items = 0;
    if (databasePtr != nullptr) {
        items = folderCounts.countUnder(databasePtr->records(), databasePtr->generation(), currentDir);
    }
    assetItemsText(items, itemsScratch);
    const float separatorWidth = faceTextWidth(PanelFace::Body, CRUMB_SEPARATOR);
    float ink = separatorWidth * static_cast<float>(crumbs.size() - 1U);  // assetBreadcrumbCrumbs yields >= 1
    for (std::size_t i = 0; i < crumbs.size(); ++i) {
        ink += faceTextWidth(i + 1U == crumbs.size() ? PanelFace::Strong : PanelFace::Body, crumbs[i].label);
    }
    const AssetCrumbRowLayout row = assetCrumbRowLayout(rowMin.x, rowWidth, ink, 2U * (crumbs.size() - 1U),
                                                        faceTextWidth(PanelFace::MonoSmall, itemsScratch), s);
    const float frameTop = rowMin.y + row.labelOffsetY;
    const float labelY = frameTop + ((frame - panelFacePoints(PanelFace::Body)) * 0.5F);
    const bool crumbsEnabled = !rootUtf8.empty();
    ImGui::BeginDisabled(!crumbsEnabled);  // 1:1 with EndDisabled; no continue/return between
    // The crumbs' drop targets are what attachFolderDropTarget counted across the loop: the tree's rows draw first and
    // count too, so the row's share is the delta (the code-review round's finding 14 -- a bump beside the call
    // counted iterations, and a crumb that lost its target still read 3).
    const std::uint32_t targetsBefore = chrome.folderDropTargetsAttached;
    float x = row.firstCrumbX;
    for (std::size_t i = 0; i < crumbs.size(); ++i) {
        const bool current = i + 1U == crumbs.size();
        const PanelFace face = current ? PanelFace::Strong : PanelFace::Body;
        const float width = faceTextWidth(face, crumbs[i].label);
        if (i > 0U) {
            drawRoleText(list, PanelFace::Body, ImVec2(x, labelY), ChromeRole::TextFaint, CRUMB_SEPARATOR);
            x += separatorWidth + row.crumbGap;
        }
        ImGui::SetCursorScreenPos(ImVec2(x, frameTop));
        ImGui::PushID(static_cast<int>(i));  // two segments CAN share a name ("a/x/x") -- F26/E9
        const bool pressed = ImGui::InvisibleButton("##crumb", ImVec2(std::max(width, 1.0F), frame));
        // The item this crumb's drop target must bind to (I324): the id the button was GIVEN -- its label hashed on
        // this same ID stack, as InvisibleButton hashes it (imgui_widgets.cpp:854) -- never GetItemID(), which reads
        // whatever item was submitted last and would match an item slipped in before the attach call below.
        chrome.crumbItemIds.push_back(ImGui::GetID("##crumb"));
        if (i == 0U) {
            // task E.4.3 (code-review G1) -- site 5: the ROOT crumb, the one "" destination the segment arm below
            // structurally cannot produce. "" is LEGAL (rung 3 accepts it).
            attachFolderDropTarget(std::string{});
        } else {
            attachFolderDropTarget(crumbs[i].relative);  // task E.4.3 -- site 4: "move up one level"
        }
        const bool hovered = ImGui::IsItemHovered();
        if (pressed) {
            record(ActionKind::Navigate, crumbs[i].relative);
        }
        ImGui::PopID();                           // no continue/break/return between Push and Pop
        ChromeRole role = ChromeRole::TextLabel;  // an ancestor; `text` while hovered
        if (current) {
            role = ChromeRole::TextBright;
        } else if (hovered) {
            role = ChromeRole::Text;
        }
        drawRoleText(list, face, ImVec2(x, labelY), role, crumbs[i].label);
        ++chrome.crumbsDrawn;
        x += width + row.crumbGap;
    }
    chrome.crumbDropTargets = chrome.folderDropTargetsAttached - targetsBefore;
    const auto rowTargets = static_cast<std::ptrdiff_t>(targetsBefore);
    chrome.crumbDropTargetItemIds.assign(std::next(chrome.folderDropTargetItemIds.begin(), rowTargets),
                                         chrome.folderDropTargetItemIds.end());
    ImGui::EndDisabled();
    // The items text, Mono small textFaint, right-aligned and clipped at the row's end -- never over a crumb.
    const float itemsY = frameTop + ((frame - panelFacePoints(PanelFace::MonoSmall)) * 0.5F);
    list->PushClipRect(ImVec2(row.itemsX, rowMin.y), ImVec2(row.itemsClipMaxX, rowMin.y + rowHeight), true);
    drawRoleText(list, PanelFace::MonoSmall, ImVec2(row.itemsX, itemsY), ChromeRole::TextFaint, itemsScratch);
    list->PopClipRect();
    chrome.itemsText = itemsScratch;
    // The rule under the row, then ONE layout item spanning row + rule, so ItemSize adds the one ItemSpacing.y the
    // pane layout subtracted and the ##contents child starts exactly there.
    horizontalRule(list, rowMin.x, rowMin.x + rowWidth, rowMin.y + rowHeight, ChromeRole::Divider);
    ImGui::SetCursorScreenPos(rowMin);
    ImGui::Dummy(ImVec2(rowWidth, rowHeight + ruleHeight()));
}

// ---- phase 4: the right pane (the current directory's contents) ---------------------------------
// task 3.1.3, Step 6: renamed from drawContentsPane -- BYTE-IDENTICAL otherwise (D3/AC-2). The
// caller now chooses between this and drawContentsGrid below.
void AssetBrowserPanel::drawContentsList(float paneHeight) {
    // task E.6.3 (D8): borderless -- the crumb row's rule frames it.
    ImGui::BeginChild("##contents", ImVec2(0.0F, paneHeight), ImGuiChildFlags_None);
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
                const float sizeColumnWidth = ImGui::GetFontSize() * ASSET_SIZE_COLUMN_FONT_MULTIPLE;
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
                        // task 3.1.5: IMMEDIATELY after the Selectable, before anything reads
                        // g.LastItemData. UNCONDITIONAL here -- searchAssets never matches a folder
                        // (the browser's own recorded fact), so every hit is a file.
                        beginAssetDragSource(hit.relativePath, hit.relativePath.c_str(), /*isDirectory=*/false);
                        ImGui::SameLine(0.0F, 0.0F);
                        ImGui::TextUnformatted(hit.relativePath.c_str());
                        // task E.4.5: the row shows the full path; rule 4 compares against the LEAF
                        const std::string_view hitLeaf = leafOf(hit.relativePath);
                        drawMaterialRowSuffix(thumbnailsPtr, databasePtr, hit.relativePath, hitLeaf, labelScratch);
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
            const float sizeColumnWidth = ImGui::GetFontSize() * ASSET_SIZE_COLUMN_FONT_MULTIPLE;
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, sizeColumnWidth);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            // task E.6.3 (D8): no ".." row -- the crumb row navigates up AND accepts drops.

            // code-review BLOCKING-2 (AC-13): filters by the kind combo ALONE -- `filter.query` is
            // guaranteed empty here (the branch above returns for a non-empty query), so
            // matchesFilter's own substring clause is always a no-op and this call is exactly "apply
            // the kind filter". `filter.anyKind == true` (the default) returns every index, so this
            // costs nothing extra in the common case beyond one small per-frame scratch vector -- the
            // SAME "not persisted, rebuilt every frame" posture `crumbs` already has above.
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
                    // task 3.1.5: same position as the search row above. GUARDED -- this list does
                    // show folders, and the helper's own kind test would refuse one anyway; checking
                    // the cheap thing here keeps findByPath off the per-directory path.
                    beginAssetDragSource(rel, entry.name.c_str(), entry.isDirectory);
                    if (entry.isDirectory) {
                        attachFolderDropTarget(rel);  // task E.4.3 -- site 3: a folder row in the list
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {  // task E.4.3
                        record(ActionKind::OpenContextMenu, rel);
                    }
                    ImGui::SameLine(0.0F, 0.0F);
                    ImGui::TextUnformatted(entry.name.c_str());
                    drawMaterialRowSuffix(thumbnailsPtr, databasePtr, rel, entry.name, labelScratch);  // task E.4.5
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

// ---- task 3.1.5 (§0.11/§D-21): the ONE drag source, three call sites -------------------------------
void AssetBrowserPanel::beginAssetDragSource(const std::string& relativePath, const char* previewText,
                                             bool isDirectory) {
    // REFUSAL AT THE SOURCE, in this order. The cheap kind test runs before findByPath so a directory
    // never reaches the database at all, and a payload that would be refused at every target is never
    // created in the first place.
    //
    // task E.4.3: `isDirectory` is REAL now. classifyAssetKind's isDirectory argument only ever
    // FORCES Folder, so no draggable kind changes classification and the asset arm below is
    // byte-identical in behaviour -- a folder took the early return before and takes the MOVE branch
    // now.
    const AssetKind kind = classifyAssetKind(leafOf(relativePath), isDirectory);
    const AssetRecord* const record =
        (assetKindIsDraggable(kind) && databasePtr != nullptr) ? databasePtr->findByPath(relativePath) : nullptr;
    if (record == nullptr || !record->guid.valid()) {
        // NOT a draggable asset: a folder, a non-draggable kind (a .txt, a .mtl), an unscanned file,
        // or an Invalid-state record. It can still be MOVED, which is the second payload's whole
        // reason for existing.
        beginAssetMoveBranch(relativePath, previewText, kind, isDirectory);
        return;
    }
    if (!ImGui::BeginDragDropSource()) {
        return;  // NOT a drag this frame. EndDragDropSource is owed ONLY when this returned true.
    }
    AssetDragPayload payload{};
    // Guid's hi/lo NSDMIs make AssetDragPayload non-trivially-default-constructible, so value-init
    // runs a constructor instead of zeroing the whole object and the SEVEN tail padding bytes stay
    // indeterminate (measured -- asset_drag.hpp records it). SetDragDropPayload memcpy's all 24, so
    // this is the ONE call site where zeroing them belongs, exactly as that header says. Nothing reads
    // them today; this makes the transmitted bytes deterministic for anyone who later hashes or
    // compares a payload.
    std::memset(&payload, 0, sizeof payload);
    payload.guid = record->guid;
    payload.kind = static_cast<std::uint8_t>(kind);
    ImGui::SetDragDropPayload(ASSET_PAYLOAD_TYPE, &payload, sizeof(payload));  // ImGui COPIES (heap, >16B)
    ImGui::TextUnformatted(previewText);  // the house preview: label text, no thumbnail
    ImGui::EndDragDropSource();
}

// task E.4.3: the move arm. A SEPARATE function rather than a second branch inside the one above, so
// each stays 1:1 Begin/End with no return between the pair -- the header's own standing constraint.
//
// The fit test happens BEFORE BeginDragDropSource: a path this long cannot be encoded, and a
// TRUNCATING encode would silently move a DIFFERENT file. relativePath.empty() takes this refusal too,
// through assetMovePayloadFits's first term -- the assets root is never draggable, and SourceIsRoot
// would refuse it at the target anyway.
//
// Per-frame cost, measured against the pinned source rather than assumed: SetDragDropPayload
// (imgui.cpp:15725) runs resize(0) then resize(n) EVERY frame the source is active, because the
// default cond is ImGuiCond_Always -- and ImVector::resize reallocates only above capacity while
// resize(0) does not free. So after the first frame of a drag this is ONE 1028-byte memcpy per frame
// and ZERO allocations.
void AssetBrowserPanel::beginAssetMoveBranch(const std::string& relativePath, const char* previewText, AssetKind kind,
                                             bool isDirectory) {
    if (!assetMovePayloadFits(relativePath)) {
        return;
    }
    if (!ImGui::BeginDragDropSource()) {
        return;
    }
    // NO memset: this type has no padding at all (1024 + 2 + 1 + 1 == 1028, already 2-aligned), which
    // its own static_asserts pin -- unlike AssetDragPayload, whose seven tail bytes need one.
    AssetMoveDragPayload payload{};
    std::memcpy(payload.path.data(), relativePath.data(), relativePath.size());
    payload.path[relativePath.size()] = '\0';
    payload.length = static_cast<std::uint16_t>(relativePath.size());
    payload.kind = static_cast<std::uint8_t>(kind);
    payload.isDirectory = isDirectory ? 1U : 0U;
    ImGui::SetDragDropPayload(ASSET_MOVE_PAYLOAD_TYPE, &payload, sizeof(payload));
    ImGui::TextUnformatted(previewText);
    ImGui::EndDragDropSource();
}

// task E.4.3: ONLY A FOLDER GETS A TARGET. A file tile submits none, so dropping on a file draws no
// highlight and does nothing -- correct, and needing no refusal.
//
// THE PEEK RULE, a third application (hierarchy_panel.cpp:207-210's shape): classifyAssetMove runs
// BEFORE AcceptDragDropPayload, so an illegal drop draws NO HIGHLIGHT. ImGui draws the highlight as a
// side effect of Accept, so calling it and then deciding is a visible promise the editor then breaks.
void AssetBrowserPanel::attachFolderDropTarget(const std::string& folderRelative) {
    // task E.6.3: counted HERE, first, before BeginDragDropTarget's early return -- no test can put a payload in
    // flight, so the call is the furthest point a record can observe. drawCrumbRow reads its share as a delta. The
    // item it binds to is recorded with it: BeginDragDropTarget reads g.LastItemData.ID, which GetItemID() returns.
    ++chrome.folderDropTargetsAttached;
    chrome.folderDropTargetItemIds.push_back(ImGui::GetItemID());
    if (!ImGui::BeginDragDropTarget()) {
        return;  // EndDragDropTarget is owed ONLY when this returned true (F18)
    }
    const ImGuiPayload* const peek = ImGui::GetDragDropPayload();
    std::string source;
    const char* acceptType = nullptr;
    if (peek != nullptr && peek->IsDataType(ASSET_MOVE_PAYLOAD_TYPE)) {
        source = decodeAssetMoveDragPayload(peek->Data, peek->DataSize).value_or(std::string{});
        acceptType = ASSET_MOVE_PAYLOAD_TYPE;
    } else if (peek != nullptr && peek->IsDataType(ASSET_PAYLOAD_TYPE)) {
        // The payload is a HINT and the database is the AUTHORITY -- asset_drag.hpp's own rule,
        // applied where it was written to apply. A record that vanished mid-drag yields "", and
        // classifyAssetMove("", folder) then answers SourceIsRoot, so the drop refuses. That is right.
        if (const std::optional<AssetDragPayload> decoded = decodeAssetDragPayload(peek->Data, peek->DataSize);
            decoded.has_value() && databasePtr != nullptr) {
            if (const AssetRecord* const found = databasePtr->findByGuid(decoded->guid); found != nullptr) {
                source = found->relativePath;
            }
        }
        acceptType = ASSET_PAYLOAD_TYPE;
    }
    if (acceptType != nullptr) {
        finishFolderDrop(source, folderRelative, acceptType);
    }
    ImGui::EndDragDropTarget();
}

// task E.4.3 (code-review G6): THE WHOLE DECISION AND ITS WHOLE EFFECT, in ONE function called by the
// real ImGui target above and by the injected seam below. The verdict, the accept counter and the
// ActionKind::MoveEntry record all live here, so a statement deleted from this function breaks the
// PRODUCT and the CASES together.
//
// It did not, and that was the gap: the seam used to carry its own copy of the counter and the
// record, so deleting the record from the real target left drag-to-move doing nothing in the product
// while I222, I223 and I224 all stayed green -- the exact "assert the EFFECT" failure this repo's
// rules exist to prevent.
//
// `acceptType == nullptr` IS THE INJECTED SEAM. Nothing in tests/ can perform a real drag -- the
// backend rewrites io.MousePos every NewFrame -- so there is no ImGui payload in flight to Accept and
// the seam models a COMPLETED drop, whose delivery is therefore unconditional. Everything else is
// identical, including which function computes the verdict.
//
// THE PEEK RULE IS HONOURED BY THE ORDER OF THE STATEMENTS: the verdict runs FIRST and returns early,
// so AcceptDragDropPayload -- which is what draws the highlight -- is never reached for an illegal
// drop. Moving the Accept above the verdict is a visible promise the editor then breaks.
void AssetBrowserPanel::finishFolderDrop(const std::string& source, const std::string& folderRelative,
                                         const char* acceptType) {
    dropPeekRefusal = folderDropVerdict(source, folderRelative);
    if (dropPeekRefusal != AssetOpRefusal::None) {
        return;
    }
    ++dropTargetsAcceptedCount;  // an EFFECT counter: this target really accepted
    const bool delivered =
        acceptType == nullptr || ImGui::AcceptDragDropPayload(acceptType, ImGuiDragDropFlags_None) != nullptr;
    if (delivered) {
        record(ActionKind::MoveEntry, source, folderRelative);
    }
}

// task E.4.3: THE DECISION, shared by the real ImGui target above and the injected seam below, so
// the two cannot diverge. It IS classifyAssetMove -- which is itself a call to assetOpPathLadder --
// so the peek, the drop-time plan and this all answer from one ladder rather than three copies.
AssetOpRefusal AssetBrowserPanel::folderDropVerdict(const std::string& source, const std::string& folderRelative) {
    return classifyAssetMove(source, folderRelative);
}

// task E.4.3: the injected drop, and the honest statement of what it does and does not cover.
//
// NOTHING IN tests/ CAN PERFORM A REAL DRAG -- the backend rewrites io.MousePos every NewFrame -- so
// ImGui's own BeginDragDropTarget/AcceptDragDropPayload half of the path above has NO AUTOMATED
// WITNESS ANYWHERE, and the drop HIGHLIGHT has none either (validation row 4 is its only cover).
// What this seam does exercise is everything the panel decides: the source resolution (including the
// database re-resolution an asset payload goes through), the verdict through the SAME
// folderDropVerdict the real target calls, and the ActionKind::MoveEntry record with BOTH paths.
void AssetBrowserPanel::applyInjectedDropPeek() {
    if (!dropPeekActive) {
        return;
    }
    dropPeekActive = false;
    std::string source = dropPeekSource;
    if (!dropPeekIsMovePayload) {
        // An ASSET payload carries a GUID, and the payload is a HINT while the database is the
        // AUTHORITY. THE RESOLUTION GOES THROUGH findByGuid, exactly as the real target's does
        // (code-review G6): resolving by PATH here would make I223's "re-resolved through the live
        // database" subcase a claim about the simulation rather than about the product. The seam is
        // handed a path because that is all a test can spell, so it turns that into the GUID a real
        // payload would carry and then resolves THAT -- one extra lookup, and the arm under test is
        // the real one. A record that vanished mid-drag yields "", which the verdict refuses as
        // SourceIsRoot.
        source.clear();
        if (databasePtr != nullptr) {
            if (const AssetRecord* const named = databasePtr->findByPath(dropPeekSource); named != nullptr) {
                if (const AssetRecord* const found = databasePtr->findByGuid(named->guid); found != nullptr) {
                    source = found->relativePath;
                }
            }
        }
    }
    // The SAME tail the real target runs, with no ImGui payload in flight to Accept.
    finishFolderDrop(source, dropPeekDestination, nullptr);
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
    const bool selected = rel == selectedEntry;
    // task E.6.3 (D8): ImGui paints no tile fill -- a Selectable fills a SQUARE over its half-spacing-widened box
    // (imgui_widgets.cpp:7387-7397) -- so the three Header slots are cleared around this ONE call; the drag source's
    // tooltip and the face after it keep the normal palette. The face draws every state.
    bool tilePressed = false;
    {
        const ClearHeaderStyle clearTileFill(ChromeRole::Panel);  // the pane's own ground
        tilePressed = ImGui::Selectable("##tile", selected, selFlags, tileDims);
    }
    const bool tileHovered = ImGui::IsItemHovered();
    const bool tileHeld = ImGui::IsItemActive();
    if (tilePressed) {
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

    // task 3.1.5: BEFORE the draw-list capture, and that ordering is load-bearing --
    // BeginDragDropSource pushes a TOOLTIP WINDOW, so capturing this window's draw list afterwards
    // would be a question worth asking. Placing the helper first removes the question entirely.
    beginAssetDragSource(rel, entry.name.c_str(), entry.isDirectory);
    if (entry.isDirectory) {
        attachFolderDropTarget(rel);  // task E.4.3 -- site 2: a folder tile in the grid
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {  // task E.4.3
        record(ActionKind::OpenContextMenu, rel);
    }

    // task 3.1.3, Step 7: three lines, none of which mutate (§D-7) -- the ONLY thumbnail participation
    // in the draw walk. `thumbnailKeyFor` returns nullopt for a folder, an undecodable extension, or
    // any of INV-V3's other six guards; `noteVisible` appends to the service's own per-frame scratch,
    // which service() clears, and `nativeTextureFor` is a const read that answers nullptr until Ready.
    void* texture = nullptr;
    // task E.4.5: the material CARD rides the SAME key -- no second question is asked of the record -- on its OWN
    // queue (noteCardWanted, never noteVisible, whose meaning E.3.3's S34/I167 pin). The cache answers "not a
    // material" by never reading one, so this block names no kind. `card` stays valid for the whole draw walk
    // (the cache mutates only in its service pass), so the face may view its name.
    const MaterialCard* card = nullptr;
    if (const std::optional<ThumbnailKey> key = thumbnailKeyFor(entry, rel);
        key.has_value() && thumbnailsPtr != nullptr) {
        thumbnailsPtr->noteVisible(*key);
        texture = thumbnailsPtr->nativeTextureFor(*key);
        thumbnailsPtr->noteCardWanted(*key);
        card = thumbnailsPtr->cardFor(*key);
    }

    // task E.3.3 (D6): the face is drawAssetTileFace's now -- the picker draws the SAME one, so a
    // pixel of padding cannot drift between the two a year from now. A search hit folds its containing
    // folder into the SAME caption (the plan's own "subtitled" requirement), so it stays identifiable
    // outside the directory the user is currently browsing (AC-15). `captionSource` is a NAMED LOCAL
    // because the face holds a view into it, alive until the call returns.
    // task E.6.3 (D8): a folder tile carries its RECURSIVE count -- the tree's number for it, from the ONE cache.
    std::size_t folderCount = 0;
    if (entry.isDirectory && databasePtr != nullptr) {
        folderCount = folderCounts.countUnder(databasePtr->records(), databasePtr->generation(), rel);
    }
    std::string captionSource = entry.name;
    if (isSearchHit) {
        const std::string parent = parentOf(rel);
        if (!parent.empty()) {
            captionSource = parent + "/" + entry.name;
        }
    }
    // A designated initialiser must follow DECLARATION order -- clang accepts a wrong one with a
    // warning while GCC and MSVC reject (E.2.2's finding 3). This is AssetTileFace's own order.
    const AssetTileFace face{.nativeTexture = texture,
                             .kind = classifyAssetKind(entry.name, entry.isDirectory),
                             .isDirectory = entry.isDirectory,
                             .fileName = entry.name,
                             .captionSource = captionSource,
                             .tileW = tileW,
                             .tileEdge = tileEdge,
                             .pad = pad,
                             // task E.4.5, in DECLARATION order. Rule 4 runs HERE against entry.name -- the leaf,
                             // search hit or not -- never against captionSource, which folds in the parent.
                             .subtitle = materialCardSubtitle(card, entry.name),
                             .tint = materialCardTint(card),
                             .folderCount = folderCount,
                             .hovered = tileHovered,
                             .selected = selected,
                             .held = tileHeld};
    ImDrawList* const tileList = ImGui::GetWindowDrawList();
    const AssetTileDrawn drawn = drawAssetTileFace(tileList, itemMin, face, labelScratch);
    // task E.6.3 (D14): what the face handed to its draw calls, for the GPU tier.
    if (drawn.ringed) {
        ++chrome.tilesRinged;
        if (selected) {
            chrome.lastSelectedRingColor = drawn.ringColor;
        } else if (!tileHovered && !tileHeld) {
            chrome.lastRestingRingColor = drawn.ringColor;
        }
    }
    if (drawn.hatched) {
        ++chrome.hatchedWells;
        chrome.lastHatchLabelColor = drawn.hatchLabelColor;
        // The well's vertices as the list RECEIVED them (I346), overwritten by every hatched tile: the last one's.
        chrome.lastHatchedWellMin = Vec2{drawn.wellMin.x, drawn.wellMin.y};
        chrome.lastHatchedWellMax = Vec2{drawn.wellMax.x, drawn.wellMax.y};
        chrome.lastHatchedWellRounding = drawn.wellRounding;
        chrome.lastHatchedWellVertices.clear();
        for (int i = drawn.hatch.vertexBegin; i < drawn.hatch.vertexEnd; ++i) {
            const ImDrawVert& vertex = tileList->VtxBuffer[i];
            chrome.lastHatchedWellVertices.push_back(DrawnVertex{Vec2{vertex.pos.x, vertex.pos.y}, vertex.col});
        }
        const int fillVertices = drawn.hatch.stripesBegin - drawn.hatch.vertexBegin;
        chrome.lastHatchedWellStripesBegin = static_cast<std::size_t>(fillVertices);
    }
    if (entry.isDirectory) {
        chrome.lastFolderTabColor = drawn.folderTabColor;
    }
    if (drawn.folderCounted) {
        ++chrome.foldersCounted;
        chrome.folderTileCounts.push_back(AssetFolderTileCount{rel, folderCount});
    }
}

void AssetBrowserPanel::drawContentsGrid(float paneHeight) {
    // task E.6.3 (D8): borderless -- the crumb row's rule frames it.
    ImGui::BeginChild("##contents", ImVec2(0.0F, paneHeight), ImGuiChildFlags_None);
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

        // task E.6.3 (D8): no ".." bar -- the crumb row navigates up AND accepts drops, which the bar never did.

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
// ---- task E.4.3: the context menu -- ONE constant-id popup per pane ------------------------------
// A right-click records OpenContextMenu; the NEXT frame this opens "##assetctx" from
// contextMenuRequested (clearing it) and draws the menu from contextTarget. One menu definition
// serves all three call sites (tree row, grid tile, list row) plus the pane background.
//
// NOT BeginPopupContextItem: the tree's rows are PushID(static_cast<int>(i)) -- an INDEX -- so a
// per-item popup is keyed on an id that changes the moment a directory above it opens, and the popup
// would close or, worse, RETARGET.
//
// NO ACCELERATOR COLUMN. The F2/Del bindings do not ship: they need a third gating condition nobody
// named -- !ImGui::GetIO().WantTextInput, because this panel's own header carries an InputText search
// box, so Del while editing the query would delete the SELECTED ASSET. A menu that advertises a
// shortcut and does nothing when it is pressed is a lie the tree would have to keep.
void AssetBrowserPanel::drawContextMenu() {
    constexpr const char* CONTEXT_MENU_ID = "##assetctx";
    if (contextMenuRequested) {
        ImGui::OpenPopup(CONTEXT_MENU_ID);
        contextMenuRequested = false;
    }
    // F13: EndPopup ONLY when BeginPopup returned true -- the BeginMenu family, not the Begin one.
    if (!ImGui::BeginPopup(CONTEXT_MENU_ID)) {
        return;
    }
    const bool haveProject = !rootUtf8.empty();
    const bool targetIsRoot = contextTarget.empty();

    // contextMenuItemsDrawnCount is incremented once per MenuItem call the body ACTUALLY makes,
    // INCLUDING a disabled one -- a BeginDisabled/EndDisabled pair still submits the item. The count
    // proves the body RAN; the enable state is asserted separately through the seams' effects.
    ImGui::BeginDisabled(!haveProject);
    if (ImGui::MenuItem("New Folder")) {
        record(ActionKind::NewFolder, {});
    }
    ++contextMenuItemsDrawnCount;
    // BeginMenu SUBMITS its own menu item whether or not the submenu opens, so it counts here; the
    // row inside counts only on the frames the submenu is actually open.
    const bool newAssetOpen = ImGui::BeginMenu("New Asset");
    ++contextMenuItemsDrawnCount;
    if (newAssetOpen) {
        if (ImGui::MenuItem("Material")) {
            // D12: the SAME ActionKind the header's New Material button records. Two affordances,
            // one implementation.
            record(ActionKind::CreateMaterial, {});
        }
        ++contextMenuItemsDrawnCount;
        ImGui::EndMenu();  // ONLY because BeginMenu returned true
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::BeginDisabled(targetIsRoot);
    if (ImGui::MenuItem("Rename")) {
        record(ActionKind::RequestRename, contextTarget);
    }
    ++contextMenuItemsDrawnCount;
    if (ImGui::MenuItem("Delete")) {
        record(ActionKind::RequestDelete, contextTarget);
    }
    ++contextMenuItemsDrawnCount;
    ImGui::EndDisabled();

    ImGui::Separator();
    // Copy GUID: disabled for a folder and for a nil-guid record -- existing behaviour, unchanged.
    const AssetRecord* const targetRecord =
        (databasePtr != nullptr && !targetIsRoot) ? databasePtr->findByPath(contextTarget) : nullptr;
    const bool canCopyGuid = targetRecord != nullptr && targetRecord->guid.valid();
    ImGui::BeginDisabled(!canCopyGuid);
    if (ImGui::MenuItem("Copy GUID")) {
        labelScratch = formatGuid(targetRecord->guid);
        ImGui::SetClipboardText(labelScratch.c_str());
    }
    ++contextMenuItemsDrawnCount;
    ImGui::EndDisabled();

    ImGui::EndPopup();
}

// ---- task E.4.3: the two confirmation modals ------------------------------------------------------
// Both copy drawIssues's orphan modal VERBATIM: the IsPopupOpen guard, AlwaysAutoResize,
// TextWrapped("%s", ...), SetItemDefaultFocus on the affirmative button, the hand-bound Escape, and
// the `else` branch that treats a programmatic close as Cancel.
//
// Both are gated on pendingOrphanDelete.empty() by their caller, so the pre-existing modal always
// wins if two are somehow pending at once -- ImGui will happily stack modals and the result is a UI a
// user cannot reason about.
void AssetBrowserPanel::drawRenameModal() {
    constexpr const char* RENAME_MODAL_ID = "Rename asset";
    if (pendingRename.empty()) {
        return;
    }
    if (!ImGui::IsPopupOpen(RENAME_MODAL_ID)) {
        ImGui::OpenPopup(RENAME_MODAL_ID);
    }
    if (ImGui::BeginPopupModal(RENAME_MODAL_ID, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ++renameModalDrawn;  // an EFFECT counter: incremented inside the body that really drew
        labelScratch = "Rename \"" + pendingRename + "\"";
        ImGui::TextWrapped("%s", labelScratch.c_str());  // NEVER a bare format string (F14)
        if (renameFocusPending) {
            ImGui::SetKeyboardFocusHere();
            renameFocusPending = false;  // the FIRST frame only, or the field can never lose focus
        }
        engine::editor::inputTextString("##renameLeaf", renameBuffer, ImGuiInputTextFlags_None);
        // Recomputed EVERY frame the modal draws, from a pure call with no disk touch -- so it is
        // legal inside phase 4b, and the button's disabled state and the error line can never
        // disagree, because both read this one variable computed once.
        renameRefusal = validateAssetName(renameBuffer);
        if (renameRefusal != AssetNameRefusal::None) {
            labelScratch = assetNameRefusalMessage(renameRefusal);
            ImGui::TextWrapped("%s", labelScratch.c_str());
        }
        ImGui::Separator();
        ImGui::BeginDisabled(renameRefusal != AssetNameRefusal::None);
        bool commit = ImGui::Button("Rename");
        ImGui::EndDisabled();
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        bool dismiss = ImGui::Button("Cancel");
        // ImGui CANNOT dismiss a MODAL with Escape: NavUpdateCancelRequest's popup branch excludes
        // ImGuiWindowFlags_Modal (imgui.cpp:15032) and BeginPopupModal always sets it
        // (imgui.cpp:13232) -- and the editor never enables ImGuiConfigFlags_NavEnableKeyboard
        // (imgui_layer.cpp:82), so that path is doubly dead. Bind it ourselves, here, in the body.
        dismiss = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || dismiss;
        // And ENTER is dead for the SAME reason, which is why SetItemDefaultFocus above cannot carry
        // it: that path also needs nav. A macOS pass measured it -- Return left this modal open and
        // renamed nothing while the button committed instantly. So bind Enter here too, symmetric
        // with Escape. THREE things this must get right:
        //   * KeypadEnter is a DISTINCT ImGuiKey (imgui.h:1678), never folded into Enter.
        //   * IsKeyPressed(key, bool) passes ImGuiKeyOwner_Any (imgui.cpp:10478-10481), so it fires
        //     even though the InputText holds Shortcut(ImGuiKey_Enter, ..., id) while active
        //     (imgui_widgets.cpp:5136) -- which is exactly why the Escape binding already works
        //     with the field focused.
        //   * It is gated on the SAME `renameRefusal` the button's BeginDisabled uses, so Enter can
        //     never commit a name the button refuses. A separate condition here would be a second
        //     validation policy with no way to keep the two in step.
        const bool enterPressed =
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        commit = (enterPressed && renameRefusal == AssetNameRefusal::None) || commit;
        // Dismiss WINS over commit: if both arrive in one frame the safe answer is to write nothing.
        if (dismiss) {
            pendingRename.clear();
            ImGui::CloseCurrentPopup();
        } else if (commit) {
            renameRequest = AssetRenameRequest{pendingRename, renameBuffer};
            pendingRename.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else {
        // A safety net, not the Esc mechanism above: only a PROGRAMMATIC close reaches here, because
        // a modal swallows outside clicks. Treating it as Cancel keeps the flow from wedging.
        pendingRename.clear();
    }
}

void AssetBrowserPanel::drawAssetDeleteModal() {
    constexpr const char* DELETE_ASSET_MODAL_ID = "Delete asset?";
    if (pendingDelete.empty()) {
        return;
    }
    if (!ImGui::IsPopupOpen(DELETE_ASSET_MODAL_ID)) {
        ImGui::OpenPopup(DELETE_ASSET_MODAL_ID);
    }
    if (ImGui::BeginPopupModal(DELETE_ASSET_MODAL_ID, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ++assetDeleteModalDrawn;
        // isDirectory comes from the CACHED LISTING's FileEntry, never from classifyAssetKind --
        // which cannot tell a folder from an extension-less file.
        bool isDirectory = false;
        if (const DirectoryListing* const parentListing = cached(parentOf(pendingDelete)); parentListing != nullptr) {
            const std::string_view leaf = leafOf(pendingDelete);
            for (const FileEntry& entry : parentListing->entries) {
                if (entry.name == leaf) {
                    isDirectory = entry.isDirectory;
                    break;
                }
            }
        }
        // ZERO I/O: countRecordsUnder is a lower_bound plus a walk over records(), which is exactly
        // what makes the count legal inside phase 4b at all.
        const std::size_t indexed =
            databasePtr != nullptr ? countRecordsUnder(databasePtr->records(), pendingDelete) : 0;
        const AssetDeletePrompt prompt = assetDeletePromptFor(pendingDelete, isDirectory, indexed);
        ImGui::TextWrapped("%s", prompt.title.c_str());
        ImGui::TextWrapped("%s", prompt.detail.c_str());
        mutedText(prompt.footer);
        ImGui::Separator();
        bool commit = ImGui::Button("Delete");
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        bool dismiss = ImGui::Button("Cancel");
        // Escape AND Enter are both hand-bound -- see drawRenameModal's citation for why neither
        // reaches a modal on its own here. No validity gate on this one: unlike Rename there is no
        // refusable input, so the Delete button is never disabled and Enter matches it exactly.
        dismiss = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || dismiss;
        const bool enterPressed =
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        commit = enterPressed || commit;
        if (dismiss) {  // dismiss WINS: if both arrive in one frame, delete nothing
            pendingDelete.clear();
            ImGui::CloseCurrentPopup();
        } else if (commit) {
            deleteRequest = pendingDelete;  // nothing touches disk here (D9)
            pendingDelete.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else {
        pendingDelete.clear();
    }
}

namespace {

// task E.4.4 (validation finding 2): the Issues count, moved out of drawIssues because onDraw now needs
// it too -- to reserve the header's line BEFORE the panes are sized. ONE sum, so the reservation and the
// header can never disagree about whether the header draws. Defined HERE, not with the helpers at the top
// of the file, so no line above drawIssues moves and every citation of one by number stays true.
// Code-review finding 2 (3.1.1): report.invalid counts EVERY Invalid-state record, INCLUDING one
// a write conflict downgraded -- report.invalidPaths already excludes those, so the count shown
// here must subtract writeConflictTotal too, or the two would silently disagree (logAssetScan's
// own identical subtraction, editor_app.cpp).
[[nodiscard]] std::size_t invalidIssueCount(const AssetScanReport& report) noexcept {
    return report.invalid - report.writeConflictTotal;
}
// code-review SHOULD-FIX 10: `importFailureTotal` (task 3.2.1's phase 7.5) was missing from this
// sum entirely, so a model-only import failure never even opened the header -- total stayed 0 and
// this function returned before ImGui::CollapsingHeader was ever called.
[[nodiscard]] std::size_t issueTotal(const AssetScanReport& report) noexcept {
    return report.orphanTotal + invalidIssueCount(report) + report.aliasedDirTotal + report.writeFailureTotal +
           report.writeConflictTotal + report.hashFailureTotal + report.importFailureTotal;
}

}  // namespace

void AssetBrowserPanel::drawIssues(float bodyHeight) {
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
            // task E.4.4 (validation finding 4): an orphan is EITHER a sidecar whose file is gone OR one whose
            // file is now on the ignore list (docs/09 section 5.10) and still on disk, so the text names both
            // and says what is deleted. ASCII only, as the footer's watcher text is.
            mutedText("No asset uses this sidecar any more -- its file is gone or now on the ignore list");
            mutedText("(docs/09 section 5.10). Only the .meta file is deleted.");
            ImGui::Separator();
            bool commit = ImGui::Button("Delete");
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            bool dismiss = ImGui::Button("Cancel");
            // Enter is hand-bound here for the SAME reason Escape is, below -- SetItemDefaultFocus
            // above cannot carry it while nav is off, so this modal's own "Enter == Delete" claim was
            // never true either. Bound now so all three of this panel's modals answer Enter and
            // Escape identically; a user who learns Enter in one must not find it dead in another.
            const bool enterPressed =
                ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
            commit = enterPressed || commit;
            // ImGui CANNOT dismiss a MODAL with Escape: NavUpdateCancelRequest's popup branch excludes
            // ImGuiWindowFlags_Modal (imgui.cpp:15032) and BeginPopupModal always sets it
            // (imgui.cpp:13232) -- and the editor does not enable ImGuiConfigFlags_NavEnableKeyboard at
            // all (imgui_layer.cpp:79), so that path is doubly dead. Esc is the universal DISMISS key
            // (.claude/rules/editor.md), so we bind it OURSELVES, HERE, inside the body -- repeat=false,
            // one press = one Cancel. Deliberately NOT the global-route chord mechanism used elsewhere:
            // the editor-chord rule exists so a focused InputText can win a chord back, but a modal
            // already blocks every other window, and a global Escape route would also fire on the
            // frames the modal is NOT up.
            dismiss = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || dismiss;
            if (dismiss) {  // dismiss WINS: if both arrive in one frame, delete nothing
                pendingOrphanDelete.clear();
                ImGui::CloseCurrentPopup();
            } else if (commit) {
                confirmedOrphanDelete = pendingOrphanDelete;  // nothing touches disk here (D9)
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
    const std::size_t invalidOnly = invalidIssueCount(report);
    const std::size_t total = issueTotal(report);
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
    // task E.6.3 (D8): the card header's colours, at the header's existing height (validation row 12 looks at it).
    ImGui::PushStyleColor(ImGuiCol_Header, roleVec4(ChromeRole::RaisedHeader));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, roleVec4(ChromeRole::Hover));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, roleVec4(ChromeRole::Active));
    issuesOpen = ImGui::CollapsingHeader(labelScratch.c_str());
    ImGui::PopStyleColor(3);
    if (issuesOpenRequest.has_value()) {  // task E.4.4 -- the seam lands exactly where a click lands
        issuesOpen = *issuesOpenRequest;
        issuesOpenRequest.reset();
    }
    // task E.4.4 (validation finding 2): the body is a scrollable CHILD, as tall as onDraw reserved for it
    // BEFORE sizing the panes (assetBrowserLayout), so a long list scrolls inside it instead of pushing
    // itself and the footer past the bottom of the panel. `bodyHeight` is 0 on the one frame a click opens
    // the header -- the reservation was made while it was closed -- so the body starts on the next frame:
    // never BeginChild at a height of 0, which ImGui reads as "fill the rest of the window".
    if (!issuesOpen || !(bodyHeight >= 1.0F)) {
        return;
    }
    // No border, so the child has no window padding and the measurement below is the rows alone.
    // EndChild ALWAYS runs (F9); the rows are submitted only while the child is visible (C2).
    issuesBodyDrawnHeight = bodyHeight;  // the EFFECT: the height this frame's body child was given
    if (ImGui::BeginChild("##issues", ImVec2(0.0F, bodyHeight))) {
        if (report.orphanTotal > 0) {
            ImGui::TextUnformatted("Orphaned .meta files:");
            for (std::size_t i = 0; i < report.orphans.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::TextUnformatted(report.orphans[i].c_str());  // E20 -- never a format string
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete .meta")) {
                    record(ActionKind::RequestDeleteOrphan, report.orphans[i]);
                }
                ++issueRowsDrawnCount;  // task E.4.4 -- the EFFECT: a row this frame's body really submitted
                ImGui::PopID();         // no continue/break/return between Push and Pop
            }
            if (report.orphanTotal > report.orphans.size()) {
                const std::size_t unlisted = report.orphanTotal - report.orphans.size();
                labelScratch = AERO_GLYPH_ELLIPSIS "and " + std::to_string(unlisted) + " more";
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
                const std::size_t unlisted = totalCount - entries.size();
                labelScratch = AERO_GLYPH_ELLIPSIS "and " + std::to_string(unlisted) + " more";
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
        // task E.4.4: the rows' natural height, for the NEXT frame's reservation -- the cursor past the last
        // row, less the spacing ItemSize added after it. GetCursorPosY is in content space, so the child's
        // own scroll position does not move it.
        issuesContentHeight = std::max(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y, 0.0F);
    }
    ImGui::EndChild();  // UNCONDITIONAL -- F9
}

// ---- phase 5: the footer -- two zones, Mono small (task E.6.3, D8) ---------------------------------------------
// INSIDE today's reservation (assetBrowserLayout's footerHeight = spacing + frame, so AV55, AV59 and I231 hold): the
// left zone is the selection, elided from the LEFT; the right zone -- the listing's notice, the matches, the issues,
// the thumbnails, the watcher word -- draws each segment whole, a narrow row shedding the thumbnails and then the
// watcher word (sooner while a selection is shown: the row reserves its cells, up to ASSET_FOOTER_SELECTION_MIN_CELLS,
// for it) and never pushing the notice left of the row (assetFooterLayout).
void AssetBrowserPanel::drawFooter() {
    const DirectoryListing* const listing = cached(currentDir);
    if (listing == nullptr) {
        return;
    }
    AssetFooterText footerText;
    // The listing's notices are BUILT FIRST AND ALWAYS (review gap 3 / AC-8): a selection once pre-empted them, so
    // clicking any file in a >10 000-entry directory made the truncation notice vanish -- "a silent cap reads as
    // 'this is everything'". BOTH caps are named, because either can fire (review gap 2).
    if (listing->status == ScanStatus::Ok) {
        if (listing->skipped > 0) {
            footerText.notice = std::to_string(listing->skipped) + " skipped";  // AC-8/E5
        }
        if (listing->truncated) {
            if (!footerText.notice.empty()) {
                footerText.notice += "  -  ";
            }
            footerText.notice += "truncated (caps: " + std::to_string(MAX_ENTRIES_PER_DIRECTORY) + " listed, " +
                                 std::to_string(MAX_ENTRIES_EXAMINED) + " scanned)";
        }
    }
    // A non-Ok status contributes NOTHING here: the right pane already explains it.
    if (!selectedEntry.empty()) {
        // code-review BLOCKING-3: matched by the FULL relative path, never by leaf name alone.
        const std::size_t idx = findEntryByRelativePath(listing->entries, currentDir, selectedEntry);
        const bool inCurrentListing = idx < listing->entries.size();
        const AssetRecord* const rec = databasePtr != nullptr ? databasePtr->findByPath(selectedEntry) : nullptr;
        // E10, preserved: a selection in neither the listing nor the database has vanished -- nothing renders.
        if (inCurrentListing || rec != nullptr) {
            std::string& selection = footerText.selection;
            selection = selectedEntry;
            // D5: a record NEVER describes a directory, so `selectedIsDirectory` matters only for a listing member.
            const bool selectedIsDirectory = inCurrentListing && listing->entries[idx].isDirectory;
            if (!selectedIsDirectory) {
                if (inCurrentListing) {
                    const FileEntry& entry = listing->entries[idx];
                    selection +=
                        entry.sizeKnown ? ("  -  " + formatFileSize(entry.size)) : std::string("  -  ") + UNKNOWN_SIZE;
                } else {
                    selection += std::string("  -  ") + UNKNOWN_SIZE;  // never another file's size (BLOCKING-3)
                }
                // task 3.1.1 (§D-7): the identity; task 3.1.2 (§D-11): the import state, in the SAME block.
                if (databasePtr != nullptr) {
                    selection += "  -  ";
                    if (rec == nullptr) {
                        selection += "no .meta";
                    } else if (rec->state == AssetMetaState::Invalid) {
                        selection += "invalid .meta";
                    } else {
                        selection += elideGuid(rec->guid);
                    }
                    if (rec != nullptr && rec->state != AssetMetaState::Invalid && !rec->metaWriteFailed) {
                        selection += "  -  ";
                        selection += importChangeLabel(rec->change);
                    }
                }
            }
        }
    }
    // task 3.1.3, Step 8 (AC-14/AC-28): the search summary, only while a query is committed; `total` is UNCAPPED.
    if (!filter.query.empty()) {
        footerText.matches = std::to_string(searchRows.total) + " matches";
        if (searchRows.truncated) {
            footerText.matches += "  -  truncated (cap: " + std::to_string(MAX_SEARCH_RESULTS) + ")";
        }
    }
    footerText.issues = reportPtr != nullptr ? issueTotal(*reportPtr) : 0U;
    // task E.3.3: the SHARED service's counts; task E.6.3: shown against the resident budget (the mock's R/256).
    footerText.thumbnailsReady = thumbnailsPtr != nullptr ? thumbnailsPtr->readyCount() : 0U;
    footerText.thumbnailsBudget = thumbnailsPtr != nullptr ? MAX_THUMBNAILS_RESIDENT : 0U;
    // task 3.1.4 (AC-36): the watcher's condition, omitted when no watcher has been reconciled -- honest: the panel
    // does not know. ASCII ONLY (3.1.3's post-merge fix, as a rule).
    if (watchStatusPtr != nullptr) {
        // task E.6.2 (D13): ONE classification for the footer and the status bar. `true`: the footer never draws
        // without a project. task E.6.3: the word is handed WHOLE into the footer's own field.
        const WatchStatus& watch = *watchStatusPtr;
        const WatchState footerState = classifyWatchState(true, watch);
        footerText.watcher = assetFooterWatchText(footerState, watch.deferredSweeps, watch.unreadableDirs);
    }

    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    const float monoAdvance = faceTextWidth(PanelFace::MonoSmall, "0");  // Mono is monospaced: one cell's advance
    const AssetFooterLayout footer = assetFooterLayout(rowMin.x, rowWidth, footerText, monoAdvance, currentUiScale());
    ImDrawList* const list = ImGui::GetWindowDrawList();
    const float textY = rowMin.y + ((ImGui::GetFrameHeight() - panelFacePoints(PanelFace::MonoSmall)) * 0.5F);
    if (!footer.left.empty()) {
        drawRoleText(list, PanelFace::MonoSmall, ImVec2(footer.leftX, textY), ChromeRole::TextMuted, footer.left);
    }
    for (const AssetFooterSegment& segment : footer.right) {
        drawRoleText(list, PanelFace::MonoSmall, ImVec2(segment.x, textY), segment.role, segment.text);
        chrome.footerRightTexts.push_back(segment.text);
    }
    chrome.footerLeftText = footer.left;
    // ONE layout item, the height of the TextUnformatted it replaces, inside the reserved band.
    ImGui::Dummy(ImVec2(rowWidth, ImGui::GetTextLineHeight()));
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
            if (thumbnailsPtr != nullptr) {
                thumbnailsPtr->requestReimportClear();
            }
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
        // ---- task E.4.3 -------------------------------------------------------------------------
        case ActionKind::OpenContextMenu:
            // ONE action, THREE effects, and that is not a shortcut: `pending` is one
            // last-writer-wins slot (F1), so recording a separate SelectEntry beside this would
            // clobber it and the menu would open on a stale target.
            selectedEntry = action.path;
            contextTarget = action.path;
            contextMenuRequested = true;
            break;
        case ActionKind::NewFolder:
            // CreateMaterial's arm verbatim: `currentDir` is read HERE because applyPending is the
            // one place that sees committed state.
            newFolderRequest = currentDir;
            break;
        case ActionKind::RequestRename:
            pendingRename = action.path;
            renameBuffer = std::string(leafOf(action.path));
            renameRefusal = AssetNameRefusal::None;
            renameFocusPending = true;
            break;
        case ActionKind::RequestDelete:
            pendingDelete = action.path;  // and NOTHING else
            break;
        case ActionKind::MoveEntry:
            // The ONLY kind that reads PendingAction::destination.
            moveRequest = AssetMoveRequest{action.path, action.destination};
            break;
    }
}

// ---- task 3.1.3: thumbnails, the two-phase wiring (D8) -----------------------------------------

// INV-V3: nullopt unless ALL SEVEN conditions hold, in ONE function so no call site can forget one.
// task E.3.3 split it: this keeps guard 1 (a folder is never a candidate) and guard 3 (no scan has ever
// completed), both of which need a FileEntry and a database; the other five moved into the pure
// thumbnailKeyForRecord, which is all a picker CANDIDATE -- a record, not a (FileEntry, path) pair --
// can be asked. Guard 2 (isThumbnailDecodable) moved WITH them, deriving the leaf from the record's own
// relativePath, so this function no longer needs entry.name for it.
std::optional<ThumbnailKey> AssetBrowserPanel::thumbnailKeyFor(const FileEntry& entry, const std::string& rel) const {
    if (entry.isDirectory) {  // 1: a folder is never a thumbnail candidate
        return std::nullopt;
    }
    if (databasePtr == nullptr) {  // 3: no scan has ever completed
        return std::nullopt;
    }
    const AssetRecord* const record = databasePtr->findByPath(rel);
    return record != nullptr ? thumbnailKeyForRecord(*record) : std::nullopt;  // 4 is structural there
}

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
// task E.4.5: EXACTLY what drawHeader's tile-size combo records, spelled by the same switch -- no default:, so a
// new TileSize is a -Wswitch warning here as it is there.
void AssetBrowserPanel::requestTileSize(TileSize size) noexcept {
    switch (size) {
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
// task E.6.3: a crumb or tree-row click's seam -- queued, then recorded by onDraw under the click's own gate (I337).
void AssetBrowserPanel::requestNavigate(std::string relativeDir) { navigateRequest = std::move(relativeDir); }
// task 3.4.2: the requestReimportAll() shape verbatim -- record(ActionKind::CreateMaterial, {}) is
// exactly what drawHeader() calls when the New Material button returns true, so the request travels
// the SAME applyPending() arm and picks up the SAME currentDir a click would.
void AssetBrowserPanel::requestCreateMaterial() noexcept { record(ActionKind::CreateMaterial, {}); }
// task E.4.4: the requestCreateMaterial() shape verbatim -- record(ActionKind::ToggleHidden, {}) is exactly
// what drawHeader() records when the `Show hidden` checkbox changes.
void AssetBrowserPanel::requestToggleHidden() noexcept { record(ActionKind::ToggleHidden, {}); }

// task E.4.4: read-only views of currentDir's CACHED listing (the rationale is at the declaration).
std::size_t AssetBrowserPanel::cachedEntryCount() const noexcept {
    const DirectoryListing* const listing = cached(currentDir);
    return listing != nullptr ? listing->entries.size() : std::size_t{0};
}
bool AssetBrowserPanel::cachedListingContains(std::string_view leafName) const noexcept {
    const DirectoryListing* const listing = cached(currentDir);
    if (listing == nullptr) {
        return false;
    }
    return std::any_of(listing->entries.begin(), listing->entries.end(),
                       [leafName](const FileEntry& entry) { return entry.name == leafName; });
}

// ---- task E.4.3: the seven gesture seams plus requestDropPeek --------------------------------------
// The first five record EXACTLY what a real widget records, so the next onDraw() drains each through
// the SAME applyPending() arm. The two COMMIT seams do not: they model the modal's own button, which
// sets the one-shot directly from inside the popup body -- the orphan modal's Delete button verbatim.
void AssetBrowserPanel::requestContextMenu(std::string path) { record(ActionKind::OpenContextMenu, std::move(path)); }
void AssetBrowserPanel::requestNewFolder() noexcept { record(ActionKind::NewFolder, {}); }
void AssetBrowserPanel::requestRename(std::string path) { record(ActionKind::RequestRename, std::move(path)); }
void AssetBrowserPanel::requestDelete(std::string path) { record(ActionKind::RequestDelete, std::move(path)); }
void AssetBrowserPanel::requestMove(std::string path, std::string destinationDir) {
    record(ActionKind::MoveEntry, std::move(path), std::move(destinationDir));
}

void AssetBrowserPanel::requestRenameCommit(std::string newLeaf) {
    // The modal's Rename button, verbatim -- including its refusal: an illegal name sets NOTHING and
    // leaves the modal up with its error line, which is what the button's BeginDisabled achieves for
    // a real click.
    if (pendingRename.empty()) {
        return;
    }
    if (validateAssetName(newLeaf) != AssetNameRefusal::None) {
        renameRefusal = validateAssetName(newLeaf);
        renameBuffer = std::move(newLeaf);
        return;
    }
    renameRequest = AssetRenameRequest{pendingRename, std::move(newLeaf)};
    pendingRename.clear();
}

void AssetBrowserPanel::requestDeleteConfirm() noexcept {
    if (pendingDelete.empty()) {
        return;  // nothing pending: the one-shot stays "" and the drain sees no request
    }
    // MOVE, never copy: this function is noexcept and a string COPY can throw std::bad_alloc, which
    // bugprone-exception-escape rejects. takeOrphanDeleteRequest's own idiom -- move out, then clear,
    // because a moved-from string is valid but unspecified.
    deleteRequest = std::move(pendingDelete);
    pendingDelete.clear();
}

void AssetBrowserPanel::requestDropPeek(std::string sourcePath, std::string destinationDir, bool asMovePayload) {
    dropPeekActive = true;
    dropPeekIsMovePayload = asMovePayload;
    dropPeekSource = std::move(sourcePath);
    dropPeekDestination = std::move(destinationDir);
}

// ---- the frame ---------------------------------------------------------------------------------
void AssetBrowserPanel::onDraw(PanelContext& /*context*/) {  // D18: the context is IGNORED
    // task E.4.3: the EFFECT counters reset at the TOP of every frame, BEFORE reconcile(), so each
    // reports what THIS frame's bodies did rather than an accumulating lifetime total.
    contextMenuItemsDrawnCount = 0;
    renameModalDrawn = 0;
    assetDeleteModalDrawn = 0;
    dropTargetsAcceptedCount = 0;
    issueRowsDrawnCount = 0;
    issuesBodyDrawnHeight = 0.0F;
    beginAssetChromeRecordFrame(chrome);  // task E.6.3 (D14): one recorded frame per onDraw
    // task E.6.3: requestNavigate's drain -- the crumbs' own enabled term and no modal, the gate a real click has
    // (a modal blocks every click, imgui.cpp:5620). Recorded for THIS frame's applyPending(). A refused request is
    // DROPPED, never deferred, as a swallowed click is: the reset runs whichever way the gate answered (I337, I338).
    const bool noModal = ImGui::GetTopMostPopupModal() == nullptr;
    const bool crumbsEnabled = !rootUtf8.empty();
    if (navigateRequest.has_value() && crumbsEnabled && noModal) {
        record(ActionKind::Navigate, std::move(*navigateRequest));
    }
    navigateRequest.reset();  // a moved-from optional is still ENGAGED -- the move is not a drain

    reconcile();   // 1 -- the only I/O
    drawHeader();  // 2
    // task E.4.4 (validation finding 2): the footer AND the Issues region are reserved BEFORE the panes
    // are sized. Reserving the footer alone put the Issues header -- and, opened, its whole list -- below
    // the panes and past the bottom of the panel, with the footer after it. The arithmetic, its floors
    // and its NaN guards are assetBrowserLayout's (asset_view.hpp, AV55-AV59); the pane height stays a
    // POSITIVE explicit height because the two panes share it side by side, and never 0 or negative,
    // which BeginChild reads as "fill" or "bottom-align at N from the edge".
    const ImGuiStyle& style = ImGui::GetStyle();
    AssetBrowserLayoutMetrics metrics;
    metrics.availHeight = ImGui::GetContentRegionAvail().y;
    metrics.fontSize = ImGui::GetFontSize();
    metrics.frameHeight = ImGui::GetFrameHeight();
    metrics.textLineHeight = ImGui::GetTextLineHeight();
    metrics.itemSpacingY = style.ItemSpacing.y;
    // task E.6.3 (the code-review round): the contents pane's crumb row, rule and spacing are drawn INSIDE
    // paneHeight, so they are RESERVED before the panes are sized (the #111 rule), from drawContentsPane's inputs.
    metrics.paneChromeHeight =
        assetContentsPaneChromeHeight(metrics.frameHeight, ruleHeight(), metrics.itemSpacingY, currentUiScale());
    metrics.issuesShown = reportPtr != nullptr && issueTotal(*reportPtr) > 0;
    metrics.issuesOpen = issuesOpen;
    metrics.issuesContentHeight = issuesContentHeight;
    const AssetBrowserLayout layout = assetBrowserLayout(metrics);
    layoutMetricsAtDraw = metrics;    // task E.4.4 -- EXACTLY what the layout read (layoutMetrics())
    drawTreePane(layout.paneHeight);  // 3
    ImGui::SameLine();
    drawContentsPane(layout.paneHeight, layout.crumbRowShown);  // 4 -- task E.6.3: crumbs over the contents (D8)
    // task E.4.3: the pane BACKGROUND is a legal context target ("" -- New Folder and New Asset
    // apply there). Recorded only when the click landed on no item at all, so it can never steal a
    // row's own right-click.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::IsAnyItemHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        record(ActionKind::OpenContextMenu, {});
    }

    applyInjectedDropPeek();              // task E.4.3 -- before applyPending, so its record is drained this frame
    drawIssues(layout.issuesBodyHeight);  // 4b -- task 3.1.3, Step 9
    // task E.4.3: the context menu and the two modals, all in phase 4b, AFTER the orphan modal and
    // each gated on pendingOrphanDelete.empty() so the pre-existing modal always wins.
    drawContextMenu();
    if (pendingOrphanDelete.empty()) {
        drawRenameModal();
        drawAssetDeleteModal();
    }
    drawFooter();  // 5
    // task E.4.4: read AFTER every child has ended, so the current window is the panel's own. ImGui set it
    // in this frame's Begin from the previous frame's content, so a steady layout reads its steady value.
    scrollMaxYAtDraw = ImGui::GetScrollMaxY();
    chrome.folderCountRecomputations = folderCounts.recomputations();
    applyPending();  // the ONLY place anything mutates
}

}  // namespace engine::editor

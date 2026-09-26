// editor/src/asset_picker.cpp -- task E.3.3 (D1): the ONE asset-reference field widget. Three rules
// govern this file, and each is pinned:
//
//   1. IT HOLDS NO DECISION. Every predicate is asset_picker_model's, and this file states NO
//      `AssetKind::` literal at all -- the kinds arrive through the rules, which arrive through the
//      field's own AERO_ASSET token. I166(c) pins that as source text.
//   2. THE PEEK RUNS BEFORE THE ACCEPT, always. ImGui draws the drop highlight as a SIDE EFFECT of
//      AcceptDragDropPayload, so calling it and then deciding is a visible promise the editor breaks.
//   3. THE OBSERVABLES AND THE LIVE ONE-SHOTS ARE OWNER-ONLY. MeshRenderer draws TWO Guid rows every
//      frame, so a per-field write would have the second row clobber the first row's answer in the
//      same frame -- assetPickerOpen() would read FALSE while the popup is open.
#include "asset_picker.hpp"

#include <aero/core/log.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/asset_drag.hpp>
#include <aero/editor/material_card.hpp>  // task E.4.5 -- the card's subtitle and tint
#include <aero/editor/project_files.hpp>
#include <aero/editor/thumbnail_cache.hpp>

#include "asset_tile.hpp"
#include "text_input.hpp"
#include "thumbnail_service.hpp"

#include <algorithm>
#include <cfloat>  // FLT_MIN -- the "fill the cell" width ImGui spells that way
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace engine::editor {
namespace {

constexpr const char* POPUP_ID = "##assetpicker";

// The one-per-TU peek (material_panel.cpp's shape, one TU over). It never decodes by hand: every byte
// goes to decodeAssetDragPayload, which is what keeps DR18 green by construction.
[[nodiscard]] std::optional<AssetDragPayload> peekAssetPayload() {
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (payload == nullptr || !payload->IsDataType(ASSET_PAYLOAD_TYPE)) {
        return std::nullopt;
    }
    return decodeAssetDragPayload(payload->Data, payload->DataSize);
}

[[nodiscard]] Vec2 toVec2(ImVec2 v) noexcept { return Vec2{v.x, v.y}; }
[[nodiscard]] ImVec2 toImVec2(Vec2 v) noexcept { return ImVec2{v.x, v.y}; }

// Rebuilds the candidate list and records what it was built FOR, so the draw loop can tell a real
// change from a repeat. A null database clears the list rather than leaving a stale one.
void rebuild(AssetPickerState& state, const AssetFieldInputs& in) {
    state.filter.query = state.search;
    if (in.database != nullptr) {
        buildAssetPickerCandidates(in.database->records(), in.rules, state.filter, MAX_SEARCH_RESULTS,
                                   state.candidates);
        state.builtForGeneration = in.database->generation();
    } else {
        state.candidates.items.clear();
        state.candidates.total = 0;
        state.candidates.truncated = false;
        state.builtForGeneration = 0;
    }
    state.builtForQuery = state.search;
    state.builtForKind = state.filter.anyKind ? std::nullopt : std::optional<AssetKind>{state.filter.kind};
    state.builtForAnyKind = state.filter.anyKind;
}

// A fresh open: empty query, the filter at All (or at the ONE admissible kind), the candidates rebuilt,
// the cursor on the bound asset when it is a candidate. D12: nothing is remembered between opens.
void beginSession(AssetPickerState& state, const AssetFieldInputs& in, const std::vector<AssetKind>& options) {
    state.search.clear();
    state.filter = AssetFilter{};
    state.filter.anyKind = options.size() > 1;
    if (!state.filter.anyKind && !options.empty()) {
        state.filter.kind = options.front();
    }
    state.warnedThisOpen = false;
    state.cursorMovedByKeyboard = false;
    rebuild(state, in);
    state.cursor = initialPickerCursor(in.current, state.candidates);
}

}  // namespace

float assetReferenceFieldWidth(const char* trailingButtonLabel) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float trailingWidth =
        ImGui::CalcTextSize(trailingButtonLabel).x + (2.0F * style.FramePadding.x) + style.ItemSpacing.x;
    return std::max(ImGui::GetContentRegionAvail().x - trailingWidth, ImGui::GetFrameHeight());
}

AssetFieldResult drawAssetReferenceField(const AssetFieldInputs& in, AssetPickerState& state) {
    AssetFieldResult result{};
    const std::vector<AssetKind> options = assetPickerFilterOptions(in.rules);
    const ImGuiStyle& style = ImGui::GetStyle();

    // 1. THE VALUE BUTTON, WHOSE LABEL IS THE ID SUFFIX AND NOTHING ELSE. `###ref` renders nothing --
    //    FindRenderedTextEnd stops at the first `##` (imgui.cpp:3918) -- and hashes from the `###`, so
    //    the id is identical however the bound asset changes and an open popup is never orphaned. An
    //    invisible label still measures one line high (CalcTextSize's `text == text_display_end` arm
    //    returns `{0, font_size}`, imgui.cpp:6447-6448), so the frame stands exactly as tall as a
    //    labelled button's and the anchor arithmetic below is unmoved.
    //
    //    task E.3.4: `thumbnailEdge` grows the frame so the bound asset's thumbnail can live INSIDE
    //    it, which is what makes a material slot's whole row ONE item -- one press target, one drop
    //    target, one popup anchor. A zero edge yields the literal 0.0F this line has always passed, so
    //    the Inspector's row is byte-identical BY CONSTRUCTION.
    const float buttonHeight = in.thumbnailEdge > 0.0F ? in.thumbnailEdge + (2.0F * style.FramePadding.y) : 0.0F;
    const bool pressed = ImGui::Button(in.idSuffix, ImVec2(in.buttonWidth, buttonHeight));

    // 2. THE BUTTON'S GEOMETRY, captured BEFORE the popup is touched. End() restores the parent's
    //    last-item data at EndPopup (imgui.cpp:8848), so reading these afterwards names the button
    //    again today -- the capture is what survives a bump that stops restoring, or a stray item
    //    submitted between here and the anchor.
    const ImVec2 buttonMin = ImGui::GetItemRectMin();
    const ImVec2 buttonMax = ImGui::GetItemRectMax();

    // 3. THE VALUE SENTENCE, ON THE DRAW LIST -- never handed to ImGui as a label. A label is truncated
    //    at its first `##` by the very rule the id above relies on, so an asset named `readme##v2.png`
    //    would read `readme` and one named `##notes.png` would leave the button blank.
    //    asset_browser_panel.cpp:635-637 records the identical hazard for a tile caption (3.1.3's E19)
    //    and answers it the same way. AddText submits NO ImGui item, so the button is still the last
    //    item for the drop target below, and the clip rect is the button's own frame, so a long
    //    sentence is cut at the frame edge rather than spilling across the next cell.
    //
    // 3a. THE THUMBNAIL (task E.3.4), on the draw list INSIDE the button's frame, after ImGui::Button
    //     has painted the frame and before the sentence, so the sentence draws OVER it. It runs above
    //     step 4's BeginDragDropTarget and that is safe for the same reason the AddText below it is:
    //     drawAssetTileFace's own contract says IT SUBMITS NO ImGui ITEM, so the button is still the
    //     last item when the drop target attaches.
    //
    //     drawAssetTileFace with pad = 0 and an EMPTY caption is an ICON-ONLY face: AddText returns
    //     immediately on an empty range (imgui_draw.cpp:1737-1738), so the shared painter needs no
    //     parameter of its own and there is no second painter anywhere (E.3.3's D6, collecting its
    //     dividend -- the browser tile, the picker's grid tile and this row are ONE function, and a
    //     padding change moves all three together).
    //
    //     A NIL reference paints NOTHING: the button's own frame is the empty state and the sentence
    //     already says "None". A NON-NIL guid the database cannot resolve paints the Unknown kind
    //     icon, because "there is something here and I cannot find it" is worth a glyph.
    if (in.thumbnailEdge > 0.0F && in.current.valid()) {
        const AssetRecord* const record = in.database != nullptr ? in.database->findByGuid(in.current) : nullptr;
        AssetTileFace face{};
        face.tileW = in.thumbnailEdge;
        face.tileEdge = in.thumbnailEdge;
        face.pad = 0.0F;                          // the icon is exactly tileEdge square at itemMin
        face.captionSource = std::string_view{};  // icon only
        if (record != nullptr) {
            face.kind = classifyAssetKind(leafOf(record->relativePath), /*isDirectory=*/false);
            face.fileName = leafOf(record->relativePath);  // iconLabelFor's input
            if (in.thumbnails != nullptr) {
                if (const std::optional<ThumbnailKey> key = thumbnailKeyForRecord(*record); key.has_value()) {
                    // MANDATORY, not polite: evictions() excludes only what was touched at the CURRENT
                    // frame, so without this the LRU evicts the one thumbnail the user is looking at
                    // (E.3.3's S34 / I167).
                    in.thumbnails->noteVisible(*key);
                    face.nativeTexture = in.thumbnails->nativeTextureFor(*key);
                }
            }
        }
        drawAssetTileFace(ImGui::GetWindowDrawList(),
                          ImVec2(buttonMin.x + style.FramePadding.x, buttonMin.y + style.FramePadding.y), face,
                          state.tileScratch);
    }

    if (!in.valueText.empty()) {
        ImDrawList* const drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(buttonMin, buttonMax, /*intersect_with_current_clip_rect=*/true);
        // task E.3.4. The zero-thumbnail arm is TODAY'S EXPRESSION, character for character, and it is
        // a BRANCH rather than a formula that reduces to it: (buttonHeight - fontSize) * 0.5F is only
        // ARITHMETICALLY FramePadding.y, and GetFrameHeight() is a rounded sum, so an unbranched
        // "generalisation" could move the Inspector's text by an ulp for no reason at all. The
        // Inspector is byte-identical BY CONSTRUCTION here, not by a test -- nothing in this tree
        // measures a widget rect or reads rendered text.
        const float textX = in.thumbnailEdge > 0.0F
                                ? buttonMin.x + style.FramePadding.x + in.thumbnailEdge + style.ItemInnerSpacing.x
                                : buttonMin.x + style.FramePadding.x;
        const float textY = in.thumbnailEdge > 0.0F
                                ? buttonMin.y + (((buttonMax.y - buttonMin.y) - ImGui::GetFontSize()) * 0.5F)
                                : buttonMin.y + style.FramePadding.y;
        drawList->AddText(ImVec2(textX, textY), ImGui::GetColorU32(ImGuiCol_Text), in.valueText.data(),
                          in.valueText.data() + in.valueText.size());
        drawList->PopClipRect();
    }

    // 4. THE DROP TARGET, on the button. PEEK -> CLASSIFY -> ACCEPT, in that order, always: an illegal
    //    drop must never draw a highlight, and the highlight is AcceptDragDropPayload's side effect.
    if (ImGui::BeginDragDropTarget()) {
        if (const std::optional<AssetDragPayload> asset = peekAssetPayload(); asset.has_value()) {
            const auto kind = static_cast<AssetKind>(asset->kind);
            if (classifyAssetDrop(kind, in.rules.surface, /*targetHasMeshRenderer=*/false, in.rules.fieldKind) !=
                    DropAction::None &&
                ImGui::AcceptDragDropPayload(ASSET_PAYLOAD_TYPE) != nullptr) {
                result = AssetFieldResult{.outcome = AssetFieldOutcome::Dropped, .guid = asset->guid};
            }
        }
        ImGui::EndDragDropTarget();  // ONLY because BeginDragDropTarget returned true
    }

    // 5. THE SEAM'S OPEN ARM -- consumed ONLY by the field it names, so a request for a field that is
    //    not drawn this frame survives to the next and never fires on a wrong one.
    bool opening = pressed;
    if (state.pendingOpen.has_value() && state.pendingOpen->hostId == in.hostId &&
        state.pendingOpen->fieldKey == in.fieldKey) {
        state.pendingOpen.reset();
        opening = true;
    }
    if (opening) {
        ImGui::OpenPopup(POPUP_ID);
        beginSession(state, in, options);
        state.openHostId.assign(in.hostId);
        state.openFieldKey.assign(in.fieldKey);
    }

    // 6. THE FOUR LIVE ONE-SHOTS ARE DROPPED WHEN NOTHING IS OPEN AT ALL. Without this they are not
    //    "pending until the popup opens" -- they are a DELAY LINE: a commit issued while closed would
    //    survive to the next open and commit something the user never chose, on the very frame the
    //    popup appeared. Any drawn reference field may perform this clear; it is idempotent, and it
    //    runs AFTER the open arm above so a request issued in the same tick as the open still applies.
    if (state.openFieldKey.empty()) {
        state.pendingSearch.reset();
        state.pendingMove.reset();
        state.pendingMoveSteps = 1;
        state.pendingCommit = false;
        state.pendingClose = false;
    }

    // 7. THE ANCHOR AND THE SIZE, computed BEFORE BeginPopup so the arithmetic is exact rather than one
    //    frame late. Both SetNext* calls are legal while the popup is closed: BeginPopup clears
    //    NextWindowData on its early-out (imgui.cpp:13196-13199). SetNextWindowSize overrides the
    //    AlwaysAutoResize flag BeginPopup adds (imgui.cpp:8182, :13201), which is what makes a fixed
    //    size legal on a popup at all -- and an API-SET POSITION IS THE ONE ImGui NEVER CLAMPS
    //    (:8279), which is why the anchor does the clamping itself.
    const AssetPickerMetrics metrics{.fontSize = ImGui::GetFontSize(),
                                     .frameHeight = ImGui::GetFrameHeight(),
                                     .textLineHeight = ImGui::GetTextLineHeight(),
                                     .itemSpacingY = style.ItemSpacing.y,
                                     .windowPadding = style.WindowPadding.y,
                                     .buttonWidth = buttonMax.x - buttonMin.x};
    const AssetPickerLayout layout = assetPickerLayout(metrics, options.size() > 1);
    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    const Vec2 workMin = toVec2(viewport->WorkPos);
    const Vec2 workMax = Vec2{workMin.x + viewport->WorkSize.x, workMin.y + viewport->WorkSize.y};
    const AssetPickerAnchor anchor =
        assetPickerAnchor(toVec2(buttonMin), toVec2(buttonMax), layout.popupSize, workMin, workMax);
    ImGui::SetNextWindowPos(toImVec2(anchor.pos), ImGuiCond_Always, toImVec2(anchor.pivot));
    ImGui::SetNextWindowSize(toImVec2(layout.popupSize), ImGuiCond_Always);

    const bool owner = state.openHostId == in.hostId && state.openFieldKey == in.fieldKey;
    const bool open = ImGui::BeginPopup(POPUP_ID);
    if (open) {
        const std::string heading = assetPickerHeading(in.rules);
        ImGui::TextDisabled("%s", heading.c_str());  // a DYNAMIC string is never a format string

        // THE KEYBOARD, on the frame it opens. SetKeyboardFocusHere is ignored while a drag is live
        // (imgui.cpp:9491-9494), which cannot be the open frame. The Hierarchy rename's exact idiom,
        // and it works with NavEnableKeyboard OFF -- which it is, everywhere, always.
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = inputTextString("##search", state.search, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool searchActive = ImGui::IsItemActive();

        if (options.size() > 1) {
            // Through a std::string, never assetKindLabel(...).data(): a string_view carries no
            // NUL-termination guarantee, and ImGui takes a const char*.
            state.tileScratch =
                state.filter.anyKind ? std::string("All") : std::string(assetKindLabel(state.filter.kind));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##kind", state.tileScratch.c_str())) {
                if (ImGui::Selectable("All", state.filter.anyKind)) {
                    state.filter.anyKind = true;
                }
                for (const AssetKind option : options) {
                    const bool selected = !state.filter.anyKind && state.filter.kind == option;
                    state.tooltipScratch = std::string(assetKindLabel(option));
                    if (ImGui::Selectable(state.tooltipScratch.c_str(), selected)) {
                        state.filter.anyKind = false;
                        state.filter.kind = option;
                    }
                }
                ImGui::EndCombo();  // ASYMMETRIC: only because BeginCombo returned true
            }
        }

        // THE REBUILD, on a real change only (D12). A rescan under an open popup must not leave stale
        // rows or dangling record indices; the cursor is clamped after, because a rebuild can only
        // SHRINK the list under it, never move it onto a different item.
        const std::uint64_t generation = in.database != nullptr ? in.database->generation() : 0U;
        const std::optional<AssetKind> wantedKind =
            state.filter.anyKind ? std::nullopt : std::optional<AssetKind>{state.filter.kind};
        if (state.search != state.builtForQuery || state.filter.anyKind != state.builtForAnyKind ||
            wantedKind != state.builtForKind || generation != state.builtForGeneration) {
            rebuild(state, in);
            state.cursor = std::min(state.cursor, state.candidates.items.size());
        }

        // THE KEYS. Every one is HAND-BOUND: NavEnableKeyboard is never set, so ImGui moves nothing and
        // closes nothing by itself. IsKeyPressed(key, bool) forwards ImGuiKeyOwner_Any
        // (imgui.cpp:10478-10481) and TestKeyOwner's Any arm ignores the box's ownership
        // (:11295-11296), so ESCAPE FIRES HERE EVEN WHILE THE BOX HAS THE KEYBOARD -- and the box
        // reverting its own text on the same frame is irrelevant, because the popup is closing.
        // Up/Down are NOT owned by a single-line box without CallbackHistory
        // (imgui_widgets.cpp:4929-4934), so they are ours while the user types. Left/Right/Home/End
        // ARE owned (:4923-4925), so they are ours ONLY when the box is inactive.
        const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        std::optional<AssetPickerMove> move;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
            move = AssetPickerMove::Next;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
            move = AssetPickerMove::Prev;
        }
        if (!searchActive) {
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
                move = AssetPickerMove::Next;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
                move = AssetPickerMove::Prev;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
                move = AssetPickerMove::First;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
                move = AssetPickerMove::Last;
            }
        }
        const bool enterKey = !searchActive && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));

        // THE FOUR LIVE ONE-SHOTS, each drained as its OWN statement, UNCONDITIONALLY, BEFORE it is
        // inspected (F9's rule) -- and only by the OWNER, which is this field by construction, because
        // a non-owner never reaches this body. A move/commit/close/search issued while NOTHING is open
        // is therefore DROPPED rather than surviving to the next open and committing something the
        // user never chose.
        const std::optional<std::string> seamSearch = std::exchange(state.pendingSearch, std::nullopt);
        const std::optional<AssetPickerMove> seamMove = std::exchange(state.pendingMove, std::nullopt);
        const std::size_t seamMoveSteps = state.pendingMoveSteps;
        const bool seamCommit = std::exchange(state.pendingCommit, false);
        const bool seamClose = std::exchange(state.pendingClose, false);
        if (seamSearch.has_value()) {
            state.search = *seamSearch;
            rebuild(state, in);
            state.cursor = std::min(state.cursor, state.candidates.items.size());
        }
        if (seamMove.has_value()) {
            // |delta| reductions in ONE frame, which is exactly what holding an arrow key does. A seam
            // that could only step once would make "move to the third candidate" three ticks.
            for (std::size_t step = 0; step < std::max<std::size_t>(seamMoveSteps, 1); ++step) {
                state.cursor = movePickerCursor(state.cursor, *seamMove, state.candidates.items.size());
            }
            state.pendingMoveSteps = 1;  // drained WITH its move, never left armed for the next one
            state.cursorMovedByKeyboard = true;
        } else if (move.has_value()) {
            state.cursor = movePickerCursor(state.cursor, *move, state.candidates.items.size());
            state.cursorMovedByKeyboard = true;
        }

        // THE GRID. Tile geometry EXACTLY as drawContentsGrid computes it, so one tile is one tile
        // everywhere. The clipper clips ROWS, never tiles -- the browser's own shape, and the reason
        // rowHeight is exact rather than a guess.
        const bool scrollToCursor = state.cursorMovedByKeyboard;
        bool commitRequested = false;
        ImGui::BeginChild("##grid", ImVec2(0.0F, layout.gridHeight), ImGuiChildFlags_Borders);
        const float tileEdge = ImGui::GetFontSize() * tileEdgeFontMultiple(TileSize::Small);
        const float pad = ImGui::GetFontSize() * TILE_CAPTION_PAD_FONT_MULTIPLE;
        const float captionH = static_cast<float>(TILE_CAPTION_LINES) * ImGui::GetTextLineHeight();
        const float tileW = tileEdge + (2.0F * pad);
        const float tileH = tileEdge + captionH + (3.0F * pad);
        const float rowHeight = tileH + ImGui::GetStyle().ItemSpacing.y;
        // gridColumnsFor is NaN- and zero-safe and NEVER returns 0, which is what keeps the division
        // below from being a division by zero (3.1.3 fixed a real +inf -> int abort there).
        const int columns = gridColumnsFor(ImGui::GetContentRegionAvail().x, tileW, ImGui::GetStyle().ItemSpacing.x);
        const int items = 1 + static_cast<int>(state.candidates.items.size());  // None is index 0
        const int rows = (items + columns - 1) / columns;
        const std::span<const AssetRecord> records =
            in.database != nullptr ? in.database->records() : std::span<const AssetRecord>{};

        ImGuiListClipper clipper;
        clipper.Begin(rows, rowHeight);
        if (scrollToCursor) {
            // AFTER Begin() and BEFORE the first Step(), which is what ImGui's own assertion requires:
            // the clipper's constructor memsets DisplayStart to 0 and Begin() is what sets it to -1,
            // so `IM_ASSERT(DisplayStart < 0)` (imgui.cpp:3418) ABORTS on a call placed above Begin --
            // and TempData is null there too. Without this call the cursor's row can be clipped away
            // entirely and SetScrollHereY never runs, so a keyboard move past the visible rows would
            // scroll nothing at all.
            clipper.IncludeItemByIndex(static_cast<int>(state.cursor) / columns);
        }
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                for (int column = 0; column < columns; ++column) {
                    const int index = (row * columns) + column;
                    if (index >= items) {
                        break;  // BEFORE the PushID, exactly as drawContentsGrid does
                    }
                    if (column > 0) {
                        ImGui::SameLine();
                    }
                    // PushID/PopID are 1:1 with NO continue, break or return between them -- an
                    // unbalanced id stack is an IM_ASSERT abort in the Debug ImGui build.
                    ImGui::PushID(index);
                    // THE TILE'S ORIGIN, captured BEFORE the Selectable -- drawTile's own shape
                    // (asset_browser_panel.cpp:634). GetItemRectMin() afterwards is a DIFFERENT point:
                    // a Selectable extends its item box on the MIN side by half the item spacing
                    // (imgui_widgets.cpp:7395-7396), so every face would be drawn up and to the left of
                    // the tile it belongs to by (ItemSpacing.x/2, ItemSpacing.y/2).
                    const ImVec2 itemMin = ImGui::GetCursorScreenPos();
                    const bool selected = static_cast<std::size_t>(index) == state.cursor;
                    if (ImGui::Selectable("##tile", selected, ImGuiSelectableFlags_None, ImVec2(tileW, tileH))) {
                        state.cursor = static_cast<std::size_t>(index);
                        commitRequested = true;  // ONE commit path, below
                    }
                    if (selected && scrollToCursor) {
                        ImGui::SetScrollHereY();
                    }
                    AssetTileFace face{};
                    face.tileW = tileW;
                    face.tileEdge = tileEdge;
                    face.pad = pad;
                    if (index == 0) {
                        face.fileName = "None";
                        face.captionSource = "None";
                        state.tooltipScratch = "Clear this reference";
                    } else {
                        state.tooltipScratch.clear();
                        const AssetPickerCandidate& candidate =
                            state.candidates.items[static_cast<std::size_t>(index) - 1];
                        face.kind = candidate.kind;
                        if (candidate.recordIndex < records.size()) {
                            const AssetRecord& record = records[candidate.recordIndex];
                            face.fileName = leafOf(record.relativePath);
                            face.captionSource = face.fileName;
                            state.tooltipScratch = record.relativePath;  // the FULL path the caption elides
                            if (in.thumbnails != nullptr) {
                                if (const std::optional<ThumbnailKey> key = thumbnailKeyForRecord(record);
                                    key.has_value()) {
                                    in.thumbnails->noteVisible(*key);
                                    face.nativeTexture = in.thumbnails->nativeTextureFor(*key);
                                    // task E.4.5: the grid tile's rule, verbatim -- the card rides the same
                                    // key on its own queue, and the face views its name for this draw walk
                                    // (the card is stable until the service pass). NO AssetKind literal here:
                                    // I166(c) pins that this widget states none.
                                    in.thumbnails->noteCardWanted(*key);
                                    const MaterialCard* const card = in.thumbnails->cardFor(*key);
                                    face.subtitle = materialCardSubtitle(card, face.fileName);
                                    face.tint = materialCardTint(card);
                                }
                            }
                        }
                    }
                    // The face submits no item, so the Selectable above is still the last item here.
                    drawAssetTileFace(ImGui::GetWindowDrawList(), itemMin, face, state.tileScratch);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", state.tooltipScratch.c_str());
                    }
                    ImGui::PopID();
                }
            }
        }
        // The truncation notice line is ALWAYS drawn, empty when nothing was truncated, so the popup's
        // size -- and therefore its anchor -- cannot change because a notice appeared.
        ImGui::TextDisabled("%s", assetPickerTruncationNotice(state.candidates).c_str());
        ImGui::EndChild();  // UNCONDITIONAL -- BeginChild/EndChild are 1:1
        state.cursorMovedByKeyboard = false;

        // THE ONE COMMIT PATH. A tile press has ALREADY auto-closed the popup by the time this runs
        // (imgui_widgets.cpp:7534-7535); the second CloseCurrentPopup() is a no-op, because the top of
        // the begin stack no longer matches the open-stack entry (imgui.cpp:13116). That idempotence
        // is why one path can serve both the mouse and the keyboard, and I166(b) pins
        // NoAutoClosePopups ABSENT so nobody "fixes" the click-to-commit close away.
        if (enter || enterKey || seamCommit || commitRequested) {
            const std::optional<Guid> picked = pickerCursorGuid(state.cursor, state.candidates);
            if (!picked.has_value()) {
                result = AssetFieldResult{.outcome = AssetFieldOutcome::Cleared, .guid = Guid{}};
            } else if (in.database != nullptr && in.database->findByGuid(*picked) == nullptr) {
                // 3.1.5's D13: a record can vanish mid-gesture. The candidate is a HINT; the database
                // is the authority, and it is asked again HERE, at commit.
                AERO_LOG_WARN("assets: the picked asset is no longer in this project");
            } else {
                result = AssetFieldResult{.outcome = AssetFieldOutcome::Picked, .guid = *picked};
            }
            ImGui::CloseCurrentPopup();
        } else if (escape || seamClose) {
            ImGui::CloseCurrentPopup();
        }

        // THE UNKNOWN-TOKEN WARN: once per OPEN, never per frame. An unresolved token is UNCONSTRAINED
        // -- never a refused field, never a disabled row -- plus this one line.
        if (!in.unknownToken.empty() && !state.warnedThisOpen) {
            state.warnedThisOpen = true;
            AERO_LOG_WARN("inspector: field '{}' names an unknown asset kind '{}' -- treated as any asset", in.fieldKey,
                          in.unknownToken);
        }
        ImGui::EndPopup();  // ASYMMETRIC: only because BeginPopup returned true
    }

    // THE OWNER-ONLY OBSERVABLES. A field that owns nothing writes NOTHING -- see rule 3 in the banner.
    // The one-frame latency is the design, not a lag: CloseCurrentPopup runs INSIDE the body, so on the
    // closing frame the popup DID draw and this still reads true; it reads false the frame after.
    if (open || owner) {
        // SEEN. The post-draw slot resets every observable in any tick this stays false, which is what
        // makes assetPickerOpen() the LAST DRAWN frame's answer even when the owning row stopped being
        // drawn entirely. It is set for a NON-open owner too, so the closing frame's own `else` arm
        // below is not mistaken for "nobody drew".
        state.ownerDrewThisTick = true;
    }
    if (open) {
        state.openValue = true;
        state.candidateCountValue = state.candidates.items.size();
        state.cursorValue = state.cursor;
    } else if (owner) {
        state.openValue = false;  // the frame AFTER the close
        state.openHostId.clear();
        state.openFieldKey.clear();
    }
    return result;
}

}  // namespace engine::editor

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

AssetFieldResult drawAssetReferenceField(const AssetFieldInputs& in, AssetPickerState& state) {
    AssetFieldResult result{};
    const std::vector<AssetKind> options = assetPickerFilterOptions(in.rules);

    // 1. THE VALUE BUTTON. The label IS the value sentence, so the id must be the ### suffix alone --
    //    otherwise the id changes when the bound asset changes, which would orphan an open popup.
    state.labelScratch.assign(in.valueText);
    state.labelScratch += in.idSuffix;
    const bool pressed = ImGui::Button(state.labelScratch.c_str(), ImVec2(in.buttonWidth, 0.0F));

    // 2. THE BUTTON'S GEOMETRY, captured BEFORE the popup is touched. End() restores the parent's
    //    last-item data at EndPopup (imgui.cpp:8848), so reading these afterwards names the button
    //    again today -- the capture is what survives a bump that stops restoring, or a stray item
    //    submitted between here and the anchor.
    const ImVec2 buttonMin = ImGui::GetItemRectMin();
    const ImVec2 buttonMax = ImGui::GetItemRectMax();

    // 3. THE DROP TARGET, on the button. PEEK -> CLASSIFY -> ACCEPT, in that order, always: an illegal
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

    // 4. THE SEAM'S OPEN ARM -- consumed ONLY by the field it names, so a request for a field that is
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

    // 5. THE FOUR LIVE ONE-SHOTS ARE DROPPED WHEN NOTHING IS OPEN AT ALL. Without this they are not
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

    // 6. THE ANCHOR AND THE SIZE, computed BEFORE BeginPopup so the arithmetic is exact rather than one
    //    frame late. Both SetNext* calls are legal while the popup is closed: BeginPopup clears
    //    NextWindowData on its early-out (imgui.cpp:13196-13199). SetNextWindowSize overrides the
    //    AlwaysAutoResize flag BeginPopup adds (imgui.cpp:8182, :13201), which is what makes a fixed
    //    size legal on a popup at all -- and an API-SET POSITION IS THE ONE ImGui NEVER CLAMPS
    //    (:8279), which is why the anchor does the clamping itself.
    const ImGuiStyle& style = ImGui::GetStyle();
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
                    const bool selected = static_cast<std::size_t>(index) == state.cursor;
                    if (ImGui::Selectable("##tile", selected, ImGuiSelectableFlags_None, ImVec2(tileW, tileH))) {
                        state.cursor = static_cast<std::size_t>(index);
                        commitRequested = true;  // ONE commit path, below
                    }
                    if (selected && scrollToCursor) {
                        ImGui::SetScrollHereY();
                    }
                    const ImVec2 itemMin = ImGui::GetItemRectMin();
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

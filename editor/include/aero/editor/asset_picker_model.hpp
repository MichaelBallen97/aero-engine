#pragma once
// Aero Engine -- everything the asset-reference picker DECIDES (task E.3.3, D11). PUBLIC, PURE,
// ImGui-free, entt-free, GPU-free, logging-free: every function below is provable from a
// std::vector<AssetRecord> literal with no window and no context of any kind, which is what lets a
// tier-0 case drive the whole widget's behaviour while the src-private widget holds only the glue.
//
// THE WIDGET STATES NO PREDICATE OF ITS OWN. What a field accepts is the DROP MATRIX's answer
// (asset_drag.hpp), asked once, for both the button's drop target and the popup's candidate list --
// so a drop and a pick can never disagree.
#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>          // Vec2 -- the anchor's arithmetic
#include <aero/editor/asset_drag.hpp>  // DropSurface, DropAction, classifyAssetDrop
#include <aero/editor/asset_meta.hpp>  // AssetRecord
#include <aero/editor/asset_view.hpp>  // AssetKind, AssetFilter, matchesFilter, MAX_SEARCH_RESULTS

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// WHAT A FIELD ACCEPTS -- and it is the drop matrix's own answer (D3). A material slot is
// {MaterialSlot, nullopt}; an inspector Guid row is {AssetField, <its AERO_ASSET kind, or nullopt>}.
struct AssetPickerRules {
    DropSurface surface = DropSurface::AssetField;
    std::optional<AssetKind> fieldKind;
};

// == classifyAssetDrop(kind, rules.surface, /*targetHasMeshRenderer=*/false, rules.fieldKind) != None.
// THE ONE PREDICATE the button's drop target and the popup's candidate walk both ask. MP14 pins the
// identity over every kind and every rules value, so this can never grow a predicate of its own.
[[nodiscard]] bool assetPickerAccepts(const AssetPickerRules& rules, AssetKind kind) noexcept;

// The accepted kinds, in ASSET_KIND_FILTER_OPTIONS' order. size() == 1 => a heading and NO combo;
// size() > 1 => a combo with "All" first.
[[nodiscard]] std::vector<AssetKind> assetPickerFilterOptions(const AssetPickerRules& rules);

// assetKindLabel VERBATIM for one kind ("Texture"/"Model"/"Audio"/"Material" -- never pluralised, the
// labels are singular), "Any asset" for several, "" for none.
[[nodiscard]] std::string assetPickerHeading(const AssetPickerRules& rules);

struct AssetPickerCandidate {
    Guid guid;
    std::size_t recordIndex = 0;  // into the span the walk was given -- SearchHit's own shape
    AssetKind kind = AssetKind::Unknown;
};
struct AssetPickerCandidates {
    std::vector<AssetPickerCandidate> items;  // in records()' own order: byte-lexicographic by path
    std::size_t total = 0;                    // UNCAPPED match count (the SearchResult rule)
    bool truncated = false;
};
// A record is offered iff its guid is VALID, assetPickerAccepts(rules, its kind), AND
// matchesFilter(leafOf(path), false, filter) -- the browser's own text-and-kind predicate, COMPOSED,
// never restated. Both must hold: `filter` is the popup's combo, `rules` is the field's law.
// Capped at `cap`; `total` is never capped. Scratch-reusing (the RenderViewScratch rule): `out.items`
// is cleared, never shrunk.
void buildAssetPickerCandidates(std::span<const AssetRecord> records, const AssetPickerRules& rules,
                                const AssetFilter& filter, std::size_t cap, AssetPickerCandidates& out);

// THE CURSOR is LINEAR over [None, items[0], items[1], ...]: index 0 is None, index i+1 is items[i].
// Two keys are therefore enough for the whole grid, and the reducer needs no notion of columns --
// which is what keeps the layout (columns, tile size, the clipper) out of the model entirely.
inline constexpr std::size_t ASSET_PICKER_NONE_INDEX = 0;
enum class AssetPickerMove : std::uint8_t { Prev = 0, Next, First, Last };
// TOTAL and CLAMPING: Prev at 0 stays 0, Next at the end stays there, an out-of-range `current` is
// clamped FIRST, and candidateCount == 0 makes every move land on 0.
[[nodiscard]] std::size_t movePickerCursor(std::size_t current, AssetPickerMove move,
                                           std::size_t candidateCount) noexcept;
// Where a fresh open puts the cursor: the bound guid's item + 1 when it is a candidate, else None.
[[nodiscard]] std::size_t initialPickerCursor(Guid current, const AssetPickerCandidates& candidates) noexcept;
// What committing the cursor MEANS: nullopt == None, else the candidate's guid. Past the end: nullopt.
[[nodiscard]] std::optional<Guid> pickerCursorGuid(std::size_t cursor,
                                                   const AssetPickerCandidates& candidates) noexcept;

// THE ANCHOR (D7): below the button, top-left at (buttonMin.x, buttonMax.y); ABOVE it, with a (0,1)
// pivot at (buttonMin.x, buttonMin.y), when the popup would cross workMax.y AND there is room above;
// x shifted left so the right edge stays inside workMax.x, then floored at workMin.x so a popup wider
// than the work area sits at the left edge rather than at a negative x.
//
// IT EXISTS BECAUSE AN API-SET POSITION IS THE ONE ImGui NEVER CLAMPS: Begin's visibility clamp runs
// only `if (!window_pos_set_by_api && !(flags & ChildWindow))` (imgui.cpp:8279), so a dropdown
// anchored under a button near the bottom of a tall Inspector would run off the screen.
//
// THE NON-FINITE CONTRACT, STATED EXACTLY, BECAUSE IT IS NARROWER THAN "NaN-safe" WOULD SUGGEST. If
// ANY of the ten input components is non-finite, the below-anchor is returned with NO correction
// applied at all -- one predictable answer instead of a mixture of clamped and unclamped components.
// IT IS NOT A SANITISER AND CANNOT BE ONE: the below-anchor is literally (buttonMin.x, buttonMax.y),
// so a non-finite BUTTON RECT comes straight back out, non-finite. Nothing here can invent a sane
// rect, and a non-finite button rect is an ImGui-level impossibility -- the caller reads it from
// GetItemRectMin/Max of an item ImGui has just laid out.
//
// What the guard DOES buy is that the corrections can never manufacture a wrong-looking FINITE answer
// out of a garbage bound, and immunity to the corrections themselves: std::clamp(NaN, lo, hi) returns
// NaN on libc++ (3.7.2's standing rule), so folding the two std::min/std::max pairs into std::clamp
// would turn a finite `pos` into a non-finite window position -- an ImGui assertion -- the moment any
// OTHER input went non-finite. MP13 proves the guard is reached at all (a non-finite buttonMin.y
// leaves pos.x at 900 instead of shifting it to 800) and the sabotage matrix's S23b proves the
// std::clamp half.
struct AssetPickerAnchor {
    Vec2 pos;
    Vec2 pivot;
    bool above = false;
};
[[nodiscard]] AssetPickerAnchor assetPickerAnchor(Vec2 buttonMin, Vec2 buttonMax, Vec2 popupSize, Vec2 workMin,
                                                  Vec2 workMax) noexcept;

// THE GEOMETRY, in FONT UNITS, as a pure function -- so the widget holds no size arithmetic and a
// tier-0 case can assert the popup is at least as tall as its parts. Shipped as STARTING VALUES
// (D-note): a change is one constant and one re-measure, and the manual validation page is the only
// instrument that can judge "is it too narrow to read a caption".
inline constexpr float ASSET_PICKER_MIN_WIDTH_FONT = 22.0F;
inline constexpr std::size_t ASSET_PICKER_GRID_ROWS_VISIBLE = 3;
struct AssetPickerMetrics {
    float fontSize = 0.0F;
    float frameHeight = 0.0F;
    float textLineHeight = 0.0F;
    float itemSpacingY = 0.0F;
    float windowPadding = 0.0F;
    float buttonWidth = 0.0F;
};
struct AssetPickerLayout {
    Vec2 popupSize;           // what SetNextWindowSize is given
    float gridHeight = 0.0F;  // the child's height: ASSET_PICKER_GRID_ROWS_VISIBLE tile rows
};
// width  = max(buttonWidth, ASSET_PICKER_MIN_WIDTH_FONT * fontSize)
// height = windowPadding*2 + heading + search + (combo when present) + gridHeight + the NOTICE LINE,
//          which is ALWAYS reserved so the popup does not resize -- and the anchor does not flip --
//          when a truncation notice appears.
// A non-finite metric yields a finite, POSITIVE layout: a NaN window size is an ImGui assertion.
[[nodiscard]] AssetPickerLayout assetPickerLayout(const AssetPickerMetrics& metrics, bool withCombo) noexcept;

// "N more not shown -- refine the search" for a truncated walk; "" otherwise. ASCII only (the
// standing rule: the UI font draws '?' past a small fixed set -- .claude/rules/editor.md, "The UI font").
[[nodiscard]] std::string assetPickerTruncationNotice(const AssetPickerCandidates& candidates);

// THE SEAM KEYS, spelled ONCE (task E.3.3), and the reason is that ComponentEntry::name is the FULL
// registration name (inspector_model.hpp) -- so a hand-written "MeshRenderer.mesh" would never match
// the "engine::MeshRenderer.mesh" the Inspector's own arm builds.
[[nodiscard]] std::string inspectorAssetFieldKey(std::string_view component, std::string_view field);
[[nodiscard]] std::string materialSlotFieldKey(std::size_t slot);  // "slot:<index>"

}  // namespace engine::editor

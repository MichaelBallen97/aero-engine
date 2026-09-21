#pragma once
// SRC-PRIVATE (task E.3.3, D1). The ONE asset-reference field widget, and the ONLY new ImGui TU this
// task adds. It holds NO DECISION: every predicate is asset_picker_model's, and I166(c) pins that
// asset_picker.cpp states no `AssetKind::` literal at all -- the kinds come from the rules, which come
// from the field's annotation, which comes from the component. That is the mechanical form of
// ADR-004's genericity claim for this widget.
//
// It NAMES NO ImGui TYPE, which is what lets inspector_panel.hpp and material_panel.hpp hold an
// AssetPickerState* without either of them seeing <imgui.h> through this header.
#include <aero/core/guid.hpp>
#include <aero/editor/asset_picker_model.hpp>
#include <aero/editor/asset_view.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

class AssetDatabase;     // forward-declared: a pointer in the inputs aggregate
class ThumbnailService;  // ditto -- src-private (thumbnail_service.hpp)

// A SEAM one-shot: "open on THIS field", matched by host AND key, so a request for a field that is not
// drawn this frame stays pending rather than firing on whichever reference row happens to draw next.
struct AssetPickerOpenRequest {
    std::string hostId;
    std::string fieldKey;
};

struct AssetPickerState {
    // ---- session state, reset on every open (D12) ----
    std::string search;
    AssetFilter filter;  // `query` mirrors `search`; kind/anyKind is the combo
    std::size_t cursor = ASSET_PICKER_NONE_INDEX;
    bool cursorMovedByKeyboard = false;  // SetScrollHereY + IncludeItemByIndex once, then cleared
    AssetPickerCandidates candidates;    // scratch-reusing
    std::uint64_t builtForGeneration = 0;
    std::string builtForQuery;
    std::optional<AssetKind> builtForKind;
    bool builtForAnyKind = true;
    bool warnedThisOpen = false;  // the unknown-token WARN: once per OPEN, never per frame

    // WHICH FIELD OWNS THE OPEN POPUP ("" == none). The observables and the four live one-shots are
    // OWNER-ONLY, and that is not tidiness: MeshRenderer draws TWO Guid rows every frame (`mesh` then
    // `material`), so a per-field write would have the `material` row clobber the `mesh` row's answer
    // in the SAME frame and assetPickerOpen() would read false while the popup is open.
    std::string openHostId;
    std::string openFieldKey;

    // Per-frame scratch the tile face clobbers, and the tooltip's own. MEMBERS rather than locals for
    // the 2.2.1 labelScratch reason: a popup over a thousand records must not allocate twice per tile
    // per frame. Neither is model state and neither is read outside the draw walk.
    std::string tileScratch;
    std::string tooltipScratch;

    // ---- seams (I138's shape) ----
    std::optional<AssetPickerOpenRequest> pendingOpen;
    std::optional<std::string> pendingSearch;
    std::optional<AssetPickerMove> pendingMove;
    std::size_t pendingMoveSteps = 1;  // |delta| reductions applied in ONE frame, as holding a key does
    bool pendingCommit = false;
    bool pendingClose = false;

    // ---- observables: the LAST DRAWN frame's answers, never live reads ----
    bool openValue = false;
    std::size_t candidateCountValue = 0;
    std::size_t cursorValue = 0;

    // THE OWNER'S SEEN-THIS-TICK STAMP, set by the widget and CONSUMED once per tick by EditorApp's
    // post-draw slot. Without it a tick in which the owning row did not draw at all -- the selection
    // moved to an entity without that component, or the panel was tabbed away -- leaves every
    // observable frozen at the last frame that DID draw, so assetPickerOpen() keeps reporting an open
    // popup ImGui has already closed and step 6's drop of the four live one-shots never runs again.
    bool ownerDrewThisTick = false;
};

enum class AssetFieldOutcome : std::uint8_t { None = 0, Picked, Cleared, Dropped };
struct AssetFieldResult {
    AssetFieldOutcome outcome = AssetFieldOutcome::None;
    Guid guid;
};

// Everything the widget needs, as ONE aggregate -- eleven parameters is where a signature stops being
// readable and starts being a place to transpose two strings, and .clang-tidy disables
// bugprone-easily-swappable-parameters, so nothing would have caught it.
struct AssetFieldInputs {
    // The button's WHOLE label, and `###` is REQUIRED: everything before the `###` is what ImGui would
    // render, so a suffix without it would draw a second copy of the sentence the widget draws itself,
    // and an id computed from anything the bound asset can change orphans an open popup.
    const char* idSuffix = "###ref";
    std::string_view valueText;  // guidFieldRow's sentence, or the slot's -- drawn on the DRAW LIST
    float buttonWidth = 0.0F;    // the HOST's arithmetic (-FLT_MIN fills the cell)
    Guid current;                // the bound guid (nil == none) -- where a fresh open lands
    AssetPickerRules rules;
    std::string_view hostId;                  // Panel::id() -- "Inspector" / "Material"
    std::string_view fieldKey;                // inspectorAssetFieldKey(...) or materialSlotFieldKey(...)
    std::string_view unknownToken;            // non-empty => WARN once on the open frame, naming fieldKey
    const AssetDatabase* database = nullptr;  // null is legal: no candidates, and the button still draws
    ThumbnailService* thumbnails = nullptr;   // null is legal: every tile draws its kind icon
    // task E.3.4, APPENDED LAST so no existing designated initialiser moves -- a designator must
    // follow DECLARATION order in C++20, and clang accepts a wrong order with -Wreorder-init-list (a
    // WARNING) while GCC and MSVC REJECT (E.2.2's finding 3). 0 == no thumbnail, which is the
    // Inspector's row and is TODAY's behaviour exactly: the button's height argument stays the 0.0F
    // it already is.
    float thumbnailEdge = 0.0F;
};

// Draws [button][popup] inside the CALLER's PushID scope and reports what happened. The host decides
// what a pick MEANS -- this names no World, no CommandStack, no MaterialDocument and no Panel.
[[nodiscard]] AssetFieldResult drawAssetReferenceField(const AssetFieldInputs& in, AssetPickerState& state);

// The reference button's width when a trailing SmallButton shares its row -- the Inspector's `Clear`
// and, since task E.3.4, the material slot's. ONE formula, TWO hosts: a theme or DPI change moves both
// together and neither host restates it. Floored at one frame height so a narrow panel still yields a
// clickable button rather than a negative one.
//
// NOT SetNextItemWidth: ImGui::Button takes an explicit ImVec2 size and IGNORES the next-item width,
// and the Inspector's shared SetNextItemWidth(-1.0F) is consumed by the first ItemAdd that follows
// (E.3.1's lesson 1) -- which is why the width had to become an argument at all.
//
// It reads the LIVE style and the CURRENT content region, so it must be called inside the draw walk,
// on the row it is sizing. `float` in and `float` out, so this header still names no ImGui type.
//
// NEITHER FILE STATES THE LITERAL, and this sentence used to say the opposite -- corrected by the
// code-review round. The label arrives as a PARAMETER and the body calls
// CalcTextSize(trailingButtonLabel), so `CalcTextSize("Clear")` reads ZERO in inspector_panel.cpp and
// ZERO in asset_picker.cpp; what I172(e) pins at exactly one apiece is the CALL,
// `assetReferenceFieldWidth("Clear")`, in inspector_panel.cpp and in material_panel.cpp. A second
// trailing-button host adds a third such call and states no literal either -- writing
// CalcTextSize("Clear") into this file on the strength of the old sentence would redden I172(e) for a
// reason that sentence declared legal.
[[nodiscard]] float assetReferenceFieldWidth(const char* trailingButtonLabel);

}  // namespace engine::editor

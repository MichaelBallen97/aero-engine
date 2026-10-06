#pragma once
// Aero Engine -- the reflection-driven Inspector panel (task 2.2.2). SRC-PRIVATE and the ONLY new
// ImGui TU this task adds: this header itself is ImGui-free (registered by editor_app.cpp, which
// never sees ImGui), but its .cpp is where every ImGui call lives.
//
// FRAME SHAPE -- onDraw is exactly FOUR phases, in this order:
//   1. reconcile  drop any edit-cache entry whose target no longer resolves (E3)
//   2. build      buildInspectorModel(context.world, primary, model) -- D15 scratch, rebuilt fresh
//   3. draw       walk the model, drawing widgets; a VALUE edit writes through the seam IMMEDIATELY
//                 (safe mid-draw -- it touches no storage structure, D9); Add/Remove are recorded
//                 into `pending`, never applied here
//   4. apply      one switch over `pending` -- the ONLY place a component is added or removed
// No walk here is recursive (F22) -- the model is two flat vectors, never a tree.
//
// task E.3.1: phase 3 measures ONE label-column width over the WHOLE model before the component loop
// and hands it to every component's two-column table, so the panel reads as one column rather than as
// N. Every field kind draws inside that table; nothing draws with SameLine arithmetic any more.
//
// task E.3.3: phase 3's Guid arm draws the picker. A pick is a DISCRETE write through resetField --
// it must never merge with a drag on the same field a moment earlier -- and this file still names no
// ImGui drop API at all, because the target belongs to the widget.
#include <aero/editor/component_ops.hpp>
#include <aero/editor/inspector_model.hpp>
#include <aero/editor/panel.hpp>
#include <aero/scene/entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

// Forward-declared, never #included here: this header stays ImGui-free BY FILE PLACEMENT, and
// asset_picker.hpp is src-private. `AssetPickerState` is a STRUCT -- declared with the matching tag so
// clang does not warn under -Wmismatched-tags.
struct AssetPickerState;
class ThumbnailService;

class InspectorPanel final : public Panel {
public:
    // D20: the id is the ImGui window name AND the imgui.ini settings key. It stays "Inspector" --
    // renaming it orphans every existing user's saved layout for this panel (F16).
    [[nodiscard]] const char* id() const noexcept override { return "Inspector"; }
    [[nodiscard]] DockSlot defaultDockSlot() const noexcept override { return DockSlot::Right; }
    void onDraw(PanelContext& context) override;

    // task 3.1.5: a reconciled POINTER, never a reference member -- MaterialPanel's exact shape and
    // 3.1.1's D13/INV-4 rule: EditorApp is movable, so a reference would bind to a pre-move address.
    // EditorApp sets it every tick, from the same reconcile statement that already sets the Material
    // panel's. NULL is a legal state and the Guid row has a sentence for it.
    void setDatabase(const AssetDatabase* db) noexcept { database = db; }

    // task E.3.3: both set ONCE in EditorApp::create (the viewportPanel posture), not reconciled --
    // each points at a heap object EditorApp holds through a unique_ptr, so the address survives the
    // app's own move. NULL is a legal state: the Guid arm falls back to 3.1.5's read-only row.
    void setAssetPicker(AssetPickerState* s) noexcept { assetPicker = s; }
    void setThumbnails(ThumbnailService* s) noexcept { thumbnails = s; }

    // ---- task E.5.2: the named selector's seam and its one observable ----------------------------
    // The SEAM: writes a one-shot the panel hands to its NEXT onDraw (moved into frameNamedSelection as
    // that call's FIRST statement, before any early return) and applies at the matching row by assigning
    // the SAME local a Selectable click assigns. `component` is the FULL registration name
    // ("engine::MeshRenderer"), the inspectorAssetFieldKey inputs, compared exactly. The request lives
    // ONE TICK: EditorApp calls expireNamedSelection() after drawShellUi, so a tick that did not draw the
    // row -- the panel hidden, another tab in front, another entity selected -- drops it, and it can never
    // land on a later frame or on another entity (E.3.3's rule for live one-shots).
    void requestNamedSelection(std::string component, std::string field, std::size_t index);
    void expireNamedSelection() noexcept {
        pendingNamedSelection.reset();
        frameNamedSelection.reset();
    }
    // Cumulative count of named-selector rows the panel SUBMITTED (one per BeginCombo call). A seam's own
    // accessor is a round trip; this is a consequence the widget produced (E.3.4's materialSamplerRowsDrawn
    // lesson), and it is what tells a selector from a drag.
    [[nodiscard]] std::size_t namedSelectorsDrawn() const noexcept { return namedSelectorsDrawnValue; }
    // task E.6.1: what ImGui had CURRENT when this panel drew -- the face of the last component header and of
    // the last field label (copies of GetFont()->GetDebugName(), taken at the draw), and how many headers
    // this frame submitted. Empty / 0 until one draws.
    [[nodiscard]] const std::string& lastHeaderFontName() const noexcept { return lastHeaderFontNameValue; }
    [[nodiscard]] const std::string& lastLabelFontName() const noexcept { return lastLabelFontNameValue; }
    [[nodiscard]] std::uint32_t headersSubmitted() const noexcept { return headersSubmittedValue; }
    // task E.6.1 (I289): the axis rows as ImGui LAID THEM OUT on the last onDraw -- the panel's content
    // width (GetContentRegionAvail at the top of onDraw), the last axis box's width (GetItemRectSize after
    // its DragScalar), and the last axis row's slack: its value cell's width minus the row's own rect
    // (EndGroup's), negative when the row overruns the cell. The box and the slack are 0 on a frame that
    // drew no axis row.
    [[nodiscard]] float lastContentWidth() const noexcept { return lastContentWidthValue; }
    [[nodiscard]] float lastAxisBoxWidth() const noexcept { return lastAxisBoxWidthValue; }
    [[nodiscard]] float lastAxisRowSlack() const noexcept { return lastAxisRowSlackValue; }

private:
    enum class ActionKind : std::uint8_t { None = 0, AddComponent, RemoveComponent };
    struct PendingAction {
        ActionKind kind = ActionKind::None;
        ComponentTypeId type{};
    };

    // At most one widget is active at a time, so ONE slot each. Keyed on (entity, type, field) so
    // reconcile can drop a cache whose target no longer resolves (E3).
    struct EditKey {
        Entity entity{};
        ComponentTypeId type{};
        std::string field;
        bool active = false;

        [[nodiscard]] bool matches(Entity e, ComponentTypeId t, std::string_view f) const noexcept {
            return entity == e && type == t && field == f;
        }
    };
    struct QuatEditCache : EditKey {
        Vec3 eulerDegrees;
    };
    struct StringEditCache : EditKey {
        std::string buffer;
    };
    // task E.5.2: one requested pick. Plain values, compared exactly.
    struct NamedSelection {
        std::string componentName;
        std::string fieldName;
        std::size_t index = 0;
        [[nodiscard]] bool matches(std::string_view component, std::string_view field) const noexcept {
            return componentName == component && fieldName == field;
        }
    };

    void drawComponent(PanelContext& context, Entity primary, const ComponentEntry& entry, float labelWidth);
    void drawField(PanelContext& context, Entity primary, const ComponentEntry& entry, const FieldEntry& field);
    // task E.3.1: what DragScalarN does internally, opened up -- three DragScalars with a coloured
    // axis letter before each, inside ONE BeginGroup/EndGroup so a single gateForLastItem() read
    // after it sees the WHOLE triplet's edges, exactly as it did after DragFloat3.
    bool drawAxisRow(PanelContext& context, Entity primary, const ComponentEntry& entry, const FieldEntry& field,
                     std::array<float, 3>& shown, float speed);
    void drawFieldResetMenu(PanelContext& context, Entity primary,
                            const ComponentEntry& entry,      // the component the row belongs to
                            const FieldEntry& field,          // the row, by kind, colour flag and value
                            std::optional<std::size_t> axis,  // nullopt == the whole field (label cell)
                            const char* strId);               // nullptr on an axis: the drag's own id
    // A reset is a VALUE edit, so it writes through the seam inline like every other arm -- it must
    // NOT go through `pending`, which is for Add/Remove only (see the four-phase note above).
    void resetField(PanelContext& context, Entity primary, const ComponentEntry& entry, const FieldEntry& field,
                    FieldValue after);
    // task E.5.2: the Int/UInt arms' other shape. A pick is a DISCRETE write through resetField.
    void drawNamedSelector(PanelContext& context, Entity primary, const ComponentEntry& entry, const FieldEntry& field,
                           const NamedSelectorRow& selector);
    void applyPending(PanelContext& context, Entity primary);

    InspectorModel model;  // D15 scratch, rebuilt every frame
    PendingAction pending{};
    QuatEditCache quatCache;
    StringEditCache stringCache;
    std::string labelScratch;
    std::string shortNameScratch;
    const AssetDatabase* database = nullptr;  // task 3.1.5, reconciled -- see setDatabase above
    // task E.3.3: see setAssetPicker/setThumbnails above. Borrowed, never owned, set once.
    AssetPickerState* assetPicker = nullptr;
    ThumbnailService* thumbnails = nullptr;
    std::string fieldKeyScratch;  // labelScratch's idiom -- no per-frame allocation once warm

    std::optional<NamedSelection> pendingNamedSelection;  // task E.5.2 -- the seam writes this
    std::optional<NamedSelection> frameNamedSelection;    // task E.5.2 -- THIS onDraw's copy
    std::string lastHeaderFontNameValue;                  // task E.6.1 -- member/accessor collision rule
    std::string lastLabelFontNameValue;                   // likewise
    std::uint32_t headersSubmittedValue = 0;              // likewise; reset at the top of onDraw
    float lastContentWidthValue = 0.0F;                   // task E.6.1 (I289) -- written at the top of onDraw
    float lastAxisBoxWidthValue = 0.0F;                   // likewise; reset at the top of onDraw
    float lastAxisRowSlackValue = 0.0F;                   // likewise; reset at the top of onDraw
    std::size_t namedSelectorsDrawnValue = 0;             // task E.5.2 -- member/accessor collision rule
};

}  // namespace engine::editor

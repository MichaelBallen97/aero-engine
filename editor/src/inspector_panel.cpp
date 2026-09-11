#include "inspector_panel.hpp"

#include <aero/core/math.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/component_commands.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/panel_context.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/world.hpp>

#include "text_input.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace engine::editor {

namespace {

// task E.3.1. THE ABSENCE OF ImGuiTableFlags_Resizable IS LOAD-BEARING AND INVISIBLE, which is why
// I145 pins it as source text. Without Resizable, imgui_tables.cpp:809-810 stamps NoResize onto every
// column, and the chain that then re-applies our requested width EVERY FRAME is:
//   :1692      InitStretchWeightOrWidth = init_width_or_weight  (unconditional, one line ABOVE the
//              IsInitializing gate -- so the value we pass to TableSetupColumn survives every frame)
//   :934-938   column->WidthAuto = InitStretchWeightOrWidth     (WidthFixed && !resizable)
//   :988-989   column->WidthRequest = width_auto                (WidthFixed && !resizable &&
//              IsRequestOutput) -- the line that actually SIZES the column, and without which the
//              whole mechanism is inert with every test still green
// :988's third condition, IsRequestOutput, is structurally true for column 0: :1239-1242 forces it on
// table->LeftMostEnabledColumn whenever no column asked for output, and on a table's very first frame
// AutoFitQueue != 0 takes the :987 arm instead. Both branches are covered.
//
// project_settings_panel.cpp takes the OPPOSITE choice for its own stated reasons. Do not unify them.
constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;

// The short display name for a CollapsingHeader label ("Transform" from "engine::Transform"); the
// full registration name is shown as an IsItemHovered tooltip instead (D13/E14).
std::string_view shortComponentName(std::string_view fullName) {
    const std::size_t pos = fullName.rfind("::");
    return pos == std::string_view::npos ? fullName : fullName.substr(pos + 2);
}

// Review finding 2: clamp `v` into T's representable domain BEFORE casting -- a raw
// static_cast<uint64_t>(-1.0) or static_cast<int64_t>(1e300) is UNDEFINED BEHAVIOUR ([conv.fpint]),
// and field.rangeMin/rangeMax are ARBITRARY annotation-authored doubles (a negative bound on an
// unsigned field is legitimate syntax -- see component_annotations.hpp's negativeUnsigned), evaluated
// unconditionally below regardless of field.hasRange. The mirror of meta_utils.cpp's
// doubleToClamped -- duplicated rather than shared, since meta_utils.hpp pulls in <entt/entt.hpp> and
// this ImGui TU must stay entt-free by file placement (D1).
template <typename T>
T doubleToClamped(double v) {
    if (v != v) {  // NaN
        return T{0};
    }
    const auto lo = static_cast<double>(std::numeric_limits<T>::lowest());
    const auto hi = static_cast<double>(std::numeric_limits<T>::max());
    if (v <= lo) {
        return std::numeric_limits<T>::lowest();
    }
    if (v >= hi) {
        return std::numeric_limits<T>::max();
    }
    return static_cast<T>(v);
}

// The Quat drag cache still speaks Vec3, so these two stay. vecDegrees/vecRadians moved to
// inspector_model.cpp at task E.3.1, where axisRowValues/axisRowFieldValue own that arithmetic.
std::array<float, 3> toArray(Vec3 v) { return {v.x, v.y, v.z}; }
Vec3 fromArray(const std::array<float, 3>& a) { return Vec3{a[0], a[1], a[2]}; }

// DragScalar's speed heuristic: a ranged field drags across its whole span in ~200 steps; an
// unranged one uses a fixed default per kind.
float dragSpeed(bool hasRange, double rangeMin, double rangeMax, float unrangedSpeed) {
    if (!hasRange) {
        return unrangedSpeed;
    }
    return static_cast<float>(std::max((rangeMax - rangeMin) / 200.0, 0.05));
}

// The continuous-gesture boundary pair (task 2.4.2, D17). IsItemActivated/IsItemDeactivated refer to
// the LAST submitted item, so this is read AFTER the widget call -- and the two halves are applied on
// OPPOSITE sides of this frame's push: an OPEN edge breaks the chain BEFORE the push, a CLOSE edge
// breaks it AFTER. That asymmetry is not stylistic -- on the release frame ImGui reports the final
// edit AND the deactivation together, so closing the chain FIRST would record that frame as a second,
// un-merged entry. Exactly the ordering defect 2.4.1's code-review round found in the gizmo, factored
// into ONE place here so it cannot drift between the seven arms below.
struct EditGate {
    bool opened = false;
    bool closed = false;
};
[[nodiscard]] EditGate gateForLastItem() { return {ImGui::IsItemActivated(), ImGui::IsItemDeactivated()}; }

// Builds the named CommandContext (never a temporary bound to a reference -- A10) and pushes the
// generic reflected field-write seam once. `field.value` is passed as `before` -- this frame's
// pre-edit value from the model rebuilt at :105, BEFORE any widget was drawn (F13/D16) -- never a
// fresh read taken at push time, which would cost extra lookups and could read the POST-edit value on
// an arm that writes before the push.
void pushFieldEdit(PanelContext& context, Entity entity, const ComponentEntry& entry, const FieldEntry& field,
                   FieldValue after) {
    CommandContext cmd = toCommandContext(context);
    context.commands.push(cmd, std::make_unique<SetFieldCommand>(entity, entry.typeId, field.name, entry.name,
                                                                 field.value, std::move(after)));
}

// task E.3.1: the ImGui half of the label column's width. Measured over the WHOLE model, once per
// frame, so every component's table agrees and the panel reads as ONE column rather than as N -- a
// per-component measurement would give Transform and MeshRenderer different dividers, which is
// sabotage seed S23 and is judged on hardware. The measurement is here; the arithmetic and the clamp
// are in inspector_model.cpp's pure inspectorLabelColumnWidth, where a tier-0 case can reach them.
float measuredLabelColumnWidth(const InspectorModel& model) {
    float widest = 0.0F;
    for (const ComponentEntry& entry : model.components) {
        for (const FieldEntry& field : entry.fields) {
            widest = std::max(widest, ImGui::CalcTextSize(field.name.c_str()).x);
        }
    }
    return inspectorLabelColumnWidth(widest, ImGui::GetStyle().CellPadding.x, ImGui::GetFontSize(),
                                     ImGui::GetContentRegionAvail().x);
}

}  // namespace

void InspectorPanel::onDraw(PanelContext& context) {
    // ID discipline (D13/E14): PushID(full registration name) per component, PushID(field name)
    // per row -- so two same-named fields in different components, and two same-short-named types
    // in different namespaces, never collide. Widgets use the "##v" label so only the left column
    // shows text.
    pending = PendingAction{};
    const Entity primary = context.selection.primary();

    // -- phase 1: reconcile -- drop any cache whose target no longer resolves (E3), OR whose target
    // is no longer the PRIMARY selection (review finding 3): a cache surviving only because its own
    // row happened not to be drawn this frame -- e.g. the user typed into a string field on entity A,
    // selected entity B (which lacks that field, so nothing in drawField ever runs for it), then
    // re-selected A -- is exactly the stranding bug. Reconcile must not depend on draw order.
    if (quatCache.active && (quatCache.entity != primary || !context.world.alive(quatCache.entity))) {
        quatCache = {};
    }
    if (stringCache.active && (stringCache.entity != primary || !context.world.alive(stringCache.entity))) {
        stringCache = {};
    }

    if (!context.world.alive(primary)) {
        const char* text = "No entity selected.";
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2{ImGui::GetCursorPosX() + ((avail.x - textSize.x) * 0.5F),
                                   ImGui::GetCursorPosY() + ((avail.y - textSize.y) * 0.5F)});
        ImGui::TextUnformatted(text);
        return;
    }

    // -- phase 2: build (D15 -- fresh into caller-owned scratch every frame) --
    buildInspectorModel(context.world, primary, model);

    // -- phase 3: draw --
    entityLabel(context.world, primary, labelScratch);
    if (context.selection.entities().size() > 1) {
        labelScratch += " (" + std::to_string(context.selection.entities().size()) + " selected)";
    }
    ImGui::TextUnformatted(labelScratch.c_str());
    ImGui::Separator();

    // ONE width for the whole panel, measured before the loop (task E.3.1).
    const float labelWidth = measuredLabelColumnWidth(model);
    for (const ComponentEntry& entry : model.components) {
        drawComponent(context, primary, entry, labelWidth);
    }

    ImGui::Separator();
    if (ImGui::Button("+ Add Component")) {
        ImGui::OpenPopup("##addComponent");
    }
    if (ImGui::BeginPopup("##addComponent")) {
        bool anyAbsent = false;
        const std::size_t count = context.world.componentTypeCount();
        for (std::size_t i = 0; i < count; ++i) {
            const ComponentTypeId candidateId = context.world.componentTypeAt(i);
            if (context.world.hasRaw(candidateId, primary)) {
                continue;
            }
            anyAbsent = true;
            shortNameScratch = std::string(shortComponentName(context.world.componentTypeName(candidateId)));
            // Review finding 4: MenuItem derives its ID from its LABEL, and two registered types can
            // share a short name (e.g. engine::Camera vs a project game::Camera) -- the same D13/E14
            // discipline drawComponent already applies via PushID(entry.name.c_str()) at :146, just
            // missing here. Keyed on the loop index, which is stable for the popup's own lifetime
            // (componentTypeAt's registration order never reorders mid-frame).
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::MenuItem(shortNameScratch.c_str())) {
                pending = PendingAction{.kind = ActionKind::AddComponent, .type = candidateId};
            }
            ImGui::PopID();
        }
        if (!anyAbsent) {
            ImGui::MenuItem("(none)", nullptr, false, false);  // E12: disabled, never omitted
        }
        ImGui::EndPopup();
    }

    // -- phase 4: apply -- the ONLY place a component is added or removed --
    applyPending(context, primary);
}

void InspectorPanel::drawComponent(PanelContext& context, Entity primary, const ComponentEntry& entry,
                                   float labelWidth) {
    ImGui::PushID(entry.name.c_str());

    shortNameScratch = std::string(shortComponentName(entry.name));
    const bool open = ImGui::CollapsingHeader(shortNameScratch.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", entry.name.c_str());  // the FULL registration name (D13)
    }
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Remove Component")) {
            pending = PendingAction{.kind = ActionKind::RemoveComponent, .type = entry.typeId};
        }
        ImGui::EndPopup();  // ONLY when BeginPopupContextItem returned true
    }
    // NO TreePop: CollapsingHeader uses NoTreePushOnOpen (C7/§DN-10).

    if (open) {
        if (!entry.hasFields) {
            ImGui::TextDisabled("(fields unavailable — built without AERO_REFLECT_TOOLS)");  // D12
        } else if (entry.fields.empty()) {
            ImGui::TextDisabled("(no fields)");  // a tag component (E13)
        } else if (ImGui::BeginTable("##fields", 2, TABLE_FLAGS)) {
            // ASYMMETRIC: EndTable ONLY when BeginTable returned true. An unbalanced call is an
            // IM_ASSERT abort in Debug, not a glitch. The CollapsingHeader above stays OUTSIDE the
            // table, the SeparatorText precedent from project_settings_panel.cpp.
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
            for (const FieldEntry& field : entry.fields) {
                drawField(context, primary, entry, field);
            }
            ImGui::EndTable();
        }
    }

    ImGui::PopID();
}

void InspectorPanel::drawFieldResetMenu(PanelContext& context, Entity primary, const ComponentEntry& entry,
                                        const FieldEntry& field, std::optional<std::size_t> axis, const char* strId) {
    if (!ImGui::BeginPopupContextItem(strId)) {
        return;  // ASYMMETRIC: EndPopup ONLY when BeginPopupContextItem returned true
    }

    // THE DEFAULT IS RESOLVED HERE, INSIDE THE OPEN POPUP -- once per frame while a menu is open,
    // never in the per-frame walk. It costs one heap allocation (measured), which is cheap for
    // something only a person looking at a menu pays for and would not be cheap per field per frame.
    const std::optional<FieldValue> defaultValue = defaultComponentField(context.world, entry.typeId, field.name);

    if (axis.has_value() && isAxisRow(field.kind, field.color)) {
        const AxisResetAction one = axisResetAction(axis, field.kind, field.value, defaultValue);
        // BeginDisabled rather than MenuItem's own `enabled` argument: both work, and this is the
        // spelling the Guid arm already uses for the identical decision.
        ImGui::BeginDisabled(!one.enabled);
        if (ImGui::MenuItem(one.label.c_str())) {
            resetField(context, primary, entry, field, one.result);
        }
        ImGui::EndDisabled();  // 1:1 with BeginDisabled
        ImGui::Separator();
    }

    const AxisResetAction all = axisResetAction(std::nullopt, field.kind, field.value, defaultValue);
    ImGui::BeginDisabled(!all.enabled);
    if (ImGui::MenuItem(all.label.c_str())) {
        resetField(context, primary, entry, field, all.result);
    }
    ImGui::EndDisabled();

    ImGui::EndPopup();
}

void InspectorPanel::resetField(PanelContext& context, Entity primary, const ComponentEntry& entry,
                                const FieldEntry& field, FieldValue after) {
    // BOTH SIDES, EXPLICITLY. A reset is a discrete edit, so it must never merge with a drag that was
    // released on the same field a moment earlier -- and a MenuItem inside a per-axis popup claims
    // ActiveId, which EndGroup then forwards, so gateForLastItem() would break the chain on the click
    // frame by accident. Arriving at the right behaviour by accident is not the same as stating it.
    context.commands.breakMergeChain();  // BEFORE the push
    pushFieldEdit(context, primary, entry, field, std::move(after));
    context.commands.breakMergeChain();  // AFTER the push
    // BOTH caches, unconditionally. At most one is live, so clearing both is two lines with no
    // predicate for a future edit to get wrong -- and without this the Quat row would keep DISPLAYING
    // the pre-reset euler triple until the pointer moved off the box.
    quatCache = {};
    stringCache = {};
}

bool InspectorPanel::drawAxisRow(PanelContext& context, Entity primary, const ComponentEntry& entry,
                                 const FieldEntry& field, std::array<float, 3>& shown, float speed) {
    // WHAT DragScalarN DOES INTERNALLY (imgui_widgets.cpp:2814), opened up: BeginGroup, then per
    // component PushID(i) / SameLine(0, ItemInnerSpacing.x) / DragScalar / PopID, then EndGroup. The
    // reason to hand-roll it is that each axis needs its OWN item to carry a label, a colour and (from
    // task E.3.1's next step) a context menu -- DragFloat3 exposes none of the three.
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;

    // The MAX over the three letters, not CalcTextSize("X").x: with a proportional font the three
    // differ by a fraction of a pixel, and taking the max means the width budget can never
    // UNDER-allocate and the last box can never overrun the cell.
    float letterWidth = 0.0F;
    for (std::size_t i = 0; i < AXIS_ROW_COMPONENTS; ++i) {
        const std::string_view label = axisRowLabel(i);
        letterWidth = std::max(letterWidth, ImGui::CalcTextSize(label.data(), label.data() + label.size()).x);
    }
    // [letter][gap][box] x3, with one gap between units: 3*letter + 5*gap + 3*box == the cell.
    const float total = ImGui::GetContentRegionAvail().x;
    const float boxWidth = std::max((total - (3.0F * letterWidth) - (5.0F * gap)) / 3.0F, 1.0F);

    bool edited = false;
    ImGui::BeginGroup();  // 1:1 with EndGroup below -- nothing between them can return
    for (std::size_t i = 0; i < AXIS_ROW_COMPONENTS; ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (i > 0) {
            ImGui::SameLine(0.0F, gap);
        }
        // THE ONE PLACE AN ImU32 IS BUILT. The colour is DERIVED from axis_palette.hpp through
        // axisRowColor -- this file states no colour literal of its own, which I143 pins, because a
        // restated literal one byte off is invisible to every automated tier (E.1.4's sabotage row 20).
        const std::array<std::uint8_t, 3> rgb = axisRowColor(i);
        const std::string_view label = axisRowLabel(i);
        ImGui::AlignTextToFramePadding();  // idempotent (ImMax-based); once per letter
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(rgb[0], rgb[1], rgb[2], 255));
        // THE LETTER BEFORE THE BOX IS LOAD-BEARING, NOT COSMETIC: ItemAdd clears the pending
        // NextItemData, so drawField's SetNextItemWidth(-1.0F) is consumed by this Text -- the first
        // item submitted here -- rather than by the first DragScalar, which would otherwise take the
        // whole cell's width.
        ImGui::TextUnformatted(label.data(), label.data() + label.size());
        ImGui::PopStyleColor();  // 1:1 with PushStyleColor
        ImGui::SameLine(0.0F, gap);
        ImGui::SetNextItemWidth(boxWidth);
        // nullptr for p_min/p_max/format is EXACTLY DragFloat3's behaviour: it passes two pointers to
        // 0.0f, which DragBehaviorT treats as unbounded, and DragScalar falls back to
        // ImGuiDataType_Float's PrintFmt, "%.3f" -- DragFloat3's own default. The display is
        // byte-identical to what this row showed before.
        edited = ImGui::DragScalar("##a", ImGuiDataType_Float, &shown[i], speed, nullptr, nullptr, nullptr,
                                   ImGuiSliderFlags_None) ||
                 edited;
        // NO str_id: DragScalar reads mouse button 0 only, so right-click is unclaimed, and the axis's
        // own drag id is non-zero and makes a perfectly good popup id. NEVER call this with nullptr
        // after EndGroup() -- a group's ItemAdd uses id 0 and only overwrites LastItemData.ID when the
        // group contains the active or deactivated id, so that call is an IM_ASSERT abort on any frame
        // nothing inside the group is active.
        drawFieldResetMenu(context, primary, entry, field, i, /*strId=*/nullptr);
        ImGui::PopID();
    }
    ImGui::EndGroup();
    return edited;
}

void InspectorPanel::drawField(PanelContext& context, Entity primary, const ComponentEntry& entry,
                               const FieldEntry& field) {
    // task E.3.1: one table ROW per field -- the label cell, then the value cell. The old
    // SameLine(120.0F) is gone: a field name longer than ~16 characters overran into the widget and a
    // short one wasted the space, and the column width now comes from the model every frame.
    ImGui::PushID(field.name.c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(field.name.c_str());
    // THE WHOLE-FIELD MENU HANGS OFF THE LABEL CELL, never off a value widget (D6): FieldKind::String
    // keeps an uncommitted buffer whose release is keyed on ImGui::IsItemActive(), so a popup opening
    // over it steals ActiveId and silently discards whatever the user was typing. An explicit str_id
    // because a Text item's own id is 0, which BeginPopupContextItem asserts on. IsItemHovered's
    // `id == 0` arm covers the window-move case; with nothing active its ActiveId check
    // short-circuits, so the ordinary right-click works.
    drawFieldResetMenu(context, primary, entry, field, /*axis=*/std::nullopt, /*strId=*/"##fieldmenu");
    ImGui::TableNextColumn();
    // -1.0F inside a cell means the CELL's width, which is what every non-axis arm wants.
    ImGui::SetNextItemWidth(-1.0F);

    switch (field.kind) {
        case FieldKind::Bool: {
            bool v = std::get<bool>(field.value);
            const bool edited = ImGui::Checkbox("##v", &v);
            // A Checkbox activates AND deactivates on the same frame (press-release, discrete), so the
            // gate is harmless here -- applied anyway so no arm is the odd one out (task 2.4.2 D17).
            const EditGate gate = gateForLastItem();
            if (gate.opened) {
                context.commands.breakMergeChain();  // BEFORE the push
            }
            if (edited) {
                pushFieldEdit(context, primary, entry, field, FieldValue{v});
            }
            if (gate.closed) {
                context.commands.breakMergeChain();  // AFTER the push
            }
            break;
        }
        case FieldKind::Int: {
            std::int64_t v = std::get<std::int64_t>(field.value);
            const auto lo = doubleToClamped<std::int64_t>(field.rangeMin);
            const auto hi = doubleToClamped<std::int64_t>(field.rangeMax);
            const float speed = dragSpeed(field.hasRange, field.rangeMin, field.rangeMax, 1.0F);
            const ImGuiSliderFlags flags = field.hasRange ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
            const bool edited = ImGui::DragScalar("##v", ImGuiDataType_S64, &v, speed, field.hasRange ? &lo : nullptr,
                                                  field.hasRange ? &hi : nullptr, nullptr, flags);
            const EditGate gate = gateForLastItem();
            if (gate.opened) {
                context.commands.breakMergeChain();
            }
            if (edited) {
                pushFieldEdit(context, primary, entry, field, FieldValue{v});
            }
            if (gate.closed) {
                context.commands.breakMergeChain();
            }
            break;
        }
        case FieldKind::UInt: {
            std::uint64_t v = std::get<std::uint64_t>(field.value);
            const auto lo = doubleToClamped<std::uint64_t>(field.rangeMin);
            const auto hi = doubleToClamped<std::uint64_t>(field.rangeMax);
            const float speed = dragSpeed(field.hasRange, field.rangeMin, field.rangeMax, 1.0F);
            const ImGuiSliderFlags flags = field.hasRange ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
            const bool edited = ImGui::DragScalar("##v", ImGuiDataType_U64, &v, speed, field.hasRange ? &lo : nullptr,
                                                  field.hasRange ? &hi : nullptr, nullptr, flags);
            const EditGate gate = gateForLastItem();
            if (gate.opened) {
                context.commands.breakMergeChain();
            }
            if (edited) {
                pushFieldEdit(context, primary, entry, field, FieldValue{v});
            }
            if (gate.closed) {
                context.commands.breakMergeChain();
            }
            break;
        }
        case FieldKind::Float: {
            double v = std::get<double>(field.value);
            const double lo = field.rangeMin;
            const double hi = field.rangeMax;
            const float speed = dragSpeed(field.hasRange, field.rangeMin, field.rangeMax, 0.1F);
            const ImGuiSliderFlags flags = field.hasRange ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
            const bool edited =
                ImGui::DragScalar("##v", ImGuiDataType_Double, &v, speed, field.hasRange ? &lo : nullptr,
                                  field.hasRange ? &hi : nullptr, nullptr, flags);
            const EditGate gate = gateForLastItem();
            if (gate.opened) {
                context.commands.breakMergeChain();
            }
            if (edited) {
                pushFieldEdit(context, primary, entry, field, FieldValue{v});
            }
            if (gate.closed) {
                context.commands.breakMergeChain();
            }
            break;
        }
        case FieldKind::Vec3: {
            std::array<float, 3> shown = axisRowValues(field.value, FieldKind::Vec3);
            bool edited = false;
            if (field.color) {
                // HDR preserves > 1 (E19); the seam does NOT clamp colours -- colour and range are
                // orthogonal. A colour's three numbers are CHANNELS, not axes, which is exactly what
                // isAxisRow(Vec3, color=true) answers false to.
                edited = ImGui::ColorEdit3("##v", shown.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            } else {
                edited = drawAxisRow(context, primary, entry, field, shown, 0.1F);
            }
            // ColorEdit3 wraps itself in BeginGroup/EndGroup and so does drawAxisRow, and EndGroup
            // forwards the active/deactivated id to LastItemData, so the gate below sees the WHOLE
            // widget's edges, not a single axis's (task 2.4.2 G2). Read once, after the branch -- the
            // merge-chain semantics are byte for byte what they were with DragFloat3.
            const EditGate gate = gateForLastItem();
            if (gate.opened) {
                context.commands.breakMergeChain();
            }
            if (edited) {
                pushFieldEdit(context, primary, entry, field, axisRowFieldValue(shown, FieldKind::Vec3));
            }
            if (gate.closed) {
                context.commands.breakMergeChain();
            }
            break;
        }
        case FieldKind::Quat: {
            // C7: drawAxisRow wraps its items in BeginGroup/EndGroup, so IsItemActive() right after it
            // is group-correct (reflects the WHOLE triplet, not just the last axis).
            const bool cacheHit = quatCache.active && quatCache.matches(primary, entry.typeId, field.name);
            std::array<float, 3> shown =
                cacheHit ? toArray(quatCache.eulerDegrees) : axisRowValues(field.value, FieldKind::Quat);
            const bool edited = drawAxisRow(context, primary, entry, field, shown, 1.0F);
            // The gate refers to this same last-submitted item; read it here, immediately after the
            // widget call and before the cache block below (which submits no ImGui item of its own).
            const EditGate gate = gateForLastItem();
            if (ImGui::IsItemActive()) {
                quatCache = {primary, entry.typeId, field.name, /*active=*/true, fromArray(shown)};
            } else if (cacheHit) {
                quatCache = {};  // released: drop, so the display re-derives next frame (E11)
            }
            if (gate.opened) {
                context.commands.breakMergeChain();
            }
            if (edited) {
                pushFieldEdit(context, primary, entry, field, axisRowFieldValue(shown, FieldKind::Quat));
            }
            if (gate.closed) {
                context.commands.breakMergeChain();
            }
            break;
        }
        case FieldKind::String: {
            const bool cacheHit = stringCache.active && stringCache.matches(primary, entry.typeId, field.name);
            if (!cacheHit) {
                stringCache.buffer = std::get<std::string>(field.value);
            }
            inputTextString("##v", stringCache.buffer, 0);
            const EditGate gate = gateForLastItem();
            // D14: commit only on deactivation-after-edit, never per-keystroke -- this lands on the
            // SAME frame as gate.closed, so the push-then-close ordering below is what makes it one
            // entry rather than one entry plus an orphaned open chain (F15).
            const bool committed = ImGui::IsItemDeactivatedAfterEdit();
            if (gate.opened) {
                context.commands.breakMergeChain();  // BEFORE the push
            }
            if (committed) {
                pushFieldEdit(context, primary, entry, field, FieldValue{stringCache.buffer});
            }
            if (gate.closed) {
                context.commands.breakMergeChain();  // AFTER the push
            }
            // The cache release below stays AFTER the push above: the push reads stringCache.buffer,
            // and release clears it.
            if (ImGui::IsItemActive()) {
                stringCache.entity = primary;
                stringCache.type = entry.typeId;
                stringCache.field = field.name;
                stringCache.active = true;
            } else if (cacheHit) {
                // Released (matches the Quat arm's release shape, review finding 3): drop
                // unconditionally rather than relying on IsItemDeactivated() alone, which only fires
                // when THIS row is drawn -- the same stranding hazard phase 1's reconcile now also
                // guards against from the other direction.
                stringCache = {};
            }
            break;
        }
        case FieldKind::Guid: {
            // task 3.1.5 (D14/D-20). The row is chosen by KIND, never by component or field name --
            // the standing rule that keeps the inspector generic. NO InputText: a hand-typed guid is
            // not a workflow anybody needs and it would need parse-error UI. NO DROP TARGET EITHER:
            // assignment happens on the Hierarchy row and in the viewport, and IR8 pins that by
            // scanning this file's own code for ImGui's drop-target entry points. Their names are
            // deliberately not spelled here: that pin strips comments, but the plain grep a reader
            // would reach for does not, and a prose mention would read as a violation.
            //
            // Both decisions -- the sentence and whether Clear is live -- come from ONE pure call, so
            // this panel holds no second copy of either and a tier-0 case asserts what is drawn here.
            const GuidFieldRow row = guidFieldRow(std::get<Guid>(field.value), database);
            ImGui::TextUnformatted(row.text.c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(!row.clearEnabled);
            const bool cleared = ImGui::SmallButton("Clear");
            ImGui::EndDisabled();  // 1:1 with BeginDisabled -- nothing between them can return
            // A button press-releases within one frame, so the gate is harmless here; applied anyway
            // so no arm is the odd one out (task 2.4.2 D17).
            const EditGate gate = gateForLastItem();
            if (gate.opened) {
                context.commands.breakMergeChain();  // BEFORE the push
            }
            if (cleared) {
                pushFieldEdit(context, primary, entry, field, FieldValue{Guid{}});
            }
            if (gate.closed) {
                context.commands.breakMergeChain();  // AFTER the push
            }
            break;
        }
    }

    ImGui::PopID();
}

void InspectorPanel::applyPending(PanelContext& context, Entity primary) {
    switch (pending.kind) {
        case ActionKind::None:
            break;
        case ActionKind::AddComponent: {
            CommandContext cmd = toCommandContext(context);
            context.commands.push(cmd, std::make_unique<AddComponentCommand>(
                                           primary, pending.type, context.world.componentTypeName(pending.type)));
            break;
        }
        case ActionKind::RemoveComponent: {
            CommandContext cmd = toCommandContext(context);
            context.commands.push(cmd, std::make_unique<RemoveComponentCommand>(
                                           primary, pending.type, context.world.componentTypeName(pending.type)));
            break;
        }
    }
    pending = PendingAction{};
}

}  // namespace engine::editor

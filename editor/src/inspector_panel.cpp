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

std::array<float, 3> toArray(Vec3 v) { return {v.x, v.y, v.z}; }
Vec3 fromArray(const std::array<float, 3>& a) { return Vec3{a[0], a[1], a[2]}; }

// degrees()/radians() are scalar-only (math/constants.hpp) -- applied componentwise for the euler
// triplet, since there is no Vec3 overload.
Vec3 vecDegrees(Vec3 v) { return Vec3{degrees(v.x), degrees(v.y), degrees(v.z)}; }
Vec3 vecRadians(Vec3 v) { return Vec3{radians(v.x), radians(v.y), radians(v.z)}; }

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

    for (const ComponentEntry& entry : model.components) {
        drawComponent(context, primary, entry);
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

void InspectorPanel::drawComponent(PanelContext& context, Entity primary, const ComponentEntry& entry) {
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
        } else {
            for (const FieldEntry& field : entry.fields) {
                drawField(context, primary, entry, field);
            }
        }
    }

    ImGui::PopID();
}

void InspectorPanel::drawField(PanelContext& context, Entity primary, const ComponentEntry& entry,
                               const FieldEntry& field) {
    ImGui::PushID(field.name.c_str());
    ImGui::TextUnformatted(field.name.c_str());
    ImGui::SameLine(120.0F);
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
            std::array<float, 3> tmp = toArray(std::get<Vec3>(field.value));
            bool edited = false;
            if (field.color) {
                // HDR preserves > 1 (E19); the seam does NOT clamp colours -- colour and range are
                // orthogonal.
                edited = ImGui::ColorEdit3("##v", tmp.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            } else {
                edited = ImGui::DragFloat3("##v", tmp.data(), 0.1F);
            }
            // DragFloat3/ColorEdit4 wrap themselves in BeginGroup/EndGroup, and EndGroup forwards the
            // active/deactivated id to LastItemData, so the gate below sees the WHOLE widget's edges,
            // not a single axis's (task 2.4.2 G2). Read once, after the branch.
            const EditGate gate = gateForLastItem();
            if (gate.opened) {
                context.commands.breakMergeChain();
            }
            if (edited) {
                pushFieldEdit(context, primary, entry, field, FieldValue{fromArray(tmp)});
            }
            if (gate.closed) {
                context.commands.breakMergeChain();
            }
            break;
        }
        case FieldKind::Quat: {
            // C7: DragScalarN/DragFloat3 wraps its items in BeginGroup/EndGroup, so IsItemActive()
            // right after it is group-correct (reflects the WHOLE triplet, not just the last axis).
            const bool cacheHit = quatCache.active && quatCache.matches(primary, entry.typeId, field.name);
            const std::array<float, 3> deg = cacheHit ? toArray(quatCache.eulerDegrees)
                                                      : toArray(vecDegrees(eulerAngles(std::get<Quat>(field.value))));
            std::array<float, 3> dragged = deg;
            const bool edited = ImGui::DragFloat3("##v", dragged.data(), 1.0F);
            // The gate refers to this same last-submitted item; read it here, immediately after the
            // widget call and before the cache block below (which submits no ImGui item of its own).
            const EditGate gate = gateForLastItem();
            if (ImGui::IsItemActive()) {
                quatCache = {primary, entry.typeId, field.name, /*active=*/true, fromArray(dragged)};
            } else if (cacheHit) {
                quatCache = {};  // released: drop, so the display re-derives next frame (E11)
            }
            if (gate.opened) {
                context.commands.breakMergeChain();
            }
            if (edited) {
                pushFieldEdit(context, primary, entry, field,
                              FieldValue{normalize(fromEulerAngles(vecRadians(fromArray(dragged))))});
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

#include "inspector_panel.hpp"

#include <aero/core/math.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/asset_drag.hpp>  // task E.3.3 -- assetReferenceKindFromToken, DropSurface
#include <aero/editor/asset_view.hpp>  // elideCaptionRight
#include <aero/editor/command_stack.hpp>
#include <aero/editor/component_commands.hpp>
#include <aero/editor/editor_glyphs.hpp>
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/entity_presentation.hpp>
#include <aero/editor/material_card.hpp>
#include <aero/editor/panel_context.hpp>
#include <aero/editor/project_files.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/thumbnail_cache.hpp>
#include <aero/scene/world.hpp>

#include "asset_picker.hpp"     // task E.3.3 -- the ONE asset-reference field widget
#include "editor_theme_ui.hpp"  // task E.6.1: currentUiScale(), for the axis row's letter gap
#include "panel_chrome_ui.hpp"  // task E.6.3: the card painter, faces and roles -- the header push moved into it (I286)
#include "text_input.hpp"
#include "thumbnail_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <imgui_internal.h>  // GetTopMostPopupModal (I337) and MenuItemEx (the Add Component popup's icons)
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>  // task E.5.2: std::exchange -- the one-shot taker's own idiom
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

// task E.6.3 (D5): the dashed button's one label, drawn on the draw list in Strong `accent`.
constexpr std::string_view ADD_COMPONENT_LABEL = "Add Component";

// The short display name for an Add Component menu entry ("Transform" from "engine::Transform"). A card's title is
// the model's humanised entry.title, and its full-name tooltip is read off CardHeaderResult::headerHovered.
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
            // task E.6.3: the humanised label it draws.
            widest = std::max(widest, ImGui::CalcTextSize(field.label.c_str()).x);
        }
    }
    return inspectorLabelColumnWidth(widest, ImGui::GetStyle().CellPadding.x, ImGui::GetFontSize(),
                                     ImGui::GetContentRegionAvail().x);
}

}  // namespace

InspectorPanel::InspectorPanel() : cardPainter(std::make_unique<CardPainter>()) {}
InspectorPanel::~InspectorPanel() = default;

void InspectorPanel::requestComponentMenuAction(std::string registrationName, ComponentMenuAction action) {
    pendingComponentMenu = ComponentMenuRequest{.registrationName = std::move(registrationName), .action = action};
}

void InspectorPanel::requestCardOpen(std::string registrationName, bool open) {
    pendingCardOpen = CardOpenRequest{.registrationName = std::move(registrationName), .open = open};
}

void InspectorPanel::onDraw(PanelContext& context) {
    // task E.5.2: the seam's request is taken HERE, before any early return below, and std::exchange
    // RESETS the member (a moved-from optional stays engaged -- E.4.3). Overwriting frameNamedSelection
    // also discards whatever the previous onDraw did not consume.
    frameNamedSelection = std::exchange(pendingNamedSelection, std::nullopt);
    frameComponentMenu = std::exchange(pendingComponentMenu, std::nullopt);  // task E.6.3: E.5.2's one-shot shape
    frameCardOpen = std::exchange(pendingCardOpen, std::nullopt);            // task E.6.3 (fix 7): the same shape
    ++drawRecordValue.framesRecorded;  // task E.6.3: per-frame counts, before any early return below
    beginCardRecordFrame(drawRecordValue.cards);
    drawRecordValue.cardsOpen = 0;
    drawRecordValue.referenceSwatchDrawn = false;
    drawRecordValue.referenceWellDrawn = false;
    drawRecordValue.referenceFields.clear();
    drawRecordValue.addComponentDashVertices = 0;
    drawRecordValue.addComponentDashed = false;
    drawRecordValue.addComponentFirstDashWidth = 0.0F;
    drawRecordValue.addComponentFirstDashGap = 0.0F;
    headersSubmittedValue = 0;  // task E.6.1: a PER-FRAME count, taken before any early return below
    // task E.6.1 (I289): this frame's geometry, likewise before any early return. drawAxisRow writes the two
    // axis values, so a frame that draws no axis row reads 0 for both.
    lastContentWidthValue = ImGui::GetContentRegionAvail().x;
    lastAxisBoxWidthValue = 0.0F;
    lastAxisRowSlackValue = 0.0F;
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
    drawEntityHeader(context, primary);

    // ONE width for the whole panel, measured before the loop (task E.3.1).
    const float labelWidth = measuredLabelColumnWidth(model);
    drawRecordValue.labelColumnWidth = labelWidth;
    for (const ComponentEntry& entry : model.components) {
        drawComponent(context, primary, entry, labelWidth);
    }
    drawAddComponent(context, primary);

    // -- phase 4: apply -- the ONLY place a component is added or removed --
    applyPending(context, primary);
}

// task E.6.3 (D5): the entity header row -- a kind swatch (the entity's kind role: the mock paints accent, which the
// Inspector, always showing the selection, would paint for every entity), the name in Strong at titleSize with
// " (N selected)" kept for a multi-selection, and e#<index> in Mono small textFaint right-aligned with the generation
// in a tooltip. Read-only: renaming stays the Hierarchy's.
void InspectorPanel::drawEntityHeader(PanelContext& context, Entity primary) {
    entityLabel(context.world, primary, labelScratch);
    if (context.selection.entities().size() > 1) {
        labelScratch += " (" + std::to_string(context.selection.entities().size()) + " selected)";
    }
    entityIdText(primary, idScratch);
    drawRecordValue.idText = idScratch;
    const float s = currentUiScale();
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    // The name's ONE face: the measure, the draw and the record all read it, so they cannot diverge (I315).
    constexpr PanelFace NAME_FACE = PanelFace::StrongTitle;
    const float titlePoints = panelFacePoints(NAME_FACE);
    const float rowMidY = rowMin.y + (titlePoints * 0.5F);
    const InspectorHeaderLayout layout = inspectorHeaderLayout(rowMin.x, rowMin.x + rowWidth, rowMidY,
                                                               faceTextWidth(PanelFace::MonoSmall, idScratch), s);
    ImDrawList* const list = ImGui::GetWindowDrawList();
    const ChromeRole swatch = entityKindRole(entityKindFor(context.world, primary), false);
    roleFill(list, ImVec2(layout.swatchMin.x, layout.swatchMin.y), ImVec2(layout.swatchMax.x, layout.swatchMax.y),
             swatch, EDITOR_THEME.panels.wellRounding * s);
    drawRecordValue.swatchRole = swatch;
    const float nameWidth = layout.nameMaxX - layout.nameX;
    const std::string name = elideCaptionRight(
        labelScratch, [nameWidth](std::string_view text) { return faceTextWidth(NAME_FACE, text) <= nameWidth; });
    drawRoleText(list, NAME_FACE, ImVec2(layout.nameX, rowMin.y), ChromeRole::TextBright, name);
    const ImFont* const titleFace = panelFaceFont(NAME_FACE);
    drawRecordValue.entityNameFontName = titleFace != nullptr ? titleFace->GetDebugName() : "";
    drawRecordValue.entityNameFontSize = titlePoints;
    const float smallPoints = panelFacePoints(PanelFace::MonoSmall);
    const ImVec2 idMin(layout.idX, rowMidY - (smallPoints * 0.5F));
    drawRoleText(list, PanelFace::MonoSmall, idMin, ChromeRole::TextFaint, idScratch);
    ImGui::Dummy(ImVec2(rowWidth, titlePoints));  // the row as ONE item: the first card starts below it
    // IsItemHovered (the row's Dummy) first: IsMouseHoveringRect alone ignores window order and popup blocking
    // (imgui.h:1147), so a popup or a modal over the header would still raise the tooltip.
    const ImVec2 idMax(rowMin.x + rowWidth, idMin.y + smallPoints);
    if (ImGui::IsItemHovered() && ImGui::IsMouseHoveringRect(idMin, idMax)) {
        entityIdTooltip(primary, idScratch);
        ImGui::SetTooltip("%s", idScratch.c_str());  // a runtime string is never a format string (I242(e))
    }
}

void InspectorPanel::drawComponent(PanelContext& context, Entity primary, const ComponentEntry& entry,
                                   float labelWidth) {
    ImGui::PushID(entry.name.c_str());
    // task E.6.3 (I337): a modal blocks every click on the card, so it blocks the seam too -- computed ONCE.
    const bool noModal = ImGui::GetTopMostPopupModal() == nullptr;
    // The header toggle's seam: the next item is the card's TreeNodeEx (begin submits nothing before it), and a modal
    // blocks the click it stands in for.
    if (frameCardOpen.has_value() && frameCardOpen->matches(entry.name) && noModal) {
        ImGui::SetNextItemOpen(frameCardOpen->open, ImGuiCond_Always);
        frameCardOpen.reset();
    }
    {
        // D4: the card. Its strip, chevron, icon and title are the painter's; the open state is ImGui's own (no
        // panel bit: this panel is one window, so no mode flip can reset it). The title is drawn, never a label, so
        // no `##` in it can truncate it.
        const PanelCard card(*cardPainter, "##card",
                             CardSpec{.title = entry.title, .icon = entry.icon, .overflow = true}, nullptr,
                             &drawRecordValue.cards);
        const CardHeaderResult& header = card.header();
        ++headersSubmittedValue;
        lastHeaderFontNameValue = drawRecordValue.cards.titleFontName;  // the face the painter DREW the title in
        drawRecordValue.cardsOpen += header.open ? 1U : 0U;
        drawRecordValue.lastTitleDrawn = entry.title;
        drawRecordValue.lastIconDrawn.assign(entry.icon);
        // The full registration name, as today's header carried it -- read off the header's OWN answer: after the
        // card's begin the last item is the overflow button, never the header (D4).
        if (header.headerHovered) {
            ImGui::SetTooltip("%s", entry.name.c_str());  // the FULL registration name (D13)
        }
        // ONE popup, opened by the overflow or by a right-click on the header -- the release rule
        // BeginPopupContextItem used (imgui.cpp:13280-13285), so a right-click on a second header while this menu is
        // open closes it on the press and opens the second on the release, as before.
        if (header.overflowPressed || header.headerRightClicked) {
            ImGui::OpenPopup("##componentMenu");
        }
        if (ImGui::BeginPopup("##componentMenu")) {
            if (ImGui::MenuItem("Remove Component")) {
                pending = PendingAction{.kind = ActionKind::RemoveComponent, .type = entry.typeId};
            }
            ImGui::EndPopup();  // ONLY when BeginPopup returned true
        }
        if (frameComponentMenu.has_value() && frameComponentMenu->matches(entry.name) && noModal) {
            pending = PendingAction{.kind = ActionKind::RemoveComponent, .type = entry.typeId};  // the item's write
            frameComponentMenu.reset();
        }
        if (header.open) {
            if (!entry.hasFields) {
                mutedText("(fields unavailable " AERO_GLYPH_EM_DASH " built without AERO_REFLECT_TOOLS)");  // D12
            } else if (entry.fields.empty()) {
                mutedText("(no fields)");  // a tag component (E13)
            } else if (ImGui::BeginTable("##fields", 2, TABLE_FLAGS)) {
                // ASYMMETRIC: EndTable ONLY when BeginTable returned true. The table runs its own splitter on top of
                // the card's content channel (imgui.h:3532-3533): legal, I318.
                ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
                for (const FieldEntry& field : entry.fields) {
                    drawField(context, primary, entry, field);
                }
                ImGui::EndTable();
                drawRecordValue.cards.lastBodyTableMaxX = ImGui::GetItemRectMax().x;  // EndTable's item: OuterRect
                drawRecordValue.cards.lastBodyTableMinY = ImGui::GetItemRectMin().y;  // where ImGui PLACED it (I336)
                drawRecordValue.cards.lastBodyTableMaxY = ImGui::GetItemRectMax().y;
            }
        }
    }  // ~PanelCard: the card ends on every path
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

void InspectorPanel::requestNamedSelection(std::string component, std::string field, std::size_t index) {
    // Built whole and assigned, never emplace()d empty. NamedSelection is a NESTED struct with a default
    // member initializer, which clang parses only once InspectorPanel is complete -- so a no-argument
    // is_constructible_v<NamedSelection> can be settled as false while the class is still open, and
    // libstdc++'s emplace() is then refused (CI's Linux clang-tidy lane; libc++ and GCC both accept it).
    // A move construction never needs the initializer.
    pendingNamedSelection =
        NamedSelection{.componentName = std::move(component), .fieldName = std::move(field), .index = index};
}

bool InspectorPanel::drawAxisRow(PanelContext& context, Entity primary, const ComponentEntry& entry,
                                 const FieldEntry& field, std::array<float, 3>& shown, float speed) {
    // WHAT DragScalarN DOES INTERNALLY (imgui_widgets.cpp:2814), opened up -- each axis needs its OWN item to carry a
    // colour and a context menu. task E.6.3 (D5): each axis is a UNIT -- an `inset` frame with its 1-dp border, the
    // coloured letter INSIDE it in the small face (draw-list text: no item, no font push), and the value region as a
    // FRAMELESS DragScalar. The units are laid out by inspector_model's pure inspectorAxisLayout (TH11), handed the
    // unit gap the SameLine call below spells.
    const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
    // The widest letter in the small face, rounded UP to a whole point (CalcTextSize's convention), so every unit
    // budgets the same letter cell.
    float letterWidth = 0.0F;
    for (std::size_t i = 0; i < AXIS_ROW_COMPONENTS; ++i) {
        letterWidth = std::max(letterWidth, std::ceil(faceTextWidth(PanelFace::BodySmall, axisRowLabel(i))));
    }
    const float total = ImGui::GetContentRegionAvail().x;
    const InspectorAxisLayout axis = inspectorAxisLayout(total, letterWidth, gap, currentUiScale());
    drawRecordValue.axisUnitWidth = axis.unitWidth;
    const float rounding = ImGui::GetStyle().FrameRounding;
    const float frame = ImGui::GetFrameHeight();
    const float rule = ruleHeight();
    ImFont* const letterFace = panelFaceFont(PanelFace::BodySmall);
    const float letterPoints = panelFacePoints(PanelFace::BodySmall);
    ImDrawList* const list = ImGui::GetWindowDrawList();

    bool edited = false;
    ImGui::BeginGroup();  // 1:1 with EndGroup below -- nothing between them can return
    for (std::size_t i = 0; i < AXIS_ROW_COMPONENTS; ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (i > 0) {
            ImGui::SameLine(0.0F, gap);
        }
        const ImVec2 unitMin = ImGui::GetCursorScreenPos();
        const ImVec2 unitMax(unitMin.x + axis.unitWidth, unitMin.y + frame);
        // The unit's frame FIRST, under the frameless drag: the inset fill and its 1-dp border.
        roleFill(list, unitMin, unitMax, ChromeRole::Inset, rounding);
        roleOutline(list, unitMin, unitMax, ChromeRole::Border, rounding, rule);
        // THE ONE PLACE AN ImU32 IS BUILT (I143): the colour DERIVED from axis_palette.hpp through axisRowColor.
        const std::array<std::uint8_t, 3> rgb = axisRowColor(i);
        const ImU32 letterColor = IM_COL32(rgb[0], rgb[1], rgb[2], 255);
        const std::string_view label = axisRowLabel(i);
        const ImVec2 letterAt(unitMin.x + axis.letterX, unitMin.y + ((frame - letterPoints) * 0.5F));
        list->AddText(letterFace, letterPoints, letterAt, letterColor, label.data(), label.data() + label.size());
        // The value region: frameless on ALL THREE frame slots (DragScalar fills FrameBgActive / FrameBgHovered /
        // FrameBg by state, imgui_widgets.cpp:2787-2789), so a hovered or held box paints no block over its value.
        // SetNextItemWidth overwrites drawField's pending -1 (the letter is no longer an item to consume it).
        ImGui::SetCursorScreenPos(ImVec2(unitMin.x + axis.valueX, unitMin.y));
        // nullptr for p_min/p_max/format is EXACTLY DragFloat3's behaviour ("%.3f", unbounded). Stated HERE, above the
        // width: I320 pins the drag at most three lines below SetNextItemWidth (a comment line still counts).
        ImGui::SetNextItemWidth(axis.valueWidth);
        {
            const ClearFrameStyle clear(ChromeRole::Inset);
            edited = ImGui::DragScalar("##a", ImGuiDataType_Float, &shown[i], speed, nullptr, nullptr, nullptr,
                                       ImGuiSliderFlags_None) ||
                     edited;
        }
        lastAxisBoxWidthValue = ImGui::GetItemRectSize().x;  // the value region ImGui laid out (I289)
        drawRecordValue.axisValueWidth = lastAxisBoxWidthValue;
        // Hover and active are an OUTLINE on the WHOLE unit, never a fill on the value region (D5).
        if (ImGui::IsItemActive()) {
            roleOutline(list, unitMin, unitMax, ChromeRole::AccentBorder, rounding, rule);
        } else if (ImGui::IsItemHovered()) {
            roleOutline(list, unitMin, unitMax, ChromeRole::Hover, rounding, rule);
        }
        // NO str_id: DragScalar reads mouse button 0 only, so right-click is unclaimed and the drag's own id is the
        // popup id. NEVER call this with nullptr after EndGroup() (a group's ItemAdd uses id 0 -- an IM_ASSERT).
        drawFieldResetMenu(context, primary, entry, field, i, /*strId=*/nullptr);
        ImGui::PopID();
    }
    ImGui::EndGroup();
    // EndGroup's item is the whole row, so this is the cell minus the row as DRAWN -- negative on an overrun.
    lastAxisRowSlackValue = total - ImGui::GetItemRectSize().x;
    return edited;
}

void InspectorPanel::drawNamedSelector(PanelContext& context, Entity primary, const ComponentEntry& entry,
                                       const FieldEntry& field, const NamedSelectorRow& selector) {
    ++namedSelectorsDrawnValue;  // a row that SUBMITTED a selector -- the consequence the seam cannot fake
    std::optional<std::size_t> chosen;
    // drawField's SetNextItemWidth(-1.0F) sizes this combo to the value cell.
    if (ImGui::BeginCombo("##v", selector.preview.c_str())) {  // EndCombo ONLY when this returned true
        for (std::size_t i = 0; i < selector.labels.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));  // 1:1 with PopID below -- nothing between them returns
            const bool current = selector.selected == i;
            if (ImGui::Selectable(selector.labels[i].c_str(), current)) {
                chosen = i;
            }
            if (current) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    // THE SEAM joins HERE and assigns the SAME local a click assigns -- one code path downstream.
    if (frameNamedSelection.has_value() && frameNamedSelection->matches(entry.name, field.name)) {
        chosen = frameNamedSelection->index;
        frameNamedSelection.reset();
    }
    // A DISCRETE write: resetField breaks the merge chain on BOTH sides and drops both edit caches -- a pick
    // must never merge with a drag on this field a moment earlier (E.3.3's pick posture). An out-of-range
    // index and the CURRENT value both push nothing: "clearing nothing would push an undo entry that changes
    // no byte" (guidFieldRow's rule). The merge-chain gate pair is not read for this row: a combo is not a
    // continuous gesture, and resetField already brackets its one push.
    if (chosen.has_value() && *chosen < selector.labels.size() && chosen != selector.selected) {
        resetField(context, primary, entry, field, namedSelectorValue(field.kind, *chosen));
    }
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
    lastLabelFontNameValue = ImGui::GetFont()->GetDebugName();  // task E.6.1: Body
    roleText(ChromeRole::TextLabel, field.label);  // task E.6.3 (D5): the humanised label; the raw name is the id
    // D5: the raw reflected name -- the scene JSON's spelling, and the one ADR-004's script bindings will use -- is
    // the label's tooltip. roleText ends in TextUnformatted, so the hover read is the label's; the tooltip window's
    // End restores LastItemData (imgui.cpp:8848), so the reset menu below still anchors on the label (I343).
    if (field.label != field.name && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", field.name.c_str());  // a runtime string is never a format string (I242(e))
    }
    drawRecordValue.lastLabelDrawn = field.label;
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
            // task E.5.2: a LABELLED selector draws the named dropdown INSTEAD of the drag -- chosen by the
            // field's own metadata alone (namedSelectorRow), never by a component or field name. Anything
            // namedSelectorRow refuses falls through to today's drag, byte for byte.
            if (const auto selector = namedSelectorRow(field); selector.has_value()) {
                drawNamedSelector(context, primary, entry, field, *selector);
                break;
            }
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
            // task E.5.2: the Int arm's rule, the unsigned arm -- metadata alone chooses the dropdown.
            if (const auto selector = namedSelectorRow(field); selector.has_value()) {
                drawNamedSelector(context, primary, entry, field, *selector);
                break;
            }
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
            // task E.3.3: the row is still chosen by KIND, never by component or field name -- the
            // standing rule that keeps the inspector generic. What is NEW is that the field says which
            // kind it WANTS, through its own AERO_ASSET token; an unresolved token is UNCONSTRAINED
            // plus one WARN on the open frame, never a refused row.
            //
            // 3.1.5's D14 is SUPERSEDED, not contradicted: there is exactly ONE assignment surface for
            // a field, and it is the widget's -- this file still names no ImGui drop API at all, which
            // IR8 clause (a) keeps pinning. Their names are deliberately not spelled here: that pin
            // strips comments, but the plain grep a reader would reach for does not.
            //
            // Both decisions -- the sentence and whether Clear is live -- come from ONE pure call, so
            // this panel holds no second copy of either and a tier-0 case asserts what is drawn here.
            // task E.4.5: a MATERIAL reference names its document, so a field bound to "Studio Brass" says so
            // instead of "mat_04.aeromat". The same three calls every host makes: the card rides the record's
            // thumbnail key on its OWN queue, and rule 4 runs against the file's CURRENT name. "" for every
            // other kind, and until the card has been read -- the sentence is then byte-identical to today's.
            // `subtitle` views the card, which is stable for this whole draw walk.
            std::string_view subtitle;
            if (thumbnails != nullptr && database != nullptr) {
                if (const AssetRecord* const record = database->findByGuid(std::get<Guid>(field.value));
                    record != nullptr) {
                    if (const std::optional<ThumbnailKey> key = thumbnailKeyForRecord(*record); key.has_value()) {
                        thumbnails->noteCardWanted(*key);
                        subtitle = materialCardSubtitle(thumbnails->cardFor(*key), leafOf(record->relativePath));
                    }
                }
            }
            const GuidFieldRow row = guidFieldRow(std::get<Guid>(field.value), database, subtitle);
            const std::optional<AssetKind> wanted = assetReferenceKindFromToken(field.assetKindToken);
            const std::string_view unknown = (!field.assetKindToken.empty() && !wanted.has_value())
                                                 ? std::string_view(field.assetKindToken)
                                                 : std::string_view{};
            AssetFieldResult picked{};
            if (assetPicker != nullptr) {
                fieldKeyScratch = inspectorAssetFieldKey(entry.name, field.name);
                // THE WIDTH IS THIS HOST'S ARITHMETIC AND IT IS SHARED (task E.3.4): `Clear` shares
                // this row, so a -FLT_MIN button would push it off the row edge. The formula and its
                // reasoning now live in asset_picker.cpp beside the button it sizes, so the material
                // slot's row uses the same one.
                const float buttonWidth = assetReferenceFieldWidth("Clear");
                // A designated initialiser must follow DECLARATION order (E.2.2's finding 3);
                // `idSuffix` is omitted because its default is right.
                const AssetFieldInputs inputs{.valueText = row.text,
                                              .buttonWidth = buttonWidth,
                                              .current = std::get<Guid>(field.value),
                                              .rules = AssetPickerRules{DropSurface::AssetField, wanted},
                                              .hostId = id(),
                                              .fieldKey = fieldKeyScratch,
                                              .unknownToken = unknown,
                                              .database = database,
                                              .thumbnails = thumbnails};
                picked = drawAssetReferenceField(inputs, *assetPicker);
                const ReferenceFieldRecord& drew = picked.drawn;  // task E.6.3: what the field drew
                drawRecordValue.referenceSwatchDrawn = drawRecordValue.referenceSwatchDrawn || drew.swatchDrawn;
                drawRecordValue.referenceWellDrawn = drawRecordValue.referenceWellDrawn || drew.wellDrawn;
                drawRecordValue.referenceFields.push_back(drew);
            } else {
                ImGui::TextUnformatted(row.text.c_str());  // no picker lent (a bare panel): 3.1.5's row
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!row.clearEnabled);
            // task E.6.3 (D5): Clear stays -- a real one-click action -- in the ghost style.
            const bool cleared = ghostButton("Clear");
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
            // D9: a pick is a DISCRETE write -- resetField's operation exactly (the chain broken on
            // BOTH sides, both caches dropped) -- and one that changes nothing pushes nothing, which
            // is guidFieldRow's own rule at this row's precision. The two refusals are the HOST's,
            // because only the host holds the current value.
            const Guid current = std::get<Guid>(field.value);
            switch (picked.outcome) {  // NO default: a fifth outcome is a -Wswitch error
                case AssetFieldOutcome::Picked:
                case AssetFieldOutcome::Dropped:
                    if (picked.guid != current) {
                        resetField(context, primary, entry, field, FieldValue{picked.guid});
                    }
                    break;
                case AssetFieldOutcome::Cleared:
                    if (current.valid()) {
                        resetField(context, primary, entry, field, FieldValue{Guid{}});
                    }
                    break;
                case AssetFieldOutcome::None:
                    break;
            }
            break;
        }
    }

    ImGui::PopID();
}

// task E.6.3 (D5): outside every card, centred on the `panel` ground. An InvisibleButton whose ONLY outline is
// dashedRect -- never ghostButton, GhostStyle or a plain Button, whose RenderFrame(..., true, ...) strokes a solid
// border through the dash gaps (imgui_widgets.cpp:812). Hovered a `hover` fill, held an `active` fill, under the
// dashes.
void InspectorPanel::drawAddComponent(PanelContext& context, Entity primary) {
    const float s = currentUiScale();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const AddComponentLayout layout = addComponentLayout(start.x, ImGui::GetContentRegionAvail().x,
                                                         faceTextWidth(PanelFace::Strong, ADD_COMPONENT_LABEL), s);
    ImGui::SetCursorScreenPos(ImVec2(layout.minX, start.y));
    const bool pressed = ImGui::InvisibleButton("##addComponentButton", ImVec2(layout.width, layout.height));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* const list = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().FrameRounding;
    if (ImGui::IsItemActive()) {
        roleFill(list, min, max, ChromeRole::Active, rounding);
    } else if (ImGui::IsItemHovered()) {
        roleFill(list, min, max, ChromeRole::Hover, rounding);
    }
    // What the list RECEIVES across the outline, never a flag set beside the call (the code-review round's finding
    // 11): each dash is one AddRectFilled at alpha 255 with no rounding, four vertices (imgui_draw.cpp:1530-1538).
    const int dashVertices = list->VtxBuffer.Size;
    dashedRect(list, min, max);
    drawRecordValue.addComponentDashVertices = list->VtxBuffer.Size - dashVertices;
    // The gaps stay open (D5), read off the same vertices: PrimRect writes a, (b.x, a.y), b, (a.x, b.y)
    // (imgui_draw.cpp:776-779) and the top run draws first, so [0]-[1] spans the first dash and [4] starts the next.
    if (drawRecordValue.addComponentDashVertices >= 8) {
        const float dashFrom = list->VtxBuffer[dashVertices].pos.x;
        const float dashTo = list->VtxBuffer[dashVertices + 1].pos.x;
        drawRecordValue.addComponentFirstDashWidth = dashTo - dashFrom;
        drawRecordValue.addComponentFirstDashGap = list->VtxBuffer[dashVertices + 4].pos.x - dashTo;
    }
    const float labelY = min.y + ((max.y - min.y - panelFacePoints(PanelFace::Strong)) * 0.5F);
    drawRoleText(list, PanelFace::Strong, ImVec2(min.x + (EDITOR_THEME.panels.addComponentPaddingX * s), labelY),
                 ChromeRole::Accent, ADD_COMPONENT_LABEL);
    drawRecordValue.addComponentMin = Vec2{min.x, min.y};
    drawRecordValue.addComponentMax = Vec2{max.x, max.y};
    drawRecordValue.addComponentDashed = drawRecordValue.addComponentDashVertices > 0;
    if (pressed) {
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
            const std::string_view fullName = context.world.componentTypeName(candidateId);
            shortNameScratch = std::string(shortComponentName(fullName));
            // Review finding 4: keyed on the loop index -- two registered types can share a short name.
            ImGui::PushID(static_cast<int>(i));
            // task E.6.3 (D5): each entry with its component's icon (componentIconFor: a view of a literal, so
            // .data() is NUL-terminated; an unknown type answers the fallback glyph).
            if (ImGui::MenuItemEx(shortNameScratch.c_str(), componentIconFor(fullName).data())) {
                pending = PendingAction{.kind = ActionKind::AddComponent, .type = candidateId};
            }
            ImGui::PopID();
        }
        if (!anyAbsent) {
            ImGui::MenuItem("(none)", nullptr, false, false);  // E12: disabled, never omitted
        }
        ImGui::EndPopup();
    }
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

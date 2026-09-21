// Aero Engine — the Material panel's ONE ImGui TU (task 3.4.2). Draws the reconciled material edit
// session and WRITES NOTHING (INV-3): EditorApp::tick() is the only place a request becomes a file
// write or a session mutation, exactly the "record a pending action, apply it after the walk" rule
// every panel in this tree follows.
//
// CLAMP-THEN-STORE IN C++, EVERY TIME (AC-18). The widget is NEVER the enforcement: an ImGui slider
// with a v_min/v_max still lets Ctrl+Click type any number at all, so every numeric edit passes
// through clampUnit/clampNonNegative before it enters the document. Both are NaN-safe by NEGATED
// comparison (`if (!(v >= lo))`), gridColumnsFor's own posture, and both bound to the SAME numbers
// material_format.cpp's UNIT_RANGE / NON_NEGATIVE_RANGE use -- a finite float max above, never an
// infinity, because an infinite factor would validate, write as `null` and fail to re-parse. So an
// interactively-produced document always passes validateMaterial, and Apply's own validation is the
// belt rather than the braces.
//
// A dynamic string is NEVER a format argument (project_settings_panel.cpp's own rule, applied here a
// third time): every draw call goes through a named local built with std::format, then passed as a
// "%s" argument.
//
// ASCII ONLY in every literal (3.1.3's post-merge lesson): the editor loads no font of its own, and
// ImGui's ProggyClean covers Basic + Extended Latin only.
#include "material_panel.hpp"

#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/asset_view.hpp>
#include <aero/editor/material_edit.hpp>
#include <aero/editor/material_inspector_model.hpp>
#include <aero/editor/panel_context.hpp>
#include <aero/editor/project_files.hpp>
#include <aero/reflect/material_format.hpp>
#include <aero/render/material.hpp>

#include "asset_picker.hpp"  // task E.3.3 -- the ONE asset-reference field widget
#include "text_input.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <imgui.h>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>  // task 3.1.5: std::exchange -- the pending slot drop's one-frame life

namespace engine::editor {

namespace {

// The header's own restated count, checked rather than trusted.
static_assert(MaterialPanel::SLOT_COUNT == render::MATERIAL_TEXTURE_SLOT_COUNT);

constexpr ImVec4 WARNING_COLOR{1.0F, 0.4F, 0.4F, 1.0F};  // project_ui.cpp's own error-text colour
constexpr ImVec4 NOTICE_COLOR{1.0F, 0.8F, 0.4F, 1.0F};   // a warm amber for the non-fatal notices

// D7/E9's rule from the viewport, verbatim: GetContentRegionAvail() is in LOGICAL units and a GPU
// allocation must be sized in PIXELS. A non-finite or non-positive scale falls back to 1.0, spelled
// with the negated `>` so NaN takes the fallback branch.
[[nodiscard]] std::uint32_t toPixels(float logical, float scale) noexcept {
    const float safeScale = (scale > 0.0F) ? scale : 1.0F;
    const long rounded = std::lround(static_cast<double>(logical) * static_cast<double>(safeScale));
    return rounded < 1 ? 1U : static_cast<std::uint32_t>(rounded);
}

// docs/09 section 11.1's two ranges, spelled exactly as material_format.cpp's UNIT_RANGE and
// NON_NEGATIVE_RANGE spell them. NaN fails the negated comparison and lands on the low bound, which is
// the only value that is both in range and not a guess.
[[nodiscard]] float clampUnit(float v) noexcept {
    if (!(v >= 0.0F)) {
        return 0.0F;
    }
    if (!(v <= 1.0F)) {
        return 1.0F;
    }
    return v;
}

[[nodiscard]] float clampNonNegative(float v) noexcept {
    if (!(v >= 0.0F)) {
        return 0.0F;
    }
    if (!(v <= std::numeric_limits<float>::max())) {
        return std::numeric_limits<float>::max();  // +inf and NaN both land on a FINITE bound
    }
    return v;
}

[[nodiscard]] std::uint32_t clampUvSet(int v) noexcept {
    if (v < 0) {
        return 0;
    }
    if (static_cast<std::uint32_t>(v) >= MATERIAL_MAX_UV_SETS) {
        return MATERIAL_MAX_UV_SETS - 1;
    }
    return static_cast<std::uint32_t>(v);
}

// ---- the four token combos ----------------------------------------------------------------------
// Each iterates an EXHAUSTIVE array of its enum, previewing and listing through the format's OWN label
// functions -- so the combo's vocabulary is the file's vocabulary by construction and a label edit is
// a format change, never a UI cosmetic. docs/09 section 11.6 makes adding a token a version bump, so a
// fifth enumerator is a deliberate act that updates the array beside everything else it touches.
constexpr std::array<MaterialAlphaMode, 3> ALPHA_MODE_VALUES{MaterialAlphaMode::Opaque, MaterialAlphaMode::Mask,
                                                             MaterialAlphaMode::Blend};
constexpr std::array<MaterialWrap, 3> WRAP_VALUES{MaterialWrap::Repeat, MaterialWrap::Clamp, MaterialWrap::Mirror};
constexpr std::array<MaterialFilter, 2> FILTER_VALUES{MaterialFilter::Nearest, MaterialFilter::Linear};
constexpr std::array<MaterialMipFilter, 3> MIP_FILTER_VALUES{MaterialMipFilter::None, MaterialMipFilter::Nearest,
                                                             MaterialMipFilter::Linear};

// BeginCombo/EndCombo is the ASYMMETRIC pair (like BeginMenu): EndCombo runs ONLY when BeginCombo
// returned true. Getting that backwards is an IM_ASSERT abort in the Debug build, not a visual glitch.
template <typename Enum, std::size_t N, typename LabelFn>
[[nodiscard]] bool tokenCombo(const char* label, Enum& value, const std::array<Enum, N>& values, LabelFn labelOf,
                              std::string& scratch) {
    bool changed = false;
    scratch = std::string(labelOf(value));
    if (ImGui::BeginCombo(label, scratch.c_str())) {
        for (const Enum candidate : values) {
            const bool selected = candidate == value;
            const std::string text(labelOf(candidate));
            if (ImGui::Selectable(text.c_str(), selected)) {
                value = candidate;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// project_settings_panel.cpp's own TABLE_FLAGS, verbatim and for its reasons: a fixed label column, a
// stretch value column, resizable by the user, and NoSavedSettings so a table's width is never a
// persisted format.
// ONE sampler row inside the disclosure's table: the label cell, then the combo filling the value
// cell. The label is a plain literal here rather than a model string, deliberately -- the six sampler
// tokens are the FORMAT's own vocabulary (docs/09 section 11.1's slot table) and their row labels name
// members of that table, not fields of the document the model enumerates. `##`-prefixed ids keep the
// label out of the widget, exactly as the scalar rows do.
template <typename Enum, std::size_t N, typename LabelFn>
[[nodiscard]] bool samplerTokenRow(const char* label, const char* id, Enum& value, const std::array<Enum, N>& values,
                                   LabelFn labelOf, std::string& scratch) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-1.0F);
    return tokenCombo(id, value, values, labelOf, scratch);
}

constexpr ImGuiTableFlags MATERIAL_TABLE_FLAGS =
    ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings;

// ONE pass over every label in every section, so all eight tables get the SAME column-0 width and the
// panel reads as ONE form rather than as eight unrelated tables. Ten CalcTextSize calls, and `Alpha
// cutoff` is measured whether or not its row is drawn this frame -- measuring only what is on screen
// would make the column jump when the alpha mode changes.
//
// WHAT IT DOES NOT DO: re-widen the column after a RUNTIME font-scale change. TableSetupColumn's init
// width reaches column->WidthRequest only under IsInitializing, and the one per-frame re-apply path is
// gated on !column_is_resizable -- project_settings_panel.cpp:59-70 records the full analysis with its
// line numbers and it is NOT restated here. Consequence, stated once: labels WRAP rather than clip
// after such a change, and the user can drag the divider.
//
// materialFieldLabel returns a string_view over a STRING LITERAL, which the model's header states as a
// contract -- so .data() is NUL-terminated and CalcTextSize(const char*) is safe.
[[nodiscard]] float widestMaterialLabel() {
    float widest = 0.0F;
    for (const MaterialSection& section : materialSections()) {
        for (const MaterialFieldId field : section.fields) {
            widest = std::max(widest, ImGui::CalcTextSize(materialFieldLabel(field).data()).x);
        }
    }
    return widest + (ImGui::GetStyle().CellPadding.x * 2.0F);
}

// ---- ONE scalar row's VALUE CELL (task E.3.4) -----------------------------------------------------
// 3.4.2's drawScalarRows, arm for arm, behind a switch with NO `default:` so an eleventh field is a
// -Wswitch error rather than a row that silently draws nothing. Every clamp, every widget, every flag
// and both of AC-18's and AC-19's rules cross VERBATIM -- this is a re-shaping of the caller, not of
// the controls.
//
// THE CALLER OWNS THE ROW AND THIS OWNS THE CELL (E.3.1's InspectorPanel::drawField shape): the label
// cell, TableNextRow, TableNextColumn and SetNextItemWidth all happen above, so every widget below
// passes "" as its label -- the label column already said what it is.
//
// alphaCutoff's conditionality is the CALLER's too, and deliberately: skipping it here would submit a
// label cell and an EMPTY value cell.
[[nodiscard]] bool drawFieldRow(MaterialFieldId field, MaterialDocument& form, std::string& nameDraft,
                                bool& nameEditing, std::string& scratch) {
    bool changed = false;
    switch (field) {
        case MaterialFieldId::Name: {
            // `name` commits on deactivate-after-edit (AC-17). The draft is re-synced on every frame
            // the widget is NOT active, which is also how a retarget reaches it -- see the header's
            // own note on why a per-frame copy of form.name cannot carry the gesture.
            if (!nameEditing) {
                nameDraft = form.name;
            }
            inputTextString("##name", nameDraft, ImGuiInputTextFlags_None);
            nameEditing = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                form.name = nameDraft;
                changed = true;
            }
            break;
        }
        case MaterialFieldId::BaseColorFactor: {
            std::array<float, 4> base{form.baseColorFactor.x, form.baseColorFactor.y, form.baseColorFactor.z,
                                      form.baseColorFactor.w};
            if (ImGui::ColorEdit4("##tint", base.data(), ImGuiColorEditFlags_Float)) {
                form.baseColorFactor.x = clampUnit(base[0]);
                form.baseColorFactor.y = clampUnit(base[1]);
                form.baseColorFactor.z = clampUnit(base[2]);
                form.baseColorFactor.w = clampUnit(base[3]);
                changed = true;
            }
            break;
        }
        case MaterialFieldId::MetallicFactor: {
            float metallic = form.metallicFactor;
            if (ImGui::SliderFloat("##metallic", &metallic, 0.0F, 1.0F)) {
                form.metallicFactor = clampUnit(metallic);
                changed = true;
            }
            break;
        }
        case MaterialFieldId::RoughnessFactor: {
            float roughness = form.roughnessFactor;
            if (ImGui::SliderFloat("##roughness", &roughness, 0.0F, 1.0F)) {
                form.roughnessFactor = clampUnit(roughness);
                changed = true;
            }
            break;
        }
        case MaterialFieldId::EmissiveFactor: {
            // HDR only for emissive: docs/09 section 11.1 leaves it unbounded above (the lights'
            // precedent), and baseColorFactor is a [0,1] tint that an HDR picker would invite somebody
            // to break.
            std::array<float, 3> emissive{form.emissiveFactor.x, form.emissiveFactor.y, form.emissiveFactor.z};
            if (ImGui::ColorEdit3("##emissive", emissive.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)) {
                form.emissiveFactor.x = clampNonNegative(emissive[0]);
                form.emissiveFactor.y = clampNonNegative(emissive[1]);
                form.emissiveFactor.z = clampNonNegative(emissive[2]);
                changed = true;
            }
            break;
        }
        case MaterialFieldId::NormalScale: {
            // v_min == v_max == 0 is ImGui's "no bound at all": the C++ clamp below is the ONLY
            // enforcement, which is exactly AC-18's point rather than an oversight.
            float normalScale = form.normalScale;
            if (ImGui::DragFloat("##normalscale", &normalScale, 0.01F, 0.0F, 0.0F, "%.3f")) {
                form.normalScale = clampNonNegative(normalScale);
                changed = true;
            }
            break;
        }
        case MaterialFieldId::OcclusionStrength: {
            float occlusion = form.occlusionStrength;
            if (ImGui::SliderFloat("##occlusion", &occlusion, 0.0F, 1.0F)) {
                form.occlusionStrength = clampUnit(occlusion);
                changed = true;
            }
            break;
        }
        case MaterialFieldId::AlphaMode:
            changed = tokenCombo("##alphamode", form.alphaMode, ALPHA_MODE_VALUES, materialAlphaModeLabel, scratch);
            break;
        case MaterialFieldId::AlphaCutoff: {
            // AC-19: the ROW is conditional -- in the CALLER -- and the VALUE is not. The format stores
            // alphaCutoff whatever the mode is, so switching to opaque and back must return the number
            // the user chose, not 0.5.
            float cutoff = form.alphaCutoff;
            if (ImGui::SliderFloat("##alphacutoff", &cutoff, 0.0F, 1.0F)) {
                form.alphaCutoff = clampUnit(cutoff);
                changed = true;
            }
            break;
        }
        case MaterialFieldId::DoubleSided: {
            bool doubleSided = form.doubleSided;
            if (ImGui::Checkbox("##doublesided", &doubleSided)) {
                form.doubleSided = doubleSided;
                changed = true;
            }
            break;
        }
    }
    return changed;
}

}  // namespace

// ---- one texture slot's ROW (task E.3.4, replacing 3.4.2's drawSlotSection) ------------------------
// ONE ImGui item carries the whole reference: the E.3.3 picker's button, grown tall enough to hold the
// bound asset's thumbnail inside its own frame, with `Clear` beside it on the same row and the
// colour-space note beneath. One press target, one drop target, one popup anchor.
//
// PushID/PopID are 1:1 across EVERY path through this function -- no continue, no break, no return
// between them (an unbalanced id stack is an IM_ASSERT abort in the Debug ImGui build).
bool MaterialPanel::drawSlotRow(std::size_t index, MaterialDocument& form, const MaterialSlotRow& row,
                                float thumbEdge) {
    bool changed = false;
    ImGui::PushID("slot");
    std::optional<MaterialTextureSlot>& slot = documentSlotAt(form, index);
    AssetFieldResult picked{};
    if (assetPicker != nullptr) {
        // materialSlotFieldKey returns std::string BY VALUE and AssetFieldInputs::fieldKey is a
        // string_view, so the key MUST land in a member first: inlining it into the aggregate below
        // leaves the view dangling for the whole call. "slot:0" is inside libc++'s SSO buffer, so the
        // bytes usually survive and NOTHING reddens -- E.3.2's recorded lesson, one host over, and
        // ASan's detect_stack_use_after_return is off by default here. The SAME applies to
        // `row.valueText`, which is why `row` is a caller-owned named local that outlives this call.
        keyScratch = materialSlotFieldKey(index);
        const AssetFieldInputs inputs{.valueText = row.valueText,
                                      // `Clear` shares this row NOW, so the width is the SHARED
                                      // formula's -- not -FLT_MIN, which is what has put Clear on its
                                      // own line since 3.4.2 and is exactly what this task changes.
                                      .buttonWidth = assetReferenceFieldWidth("Clear"),
                                      .current = slot.has_value() ? slot->guid : Guid{},
                                      .rules = AssetPickerRules{DropSurface::MaterialSlot, std::nullopt},
                                      .hostId = id(),
                                      .fieldKey = keyScratch,
                                      .unknownToken = std::string_view{},
                                      .database = databasePtr,
                                      .thumbnails = thumbnails,
                                      .thumbnailEdge = thumbEdge};
        picked = drawAssetReferenceField(inputs, *assetPicker);
    }
    switch (picked.outcome) {  // NO default: a fifth outcome is a -Wswitch error
        case AssetFieldOutcome::Picked:
        case AssetFieldOutcome::Dropped: {
            // EXACTLY 3.4.2's idiom: a REBIND keeps the slot's sampler tokens, a FRESH bind takes the
            // format's own defaults, which MaterialTextureSlot{} already is.
            MaterialTextureSlot bound = slot.has_value() ? *slot : MaterialTextureSlot{};
            bound.guid = picked.guid;
            slot = bound;
            changed = true;
            if (picked.outcome == AssetFieldOutcome::Dropped) {
                // Reported so tick() sees the SAME thing for a real gesture as for the seam: the
                // drain's only job here is the vanished-guid refusal WARN, and a warning that fired
                // only for driven drops would be a warning nobody ever sees.
                observedSlotDrop = MaterialSlotTextureDrop{.slot = index, .textureGuid = picked.guid};
            }
            break;
        }
        case AssetFieldOutcome::Cleared:
            slot.reset();  // AC-20: the WHOLE slot, never a nil guid -- absence is spelled by omission
            changed = true;
            break;
        case AssetFieldOutcome::None:
            break;
    }

    // `Clear` SHARES the row now. A SmallButton has no FramePadding.y, so on a tall picker button it
    // sits at the row's baseline rather than its middle -- AlignTextToFramePadding aligns to the
    // DEFAULT frame height and would fight the tall button, so this is SameLine and nothing else.
    ImGui::SameLine();
    ImGui::BeginDisabled(!row.clearEnabled);  // 1:1 with EndDisabled; nothing exits between them
    if (ImGui::SmallButton("Clear")) {
        slot.reset();  // AC-20: the WHOLE slot, never a nil guid
        changed = true;
    }
    ImGui::EndDisabled();

    // The colour-space note, composed from materialSlotIsSrgb by the model. A DYNAMIC string is never a
    // format string -- and a static one goes through "%s" too, so there is ONE rule here rather than a
    // judgement at every call.
    ImGui::TextDisabled("%s", materialColorSpaceNote(index).data());

    // THE NOTICES STAY ON THE CHANNEL, never inside the disclosure: every one of them explains
    // something the user can SEE without opening anything.
    if (row.notice != MaterialSlotNotice::None) {
        ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
        ImGui::TextWrapped("%s", materialSlotNoticeText(row.notice).data());
        ImGui::PopStyleColor();
    }
    // What the PREVIEW made of it (3.4.2's D7/AC-21), unchanged: exactly ONE row, and only when there
    // is something to say. The refusal's own sentence comes from the loader, which is the only thing
    // that knows whether the file was missing, a .hdr, undecodable, uncookable or refused by the GPU.
    // No `default:` -- a fifth state is a -Wswitch failure rather than a slot that silently says
    // nothing.
    switch (preview.slotTextureState(index)) {
        case PreviewTextureState::Loading:
            ImGui::TextDisabled("%s", "Loading the preview texture...");
            break;
        case PreviewTextureState::Failed:
            labelScratch = std::string(preview.slotNotice(index));
            ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
            ImGui::TextWrapped("%s", labelScratch.c_str());
            ImGui::PopStyleColor();
            break;
        case PreviewTextureState::None:
        case PreviewTextureState::Ready:
            break;
    }
    // AC-22, the v1 rule, and OUTSIDE the disclosure deliberately: MeshVertex carries one UV set, so a
    // consumer honours set 0 and WARNs. The value is STORED for fidelity -- this note is why it looks
    // ignored, and a user who has not opened the sampler node still needs to read it.
    if (slot.has_value() && slot->uvSet != 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
        ImGui::TextWrapped("%s", "v1 consumers honour UV set 0; this value is stored, not sampled.");
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
    return changed;
}

// ---- the sampler disclosure (task E.3.4) ----------------------------------------------------------
// The six sampler tokens, behind a per-slot node that starts CLOSED. Drawn only while the slot is
// bound -- an unbound slot has nothing to sample and six combos over an absent slot would be six
// controls that write nowhere.
void MaterialPanel::drawSamplerDisclosure(std::size_t index, MaterialDocument& form, const MaterialSlotRow& row,
                                          bool& changed) {
    std::optional<MaterialTextureSlot>& slot = documentSlotAt(form, index);
    if (!slot.has_value()) {
        return;
    }
    // THE PANEL owns the open state. SetNextItemOpen(ImGuiCond_Always) makes TreeNodeUpdateNextOpen
    // take is_open straight from NextItemData.OpenVal (imgui_widgets.cpp:6817-6823) AND the click path
    // still runs, flipping is_open and raising ImGuiItemStatusFlags_ToggledOpen (:7073-7078), which
    // IsItemToggledOpen reads (imgui.cpp:6590-6594). So a forced-open node is STILL clickable and this
    // array is the single source of truth. ImGuiCond_Once would hand the decision to ImGui's own
    // storage instead, and a close request would then not close.
    ImGui::SetNextItemOpen(slotDetailsOpen(index), ImGuiCond_Always);
    // NoTreePushOnOpen means there is NO TreePop -- the same property that lets every CollapsingHeader
    // in this tree need none (imgui_widgets.cpp:7152-7153, TreePop at :7242). This task therefore adds
    // NO asymmetric ImGui pair at all. No Indent either: the rows are a two-column table under the node
    // and read as a sub-list without one.
    const bool open = ImGui::TreeNodeEx(materialSamplerNodeLabel(row.samplerIsDefault).data(),
                                        ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    if (ImGui::IsItemToggledOpen()) {
        setSlotDetailsOpen(index, !slotDetailsOpen(index));
    }
    if (!open) {
        return;
    }
    if (ImGui::BeginTable("##sampler", 2, MATERIAL_TABLE_FLAGS)) {  // EndTable ONLY if true
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, widestMaterialLabel());
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("UV set");
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1.0F);
        int uvSet = static_cast<int>(slot->uvSet);
        if (ImGui::DragInt("##uvset", &uvSet, 0.1F, 0, static_cast<int>(MATERIAL_MAX_UV_SETS) - 1)) {
            slot->uvSet = clampUvSet(uvSet);
            changed = true;
        }

        // Five separate statements rather than one chain of `|| changed`: every one of these lines
        // would otherwise sit within a couple of columns of the format limit, and Homebrew's
        // clang-format 18 and Ubuntu's disagree about how to break a chain that long.
        changed =
            samplerTokenRow("Wrap U", "##wrapu", slot->wrapU, WRAP_VALUES, materialWrapLabel, labelScratch) || changed;
        changed =
            samplerTokenRow("Wrap V", "##wrapv", slot->wrapV, WRAP_VALUES, materialWrapLabel, labelScratch) || changed;
        changed = samplerTokenRow("Min filter", "##minfilter", slot->minFilter, FILTER_VALUES, materialFilterLabel,
                                  labelScratch) ||
                  changed;
        changed = samplerTokenRow("Mag filter", "##magfilter", slot->magFilter, FILTER_VALUES, materialFilterLabel,
                                  labelScratch) ||
                  changed;
        changed = samplerTokenRow("Mip filter", "##mipfilter", slot->mipFilter, MIP_FILTER_VALUES,
                                  materialMipFilterLabel, labelScratch) ||
                  changed;
        ImGui::EndTable();
    }
}

// ---- the body (task E.3.4) ------------------------------------------------------------------------
// CALLED FROM BOTH body paths and from nowhere else, which is what keeps the two modes drawing the
// same thing. It submits no BeginChild of its own: whether a child surrounds it is the CALLER's
// decision and the layout's mode is what makes it.
//
// The eight sections, one PushID each, under ONE shared label-column width measured over every label
// in every section -- so all eight tables align as if they were a single form.
void MaterialPanel::drawBody(MaterialDocument& form, const MaterialPanelLayout& layout,
                             const std::optional<MaterialError>& invalid, bool& changed) {
    // E.2.4's no-sun notice, MOVED here from under the image. It WRAPS, and a wrapped line cannot live
    // in a fixed-height region -- that is the whole reason it moved. previewHasSunValue is latched in
    // the SERVICE pass and is read here unchanged, so WHEN it is true does not move.
    if (!previewHasSunValue) {
        ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
        ImGui::TextWrapped("%s", "No directional light in the scene -- the preview is lit by its environment only.");
        ImGui::PopStyleColor();
    }
    // The validation message, wrapped, at the top of the form where a reader meets it before the field
    // that caused it. The footer's status line echoes it in ONE line beside a dead Apply; this is the
    // detail, and it is here rather than in the footer precisely because it wraps.
    if (invalid.has_value()) {
        labelScratch = invalid->message;
        ImGui::PushStyleColor(ImGuiCol_Text, WARNING_COLOR);
        ImGui::TextWrapped("%s", labelScratch.c_str());
        ImGui::PopStyleColor();
    }

    // ONE measurement over EVERY label in EVERY section, so all eight tables share one column-0 width
    // and the panel reads as one form. Measured before the loop, so no section can widen the column
    // for the sections after it.
    const float labelWidth = widestMaterialLabel();

    std::size_t sectionIndex = 0;
    for (const MaterialSection& section : materialSections()) {
        ImGui::PushID(static_cast<int>(sectionIndex));  // 1:1 with the PopID at the bottom, EVERY path
        if (ImGui::CollapsingHeader(section.title.data(), ImGuiTreeNodeFlags_DefaultOpen)) {
            // materialSlotRow is computed ONCE per section, before the row, and the SAME value
            // reaches drawSlotRow and drawSamplerDisclosure. Computing it twice is not wrong, it is a
            // second place to pass the wrong record.
            //
            // `row` MUST be a named local that outlives drawAssetReferenceField: its valueText is a
            // std::string and AssetFieldInputs::valueText is a string_view, so inlining
            // materialSlotRow(...).valueText into the widget's aggregate would hand it a view into a
            // temporary that dies at the end of that full-expression.
            //
            // THE FOUR PLAIN VALUES are resolved here, by the caller, which is what keeps the model
            // free of AssetDatabase, of AssetKind and of asset_view. databaseAvailable and recordFound
            // are NOT interchangeable: without a database this panel is not entitled to say "this GUID
            // is not in this project" -- it has not read one.
            MaterialSlotRow row{};
            if (section.slot.has_value()) {
                const std::optional<MaterialTextureSlot>& slot = documentSlotAt(form, *section.slot);
                const AssetRecord* const record = (databasePtr != nullptr && slot.has_value() && slot->guid.valid())
                                                      ? databasePtr->findByGuid(slot->guid)
                                                      : nullptr;
                row = materialSlotRow(
                    slot, record != nullptr ? std::string_view(record->relativePath) : std::string_view{},
                    record != nullptr && classifyAssetKind(leafOf(record->relativePath), false) == AssetKind::Texture,
                    databasePtr != nullptr, record != nullptr);
                changed = drawSlotRow(*section.slot, form, row, layout.thumbEdge) || changed;
            }
            if (!section.fields.empty()) {
                if (ImGui::BeginTable("##rows", 2, MATERIAL_TABLE_FLAGS)) {  // EndTable ONLY if true
                    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
                    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
                    for (const MaterialFieldId field : section.fields) {
                        // AC-19's conditionality lives HERE, before TableNextRow: skipping inside
                        // drawFieldRow instead would submit a label cell and an EMPTY value cell.
                        if (field == MaterialFieldId::AlphaCutoff && form.alphaMode != MaterialAlphaMode::Mask) {
                            continue;
                        }
                        // The CALLER owns the row, the ARM owns the cell.
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(materialFieldLabel(field).data());
                        ImGui::TableNextColumn();
                        ImGui::SetNextItemWidth(-1.0F);
                        changed = drawFieldRow(field, form, nameDraft, nameEditing, labelScratch) || changed;
                    }
                    ImGui::EndTable();
                }
            }
            if (section.id == MaterialSectionId::File) {
                drawFileSection(labelWidth);
            }
            // LAST in the section, below the notices: the six sampler tokens, closed by default. It
            // re-reads the slot itself rather than trusting a flag from above, because drawSlotRow may
            // have bound or cleared it this very frame.
            if (section.slot.has_value()) {
                drawSamplerDisclosure(*section.slot, form, row, changed);
            }
        }
        ImGui::PopID();
        ++sectionIndex;
    }
}

// ---- the File section (task E.3.4) ----------------------------------------------------------------
// Read-only diagnostics: where this material lives, what identity the project gave it, and the full
// per-key list of what Apply would delete. The list LEFT the top status strip and landed here, which
// is what closes the one-commit gap the previous step opened; the footer's status line still carries
// the SUMMARY every frame.
//
// It draws its OWN two-column table with the SAME labelWidth the seven other sections use -- never a
// bare TextUnformatted run, or the one read-only section would be the one that does not line up.
void MaterialPanel::drawFileSection(float labelWidth) {
    if (sessionPtr == nullptr) {
        return;  // unreachable from the Ready arm; a null pointer is never assumed away
    }
    if (ImGui::BeginTable("##file", 2, MATERIAL_TABLE_FLAGS)) {  // EndTable ONLY if true
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, labelWidth);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Path");
        ImGui::TableNextColumn();
        labelScratch = std::string(sessionPtr->targetPath());
        ImGui::TextWrapped("%s", labelScratch.c_str());

        // The GUID row is drawn only when there IS a database to ask; "no .meta yet" is a real answer
        // and is 3.4.2's own wording, carried verbatim.
        if (databasePtr != nullptr) {
            const AssetRecord* const record = databasePtr->findByPath(sessionPtr->targetPath());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("GUID");
            ImGui::TableNextColumn();
            labelScratch =
                record == nullptr || !record->guid.valid() ? std::string("no .meta yet") : formatGuid(record->guid);
            ImGui::TextDisabled("%s", labelScratch.c_str());
        }
        ImGui::EndTable();
    }

    // The parser's own per-key list (3.4.2's engine channel). Each entry names a key Apply will DELETE,
    // which is the only half of "not canonical" worth interrupting somebody over. OUTSIDE the table
    // and wrapped, because a removed key's name is unbounded and a wrapped line inside a fixed-width
    // cell would be clipped rather than wrapped.
    for (const std::string& warning : sessionPtr->warnings()) {
        labelScratch = warning;
        ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
        ImGui::TextWrapped("%s", labelScratch.c_str());
        ImGui::PopStyleColor();
    }
}

MaterialPanel::MaterialPanel(rhi::Device& device) noexcept : preview(&device) {}

// ---- the preview strip (AC-28/AC-32) --------------------------------------------------------------
// Records a request, APPLIES THE RESIZE and reads a native handle -- in that order, which is the whole
// of the code-review round's BLOCKING-1. ImGui records the ImTextureID here and binds it in
// ImGuiLayer::endFrame, AFTER the post-draw service pass, so the allocation must be settled before the
// handle is read; MaterialPreview::prepareFrame carries the full reasoning. Nothing else GPU-shaped
// happens in this walk: every create, upload and destroy stays in the service pass (INV-5).
void MaterialPanel::drawPreview(float previewHeight) {
    // task E.3.4: NO SeparatorText here any more -- this is the HEADER now, above the body and the
    // footer, and the identity line above it already says which material this is. The Error arm's own
    // SeparatorText("Preview") + "Nothing to preview until this file parses." is untouched; I100 reads
    // it.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (!(avail.x > 0.0F)) {
        return;  // a degenerate/collapsed region: no request, no image (the viewport's own E1 rule)
    }
    // The HEIGHT is materialPanelLayout's answer, which floors it at the model's own minimum in BOTH
    // modes, so it is never zero and never negative (AC-3). The width still follows the panel.
    // The `avail.x > 0` guard above is the PANEL's and is unchanged: previewShown is the model's answer
    // about HEIGHT alone, and a zero-width panel must still cost no GPU extent.
    const ImVec2 imageSize{avail.x, previewHeight};
    const ImGuiIO& io = ImGui::GetIO();
    rhi::Extent2D pixels{toPixels(imageSize.x, io.DisplayFramebufferScale.x),
                         toPixels(imageSize.y, io.DisplayFramebufferScale.y)};
    // §6.4's cap, applied HERE and with the ASPECT PRESERVED rather than left to RenderTargetConfig's
    // maxExtent: that one clamps each axis independently, which would silently stretch the sphere on
    // any panel wider than 512 px.
    const std::uint32_t larger = std::max(pixels.width, pixels.height);
    if (larger > PREVIEW_MAX_EXTENT) {
        const double k = static_cast<double>(PREVIEW_MAX_EXTENT) / static_cast<double>(larger);
        pixels.width = static_cast<std::uint32_t>(std::max(1L, std::lround(pixels.width * k)));
        pixels.height = static_cast<std::uint32_t>(std::max(1L, std::lround(pixels.height * k)));
    }
    // The resize happens INSIDE this call, before the handle below is read (the viewport's step 5/6
    // ordering). A false return means there is no texture this frame -- including the frame an
    // allocation failed, where the previous pair has already been destroyed and must not be bound.
    const bool renderable = preview.prepareFrame(pixels);

    void* const native = renderable ? preview.nativeColorTexture() : nullptr;
    const rhi::Extent2D drawExtent = preview.drawExtent();
    const rhi::Extent2D textureExtent = preview.textureExtent();
    if (!renderable || native == nullptr || textureExtent.width == 0 || textureExtent.height == 0) {
        const char* const why = preview.unavailableReason();
        // The ONE line AC-32 asks for in a tools-OFF build, and the same line for every other reason a
        // preview is not on screen. Never an empty string: an empty TextDisabled is a blank gap that
        // reads as a rendering bug.
        ImGui::TextDisabled("%s", (why != nullptr && *why != '\0') ? why : "Preview unavailable.");
        return;
    }
    // The UV sub-rect (the viewport's D5/D6): textureExtent() >= drawExtent() on both axes, always, so
    // uvMax is in (0,1]. Both come from the allocation prepareFrame just settled, so they describe the
    // texture ImGui is about to be handed rather than the one it held last frame.
    const ImVec2 uvMax{static_cast<float>(drawExtent.width) / static_cast<float>(textureExtent.width),
                       static_cast<float>(drawExtent.height) / static_cast<float>(textureExtent.height)};
    // ImTextureID is an ImU64 holding the raw native texture pointer (viewport_panel.cpp's step 8); a
    // pointer-to-integer conversion is the only way to spell that.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto texId = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(native));
    ImGui::Image(texId, imageSize, ImVec2(0, 0), uvMax);
    // task E.3.4: E.2.4's no-sun notice has MOVED OUT of here and into the body. It WRAPS, and a
    // wrapped line inside the header would make the header's height a function of the panel's WIDTH,
    // which would make the preview's height one too -- so dragging the dock divider sideways would
    // reallocate the render target. It reads the same latched previewHasSunValue on the same tick, so
    // WHEN it is true has not moved.
}

void MaterialPanel::servicePreview(MaterialSession& session, const AssetDatabase& database,
                                   std::string_view assetsRootAbs, float deltaSeconds,
                                   const render::TonemapParams& tonemap, const MaterialPreviewLighting& lighting) {
    // The one-shot is drained as its OWN statement, unconditionally, before it is inspected (F9's
    // ||-short-circuit rule, applied to a channel that crosses into the GPU layer).
    const bool documentChanged = session.takeDocumentChanged();
    // task E.2.4: latched HERE, read by the next draw walk. One frame late, and invisible.
    previewHasSunValue = lighting.hasSun;
    preview.service(session.document(), documentChanged, &database, assetsRootAbs, deltaSeconds, tonemap, lighting);
}

void MaterialPanel::onDraw(PanelContext& /*context*/) {  // no World/Selection/Project read (the
                                                         // ImportDetailsPanel "context is ignored"
                                                         // precedent)
    // task 3.1.5: the seam's pending slot drop is consumed HERE, at the top, BEFORE any early return
    // -- a one-frame life whatever path this draw takes. Consuming it at the fold point instead let a
    // drop driven at an UNTARGETED panel survive until some later material was selected and then bind
    // a slot nobody asked for, which is the one thing this channel must never do (found by DP12).
    const std::optional<MaterialSlotTextureDrop> slotDrop = std::exchange(pendingSlotDrop, std::nullopt);
    if (sessionPtr == nullptr) {
        // The very first frame of a session's life: EditorApp::tick() reconciles BEFORE drawShellUi,
        // so in practice this is reached only with no panel registration at all -- but a null pointer
        // is always checked here rather than assumed away.
        ImGui::TextDisabled("Select a material in the Assets panel.");
        return;
    }
    switch (sessionPtr->state()) {
        case MaterialSessionState::Untargeted:
            ImGui::TextDisabled("Select a material in the Assets panel.");
            return;
        case MaterialSessionState::Error: {
            labelScratch = std::string(sessionPtr->targetPath());
            ImGui::TextUnformatted(labelScratch.c_str());
            const MaterialError* error = sessionPtr->error();
            if (error != nullptr) {
                // line > 0 <=> the failure happened at the JSON stage and carries a position;
                // material-stage failures put their context (the key path) in the message instead.
                labelScratch = error->line > 0 ? std::format("{} ({}:{})", error->message, error->line, error->column)
                                               : error->message;
                ImGui::PushStyleColor(ImGuiCol_Text, WARNING_COLOR);
                ImGui::TextWrapped("%s", labelScratch.c_str());
                ImGui::PopStyleColor();
            }
            // Nothing editable, and no Apply or Revert drawn at all (AC-9): the file may hold a
            // hand-recoverable value one `git checkout` away, and this editor never "repairs" one.
            ImGui::TextDisabled("This file cannot be edited until it parses.");
            // NO IMAGE HERE, and the section says so rather than going missing. Calling drawPreview()
            // would blit the LAST GOOD MATERIAL's picture under this error text: the render target
            // keeps whatever was last rendered into it, and service() refuses to render with no
            // document -- so the image would be a stale frame of a different material, presented as if
            // it were this file. The code-review round's finding 9; materialPreviewImageCount() is what
            // pins it, because a stale picture and a correct one look identical to every tier here.
            ImGui::SeparatorText("Preview");
            ImGui::TextDisabled("%s", "Nothing to preview until this file parses.");
            return;
        }
        case MaterialSessionState::Ready:
            break;
    }
    const MaterialDocument* document = sessionPtr->document();
    if (document == nullptr) {
        // Unreachable through state() == Ready, which already implies an engaged session copy --
        // bugprone-unchecked-optional-access's sibling problem, and a null deref is never assumed away.
        ImGui::TextDisabled("Select a material in the Assets panel.");
        return;
    }

    // ---- the retarget reset (task E.3.4) ---------------------------------------------------------
    // slotDetails is UI state about THIS material, so a new target starts every disclosure CLOSED.
    // The detection lands here, above everything, because every path below this point draws the form.
    // targetPath() returns a string_view, so the member takes an explicit std::string construction.
    if (sessionPtr->targetPath() != lastTargetPath) {
        slotDetails.fill(false);
        lastTargetPath = std::string(sessionPtr->targetPath());
    }

    // ---- the geometry, read ONCE -----------------------------------------------------------------
    // Six live style reads, never a literal. separatorHeight above all: ImGui 1.92.8 REMOVED the
    // "a 1 px Separator does not move the cursor" hack -- the line that implemented it is commented
    // out at imgui_widgets.cpp:1706 and ItemSize(ImVec2(0.0f, thickness)) now runs unconditionally --
    // while SeparatorEx's own header comment at :1657 still DESCRIBES the removed hack. Read the code,
    // not the comment. Separator() passes ImMax(style.SeparatorSize, 1.0f) (:1741), and ScaleAllSizes
    // does SeparatorSize = ImTrunc(SeparatorSize * scale) (imgui.cpp:1645) -- which this editor CALLS
    // unconditionally at imgui_layer.cpp:87-89 -- so the value is 1 only while the window's display
    // scale is 1. Measured on this machine at a scale of 2: it is 2.
    const ImGuiStyle& style = ImGui::GetStyle();
    const MaterialPanelLayout layout =
        materialPanelLayout(MaterialPanelMetrics{.availHeight = ImGui::GetContentRegionAvail().y,
                                                 .fontSize = ImGui::GetFontSize(),
                                                 .frameHeight = ImGui::GetFrameHeight(),
                                                 .textLineHeight = ImGui::GetTextLineHeight(),
                                                 .itemSpacingY = style.ItemSpacing.y,
                                                 .separatorHeight = std::max(style.SeparatorSize, 1.0F)});

    // ---- HEADER ----------------------------------------------------------------------------------
    labelScratch = std::string(sessionPtr->targetPath());
    if (sessionPtr->dirty()) {
        labelScratch += " *";  // VERBATIM today's suffix, so nothing that reads it regresses
    }
    ImGui::TextUnformatted(labelScratch.c_str());
    drawPreview(layout.previewHeight);
    ImGui::Separator();

    // A per-frame COPY of the session document: nothing below can mutate the session, and the copy is
    // recorded as ONE pending edit iff it ends the frame different from what it started as. tick()
    // drains that edit into the session before the next onDraw, so the value read back here is always
    // the last one recorded (ImportDetailsPanel's own recorded shape).
    MaterialDocument form = *document;
    bool changed = false;
    // task 3.1.5, the SEAM's own fold. requestSlotTextureDrop cannot write the frame copy -- there is
    // no frame copy outside onDraw -- so it records here and the NEXT onDraw folds it in at exactly
    // the point the picker would have written it, before the slot section runs. That is what makes a
    // driven drop and a real one converge on the same `changed = true`.
    if (slotDrop.has_value() && slotDrop->slot < SLOT_COUNT) {
        std::optional<MaterialTextureSlot>& target = documentSlotAt(form, slotDrop->slot);
        MaterialTextureSlot bound = target.has_value() ? *target : MaterialTextureSlot{};
        bound.guid = slotDrop->textureGuid;
        target = bound;
        changed = true;
    }

    // The SESSION copy is what Apply would write, so the gate validates that and not the form: a
    // pending edit recorded this frame reaches the session next frame and is judged then. Computed
    // ONCE, here, and handed to both the body (which wraps it) and the footer (which gates Apply on
    // it and echoes it in one line).
    const std::optional<MaterialError> invalid = validateMaterial(*document);

    // ---- BODY ------------------------------------------------------------------------------------
    // TWO PATHS, AND THE CHILD EXISTS IN ONLY ONE OF THEM. The 1:1 rule for BeginChild/EndChild is NOT
    // "call EndChild whatever happened" -- it is "EndChild exactly once per BeginChild". A branch that
    // calls NEITHER satisfies it; a branch that calls one without the other is an IM_ASSERT abort. So
    // the pair is written INSIDE the FixedRegions arm, both calls in the same block, with nothing
    // between them that can return or continue.
    //
    // Everything inside drawBody() is IDENTICAL in both paths -- one function, called from two places,
    // so the sections, the label column and the two notices cannot drift between the modes.
    if (layout.mode == MaterialPanelMode::FixedRegions) {
        // -footerHeight, NEVER layout.bodyHeight: CalcItemSize resolves a negative child height as
        // ImMax(4.0f, avail.y + size.y) (imgui.cpp:12344-12345), so ImGui's own remainder is
        // authoritative and a one-pixel error in footerHeight costs the child a pixel instead of
        // clipping Apply off the bottom of the panel. asset_browser_panel.cpp:1311-1315 floors its own
        // child at 1.0F for the OPPOSITE requirement -- two side-by-side panes must share ONE explicit
        // height -- and both are right; here the child is alone and ImGui's remainder is the point.
        ImGui::BeginChild("##body", ImVec2(0.0F, -layout.footerHeight));
        drawBody(form, layout, invalid, changed);
        ImGui::EndChild();  // 1:1 with the BeginChild above, same block, nothing exits between them
    } else {
        // NO CHILD. layout.footerHeight is not passed to anything here -- passing it would be
        // meaningless, and passing layout.bodyHeight (which is 0.0F in this mode) to BeginChild would
        // be worse: CalcItemSize reads a ZERO height as "use the whole remaining region" rather than
        // as "nothing", so the footer would land off the bottom of a full-height child. The mode is
        // what prevents that, which is why it is an enum on the layout and not a height comparison
        // here.
        drawBody(form, layout, invalid, changed);
    }

    if (changed && !(form == *document)) {
        pendingDocument = form;  // last-writer-wins; nothing is applied here
    }

    // ---- FOOTER ----------------------------------------------------------------------------------
    ImGui::Separator();
    const bool dirty = sessionPtr->dirty();
    const bool applyEnabled = dirty && !invalid.has_value();
    if (applyEnabled) {
        // viewport_panel.cpp's emphasis idiom -- a colour the STYLE already owns, so E.6.1's
        // EditorTheme inherits it for free and this task states no colour literal at all. ENABLED
        // ONLY: BeginDisabled pushes ALPHA, so an unconditional push would paint a FADED primary.
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    ImGui::BeginDisabled(!applyEnabled);  // 1:1 with EndDisabled; nothing exits between them
    if (ImGui::Button("Apply")) {
        applyRequested = true;
    }
    ImGui::EndDisabled();
    if (applyEnabled) {
        ImGui::PopStyleColor();  // 1:1, on the SAME condition, evaluated once above
    }
    // A DISABLED item is not hovered without ImGuiHoveredFlags_AllowWhenDisabled, and SetItemTooltip
    // carries ForTooltip flags that skip a disabled item outright -- IsItemHovered + SetTooltip is the
    // pair this tree uses (viewport_panel.cpp's A6 note).
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        // BOTH tooltips interpolate the file leaf, so BOTH go through a named local and a "%s"
        // argument: A DYNAMIC STRING IS NEVER A FORMAT STRING (project_settings_panel.cpp's rule, a
        // fourth application). A material named "100%s.aeromat" is exactly the input that proves it.
        labelScratch = applyEnabled
                           ? std::format("Write these changes to {}.", leafOf(sessionPtr->targetPath()))
                           : (dirty ? std::string("This material has a value the format refuses; fix it to save.")
                                    : std::string("Nothing to save -- this material matches the file."));
        ImGui::SetTooltip("%s", labelScratch.c_str());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty);
    if (ImGui::Button("Revert")) {
        revertRequested = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        labelScratch = dirty ? std::format("Discard your changes and reload {}.", leafOf(sessionPtr->targetPath()))
                             : std::string("Nothing to discard -- this material matches the file.");
        ImGui::SetTooltip("%s", labelScratch.c_str());
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(materialDirtyWord(dirty).data());  // the MODEL's string, never a literal here

    // ONE line, ALWAYS drawn, empty when there is nothing to say -- so the footer's height, and
    // therefore the preview's, cannot change because a notice appeared (asset_picker.cpp's own
    // always-reserved idiom). TextUnformatted, NEVER TextWrapped: a long message is CLIPPED at the
    // panel edge rather than growing a second line.
    const MaterialStatusLine status = materialStatusLine(
        invalid.has_value() ? std::string_view(invalid->message) : std::string_view{},
        sessionPtr->externalChangeNoticed(), sessionPtr->warnings().size(), sessionPtr->lastMessage());
    switch (status.severity) {  // NO default: a fourth severity is a -Wswitch error
        case MaterialStatusSeverity::Warning:
            ImGui::PushStyleColor(ImGuiCol_Text, WARNING_COLOR);
            break;
        case MaterialStatusSeverity::Notice:
            ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
            break;
        case MaterialStatusSeverity::Disabled:
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            break;
    }
    ImGui::TextUnformatted(status.text.c_str());
    ImGui::PopStyleColor();  // 1:1 -- every arm above pushed exactly one
}

}  // namespace engine::editor

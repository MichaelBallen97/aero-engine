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
#include <aero/editor/panel_context.hpp>
#include <aero/editor/project_files.hpp>
#include <aero/reflect/material_format.hpp>
#include <aero/render/material.hpp>

#include "text_input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <imgui.h>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

namespace {

// The header's own restated count, checked rather than trusted.
static_assert(MaterialPanel::SLOT_COUNT == render::MATERIAL_TEXTURE_SLOT_COUNT);

constexpr ImVec4 WARNING_COLOR{1.0F, 0.4F, 0.4F, 1.0F};  // project_ui.cpp's own error-text colour
constexpr ImVec4 NOTICE_COLOR{1.0F, 0.8F, 0.4F, 1.0F};   // a warm amber for the non-fatal notices

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

// ---- the scalar rows, docs/09 section 11 order (AC-17) -------------------------------------------
[[nodiscard]] bool drawScalarRows(MaterialDocument& form, std::string& nameDraft, bool& nameEditing,
                                  std::string& scratch) {
    bool changed = false;

    // `name` commits on deactivate-after-edit (AC-17). The draft is re-synced on every frame the
    // widget is NOT active, which is also how a retarget reaches it -- see the header's own note on
    // why a per-frame copy of form.name cannot carry the gesture.
    if (!nameEditing) {
        nameDraft = form.name;
    }
    inputTextString("Name", nameDraft, ImGuiInputTextFlags_None);
    nameEditing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        form.name = nameDraft;
        changed = true;
    }

    std::array<float, 4> base{form.baseColorFactor.x, form.baseColorFactor.y, form.baseColorFactor.z,
                              form.baseColorFactor.w};
    if (ImGui::ColorEdit4("Base color", base.data(), ImGuiColorEditFlags_Float)) {
        form.baseColorFactor.x = clampUnit(base[0]);
        form.baseColorFactor.y = clampUnit(base[1]);
        form.baseColorFactor.z = clampUnit(base[2]);
        form.baseColorFactor.w = clampUnit(base[3]);
        changed = true;
    }

    float metallic = form.metallicFactor;
    if (ImGui::SliderFloat("Metallic", &metallic, 0.0F, 1.0F)) {
        form.metallicFactor = clampUnit(metallic);
        changed = true;
    }
    float roughness = form.roughnessFactor;
    if (ImGui::SliderFloat("Roughness", &roughness, 0.0F, 1.0F)) {
        form.roughnessFactor = clampUnit(roughness);
        changed = true;
    }

    // HDR only for emissive: docs/09 section 11.1 leaves it unbounded above (the lights' precedent),
    // and baseColorFactor is a [0,1] tint that an HDR picker would invite somebody to break.
    std::array<float, 3> emissive{form.emissiveFactor.x, form.emissiveFactor.y, form.emissiveFactor.z};
    if (ImGui::ColorEdit3("Emissive", emissive.data(), ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)) {
        form.emissiveFactor.x = clampNonNegative(emissive[0]);
        form.emissiveFactor.y = clampNonNegative(emissive[1]);
        form.emissiveFactor.z = clampNonNegative(emissive[2]);
        changed = true;
    }

    // v_min == v_max == 0 is ImGui's "no bound at all": the C++ clamp below is the ONLY enforcement,
    // which is exactly AC-18's point rather than an oversight.
    float normalScale = form.normalScale;
    if (ImGui::DragFloat("Normal scale", &normalScale, 0.01F, 0.0F, 0.0F, "%.3f")) {
        form.normalScale = clampNonNegative(normalScale);
        changed = true;
    }
    float occlusion = form.occlusionStrength;
    if (ImGui::SliderFloat("Occlusion strength", &occlusion, 0.0F, 1.0F)) {
        form.occlusionStrength = clampUnit(occlusion);
        changed = true;
    }

    if (tokenCombo("Alpha mode", form.alphaMode, ALPHA_MODE_VALUES, materialAlphaModeLabel, scratch)) {
        changed = true;
    }
    // AC-19: the ROW is conditional, the VALUE is not. The format stores alphaCutoff whatever the mode
    // is, so switching to opaque and back must return the number the user chose, not 0.5.
    if (form.alphaMode == MaterialAlphaMode::Mask) {
        float cutoff = form.alphaCutoff;
        if (ImGui::SliderFloat("Alpha cutoff", &cutoff, 0.0F, 1.0F)) {
            form.alphaCutoff = clampUnit(cutoff);
            changed = true;
        }
    }

    bool doubleSided = form.doubleSided;
    if (ImGui::Checkbox("Double sided", &doubleSided)) {
        form.doubleSided = doubleSided;
        changed = true;
    }
    return changed;
}

// ---- one texture slot (AC-20/AC-21/AC-22) --------------------------------------------------------
// PushID/PopID are 1:1 across EVERY path through this function -- no continue, no break, no return
// between them (an unbalanced id stack is an IM_ASSERT abort in the Debug ImGui build).
[[nodiscard]] bool drawSlotSection(std::size_t index, MaterialDocument& form, const AssetDatabase* database,
                                   std::string& search, std::string& scratch) {
    bool changed = false;
    ImGui::PushID(static_cast<int>(index));
    scratch = std::string(materialSlotLabel(index));
    // DEFAULT-OPEN, like every CollapsingHeader in this tree: no tier here can click one, so a closed
    // node's branches never execute anywhere, under any sanitizer (3.2.4's recorded lesson).
    if (ImGui::CollapsingHeader(scratch.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        std::optional<MaterialTextureSlot>& slot = documentSlotAt(form, index);

        // --- bound state -------------------------------------------------------------------------
        const AssetRecord* record = nullptr;
        if (slot.has_value() && database != nullptr) {
            record = database->findByGuid(slot->guid);
        }
        if (!slot.has_value()) {
            ImGui::TextDisabled("None");
        } else {
            scratch = formatGuid(slot->guid);
            ImGui::TextUnformatted(scratch.c_str());
            if (record != nullptr) {
                const AssetKind kind = classifyAssetKind(leafOf(record->relativePath), false);
                scratch = std::format("{}  -  {}", record->relativePath, assetKindLabel(kind));
                ImGui::TextUnformatted(scratch.c_str());
                if (kind != AssetKind::Texture) {
                    // AC-21: named, not silently ignored -- and Apply stays legal either way.
                    ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
                    ImGui::TextWrapped("%s", "This asset is not a texture; the slot will use its default.");
                    ImGui::PopStyleColor();
                }
            } else if (database != nullptr) {
                ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
                ImGui::TextWrapped("%s", "This GUID is not in this project; the slot will use its default.");
                ImGui::PopStyleColor();
            }
        }

        // --- the picker --------------------------------------------------------------------------
        // Texture-kind records only, in the database's OWN path order (records() is already sorted),
        // filtered by the browser's own leaf-name predicate so the two search boxes behave alike.
        inputTextString("Search", search, ImGuiInputTextFlags_None);
        if (record != nullptr) {
            scratch = record->relativePath;
        } else {
            scratch = slot.has_value() ? "(unresolved)" : "None";
        }
        if (ImGui::BeginCombo("Texture", scratch.c_str())) {
            if (ImGui::Selectable("None", !slot.has_value())) {
                slot.reset();
                changed = true;
            }
            if (database != nullptr) {
                const AssetFilter filter{.query = search, .kind = AssetKind::Texture, .anyKind = false};
                for (const AssetRecord& candidate : database->records()) {
                    const bool eligible =
                        candidate.guid.valid() && matchesFilter(leafOf(candidate.relativePath), false, filter);
                    if (eligible) {
                        const bool selected = slot.has_value() && slot->guid == candidate.guid;
                        if (ImGui::Selectable(candidate.relativePath.c_str(), selected)) {
                            // A REBIND keeps the slot's sampler tokens; a FRESH bind takes the
                            // format's own defaults, which is what MaterialTextureSlot{} already is.
                            MaterialTextureSlot bound = slot.has_value() ? *slot : MaterialTextureSlot{};
                            bound.guid = candidate.guid;
                            slot = bound;
                            changed = true;
                        }
                    }
                }
            }
            ImGui::EndCombo();  // ASYMMETRIC: only because BeginCombo returned true
        }

        ImGui::BeginDisabled(!slot.has_value());  // 1:1 with EndDisabled; nothing exits between them
        if (ImGui::Button("Clear")) {
            slot.reset();  // AC-20: the WHOLE slot, never a nil guid -- absence is spelled by omission
            changed = true;
        }
        ImGui::EndDisabled();

        // --- sampler state, while bound ----------------------------------------------------------
        if (slot.has_value()) {
            int uvSet = static_cast<int>(slot->uvSet);
            if (ImGui::DragInt("UV set", &uvSet, 0.1F, 0, static_cast<int>(MATERIAL_MAX_UV_SETS) - 1)) {
                slot->uvSet = clampUvSet(uvSet);
                changed = true;
            }
            if (slot->uvSet != 0) {
                // AC-22, the v1 rule: MeshVertex carries one UV set, so a consumer honours set 0 and
                // WARNs. The value is STORED for fidelity -- this note is why it looks ignored.
                ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
                ImGui::TextWrapped("%s", "v1 consumers honour UV set 0; this value is stored, not sampled.");
                ImGui::PopStyleColor();
            }
            // Five separate `if`s rather than one chain of `|| changed`: every one of these lines
            // would otherwise sit within a couple of columns of the format limit, and Homebrew's
            // clang-format 18 and Ubuntu's disagree about how to break a chain that long.
            if (tokenCombo("Wrap U", slot->wrapU, WRAP_VALUES, materialWrapLabel, scratch)) {
                changed = true;
            }
            if (tokenCombo("Wrap V", slot->wrapV, WRAP_VALUES, materialWrapLabel, scratch)) {
                changed = true;
            }
            if (tokenCombo("Min filter", slot->minFilter, FILTER_VALUES, materialFilterLabel, scratch)) {
                changed = true;
            }
            if (tokenCombo("Mag filter", slot->magFilter, FILTER_VALUES, materialFilterLabel, scratch)) {
                changed = true;
            }
            if (tokenCombo("Mip filter", slot->mipFilter, MIP_FILTER_VALUES, materialMipFilterLabel, scratch)) {
                changed = true;
            }
        }
    }
    ImGui::PopID();
    return changed;
}

}  // namespace

void MaterialPanel::onDraw(PanelContext& /*context*/) {  // no World/Selection/Project read (the
                                                         // ImportDetailsPanel "context is ignored"
                                                         // precedent)
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

    // ---- the status strip ------------------------------------------------------------------------
    labelScratch = std::string(sessionPtr->targetPath());
    if (sessionPtr->dirty()) {
        labelScratch += " *";
    }
    ImGui::TextUnformatted(labelScratch.c_str());
    if (databasePtr != nullptr) {
        const AssetRecord* record = databasePtr->findByPath(sessionPtr->targetPath());
        labelScratch =
            record == nullptr || !record->guid.valid() ? std::string("no .meta yet") : formatGuid(record->guid);
        ImGui::TextDisabled("%s", labelScratch.c_str());
    }
    if (sessionPtr->externalChangeNoticed()) {
        ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
        ImGui::TextWrapped("%s", "This file changed on disk; Apply will overwrite it.");
        ImGui::PopStyleColor();
    }
    // The parser's own per-key list (task 3.4.2's engine channel). Each entry names a key Apply will
    // DELETE, which is the only half of "not canonical" worth interrupting somebody over.
    for (const std::string& warning : sessionPtr->warnings()) {
        labelScratch = warning;
        ImGui::PushStyleColor(ImGuiCol_Text, NOTICE_COLOR);
        ImGui::TextWrapped("%s", labelScratch.c_str());
        ImGui::PopStyleColor();
    }
    if (!sessionPtr->lastMessage().empty()) {
        labelScratch = std::string(sessionPtr->lastMessage());
        ImGui::TextDisabled("%s", labelScratch.c_str());
    }
    ImGui::Separator();

    // ---- the editable form -----------------------------------------------------------------------
    // A per-frame COPY of the session document: nothing below can mutate the session, and the copy is
    // recorded as ONE pending edit iff it ends the frame different from what it started as. tick()
    // drains that edit into the session before the next onDraw, so the value read back here is always
    // the last one recorded (ImportDetailsPanel's own recorded shape).
    MaterialDocument form = *document;
    bool changed = drawScalarRows(form, nameDraft, nameEditing, labelScratch);
    ImGui::SeparatorText("Textures");
    for (std::size_t i = 0; i < SLOT_COUNT; ++i) {
        changed = drawSlotSection(i, form, databasePtr, slotSearch[i], labelScratch) || changed;
    }
    if (changed && !(form == *document)) {
        pendingDocument = form;  // last-writer-wins; nothing is applied here
    }

    // ---- Apply / Revert --------------------------------------------------------------------------
    // The SESSION copy is what Apply would write, so the gate validates that and not the form: a
    // pending edit recorded this frame reaches the session next frame and is judged then.
    ImGui::Separator();
    const bool dirty = sessionPtr->dirty();
    const std::optional<MaterialError> invalid = validateMaterial(*document);
    ImGui::BeginDisabled(!dirty || invalid.has_value());  // 1:1 with EndDisabled; nothing exits between
    if (ImGui::Button("Apply")) {
        applyRequested = true;
    }
    ImGui::EndDisabled();
    // A DISABLED item is not hovered without ImGuiHoveredFlags_AllowWhenDisabled, and SetItemTooltip
    // carries ForTooltip flags that skip a disabled item outright -- IsItemHovered + SetTooltip is the
    // pair this tree uses (viewport_panel.cpp's A6 note).
    if ((!dirty || invalid.has_value()) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", dirty ? "This material has a value the format refuses; fix it to save."
                                      : "Nothing to save -- this material matches the file.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty);
    if (ImGui::Button("Revert")) {
        revertRequested = true;
    }
    ImGui::EndDisabled();
    if (invalid.has_value()) {
        labelScratch = invalid->message;
        ImGui::PushStyleColor(ImGuiCol_Text, WARNING_COLOR);
        ImGui::TextWrapped("%s", labelScratch.c_str());
        ImGui::PopStyleColor();
    }
}

}  // namespace engine::editor

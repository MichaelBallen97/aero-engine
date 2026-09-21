// Aero Engine — the Material panel's pure model (task E.3.4). See the header for why it is public and
// what it is not allowed to name. PURE: no ImGui, no GPU, no <filesystem>, no logging.
#include <aero/editor/material_edit.hpp>  // materialSlotIsSrgb -- the colour-space note is COMPOSED
#include <aero/editor/material_inspector_model.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine::editor {

namespace {

// std::array rather than a C array throughout: modernize-avoid-c-arrays is --warnings-as-errors in
// CI, and a std::array converts to MaterialSection::fields' std::span implicitly.
constexpr std::array<MaterialFieldId, 1> MATERIAL_FIELDS{MaterialFieldId::Name};
constexpr std::array<MaterialFieldId, 1> BASE_FIELDS{MaterialFieldId::BaseColorFactor};
// NOT `MR_FIELDS`: `MR` is this task's own test-id prefix, and a whole-tree prefix sweep would read
// this name as a claimed id. Spelled out instead; it costs nothing.
constexpr std::array<MaterialFieldId, 2> METALLIC_ROUGHNESS_FIELDS{MaterialFieldId::MetallicFactor,
                                                                   MaterialFieldId::RoughnessFactor};
constexpr std::array<MaterialFieldId, 1> NORMAL_FIELDS{MaterialFieldId::NormalScale};
constexpr std::array<MaterialFieldId, 1> OCCLUSION_FIELDS{MaterialFieldId::OcclusionStrength};
constexpr std::array<MaterialFieldId, 1> EMISSION_FIELDS{MaterialFieldId::EmissiveFactor};
constexpr std::array<MaterialFieldId, 3> RENDERING_FIELDS{MaterialFieldId::AlphaMode, MaterialFieldId::AlphaCutoff,
                                                          MaterialFieldId::DoubleSided};

// The five slot sections carry 0..4 in ASCENDING order, which is render::materialSlotAt's binding
// order AND docs/09 section 11.1's canonical order for `textures`. MR3 asserts the identity rather
// than this list -- so a format change that reorders `textures` reddens there instead of silently
// desynchronising the panel from the file.
constexpr std::array<MaterialSection, MATERIAL_SECTION_COUNT> SECTIONS{{
    {MaterialSectionId::Material, "Material", std::nullopt, MATERIAL_FIELDS},
    {MaterialSectionId::BaseColor, "Base Color", std::size_t{0}, BASE_FIELDS},
    {MaterialSectionId::MetallicRoughness, "Metallic / Roughness", std::size_t{1}, METALLIC_ROUGHNESS_FIELDS},
    {MaterialSectionId::Normal, "Normal", std::size_t{2}, NORMAL_FIELDS},
    {MaterialSectionId::Occlusion, "Occlusion", std::size_t{3}, OCCLUSION_FIELDS},
    {MaterialSectionId::Emission, "Emission", std::size_t{4}, EMISSION_FIELDS},
    {MaterialSectionId::Rendering, "Rendering", std::nullopt, RENDERING_FIELDS},
    {MaterialSectionId::File, "File", std::nullopt, {}},
}};
static_assert(SECTIONS.size() == MATERIAL_SECTION_COUNT);

// TWO terms, and which one carries which guarantee is worth stating exactly, because sabotage seed
// S21 measured it: `std::isfinite` rejects BOTH a NaN and an infinity on its own, so it -- not the
// negated comparison -- is what makes a NaN take the fallback here. Rewriting `v > 0` as `!(v <= 0)`
// or back again therefore changes nothing and reddens nothing, which is why S21 is recorded as inert
// rather than as covered.
//
// The negated spelling is kept anyway, as gridColumnsFor's own posture and 3.7.2's
// std::clamp(NaN) lesson: it is the form that stays correct if the isfinite term is ever dropped, and
// a guard whose NaN-safety depends on exactly one of two terms is one edit from being wrong. The
// isfinite term is the one MR18's +inf arm needs -- an infinite fontSize would otherwise carry
// straight through to thumbEdge with nothing downstream able to clamp it back.
[[nodiscard]] float positiveFinite(float value, float fallback) noexcept {
    return (value > 0.0F && std::isfinite(value)) ? value : fallback;
}

}  // namespace

std::span<const MaterialSection> materialSections() noexcept { return SECTIONS; }

std::string_view materialFieldLabel(MaterialFieldId field) noexcept {
    // Four labels are DELIBERATELY renamed from what the panel ships today, because the section title
    // already carries the word: inside "Base Color" the factor's row is "Tint", inside "Normal" the
    // scale's row is "Scale". The other six are byte-identical to today's strings (MR4 pins them), so
    // a reader of the diff sees four renames and not ten.
    switch (field) {
        case MaterialFieldId::Name:
            return "Name";  // unchanged
        case MaterialFieldId::BaseColorFactor:
            return "Tint";  // was "Base color"
        case MaterialFieldId::MetallicFactor:
            return "Metallic";  // unchanged
        case MaterialFieldId::RoughnessFactor:
            return "Roughness";  // unchanged
        case MaterialFieldId::EmissiveFactor:
            return "Color";  // was "Emissive"
        case MaterialFieldId::NormalScale:
            return "Scale";  // was "Normal scale"
        case MaterialFieldId::OcclusionStrength:
            return "Strength";  // was "Occlusion strength"
        case MaterialFieldId::AlphaMode:
            return "Alpha mode";  // unchanged
        case MaterialFieldId::AlphaCutoff:
            return "Alpha cutoff";  // unchanged
        case MaterialFieldId::DoubleSided:
            return "Double sided";  // unchanged
    }
    return "";  // unreachable; enumerated so an eleventh field is a -Wswitch error
}

std::string_view materialSlotNoticeText(MaterialSlotNotice notice) noexcept {
    // VERBATIM the two sentences material_panel.cpp ships, so a reader of the diff sees a MOVE, not a
    // re-wording. They are the slot's OWN notices -- not the preview-texture-state switch's
    // "Loading the preview texture..." and Failed arms, which stay where they are.
    switch (notice) {  // NO default:
        case MaterialSlotNotice::None:
            return "";
        case MaterialSlotNotice::NotATexture:
            return "This asset is not a texture; the slot will use its default.";
        case MaterialSlotNotice::Unresolvable:
            return "This GUID is not in this project; the slot will use its default.";
    }
    return "";
}

bool materialSlotSamplerIsDefault(const MaterialTextureSlot& slot) noexcept {
    // Against a DEFAULT-CONSTRUCTED slot's tokens, never a hand-written list, so a seventh token
    // follows the struct automatically. The GUID is deliberately NOT compared: a slot is "custom"
    // because of how it is SAMPLED, not because of what it points at (MR5's own clause).
    const MaterialTextureSlot d{};
    return slot.uvSet == d.uvSet && slot.wrapU == d.wrapU && slot.wrapV == d.wrapV && slot.minFilter == d.minFilter &&
           slot.magFilter == d.magFilter && slot.mipFilter == d.mipFilter;
}

std::string_view materialSamplerNodeLabel(bool samplerIsDefault) noexcept {
    return samplerIsDefault ? "Sampler" : "Sampler (custom)";
}

std::string_view materialColorSpaceNote(std::size_t slotIndex) noexcept {
    // COMPOSED from materialSlotIsSrgb -- which is itself composed from
    // render::defaultTextureKindForSlot (material_edit.hpp:60-64) rather than restated, the 3.4.1
    // lesson that deleted a hand-written five-entry table. So the day a sixth slot exists the note
    // follows the PREDICATE and not a list. MR6 asserts the agreement over all five indices.
    return materialSlotIsSrgb(slotIndex) ? "Sampled as sRGB (color data)." : "Sampled as linear (non-color data).";
}

MaterialSlotRow materialSlotRow(const std::optional<MaterialTextureSlot>& slot, std::string_view recordRelativePath,
                                bool recordIsTexture, bool databaseAvailable, bool recordFound) {
    MaterialSlotRow out{};
    if (!slot.has_value()) {
        out.valueText = "None";
        // samplerIsDefault stays TRUE: an absent slot has nothing custom about it, so the disclosure
        // that is not drawn would not have said "(custom)" either (MR8).
        return out;
    }
    out.clearEnabled = true;
    out.samplerIsDefault = materialSlotSamplerIsDefault(*slot);
    // databaseAvailable is tested FIRST and the two are not interchangeable: without a database this
    // panel is not entitled to say "this GUID is not in this project" -- it has not read one. That is
    // today's behaviour and AC-17. A recordFound that is true while databaseAvailable is false is a
    // caller bug and takes the conservative arm.
    if (!databaseAvailable) {
        out.valueText = "(unresolved)";
        return out;
    }
    if (!recordFound) {
        out.valueText = "(unresolved)";
        out.notice = MaterialSlotNotice::Unresolvable;
        return out;
    }
    // The FULL relative path, not the leaf -- deliberately different from the Inspector's
    // "<leaf> (<kind>)" (inspector_model.hpp:84-86), because this row is the only place the user sees
    // WHICH FILE IN WHICH FOLDER a material references. Do not unify them.
    out.valueText = std::string(recordRelativePath);
    if (!recordIsTexture) {
        out.notice = MaterialSlotNotice::NotATexture;  // AC-21: named, not silently ignored
    }
    return out;
}

MaterialStatusLine materialStatusLine(std::string_view invalidMessage, bool externalChanged, std::size_t warningCount,
                                      std::string_view lastMessage) {
    // THE PRIORITY ORDER, in ONE place. Each arm returns, so the order IS the priority and there is
    // no way to satisfy two. MR12 asserts it pairwise.
    if (!invalidMessage.empty()) {
        return {std::string(invalidMessage), MaterialStatusSeverity::Warning};
    }
    if (externalChanged) {
        return {"This file changed on disk; Apply will overwrite it.", MaterialStatusSeverity::Notice};
    }
    if (warningCount > 0) {
        return {materialUnknownKeySummary(warningCount), MaterialStatusSeverity::Notice};
    }
    if (!lastMessage.empty()) {
        return {std::string(lastMessage), MaterialStatusSeverity::Disabled};
    }
    return {};  // "" + Disabled -- and the line is DRAWN anyway (the always-reserved idiom)
}

std::string materialUnknownKeySummary(std::size_t warningCount) {
    // Its own function because the pluralisation is the kind of thing that ships wrong and never
    // reddens. MR13's SINGULAR arm is its own assertion for that reason.
    if (warningCount == 0) {
        return {};
    }
    return warningCount == 1 ? std::string("1 unknown key will be removed on Apply.")
                             : std::format("{} unknown keys will be removed on Apply.", warningCount);
}

std::string_view materialDirtyWord(bool dirty) noexcept { return dirty ? "Unsaved changes" : ""; }

MaterialPanelLayout materialPanelLayout(const MaterialPanelMetrics& m) noexcept {
    // Every metric passes through positiveFinite, so everything after these six lines is finite and
    // non-negative by construction -- which is what makes the std::min/std::max chain below total.
    const float fontSize = positiveFinite(m.fontSize, 1.0F);
    const float availHeight = positiveFinite(m.availHeight, 0.0F);
    const float textLine = positiveFinite(m.textLineHeight, fontSize);
    const float frame = positiveFinite(m.frameHeight, fontSize);
    const float spacing = positiveFinite(m.itemSpacingY, 0.0F);
    const float separator = positiveFinite(m.separatorHeight, 1.0F);

    MaterialPanelLayout out{};
    out.thumbEdge = MATERIAL_SLOT_THUMB_FONT_MULTIPLE * fontSize;

    // Each term is ONE ItemSize advance: ItemSize moves CursorPos.y by line_height + ItemSpacing.y
    // (imgui.cpp:12129). The LAST item in each region contributes its height without a trailing
    // spacing, which is why the footer ends at textLine and not at textLine + spacing.
    const float identity = textLine + spacing;
    const float headerTail = spacing + separator + spacing;
    // THREE spacings, and the leading one is the one that is easy to lose. EndChild() calls
    // ItemSize(child_size) (imgui.cpp:6860) and ItemSize advances CursorPos.y by
    // line_height + ItemSpacing.y (:12129), so the child's own bottom edge is followed by a spacing
    // BEFORE the separator. ImGui's changelog spells this recipe verbatim at imgui.cpp:437 and calls
    // the two-term form a BUG. Measured: 45 points at 1x, 64 at 2x.
    out.footerHeight = spacing + separator + spacing + frame + spacing + textLine;

    const float fixed = identity + headerTail + out.footerHeight;
    const float usable = std::max(0.0F, availHeight - fixed);

    const float minPreview = MATERIAL_PREVIEW_MIN_FONT * fontSize;
    const float maxPreview = MATERIAL_PREVIEW_MAX_FONT * fontSize;
    const float minBody = MATERIAL_BODY_MIN_FONT * fontSize;

    // THE ONE BRANCH. The fixed-region walk needs room for the smallest preview it is willing to draw
    // AND for ImGui's own 4-point child floor; below that the WINDOW scrolls instead, exactly as it
    // does today, and the preview is the floor rather than nothing. Measured on this machine: a
    // 320x180 editor gives the panel 98 points against a 103-point chrome at a display scale of 2, so
    // this branch is not a defensive nicety -- it is the arm the product actually takes.
    //
    // THE THRESHOLD IS `minPreview + FLOOR` AND NOT `minBody`, BECAUSE THAT MAKES THE FUNCTION
    // CONTINUOUS: at usable == minPreview + FLOOR the chain below evaluates to EXACTLY minPreview, so
    // dragging the dock divider across the boundary moves the preview by zero. A threshold of minBody
    // would step it, and a threshold of FLOOR alone would collapse it from minPreview to ~1 point --
    // a one-point drag producing a 130-point jump, which is the defect validation row 7 exists to
    // catch. MR16 asserts the continuity; MR15 asserts the boundary from both sides.
    if (usable >= minPreview + MATERIAL_BODY_FLOOR_POINTS) {
        out.mode = MaterialPanelMode::FixedRegions;

        float preview = availHeight * MATERIAL_PREVIEW_FRACTION;
        preview = std::max(preview, minPreview);
        preview = std::min(preview, maxPreview);
        preview = std::min(preview, std::max(minPreview, usable - minBody));  // the body's minimum, if it fits
        // REDUNDANT UNDER THE GUARD ABOVE, AND KEPT ANYWAY AS A STATEMENT OF INTENT. Proof that it
        // never lowers: preview <= max(minPreview, usable - minBody); the first arm is <= usable -
        // FLOOR by the guard, and the second is <= usable - FLOOR because minBody >= FLOOR. That
        // redundancy is why seed S18 reddens nothing any more, and the plan's section 9 says so rather
        // than leaving a seed that looks covered.
        preview = std::min(preview, std::max(0.0F, usable - MATERIAL_BODY_FLOOR_POINTS));
        // NEGATED, so a NaN takes the fallback -- and the fallback is minPreview rather than 0, which
        // is what keeps the mode boundary continuous even under a perverse retune (MAX_FONT < MIN_FONT
        // would otherwise let the middle clamp drop below the floor this mode promises).
        if (!(preview >= minPreview)) {
            preview = minPreview;
        }

        out.previewHeight = preview;
        out.bodyHeight = std::max(MATERIAL_BODY_FLOOR_POINTS, usable - preview);
    } else {
        out.mode = MaterialPanelMode::Scrolling;
        out.previewHeight = minPreview;  // the floor IS the guarantee here, not a clamp on something
        out.bodyHeight = 0.0F;           // NATURAL: nothing reserves it and nothing may read it
    }

    // TRUE IN BOTH MODES, BY CONSTRUCTION: previewHeight >= minPreview = MATERIAL_PREVIEW_MIN_FONT *
    // fontSize, and fontSize is at least 1 after the guard above, so the product is at least 10.
    out.previewShown = out.previewHeight >= 1.0F;
    out.headerHeight = identity + out.previewHeight + headerTail;
    return out;
}

}  // namespace engine::editor

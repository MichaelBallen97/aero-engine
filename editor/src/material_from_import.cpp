// editor/src/material_from_import.cpp -- task 3.1.5: ImportedMaterial -> MaterialDocument.
// PURE: no ImGui, no disk, no GPU, no logging; warnings are RETURNED.
#include <aero/editor/material_edit.hpp>  // documentSlotAt + materialSlotLabel -- ONE vocabulary
#include <aero/editor/material_from_import.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

namespace {

// The SOURCE side of the slot walk, in documentSlotAt's own order. It lives here rather than on the
// public header because there is exactly one caller; documentSlotAt is public because 3.4.2's panel,
// session and this file all need it.
[[nodiscard]] const std::optional<ImportedTextureRef>& importedSlotAt(const ImportedMaterial& material,
                                                                      std::size_t index) noexcept {
    switch (index) {
        case 1:
            return material.metallicRoughness;
        case 2:
            return material.normal;
        case 3:
            return material.occlusion;
        case 4:
            return material.emissive;
        default:
            break;
    }
    // Index 0 AND every out-of-range index answer baseColor -- materialSlotAt()'s own documented
    // behaviour, mirrored so the two walks cannot disagree at their edges.
    return material.baseColor;
}

}  // namespace

MaterialAlphaMode materialAlphaModeFromImported(AlphaMode mode) noexcept {
    switch (mode) {
        case AlphaMode::Opaque:
            return MaterialAlphaMode::Opaque;
        case AlphaMode::Mask:
            return MaterialAlphaMode::Mask;
        case AlphaMode::Blend:
            return MaterialAlphaMode::Blend;
    }
    return MaterialAlphaMode::Opaque;  // unreachable; no default: so a new enumerator is a -Wswitch error
}

MaterialWrap materialWrapFromImported(TextureWrap wrap) noexcept {
    switch (wrap) {
        case TextureWrap::Repeat:
            return MaterialWrap::Repeat;
        case TextureWrap::ClampToEdge:
            return MaterialWrap::Clamp;
        case TextureWrap::MirroredRepeat:
            return MaterialWrap::Mirror;
    }
    return MaterialWrap::Repeat;  // unreachable; no default: so a new enumerator is a -Wswitch error
}

MaterialFilter materialFilterFromImported(TextureFilter filter) noexcept {
    switch (filter) {
        case TextureFilter::Nearest:
            return MaterialFilter::Nearest;
        case TextureFilter::Linear:
            return MaterialFilter::Linear;
    }
    return MaterialFilter::Linear;  // unreachable; no default: so a new enumerator is a -Wswitch error
}

MaterialMipFilter materialMipFilterFromImported(MipFilter filter) noexcept {
    switch (filter) {
        case MipFilter::None:
            return MaterialMipFilter::None;
        case MipFilter::Nearest:
            return MaterialMipFilter::Nearest;
        case MipFilter::Linear:
            return MaterialMipFilter::Linear;
    }
    return MaterialMipFilter::Linear;  // unreachable; no default: so a new enumerator is a -Wswitch error
}

MaterialFromImportResult materialDocumentFromImported(const ImportedMaterial& material,
                                                      std::span<const ImportedImage> images) {
    MaterialFromImportResult result;
    MaterialDocument& document = result.document;

    // Ten assignments, member for member. Both sides carry glTF 2.0's own defaults verbatim, which is
    // what makes materialDocumentFromImported(ImportedMaterial{}, {}) == MaterialDocument{} a pinned
    // property (MF1) rather than a coincidence.
    document.name = material.name;
    document.baseColorFactor = material.baseColorFactor;
    document.metallicFactor = material.metallicFactor;
    document.roughnessFactor = material.roughnessFactor;
    document.emissiveFactor = material.emissiveFactor;
    document.normalScale = material.normalScale;
    document.occlusionStrength = material.occlusionStrength;
    document.alphaMode = materialAlphaModeFromImported(material.alphaMode);
    document.alphaCutoff = material.alphaCutoff;
    document.doubleSided = material.doubleSided;

    // render::MATERIAL_TEXTURE_SLOT_COUNT is the count material_edit.hpp already declares every walk
    // uses -- "engine and editor code alike iterate 0..MATERIAL_TEXTURE_SLOT_COUNT-1 and never ask for
    // anything else". One count, not a second five.
    for (std::size_t slot = 0; slot < render::MATERIAL_TEXTURE_SLOT_COUNT; ++slot) {
        const std::optional<ImportedTextureRef>& source = importedSlotAt(material, slot);
        if (!source.has_value()) {
            continue;  // the source bound nothing here; omission is how absence is spelled
        }
        const std::string label(materialSlotLabel(slot));
        const ImportedTextureRef& ref = *source;

        // FOUR OMISSION ARMS, in this order. Each leaves the document slot DISENGAGED and emits
        // exactly one warning naming the slot and the reason -- a present-but-nil slot is a
        // contradiction docs/09 section 11 forbids, not a degraded binding.
        if (ref.imageIndex >= images.size()) {
            result.warnings.push_back(label + ": the source names image " + std::to_string(ref.imageIndex) +
                                      ", which this model does not have");
            continue;
        }
        const ImportedImage& image = images[ref.imageIndex];
        if (image.embedded) {
            result.warnings.push_back(label +
                                      ": the source embeds this texture in the model file; extract it "
                                      "beside the model to use it");
            continue;
        }
        if (!image.refusal.empty()) {
            result.warnings.push_back(label + ": " + image.refusal);
            continue;
        }
        if (!image.guid.valid()) {
            result.warnings.push_back(label + ": '" + image.uri + "' is not an asset in this project");
            continue;
        }

        MaterialTextureSlot bound;
        bound.guid = image.guid;
        bound.uvSet = ref.uvSet;
        if (bound.uvSet >= MATERIAL_MAX_UV_SETS) {
            bound.uvSet = MATERIAL_MAX_UV_SETS - 1U;
            result.warnings.push_back(label + ": UV set " + std::to_string(ref.uvSet) +
                                      " is beyond this format's limit; using " + std::to_string(bound.uvSet));
        }
        bound.wrapU = materialWrapFromImported(ref.wrapU);
        bound.wrapV = materialWrapFromImported(ref.wrapV);
        bound.minFilter = materialFilterFromImported(ref.minFilter);
        bound.magFilter = materialFilterFromImported(ref.magFilter);
        bound.mipFilter = materialMipFilterFromImported(ref.mipFilter);
        documentSlotAt(document, slot) = bound;
    }

    return result;
}

}  // namespace engine::editor

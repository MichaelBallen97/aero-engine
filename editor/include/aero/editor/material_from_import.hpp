#pragma once
// Aero Engine -- the ImportedMaterial -> MaterialDocument materializer (task 3.1.5). Pair 21. PUBLIC
// and PURE: no ImGui, no disk, no GPU, no logging; warnings are RETURNED, never printed.
//
// It is the LOSSLESS direction material_format.hpp's own header predicted: "the four enums mirror the
// editor's ImportedMaterial / ImportedTextureRef value sets 1:1 ... so a future import-materializer is
// lossless". This is that materializer, and the four mirrors below are the mapping made explicit.
//
// EVERY MIRROR IS A SWITCH WITH NO `default:`, never a static_cast renumbering. -Wswitch is the
// completeness pin and it is stronger than any test: `static_cast<MaterialWrap>(static_cast<int>(w))`
// would compile, would be correct today, and would silently renumber the day either enum gains a
// value. Each ends with an unreachable return to satisfy -Wreturn-type.
#include <aero/editor/model_import.hpp>
#include <aero/reflect/material_format.hpp>

#include <span>
#include <string>
#include <vector>

namespace engine::editor {

// ---- the four enum mirrors (each total, each `default:`-free) -------------------------------------
[[nodiscard]] MaterialAlphaMode materialAlphaModeFromImported(AlphaMode mode) noexcept;
[[nodiscard]] MaterialWrap materialWrapFromImported(TextureWrap wrap) noexcept;
[[nodiscard]] MaterialFilter materialFilterFromImported(TextureFilter filter) noexcept;
[[nodiscard]] MaterialMipFilter materialMipFilterFromImported(MipFilter filter) noexcept;

struct MaterialFromImportResult {
    MaterialDocument document;
    std::vector<std::string> warnings;  // one per omitted slot / clamped value, in SLOT order
};

// The ten factors copy member for member -- ImportedMaterial and MaterialDocument carry glTF 2.0's own
// defaults verbatim, so materialDocumentFromImported(ImportedMaterial{}, {}) == MaterialDocument{} is a
// PINNED PROPERTY rather than a coincidence.
//
// The five slots are walked in documentSlotAt's fixed order (baseColor, metallicRoughness, normal,
// occlusion, emissive). An engaged ImportedTextureRef is OMITTED, with exactly one warning naming the
// slot and the reason, when: it names an image this model does not have, the image is EMBEDDED, the
// image was REFUSED at import, or the image resolved to no asset in this project. Absence is spelled
// by OMISSION -- docs/09 section 11's own rule -- so a PRESENT-BUT-NIL slot is never produced.
// A uvSet at or beyond MATERIAL_MAX_UV_SETS is CLAMPED, with its own warning.
//
// `images` is the model's own image table; passing an empty span is legal and simply makes every
// engaged slot take the first omission arm.
[[nodiscard]] MaterialFromImportResult materialDocumentFromImported(const ImportedMaterial& material,
                                                                    std::span<const ImportedImage> images);

}  // namespace engine::editor

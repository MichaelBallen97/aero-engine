#pragma once
// Aero Engine — src-private: THE source-image-file -> GPU-texture chain, extracted from
// MaterialPreview::loadOneTexture at task 3.1.5 because the scene-asset ledger became its second
// consumer, and 3.4.1's recorded rule is that a mapping worth having twice is worth having once.
//
// The returned texture is THE CALLER'S TO DESTROY: the material registry BORROWS slot textures and
// destroyMaterial never touches an rhi::TextureHandle (material.hpp). This function creates; it never
// caches, never remembers and never destroys.
//
// It takes an ABSOLUTE PATH and an srgb flag and knows nothing about AssetDatabase (§0.25): the record
// lookup and the path join stay in the callers, which is exactly what lets the ledger reach this chain
// without the preview's cache semantics coming along.
#include <aero/rhi/handles.hpp>

#include <string>
#include <string_view>

namespace engine::rhi {
class Device;  // forward-declared, never #included here -- material_preview.hpp's own shape
}  // namespace engine::rhi

namespace engine::editor {

struct LoadedTexture {
    rhi::TextureHandle texture{};  // invalid iff !error.empty()
    std::string error;             // "" on success; otherwise the exact user-facing sentence
};

// The six error sentences this returns are §0.25's table, verbatim from the preview. They say
// "preview" and the ledger is not a preview -- accepted and recorded there: the alternative is a
// per-caller noun parameter, which is a second vocabulary for one mapping.
[[nodiscard]] LoadedTexture loadTextureFromSourceFile(rhi::Device& device, std::string_view absolutePathUtf8,
                                                      bool srgb);

}  // namespace engine::editor

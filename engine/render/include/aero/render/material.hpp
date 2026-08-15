#pragma once
// Aero Engine — render material vocabulary (task 3.4.1): the numeric parameter set, the five texture
// slots, and the generational MaterialHandle ForwardRenderer's registry mints. ADR-001 names
// materials in the handles rule — "Everything (entities, textures, meshes, materials) is referenced
// by {index, generation}" — so a render-side material is a HANDLE INTO A REGISTRY, never a POD passed
// by value and never a pointer.
//
// All engine types: core math, core Handle, rhi handles and rhi::SamplerDesc. No reflect type, no
// assets type, no third-party type (rule #3). This header deliberately does NOT know that .aeromat
// exists: engine/reflect owns the FILE and a consumer maps its tokens onto rhi::SamplerDesc through
// docs/09 section 11.4's normative table.
//
// OWNERSHIP, stated once because the three answers differ (task 3.4.1 section 0.9):
//   * TEXTURES ARE BORROWED. The caller creates them (render::createTextureFromCookedTexture is the
//     usual route) and the caller destroys them. destroyMaterial never touches a rhi::TextureHandle.
//   * SAMPLERS ARE RENDERER-OWNED. The registry deduplicates identical SamplerDescs into shared
//     handles held for the renderer's lifetime and released in its destructor — never per material.
//   * THE THREE 1x1 DEFAULT TEXTURES AND THE DEFAULT MATERIAL ARE RENDERER-OWNED, and destroying the
//     default material is a logged no-op: it is what every invalid MeshInstance::material resolves
//     to, so removing it would leave those draws nothing to fall back on.

#include <aero/core/handle.hpp>
#include <aero/core/math.hpp>
#include <aero/rhi/descriptors.hpp>  // rhi::SamplerDesc
#include <aero/rhi/format.hpp>       // rhi::TextureFormat
#include <aero/rhi/handles.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::render {

// Render-side tri-state, mirroring the file format's (docs/09 section 11). Blend is STORED and DRAWN
// OPAQUE behind a once-per-renderer latched WARN (D9): real transparency is a blend-enabled pipeline
// plus back-to-front sorting plus a draw-order policy, a coherent feature with no owner today. The
// tri-state survives so the future blend path changes no caller.
enum class MaterialAlpha : std::uint8_t { Opaque, Mask, Blend };

// The glTF metallic-roughness numeric set. Struct defaults are glTF's own — the FILE FORMAT's
// defaults (docs/09 section 11) — which is a different question from what an UNSET
// MeshInstance::material draws with; that answer is DEFAULT_MATERIAL_PARAMS below.
struct MaterialParams {
    Vec4 baseColorFactor = Vec4::one();
    Vec3 emissiveFactor{};
    float metallicFactor = 1.0F;
    float roughnessFactor = 1.0F;
    float normalScale = 1.0F;
    float occlusionStrength = 1.0F;
    MaterialAlpha alpha = MaterialAlpha::Opaque;
    // Pushed to the shader only when alpha == Mask; Opaque and Blend push 0.0, which a texel's alpha
    // can never fall below, so the shader's single discard can never fire there.
    float alphaCutoff = 0.5F;
    bool doubleSided = false;
    bool operator==(const MaterialParams&) const = default;
};

// The premade fallback every invalid MeshInstance::material resolves to (D6): white dielectric,
// metallic 0, roughness 1, emissive black — deliberately NOT glTF's metallic-1 default, because a
// metal with no environment to reflect renders near-black under analytic lights and v1 has no IBL.
// This default's one job is visual continuity with the pre-3.4.1 Lambert primitives. Two different
// defaults for two different questions, and this is the render-side one.
inline constexpr MaterialParams DEFAULT_MATERIAL_PARAMS{.metallicFactor = 0.0F};

// One slot: a texture the CALLER owns (invalid == "use the built-in default for this slot", resolved
// at BIND time rather than at create time, so an updateMaterial that adds or removes a texture needs
// no default bookkeeping) plus the sampler STATE, which the registry resolves to a dedup'd handle.
struct MaterialTextureSlot {
    rhi::TextureHandle texture{};
    rhi::SamplerDesc sampler{};
};

// The five glTF slots in FIXED binding order — baseColor t0/s0, metallicRoughness t1/s1, normal
// t2/s2, occlusion t3/s3, emissive t4/s4, all in space2 (the 0.4.3 binding law). Declaration order IS
// binding order (Device::bindFragmentSamplers' documented contract).
struct MaterialTextureSlots {
    MaterialTextureSlot baseColor;
    MaterialTextureSlot metallicRoughness;
    MaterialTextureSlot normal;
    MaterialTextureSlot occlusion;
    MaterialTextureSlot emissive;
};

inline constexpr std::size_t MATERIAL_TEXTURE_SLOT_COUNT = 5;

// Index the five named slots in binding order. THE one place "which slot is index k" is decided; an
// out-of-range index answers baseColor, which engine code never asks for (it iterates 0..4).
[[nodiscard]] inline const MaterialTextureSlot& materialSlotAt(const MaterialTextureSlots& slots,
                                                               std::size_t index) noexcept {
    switch (index) {
        case 1:
            return slots.metallicRoughness;
        case 2:
            return slots.normal;
        case 3:
            return slots.occlusion;
        case 4:
            return slots.emissive;
        default:
            return slots.baseColor;
    }
}

// One built-in 1x1 identity default: the four RGBA8 bytes and the format the texture is created with.
// The colour space is part of the identity — a white texel sampled through an sRGB view and through a
// linear view are the same number here only because 0xFF is the fixed point of both transfer
// functions, and that is a coincidence of white rather than a reason to merge the two.
struct MaterialDefaultTexture {
    std::array<std::uint8_t, 4> texel{};
    rhi::TextureFormat format = rhi::TextureFormat::RGBA8Unorm;
};

// THREE physical 1x1 textures cover FIVE slots, because occlusion shares metallicRoughness' white
// linear texel and emissive shares baseColor's white sRGB one. Naming that set makes the renderer's
// fallbacks addressable BY KIND rather than by a hand-written per-slot table of member names — see
// defaultTextureKindForSlot below for why that distinction is load-bearing.
enum class MaterialDefaultTextureKind : std::uint8_t { WhiteSrgb, WhiteLinear, FlatNormal };

inline constexpr std::size_t MATERIAL_DEFAULT_TEXTURE_KIND_COUNT = 3;

// THE single decision of "which built-in default belongs to slot k" (D7), by slot index in binding
// order:
//   0 baseColor         white, sRGB    — the factor does the work
//   1 metallicRoughness white, linear  — glTF reads metallic from B and roughness from G, so a white
//                                        texel passes both factors through unscaled
//   2 normal            flat, linear   — 80 80 FF FF is +Z in tangent space
//   3 occlusion         white, linear  — occlusion 1 == unoccluded
//   4 emissive          white, sRGB    — glTF's emissive factor defaults to 0, so an absent emissive
//                                        map renders black THROUGH a white texel
// ForwardRenderer::bindMaterialTextures resolves slot k's fallback THROUGH THIS FUNCTION, indexing an
// array held by kind — it does not keep a second, hand-written table of its own. That is deliberate: a
// per-slot table inside the renderer is a place where "slot 0 gets the flat normal" is a one-token
// typo with no witness at all, since binding the wrong default still draws a fully lit surface (a
// baseColor of 80 80 FF and normals decoded from white). With the mapping spelled once, a swap here
// moves defaultTextureTexel's answer and reddens the tier-0 case that pins it.
[[nodiscard]] inline MaterialDefaultTextureKind defaultTextureKindForSlot(std::size_t slotIndex) noexcept {
    switch (slotIndex) {
        case 1:
        case 3:
            return MaterialDefaultTextureKind::WhiteLinear;
        case 2:
            return MaterialDefaultTextureKind::FlatNormal;
        default:
            return MaterialDefaultTextureKind::WhiteSrgb;
    }
}

// THE definition of the three built-in defaults' bytes. ForwardRenderer::create() uploads exactly
// these; nothing else spells them, so a texel typo is a red test rather than a silently wrong surface.
[[nodiscard]] inline MaterialDefaultTexture defaultTextureTexelForKind(MaterialDefaultTextureKind kind) noexcept {
    switch (kind) {
        case MaterialDefaultTextureKind::WhiteLinear:
            return {{0xFF, 0xFF, 0xFF, 0xFF}, rhi::TextureFormat::RGBA8Unorm};
        case MaterialDefaultTextureKind::FlatNormal:
            return {{0x80, 0x80, 0xFF, 0xFF}, rhi::TextureFormat::RGBA8Unorm};
        case MaterialDefaultTextureKind::WhiteSrgb:
            break;
    }
    return {{0xFF, 0xFF, 0xFF, 0xFF}, rhi::TextureFormat::RGBA8UnormSrgb};
}

// What slot k's built-in default LOOKS like — the composition of the two functions above, and the
// only form a caller outside the renderer ever needs. Composed rather than restated: a second switch
// over slot indices would be a second place for the two answers to disagree.
[[nodiscard]] inline MaterialDefaultTexture defaultTextureTexel(std::size_t slotIndex) noexcept {
    return defaultTextureTexelForKind(defaultTextureKindForSlot(slotIndex));
}

// Phantom tag, deliberately never defined — the rhi handles.hpp shape. A Handle<Material> is not
// interchangeable with any other resource handle at compile time.
struct Material;
using MaterialHandle = Handle<Material>;

}  // namespace engine::render

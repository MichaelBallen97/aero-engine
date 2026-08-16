#pragma once
// Aero Engine — the pure document->render bridge for materials (task 3.4.2, D8). PUBLIC and PURE: no
// ImGui, no <filesystem>, no <fstream>, no GPU CALL, no logging. Every function here is provable from
// a MaterialDocument literal with no context of any kind.
//
// It names render:: and rhi:: aggregates but calls nothing: the two render types below are plain value
// structs (material.hpp is header-only), which is what lets this stay tier-0 testable on
// aero_editor_shell_test. aero::render is PUBLIC on aero_editor_core for exactly this header -- the
// aero::assets precedent from task 3.3.1, whose four criteria aero_render meets one for one (see
// editor/CMakeLists.txt for the criterion-by-criterion note).
//
// 3.4.1's material_format.hpp:27-30 assigned the enum-mirror assertion to THIS task and THIS tier:
// the four format enums mirror ImportedMaterial / ImportedTextureRef's value sets 1:1, and the
// correspondence is asserted in the EDITOR tier, where the golden rule allows both types to be named
// at once. ME25-ME28 are that assertion.
//
// NAME COLLISION, stated once: engine::MaterialTextureSlot (the FILE's slot, reflect) and
// engine::render::MaterialTextureSlot (the GPU's slot) are two different types. Inside
// engine::editor an unqualified MaterialTextureSlot is the FILE's; the render one is always spelled
// render::MaterialTextureSlot -- exactly how samples/phase-3-materials disambiguates them.
#include <aero/reflect/material_format.hpp>
#include <aero/render/material.hpp>
#include <aero/rhi/descriptors.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine::editor {

// ---- the nine-field mapping, alpha tri-state included (AC-25/AC-26) ------------------------------
// No clamping and no defaulting: the panel's widgets clamp before the value ever enters the document
// and validateMaterial is Apply's belt. A mapping that "fixed up" values here would make the on-screen
// preview disagree with the bytes Apply writes.
[[nodiscard]] render::MaterialAlpha materialAlphaFor(MaterialAlphaMode mode) noexcept;  // no default:
[[nodiscard]] render::MaterialParams materialParamsFor(const MaterialDocument& doc) noexcept;

// ---- the slot walk, BOTH directions (AC-25; S17's witness) ---------------------------------------
// Index is render::MaterialTextureSlots' BINDING order: 0 baseColor, 1 metallicRoughness, 2 normal,
// 3 occlusion, 4 emissive -- render::materialSlotAt()'s order, and material.hpp is the authority for
// it. An out-of-range index answers baseColor, exactly as materialSlotAt() does; engine and editor
// code alike iterate 0..MATERIAL_TEXTURE_SLOT_COUNT-1 and never ask for anything else.
[[nodiscard]] const std::optional<MaterialTextureSlot>& documentSlotAt(const MaterialDocument& doc,
                                                                       std::size_t index) noexcept;
[[nodiscard]] std::optional<MaterialTextureSlot>& documentSlotAt(MaterialDocument& doc, std::size_t index) noexcept;
// "baseColor" ... "emissive" -- docs/09 section 11's own key names, so a panel section header and a
// file key can never drift apart.
[[nodiscard]] std::string_view materialSlotLabel(std::size_t index) noexcept;

// ---- docs/09 section 11.4, the NORMATIVE token->SamplerDesc mapping (AC-25/AC-26) ----------------
// "none" is the one row that is not a rename: rhi::MipmapMode has no None, so "do not use mips" is
// the clamp-to-base idiom -- MipmapMode::Nearest AND maxLod = 0.0. ME21 pins that pair explicitly.
// addressW is left untouched at its desc default: v1 materials reference 2D textures (section 11.4).
[[nodiscard]] rhi::AddressMode materialAddressModeFor(MaterialWrap wrap) noexcept;  // no default:
[[nodiscard]] rhi::Filter materialFilterFor(MaterialFilter filter) noexcept;        // no default:
[[nodiscard]] rhi::SamplerDesc materialSamplerDescFor(const MaterialTextureSlot& slot) noexcept;

// ---- the preview's colour-space rule (D7) --------------------------------------------------------
// Mirrors material.hpp's defaultTextureKindForSlot kind split: slots 0 and 4 sample sRGB, 1/2/3
// linear. COMPOSED FROM that function rather than restated, so the two can never disagree -- the
// 3.4.1 lesson that deleted a hand-written five-entry table for exactly this reason.
[[nodiscard]] bool materialSlotIsSrgb(std::size_t index) noexcept;

// ---- New Material's unique name (AC-5) -----------------------------------------------------------
// "NewMaterial.aeromat", then "NewMaterial-2.aeromat", ... Returns "" on exhaustion (the caller logs
// ONE WARN naming the directory and writes nothing -- AC-6). PURE: `taken` is the caller's own
// listing of names already present in the target directory, compared ASCII-case-insensitively so two
// names differing only in case cannot collide on a case-insensitive filesystem.
inline constexpr std::size_t MAX_NEW_MATERIAL_ATTEMPTS = 64;
[[nodiscard]] std::string uniqueMaterialFileName(std::string_view stem, std::span<const std::string_view> taken,
                                                 std::size_t maxAttempts = MAX_NEW_MATERIAL_ATTEMPTS);

}  // namespace engine::editor

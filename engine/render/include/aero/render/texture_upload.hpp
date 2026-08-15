#pragma once
// Aero Engine — the cooked-texture → GPU bridge (task 3.4.1). The sentence 3.3.2 repeated in five
// places, finally owned: "nothing in this tree can upload the artifact" (cooked_texture.hpp's
// "That belongs to task 3.4.1"). One function, no state, no policy.
//
// This is the one place where engine/assets and engine/rhi meet, and it is deliberately a LEAF: it
// takes a parse-side view and a device, and returns a handle. It does not read files, does not cook,
// does not cache, and does not own what it returns.
//
// LAYERING: this header names assets::CookedTextureView, which is what puts `PUBLIC aero::assets` on
// aero_render's link line. That edge is downward in the as-built order (core -> assets -> platform ->
// rhi -> render) and cycle-free, because engine/assets links aero::core and nothing else.

#include <aero/assets/cooked_texture.hpp>
#include <aero/rhi/format.hpp>
#include <aero/rhi/handles.hpp>

namespace engine::rhi {
class Device;  // forward-declared: the .cpp includes device.hpp, callers need only the handle types
}  // namespace engine::rhi

namespace engine::render {

// The TOTAL mapping from a cooked texture's format to the rhi format a texture is created with. A
// switch with NO `default:`, so a ninth CookedTextureFormat is a -Wswitch failure on the Linux lane
// rather than a silent fallback (the house style, and cooked_texture.hpp's own posture).
//
// The two BC1 arms are worth stating rather than assuming: our cooked BC1 files declare Vulkan's
// BC1_RGB (131/132) and SDL exposes only BC1_RGBA, but the cook's encoder emits ONLY opaque
// four-colour blocks, which decode identically under both (alpha 1 everywhere). That is a fact about
// the encoder, not a lossy reinterpretation.
//
// Returns TextureFormat::Invalid only for a value outside the eight, which is unreachable through a
// CookedTextureView: a view is constructible only by parseCookedTexture, whose step 5 refuses every
// other value.
[[nodiscard]] rhi::TextureFormat cookedTextureToRhiFormat(assets::CookedTextureFormat format) noexcept;

// Parse-side view -> GPU texture. Creates with mipLevels = view.levelCount() and TextureUsage::Sampler,
// then performs one BLOCKING uploadTexture per level straight from levelBytes(level) — the sizes agree
// with rhi::textureLevelByteSize by construction, because docs/09 section 10's level arithmetic and
// the rhi formula are the same ceil-div rule.
//
// The returned texture is THE CALLER'S TO DESTROY. Nothing here retains it, and the material registry
// that usually receives it BORROWS it (material.hpp's ownership note).
//
// Returns an INVALID handle plus one AERO_LOG_ERROR when:
//   * the cooked top level is not block-aligned for its format — the device refuses such a texture on
//     every backend (device.hpp's createTexture rule), so this refusal names the ARTIFACT-level fact
//     instead of surfacing a generic backend message. The committed 5x3 BC5 test golden is the worked
//     example: a perfectly valid cooked artifact that cannot become a texture. Cook as rgba8, or
//     resize the source.
//   * a dimension exceeds rhi::MAX_TEXTURE_DIMENSION_2D. Defence in depth only: the cook's own cap is
//     the same 16384, so no cooker-produced file can reach this arm — a hand-built view can.
//   * createTexture or any level's upload fails; the partially-uploaded texture is destroyed first.
[[nodiscard]] rhi::TextureHandle createTextureFromCookedTexture(rhi::Device& device,
                                                                const assets::CookedTextureView& view);

}  // namespace engine::render

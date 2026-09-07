#pragma once
// Aero Engine — the viewport's icon vocabulary (task E.2.3): what draws an icon, which glyph, where
// in the atlas, how big, and how the atlas is built.
//
// ImGui-free, render-free and rhi-free BY RULE, held by FILE PLACEMENT like every header under
// editor/include (2.1.3 D9). It names Vec2 from core and World/Entity from scene, and NOTHING else
// -- deliberately, because picking.hpp includes it and picking.hpp is the narrowest public editor
// header in the tree and states its own render-freedom in its banner.
//
// ONE PREDICATE, THREE READERS. viewportIconFor is the ONLY answer to "does this entity draw an
// icon?", and it is read by (1) emitViewportGizmos, which draws it, (2) pickEntity, which makes it
// clickable at its drawn extent, and (3) ViewportPanel::drawSelectionOverlay, which suppresses
// E.1.4's diamond marker for it. Three readers of one rule: a second spelling would let an entity be
// drawn and not clickable, or clickable and not drawn, on any frame the two disagreed -- which is
// buildSelectionMaskSet's D11, one layer over.
//
// THE ENUM'S ORDER IS THREE THINGS AT ONCE and they are deliberately the same order: the atlas's CELL
// order, the PRIORITY order for an entity carrying more than one of these components, and the
// declaration order a switch must be total over. A light beats a camera because a light is the
// affordance with no other handle; DirectionalLight beats SpotLight beats PointLight because that is
// the order they were added to the engine and no better rule exists.
//
// THE ATLAS IS MINIFIED ON SCREEN. A 64-texel cell drawn at VIEWPORT_ICON_SIZE_POINTS points is a
// 2.91x minification on a 1x display and 1.45x on a 2x one, with mipLevels = 1 and bilinear
// filtering -- so every glyph feature is at least VIEWPORT_ICON_MIN_FEATURE_TEXELS thick or it
// crawls. A mip chain is the fix if the manual pass finds it does anyway (Device::uploadTexture
// already takes a level); it is a HANDOFF, not built here, because generating mips before their
// consumer exists is the abstraction 3.6.3 refused.

#include <aero/core/math.hpp>
#include <aero/scene/entity.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace engine {
class World;  // forward-declared: viewportIconFor takes it by reference and viewport_icons.cpp
              // includes <aero/scene/world.hpp> itself -- picking.hpp's own posture.
}  // namespace engine

namespace engine::editor {

enum class ViewportIconKind : std::uint8_t { DirectionalLight = 0, SpotLight, PointLight, Camera };
inline constexpr std::size_t VIEWPORT_ICON_COUNT = 4;

// ---- the atlas ---------------------------------------------------------------------------------
// ONE ROW OF SQUARE CELLS. 256x64 RGBA8Unorm = 65 536 bytes, built on the CPU and uploaded once.
inline constexpr std::uint32_t VIEWPORT_ICON_CELL_TEXELS = 64;
inline constexpr std::uint32_t VIEWPORT_ICON_ATLAS_WIDTH =
    VIEWPORT_ICON_CELL_TEXELS * static_cast<std::uint32_t>(VIEWPORT_ICON_COUNT);
inline constexpr std::uint32_t VIEWPORT_ICON_ATLAS_HEIGHT = VIEWPORT_ICON_CELL_TEXELS;
inline constexpr std::size_t VIEWPORT_ICON_ATLAS_BYTES =
    static_cast<std::size_t>(VIEWPORT_ICON_ATLAS_WIDTH) * VIEWPORT_ICON_ATLAS_HEIGHT * 4U;

// LOAD-BEARING, NOT COSMETIC. The sampler is Linear with ClampToEdge, and ClampToEdge clamps to the
// TEXTURE's edge, not to the CELL's -- so a texel sampled at a cell boundary blends with the
// NEIGHBOURING GLYPH. uvMin/uvMax select the cell; the hardware knows nothing about cells. Every
// glyph is inset by this many fully-transparent texels on all four sides, and VI4 asserts it. The
// failure it prevents is a faint ghost of the spot glyph on the sun icon's right edge, which no
// other tier can see.
inline constexpr std::uint32_t VIEWPORT_ICON_GUTTER_TEXELS = 2;

// The thinnest a glyph feature may be, in texels. See the minification note in the banner.
inline constexpr std::uint32_t VIEWPORT_ICON_MIN_FEATURE_TEXELS = 4;

// The drawn size, in LOGICAL POINTS -- the same unit the mouse arrives in. renderScene multiplies by
// the panel's framebuffer scale to get DebugBillboard::sizePx, which is in the frame's PIXELS;
// updatePick uses HALF_POINTS unscaled, because a PickRequest is in points throughout.
//
// A TUNING CONSTANT, judged on the validation page: no tier-0 case asserts its VALUE, only its two
// relationships (VI11), so a retune after that pass is a one-line change that reddens nothing.
inline constexpr float VIEWPORT_ICON_SIZE_POINTS = 22.0F;
inline constexpr float VIEWPORT_ICON_HALF_POINTS = VIEWPORT_ICON_SIZE_POINTS * 0.5F;

struct ViewportIconUv {
    Vec2 uvMin{};  // uvMin.y is the TOP of the sprite -- DebugBillboard's own convention
    Vec2 uvMax{};
    bool operator==(const ViewportIconUv&) const = default;
};

// The atlas CELL a kind occupies -- stated as a total switch rather than a cast of the enumerator, so
// a fifth enumerator is a -Wswitch error on the Linux lane rather than a silent read of a cell that
// does not exist (axisColorSrgbBytes's precedent, one header over). An out-of-range cast returns
// cell 0 rather than reading past anything.
[[nodiscard]] constexpr std::uint32_t viewportIconCell(ViewportIconKind kind) noexcept {
    switch (kind) {
        case ViewportIconKind::DirectionalLight:
            return 0U;
        case ViewportIconKind::SpotLight:
            return 1U;
        case ViewportIconKind::PointLight:
            return 2U;
        case ViewportIconKind::Camera:
            return 3U;
    }
    return 0U;
}

// EXACT IN FLOAT: VIEWPORT_ICON_COUNT is 4, so every boundary is k/4 and every division is exact.
[[nodiscard]] constexpr ViewportIconUv viewportIconUv(ViewportIconKind kind) noexcept {
    const auto cell = static_cast<float>(viewportIconCell(kind));
    const auto span = static_cast<float>(VIEWPORT_ICON_COUNT);
    return ViewportIconUv{.uvMin = Vec2{cell / span, 0.0F}, .uvMax = Vec2{(cell + 1.0F) / span, 1.0F}};
}

// ---- the predicate -----------------------------------------------------------------------------
// nullopt for a dead handle, a null handle, or an entity carrying none of the four components.
// eachEntity + has<T> ONLY, never World::each<T>, which logs one AERO_LOG_ERROR PER CALL for an
// unregistered type straight into 2.2.5's Console (picking.hpp's load-bearing rule). Emits NO log
// record on any path.
[[nodiscard]] std::optional<ViewportIconKind> viewportIconFor(const World& world, Entity entity);

// ---- the rasteriser ----------------------------------------------------------------------------
// Fills `out` with the atlas. RGB IS EXACTLY 255 EVERYWHERE and the glyph lives in ALPHA alone, which
// is what lets one glyph set carry four tints: debug_billboard.frag.hlsl is
// `uAtlas.Sample(uAtlasSmp, uv) * color`, so a white/coverage sprite tints EXACTLY.
//
// BIT-DETERMINISTIC ON EVERY LANE, and that is a constraint on how it is WRITTEN. The only
// approximating libm function it reaches is std::sqrt, which IEEE-754 requires to be correctly
// rounded (std::lround, the only other one, is exactly specified for every input); std::sin,
// std::cos and std::pow are DELIBERATELY ABSENT (VI7 pins that as source text, GR22's form) because
// none is required to be correctly rounded and a one-ulp difference would make a byte assertion a
// three-lane tolerance argument. The eight rays of the sun glyph are at multiples of 45 degrees,
// whose components are the exact literals 1, 0 and 0.70710678F -- no trigonometry is needed.
//
// Returns FALSE and writes NOTHING when out.size() != VIEWPORT_ICON_ATLAS_BYTES.
[[nodiscard]] bool buildViewportIconAtlas(std::span<std::uint8_t> out) noexcept;

}  // namespace engine::editor

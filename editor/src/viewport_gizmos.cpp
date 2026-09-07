// editor/src/viewport_gizmos.cpp — task E.2.3: the World walk that fills the debug batch with the
// scene's icons and the selection's light gizmos.
//
// TWO PASSES: one over the SELECTION (capped, dead handles skipped silently) that emits the light
// gizmos and fills the membership set as it goes, and one over the WHOLE world that emits the icons.
// Nothing here re-derives a decision another layer already made -- the icon predicate is
// viewportIconFor, the shapes are render::emitLightGizmo*, and the active directional light arrives
// as a plain Entity the caller resolved.

#include <aero/editor/viewport_gizmos.hpp>
#include <aero/scene/camera.hpp>
#include <aero/scene/light.hpp>
#include <aero/scene/spot_light.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>

#include <algorithm>
#include <cmath>  // std::isfinite, for the icon arm's own totality gate
#include <cstddef>
#include <optional>

namespace engine::editor {
namespace {

// Every icon is an OVERLAY billboard: flush() records the Overlay billboard bucket LAST of four, so
// an icon is over its own gizmo with no coordination at all.
constexpr auto ICON_DEPTH = render::DebugDepth::Overlay;

// The entity's world origin, spelled EXACTLY as selection_overlay.cpp spells it, which is what makes
// the icon, the retired diamond marker and the pick name the SAME point. The tree's `translationOf`
// is a file-local helper duplicated in two engine TUs and is not public API; a third copy would be
// the wrong move.
[[nodiscard]] Vec3 originOf(const Mat4& model) noexcept { return transformPoint(model, Vec3::zero()); }

// The entity's -Z world axis -- DirectionalLight's and SpotLight's own aiming rule (1.4.1's D6). A
// degenerate matrix normalizes to zero, which every emitter's own totality gate then refuses.
[[nodiscard]] Vec3 aimOf(const Mat4& model) noexcept {
    return normalizeOrZero(transformDirection(model, Vec3{0.0F, 0.0F, -1.0F}));
}

// light_gizmo.cpp's own `finite`, restated for the same reason it restated debug_draw.cpp's: both are
// in an anonymous namespace in another translation unit and neither is reachable. Three isfinite
// calls, nothing else.
[[nodiscard]] bool finite(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

}  // namespace

ViewportGizmoCounts emitViewportGizmos(const World& world, const ViewportGizmoParams& params,
                                       ViewportGizmoScratch& scratch, render::DebugDrawBatch& batch) {
    ViewportGizmoCounts counts{};

    // ---- step 1+2: the selection, capped, emitting its gizmos as it goes ------------------------
    // ONE loop, not two: the cap rule and the membership set are built by the same pass, so the
    // gizmos and the tints can never disagree about which entities survived the cap. A dead or null
    // handle is skipped SILENTLY and does NOT consume a cap slot (2.3.2's A7, the rule
    // buildSelectionMaskSet copies); only LIVE entities past the cap count as skipped.
    //
    // Gizmos are bounded by the SELECTION rather than by the scene (D4), which is what makes
    // MAX_HIGHLIGHTED_ENTITIES the one cap the outline, the marker and the gizmo all share.
    scratch.selectedIndices.clear();
    std::uint32_t admitted = 0;
    for (const Entity e : params.selected) {
        if (!world.alive(e)) {
            continue;
        }
        if (admitted >= params.entityCap) {
            ++counts.skippedOverCap;
            continue;
        }
        ++admitted;
        scratch.selectedIndices.push_back(e.index);

        const std::optional<ViewportIconKind> kind = viewportIconFor(world, e);
        if (!kind.has_value() || *kind == ViewportIconKind::Camera) {
            continue;  // a Camera draws its icon and NO gizmo (AC-2)
        }
        const Mat4 model = worldMatrix(world, e);
        render::LightGizmoStyle style = params.gizmoStyle;
        style.color = (e == params.primary) ? params.tints.primarySelected : params.tints.secondarySelected;
        std::uint32_t lines = 0;
        switch (*kind) {
            case ViewportIconKind::DirectionalLight:
                lines = render::emitDirectionalLightGizmo(
                    batch, {.origin = originOf(model), .direction = aimOf(model), .style = style});
                break;
            case ViewportIconKind::SpotLight: {
                const auto* const spot = world.get<SpotLight>(e);
                if (spot != nullptr) {
                    lines = render::emitSpotLightGizmo(batch, {.apex = originOf(model),
                                                               .direction = aimOf(model),
                                                               .range = spot->range,
                                                               .innerConeRadians = spot->innerConeRadians,
                                                               .outerConeRadians = spot->outerConeRadians,
                                                               .style = style});
                }
                break;
            }
            case ViewportIconKind::PointLight: {
                const auto* const point = world.get<PointLight>(e);
                if (point != nullptr) {
                    lines = render::emitPointLightGizmo(
                        batch, {.center = originOf(model), .range = point->range, .style = style});
                }
                break;
            }
            case ViewportIconKind::Camera:
                break;  // unreachable: filtered above. No `default:` -- a fifth kind is a -Wswitch error
        }
        if (lines > 0U) {
            counts.gizmoLines += lines;
            ++counts.gizmoEntities;
        }
    }
    // Sorted so the icon pass's membership test below is a binary search rather than a linear scan.
    std::sort(scratch.selectedIndices.begin(), scratch.selectedIndices.end());
    const auto isSelected = [&scratch](Entity e) {
        return std::binary_search(scratch.selectedIndices.begin(), scratch.selectedIndices.end(), e.index);
    };

    // ---- step 3: the icons, over the WHOLE world -----------------------------------------------
    world.eachEntity([&](Entity e) {
        const std::optional<ViewportIconKind> kind = viewportIconFor(world, e);
        if (!kind.has_value()) {
            return;
        }
        // THE TINT TABLE, in precedence order. Selection BEATS muting, deliberately: "what did I just
        // click" outranks "is this one live", and a user who selects it is about to read the
        // Inspector anyway. An INVALID activeDirectional mutes NOTHING -- that `.valid()` term is what
        // makes a caller who forgot to resolve it draw a normal picture rather than a uniformly grey
        // one (VG7).
        Vec4 tint = params.tints.unselected;
        if (e == params.primary) {
            tint = params.tints.primarySelected;
        } else if (isSelected(e)) {
            tint = params.tints.secondarySelected;
        } else if (*kind == ViewportIconKind::DirectionalLight && params.activeDirectional.valid() &&
                   !(e == params.activeDirectional)) {
            tint = params.tints.mutedDirectional;
        }
        const ViewportIconUv uv = viewportIconUv(*kind);
        const Vec3 center = originOf(worldMatrix(world, e));
        // THE ICON ARM'S OWN TOTALITY GATE, matching every emitter in light_gizmo.cpp field for field.
        // A Transform carrying a NaN or an infinity makes this centre non-finite, and
        // DebugDrawBatch::billboard would then take its REJECTION branch -- ++rejectedBillboardCount,
        // return false -- which AC-12 forbids for a degenerate input ("never a NaN, a rejection or a
        // log record") and which would additionally be counted here as though the budget were full.
        // Skipped silently and counted NOWHERE, exactly as an emitter returning 0 lines is.
        if (!finite(center)) {
            return;
        }
        if (batch.billboard(center, params.iconSizePixels, tint, uv.uvMin, uv.uvMax, ICON_DEPTH)) {
            ++counts.icons;
        } else {
            ++counts.iconsDropped;
        }
    });
    return counts;
}

}  // namespace engine::editor

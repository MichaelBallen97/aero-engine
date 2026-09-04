// editor/src/selection_overlay.cpp — task 2.3.2: the selection, turned into screen-space segments.
// ImGui-free by construction; the panel owns colour and drawing, this owns geometry (D6).
#include <aero/editor/picking.hpp>
#include <aero/editor/selection_overlay.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace engine::editor {

namespace {

// D8's marker: a 4-segment screen-space diamond at the entity's projected world origin, drawn ONLY
// for a SELECTED non-mesh entity. The recorded trade-off is real -- the pick target for a light is
// INVISIBLE until you hit it -- and the always-on alternative is really the first step of a
// gizmo-icon system, which is not this task's deliverable (Handoffs).
void appendPointMarker(const Mat4& viewProj, Vec3 worldPoint, Vec2 viewportSizePoints, OverlayRole role,
                       std::vector<OverlaySegment>& out) {
    Vec2 center{};
    if (!projectToViewport(viewProj, worldPoint, viewportSizePoints, center)) {
        return;  // at or behind the eye, or non-finite (E4/E7)
    }
    constexpr float H = POINT_MARKER_HALF_POINTS;
    const Vec2 top{center.x, center.y - H};
    const Vec2 right{center.x + H, center.y};
    const Vec2 bottom{center.x, center.y + H};
    const Vec2 left{center.x - H, center.y};
    out.push_back(OverlaySegment{.a = top, .b = right, .role = role});
    out.push_back(OverlaySegment{.a = right, .b = bottom, .role = role});
    out.push_back(OverlaySegment{.a = bottom, .b = left, .role = role});
    out.push_back(OverlaySegment{.a = left, .b = top, .role = role});
}

}  // namespace

void buildSelectionOverlay(const World& world, std::span<const Entity> entities, Entity primary, const Mat4& viewProj,
                           Vec2 viewportSizePoints, std::vector<OverlaySegment>& scratch) {
    scratch.clear();  // CALLER-OWNED SCRATCH: cleared on entry, reused across frames (D6/AC-18)
    std::size_t drawn = 0;
    for (const Entity entity : entities) {
        if (drawn >= MAX_HIGHLIGHTED_ENTITIES) {
            break;  // D15 -- a contract, never a log line
        }
        if (!world.alive(entity)) {
            // E2: dead and null handles are skipped SILENTLY, and the counter does NOT advance (A7) --
            // stale handles must not consume another entity's budget. This must NOT prune: that is
            // 2.2.1's job, done by the Hierarchy at the top of ITS onDraw (F34), and the Hierarchy can
            // be hidden or unregistered, in which case nothing prunes at all.
            continue;
        }
        ++drawn;
        const OverlayRole role = (entity == primary) ? OverlayRole::Primary : OverlayRole::Selected;
        const Mat4 model = worldMatrix(world, entity);  // silent identity when untransformed (F16/E3)
        // task E.1.4: EVERY entity here is a marker entity -- `entities` IS the marker list, decided
        // once by scene_render::buildSelectionMaskSet, so there is no second resolution and no second
        // place for "has geometry" to be answered differently (D11).
        //
        // E5, deliberately asymmetric with pickEntity: a zero-scaled entity still projects to a
        // finite point, which is informative and exactly what a user who typed scale = 0 should see.
        // You cannot CLICK a zero-volume object, but you should still SEE what you selected -- so
        // there is no determinant guard here.
        appendPointMarker(viewProj, transformPoint(model, Vec3::zero()), viewportSizePoints, role, scratch);
    }
}

}  // namespace engine::editor

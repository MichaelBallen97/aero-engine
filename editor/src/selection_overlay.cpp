// editor/src/selection_overlay.cpp — task 2.3.2: the selection, turned into screen-space segments.
// ImGui-free by construction; the panel owns colour and drawing, this owns geometry (D6).
#include <aero/editor/picking.hpp>
#include <aero/editor/scene_bounds.hpp>
#include <aero/editor/selection_overlay.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace engine::editor {

namespace {

[[nodiscard]] bool allFinite(Vec2 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y); }

// One entity's oriented box: 8 corners to CLIP space, then 12 edges, each clipped to
// w > CLIP_W_EPSILON IN CLIP SPACE (D14) and only THEN divided and mapped to viewport points.
// Clipping per EDGE rather than rejecting whole CORNERS is what stops edges popping out of existence
// as the camera approaches -- very visible, since 2.3.1's camera can fly straight into a selection.
//
// The corners come from scene_bounds.hpp's aabbCorner (task 3.1.5), which DELETED this file's own copy
// of the bit-order enumeration: BOX_EDGES is derived from that one assignment, so the bounds walk, the
// pick and the highlight agree by construction rather than by three matching comments.
void appendBoxEdges(const Mat4& mvp, const Aabb& box, Vec2 viewportSizePoints, OverlayRole role,
                    std::vector<OverlaySegment>& out) {
    std::array<Vec4, 8> clip{};
    for (std::size_t i = 0; i < clip.size(); ++i) {
        clip[i] = mvp * toVec4(aabbCorner(box, i), 1.0F);
    }
    for (const BoxEdge edge : BOX_EDGES) {
        Vec4 a = clip[edge.a];
        Vec4 b = clip[edge.b];
        if (!clipSegmentToNearPlane(a, b)) {
            continue;  // both endpoints at or behind the eye: this edge does not exist on screen
        }
        const Vec2 pa = ndcToViewportPoints(Vec2{a.x / a.w, a.y / a.w}, viewportSizePoints);
        const Vec2 pb = ndcToViewportPoints(Vec2{b.x / b.w, b.y / b.w}, viewportSizePoints);
        if (!allFinite(pa) || !allFinite(pb)) {
            continue;  // E4: ImDrawList would consume a NaN and corrupt the frame's whole vertex buffer
        }
        out.push_back(OverlaySegment{.a = pa, .b = pb, .role = role});
    }
}

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
                           Vec2 viewportSizePoints, std::vector<OverlaySegment>& scratch,
                           const MeshBoundsLookup* lookup) {
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
        if (world.has<MeshRenderer>(entity)) {          // silent for an unregistered type (F15)
            // task 3.1.5: the SAME localBoundsFor the pick and the frame walk call (INV-D6). A nullopt
            // -- an unresolved reference -- falls through to the marker below, the existing non-mesh
            // path, reached by one more condition rather than by new code.
            const std::optional<Aabb> local = localBoundsFor(*world.get<MeshRenderer>(entity), lookup);
            if (local.has_value()) {
                appendBoxEdges(viewProj * model, *local, viewportSizePoints, role, scratch);
                continue;
            }
        }
        // E5, deliberately asymmetric with pickEntity: a zero-scaled box collapses to a line or a
        // point on screen, which is finite, informative, and exactly what a user who typed scale = 0
        // should see. You cannot CLICK a zero-volume object, but you should still SEE what you
        // selected -- so there is no determinant guard here.
        appendPointMarker(viewProj, transformPoint(model, Vec3::zero()), viewportSizePoints, role, scratch);
    }
}

}  // namespace engine::editor

// editor/src/scene_bounds.cpp — task 2.3.1: Aabb's algebra + the three world-bounds entry points.
#include <aero/editor/scene_bounds.hpp>
#include <aero/editor/tree_walk.hpp>
#include <aero/scene/component.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace engine::editor {

namespace {

constexpr float INF = std::numeric_limits<float>::infinity();

[[nodiscard]] bool allFinite(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// The per-entity contribution shared by entityBounds/selectionBounds/sceneBounds.
void contribute(const World& world, Entity entity, Aabb& out) {
    if (!world.alive(entity)) {
        return;  // dead/null contribute nothing
    }
    const Mat4 m = worldMatrix(world, entity);        // silent identity when untransformed
    if (!world.has<MeshRenderer>(entity)) {           // SILENT for an unregistered type -- has<T> never logs
        out.expand(transformPoint(m, Vec3::zero()));  // a POINT at the world translation
        return;
    }
    constexpr float H = LOCAL_MESH_HALF_EXTENT;
    // Deliberately the 8-corner loop, not the min/max-of-columns trick: the latter is faster but is
    // easy to get subtly wrong under NEGATIVE scale, and this is not a hot path (focusOn runs once per
    // key press, not per frame). Transforming first and taking min/max afterwards makes negative/zero
    // scale correct for free (E20).
    for (int i = 0; i < 8; ++i) {
        const Vec3 local{(i & 1) != 0 ? H : -H, (i & 2) != 0 ? H : -H, (i & 4) != 0 ? H : -H};
        out.expand(transformPoint(m, local));
    }
}

// entityBounds' includeDescendants=true path, sharing one pair of scratch vectors across a
// selectionBounds span loop (walkForest is zero-allocation once warm; childArena returns to its
// entry size -- I4).
void boundsWithDescendants(const World& world, Entity root, std::vector<TreeWalkEntry>& stack,
                           std::vector<Entity>& arena, Aabb& out) {
    const std::array<Entity, 1> roots{root};
    walkForest(
        world, std::span<const Entity>(roots), stack, arena,
        [&](Entity e) {
            contribute(world, e, out);
            return true;  // always descend -- every subtree contributes
        },
        [](Entity /*e*/, bool /*open*/) {});  // nothing owed on unwind: contribute() is not ImGui state
}

}  // namespace

Aabb Aabb::empty() noexcept { return Aabb{Vec3{INF, INF, INF}, Vec3{-INF, -INF, -INF}}; }

bool Aabb::valid() const noexcept {
    return allFinite(min) && allFinite(max) && min.x <= max.x && min.y <= max.y && min.z <= max.z;
}

Vec3 Aabb::center() const noexcept { return (min + max) * 0.5F; }
Vec3 Aabb::size() const noexcept { return max - min; }
float Aabb::radius() const noexcept { return length(size()) * 0.5F; }

void Aabb::expand(Vec3 point) noexcept {
    min = {std::min(min.x, point.x), std::min(min.y, point.y), std::min(min.z, point.z)};
    max = {std::max(max.x, point.x), std::max(max.y, point.y), std::max(max.z, point.z)};
}

void Aabb::expand(const Aabb& other) noexcept {
    if (!other.valid()) {
        return;  // NO-OP: an invalid box contributes nothing
    }
    expand(other.min);
    expand(other.max);
}

Aabb entityBounds(const World& world, Entity entity, bool includeDescendants) {
    Aabb out = Aabb::empty();
    if (!includeDescendants) {
        contribute(world, entity, out);
        return out;
    }
    std::vector<TreeWalkEntry> stack;
    std::vector<Entity> arena;
    boundsWithDescendants(world, entity, stack, arena, out);
    return out;
}

Aabb selectionBounds(const World& world, std::span<const Entity> entities) {
    Aabb out = Aabb::empty();
    std::vector<TreeWalkEntry> stack;  // hoisted ONCE across the span loop (walkForest stays warm)
    std::vector<Entity> arena;
    for (const Entity e : entities) {
        if (!world.alive(e)) {
            continue;  // dead/null handles skipped -- they must NOT drag the box to the origin (E17)
        }
        boundsWithDescendants(world, e, stack, arena, out);
    }
    return out;
}

Aabb sceneBounds(const World& world) {
    Aabb out = Aabb::empty();
    // O(1) FAST PATH ONLY (D10 as amended by plan C2/R-1). NOT an ERROR suppressor: every World()
    // registers MeshRenderer so this can never be false on a live World, and the one state where it
    // fires -- a moved-from World -- suppresses no log either, because beginQuery bails at
    // impl == nullptr before its own ERROR. Silence is held STRUCTURALLY, by the walk below never
    // calling each<T>. See scene_bounds.hpp / plan §A (C2) / §R (R-1) for the full citation trail.
    if (!world.registered(componentTypeId<MeshRenderer>())) {
        return out;
    }
    world.eachEntity([&](Entity e) {       // const (world.hpp)
        if (world.has<MeshRenderer>(e)) {  // silent for a live, unregistered-elsewhere id
            contribute(world, e, out);
        }
    });
    return out;
}

// Do not "improve" this into each<MeshRenderer> later. It would force the signature non-const, break
// symmetry with entityBounds/selectionBounds, and re-open the hazard this walk currently cannot have
// -- for no gain, since the guard's ERROR-suppression story was never reachable. scene_bounds_test.cpp
// case 12b is the proof that the hazard is real for an ACTUALLY unregistered type.

}  // namespace engine::editor

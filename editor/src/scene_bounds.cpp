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
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

constexpr float INF = std::numeric_limits<float>::infinity();

[[nodiscard]] bool allFinite(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// The per-entity contribution shared by entityBounds/selectionBounds/sceneBounds.
void contribute(const World& world, Entity entity, const MeshBoundsLookup* lookup, Aabb& out) {
    if (!world.alive(entity)) {
        return;  // dead/null contribute nothing
    }
    const Mat4 m = worldMatrix(world, entity);  // silent identity when untransformed
    std::optional<Aabb> local;
    if (world.has<MeshRenderer>(entity)) {  // SILENT for an unregistered type -- has<T> never logs
        local = localBoundsFor(*world.get<MeshRenderer>(entity), lookup);
    }
    if (!local.has_value()) {
        // The POINT class: no MeshRenderer at all, or (task 3.1.5) a reference the editor cannot
        // resolve yet. One path, not two -- which is why the optional shape was chosen over "return
        // the cube box while loading".
        out.expand(transformPoint(m, Vec3::zero()));
        return;
    }
    // Deliberately the 8-corner loop, not the min/max-of-columns trick: the latter is faster but is
    // easy to get subtly wrong under NEGATIVE scale, and this is not a hot path (focusOn runs once per
    // key press, not per frame). Transforming first and taking min/max afterwards makes negative/zero
    // scale correct for free (E20).
    for (std::size_t i = 0; i < 8; ++i) {
        out.expand(transformPoint(m, aabbCorner(*local, i)));
    }
}

// entityBounds' includeDescendants=true path, sharing one pair of scratch vectors across a
// selectionBounds span loop (walkForest is zero-allocation once warm; childArena returns to its
// entry size -- I4).
void boundsWithDescendants(const World& world, Entity root, const MeshBoundsLookup* lookup,
                           std::vector<TreeWalkEntry>& stack, std::vector<Entity>& arena, Aabb& out) {
    const std::array<Entity, 1> roots{root};
    walkForest(
        world, std::span<const Entity>(roots), stack, arena,
        [&](Entity e) {
            contribute(world, e, lookup, out);
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

Aabb primitiveLocalBounds(std::uint32_t primitive) noexcept {
    // Cube (0) and Sphere (1) share the box: the cube's faces sit at +/-0.5 and the sphere's radius IS
    // 0.5. The Plane (2) is a unit quad in XZ with ZERO thickness in Y -- appendQuad(center = 0,
    // u = +X, v = -Z) puts its four corners at (+/-0.5, 0, +/-0.5), verified against
    // engine/render/src/primitives.cpp rather than remembered.
    constexpr Aabb CUBE{Vec3{-0.5F, -0.5F, -0.5F}, Vec3{0.5F, 0.5F, 0.5F}};
    constexpr Aabb PLANE{Vec3{-0.5F, 0.0F, -0.5F}, Vec3{0.5F, 0.0F, 0.5F}};
    return primitive == 2U ? PLANE : CUBE;  // out of range -> CUBE, as clampPrimitive clamps to Cube
}

Vec3 aabbCorner(const Aabb& box, std::size_t i) noexcept {
    return Vec3{(i & 1U) != 0U ? box.max.x : box.min.x, (i & 2U) != 0U ? box.max.y : box.min.y,
                (i & 4U) != 0U ? box.max.z : box.min.z};
}

void MeshBoundsLookup::set(MeshBoundsKey key, const Aabb& box) {
    if (!key.mesh.valid()) {
        return;  // a nil guid is the NONE sentinel, never a key (guid.hpp)
    }
    const auto it = std::lower_bound(
        entries.begin(), entries.end(), key,
        [](const std::pair<MeshBoundsKey, Aabb>& entry, const MeshBoundsKey& probe) { return entry.first < probe; });
    if (it != entries.end() && it->first == key) {
        it->second = box;
        return;
    }
    entries.insert(it, std::pair<MeshBoundsKey, Aabb>{key, box});
}

void MeshBoundsLookup::removeMesh(Guid mesh) noexcept {
    if (!mesh.valid()) {
        return;
    }
    // Every meshIndex of one guid is CONTIGUOUS under MeshBoundsKey::operator<, so the retire is one
    // erase range rather than a scan. `first` is the smallest key that guid can hold and `last` the
    // smallest key the NEXT guid can, which needs no successor arithmetic on a Guid.
    const auto byKey = [](const std::pair<MeshBoundsKey, Aabb>& entry, const MeshBoundsKey& probe) {
        return entry.first < probe;
    };
    const auto first = std::lower_bound(entries.begin(), entries.end(), MeshBoundsKey{mesh, 0}, byKey);
    auto last = first;
    while (last != entries.end() && last->first.mesh == mesh) {
        ++last;
    }
    entries.erase(first, last);
}

void MeshBoundsLookup::clear() noexcept { entries.clear(); }

const Aabb* MeshBoundsLookup::find(MeshBoundsKey key) const noexcept {
    if (!key.mesh.valid()) {
        return nullptr;  // stated, not derived from set's refusal: the two must not drift
    }
    const auto it = std::lower_bound(
        entries.begin(), entries.end(), key,
        [](const std::pair<MeshBoundsKey, Aabb>& entry, const MeshBoundsKey& probe) { return entry.first < probe; });
    return it != entries.end() && it->first == key ? &it->second : nullptr;
}

std::size_t MeshBoundsLookup::size() const noexcept { return entries.size(); }

std::optional<Aabb> localBoundsFor(const MeshRenderer& renderer, const MeshBoundsLookup* lookup) noexcept {
    if (!renderer.mesh.valid()) {
        return primitiveLocalBounds(renderer.primitive);  // ALWAYS engaged -- the primitive path
    }
    if (lookup == nullptr) {
        return std::nullopt;
    }
    const Aabb* const box = lookup->find(MeshBoundsKey{renderer.mesh, renderer.meshIndex});
    // nullopt for loading, failed, missing, or a stale meshIndex -- one answer for every unresolved
    // cause, because every consumer treats them identically (the non-mesh class).
    return box != nullptr ? std::optional<Aabb>{*box} : std::nullopt;
}

Aabb entityBounds(const World& world, Entity entity, bool includeDescendants, const MeshBoundsLookup* lookup) {
    Aabb out = Aabb::empty();
    if (!includeDescendants) {
        contribute(world, entity, lookup, out);
        return out;
    }
    std::vector<TreeWalkEntry> stack;
    std::vector<Entity> arena;
    boundsWithDescendants(world, entity, lookup, stack, arena, out);
    return out;
}

Aabb selectionBounds(const World& world, std::span<const Entity> entities, const MeshBoundsLookup* lookup) {
    Aabb out = Aabb::empty();
    std::vector<TreeWalkEntry> stack;  // hoisted ONCE across the span loop (walkForest stays warm)
    std::vector<Entity> arena;
    for (const Entity e : entities) {
        if (!world.alive(e)) {
            continue;  // dead/null handles skipped -- they must NOT drag the box to the origin (E17)
        }
        boundsWithDescendants(world, e, lookup, stack, arena, out);
    }
    return out;
}

Aabb sceneBounds(const World& world, const MeshBoundsLookup* lookup) {
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
            contribute(world, e, lookup, out);
        }
    });
    return out;
}

// Do not "improve" this into each<MeshRenderer> later. It would force the signature non-const, break
// symmetry with entityBounds/selectionBounds, and re-open the hazard this walk currently cannot have
// -- for no gain, since the guard's ERROR-suppression story was never reachable. scene_bounds_test.cpp
// case 12b is the proof that the hazard is real for an ACTUALLY unregistered type.

}  // namespace engine::editor

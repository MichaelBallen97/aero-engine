#include <aero/core/math.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/scene/camera.hpp>
#include <aero/scene/light.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace engine::editor {

bool isDescendantOf(const World& world, Entity candidate, Entity root) {
    if (!candidate.valid() || !root.valid()) {
        return false;
    }
    // Walk UP candidate's parent chain -- O(depth), no allocation, ITERATIVE (D13/I7). This is the
    // same shape World::setParent's own cycle check uses, and it is why the World can never be asked
    // to close a cycle from this file.
    for (Entity cur = candidate; cur.valid(); cur = world.parent(cur)) {
        if (cur == root) {
            return true;  // `root` IS candidate, or is an ancestor of it
        }
    }
    return false;
}

std::vector<Entity> topMost(const World& world, std::span<const Entity> entities) {
    std::vector<Entity> out;
    out.reserve(entities.size());
    for (const Entity e : entities) {
        if (!world.alive(e)) {
            continue;  // dead/null handles are dropped
        }
        bool covered = false;
        for (const Entity other : entities) {
            // `other` must be a STRICT ancestor: the self case would drop everything.
            if (!(other == e) && world.alive(other) && isDescendantOf(world, e, other)) {
                covered = true;
                break;
            }
        }
        if (!covered && std::find(out.begin(), out.end(), e) == out.end()) {
            out.push_back(e);  // input order preserved (D19)
        }
    }
    return out;
}

void entityLabel(const World& world, Entity entity, std::string& out) {
    out.clear();  // cleared first -- one allocation per frame
    const std::string_view name = world.name(entity);
    if (!name.empty()) {
        out.assign(name);
        return;
    }
    out.append("Entity ").append(std::to_string(entity.index));  // D21 -- never a blank row
}

Entity createEntity(World& world, Entity parent, std::string_view name) {
    const Entity created = world.create();
    if (!created.valid()) {
        return {};  // E29 -- a moved-from World; World already logged
    }
    world.add<Transform>(created, Transform{});
    if (!name.empty()) {
        world.setName(created, name);
    }
    if (parent.valid() && !world.setParent(created, parent)) {
        world.destroy(created);  // a dead parent: do not leave an orphan behind
        return {};
    }
    return created;
}

Entity resolveCreateChildParent(std::span<const Entity> selection, Entity primary, Entity rightClicked) {
    const bool outsideSelection =
        rightClicked.valid() && std::find(selection.begin(), selection.end(), rightClicked) == selection.end();
    return outsideSelection ? rightClicked : primary;
}

std::size_t destroyEntities(World& world, std::span<const Entity> entities) {
    const std::vector<Entity> roots = topMost(world, entities);  // D19/E8
    std::size_t destroyed = 0;
    for (const Entity e : roots) {
        // An earlier subtree may already have taken this one (topMost cannot see a relationship the
        // caller's span does not contain); destroy() answers false silently for a dead handle.
        if (world.destroy(e)) {
            ++destroyed;
        }
    }
    return destroyed;
}

bool canReparent(const World& world, Entity child, Entity parent) {
    if (!world.alive(child)) {
        return false;
    }
    if (!parent.valid()) {
        return true;  // detach to root is always legal (E17)
    }
    if (!world.alive(parent) || child == parent) {
        return false;
    }
    return !isDescendantOf(world, parent, child);  // would close a cycle (E14)
}

bool reparentEntity(World& world, Entity child, Entity parent) {
    if (!canReparent(world, child, parent)) {
        return false;  // predicted refusal -> World never logs
    }
    return world.setParent(child, parent);
}

std::vector<Entity> reparentTargets(const World& world, std::span<const Entity> selection, Entity dragged) {
    const bool draggedIsSelected = std::find(selection.begin(), selection.end(), dragged) != selection.end();
    if (draggedIsSelected) {
        return topMost(world, selection);  // D19: move the whole selection as one, never split (E16)
    }
    return topMost(world, std::vector<Entity>{dragged});  // dragging an unselected row moves only it
}

std::vector<Entity> duplicateEntities(World& world, std::span<const Entity> entities) {
    struct Pending {
        Entity source;
        Entity destinationParent;
    };
    std::vector<Entity> created;
    std::vector<Pending> stack;
    std::vector<Entity> children;  // reused per node; children are pushed back-to-front (E11)
    std::string nameCopy;          // reused; materialised BEFORE any mutation (D18)

    const std::vector<Entity> sources = topMost(world, entities);  // D19/E9
    created.reserve(sources.size());
    for (const Entity source : sources) {
        stack.clear();
        stack.push_back(Pending{source, world.parent(source)});  // E10: a root's copy is a root
        Entity subtreeRoot{};
        while (!stack.empty()) {
            const Pending job = stack.back();  // BY VALUE -- the push below would dangle a reference
            stack.pop_back();

            const Entity copy = world.create();
            if (!copy.valid()) {
                break;  // moved-from World; nothing more can be created
            }
            if (!subtreeRoot.valid()) {
                subtreeRoot = copy;
            }
            // D18: name() returns a view into World-owned storage; setName on the same World is the
            // same aliasing class addRaw warns about. Copy first, always.
            nameCopy.assign(world.name(job.source));
            if (!nameCopy.empty()) {
                world.setName(copy, nameCopy);
            }
            if (job.destinationParent.valid()) {
                world.setParent(copy, job.destinationParent);
            }
            // D4: the walk is over the REGISTRATION TABLE, not a hardcoded list of built-ins. A
            // component type registered after them is copied with zero change here (AC-13/E12).
            for (std::size_t i = 0; i < world.componentTypeCount(); ++i) {
                const engine::ComponentTypeId id = world.componentTypeAt(i);
                if (world.hasRaw(id, job.source)) {
                    world.copyComponent(id, job.source, copy);
                }
            }
            // READ-ONLY inside eachChild (F6): the callback only appends to a local vector; every
            // World mutation for these children happens in a LATER iteration, outside the walk.
            children.clear();
            world.eachChild(job.source, [&children](Entity c) { children.push_back(c); });
            // Push back-to-front so the LIFO stack pops them in ATTACH order and the copy's own child
            // list ends up in the same order as the source's (E11).
            for (std::size_t i = children.size(); i > 0; --i) {
                stack.push_back(Pending{children[i - 1U], copy});
            }
        }
        if (subtreeRoot.valid()) {
            created.push_back(subtreeRoot);
        }
    }
    return created;
}

void RootOrder::reconcile(const World& world) {
    // 1. drop entries that died or stopped being roots -- surviving order is preserved (AC-16).
    std::erase_if(order, [&world](Entity e) { return !world.alive(e) || world.parent(e).valid(); });

    // 2. stamp the survivors by index, recording GENERATION -- not mere presence. A recycled slot
    //    reuses the index with a new generation and must NOT be mistaken for its predecessor.
    scratch.clear();
    for (const Entity e : order) {
        if (e.index >= stamp.size()) {
            stamp.resize(static_cast<std::size_t>(e.index) + 1U, 0U);
        }
        stamp[e.index] = e.generation;
        scratch.push_back(e);
    }

    // 3. append every root not already stamped, in World::eachEntity order.
    world.eachEntity([&](Entity e) {
        if (world.parent(e).valid()) {
            return;
        }
        const bool known = e.index < stamp.size() && stamp[e.index] == e.generation;
        if (!known) {
            order.push_back(e);
        }
    });

    // 4. clear exactly the indices stamped in pass 2 -- never a full-vector fill, so the call stays
    //    O(N + R) instead of O(maxEntityIndex).
    for (const Entity e : scratch) {
        stamp[e.index] = 0U;
    }
    scratch.clear();
}

std::span<const Entity> RootOrder::entities() const noexcept { return std::span<const Entity>{order}; }

std::size_t RootOrder::indexOf(Entity entity) const noexcept {
    const auto it = std::find(order.begin(), order.end(), entity);
    return it == order.end() ? NO_ROOT_SLOT : static_cast<std::size_t>(it - order.begin());
}

void RootOrder::insert(Entity entity, std::size_t index) {
    if (!entity.valid() || std::find(order.begin(), order.end(), entity) != order.end()) {
        return;  // invalid, or already tracked -- reconcile() owns the append path
    }
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(std::min(index, order.size())), entity);
}

void RootOrder::clear() noexcept {
    order.clear();
    scratch.clear();
    std::fill(stamp.begin(), stamp.end(), 0U);
}

void seedDefaultScene(World& world) {
    const Entity camera = createEntity(world, {}, "Main Camera");
    if (camera.valid()) {
        world.add<Transform>(camera, Transform{.position = {0.0F, 1.0F, 5.0F}});
        world.add<Camera>(camera, Camera{});
    }
    const Entity light = createEntity(world, {}, "Directional Light");
    if (light.valid()) {
        world.add<Transform>(light, Transform{.rotation = fromAxisAngle(Vec3{1.0F, 0.0F, 0.0F}, radians(-50.0F))});
        world.add<DirectionalLight>(light, DirectionalLight{});
    }
    const Entity cube = createEntity(world, {}, "Cube");
    if (cube.valid()) {
        world.add<MeshRenderer>(cube, MeshRenderer{});  // primitive = 0 == Cube (F23)
    }
}

}  // namespace engine::editor

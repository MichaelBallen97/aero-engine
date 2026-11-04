// engine/scene/src/prefab.cpp -- task 4.4.4: engine::Prefab, a saved entity tree.
//
// No EnTT in this TU: every read and write goes through the public World API, and the component copy is the
// type-erased addRaw/getRaw pair. Both walks are iterative with an explicit stack (misc-no-recursion), children
// are read into a local before anything is mutated and pushed back-to-front so they pop in attach order.

#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/core/profiler.hpp>
#include <aero/scene/prefab.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace engine {

namespace {

// One pending node of a walk: the entity to copy and the parent its copy goes under (null for the walk's root).
struct Pending {
    Entity source;
    Entity destinationParent;
};

// The snapshot loop (editor/src/scene_snapshot.cpp), over the registration table. A type the far World lacks is
// skipped with one WARN per type per walk, never an addRaw ERROR.
void copyComponents(const World& from, Entity source, World& to, Entity copy, std::vector<ComponentTypeId>& warned) {
    for (std::size_t i = 0; i < from.componentTypeCount(); ++i) {
        const ComponentTypeId id = from.componentTypeAt(i);
        if (!from.hasRaw(id, source)) {
            continue;
        }
        if (!to.registered(id)) {
            if (std::find(warned.begin(), warned.end(), id) == warned.end()) {
                warned.push_back(id);
                AERO_LOG_WARN("scene: a prefab skipped the unregistered component type '{}'",
                              from.componentTypeName(id));
            }
            continue;
        }
        // A tag has no value: getRaw gives a non-null address anyway, and addRaw stores nothing for an empty type.
        to.addRaw(id, copy, from.getRaw(id, source));
        if (!to.hasRaw(id, copy)) {
            AERO_LOG_WARN("scene: a prefab could not copy component '{}' -- it did not take",
                          from.componentTypeName(id));
        }
    }
}

// Copies the tree under `sourceRoot` into `to`, preorder, and returns the copy of the root. `rootParent` is the
// parent the root's copy is linked under (null: a root). Entity{} when the first create fails; a later failure
// destroys what the walk made and returns Entity{}.
Entity copyTree(const World& from, Entity sourceRoot, World& to, Entity rootParent,
                std::vector<ComponentTypeId>& warned) {
    std::vector<Pending> stack;
    std::vector<Entity> children;
    std::string nameCopy;  // World::name()'s view dies on the next mutation of the same World
    Entity rootCopy{};
    stack.push_back(Pending{sourceRoot, rootParent});
    while (!stack.empty()) {
        const Pending job = stack.back();  // by value: the push below would dangle a reference
        stack.pop_back();

        const Entity copy = to.create();
        if (!copy.valid()) {
            if (rootCopy.valid()) {
                to.destroy(rootCopy);
            }
            return Entity{};
        }
        if (!rootCopy.valid()) {
            rootCopy = copy;
        }
        nameCopy.assign(from.name(job.source));
        if (!nameCopy.empty()) {
            to.setName(copy, nameCopy);
        }
        if (job.destinationParent.valid()) {
            to.setParent(copy, job.destinationParent);
        }
        copyComponents(from, job.source, to, copy, warned);

        children.clear();
        from.eachChild(job.source, [&children](Entity child) { children.push_back(child); });
        for (std::size_t i = children.size(); i > 0; --i) {
            stack.push_back(Pending{children[i - 1U], copy});
        }
    }
    return rootCopy;
}

// placement o stored, for a root. The placement's rotation is normalized first, the transform helpers' posture, so a
// zero Quat from a script is the identity and not a collapsed tree; a NaN stays NaN. The stored rotation is the
// file's value and is not touched.
Transform placedRoot(const Transform& stored, const Transform& placement) {
    const Quat rotation = normalizeOrIdentity(placement.rotation);
    const Vec3 scaled{placement.scale.x * stored.position.x, placement.scale.y * stored.position.y,
                      placement.scale.z * stored.position.z};
    Transform result;
    result.position = placement.position + rotation * scaled;
    result.rotation = rotation * stored.rotation;
    result.scale = Vec3{placement.scale.x * stored.scale.x, placement.scale.y * stored.scale.y,
                        placement.scale.z * stored.scale.z};
    return result;
}

[[nodiscard]] Vec3 worldPosition(const World& world, Entity entity) {
    return transformPoint(worldMatrix(world, entity), Vec3{});
}

}  // namespace

Prefab::Prefab(World&& world, Entity root) noexcept : store(std::move(world)), rootEntity(root) {}

std::optional<Prefab> Prefab::capture(const World& source, std::span<const Entity> roots,
                                      std::string_view wrapperName) {
    if (roots.empty()) {
        return std::nullopt;
    }
    for (const Entity root : roots) {
        if (!source.alive(root)) {
            return std::nullopt;
        }
    }

    const bool single = roots.size() == 1U;
    Vec3 pivot{};  // the mean of the roots' world positions; unused when single
    if (!single) {
        for (const Entity root : roots) {
            pivot = pivot + worldPosition(source, root);
        }
        pivot = pivot / static_cast<float>(roots.size());
    }

    Prefab result{World{}, Entity{}};
    World& store = result.store;
    Entity wrapper{};
    if (!single) {
        wrapper = store.create();
        if (!wrapper.valid()) {
            return std::nullopt;
        }
        store.add<Transform>(wrapper, Transform{});
        if (!wrapperName.empty()) {
            store.setName(wrapper, wrapperName);
        }
        result.rootEntity = wrapper;
    }

    std::vector<ComponentTypeId> warned;
    for (const Entity root : roots) {
        const Entity copy = copyTree(source, root, store, wrapper, warned);
        if (!copy.valid()) {
            return std::nullopt;
        }
        if (single) {
            result.rootEntity = copy;
        }
        if (Transform* transform = store.get<Transform>(copy)) {
            // One root's position is the literal zero, never p - p, which is NaN for a NaN or infinite world
            // position and must not be written into a file.
            transform->position = single ? Vec3{} : worldPosition(source, root) - pivot;
        }
    }
    return std::optional<Prefab>(std::move(result));
}

std::optional<Prefab> Prefab::fromWorld(World&& world) {
    std::size_t rootCount = 0;
    Entity root{};
    world.eachEntity([&](Entity entity) {
        if (!world.parent(entity).valid()) {
            ++rootCount;
            root = entity;
        }
    });
    if (rootCount != 1U) {
        return std::nullopt;
    }
    return std::optional<Prefab>(Prefab{std::move(world), root});
}

Entity Prefab::instantiate(World& destination, Transform placement, Entity parent) const {
    AERO_PROFILE_ZONE;
    if (!store.alive(rootEntity)) {
        return Entity{};  // a moved-from Prefab
    }
    if (parent.valid() && !destination.alive(parent)) {
        return Entity{};  // refused before anything is created
    }
    std::vector<ComponentTypeId> warned;
    const Entity root = copyTree(store, rootEntity, destination, Entity{}, warned);
    if (!root.valid()) {
        return Entity{};
    }
    if (Transform* stored = destination.get<Transform>(root)) {
        *stored = placedRoot(*stored, placement);
    } else {
        destination.add<Transform>(root, placement);
    }
    if (parent.valid() && !destination.setParent(root, parent)) {
        destination.destroy(root);  // a belt: a live parent and a fresh root cannot refuse
        return Entity{};
    }
    return root;
}

}  // namespace engine

#include <aero/editor/tree_walk.hpp>
#include <aero/scene/world.hpp>

namespace engine::editor {

void walkForest(const World& world, std::span<const Entity> roots, std::vector<TreeWalkEntry>& stack,
                std::vector<Entity>& childArena, const std::function<bool(Entity)>& enter,
                const std::function<void(Entity, bool)>& unwind) {
    const std::size_t stackFloor = stack.size();
    // Seed BACK-TO-FRONT so the LIFO stack pops the roots in DISPLAY (front-to-back) order.
    for (std::size_t i = roots.size(); i > 0; --i) {
        stack.push_back(TreeWalkEntry{.entity = roots[i - 1U]});
    }

    while (stack.size() > stackFloor) {
        // INDEX, never a reference: this loop push_back()s into `stack`, which would dangle a
        // `TreeWalkEntry&` on reallocation. (ASan would catch it; a reader would not.)
        const std::size_t top = stack.size() - 1U;

        if (!stack[top].entered) {
            stack[top].entered = true;
            stack[top].open = enter(stack[top].entity);
            if (stack[top].open) {
                stack[top].childBegin = childArena.size();
                // READ-ONLY inside eachChild (F6): the callback only appends to childArena.
                world.eachChild(stack[top].entity, [&childArena](Entity c) { childArena.push_back(c); });
                stack[top].childNext = stack[top].childBegin;
                stack[top].childEnd = childArena.size();
            }
            continue;
        }

        if (stack[top].open && stack[top].childNext < stack[top].childEnd) {
            const Entity child = childArena[stack[top].childNext];
            ++stack[top].childNext;
            stack.push_back(TreeWalkEntry{.entity = child});  // invalidates any reference to stack[top]
            continue;
        }

        // Exhausted: unwind, then shrink the arena back to what it was before this node's children
        // were appended (I4) -- always, so a caller that never expands anything sees the arena stay
        // exactly at its floor.
        const Entity entity = stack[top].entity;
        const bool open = stack[top].open;
        if (open) {
            childArena.resize(stack[top].childBegin);
        }
        unwind(entity, open);
        stack.pop_back();
    }
}

}  // namespace engine::editor

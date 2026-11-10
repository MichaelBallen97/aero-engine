// Aero Engine -- ParkedScene (task 4.7.2). The order of the statements below is the contract: in park() every call
// that can throw runs before the first move, and in restore() the first prune sees the play World and the second the
// restored one.
#include <aero/editor/parked_scene.hpp>

#include <utility>

namespace engine::editor {

bool ParkedScene::park(World& live, const RootOrder& roots) {
    if (parkedWorld.has_value()) {
        return false;
    }
    World copy = live.clone();             // may throw: nothing has moved yet
    RootOrder rootsCopy = roots;           // may throw: likewise
    parkedWorld.emplace(std::move(live));  // noexcept from here on
    live = std::move(copy);
    parkedRoots = std::move(rootsCopy);
    return true;
}

bool ParkedScene::restore(World& live, RootOrder& roots, Selection& selection) {
    if (!parkedWorld.has_value()) {
        return false;
    }
    selection.prune(live);           // the play World: an entity the game destroyed leaves the selection
    live = std::move(*parkedWorld);  // the World from before Play is back; the play copy is destroyed here
    parkedWorld.reset();
    roots = std::exchange(parkedRoots, RootOrder{});
    selection.prune(live);  // the restored World: an entity that existed only in play leaves it too
    return true;
}

bool ParkedScene::parked() const noexcept { return parkedWorld.has_value(); }

}  // namespace engine::editor

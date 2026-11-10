// tests/editor/parked_scene_test.cpp -- task 4.7.2: ParkedScene, the scene a game's play leaves untouched (RS1-RS6).
// A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp. Tier 0: no window, no GPU, no ImGui.
// No #if of any kind: the scene-text arms branch on sceneIoAvailable() at run time.
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/parked_scene.hpp>
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/scene.hpp>

#include "../world_dump.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>  // MSVC: a CHECK over a std::string needs the complete std::ostream (the 0.4.1 trap)
#include <span>
#include <string>
#include <utility>
#include <vector>

using engine::Entity;
using engine::World;
using engine::editor::createEntity;
using engine::editor::ParkedScene;
using engine::editor::RootOrder;
using engine::editor::sceneIoAvailable;
using engine::editor::sceneToText;
using engine::editor::seedDefaultScene;
using engine::editor::Selection;

namespace {

// The named handles buildScene() hands back.
struct Named {
    Entity r1;
    Entity c1;
    Entity c2;
    Entity g;
    Entity r2;
    Entity r3;
    Entity tmp;  // created and destroyed: a free slot
};

// Deterministic: called twice it gives two Worlds with identical handles and identical root orders. The root order
// ends as [...seed, R2, R3, R1], which a plain reconcile of the finished World would not give.
Named buildScene(World& world, RootOrder& roots) {
    seedDefaultScene(world);
    Named n;
    n.r1 = createEntity(world, {}, "R1");
    n.c1 = createEntity(world, n.r1, "C1");
    n.c2 = createEntity(world, n.r1, "C2");
    n.g = createEntity(world, n.c2, "G");
    n.r2 = createEntity(world, {}, "R2");
    n.r3 = createEntity(world, {}, "R3");
    n.tmp = createEntity(world, {}, "Tmp");
    world.destroy(n.tmp);
    engine::MeshRenderer mesh;
    mesh.mesh = engine::Guid{11ULL, 12ULL};
    mesh.meshIndex = 3;
    world.add<engine::MeshRenderer>(n.c1, mesh);
    world.add<engine::PointLight>(n.r2, engine::PointLight{});
    world.add<engine::ScriptComponent>(n.g, engine::ScriptComponent{engine::Guid{21ULL, 22ULL}});
    roots.reconcile(world);
    world.setParent(n.r1, n.r2);
    roots.reconcile(world);
    world.setParent(n.r1, Entity{});
    roots.reconcile(world);
    return n;
}

std::vector<Entity> rootsOf(const RootOrder& roots) {
    const std::span<const Entity> view = roots.entities();
    return {view.begin(), view.end()};
}

// The three equalities the park promises: the dump, the built-in values, and (where reflection is built) the text.
void checkSameScene(World& actual, World& twin) {
    CHECK(world_test::dumpWorld(actual) == world_test::dumpWorld(twin));
    CHECK(world_test::firstBuiltinDifference(actual, twin).empty());
    CHECK(world_test::firstBuiltinDifference(twin, actual).empty());
    const std::optional<std::string> left = sceneToText(actual);
    const std::optional<std::string> right = sceneToText(twin);
    if (sceneIoAvailable()) {
        CHECK(left.has_value());
        CHECK((left == right));
    } else {
        CHECK_FALSE(left.has_value());
        CHECK_FALSE(right.has_value());
    }
}

bool nextCreatesEqual(World& actual, World& twin, int count) {
    bool equal = true;
    for (int i = 0; i < count; ++i) {
        equal = (actual.create() == twin.create()) && equal;
    }
    return equal;
}

}  // namespace

TEST_CASE("parked scene: park leaves an identical copy and restore brings the original back (task 4.7.2, RS1)") {
    World live;
    RootOrder roots;
    buildScene(live, roots);
    World twin;
    RootOrder twinRoots;
    buildScene(twin, twinRoots);
    checkSameScene(live, twin);

    ParkedScene parked;
    CHECK_FALSE(parked.parked());
    CHECK(parked.park(live, roots));
    CHECK(parked.parked());
    checkSameScene(live, twin);  // the copy keeps every handle and every value

    Selection selection;
    CHECK(parked.restore(live, roots, selection));
    CHECK_FALSE(parked.parked());
    checkSameScene(live, twin);
    CHECK(rootsOf(roots) == rootsOf(twinRoots));
}

TEST_CASE(
    "parked scene: a play that changes everything leaves the restored scene equal to its twin (task 4.7.2, RS2)") {
    World live;
    RootOrder roots;
    const Named n = buildScene(live, roots);
    World twin;
    RootOrder twinRoots;
    buildScene(twin, twinRoots);

    ParkedScene parked;
    REQUIRE(parked.park(live, roots));

    // The play: every kind of change a game makes.
    live.get<engine::Transform>(n.c1)->position = engine::Vec3{9.0F, 8.0F, 7.0F};
    CHECK(live.setName(n.r1, "Renamed"));
    CHECK(live.setParent(n.c1, n.r2));
    live.add<engine::PointLight>(n.g, engine::PointLight{});
    live.remove<engine::MeshRenderer>(n.c1);
    const Entity s1 = createEntity(live, {}, "S1");  // takes the free slot
    const Entity s2 = createEntity(live, s1, "S2");
    CHECK(s1.index == n.tmp.index);
    CHECK(live.destroy(n.r1));
    CHECK(live.destroy(n.r2));
    const Entity recycled = createEntity(live, {}, "Recycled");
    CHECK(live.alive(s2));
    CHECK(live.alive(recycled));
    CHECK_FALSE(live.alive(n.c1));
    roots.reconcile(live);

    Selection selection;
    CHECK(parked.restore(live, roots, selection));
    checkSameScene(live, twin);
    CHECK(nextCreatesEqual(live, twin, 10));
}

TEST_CASE("parked scene: the root order comes back with the World, not reconciled afterwards (task 4.7.2, RS3)") {
    World live;
    RootOrder roots;
    const Named n = buildScene(live, roots);
    World twin;
    RootOrder twinRoots;
    buildScene(twin, twinRoots);
    const std::vector<Entity> expected = rootsOf(twinRoots);
    REQUIRE(rootsOf(roots) == expected);

    ParkedScene parked;
    REQUIRE(parked.park(live, roots));
    CHECK(live.destroy(n.r2));
    roots.reconcile(live);
    const RootOrder playRoots = roots;
    CHECK(rootsOf(playRoots) != expected);

    Selection selection;
    CHECK(parked.restore(live, roots, selection));
    CHECK(rootsOf(roots) == expected);

    // The control: the play-time order, reconciled against the restored World, puts R2 at the bottom.
    RootOrder reconciled = playRoots;
    reconciled.reconcile(live);
    CHECK(rootsOf(reconciled) != expected);
    REQUIRE(!expected.empty());
    CHECK(rootsOf(reconciled).back() == n.r2);
}

TEST_CASE(
    "parked scene: restore prunes the selection against both Worlds and never bumps the revision "
    "(task 4.7.2, RS4)") {
    World live;
    RootOrder roots;
    const Named n = buildScene(live, roots);
    const Entity a = n.r3;
    const Entity b = n.r2;
    const Entity d = n.g;

    ParkedScene parked;
    REQUIRE(parked.park(live, roots));
    const Entity p = createEntity(live, {}, "P");
    CHECK(live.destroy(b));
    CHECK(live.destroy(d));
    const Entity q = createEntity(live, {}, "Q");
    CHECK(q.index == d.index);  // the premise: Q reuses D's slot under a newer generation
    CHECK(q != d);

    Selection selection;
    selection.set(a);
    selection.add(b);
    selection.add(q);
    selection.add(p);
    CHECK(selection.primary() == p);
    const std::uint64_t revision = selection.revision();

    CHECK(parked.restore(live, roots, selection));
    CHECK(selection.revision() == revision);
    REQUIRE(selection.count() == 1);
    CHECK(selection.entities()[0] == a);
    CHECK(selection.primary() == a);
    CHECK(live.alive(b));
    CHECK(live.alive(d));
    CHECK_FALSE(live.alive(q));
    CHECK_FALSE(live.alive(p));
}

TEST_CASE("parked scene: park while parked and restore with nothing parked change nothing (task 4.7.2, RS5)") {
    World live;
    RootOrder roots;
    const Named n = buildScene(live, roots);
    World twin;
    RootOrder twinRoots;
    buildScene(twin, twinRoots);

    ParkedScene parked;
    Selection selection;
    selection.set(n.r1);
    const std::uint64_t revision = selection.revision();
    CHECK_FALSE(parked.restore(live, roots, selection));
    CHECK_FALSE(parked.parked());
    checkSameScene(live, twin);
    CHECK(rootsOf(roots) == rootsOf(twinRoots));
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == n.r1);
    CHECK(selection.revision() == revision);

    REQUIRE(parked.park(live, roots));
    const Entity spawn = createEntity(live, {}, "Spawn");
    const std::string before = world_test::dumpWorld(live);
    CHECK_FALSE(parked.park(live, roots));  // must not overwrite the parked original with the copy
    CHECK(live.alive(spawn));
    CHECK(world_test::dumpWorld(live) == before);

    CHECK(parked.restore(live, roots, selection));
    checkSameScene(live, twin);
    CHECK_FALSE(live.alive(spawn));
}

TEST_CASE("parked scene: fifty cycles leave the World equal to its twin and survive a move (task 4.7.2, RS6)") {
    World live;
    RootOrder roots;
    const Named n = buildScene(live, roots);
    World twin;
    RootOrder twinRoots;
    buildScene(twin, twinRoots);

    ParkedScene parked;
    Selection selection;
    int mismatches = 0;
    for (int cycle = 0; cycle < 50; ++cycle) {
        REQUIRE(parked.park(live, roots));
        for (int i = 0; i < 20; ++i) {
            const std::string name(64, static_cast<char>('a' + (i % 26)));
            const Entity spawned = createEntity(live, {}, name);
            live.add<engine::PointLight>(spawned, engine::PointLight{});
        }
        CHECK(live.destroy(n.r1));
        roots.reconcile(live);
        if (cycle == 25) {
            ParkedScene moved = std::move(parked);
            parked = ParkedScene{};
            CHECK(moved.restore(live, roots, selection));
        } else {
            CHECK(parked.restore(live, roots, selection));
        }
        if (world_test::dumpWorld(live) != world_test::dumpWorld(twin) || rootsOf(roots) != rootsOf(twinRoots)) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    CHECK_FALSE(parked.parked());
    checkSameScene(live, twin);
    CHECK(nextCreatesEqual(live, twin, 10));
}

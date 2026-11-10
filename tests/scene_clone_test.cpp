// tests/scene_clone_test.cpp -- task 4.7.2: World::clone, the identity-exact copy of a World (WC1-WC8). Tier-0
// throughout: no GPU, no files, no reflection, so it runs identically with AERO_REFLECT_TOOLS=OFF. The entity pool is
// read through WorldAccess::registry because identity is a property of the pool; every other check uses the public
// World API and the two oracles of world_dump.hpp.

#include <aero/core/log.hpp>
#include <aero/scene/internal/world_access.hpp>
#include <aero/scene/scene.hpp>

#include "world_dump.hpp"

#include <doctest/doctest.h>

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

using engine::ComponentTypeId;
using engine::componentTypeId;
using engine::Entity;
using engine::World;
using engine::scene::internal::SceneEntity;
using engine::scene::internal::WorldAccess;

namespace {

// A game's component and tag, registered through World::registerComponent<T>.
struct CloneGameComp {
    std::string label;
    std::vector<int> values;
    float weight = 0.0F;
};
struct CloneGameTag {};

// The same two, registered through the internal seam, which makes a typed pool.
struct CloneSeamComp {
    std::string note;
    int n = 0;
};
struct CloneSeamTag {};

// Two types that share one registered name, and one registered late.
struct SharedNameA {
    int a = 1;
};
struct SharedNameB {
    int b = 2;
};
struct LateComp {
    int late = 3;
};

constexpr std::uint32_t QUIET_NAN_BITS = 0x7FC00ABCU;

// Counts the WARN and ERROR records a case provokes, through the public setLogCallback seam.
class LogCount {
public:
    LogCount() {
        engine::setLogCallback([this](const engine::LogRecord& record) {
            if (record.level == engine::LogLevel::Warn) {
                ++warnings;
            } else if (record.level >= engine::LogLevel::Error) {
                ++errors;
            }
        });
    }
    LogCount(const LogCount&) = delete;
    LogCount& operator=(const LogCount&) = delete;
    LogCount(LogCount&&) = delete;
    LogCount& operator=(LogCount&&) = delete;
    ~LogCount() { engine::setLogCallback(nullptr); }

    [[nodiscard]] int warnCount() const noexcept { return warnings; }
    [[nodiscard]] int errorCount() const noexcept { return errors; }

private:
    int warnings = 0;
    int errors = 0;
};

// A scripted history that leaves holes and recycled slots in the entity pool. Returns every identifier it issued.
std::vector<Entity> buildHistory(World& w) {
    std::vector<Entity> issued;
    auto make = [&]() {
        const Entity e = w.create();
        issued.push_back(e);
        return e;
    };
    std::vector<Entity> first;
    first.reserve(12);
    for (int i = 0; i < 12; ++i) {
        first.push_back(make());
    }
    CHECK(w.setParent(first[3], first[2]));
    CHECK(w.setParent(first[4], first[2]));
    CHECK(w.setParent(first[5], first[3]));
    CHECK(w.destroy(first[2]));  // a subtree: 2, 3, 4, 5
    CHECK(w.destroy(first[9]));  // a root
    static_cast<void>(make());   // recycling creates
    static_cast<void>(make());
    const Entity gone = first[7];
    CHECK(w.destroy(gone));
    CHECK(w.recreate(gone) == gone);  // the same identity back
    Entity slot = make();
    for (int cycle = 0; cycle < 5; ++cycle) {  // five destroy / create cycles on one slot
        CHECK(w.destroy(slot));
        slot = make();
    }
    w.clear();
    std::vector<Entity> second;
    second.reserve(5);
    for (int i = 0; i < 5; ++i) {
        second.push_back(make());
    }
    CHECK(w.destroy(second[1]));
    CHECK(w.destroy(second[3]));
    static_cast<void>(make());
    static_cast<void>(make());
    CHECK(w.destroy(second[0]));
    return issued;
}

// A cheap deterministic mix, so a subset is scrambled without a random engine.
bool wants(std::uint32_t entity, std::uint32_t type) {
    std::uint32_t h = (entity + 1U) * 2654435761U ^ (type + 1U) * 40503U;
    h ^= h >> 15U;
    h *= 2246822519U;
    h ^= h >> 13U;
    return (h & 1U) != 0U;
}

// All eleven built-ins on scrambled subsets of `entities`, with values that differ per entity.
void addBuiltins(World& w, const std::vector<Entity>& entities) {
    for (std::uint32_t i = 0; i < entities.size(); ++i) {
        const Entity e = entities[i];
        const auto f = static_cast<float>(i);
        if (wants(i, 0)) {
            w.add<engine::Transform>(e,
                                     engine::Transform{{f, f * 2.0F, -f}, {0.0F, 0.0F, 0.0F, 1.0F}, {1.0F, f, 2.0F}});
        }
        if (wants(i, 1)) {
            engine::Camera camera;
            camera.nearPlane = 0.1F + f;
            w.add<engine::Camera>(e, camera);
        }
        if (wants(i, 2)) {
            engine::DirectionalLight light;
            light.intensity = f;
            w.add<engine::DirectionalLight>(e, light);
        }
        if (wants(i, 3)) {
            engine::PointLight light;
            light.range = f + 1.0F;
            w.add<engine::PointLight>(e, light);
        }
        if (wants(i, 4)) {
            engine::MeshRenderer mesh;
            mesh.meshIndex = i;
            w.add<engine::MeshRenderer>(e, mesh);
        }
        if (wants(i, 5)) {
            engine::AnimationPlayer player;
            player.time = f;
            w.add<engine::AnimationPlayer>(e, player);
        }
        if (wants(i, 6)) {
            engine::AudioSource source;
            source.pitch = 1.0F + f;
            w.add<engine::AudioSource>(e, source);
        }
        if (wants(i, 7)) {
            engine::AudioListener listener;
            listener.volume = f;
            w.add<engine::AudioListener>(e, listener);
        }
        if (wants(i, 8)) {
            engine::Environment environment;
            environment.ambientIntensity = f;
            w.add<engine::Environment>(e, environment);
        }
        if (wants(i, 9)) {
            engine::SpotLight light;
            light.range = f + 2.0F;
            w.add<engine::SpotLight>(e, light);
        }
        if (wants(i, 10)) {
            engine::ScriptComponent script;
            script.script = engine::Guid{i + 1ULL, 7ULL};
            w.add<engine::ScriptComponent>(e, script);
        }
    }
}

template <typename T>
std::vector<Entity> eachSequence(World& w) {
    std::vector<Entity> out;
    w.each<T>([&](Entity e, T&) { out.push_back(e); });
    return out;
}

std::vector<Entity> liveEntities(const World& w) {
    std::vector<Entity> out;
    w.eachEntity([&](Entity e) { out.push_back(e); });
    return out;
}

std::vector<Entity> childrenOf(const World& w, Entity parent) {
    std::vector<Entity> out;
    w.eachChild(parent, [&](Entity child) { out.push_back(child); });
    return out;
}

}  // namespace

TEST_CASE("scene: clone reproduces the entity pool, free slots and versions included (task 4.7.2, WC1)") {
    World source;
    const std::vector<Entity> issued = buildHistory(source);
    World copy = source.clone();

    auto& sourcePool = WorldAccess::registry(source)->storage<SceneEntity>();
    auto& copyPool = WorldAccess::registry(copy)->storage<SceneEntity>();
    REQUIRE(copyPool.size() == sourcePool.size());
    CHECK(copyPool.free_list() == sourcePool.free_list());
    for (std::size_t i = 0; i < sourcePool.size(); ++i) {
        CHECK(static_cast<std::uint64_t>(copyPool.data()[i]) == static_cast<std::uint64_t>(sourcePool.data()[i]));
    }
    for (const Entity e : issued) {
        CHECK(copy.alive(e) == source.alive(e));
    }
    CHECK(copy.entityCount() == source.entityCount());
    CHECK(liveEntities(copy) == liveEntities(source));
}

TEST_CASE("scene: clone mints and recreates the identities the source would (task 4.7.2, WC2)") {
    World source;
    const std::vector<Entity> issued = buildHistory(source);
    World copy = source.clone();

    for (int i = 0; i < 40; ++i) {
        const Entity a = source.create();
        const Entity b = copy.create();
        CHECK(a == b);
        if (i % 3 == 0) {
            CHECK(source.destroy(a));
            CHECK(copy.destroy(b));
        }
        if (i == 19) {
            source.clear();
            copy.clear();
        }
    }

    // Both Worlds hold the same live set now; destroy one entity on each and bring the identity back.
    const std::vector<Entity> live = liveEntities(source);
    REQUIRE_FALSE(live.empty());
    const Entity victim = live.back();
    CHECK(source.destroy(victim));
    CHECK(copy.destroy(victim));
    CHECK(source.recreate(victim) == victim);
    CHECK(copy.recreate(victim) == victim);

    // An index past the pool's size is refused on both, loudly.
    const LogCount log;
    const Entity never{static_cast<std::uint32_t>(issued.size() + 1000U), 1U};
    CHECK_FALSE(source.recreate(never).valid());
    CHECK_FALSE(copy.recreate(never).valid());
    CHECK(log.errorCount() == 2);

    // A fresh World's clone mints {0, 1} first.
    const World fresh;
    World fromFresh = fresh.clone();
    CHECK(fromFresh.create() == Entity{0U, 1U});
}

TEST_CASE("scene: clone copies the eleven built-in pools in packed order (task 4.7.2, WC3)") {
    World source;
    std::vector<Entity> entities;
    entities.reserve(60);
    for (int i = 0; i < 60; ++i) {
        entities.push_back(source.create());
    }
    addBuiltins(source, entities);
    // Removes reorder each pool (swap-and-pop), so packed order is no longer creation order.
    for (std::uint32_t i = 0; i < entities.size(); i += 3U) {
        source.remove<engine::Transform>(entities[i]);
        source.remove<engine::MeshRenderer>(entities[i]);
        source.remove<engine::Camera>(entities[i]);
        source.remove<engine::ScriptComponent>(entities[i]);
    }
    World copy = source.clone();

    CHECK(eachSequence<engine::Transform>(copy) == eachSequence<engine::Transform>(source));
    CHECK(eachSequence<engine::Camera>(copy) == eachSequence<engine::Camera>(source));
    CHECK(eachSequence<engine::DirectionalLight>(copy) == eachSequence<engine::DirectionalLight>(source));
    CHECK(eachSequence<engine::PointLight>(copy) == eachSequence<engine::PointLight>(source));
    CHECK(eachSequence<engine::MeshRenderer>(copy) == eachSequence<engine::MeshRenderer>(source));
    CHECK(eachSequence<engine::AnimationPlayer>(copy) == eachSequence<engine::AnimationPlayer>(source));
    CHECK(eachSequence<engine::AudioSource>(copy) == eachSequence<engine::AudioSource>(source));
    CHECK(eachSequence<engine::AudioListener>(copy) == eachSequence<engine::AudioListener>(source));
    CHECK(eachSequence<engine::Environment>(copy) == eachSequence<engine::Environment>(source));
    CHECK(eachSequence<engine::SpotLight>(copy) == eachSequence<engine::SpotLight>(source));
    CHECK(eachSequence<engine::ScriptComponent>(copy) == eachSequence<engine::ScriptComponent>(source));
    CHECK(copy.componentCount<engine::Transform>() == source.componentCount<engine::Transform>());
    CHECK(copy.componentCount<engine::SpotLight>() == source.componentCount<engine::SpotLight>());
    CHECK(world_test::firstBuiltinDifference(source, copy).empty());
    CHECK(world_test::dumpWorld(copy) == world_test::dumpWorld(source));

    const Entity probe = entities[1];
    REQUIRE(source.get<engine::Transform>(probe) != nullptr);
    const engine::Transform before = *source.get<engine::Transform>(probe);
    copy.get<engine::Transform>(probe)->position.x += 100.0F;
    CHECK(*source.get<engine::Transform>(probe) == before);
}

TEST_CASE("scene: clone copies game and seam pools, tags and non-trivial members (task 4.7.2, WC4)") {
    std::optional<World> source;
    source.emplace();
    World& s = *source;
    const ComponentTypeId gameId = s.registerComponent<CloneGameComp>("CloneGameComp");
    const ComponentTypeId gameTagId = s.registerComponent<CloneGameTag>("CloneGameTag");
    const ComponentTypeId seamId = engine::scene::internal::registerComponent<CloneSeamComp>(s, "CloneSeamComp");
    const ComponentTypeId seamTagId = engine::scene::internal::registerComponent<CloneSeamTag>(s, "CloneSeamTag");
    REQUIRE(gameId.valid());
    REQUIRE(gameTagId.valid());
    REQUIRE(seamId.valid());
    REQUIRE(seamTagId.valid());

    std::vector<Entity> entities;
    entities.reserve(6);
    for (int i = 0; i < 6; ++i) {
        entities.push_back(s.create());
    }
    const std::string longLabel(120, 'x');  // past any small-string buffer
    for (std::size_t i = 0; i < entities.size(); ++i) {
        CloneGameComp game;
        game.label = longLabel + std::to_string(i);
        game.values = {static_cast<int>(i), 2, 3, 4, 5, 6, 7, 8, 9, 10};
        game.weight = (i == 2) ? std::bit_cast<float>(QUIET_NAN_BITS) : static_cast<float>(i);
        s.add<CloneGameComp>(entities[i], game);
        s.add<CloneSeamComp>(entities[i], CloneSeamComp{longLabel + "seam" + std::to_string(i), static_cast<int>(i)});
        if (i % 2 == 0) {
            s.add<CloneGameTag>(entities[i]);
            s.add<CloneSeamTag>(entities[i]);
        }
    }

    World copy = s.clone();
    for (const Entity e : entities) {
        const CloneGameComp* from = s.get<CloneGameComp>(e);
        const CloneGameComp* to = copy.get<CloneGameComp>(e);
        REQUIRE(from != nullptr);
        REQUIRE(to != nullptr);
        CHECK(to->label == from->label);
        CHECK(to->values == from->values);
        CHECK(to->label.data() != from->label.data());
        CHECK(to->values.data() != from->values.data());
        CHECK(std::bit_cast<std::uint32_t>(to->weight) == std::bit_cast<std::uint32_t>(from->weight));
        const CloneSeamComp* seamFrom = s.get<CloneSeamComp>(e);
        const CloneSeamComp* seamTo = copy.get<CloneSeamComp>(e);
        REQUIRE(seamTo != nullptr);
        CHECK(seamTo->note == seamFrom->note);
        CHECK(seamTo->n == seamFrom->n);
        CHECK(copy.has<CloneGameTag>(e) == s.has<CloneGameTag>(e));
        CHECK(copy.has<CloneSeamTag>(e) == s.has<CloneSeamTag>(e));
    }
    CHECK(std::bit_cast<std::uint32_t>(copy.get<CloneGameComp>(entities[2])->weight) == QUIET_NAN_BITS);

    // The clone's pools take further work.
    const Entity extra = copy.create();
    CloneGameComp more;
    more.label = "more";
    CHECK(copy.add<CloneGameComp>(extra, more) != nullptr);
    CHECK(copy.copyComponent(gameId, entities[0], extra));
    CHECK(copy.get<CloneGameComp>(extra)->label == longLabel + "0");
    CHECK(copy.copyComponent(seamId, entities[1], extra));
    CHECK(copy.get<CloneSeamComp>(extra)->n == 1);
    copy.add<CloneGameTag>(extra);
    CHECK(copy.has<CloneGameTag>(extra));
    CHECK(copy.componentCount<CloneGameComp>() == entities.size() + 1U);

    // The source is destroyed; the clone owns its memory.
    source.reset();
    CHECK(copy.get<CloneGameComp>(entities[5])->label == longLabel + "5");
    CHECK(copy.get<CloneSeamComp>(entities[5])->note == longLabel + "seam5");
}

TEST_CASE("scene: clone replays the registration table without a log record (task 4.7.2, WC5)") {
    World source;
    {
        const LogCount registrationLog;
        const ComponentTypeId a = source.registerComponent<SharedNameA>("shared");
        const ComponentTypeId b = source.registerComponent<SharedNameB>("shared");
        CHECK(a.valid());
        CHECK(b.valid());
        CHECK(registrationLog.warnCount() == 1);
    }
    World copy;
    {
        const LogCount log;
        copy = source.clone();
        CHECK(log.warnCount() == 0);
        CHECK(log.errorCount() == 0);
    }
    REQUIRE(copy.componentTypeCount() == source.componentTypeCount());
    for (std::size_t i = 0; i < source.componentTypeCount(); ++i) {
        const ComponentTypeId id = source.componentTypeAt(i);
        CHECK(copy.componentTypeAt(i) == id);
        CHECK(copy.componentTypeName(id) == source.componentTypeName(id));
        CHECK(copy.findComponentType(source.componentTypeName(id)) ==
              source.findComponentType(source.componentTypeName(id)));
    }
    CHECK(copy.findComponentType("shared") == componentTypeId<SharedNameA>());

    const std::size_t before = source.componentTypeCount();
    CHECK(copy.registerComponent<LateComp>("LateComp").valid());
    CHECK(copy.componentTypeCount() == before + 1U);
    CHECK(source.componentTypeCount() == before);
    const Entity e = copy.create();
    CHECK(copy.add<LateComp>(e) != nullptr);
}

TEST_CASE("scene: clone copies the hierarchy and the names and shares nothing (task 4.7.2, WC6)") {
    std::optional<World> source;
    source.emplace();
    World& s = *source;
    std::vector<Entity> e;
    e.reserve(9);
    for (int i = 0; i < 9; ++i) {
        e.push_back(s.create());
    }
    // Attach order differs from creation order.
    CHECK(s.setParent(e[6], e[1]));
    CHECK(s.setParent(e[3], e[1]));
    CHECK(s.setParent(e[8], e[1]));
    CHECK(s.setParent(e[2], e[6]));
    CHECK(s.setParent(e[7], e[2]));
    s.setName(e[1], "forest root with a name longer than any small-string buffer allows");
    s.setName(e[3], "three");
    s.setName(e[7], "seven");

    World copy = s.clone();
    for (const Entity entity : e) {
        CHECK(copy.parent(entity) == s.parent(entity));
        CHECK(copy.childCount(entity) == s.childCount(entity));
        CHECK(childrenOf(copy, entity) == childrenOf(s, entity));
        CHECK(copy.name(entity) == s.name(entity));
    }

    const std::string sourceBefore = world_test::dumpWorld(s);
    const std::string copyBefore = world_test::dumpWorld(copy);
    CHECK(copy.setParent(e[3], Entity{}));
    CHECK(copy.setName(e[1], ""));
    CHECK(copy.destroy(e[6]));
    CHECK(world_test::dumpWorld(s) == sourceBefore);
    CHECK(world_test::dumpWorld(copy) != copyBefore);

    const World second = s.clone();
    const std::string secondBefore = world_test::dumpWorld(second);
    CHECK(s.setParent(e[8], Entity{}));
    CHECK(s.setName(e[3], "renamed"));
    CHECK(s.destroy(e[1]));
    CHECK(world_test::dumpWorld(second) == secondBefore);

    source.reset();  // the clone stands alone
    CHECK(world_test::dumpWorld(second) == secondBefore);
}

TEST_CASE("scene: clone of a moved-from World is a moved-from World and one ERROR (task 4.7.2, WC7)") {
    std::optional<World> source;
    source.emplace();
    const World moved = std::move(*source);
    CHECK(moved.entityCount() == 0U);

    const LogCount log;
    World copy = source->clone();
    CHECK(log.errorCount() == 1);
    CHECK(copy.entityCount() == 0U);
    CHECK(copy.componentTypeCount() == 0U);
    CHECK(copy.create() == Entity{});  // a later call stays loud
}

TEST_CASE("scene: clone copies ten thousand entities across run-time pool pages (task 4.7.2, WC8)") {
    World source;
    static_cast<void>(source.registerComponent<CloneGameComp>("CloneGameComp"));
    constexpr std::size_t COUNT = 10000;
    std::vector<Entity> entities;
    entities.reserve(COUNT);
    for (std::size_t i = 0; i < COUNT; ++i) {
        const Entity e = source.create();
        entities.push_back(e);
        source.add<engine::Transform>(e,
                                      engine::Transform{{static_cast<float>(i), 0.0F, 0.0F}, {}, {1.0F, 1.0F, 1.0F}});
        if (i % 2 == 0) {
            source.add<engine::MeshRenderer>(e, engine::MeshRenderer{});
        }
        if (i % 4 == 0) {
            source.add<engine::ScriptComponent>(e, engine::ScriptComponent{engine::Guid{i + 1U, 1U}});
        }
        source.setName(e, "entity " + std::to_string(i));
        CloneGameComp game;
        game.label = "label " + std::to_string(i);
        game.values = {static_cast<int>(i)};
        game.weight = static_cast<float>(i);
        source.add<CloneGameComp>(e, game);
    }
    for (std::size_t i = 1; i < COUNT; ++i) {
        CHECK(source.setParent(entities[i], entities[(i - 1U) / 4U]));  // a 4-ary forest
    }
    for (std::size_t i = 9; i < COUNT; i += 10U) {
        static_cast<void>(source.destroy(entities[i]));  // a tenth, subtrees with them
    }

    const auto start = std::chrono::steady_clock::now();
    World copy = source.clone();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
    MESSAGE("World::clone of ", source.entityCount(), " entities: ", elapsed.count(), " us");

    CHECK(copy.entityCount() == source.entityCount());
    CHECK(world_test::dumpWorld(copy) == world_test::dumpWorld(source));
    CHECK(world_test::firstBuiltinDifference(source, copy).empty());
    CHECK(copy.componentCount<CloneGameComp>() == source.componentCount<CloneGameComp>());
    const std::vector<Entity> live = liveEntities(source);
    REQUIRE_FALSE(live.empty());
    CHECK(copy.get<CloneGameComp>(live.back())->label == source.get<CloneGameComp>(live.back())->label);
}

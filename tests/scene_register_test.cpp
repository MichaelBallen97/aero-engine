// tests/scene_register_test.cpp -- task 4.5.1: World::registerComponent<T>, a game's own component type registered
// through public headers alone. Tier-0 throughout: no GPU, no files, no reflection, so it runs identically with
// AERO_REFLECT_TOOLS=OFF. RG14 reaches the pool through WorldAccess::registry because it checks the one storage
// override no production path calls; every other case uses the public World API only.

#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/scene/internal/world_access.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using engine::ComponentTypeId;
using engine::componentTypeId;
using engine::Entity;
using engine::Prefab;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::scene::internal::ErasedStorage;
using engine::scene::internal::SceneEntity;

namespace {

struct Velocity {
    Vec3 value;
    bool operator==(const Velocity&) const = default;
};

struct GameTag {};
struct OtherTag {};

struct Named {
    std::string text = "default";
};

// Counts live instances: a construct/destroy imbalance on any path fails the case that uses it.
struct Counted {
    inline static int live = 0;
    std::string text;
    std::uint32_t tagValue = 0;

    Counted() { ++live; }
    Counted(const Counted& other) : text(other.text), tagValue(other.tagValue) { ++live; }
    Counted(Counted&& other) noexcept : text(std::move(other.text)), tagValue(other.tagValue) { ++live; }
    Counted& operator=(const Counted&) = default;
    Counted& operator=(Counted&&) noexcept = default;
    ~Counted() { --live; }
};

// A type whose copy can be made to throw, to reach the storage's rollback.
struct Fragile {
    inline static bool armed = false;
    inline static int live = 0;
    int value = 0;

    Fragile() { ++live; }
    explicit Fragile(int v) : value(v) { ++live; }
    Fragile(const Fragile& other) : value(other.value) {
        if (armed) {
            throw std::runtime_error("copy refused");
        }
        ++live;
    }
    Fragile(Fragile&& other) noexcept : value(other.value) { ++live; }
    Fragile& operator=(const Fragile&) = default;
    Fragile& operator=(Fragile&&) noexcept = default;
    ~Fragile() { --live; }
};

// Two types that share one registered name.
struct SameNameA {
    int a = 1;
};
struct SameNameB {
    int b = 2;
};

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

const std::string longText(120, 'x');  // past any small-string buffer

}  // namespace

TEST_CASE("scene register: add, get, has, remove and count through the public registration (RG1)") {
    World w;
    const std::size_t builtins = w.componentTypeCount();
    const ComponentTypeId id = w.registerComponent<Velocity>("game::Velocity");
    CHECK(id == componentTypeId<Velocity>());
    CHECK(w.registered(id));
    CHECK(w.componentTypeCount() == builtins + 1U);
    CHECK(w.findComponentType("game::Velocity") == id);
    CHECK(w.componentTypeName(id) == "game::Velocity");
    CHECK(w.componentTypeAt(builtins) == id);

    const Entity e = w.create();
    CHECK_FALSE(w.has<Velocity>(e));
    auto* v = w.add<Velocity>(e, Velocity{Vec3{1.0F, 2.0F, 3.0F}});
    REQUIRE(v != nullptr);
    CHECK(v->value.y == 2.0F);
    CHECK(w.has<Velocity>(e));
    CHECK(w.get<Velocity>(e) == v);
    CHECK(w.componentCount<Velocity>() == 1U);
    CHECK(w.remove<Velocity>(e));
    CHECK_FALSE(w.remove<Velocity>(e));
    CHECK(w.get<Velocity>(e) == nullptr);
    CHECK(w.componentCount<Velocity>() == 0U);
}

TEST_CASE("scene register: each walks mixed storages over several pages, against a model (RG2)") {
    const int before = Counted::live;
    {
        World w;
        w.registerComponent<Counted>("game::Counted");
        w.registerComponent<Velocity>("game::Velocity");
        std::unordered_map<std::uint32_t, std::uint32_t> model;
        std::vector<Entity> entities;
        for (std::uint32_t i = 0; i < 3000U; ++i) {
            const Entity e = w.create();
            w.add<Transform>(e);
            Counted c;
            c.tagValue = i;
            c.text = std::to_string(i) + longText;
            w.add<Counted>(e, c);
            if (i % 2U == 0U) {
                w.add<Velocity>(e, Velocity{Vec3{static_cast<float>(i), 0.0F, 0.0F}});
            }
            entities.push_back(e);
            model[e.index] = i;
        }
        std::mt19937 rng(7);
        std::ranges::shuffle(entities, rng);
        for (std::size_t i = 0; i < 1800U; ++i) {
            if (i % 3U == 0U) {
                w.remove<Counted>(entities[i]);
                if (!w.alive(entities[i])) {
                    FAIL("a live entity was reported dead");
                }
            } else {
                w.destroy(entities[i]);
            }
            model.erase(entities[i].index);
        }
        // An entity that kept Transform but lost Counted is not in the model, so each<Transform, Counted> skips it.
        std::size_t seen = 0;
        w.each<Transform, Counted>([&](Entity e, Transform&, Counted& c) {
            ++seen;
            REQUIRE(model.contains(e.index));
            CHECK(c.tagValue == model[e.index]);
            CHECK(c.text == std::to_string(c.tagValue) + longText);
        });
        CHECK(seen == model.size());
        CHECK(w.componentCount<Counted>() == model.size());
        std::size_t both = 0;
        w.each<Counted, Velocity>([&](Entity, Counted& c, Velocity& v) {
            ++both;
            CHECK(static_cast<std::uint32_t>(v.value.x) == c.tagValue);
        });
        CHECK(both > 0U);
    }
    CHECK(Counted::live == before);
}

TEST_CASE("scene register: destroy runs destructors, a subtree included, and a recycled slot holds nothing (RG3)") {
    const int before = Counted::live;
    {
        World w;
        w.registerComponent<Counted>("game::Counted");
        const Entity parent = w.create();
        const Entity child = w.create();
        w.setParent(child, parent);
        w.add<Counted>(parent);
        w.add<Counted>(child);
        CHECK(Counted::live == before + 2);
        CHECK(w.destroy(parent));
        CHECK(Counted::live == before);
        CHECK(w.componentCount<Counted>() == 0U);
        const Entity next = w.create();
        CHECK(next.index == parent.index);  // recycled
        CHECK_FALSE(w.has<Counted>(next));
        CHECK(w.get<Counted>(next) == nullptr);
    }
    CHECK(Counted::live == before);
}

TEST_CASE("scene register: clear empties the storage, runs destructors and keeps the registration (RG4)") {
    const int before = Counted::live;
    {
        World w;
        w.registerComponent<Counted>("game::Counted");
        w.registerComponent<GameTag>("game::GameTag");
        for (int i = 0; i < 2500; ++i) {
            const Entity e = w.create();
            w.add<Counted>(e);
            w.add<GameTag>(e);
        }
        CHECK(Counted::live == before + 2500);
        w.clear();
        CHECK(Counted::live == before);
        CHECK(w.componentCount<Counted>() == 0U);
        CHECK(w.componentCount<GameTag>() == 0U);
        CHECK(w.registered(componentTypeId<Counted>()));
        const Entity e = w.create();
        CHECK(w.add<Counted>(e) != nullptr);
        w.add<GameTag>(e);
        CHECK(w.has<GameTag>(e));
        CHECK(Counted::live == before + 1);
    }
    CHECK(Counted::live == before);
}

TEST_CASE("scene register: a moved World keeps data and registration, a moved-from World rejects (RG5)") {
    const int before = Counted::live;
    {
        World a;
        a.registerComponent<Counted>("game::Counted");
        const Entity e = a.create();
        Counted c;
        c.text = longText;
        a.add<Counted>(e, c);
        const Counted* address = a.get<Counted>(e);
        World b{std::move(a)};
        CHECK(b.get<Counted>(e) == address);
        CHECK(b.get<Counted>(e)->text == longText);

        {
            const LogCount log;
            // NOLINTBEGIN(bugprone-use-after-move)
            CHECK_FALSE(a.registerComponent<Velocity>("game::Velocity").valid());
            CHECK(log.warnCount() == 0);
            CHECK(log.errorCount() == 0);
            CHECK_FALSE(a.registered(componentTypeId<Counted>()));
            CHECK(a.componentCount<Counted>() == 0U);
            CHECK(a.get<Counted>(e) == nullptr);
            // NOLINTEND(bugprone-use-after-move)
        }

        World other;
        other.registerComponent<Counted>("game::Counted");
        other.add<Counted>(other.create());
        other = std::move(b);  // the old storage dies here
        CHECK(other.get<Counted>(e)->text == longText);
        CHECK(other.componentCount<Counted>() == 1U);
    }
    CHECK(Counted::live == before);
}

TEST_CASE("scene register: a string past SSO survives a replace, a swap-and-pop and a page crossing (RG6)") {
    const int before = Counted::live;
    {
        World w;
        w.registerComponent<Named>("game::Named");
        w.registerComponent<Counted>("game::Counted");

        const Entity a = w.create();
        CHECK(w.add<Named>(a)->text == "default");
        w.add<Named>(a, Named{longText});  // a replace: erase then insert
        CHECK(w.get<Named>(a)->text == longText);
        CHECK(w.componentCount<Named>() == 1U);

        // 2100 elements cross two page boundaries; every one carries a distinct long string.
        std::vector<Entity> entities;
        for (std::uint32_t i = 0; i < 2100U; ++i) {
            const Entity e = w.create();
            Counted c;
            c.tagValue = i;
            c.text = longText + std::to_string(i);
            w.add<Counted>(e, c);
            entities.push_back(e);
        }
        // Remove from the front: each removal swap-and-pops the back element into the hole.
        for (std::size_t i = 0; i < 500U; ++i) {
            CHECK(w.remove<Counted>(entities[i]));
        }
        for (std::size_t i = 500U; i < entities.size(); ++i) {
            const Counted* c = w.get<Counted>(entities[i]);
            REQUIRE(c != nullptr);
            CHECK(c->tagValue == i);
            CHECK(c->text == longText + std::to_string(i));
        }
        CHECK(w.componentCount<Counted>() == 1600U);
    }
    CHECK(Counted::live == before);
}

TEST_CASE("scene register: copyComponent across a relocation and across a new page (RG7)") {
    const int before = Counted::live;
    {
        World w;
        w.registerComponent<Counted>("game::Counted");
        const Entity src = w.create();
        const Entity dst = w.create();
        Counted one;
        one.text = longText + "src";
        // The destination holds the type first, so removing it relocates the source.
        w.add<Counted>(dst);
        w.add<Counted>(src, one);
        CHECK(w.copyComponent(componentTypeId<Counted>(), src, dst));
        CHECK(w.get<Counted>(dst)->text == one.text);
        CHECK(w.get<Counted>(src)->text == one.text);

        // A push that opens a new page while its source sits on the page before.
        for (int i = 0; i < 1022; ++i) {
            w.add<Counted>(w.create());
        }
        CHECK(w.componentCount<Counted>() == 1024U);
        const Entity fresh = w.create();
        CHECK(w.copyComponent(componentTypeId<Counted>(), src, fresh));
        CHECK(w.componentCount<Counted>() == 1025U);
        CHECK(w.get<Counted>(fresh)->text == one.text);
        CHECK(w.copyComponent(componentTypeId<Counted>(), src, src));
        CHECK(w.get<Counted>(src)->text == one.text);
    }
    CHECK(Counted::live == before);
}

TEST_CASE("scene register: an empty T is a tag: presence, removal, copy, destroy (RG8)") {
    World w;
    w.registerComponent<GameTag>("game::GameTag");
    const Entity e = w.create();
    const Entity f = w.create();
    CHECK(w.add<GameTag>(e) == nullptr);
    CHECK(w.has<GameTag>(e));
    CHECK(w.addRaw(componentTypeId<GameTag>(), f, nullptr) == nullptr);
    CHECK(w.hasRaw(componentTypeId<GameTag>(), f));
    CHECK(w.remove<GameTag>(f));
    CHECK(w.getRaw(componentTypeId<GameTag>(), e) == nullptr);
    CHECK(w.componentCount<GameTag>() == 1U);
    CHECK(w.copyComponent(componentTypeId<GameTag>(), e, f));
    CHECK(w.has<GameTag>(f));
    CHECK(w.componentCount<GameTag>() == 2U);
    CHECK(w.remove<GameTag>(e));
    CHECK_FALSE(w.has<GameTag>(e));
    CHECK(w.destroy(f));
    CHECK(w.componentCount<GameTag>() == 0U);
    // Enough tags to cross a page's worth of positions: nothing is stored, nothing breaks.
    for (int i = 0; i < 2100; ++i) {
        w.add<GameTag>(w.create());
    }
    CHECK(w.componentCount<GameTag>() == 2100U);
}

TEST_CASE("scene register: an unregistered game type is rejected loudly until it is registered (RG9)") {
    World w;
    const Entity e = w.create();
    {
        const LogCount log;
        CHECK(w.add<Velocity>(e) == nullptr);
        CHECK(log.errorCount() == 1);
        int calls = 0;
        w.each<Velocity>([&](Entity, Velocity&) { ++calls; });
        CHECK(calls == 0);
        CHECK_FALSE(w.has<Velocity>(e));
        CHECK(w.get<Velocity>(e) == nullptr);
        CHECK(w.componentCount<Velocity>() == 0U);
    }
    CHECK_FALSE(w.registered(componentTypeId<Velocity>()));  // add did not register on the side

    w.registerComponent<Velocity>("game::Velocity");
    REQUIRE(w.add<Velocity>(e, Velocity{Vec3{4.0F, 0.0F, 0.0F}}) != nullptr);
    int calls = 0;
    w.each<Velocity>([&](Entity, Velocity& v) {
        ++calls;
        CHECK(v.value.x == 4.0F);
    });
    CHECK(calls == 1);
}

TEST_CASE("scene register: a second call returns the same id and keeps the elements (RG10)") {
    World w;
    const ComponentTypeId first = w.registerComponent<Velocity>("game::Velocity");
    const std::size_t count = w.componentTypeCount();
    const Entity e = w.create();
    w.add<Velocity>(e, Velocity{Vec3{1.0F, 0.0F, 0.0F}});

    {
        const LogCount log;
        CHECK(w.registerComponent<Velocity>("game::Velocity") == first);  // the same name: silent
        CHECK(log.warnCount() == 0);
        CHECK(log.errorCount() == 0);
    }
    {
        const LogCount log;
        CHECK(w.registerComponent<Velocity>("another::Name") == first);  // a different name: one WARN
        CHECK(log.warnCount() == 1);
        CHECK(log.errorCount() == 0);
    }
    CHECK(w.componentTypeName(first) == "game::Velocity");
    CHECK(w.findComponentType("another::Name") == ComponentTypeId{});
    CHECK(w.componentTypeCount() == count);
    CHECK(w.componentCount<Velocity>() == 1U);
    CHECK(w.get<Velocity>(e)->value.x == 1.0F);  // the storage was not touched
}

TEST_CASE("scene register: one name on two types warns once and both store independently (RG11)") {
    World w;
    const ComponentTypeId a = w.registerComponent<SameNameA>("game::Shared");
    const LogCount log;
    const ComponentTypeId b = w.registerComponent<SameNameB>("game::Shared");
    CHECK(log.warnCount() == 1);
    CHECK(a != b);
    CHECK(w.findComponentType("game::Shared") == a);  // the first registrant wins
    const Entity e = w.create();
    w.add<SameNameA>(e, SameNameA{10});
    w.add<SameNameB>(e, SameNameB{20});
    CHECK(w.get<SameNameA>(e)->a == 10);
    CHECK(w.get<SameNameB>(e)->b == 20);
    CHECK(w.remove<SameNameA>(e));
    CHECK(w.get<SameNameB>(e)->b == 20);
    CHECK(w.componentCount<SameNameA>() == 0U);
    CHECK(w.componentCount<SameNameB>() == 1U);
}

TEST_CASE("scene register: a capture skips a game type with one WARN per type and a spawn-site add works (RG12)") {
    const int before = Counted::live;
    {
        World source;
        source.registerComponent<Velocity>("game::Velocity");
        source.registerComponent<Counted>("game::Counted");
        const Entity root = source.create();
        source.add<Transform>(root, Transform{.position = {5.0F, 0.0F, 0.0F}});
        source.add<Velocity>(root, Velocity{Vec3{0.0F, 0.0F, -12.0F}});
        source.add<Counted>(root);
        const Entity child = source.create();
        source.setParent(child, root);
        source.add<Counted>(child);

        const std::array roots{root};
        std::optional<Prefab> prefab;
        {
            const LogCount log;
            prefab = Prefab::capture(source, std::span<const Entity>{roots}, "");
            REQUIRE(prefab.has_value());
            CHECK(log.warnCount() == 2);  // one per skipped type: Velocity and Counted
            CHECK(log.errorCount() == 0);
        }
        CHECK(prefab->world().componentCount<Counted>() == 0U);
        CHECK_FALSE(prefab->world().registered(componentTypeId<Velocity>()));

        World dest;
        dest.registerComponent<Velocity>("game::Velocity");
        const Entity copy = prefab->instantiate(dest, Transform{});
        REQUIRE(copy.valid());
        CHECK(dest.has<Transform>(copy));
        CHECK_FALSE(dest.has<Velocity>(copy));                                           // the instance lacks it
        REQUIRE(dest.add<Velocity>(copy, Velocity{Vec3{1.0F, 0.0F, 0.0F}}) != nullptr);  // a spawn-site add
        CHECK(dest.get<Velocity>(copy)->value.x == 1.0F);
    }
    CHECK(Counted::live == before);
}

TEST_CASE("scene register: registering in one World does not register in another (RG13)") {
    World a;
    World b;
    const ComponentTypeId id = a.registerComponent<Velocity>("game::Velocity");
    CHECK_FALSE(b.registered(id));
    CHECK(b.findComponentType("game::Velocity") == ComponentTypeId{});
    const ComponentTypeId again = b.registerComponent<Velocity>("game::Velocity");
    CHECK(again == id);  // the id is per program image, the data is per World
    a.add<Velocity>(a.create(), Velocity{Vec3{1.0F, 0.0F, 0.0F}});
    b.add<Velocity>(b.create(), Velocity{Vec3{9.0F, 0.0F, 0.0F}});
    b.add<Velocity>(b.create(), Velocity{Vec3{8.0F, 0.0F, 0.0F}});
    CHECK(a.componentCount<Velocity>() == 1U);
    CHECK(b.componentCount<Velocity>() == 2U);
}

TEST_CASE("scene register: swap_elements on the pool keeps each entity's value for an owning T (RG14)") {
    const int before = Counted::live;
    {
        World w;
        w.registerComponent<Counted>("game::Counted");
        std::vector<Entity> entities;
        for (std::uint32_t i = 0; i < 6U; ++i) {
            const Entity e = w.create();
            Counted c;
            c.tagValue = i;
            c.text = longText + std::to_string(i);
            w.add<Counted>(e, c);
            entities.push_back(e);
        }
        engine::scene::internal::Registry* registry = engine::scene::internal::WorldAccess::registry(w);
        REQUIRE(registry != nullptr);
        // The pool id is an implementation detail: find the pool by what it holds. Nothing else is populated, so the
        // one non-entity pool that contains the first entity is Counted's.
        const SceneEntity first = engine::scene::internal::toEntt(entities[0]);
        ErasedStorage* pool = nullptr;
        for (auto&& entry : registry->storage()) {
            if (entry.second.type() != entt::type_id<SceneEntity>() && entry.second.contains(first)) {
                pool = &entry.second;
            }
        }
        REQUIRE(pool != nullptr);
        CHECK(pool->size() == 6U);
        pool->swap_elements(engine::scene::internal::toEntt(entities[1]), engine::scene::internal::toEntt(entities[4]));
        pool->swap_elements(engine::scene::internal::toEntt(entities[0]), engine::scene::internal::toEntt(entities[5]));
        for (std::uint32_t i = 0; i < 6U; ++i) {
            const Counted* c = w.get<Counted>(entities[i]);
            REQUIRE(c != nullptr);
            CHECK(c->tagValue == i);
            CHECK(c->text == longText + std::to_string(i));
        }
        CHECK(Counted::live == before + 6);
    }
    CHECK(Counted::live == before);
}

TEST_CASE("scene register: a T whose copy throws leaves the entity without it and the pool intact (RG16)") {
    Fragile::armed = false;
    const int before = Fragile::live;
    {
        World w;
        w.registerComponent<Fragile>("game::Fragile");
        std::vector<Entity> entities;
        for (int i = 0; i < 3; ++i) {
            const Entity e = w.create();
            REQUIRE(w.add<Fragile>(e, Fragile{i + 1}) != nullptr);
            entities.push_back(e);
        }
        const Entity victim = w.create();
        const Fragile value{99};
        Fragile::armed = true;
        bool threw = false;
        try {
            w.addRaw(componentTypeId<Fragile>(), victim, &value);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        Fragile::armed = false;
        CHECK(threw);
        CHECK_FALSE(w.has<Fragile>(victim));
        CHECK(w.componentCount<Fragile>() == 3U);
        for (int i = 0; i < 3; ++i) {
            CHECK(w.get<Fragile>(entities[static_cast<std::size_t>(i)])->value == i + 1);
        }
        REQUIRE(w.add<Fragile>(victim, Fragile{7}) != nullptr);
        CHECK(w.get<Fragile>(victim)->value == 7);
        CHECK(w.componentCount<Fragile>() == 4U);
    }
    CHECK(Fragile::live == before);
}

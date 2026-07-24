// tests/scene_test.cpp — task 1.3.1: engine::World over the EnTT registry.
// Tier-0 throughout: no GPU, no reflect-gen, no files, no randomness, no sleeps. Runs identically
// with AERO_REFLECT_TOOLS=OFF — the structural proof that the scene layer has zero codegen
// dependency (AC-13).
//
// LIFETIME ACCOUNTING (verified against EnTT 3.16): the backing storage's swap-and-pop erase uses
// std::exchange, so ONE component erase runs the copy constructor once and the destructor TWICE
// (entt keeps the erased element alive in a temporary "to allow reentrant destructors"). The only
// robust invariant is therefore the BALANCE — Counted::live() == ctor - dtor — never a raw
// destructor delta. Do not "tighten" these assertions into delta checks; they will fail.
//
// Log output is exercised but NOT asserted (the 0.2.4 deferral — a log sink arrives in Phase 2).
// The moved-from cases deliberately emit ERROR lines; that is the tested behaviour, not a failure.

#include <aero/scene/internal/world_access.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

using engine::ComponentTypeId;
using engine::componentTypeId;
using engine::Entity;
using engine::World;
using engine::scene::internal::registerComponent;

namespace {

struct Position {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};
struct Velocity {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};
// Third non-empty component, deliberately laid on a NON-NESTING subset (every 3rd entity) so that a
// query's lead storage is not a subset of every other queried storage — the only shape that reaches
// advanceQuery's rejection sweep. See TEST_CASE "scene: each<T> and each<T,U>".
struct Health {
    float hp = 0.0F;
};
struct Marker {};  // empty/tag component
struct Big {
    std::array<std::uint8_t, 64> bytes{};  // size/alignment independence
};
struct Counted {
    static int ctor;
    static int dtor;
    int id = 0;
    Counted() { ++ctor; }
    explicit Counted(int value) : id(value) { ++ctor; }
    Counted(const Counted& other) : id(other.id) { ++ctor; }
    Counted& operator=(const Counted&) = default;
    ~Counted() { ++dtor; }
    static int live() noexcept { return ctor - dtor; }
    static void reset() noexcept {
        ctor = 0;
        dtor = 0;
    }
};
int Counted::ctor = 0;
int Counted::dtor = 0;

// Registers every fixture type. Returns by value; the move is proven safe by TEST_CASE 14.
World makeWorld() {
    World world;
    registerComponent<Position>(world, "test::Position");
    registerComponent<Velocity>(world, "test::Velocity");
    registerComponent<Health>(world, "test::Health");
    registerComponent<Marker>(world, "test::Marker");
    registerComponent<Big>(world, "test::Big");
    registerComponent<Counted>(world, "test::Counted");
    return world;
}

// AC-6's negative half. There is no compile-fail harness in this suite, so the contract is pinned by
// (a) the static_assert text in world.hpp and (b) this documented, deliberately-commented case.
// UNCOMMENTING EITHER LINE MUST FAIL THE BUILD with the quoted message:
//   world.get<Marker>(e);              // "an empty (tag) component stores no value - use has<T>()"
//   world.each<Marker>([](auto...){}); // "an empty (tag) component cannot be bound by reference"
// The predicate both assertions key on is pinned here so a change to Marker cannot silently make
// them vacuous:
static_assert(std::is_empty_v<Marker>);
static_assert(!std::is_empty_v<Position>);

// An optimizer barrier. Round-tripping a pointer through volatile storage stops the compiler from
// reasoning that two distinct objects have distinct addresses and constant-folding the comparison,
// so a check written through this observes the address the LINKER actually assigned.
const void* opaque(const void* p) {
    const void* volatile v = p;
    return v;
}

}  // namespace

// The anchor-identity guard. componentTypeId<T>() is the address of a per-type anchor byte, and the
// whole component API is keyed on it: if two types ever share an id, add<B>() writes a B into A's
// storage — type-confused memory corruption.
//
// This MUST be a runtime check. A static_assert cannot see it: the compiler runs before the linker,
// and the hazard is identical-COMDAT-folding at LINK time (MSVC defaults to /OPT:REF,ICF,LBR on any
// link without /DEBUG — i.e. every Release link). engine/scene/include/aero/scene/component.hpp keeps
// the anchor NON-const precisely so it lands in writable data, which ICF will not fold; this case is
// what fails if that ever regresses.
TEST_CASE("scene: distinct component types have distinct ids") {
    const std::array<ComponentTypeId, 6> ids{componentTypeId<Position>(), componentTypeId<Velocity>(),
                                             componentTypeId<Health>(),   componentTypeId<Marker>(),
                                             componentTypeId<Big>(),      componentTypeId<Counted>()};

    for (std::size_t i = 0; i < ids.size(); ++i) {
        CHECK(ids[i].valid());
        for (std::size_t j = i + 1; j < ids.size(); ++j) {
            // Compared through the barrier: this is the linker's answer, not the compiler's.
            CHECK(opaque(ids[i].value) != opaque(ids[j].value));
        }
    }

    // The same type always answers with the same id, in this TU and across the layer's own TUs.
    CHECK(opaque(componentTypeId<Position>().value) == opaque(ids[0].value));
    CHECK(componentTypeId<Position>() == ids[0]);
}

TEST_CASE("scene: entity create/alive/destroy") {
    World w;  // no registration needed for entity-only operations
    const Entity a = w.create();
    const Entity b = w.create();
    CHECK(a.valid());
    CHECK(b.valid());
    CHECK(a.generation >= 1U);
    CHECK(b.generation >= 1U);
    CHECK(a.index != b.index);
    CHECK(!(a == b));

    CHECK(w.alive(a));
    CHECK(w.alive(b));
    CHECK(!w.alive(Entity{}));

    CHECK(w.destroy(a));
    CHECK(!w.destroy(a));
    CHECK(!w.alive(a));
    CHECK(w.alive(b));

    CHECK(!w.destroy(Entity{}));
}

TEST_CASE("scene: destroyed handles stay rejected after recycling") {
    World w;
    const Entity a = w.create();
    CHECK(w.destroy(a));
    const Entity recycled = w.create();
    CHECK(recycled.index == a.index);
    CHECK(recycled.generation == a.generation + 1U);
    CHECK(!(recycled == a));

    registerComponent<Position>(w, "test::Position");
    CHECK(!w.alive(a));
    CHECK(w.get<Position>(a) == nullptr);
    CHECK(!w.has<Position>(a));
    CHECK(!w.remove<Position>(a));
    CHECK(w.add<Position>(a) == nullptr);  // on the recycled slot: the STALE handle is rejected
    CHECK(w.alive(recycled));

    // The dangerous configuration, and the only one that tells a real generation check apart from an
    // implementation that ignores the generation entirely: the recycled twin now actually HOLDS a
    // Position, so every rejection below has to come from the handle rather than from an empty
    // storage — and the live component has to survive the stale operations untouched.
    REQUIRE(w.add<Position>(recycled, Position{7.0F, 8.0F, 9.0F}) != nullptr);
    CHECK(w.componentCount<Position>() == 1);
    CHECK(w.get<Position>(a) == nullptr);
    CHECK(!w.has<Position>(a));
    // remove must not erase the live twin's component; add must not overwrite it.
    CHECK(!w.remove<Position>(a));
    CHECK(w.add<Position>(a) == nullptr);
    // The same three rejections through the type-erased primitives the loader (1.4.2) will use.
    CHECK(w.getRaw(componentTypeId<Position>(), a) == nullptr);
    CHECK(!w.hasRaw(componentTypeId<Position>(), a));
    CHECK(!w.removeRaw(componentTypeId<Position>(), a));
    CHECK(w.componentCount<Position>() == 1);

    const auto* live = w.get<Position>(recycled);
    REQUIRE(live != nullptr);
    CHECK(live->x == 7.0F);
    CHECK(live->y == 8.0F);
    CHECK(live->z == 9.0F);
}

TEST_CASE("scene: entityCount tracks the live set") {
    World w;
    CHECK(w.entityCount() == 0);

    std::vector<Entity> es;
    es.reserve(6);
    for (int i = 0; i < 6; ++i) {
        es.push_back(w.create());
    }
    CHECK(w.entityCount() == 6);

    CHECK(w.destroy(es[1]));
    CHECK(w.destroy(es[3]));
    CHECK(w.destroy(es[4]));
    CHECK(w.entityCount() == 3);

    (void)w.create();
    (void)w.create();
    CHECK(w.entityCount() == 5);

    w.clear();
    CHECK(w.entityCount() == 0);
}

TEST_CASE("scene: eachEntity visits exactly the live entities") {
    World w;
    std::vector<Entity> es;
    es.reserve(6);
    for (int i = 0; i < 6; ++i) {
        es.push_back(w.create());
    }
    const std::vector<Entity> destroyed{es[1], es[3], es[4]};
    for (const Entity e : destroyed) {
        CHECK(w.destroy(e));
    }

    std::vector<Entity> seen;
    w.eachEntity([&](Entity e) { seen.push_back(e); });
    CHECK(seen.size() == 3);
    CHECK(seen.size() == w.entityCount());
    for (const Entity e : seen) {
        CHECK(w.alive(e));
    }
    CHECK(std::none_of(seen.begin(), seen.end(),
                       [&](Entity e) { return std::find(destroyed.begin(), destroyed.end(), e) != destroyed.end(); }));

    std::vector<Entity> expected;
    for (const Entity e : es) {
        if (std::find(destroyed.begin(), destroyed.end(), e) == destroyed.end()) {
            expected.push_back(e);
        }
    }
    std::sort(seen.begin(), seen.end(), [](Entity a, Entity b) { return a.index < b.index; });
    std::sort(expected.begin(), expected.end(), [](Entity a, Entity b) { return a.index < b.index; });
    CHECK(seen == expected);

    const World& cw = w;
    std::size_t constCount = 0;
    cw.eachEntity([&](Entity) { ++constCount; });
    CHECK(constCount == 3);

    const World empty;
    std::size_t emptyCount = 0;
    empty.eachEntity([&](Entity) { ++emptyCount; });
    CHECK(emptyCount == 0);
}

TEST_CASE("scene: clear() empties entities and components") {
    World w = makeWorld();
    std::vector<Entity> es;
    for (int i = 0; i < 4; ++i) {
        const Entity e = w.create();
        REQUIRE(w.add<Position>(e, Position{1.0F, 2.0F, 3.0F}) != nullptr);
        es.push_back(e);
    }
    const Entity m0 = w.create();
    const Entity m1 = w.create();
    w.add<Marker>(m0);
    w.add<Marker>(m1);

    const std::size_t typesBefore = w.componentTypeCount();
    w.clear();

    CHECK(w.entityCount() == 0);
    CHECK(w.componentCount<Position>() == 0);
    CHECK(w.componentCount<Marker>() == 0);
    for (const Entity e : es) {
        CHECK(!w.alive(e));
    }
    CHECK(!w.alive(m0));
    CHECK(!w.alive(m1));

    CHECK(w.componentTypeCount() == typesBefore);
    CHECK(w.registered(componentTypeId<Position>()));

    // Repopulating WITHOUT re-registering works.
    const Entity e = w.create();
    REQUIRE(w.add<Position>(e, Position{1.0F, 2.0F, 3.0F}) != nullptr);
}

TEST_CASE("scene: add/get/has/remove round-trip") {
    World w = makeWorld();
    const Entity e = w.create();
    auto* p = w.add<Position>(e, Position{1.5F, -2.0F, 3.25F});
    REQUIRE(p != nullptr);
    CHECK(p->x == 1.5F);
    CHECK(p->y == -2.0F);
    CHECK(p->z == 3.25F);

    const auto* got = w.get<Position>(e);
    REQUIRE(got != nullptr);
    CHECK(got->x == 1.5F);
    CHECK(w.has<Position>(e));
    CHECK(w.componentCount<Position>() == 1);

    auto* v = w.add<Velocity>(e, Velocity{9.0F, 8.0F, 7.0F});
    REQUIRE(v != nullptr);
    CHECK(w.get<Position>(e)->x == 1.5F);  // independent of the Velocity add

    const Entity e2 = w.create();
    REQUIRE(w.add<Position>(e2, Position{0.0F, 0.0F, 0.0F}) != nullptr);
    CHECK(!w.has<Velocity>(e2));
    CHECK(w.componentCount<Velocity>() == 1);
    CHECK(w.componentCount<Position>() == 2);

    p->x = 42.0F;
    CHECK(w.get<Position>(e)->x == 42.0F);  // mutation through p is visible via a fresh get

    CHECK(w.remove<Velocity>(e));
    CHECK(!w.remove<Velocity>(e));
    CHECK(w.get<Velocity>(e) == nullptr);
    CHECK(!w.has<Velocity>(e));
    CHECK(w.componentCount<Position>() == 2);  // unaffected

    const World& cw = w;
    CHECK(cw.get<Position>(e) != nullptr);
    CHECK(cw.has<Position>(e));
    CHECK(cw.componentCount<Position>() == 2);

    // addRaw(id, e, nullptr) — the 1.4.2 loader path.
    const Entity e3 = w.create();
    void* raw = w.addRaw(componentTypeId<Position>(), e3, nullptr);
    REQUIRE(raw != nullptr);
    const auto* rawPos = static_cast<const Position*>(raw);
    CHECK(rawPos->x == 0.0F);
    CHECK(rawPos->y == 0.0F);
    CHECK(rawPos->z == 0.0F);
}

TEST_CASE("scene: add replaces an existing component") {
    World w = makeWorld();
    const Entity e = w.create();
    REQUIRE(w.add<Position>(e, Position{1.0F, 2.0F, 3.0F}) != nullptr);
    auto* second = w.add<Position>(e, Position{9.0F, 8.0F, 7.0F});
    REQUIRE(second != nullptr);
    CHECK(second->x == 9.0F);
    CHECK(second->y == 8.0F);
    CHECK(second->z == 7.0F);
    CHECK(w.componentCount<Position>() == 1);
    CHECK(w.get<Position>(e)->x == 9.0F);
    // NOTE: the address is deliberately NOT asserted to equal the first add<T>()'s return — D11's
    // replace is erase-then-insert and the storage's packed order may change. With a single element
    // it happens to come back to the same address; with several it need not. No test may depend on
    // either.
}

TEST_CASE("scene: tag components") {
    World w = makeWorld();
    const Entity e = w.create();
    CHECK(w.add<Marker>(e) == nullptr);
    CHECK(w.has<Marker>(e));
    CHECK(w.componentCount<Marker>() == 1);

    const Entity e2 = w.create();
    CHECK(!w.has<Marker>(e2));
    CHECK(w.componentCount<Marker>() == 1);

    CHECK(w.remove<Marker>(e));
    CHECK(!w.remove<Marker>(e));
    CHECK(w.componentCount<Marker>() == 0);
    CHECK(!w.has<Marker>(e));

    void* raw = w.addRaw(componentTypeId<Marker>(), e2, nullptr);
    CHECK(raw == nullptr);
    CHECK(w.hasRaw(componentTypeId<Marker>(), e2));
    // See the commented negative case above `TEST_CASE`s (AC-6's compile-time half): get<Marker>()
    // and each<Marker>() are both a compile error, on purpose.
}

TEST_CASE("scene: a 64-byte component round-trips byte-exact") {
    World w = makeWorld();
    Big source{};
    for (std::size_t i = 0; i < source.bytes.size(); ++i) {
        source.bytes[i] = static_cast<std::uint8_t>((i * 3) + 1);
    }
    const Entity e = w.create();
    Big* stored = w.add<Big>(e, source);
    REQUIRE(stored != nullptr);
    CHECK(std::memcmp(stored->bytes.data(), source.bytes.data(), source.bytes.size()) == 0);

    const Big* reread = w.get<Big>(e);
    REQUIRE(reread != nullptr);
    CHECK(std::memcmp(reread->bytes.data(), source.bytes.data(), source.bytes.size()) == 0);
    CHECK(w.componentCount<Big>() == 1);

    Big other{};
    for (std::size_t i = 0; i < other.bytes.size(); ++i) {
        other.bytes[i] = static_cast<std::uint8_t>(255U - i);
    }
    const Entity e2 = w.create();
    Big* stored2 = w.add<Big>(e2, other);
    REQUIRE(stored2 != nullptr);
    CHECK(std::memcmp(w.get<Big>(e)->bytes.data(), source.bytes.data(), source.bytes.size()) == 0);
    CHECK(std::memcmp(w.get<Big>(e2)->bytes.data(), other.bytes.data(), other.bytes.size()) == 0);
}

TEST_CASE("scene: component lifetimes") {
    Counted::reset();
    {
        World w = makeWorld();
        std::vector<Entity> es;
        for (int i = 0; i < 4; ++i) {
            const Entity e = w.create();
            REQUIRE(w.add<Counted>(e, Counted{i}) != nullptr);
            es.push_back(e);
        }
        CHECK(Counted::live() == 4);
        CHECK(w.componentCount<Counted>() == 4);

        CHECK(w.remove<Counted>(es[0]));
        CHECK(Counted::live() == 3);
        CHECK(w.componentCount<Counted>() == 3);

        CHECK(w.destroy(es[1]));
        CHECK(Counted::live() == 2);
        CHECK(w.componentCount<Counted>() == 2);

        w.clear();
        CHECK(Counted::live() == 0);
        CHECK(w.componentCount<Counted>() == 0);

        for (int i = 0; i < 3; ++i) {
            const Entity e = w.create();
            REQUIRE(w.add<Counted>(e, Counted{i}) != nullptr);
        }
        CHECK(Counted::live() == 3);
    }
    CHECK(Counted::live() == 0);
    CHECK(Counted::ctor == Counted::dtor);
}

TEST_CASE("scene: each<T> and each<T,U>") {
    World w = makeWorld();
    std::vector<Entity> es;
    for (int i = 0; i < 20; ++i) {
        const Entity e = w.create();
        REQUIRE(w.add<Position>(e, Position{static_cast<float>(i), 0.0F, 0.0F}) != nullptr);
        if (i % 2 == 0) {
            REQUIRE(w.add<Velocity>(e, Velocity{}) != nullptr);
        }
        // Every 3rd, so Health interleaves with Velocity instead of nesting inside it.
        if (i % 3 == 0) {
            REQUIRE(w.add<Health>(e, Health{static_cast<float>(i) * 10.0F}) != nullptr);
        }
        if (i % 5 == 0) {
            w.add<Marker>(e);
        }
        es.push_back(e);
    }

    std::size_t posCount = 0;
    float sumX = 0.0F;
    w.each<Position>([&](Entity, Position& p) {
        ++posCount;
        sumX += p.x;
    });
    CHECK(posCount == 20);
    CHECK(sumX == 190.0F);

    std::size_t pvCount = 0;
    w.each<Position, Velocity>([&](Entity e, Position&, Velocity&) {
        ++pvCount;
        CHECK(w.has<Velocity>(e));
    });
    CHECK(pvCount == 10);

    std::vector<std::uint32_t> orderA;
    w.each<Position, Velocity>([&](Entity e, Position&, Velocity&) { orderA.push_back(e.index); });
    std::vector<std::uint32_t> orderB;
    w.each<Velocity, Position>([&](Entity e, Velocity&, Position&) { orderB.push_back(e.index); });
    std::sort(orderA.begin(), orderA.end());
    std::sort(orderB.begin(), orderB.end());
    CHECK(orderA == orderB);  // the SET is identical regardless of argument order

    // ---- the intersection filter itself ---------------------------------------------------------
    // Every query above has a lead storage (the smallest) that is a strict subset of every other
    // queried storage, so the rejection sweep inside the query never has anything to reject. Health
    // is the case that does: it sits on every 3rd entity, Velocity on every 2nd, so entities 3, 9
    // and 15 hold Health and NO Velocity — exactly what a missing sweep would leak. The intersection
    // is the 4 entities divisible by 6, a count that matches no individual storage size
    // (P=20, V=10, H=7), so "yield the lead storage whole" cannot pass either.
    CHECK(w.componentCount<Position>() == 20);
    CHECK(w.componentCount<Velocity>() == 10);
    CHECK(w.componentCount<Health>() == 7);

    const auto byIndex = [](Entity lhs, Entity rhs) { return lhs.index < rhs.index; };

    std::vector<Entity> expectedTriple;  // i % 6 == 0 -> 0, 6, 12, 18
    std::vector<Entity> expectedHealth;  // i % 3 == 0 -> 0, 3, 6, 9, 12, 15, 18
    for (std::size_t i = 0; i < es.size(); ++i) {
        if (i % 3 == 0) {
            expectedHealth.push_back(es[i]);
        }
        if (i % 6 == 0) {
            expectedTriple.push_back(es[i]);
        }
    }
    REQUIRE(expectedTriple.size() == 4);
    REQUIRE(expectedHealth.size() == 7);

    // Lead = Health (7 entries, the smallest), and it is NOT a subset of Velocity: the sweep must
    // skip 3, 9 and 15. The IDENTITIES are asserted, not just the count, so a wrong-subset
    // regression cannot pass on a coincidental tally.
    std::vector<Entity> velHealth;
    w.each<Velocity, Health>([&](Entity e, Velocity&, Health&) { velHealth.push_back(e); });
    std::sort(velHealth.begin(), velHealth.end(), byIndex);
    CHECK(velHealth.size() == 4);
    CHECK(velHealth == expectedTriple);

    // The same query with the lead FIRST in the pack (lead index 0), so the sweep's skip-the-lead
    // step is exercised at the other end of the argument list.
    std::vector<Entity> healthVel;
    w.each<Health, Velocity>([&](Entity e, Health&, Velocity&) { healthVel.push_back(e); });
    std::sort(healthVel.begin(), healthVel.end(), byIndex);
    CHECK(healthVel.size() == 4);
    CHECK(healthVel == expectedTriple);

    // Three types at once, with a cross-pool consistency check: hp was stored as 10x Position.x, so
    // a mis-ordered slot fill shows up as a value mismatch rather than a count mismatch.
    std::vector<Entity> triple;
    w.each<Position, Velocity, Health>([&](Entity e, Position& p, Velocity&, Health& h) {
        triple.push_back(e);
        CHECK(h.hp == p.x * 10.0F);
    });
    std::sort(triple.begin(), triple.end(), byIndex);
    CHECK(triple.size() == 4);
    CHECK(triple == expectedTriple);

    // The control: Health IS a subset of Position, so nothing is rejected and all 7 Health holders
    // are visited. Proves the sweep filters rather than dropping entities unconditionally.
    std::vector<Entity> healthPos;
    w.each<Health, Position>([&](Entity e, Health&, Position&) { healthPos.push_back(e); });
    std::sort(healthPos.begin(), healthPos.end(), byIndex);
    CHECK(healthPos.size() == 7);
    CHECK(healthPos == expectedHealth);

    // Mutation through the reference.
    const Entity known = es[0];
    w.each<Position, Velocity>([&](Entity e, Position& p, Velocity&) {
        if (e == known) {
            p.x += 100.0F;
        }
    });
    CHECK(w.get<Position>(known)->x == 100.0F);

    struct Unused {
        int v = 0;
    };
    registerComponent<Unused>(w, "test::Unused");
    std::size_t unusedCount = 0;
    w.each<Position, Unused>([&](Entity, Position&, Unused&) { ++unusedCount; });
    CHECK(unusedCount == 0);  // registered but empty storage — no ERROR, just zero invocations

    World emptyWorld = makeWorld();  // registered, but no entities created yet
    std::size_t emptyCount = 0;
    emptyWorld.each<Position>([&](Entity, Position&) { ++emptyCount; });
    CHECK(emptyCount == 0);

    // Nested each: the inner query runs fully inside each outer visit, on the SAME World — the
    // cursor lives entirely on the caller's stack, so nesting is safe (E-nested-each).
    std::size_t outerCount = 0;
    std::size_t innerTotal = 0;
    w.each<Position>([&](Entity, Position&) {
        ++outerCount;
        std::size_t innerCount = 0;
        w.each<Velocity>([&](Entity, Velocity&) { ++innerCount; });
        innerTotal += innerCount;
    });
    CHECK(outerCount == 20);
    CHECK(innerTotal == 20 * 10U);  // 10 Velocity holders visited per outer visit
}

TEST_CASE("scene: unregistered types are rejected, not fatal") {
    World w = makeWorld();
    struct Unregistered {
        int z = 0;
    };
    const Entity e = w.create();

    CHECK(w.add<Unregistered>(e) == nullptr);
    CHECK(w.get<Unregistered>(e) == nullptr);
    CHECK(!w.has<Unregistered>(e));
    CHECK(!w.remove<Unregistered>(e));
    CHECK(w.componentCount<Unregistered>() == 0);

    std::size_t count = 0;
    w.each<Unregistered>([&](Entity, Unregistered&) { ++count; });
    CHECK(count == 0);

    CHECK(!w.registered(componentTypeId<Unregistered>()));
}

TEST_CASE("scene: registration table") {
    World w;
    const ComponentTypeId id = registerComponent<Position>(w, "test::Position");
    CHECK(id.valid());
    CHECK(id == componentTypeId<Position>());
    CHECK(w.registered(id));
    CHECK(w.componentTypeName(id) == std::string_view{"test::Position"});
    CHECK(w.findComponentType("test::Position") == id);
    CHECK(w.componentTypeCount() == 5);  // 5: Transform+Camera+DirectionalLight+PointLight (task 1.3.3)

    const ComponentTypeId again = registerComponent<Position>(w, "test::Position");
    CHECK(again == id);
    CHECK(w.componentTypeCount() == 5);  // still 5 — re-registration is idempotent

    const Entity e = w.create();
    REQUIRE(w.add<Position>(e) != nullptr);
    registerComponent<Position>(w, "test::Position");
    CHECK(w.componentCount<Position>() == 1);  // storage untouched by re-registration

    const ComponentTypeId velId = registerComponent<Velocity>(w, "test::Velocity");
    CHECK(w.componentTypeCount() == 6);  // 4 builtins + Position + Velocity
    CHECK(w.findComponentType("test::Position") == id);
    CHECK(w.findComponentType("test::Velocity") == velId);

    CHECK(!w.findComponentType("nope").valid());
    CHECK(w.componentTypeName(ComponentTypeId{}).empty());

    // Two independent Worlds.
    World a;
    World b;
    const ComponentTypeId idA = registerComponent<Position>(a, "test::Position");
    CHECK(a.registered(idA));
    CHECK(!b.registered(componentTypeId<Position>()));
    const ComponentTypeId idB = registerComponent<Position>(b, "test::Position");
    CHECK(idB == idA);  // same per-program-image id
    const Entity ea = a.create();
    REQUIRE(a.add<Position>(ea) != nullptr);
    CHECK(b.componentCount<Position>() == 0);  // independent storages

    // Re-register under a DIFFERENT name — same id, first name kept (a WARN is emitted, not
    // asserted — the 0.2.4 deferral).
    const ComponentTypeId renamed = registerComponent<Position>(w, "test::PositionRenamed");
    CHECK(renamed == id);
    CHECK(w.componentTypeName(id) == std::string_view{"test::Position"});

    // An ALREADY-REGISTERED type asked for under another type's name. This is still the existing-id
    // branch (WARN, registration unchanged) — NOT the duplicate-name branch — because Velocity is
    // already in this World's table. Pinned non-tautologically: the call must hand back Velocity's
    // own id, must not rename it, and must not grow the table.
    const ComponentTypeId velRenamed = registerComponent<Velocity>(w, "test::Position");
    CHECK(velRenamed == velId);
    CHECK(w.componentTypeName(velId) == std::string_view{"test::Velocity"});
    CHECK(w.componentTypeCount() == 6);  // unchanged by the existing-id WARN branch
    CHECK(w.findComponentType("test::Position") == id);
    CHECK(w.findComponentType("test::Velocity") == velId);

    // Duplicate NAME on a genuinely NEW id — the OTHER branch. Big has never been registered in this
    // World, so it IS appended, under a name that is already taken: the table grows, the new type
    // answers to that duplicate name, and findComponentType keeps resolving the name to the FIRST
    // registrant.
    const ComponentTypeId bigId = registerComponent<Big>(w, "test::Position");
    CHECK(bigId.valid());
    CHECK(bigId == componentTypeId<Big>());
    CHECK(!(bigId == id));
    CHECK(w.registered(bigId));
    CHECK(w.componentTypeCount() == 7);  // + Big, appended under a duplicate name
    CHECK(w.componentTypeName(bigId) == std::string_view{"test::Position"});
    CHECK(w.findComponentType("test::Position") == id);
    CHECK(w.findComponentType("test::Velocity") == velId);

    // The appended type is fully usable despite sharing a name — the duplicate is a label clash, not
    // a registration failure.
    const Entity eb = w.create();
    REQUIRE(w.add<Big>(eb) != nullptr);
    CHECK(w.componentCount<Big>() == 1);
}

TEST_CASE("scene: move semantics and the inert moved-from World") {
    std::optional<World> w;
    w.emplace();
    *w = makeWorld();

    std::vector<Entity> es;
    for (int i = 0; i < 5; ++i) {
        const Entity e = w->create();
        REQUIRE(w->add<Position>(e, Position{static_cast<float>(i), 0.0F, 0.0F}) != nullptr);
        if (i < 2) {
            REQUIRE(w->add<Velocity>(e, Velocity{}) != nullptr);
        }
        es.push_back(e);
    }
    const Entity e0 = es[0];

    World moved = std::move(*w);

    // moved-to owns everything.
    CHECK(moved.entityCount() == 5);
    CHECK(moved.componentCount<Position>() == 5);
    CHECK(moved.registered(componentTypeId<Position>()));
    const Position* p0 = moved.get<Position>(e0);
    REQUIRE(p0 != nullptr);
    CHECK(p0->x == 0.0F);
    std::size_t eachCount = 0;
    moved.each<Position>([&](Entity, Position&) { ++eachCount; });
    CHECK(eachCount == 5);
    CHECK(moved.findComponentType("test::Position").valid());

    // moved-from is INERT on every entry point.
    CHECK(!w->create().valid());
    CHECK(w->entityCount() == 0);
    CHECK(!w->alive(e0));
    CHECK(w->get<Position>(e0) == nullptr);
    CHECK(w->add<Position>(e0) == nullptr);
    CHECK(!w->has<Position>(e0));
    CHECK(!w->remove<Position>(e0));
    CHECK(!w->destroy(e0));
    CHECK(w->componentTypeCount() == 0);
    CHECK(!w->registered(componentTypeId<Position>()));
    CHECK(!w->findComponentType("test::Position").valid());
    std::size_t movedFromEachCount = 0;
    w->each<Position>([&](Entity, Position&) { ++movedFromEachCount; });
    CHECK(movedFromEachCount == 0);
    std::size_t movedFromEachEntityCount = 0;
    w->eachEntity([&](Entity) { ++movedFromEachEntityCount; });
    CHECK(movedFromEachEntityCount == 0);

    // Move-assignment over a LIVE World releases the old one.
    Counted::reset();
    {
        World worldA = makeWorld();
        for (int i = 0; i < 3; ++i) {
            const Entity e = worldA.create();
            REQUIRE(worldA.add<Counted>(e, Counted{i}) != nullptr);
        }
        World worldB = makeWorld();
        const Entity eb = worldB.create();
        REQUIRE(worldB.add<Counted>(eb, Counted{99}) != nullptr);

        CHECK(Counted::live() == 4);
        worldA = std::move(worldB);
        CHECK(Counted::live() == 1);
    }
    CHECK(Counted::live() == 0);
}

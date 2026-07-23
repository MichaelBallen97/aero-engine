// tests/transform_test.cpp — task 1.3.2: engine::Transform, the World-owned entity hierarchy, and
// world-matrix composition. Tier-0 throughout: no GPU, no reflect-gen, no generated code, no files,
// no randomness, no sleeps.
//
// THIS TU MUST NEVER TOUCH GENERATED CODE. It rides aero_tests unconditionally and passes identically
// with -DAERO_REFLECT_TOOLS=OFF — the structural proof that the component, the hierarchy and the
// matrices have ZERO codegen dependency (AC-13). The generated meta/JSON proofs live in the two gated
// targets (aero_reflect_meta_test / aero_reflect_json_test) and in the reflect-gen ctest cases.
//
// DESTRUCTION ORDER IS NOT A CONTRACT. destroy() destroys a subtree post-order (children before
// parents), but the exact sequence is an implementation detail the spec deliberately leaves free:
// these cases pin the destroyed SET, the entityCount() delta and the lifetime BALANCE, never an
// ordering. Do not "tighten" them into sequence assertions.
//
// LIFETIME ACCOUNTING: the backing storage's swap-and-pop erase runs the destructor twice for one
// erase (it keeps the erased element alive in a temporary). The only robust invariant is the BALANCE,
// Counted::live() == ctor - dtor — never a raw destructor delta.
//
// Log output is exercised but NOT asserted (the 0.2.4 deferral). The rejection cases deliberately
// emit ERROR lines; that is the tested behaviour, not a failure.

#include <aero/scene/internal/world_access.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

using engine::Entity;
using engine::Mat4;
using engine::Quat;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::scene::internal::registerComponent;

namespace {

// The 1.3.1 lifetime fixture, reused verbatim: only the BALANCE is ever asserted.
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

// A small distinct component type, used only to prove the registration table's ORDER (TEST_CASE 1):
// registering it under Transform's own name must not steal the name away from Transform, because
// Transform was registered first (in the constructor).
struct Dummy {
    int value = 0;
};

// A root -> child -> ... chain of `n` entities, each carrying `step` as its local translation.
std::vector<Entity> makeChain(World& world, std::size_t n, Vec3 step) {
    std::vector<Entity> chain;
    chain.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Entity e = world.create();
        REQUIRE(world.add<Transform>(e, Transform{step, Quat::identity(), Vec3::one()}) != nullptr);
        if (i > 0) {
            REQUIRE(world.setParent(e, chain[i - 1U]));
        }
        chain.push_back(e);
    }
    return chain;
}

std::vector<Entity> collectChildren(const World& world, Entity parent) {
    std::vector<Entity> out;
    world.eachChild(parent, [&](Entity child) { out.push_back(child); });
    return out;
}

}  // namespace

TEST_CASE("transform: every World seeds engine::Transform by construction") {
    World w;
    CHECK(w.componentTypeCount() == 1);
    CHECK(w.findComponentType("engine::Transform") == engine::componentTypeId<Transform>());
    CHECK(w.componentTypeName(engine::componentTypeId<Transform>()) == std::string_view{"engine::Transform"});
    CHECK(w.registered(engine::componentTypeId<Transform>()));

    // add/get/has/remove/each/addRaw all work with NO explicit registration call.
    const Entity e = w.create();
    auto* added = w.add<Transform>(e, Transform{Vec3{1.0F, 2.0F, 3.0F}, Quat::identity(), Vec3::one()});
    REQUIRE(added != nullptr);
    CHECK(added->position == Vec3{1.0F, 2.0F, 3.0F});
    CHECK(w.has<Transform>(e));
    const Transform* got = w.get<Transform>(e);
    REQUIRE(got != nullptr);
    CHECK(got->position == Vec3{1.0F, 2.0F, 3.0F});

    std::size_t eachCount = 0;
    w.each<Transform>([&](Entity, Transform&) { ++eachCount; });
    CHECK(eachCount == 1);

    CHECK(w.remove<Transform>(e));
    CHECK(!w.has<Transform>(e));

    // addRaw with a null source default-constructs (identity Transform).
    void* raw = w.addRaw(engine::componentTypeId<Transform>(), e, nullptr);
    REQUIRE(raw != nullptr);
    CHECK(*static_cast<Transform*>(raw) == Transform{});

    // Two independent Worlds: each seeds its OWN table and storage.
    World a;
    const World b;
    CHECK(a.componentTypeCount() == 1);
    CHECK(b.componentTypeCount() == 1);
    const Entity ea = a.create();
    REQUIRE(a.add<Transform>(ea) != nullptr);
    CHECK(b.componentCount<Transform>() == 0);

    // Registration order: Transform is registered FIRST (in the constructor), so a later type
    // registered under the SAME name does not steal it -- findComponentType keeps resolving to
    // Transform, proving Transform is the earliest entry a menu walk would see.
    const engine::ComponentTypeId dup = registerComponent<Dummy>(w, "engine::Transform");
    CHECK(dup == engine::componentTypeId<Dummy>());
    CHECK(w.findComponentType("engine::Transform") == engine::componentTypeId<Transform>());
}

TEST_CASE("transform: move semantics do not re-seed or lose the table") {
    std::optional<World> w;
    w.emplace();
    const Entity e = w->create();
    REQUIRE(w->add<Transform>(e, Transform{Vec3{4.0F, 5.0F, 6.0F}, Quat::identity(), Vec3::one()}) != nullptr);

    World moved = std::move(*w);
    CHECK(moved.componentTypeCount() == 1);
    CHECK(moved.findComponentType("engine::Transform").valid());
    const Transform* got = moved.get<Transform>(e);
    REQUIRE(got != nullptr);
    CHECK(got->position == Vec3{4.0F, 5.0F, 6.0F});

    // Moved-from is inert on every entry point, including the hierarchy (task 1.3.2).
    CHECK(w->componentTypeCount() == 0);
    CHECK(!w->parent(e).valid());
    CHECK(w->childCount(e) == 0);
    CHECK(!w->setParent(e, Entity{}));
    std::size_t moveFromChildCount = 0;
    w->eachChild(e, [&](Entity) { ++moveFromChildCount; });
    CHECK(moveFromChildCount == 0);
}

TEST_CASE("transform: the default value is the identity transform") {
    const Transform t{};
    CHECK(t.position == Vec3::zero());
    CHECK(t.rotation == Quat::identity());
    CHECK(t.scale == Vec3::one());

    // compose() of an identity TRS must be bit-identity -- an EXACT compare, not approxEquals.
    CHECK(engine::localMatrix(t) == Mat4::identity());

    CHECK(Transform{} == Transform{});
    Transform other{};
    other.position = Vec3{1.0F, 0.0F, 0.0F};
    CHECK_FALSE(Transform{} == other);
}

TEST_CASE("transform: the component behaves like any other through World") {
    World w;
    const Entity e = w.create();

    auto* first = w.add<Transform>(e, Transform{Vec3{1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()});
    REQUIRE(first != nullptr);
    first->position = Vec3{2.0F, 0.0F, 0.0F};
    const Transform* reread = w.get<Transform>(e);
    REQUIRE(reread != nullptr);
    CHECK(reread->position == Vec3{2.0F, 0.0F, 0.0F});

    // add<T>() again REPLACES -- address deliberately not asserted.
    REQUIRE(w.add<Transform>(e, Transform{Vec3{9.0F, 9.0F, 9.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    CHECK(w.componentCount<Transform>() == 1);
    const Transform* replaced = w.get<Transform>(e);
    REQUIRE(replaced != nullptr);
    CHECK(replaced->position == Vec3{9.0F, 9.0F, 9.0F});

    CHECK(w.has<Transform>(e));
    CHECK(w.remove<Transform>(e));
    CHECK_FALSE(w.remove<Transform>(e));

    const Entity e2 = w.create();
    REQUIRE(w.add<Transform>(e2) != nullptr);
    std::size_t count = 0;
    w.each<Transform>([&](Entity, Transform&) { ++count; });
    CHECK(count == 1);

    const World& cw = w;
    const auto* viaConst = cw.get<Transform>(e2);
    REQUIRE(viaConst != nullptr);
    CHECK(*viaConst == Transform{});
}

TEST_CASE("scene: setParent links and detaches") {
    World w;
    const Entity p = w.create();
    const Entity c = w.create();

    CHECK(w.setParent(c, p));
    CHECK(w.parent(c) == p);
    CHECK(w.childCount(p) == 1);
    CHECK(collectChildren(w, p) == std::vector<Entity>{c});

    CHECK(w.setParent(c, Entity{}));
    CHECK(w.parent(c) == Entity{});
    CHECK(w.childCount(p) == 0);
    CHECK(collectChildren(w, p).empty());

    CHECK(w.setParent(c, p));  // re-attach
    CHECK(w.parent(c) == p);

    const Entity neverParented = w.create();
    CHECK(w.parent(neverParented) == Entity{});
    CHECK(w.childCount(neverParented) == 0);
}

TEST_CASE("scene: re-attaching to the current parent is a silent no-op that preserves sibling order") {
    World w;
    const Entity p = w.create();
    const Entity a = w.create();
    const Entity b = w.create();
    const Entity c = w.create();
    REQUIRE(w.setParent(a, p));
    REQUIRE(w.setParent(b, p));
    REQUIRE(w.setParent(c, p));
    CHECK(collectChildren(w, p) == std::vector<Entity>{a, b, c});

    CHECK(w.setParent(b, p));                                      // already b's parent
    CHECK(collectChildren(w, p) == std::vector<Entity>{a, b, c});  // unchanged -- no move to the back

    const Entity root = w.create();
    CHECK(w.setParent(root, Entity{}));  // already a root
    CHECK(w.parent(root) == Entity{});
}

TEST_CASE("scene: reparenting moves between child lists and preserves the survivors' order") {
    World w;
    const Entity p1 = w.create();
    const Entity p2 = w.create();
    const Entity a = w.create();
    const Entity b = w.create();
    const Entity c = w.create();
    REQUIRE(w.setParent(a, p1));
    REQUIRE(w.setParent(b, p1));
    REQUIRE(w.setParent(c, p1));

    CHECK(w.setParent(b, p2));
    CHECK(collectChildren(w, p1) == std::vector<Entity>{a, c});  // ordered erase -- c did not jump into b's slot
    CHECK(collectChildren(w, p2) == std::vector<Entity>{b});
    CHECK(w.parent(b) == p2);

    const Entity d = w.create();
    REQUIRE(w.setParent(d, p1));
    CHECK(collectChildren(w, p1) == std::vector<Entity>{a, c, d});  // appends
}

TEST_CASE("scene: eachChild yields direct children only, in attach order") {
    World w;
    const Entity p = w.create();
    const Entity c1 = w.create();
    const Entity c2 = w.create();
    const Entity g1 = w.create();
    const Entity g2 = w.create();
    REQUIRE(w.setParent(c1, p));
    REQUIRE(w.setParent(c2, p));
    REQUIRE(w.setParent(g1, c1));
    REQUIRE(w.setParent(g2, c1));

    CHECK(collectChildren(w, p) == std::vector<Entity>{c1, c2});  // grandchildren absent
    CHECK(collectChildren(w, c1) == std::vector<Entity>{g1, g2});

    // Nested eachChild over a DIFFERENT parent, inside an outer eachChild, works.
    std::vector<Entity> nested;
    w.eachChild(p, [&](Entity child) { w.eachChild(child, [&](Entity grandchild) { nested.push_back(grandchild); }); });
    CHECK(nested == std::vector<Entity>{g1, g2});

    CHECK(collectChildren(w, g1).empty());  // a leaf
    CHECK(collectChildren(w, Entity{}).empty());
    const Entity dead = w.create();
    REQUIRE(w.destroy(dead));
    CHECK(collectChildren(w, dead).empty());

    CHECK(w.childCount(p) == collectChildren(w, p).size());
    CHECK(w.childCount(c1) == collectChildren(w, c1).size());
}

TEST_CASE("scene: setParent rejection matrix") {
    World w;
    const Entity p = w.create();
    const Entity c = w.create();
    REQUIRE(w.setParent(c, p));

    const Entity dead = w.create();
    REQUIRE(w.destroy(dead));

    // (a) null/dead child.
    CHECK_FALSE(w.setParent(Entity{}, p));
    // (b) dead child.
    CHECK_FALSE(w.setParent(dead, p));
    // (c) dead, NON-NULL parent -- distinct from a detach (parent == Entity{}, which succeeds).
    CHECK_FALSE(w.setParent(c, dead));
    CHECK(w.setParent(c, Entity{}));  // the detach case still succeeds
    REQUIRE(w.setParent(c, p));       // restore for the remaining checks
    // (d) self.
    CHECK_FALSE(w.setParent(c, c));
    // (e) direct cycle: p is c's own child? no -- construct one: make p a child of c first is illegal
    // (c is p's child, so c is already p's descendant) -- setParent(p, c) would close the cycle.
    CHECK_FALSE(w.setParent(p, c));

    // Nothing above changed the surviving graph.
    CHECK(w.parent(c) == p);
    CHECK(w.childCount(p) == 1);

    // (f) moved-from World.
    std::optional<World> mw;
    mw.emplace();
    const Entity mp = mw->create();
    const Entity mc = mw->create();
    const World movedTo = std::move(*mw);
    CHECK(movedTo.entityCount() == 2);  // the moved-TO World owns both entities
    CHECK_FALSE(mw->setParent(mc, mp));
}

TEST_CASE("scene: cycle rejection at depth") {
    World w;
    const std::vector<Entity> chain = makeChain(w, 4, Vec3::zero());  // a -> b -> c -> d
    const Entity a = chain[0];
    const Entity b = chain[1];
    const Entity c = chain[2];
    const Entity d = chain[3];

    CHECK_FALSE(w.setParent(a, d));  // a is an ancestor of d
    CHECK_FALSE(w.setParent(b, d));
    CHECK_FALSE(w.setParent(a, a));

    CHECK(w.parent(b) == a);
    CHECK(w.parent(c) == b);
    CHECK(w.parent(d) == c);
    CHECK(w.childCount(a) == 1);

    // d is NOT an ancestor of a, so reparenting a under d is legal.
    CHECK(w.setParent(d, a));
    CHECK(w.parent(d) == a);
}

TEST_CASE("scene: destroy destroys the whole subtree") {
    World w;
    const Entity root = w.create();
    const Entity m1 = w.create();
    const Entity m2 = w.create();
    const Entity l1 = w.create();
    const Entity l2 = w.create();
    REQUIRE(w.setParent(m1, root));
    REQUIRE(w.setParent(m2, root));
    REQUIRE(w.setParent(l1, m1));
    REQUIRE(w.setParent(l2, m1));
    CHECK(w.entityCount() == 5);

    CHECK(w.destroy(root));
    CHECK(w.entityCount() == 0);
    CHECK(!w.alive(root));
    CHECK(!w.alive(m1));
    CHECK(!w.alive(m2));
    CHECK(!w.alive(l1));
    CHECK(!w.alive(l2));

    // A sibling branch under a shared grandparent survives.
    const Entity g = w.create();
    const Entity root2 = w.create();
    const Entity sib = w.create();
    REQUIRE(w.setParent(root2, g));
    REQUIRE(w.setParent(sib, g));
    const Entity leaf = w.create();
    REQUIRE(w.setParent(leaf, root2));

    CHECK(w.destroy(root2));
    CHECK(w.alive(sib));
    CHECK(w.alive(g));
    CHECK(w.childCount(g) == 1);
    CHECK(collectChildren(w, g) == std::vector<Entity>{sib});

    CHECK_FALSE(w.destroy(root2));  // already destroyed
}

TEST_CASE("scene: destroy an interior node takes its branch only") {
    World w;
    const Entity root = w.create();
    const Entity a = w.create();
    const Entity b = w.create();
    const Entity c = w.create();
    const Entity b1 = w.create();
    const Entity b2 = w.create();
    REQUIRE(w.setParent(a, root));
    REQUIRE(w.setParent(b, root));
    REQUIRE(w.setParent(c, root));
    REQUIRE(w.setParent(b1, b));
    REQUIRE(w.setParent(b2, b));

    const std::size_t before = w.entityCount();
    CHECK(w.destroy(b));
    CHECK(w.entityCount() == before - 3U);
    CHECK(w.alive(a));
    CHECK(w.alive(c));
    CHECK(!w.alive(b));
    CHECK(!w.alive(b1));
    CHECK(!w.alive(b2));
    CHECK(collectChildren(w, root) == std::vector<Entity>{a, c});

    CHECK(w.destroy(a));
    CHECK(w.entityCount() == before - 4U);
    CHECK(collectChildren(w, root) == std::vector<Entity>{c});
}

TEST_CASE("scene: destroying a wide, deep subtree survives node relocation") {
    World w;
    const Entity g = w.create();
    const Entity root = w.create();
    const Entity keep = w.create();
    REQUIRE(w.setParent(root, g));
    REQUIRE(w.setParent(keep, g));

    const Entity a = w.create();
    const Entity b = w.create();
    const Entity c = w.create();
    REQUIRE(w.setParent(a, root));
    REQUIRE(w.setParent(b, root));
    REQUIRE(w.setParent(c, root));

    std::vector<Entity> grandchildren;
    for (const Entity mid : {a, b, c}) {
        for (int i = 0; i < 3; ++i) {
            const Entity leaf = w.create();
            REQUIRE(w.setParent(leaf, mid));
            grandchildren.push_back(leaf);
        }
    }
    REQUIRE(grandchildren.size() == 9U);

    const std::size_t before = w.entityCount();
    CHECK(w.destroy(root));
    CHECK(w.entityCount() == before - 13U);  // root + a,b,c + 9 grandchildren
    CHECK(!w.alive(root));
    CHECK(!w.alive(a));
    CHECK(!w.alive(b));
    CHECK(!w.alive(c));
    for (const Entity leaf : grandchildren) {
        CHECK(!w.alive(leaf));
    }
    CHECK(w.alive(keep));
    CHECK(w.alive(g));
    CHECK(w.childCount(g) == 1);
    CHECK(collectChildren(w, g) == std::vector<Entity>{keep});

    // The surviving graph is still usable.
    CHECK(w.setParent(keep, Entity{}));
    CHECK(w.setParent(keep, g));
    // No Transform anywhere in this subtree, so the answer is exactly identity -- the point is that
    // the walk is COMPUTABLE (no dangling-node UB) after the relocation-heavy destroy above.
    CHECK(engine::worldMatrix(w, keep) == Mat4::identity());
}

TEST_CASE("scene: subtree destroy runs every descendant's component destructors exactly once") {
    Counted::reset();
    {
        World w;
        registerComponent<Counted>(w, "test::Counted");
        const std::vector<Entity> chain = makeChain(w, 4, Vec3::zero());
        for (const Entity e : chain) {
            REQUIRE(w.add<Counted>(e, Counted{1}) != nullptr);
        }
        CHECK(Counted::live() == 4);
        CHECK(w.componentCount<Counted>() == 4);

        CHECK(w.destroy(chain.front()));
        CHECK(Counted::live() == 0);
        CHECK(w.componentCount<Counted>() == 0);

        for (int i = 0; i < 3; ++i) {
            const Entity e = w.create();
            REQUIRE(w.add<Counted>(e, Counted{2}) != nullptr);
        }
    }
    CHECK(Counted::live() == 0);
    CHECK(Counted::ctor == Counted::dtor);
}

TEST_CASE("scene: no stale parent is observable after destroy and slot recycling") {
    World w;
    const Entity p = w.create();
    const Entity c = w.create();
    REQUIRE(w.setParent(c, p));
    const auto pIndex = p.index;

    REQUIRE(w.destroy(p));  // takes c with it

    const Entity recycled = w.create();
    CHECK(recycled.index == pIndex);
    CHECK(recycled.generation == p.generation + 1U);

    CHECK(w.parent(recycled) == Entity{});
    CHECK(w.childCount(recycled) == 0);
    CHECK(collectChildren(w, recycled).empty());
    CHECK(w.parent(c) == Entity{});  // stale handle -> silent null

    CHECK_FALSE(w.setParent(c, recycled));  // c is dead
}

TEST_CASE("scene: clear resets the hierarchy and the World stays reusable") {
    World w;
    const Entity root = w.create();
    const Entity mid = w.create();
    const Entity leaf = w.create();
    REQUIRE(w.setParent(mid, root));
    REQUIRE(w.setParent(leaf, mid));

    const std::size_t typesBefore = w.componentTypeCount();
    w.clear();
    CHECK(w.entityCount() == 0);
    CHECK(!w.alive(root));
    CHECK(!w.alive(mid));
    CHECK(!w.alive(leaf));
    CHECK(w.parent(mid) == Entity{});
    CHECK(w.childCount(root) == 0);
    CHECK(w.componentTypeCount() == typesBefore);

    const Entity a = w.create();
    const Entity b = w.create();
    CHECK(w.setParent(b, a));
    CHECK(w.parent(b) == a);
    CHECK(w.childCount(a) == 1);
    CHECK(w.add<Transform>(a) != nullptr);  // no re-registration needed
}

TEST_CASE("transform: worldMatrix composes translation, rotation and scale chains") {
    World w;

    SUBCASE("translation") {
        const Entity root = w.create();
        const Entity child = w.create();
        const Entity gc = w.create();
        REQUIRE(w.add<Transform>(root, Transform{Vec3{1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.add<Transform>(child, Transform{Vec3{0.0F, 2.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.add<Transform>(gc, Transform{Vec3{0.0F, 0.0F, 3.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.setParent(child, root));
        REQUIRE(w.setParent(gc, child));

        CHECK(engine::approxEquals(engine::transformPoint(engine::worldMatrix(w, gc), Vec3::zero()),
                                   Vec3{1.0F, 2.0F, 3.0F}));
    }

    SUBCASE("rotation") {
        const Entity parent = w.create();
        const Entity child = w.create();
        REQUIRE(w.add<Transform>(parent,
                                 Transform{Vec3::zero(), engine::fromAxisAngle(Vec3::unitY(), engine::radians(90.0F)),
                                           Vec3::one()}) != nullptr);
        REQUIRE(w.add<Transform>(child, Transform{Vec3{1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.setParent(child, parent));

        CHECK(engine::approxEquals(engine::transformPoint(engine::worldMatrix(w, child), Vec3::zero()),
                                   Vec3{0.0F, 0.0F, -1.0F}));
    }

    SUBCASE("scale") {
        const Entity parent = w.create();
        const Entity child = w.create();
        REQUIRE(w.add<Transform>(parent, Transform{Vec3::zero(), Quat::identity(), Vec3{2.0F, 2.0F, 2.0F}}) != nullptr);
        REQUIRE(w.add<Transform>(child, Transform{Vec3{1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.setParent(child, parent));

        CHECK(engine::approxEquals(engine::transformPoint(engine::worldMatrix(w, child), Vec3::zero()),
                                   Vec3{2.0F, 0.0F, 0.0F}));
    }

    SUBCASE("full 3-deep TRS chain") {
        const Entity root = w.create();
        const Entity mid = w.create();
        const Entity leaf = w.create();
        const Transform rootT{Vec3{1.0F, 0.0F, 0.0F}, engine::fromAxisAngle(Vec3::unitY(), engine::radians(30.0F)),
                              Vec3{1.5F, 1.5F, 1.5F}};
        const Transform midT{Vec3{0.0F, 1.0F, 0.0F}, engine::fromAxisAngle(Vec3::unitX(), engine::radians(20.0F)),
                             Vec3::one()};
        const Transform leafT{Vec3{0.0F, 0.0F, 1.0F}, Quat::identity(), Vec3{0.5F, 0.5F, 0.5F}};
        REQUIRE(w.add<Transform>(root, rootT) != nullptr);
        REQUIRE(w.add<Transform>(mid, midT) != nullptr);
        REQUIRE(w.add<Transform>(leaf, leafT) != nullptr);
        REQUIRE(w.setParent(mid, root));
        REQUIRE(w.setParent(leaf, mid));

        const Mat4 expected = engine::localMatrix(rootT) * (engine::localMatrix(midT) * engine::localMatrix(leafT));
        CHECK(engine::approxEquals(engine::worldMatrix(w, leaf), expected));
    }

    SUBCASE("a root's worldMatrix equals its own localMatrix, exactly") {
        const Entity root = w.create();
        const Transform t{Vec3{3.0F, 4.0F, 5.0F}, engine::fromAxisAngle(Vec3::unitZ(), engine::radians(15.0F)),
                          Vec3{1.2F, 1.3F, 1.4F}};
        REQUIRE(w.add<Transform>(root, t) != nullptr);
        CHECK(engine::worldMatrix(w, root) == engine::localMatrix(t));
    }
}

TEST_CASE("transform: worldMatrix identity semantics — gaps, roots, missing transforms, dead handles") {
    World w;

    SUBCASE("a gap entity (parented, no Transform) passes its parent's matrix straight through") {
        const Entity root = w.create();
        const Entity gap = w.create();
        const Entity child = w.create();
        REQUIRE(w.add<Transform>(root, Transform{Vec3{1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.add<Transform>(child, Transform{Vec3{0.0F, 1.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.setParent(gap, root));
        REQUIRE(w.setParent(child, gap));

        CHECK(engine::approxEquals(engine::worldMatrix(w, gap), engine::worldMatrix(w, root)));
        CHECK(engine::approxEquals(engine::transformPoint(engine::worldMatrix(w, child), Vec3::zero()),
                                   Vec3{1.0F, 1.0F, 0.0F}));
    }

    SUBCASE("no Transform and no ancestors -> exactly identity") {
        const Entity e = w.create();
        CHECK(engine::worldMatrix(w, e) == Mat4::identity());
    }

    SUBCASE("null and dead handles -> exactly identity, silently") {
        const Entity dead = w.create();
        REQUIRE(w.destroy(dead));
        CHECK(engine::worldMatrix(w, Entity{}) == Mat4::identity());
        CHECK(engine::worldMatrix(w, dead) == Mat4::identity());
    }

    SUBCASE("moved-from World -> exactly identity") {
        std::optional<World> mw;
        mw.emplace();
        const Entity e = mw->create();
        const World movedTo = std::move(*mw);
        CHECK(movedTo.entityCount() == 1);  // the moved-TO World owns the entity
        CHECK(engine::worldMatrix(*mw, e) == Mat4::identity());
    }

    SUBCASE("reparenting changes the answer immediately") {
        const Entity p1 = w.create();
        const Entity p2 = w.create();
        const Entity child = w.create();
        REQUIRE(w.add<Transform>(p1, Transform{Vec3{1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.add<Transform>(p2, Transform{Vec3{5.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.add<Transform>(child, Transform{Vec3::zero(), Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.setParent(child, p1));

        const Mat4 under1 = engine::worldMatrix(w, child);
        REQUIRE(w.setParent(child, p2));
        const Mat4 under2 = engine::worldMatrix(w, child);
        CHECK_FALSE(engine::approxEquals(under1, under2));
        CHECK(engine::approxEquals(under2, engine::worldMatrix(w, p2)));
    }

    SUBCASE("remove<Transform> turns a node into a gap without touching its links") {
        const Entity root = w.create();
        const Entity child = w.create();
        REQUIRE(w.add<Transform>(child, Transform{}) != nullptr);
        REQUIRE(w.setParent(child, root));
        CHECK(w.remove<Transform>(child));
        CHECK(w.parent(child) == root);
        CHECK(w.childCount(root) == 1);
    }
}

TEST_CASE("transform: worldMatrix over a 1000-deep chain is iterative and exact") {
    World w;
    const std::vector<Entity> chain = makeChain(w, 1000, Vec3{1.0F, 0.0F, 0.0F});
    CHECK(engine::approxEquals(engine::transformPoint(engine::worldMatrix(w, chain.back()), Vec3::zero()),
                               Vec3{1000.0F, 0.0F, 0.0F}));

    CHECK(w.destroy(chain.front()));
    CHECK(w.entityCount() == 0);
}

TEST_CASE("transform: non-uniform parent scale composes as a plain matrix product") {
    World w;
    const Entity parent = w.create();
    const Entity child = w.create();
    const Transform parentT{Vec3::zero(), Quat::identity(), Vec3{2.0F, 1.0F, 1.0F}};
    const Transform childT{Vec3::zero(), engine::fromAxisAngle(Vec3::unitZ(), engine::radians(90.0F)), Vec3::one()};
    REQUIRE(w.add<Transform>(parent, parentT) != nullptr);
    REQUIRE(w.add<Transform>(child, childT) != nullptr);
    REQUIRE(w.setParent(child, parent));

    // The assertion IS the plain matrix product -- nothing more. The result carries SHEAR, inherent
    // to TRS hierarchies (Unity/Godot behave identically); decompose() of it is out of contract. The
    // engine validates nothing here.
    const Mat4 expected = engine::localMatrix(parentT) * engine::localMatrix(childT);
    CHECK(engine::approxEquals(engine::worldMatrix(w, child), expected));
}

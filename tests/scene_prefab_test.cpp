// tests/scene_prefab_test.cpp -- task 4.4.4: engine::Prefab, a saved entity tree.
// Tier-0 throughout: no GPU, no files, no reflection, so it runs identically with AERO_REFLECT_TOOLS=OFF.
// A prefab stored by hand (a World filled in the test, then fromWorld) is how a non-zero stored root position is
// reached, since capture always zeroes it.

#include <aero/core/guid.hpp>
#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/scene/internal/world_access.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using engine::Entity;
using engine::Guid;
using engine::Prefab;
using engine::Quat;
using engine::Transform;
using engine::Vec3;
using engine::World;

namespace {

constexpr float QUIET_NAN = std::numeric_limits<float>::quiet_NaN();
constexpr float PLUS_INF = std::numeric_limits<float>::infinity();

// Registered only where a case chooses to, so the far World lacks it.
struct PrefabOnlyTag {};

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

[[nodiscard]] Quat about(Vec3 axis, float degrees) { return engine::fromAxisAngle(axis, engine::radians(degrees)); }

Entity make(World& world, std::string_view name, Entity parent, std::optional<Transform> transform) {
    const Entity entity = world.create();
    if (!name.empty()) {
        world.setName(entity, name);
    }
    if (transform.has_value()) {
        world.add<Transform>(entity, *transform);
    }
    if (parent.valid()) {
        world.setParent(entity, parent);
    }
    return entity;
}

[[nodiscard]] Transform at(Vec3 position) {
    Transform transform;
    transform.position = position;
    return transform;
}

[[nodiscard]] std::vector<Entity> childrenOf(const World& world, Entity parent) {
    std::vector<Entity> out;
    world.eachChild(parent, [&out](Entity child) { out.push_back(child); });
    return out;
}

[[nodiscard]] std::string nameOf(const World& world, Entity entity) { return std::string(world.name(entity)); }

[[nodiscard]] Transform transformOf(const World& world, Entity entity) {
    const auto* transform = world.get<Transform>(entity);
    REQUIRE(transform != nullptr);
    return *transform;
}

[[nodiscard]] std::vector<Entity> allEntities(const World& world) {
    std::vector<Entity> out;
    world.eachEntity([&out](Entity entity) { out.push_back(entity); });
    return out;
}

// A three-level tree: Root(Mid(Leaf), Side), each with its own Transform.
struct Tree {
    Entity root;
    Entity mid;
    Entity leaf;
    Entity side;
};

[[nodiscard]] Tree makeTree(World& world, Transform rootTransform) {
    Tree tree;
    tree.root = make(world, "Root", Entity{}, rootTransform);
    Transform midTransform = at(Vec3{0.0F, 1.0F, 0.0F});
    midTransform.rotation = about(Vec3{0.0F, 0.0F, 1.0F}, 45.0F);
    tree.mid = make(world, "Mid", tree.root, midTransform);
    Transform leafTransform = at(Vec3{1.0F, 0.0F, 2.0F});
    leafTransform.scale = Vec3{1.5F, 1.5F, 1.5F};
    tree.leaf = make(world, "Leaf", tree.mid, leafTransform);
    tree.side = make(world, "Side", tree.root, at(Vec3{-3.0F, 0.0F, 0.0F}));
    return tree;
}

[[nodiscard]] Prefab captureOne(const World& world, Entity root) {
    const std::array<Entity, 1> roots{root};
    std::optional<Prefab> prefab = Prefab::capture(world, roots, "");
    REQUIRE(prefab.has_value());
    return std::move(*prefab);
}

// A prefab whose root keeps a chosen stored Transform: built in a World by hand, taken by fromWorld.
[[nodiscard]] Prefab storedByHand(Transform rootTransform) {
    World world;
    (void)makeTree(world, rootTransform);
    std::optional<Prefab> prefab = Prefab::fromWorld(std::move(world));
    REQUIRE(prefab.has_value());
    return std::move(*prefab);
}

// The position capture reads for a root, off the source World.
[[nodiscard]] Vec3 worldPositionOf(const World& world, Entity entity) {
    return engine::transformPoint(engine::worldMatrix(world, entity), Vec3{});
}

[[nodiscard]] Transform rotatedAndScaledPlacement() {
    Transform placement;
    placement.position = Vec3{1.0F, 2.0F, 3.0F};
    placement.rotation = about(Vec3{0.0F, 1.0F, 0.0F}, 90.0F);
    placement.scale = Vec3{2.0F, 2.0F, 2.0F};
    return placement;
}

}  // namespace

TEST_CASE("prefab: one captured root stores position zero and keeps rotation, scale, names, children (PW1)") {
    World source;
    Transform rootTransform = at(Vec3{5.0F, 6.0F, 7.0F});
    rootTransform.rotation = about(Vec3{0.0F, 1.0F, 0.0F}, 90.0F);
    rootTransform.scale = Vec3{2.0F, 2.0F, 2.0F};
    const Tree tree = makeTree(source, rootTransform);

    const Prefab prefab = captureOne(source, tree.root);
    const World& stored = prefab.world();

    CHECK(prefab.entityCount() == 4U);
    const Transform savedRoot = transformOf(stored, prefab.root());
    CHECK(savedRoot.position == Vec3{0.0F, 0.0F, 0.0F});
    CHECK(savedRoot.rotation == rootTransform.rotation);
    CHECK(savedRoot.scale == rootTransform.scale);
    CHECK(nameOf(stored, prefab.root()) == "Root");

    const std::vector<Entity> kids = childrenOf(stored, prefab.root());
    REQUIRE(kids.size() == 2U);
    CHECK(nameOf(stored, kids[0]) == "Mid");
    CHECK(nameOf(stored, kids[1]) == "Side");
    CHECK(transformOf(stored, kids[0]) == transformOf(source, tree.mid));
    CHECK(transformOf(stored, kids[1]) == transformOf(source, tree.side));
    const std::vector<Entity> grand = childrenOf(stored, kids[0]);
    REQUIRE(grand.size() == 1U);
    CHECK(nameOf(stored, grand[0]) == "Leaf");
    CHECK(transformOf(stored, grand[0]) == transformOf(source, tree.leaf));
}

TEST_CASE("prefab: a root under a transformed parent keeps its LOCAL rotation and scale (PW2)") {
    World source;
    Transform parentTransform = at(Vec3{10.0F, 0.0F, 0.0F});
    parentTransform.rotation = about(Vec3{0.0F, 1.0F, 0.0F}, 90.0F);
    parentTransform.scale = Vec3{2.0F, 2.0F, 2.0F};
    const Entity parent = make(source, "Parent", Entity{}, parentTransform);
    Transform local = at(Vec3{1.0F, 0.0F, 0.0F});
    local.rotation = about(Vec3{1.0F, 0.0F, 0.0F}, 90.0F);
    local.scale = Vec3{3.0F, 3.0F, 3.0F};
    const Entity child = make(source, "Child", parent, local);

    const Prefab prefab = captureOne(source, child);
    const Transform saved = transformOf(prefab.world(), prefab.root());
    CHECK(saved.position == Vec3{0.0F, 0.0F, 0.0F});
    CHECK(saved.rotation == local.rotation);
    CHECK(saved.scale == local.scale);
    CHECK(prefab.entityCount() == 1U);
}

TEST_CASE("prefab: several roots go under one identity wrapper at the mean (PW3)") {
    World source;
    const Entity first = make(source, "First", Entity{}, at(Vec3{2.0F, 0.0F, 0.0F}));
    const Entity firstChild = make(source, "FirstChild", first, at(Vec3{0.0F, 1.0F, 0.0F}));
    const Entity second = make(source, "Second", Entity{}, at(Vec3{4.0F, 2.0F, 0.0F}));
    const Entity secondChild = make(source, "SecondChild", second, at(Vec3{0.0F, 0.0F, 1.0F}));

    const std::array<Entity, 2> roots{first, second};
    const std::optional<Prefab> prefab = Prefab::capture(source, roots, "Group");
    REQUIRE(prefab.has_value());
    const World& stored = prefab->world();

    CHECK(prefab->entityCount() == 5U);
    const std::vector<Entity> everything = allEntities(stored);
    REQUIRE(!everything.empty());
    CHECK(everything.front() == prefab->root());  // the wrapper is created first
    CHECK(nameOf(stored, prefab->root()) == "Group");
    CHECK(transformOf(stored, prefab->root()) == Transform{});

    const std::vector<Entity> kids = childrenOf(stored, prefab->root());
    REQUIRE(kids.size() == 2U);
    CHECK(nameOf(stored, kids[0]) == "First");
    CHECK(nameOf(stored, kids[1]) == "Second");
    CHECK(engine::approxEquals(transformOf(stored, kids[0]).position, Vec3{-1.0F, -1.0F, 0.0F}));
    CHECK(engine::approxEquals(transformOf(stored, kids[1]).position, Vec3{1.0F, 1.0F, 0.0F}));
    CHECK(childrenOf(stored, kids[0]).size() == 1U);
    CHECK(transformOf(stored, childrenOf(stored, kids[0])[0]) == transformOf(source, firstChild));
    CHECK(transformOf(stored, childrenOf(stored, kids[1])[0]) == transformOf(source, secondChild));
}

TEST_CASE("prefab: a root under a translated parent contributes its WORLD position (PW4)") {
    World source;
    const Entity holder = make(source, "Holder", Entity{}, at(Vec3{10.0F, 0.0F, 0.0F}));
    const Entity underHolder = make(source, "Under", holder, at(Vec3{1.0F, 0.0F, 0.0F}));  // world x = 11
    const Entity loose = make(source, "Loose", Entity{}, at(Vec3{3.0F, 0.0F, 0.0F}));

    const std::array<Entity, 2> roots{underHolder, loose};
    const std::optional<Prefab> prefab = Prefab::capture(source, roots, "Pair");
    REQUIRE(prefab.has_value());
    const std::vector<Entity> kids = childrenOf(prefab->world(), prefab->root());
    REQUIRE(kids.size() == 2U);
    // The mean is x = 7: the local 1 would have given 2.
    CHECK(engine::approxEquals(transformOf(prefab->world(), kids[0]).position, Vec3{4.0F, 0.0F, 0.0F}));
    CHECK(engine::approxEquals(transformOf(prefab->world(), kids[1]).position, Vec3{-4.0F, 0.0F, 0.0F}));
}

TEST_CASE("prefab: components copy through the table, values intact, one WARN per absent type (PW5)") {
    World source;
    engine::scene::internal::registerComponent<PrefabOnlyTag>(source, "test::PrefabOnlyTag");
    const Tree tree = makeTree(source, at(Vec3{1.0F, 1.0F, 1.0F}));
    for (const Entity tagged : {tree.root, tree.mid, tree.leaf}) {
        source.add<PrefabOnlyTag>(tagged);
    }
    engine::MeshRenderer renderer;
    renderer.mesh = Guid{0x4400000000000001ULL, 0x11ULL};
    renderer.meshIndex = 3U;
    renderer.material = Guid{0x4400000000000001ULL, 0x22ULL};
    source.add<engine::MeshRenderer>(tree.mid, renderer);
    engine::ScriptComponent script;
    script.script = Guid{0x4400000000000001ULL, 0x33ULL};
    source.add<engine::ScriptComponent>(tree.leaf, script);

    const LogCount log;
    const Prefab prefab = captureOne(source, tree.root);
    CHECK(log.warnCount() == 1);
    CHECK(log.errorCount() == 0);

    const std::vector<Entity> kids = childrenOf(prefab.world(), prefab.root());
    REQUIRE(kids.size() == 2U);
    const auto* copiedRenderer = prefab.world().get<engine::MeshRenderer>(kids[0]);
    REQUIRE(copiedRenderer != nullptr);
    CHECK(*copiedRenderer == renderer);
    const Entity leaf = childrenOf(prefab.world(), kids[0])[0];
    const auto* copiedScript = prefab.world().get<engine::ScriptComponent>(leaf);
    REQUIRE(copiedScript != nullptr);
    CHECK(copiedScript->script == script.script);
    CHECK_FALSE(prefab.world().registered(engine::componentTypeId<PrefabOnlyTag>()));
}

TEST_CASE("prefab: capture refuses an empty span, a null root and a dead root with no record (PW6)") {
    World source;
    const Entity alive = make(source, "Alive", Entity{}, at(Vec3{1.0F, 0.0F, 0.0F}));
    const Entity dead = make(source, "Dead", Entity{}, at(Vec3{2.0F, 0.0F, 0.0F}));
    source.destroy(dead);

    const LogCount log;
    CHECK_FALSE(Prefab::capture(source, std::span<const Entity>{}, "").has_value());
    const std::array<Entity, 1> none{Entity{}};
    CHECK_FALSE(Prefab::capture(source, none, "").has_value());
    const std::array<Entity, 2> mixed{alive, dead};
    CHECK_FALSE(Prefab::capture(source, mixed, "Group").has_value());
    CHECK(log.warnCount() == 0);
    CHECK(log.errorCount() == 0);
    CHECK(source.entityCount() == 1U);
}

TEST_CASE("prefab: a Transform-less root stays so and still counts toward the pivot (PW7)") {
    World source;
    const Entity holder = make(source, "Holder", Entity{}, at(Vec3{5.0F, 0.0F, 0.0F}));
    const Entity bare = make(source, "Bare", holder, std::nullopt);  // world x = 5 through the holder
    const Entity placed = make(source, "Placed", Entity{}, at(Vec3{1.0F, 0.0F, 0.0F}));
    make(source, "BareChild", bare, at(Vec3{0.0F, 2.0F, 0.0F}));

    const Prefab single = captureOne(source, bare);
    CHECK_FALSE(single.world().has<Transform>(single.root()));
    CHECK(single.entityCount() == 2U);

    const std::array<Entity, 2> roots{bare, placed};
    const std::optional<Prefab> pair = Prefab::capture(source, roots, "Pair");
    REQUIRE(pair.has_value());
    const std::vector<Entity> kids = childrenOf(pair->world(), pair->root());
    REQUIRE(kids.size() == 2U);
    CHECK_FALSE(pair->world().has<Transform>(kids[0]));
    // The pivot is x = 3 only if the bare root counted: ignoring it would put "Placed" at 0.
    CHECK(engine::approxEquals(transformOf(pair->world(), kids[1]).position, Vec3{-2.0F, 0.0F, 0.0F}));
}

TEST_CASE("prefab: fromWorld takes exactly one root and refuses zero, two and a moved-from World (PW8)") {
    const LogCount log;
    CHECK_FALSE(Prefab::fromWorld(World{}).has_value());

    World two;
    make(two, "A", Entity{}, std::nullopt);
    make(two, "B", Entity{}, std::nullopt);
    CHECK_FALSE(Prefab::fromWorld(std::move(two)).has_value());

    std::optional<World> gone;
    gone.emplace();
    make(*gone, "Gone", Entity{}, std::nullopt);
    const World taken = std::move(*gone);
    CHECK_FALSE(Prefab::fromWorld(std::move(*gone)).has_value());
    CHECK(log.errorCount() == 0);
    CHECK(log.warnCount() == 0);

    World one;
    const Entity only = make(one, "Only", Entity{}, std::nullopt);
    make(one, "Kid", only, std::nullopt);
    const std::optional<Prefab> prefab = Prefab::fromWorld(std::move(one));
    REQUIRE(prefab.has_value());
    CHECK(prefab->root() == only);
    CHECK(prefab->entityCount() == 2U);
}

TEST_CASE("prefab: a child created before its parent is accepted and instantiates parent first (PW9)") {
    World world;
    const Entity child = world.create();
    world.setName(child, "Child");
    const Entity parent = world.create();
    world.setName(parent, "Parent");
    REQUIRE(world.setParent(child, parent));

    const std::optional<Prefab> prefab = Prefab::fromWorld(std::move(world));
    REQUIRE(prefab.has_value());
    CHECK(prefab->root() == parent);

    World destination;
    const Entity root = prefab->instantiate(destination, Transform{});
    REQUIRE(root.valid());
    const std::vector<Entity> everything = allEntities(destination);
    REQUIRE(everything.size() == 2U);
    CHECK(everything.front() == root);
    CHECK(nameOf(destination, root) == "Parent");
    const std::vector<Entity> kids = childrenOf(destination, root);
    REQUIRE(kids.size() == 1U);
    CHECK(nameOf(destination, kids[0]) == "Child");
}

TEST_CASE("prefab: an identity placement copies the tree into new handles and leaves the prefab alone (PW10)") {
    World source;
    Transform rootTransform = at(Vec3{4.0F, 4.0F, 4.0F});
    rootTransform.rotation = about(Vec3{1.0F, 0.0F, 0.0F}, 30.0F);
    rootTransform.scale = Vec3{0.5F, 2.0F, 1.0F};
    const Tree tree = makeTree(source, rootTransform);
    const Prefab prefab = captureOne(source, tree.root);
    const Transform stored = transformOf(prefab.world(), prefab.root());

    World destination;
    const Entity filler = destination.create();
    const Entity filler2 = destination.create();
    const Entity root = prefab.instantiate(destination, Transform{});
    REQUIRE(root.valid());

    CHECK(destination.entityCount() == 6U);
    CHECK(root != filler);
    CHECK(root != filler2);
    CHECK(transformOf(destination, root) == stored);
    CHECK(nameOf(destination, root) == "Root");
    const std::vector<Entity> kids = childrenOf(destination, root);
    REQUIRE(kids.size() == 2U);
    CHECK(nameOf(destination, kids[0]) == "Mid");
    CHECK(nameOf(destination, kids[1]) == "Side");
    const std::vector<Entity> grand = childrenOf(destination, kids[0]);
    REQUIRE(grand.size() == 1U);
    CHECK(nameOf(destination, grand[0]) == "Leaf");
    CHECK(transformOf(destination, grand[0]) == transformOf(source, tree.leaf));

    CHECK(prefab.entityCount() == 4U);
    CHECK(transformOf(prefab.world(), prefab.root()) == stored);
    CHECK(nameOf(prefab.world(), prefab.root()) == "Root");
}

TEST_CASE("prefab: the root is placement o stored: position, rotation and scale (PW11)") {
    Transform stored = at(Vec3{1.0F, 0.0F, 0.0F});
    stored.rotation = about(Vec3{1.0F, 0.0F, 0.0F}, 90.0F);
    stored.scale = Vec3{3.0F, 3.0F, 3.0F};
    const Prefab prefab = storedByHand(stored);
    const Transform placement = rotatedAndScaledPlacement();

    World destination;
    const Entity root = prefab.instantiate(destination, placement);
    REQUIRE(root.valid());
    const Transform placed = transformOf(destination, root);
    CHECK(engine::approxEquals(placed.position, Vec3{1.0F, 2.0F, 1.0F}));
    CHECK(engine::approxEquals(placed.rotation, placement.rotation * stored.rotation));
    CHECK_FALSE(engine::approxEquals(placed.rotation, stored.rotation * placement.rotation));
    CHECK(engine::approxEquals(placed.scale, Vec3{6.0F, 6.0F, 6.0F}));
}

TEST_CASE("prefab: under a uniform rotated placement the tree reads as parented under it (PW12)") {
    Transform stored = at(Vec3{1.0F, 0.0F, 0.0F});
    stored.rotation = about(Vec3{1.0F, 0.0F, 0.0F}, 90.0F);
    stored.scale = Vec3{3.0F, 3.0F, 3.0F};
    const Prefab prefab = storedByHand(stored);
    const Transform placement = rotatedAndScaledPlacement();

    World destination;
    const Entity root = prefab.instantiate(destination, placement);
    REQUIRE(root.valid());
    const Entity mid = childrenOf(destination, root)[0];
    const Entity leaf = childrenOf(destination, mid)[0];
    const World& saved = prefab.world();
    const Entity savedMid = childrenOf(saved, prefab.root())[0];
    const Entity savedLeaf = childrenOf(saved, savedMid)[0];
    const engine::Mat4 viaParts = engine::localMatrix(placement) * engine::localMatrix(stored) *
                                  engine::localMatrix(transformOf(saved, savedMid)) *
                                  engine::localMatrix(transformOf(saved, savedLeaf));
    CHECK(engine::approxEquals(engine::worldMatrix(destination, leaf), viaParts, 1e-4F));

    // A root saved at scale 0.2 reads 0.2 at placement scale 1: the root's own Transform is not replaced.
    Transform small;
    small.scale = Vec3{0.2F, 0.2F, 0.2F};
    const Prefab little = storedByHand(small);
    World other;
    const Entity littleRoot = little.instantiate(other, Transform{});
    CHECK(transformOf(other, littleRoot).scale == Vec3{0.2F, 0.2F, 0.2F});
}

TEST_CASE("prefab: the placement never reaches a child's Transform (PW13)") {
    Transform stored = at(Vec3{1.0F, 0.0F, 0.0F});
    stored.rotation = about(Vec3{0.0F, 0.0F, 1.0F}, 20.0F);
    const Prefab prefab = storedByHand(stored);

    World destination;
    const Entity root = prefab.instantiate(destination, rotatedAndScaledPlacement());
    REQUIRE(root.valid());
    const std::vector<Entity> from = childrenOf(prefab.world(), prefab.root());
    const std::vector<Entity> to = childrenOf(destination, root);
    REQUIRE(from.size() == to.size());
    for (std::size_t i = 0; i < from.size(); ++i) {
        CHECK(transformOf(destination, to[i]) == transformOf(prefab.world(), from[i]));
    }
    CHECK(transformOf(destination, childrenOf(destination, to[0])[0]) ==
          transformOf(prefab.world(), childrenOf(prefab.world(), from[0])[0]));
}

TEST_CASE("prefab: a stored root with no Transform takes exactly the placement (PW14)") {
    World world;
    const Entity bare = make(world, "Bare", Entity{}, std::nullopt);
    const Transform childTransform = at(Vec3{0.0F, 7.0F, 0.0F});
    make(world, "Kid", bare, childTransform);
    const std::optional<Prefab> prefab = Prefab::fromWorld(std::move(world));
    REQUIRE(prefab.has_value());

    const Transform placement = rotatedAndScaledPlacement();
    World destination;
    const Entity root = prefab->instantiate(destination, placement);
    REQUIRE(root.valid());
    CHECK(transformOf(destination, root) == placement);
    const std::vector<Entity> kids = childrenOf(destination, root);
    REQUIRE(kids.size() == 1U);
    CHECK(transformOf(destination, kids[0]) == childTransform);
}

TEST_CASE("prefab: a zero placement rotation acts as identity and a NaN one aborts nothing (PW15)") {
    Transform stored = at(Vec3{1.0F, 2.0F, 3.0F});
    stored.rotation = about(Vec3{0.0F, 1.0F, 0.0F}, 60.0F);
    const Prefab prefab = storedByHand(stored);

    Transform zero = at(Vec3{10.0F, 0.0F, 0.0F});
    zero.rotation = Quat{0.0F, 0.0F, 0.0F, 0.0F};
    World destination;
    const Entity flat = prefab.instantiate(destination, zero);
    REQUIRE(flat.valid());
    const Transform placed = transformOf(destination, flat);
    CHECK(placed.rotation == stored.rotation);
    CHECK(placed.position == Vec3{11.0F, 2.0F, 3.0F});

    Transform nan = at(Vec3{0.0F, 0.0F, 0.0F});
    const float notANumber = std::nanf("");
    nan.rotation = Quat{notANumber, notANumber, notANumber, notANumber};
    const Entity poisoned = prefab.instantiate(destination, nan);
    REQUIRE(poisoned.valid());
    const Quat result = transformOf(destination, poisoned).rotation;
    CHECK(std::isnan(result.x));
    CHECK(std::isnan(result.y));
    CHECK(std::isnan(result.z));
    CHECK(std::isnan(result.w));
    CHECK(transformOf(prefab.world(), prefab.root()) == stored);
}

TEST_CASE("prefab: a live parent takes the root last, a dead parent changes nothing (PW16)") {
    const Transform stored = at(Vec3{0.0F, 0.0F, 2.0F});
    const Prefab prefab = storedByHand(stored);

    World destination;
    const Entity holder = make(destination, "Holder", Entity{}, at(Vec3{10.0F, 0.0F, 0.0F}));
    const Entity existing = make(destination, "Existing", holder, std::nullopt);
    const Transform placement = at(Vec3{1.0F, 0.0F, 0.0F});
    const Entity root = prefab.instantiate(destination, placement, holder);
    REQUIRE(root.valid());
    CHECK(destination.parent(root) == holder);
    const std::vector<Entity> kids = childrenOf(destination, holder);
    REQUIRE(kids.size() == 2U);
    CHECK(kids[0] == existing);
    CHECK(kids[1] == root);
    const engine::Mat4 expected =
        engine::localMatrix(at(Vec3{10.0F, 0.0F, 0.0F})) * engine::localMatrix(transformOf(destination, root));
    CHECK(engine::approxEquals(engine::worldMatrix(destination, root), expected, 1e-4F));

    const Entity doomed = destination.create();
    destination.destroy(doomed);
    const std::size_t before = destination.entityCount();
    const LogCount log;
    CHECK_FALSE(prefab.instantiate(destination, placement, doomed).valid());
    CHECK(destination.entityCount() == before);
    CHECK(log.errorCount() == 0);
    CHECK(log.warnCount() == 0);
}

TEST_CASE("prefab: two instances share nothing with each other or the prefab (PW17)") {
    const Prefab prefab = storedByHand(at(Vec3{1.0F, 0.0F, 0.0F}));
    World first;
    World second;
    const Entity a = prefab.instantiate(first, Transform{});
    const Entity b = prefab.instantiate(second, Transform{});
    const Entity c = prefab.instantiate(first, Transform{});
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    REQUIRE(c.valid());

    first.setName(a, "Renamed");
    first.get<Transform>(a)->position = Vec3{9.0F, 9.0F, 9.0F};
    CHECK(nameOf(first, c) == "Root");
    CHECK(transformOf(first, c).position == Vec3{1.0F, 0.0F, 0.0F});
    CHECK(nameOf(second, b) == "Root");
    CHECK(transformOf(second, b).position == Vec3{1.0F, 0.0F, 0.0F});
    CHECK(nameOf(prefab.world(), prefab.root()) == "Root");
    CHECK(transformOf(prefab.world(), prefab.root()).position == Vec3{1.0F, 0.0F, 0.0F});
}

TEST_CASE("prefab: a moved-from destination yields a null Entity and one ERROR (PW18)") {
    const Prefab prefab = storedByHand(Transform{});
    std::optional<World> destination;
    destination.emplace();
    const World taken = std::move(*destination);

    const LogCount log;
    CHECK_FALSE(prefab.instantiate(*destination, Transform{}).valid());
    CHECK(log.errorCount() == 1);
    CHECK(log.warnCount() == 0);
    CHECK(taken.entityCount() == 0U);
}

TEST_CASE("prefab: one root at a NaN position stores the literal zero and keeps its rotation and scale (PW19)") {
    World source;
    const Quat turned = about(Vec3{0.0F, 1.0F, 0.0F}, 90.0F);
    const Vec3 stretched{2.0F, 3.0F, 4.0F};
    Transform rootTransform = at(Vec3{QUIET_NAN, 1.0F, 2.0F});
    rootTransform.rotation = turned;
    rootTransform.scale = stretched;
    const Entity root = make(source, "Root", Entity{}, rootTransform);

    const Prefab prefab = captureOne(source, root);
    const Transform saved = transformOf(prefab.world(), prefab.root());
    CHECK(saved.position == Vec3{});  // y and z are finite in the source and still do not survive: the whole zero
    CHECK(saved.rotation == turned);
    CHECK(saved.scale == stretched);
}

TEST_CASE("prefab: one root at an infinite position stores the literal zero, either sign, any axis (PW20)") {
    World source;
    const Entity positive = make(source, "Positive", Entity{}, at(Vec3{PLUS_INF, 0.0F, 0.0F}));
    const Entity negative = make(source, "Negative", Entity{}, at(Vec3{0.0F, -PLUS_INF, 0.0F}));

    const Prefab first = captureOne(source, positive);
    CHECK(transformOf(first.world(), first.root()).position == Vec3{});
    const Prefab second = captureOne(source, negative);
    CHECK(transformOf(second.world(), second.root()).position == Vec3{});
}

TEST_CASE("prefab: a root non-finite on one axis leaves the mean of the others whole and stores zero (PW21)") {
    World source;
    const Entity nanRoot = make(source, "NanRoot", Entity{}, at(Vec3{100.0F, QUIET_NAN, 0.0F}));
    const Entity first = make(source, "First", Entity{}, at(Vec3{2.0F, 0.0F, 0.0F}));
    const Entity infRoot = make(source, "InfRoot", Entity{}, at(Vec3{PLUS_INF, 100.0F, 0.0F}));
    const Entity second = make(source, "Second", Entity{}, at(Vec3{4.0F, 2.0F, 0.0F}));

    const std::array<Entity, 4> roots{nanRoot, first, infRoot, second};
    const std::optional<Prefab> prefab = Prefab::capture(source, roots, "Group");
    REQUIRE(prefab.has_value());
    const World& stored = prefab->world();

    CHECK(transformOf(stored, prefab->root()) == Transform{});
    const std::vector<Entity> kids = childrenOf(stored, prefab->root());
    REQUIRE(kids.size() == 4U);
    // The mean is (3, 1, 0): the x = 100 and y = 100 of the two left out reach no one.
    CHECK(transformOf(stored, kids[0]).position == Vec3{});
    CHECK(transformOf(stored, kids[1]).position == Vec3{-1.0F, -1.0F, 0.0F});
    CHECK(transformOf(stored, kids[2]).position == Vec3{});
    CHECK(transformOf(stored, kids[3]).position == Vec3{1.0F, 1.0F, 0.0F});
}

TEST_CASE("prefab: with every root non-finite the capture succeeds and each child stores zero (PW22)") {
    World source;
    const Entity first = make(source, "First", Entity{}, at(Vec3{QUIET_NAN, 0.0F, 0.0F}));
    const Entity second = make(source, "Second", Entity{}, at(Vec3{0.0F, PLUS_INF, 0.0F}));
    const Entity third = make(source, "Third", Entity{}, at(Vec3{0.0F, 0.0F, -PLUS_INF}));

    const std::array<Entity, 3> roots{first, second, third};
    const std::optional<Prefab> prefab = Prefab::capture(source, roots, "Group");
    REQUIRE(prefab.has_value());
    const World& stored = prefab->world();

    CHECK(transformOf(stored, prefab->root()) == Transform{});
    const std::vector<Entity> kids = childrenOf(stored, prefab->root());
    REQUIRE(kids.size() == 3U);
    for (const Entity kid : kids) {
        CHECK(transformOf(stored, kid).position == Vec3{});
    }
}

TEST_CASE("prefab: finite roots whose float sum overflows still centre on their exact mean (PW23)") {
    World source;
    const Entity first = make(source, "First", Entity{}, at(Vec3{0x3p126F, 0.0F, 0.0F}));
    const Entity second = make(source, "Second", Entity{}, at(Vec3{0x2p126F, 0.0F, 0.0F}));

    const std::array<Entity, 2> roots{first, second};
    const std::optional<Prefab> prefab = Prefab::capture(source, roots, "Group");
    REQUIRE(prefab.has_value());
    const std::vector<Entity> kids = childrenOf(prefab->world(), prefab->root());
    REQUIRE(kids.size() == 2U);
    CHECK(transformOf(prefab->world(), kids[0]).position == Vec3{0x1p125F, 0.0F, 0.0F});
    CHECK(transformOf(prefab->world(), kids[1]).position == Vec3{-0x1p125F, 0.0F, 0.0F});
}

TEST_CASE("prefab: a root whose offset passes FLT_MAX stores zero on every axis, its siblings keep theirs (PW24)") {
    World source;
    const Entity first = make(source, "First", Entity{}, at(Vec3{0x3p126F, 1.0F, 0.0F}));
    const Entity second = make(source, "Second", Entity{}, at(Vec3{0x3p126F, 1.0F, 0.0F}));
    const Entity third = make(source, "Third", Entity{}, at(Vec3{-0x3p126F, 4.0F, 0.0F}));

    const std::array<Entity, 3> roots{first, second, third};
    const std::optional<Prefab> prefab = Prefab::capture(source, roots, "Group");
    REQUIRE(prefab.has_value());
    const std::vector<Entity> kids = childrenOf(prefab->world(), prefab->root());
    REQUIRE(kids.size() == 3U);
    // The mean is (0x1p126, 2, 0). The third's x would be -0x1p128, past FLT_MAX; its finite y of 2 goes with it.
    CHECK(transformOf(prefab->world(), kids[0]).position == Vec3{0x1p127F, -1.0F, 0.0F});
    CHECK(transformOf(prefab->world(), kids[1]).position == Vec3{0x1p127F, -1.0F, 0.0F});
    CHECK(transformOf(prefab->world(), kids[2]).position == Vec3{});
}

TEST_CASE("prefab: the finiteness test reads the WORLD position, Transform or not (PW25)") {
    World source;
    Transform huge;
    huge.scale = Vec3{1e30F, 1e30F, 1e30F};
    const Entity hugeParent = make(source, "HugeParent", Entity{}, huge);
    const Entity distant = make(source, "Distant", hugeParent, at(Vec3{1e30F, 0.0F, 0.0F}));
    const Entity nanParent = make(source, "NanParent", Entity{}, at(Vec3{QUIET_NAN, 0.0F, 0.0F}));
    const Entity bare = make(source, "Bare", nanParent, std::nullopt);
    const Entity first = make(source, "First", Entity{}, at(Vec3{2.0F, 0.0F, 0.0F}));
    const Entity second = make(source, "Second", Entity{}, at(Vec3{4.0F, 0.0F, 0.0F}));

    // Each sample is what the case claims: a finite local position at +inf in the world, a bare root at NaN.
    REQUIRE(worldPositionOf(source, distant) == Vec3{PLUS_INF, 0.0F, 0.0F});
    REQUIRE(std::isnan(worldPositionOf(source, bare).x));

    const std::array<Entity, 4> roots{distant, bare, first, second};
    const std::optional<Prefab> prefab = Prefab::capture(source, roots, "Group");
    REQUIRE(prefab.has_value());
    const World& stored = prefab->world();
    const std::vector<Entity> kids = childrenOf(stored, prefab->root());
    REQUIRE(kids.size() == 4U);
    CHECK(transformOf(stored, kids[0]).position == Vec3{});
    CHECK_FALSE(stored.has<Transform>(kids[1]));
    CHECK(transformOf(stored, kids[2]).position == Vec3{-1.0F, 0.0F, 0.0F});
    CHECK(transformOf(stored, kids[3]).position == Vec3{1.0F, 0.0F, 0.0F});
}

TEST_CASE("prefab: capture fixes the positions it computes, never a value it only copies (PW26)") {
    World source;
    const Entity first = make(source, "First", Entity{}, at(Vec3{2.0F, 0.0F, 0.0F}));
    make(source, "Child", first, at(Vec3{QUIET_NAN, 0.0F, 0.0F}));
    const Entity second = make(source, "Second", Entity{}, at(Vec3{4.0F, 0.0F, 0.0F}));
    Transform stretched = at(Vec3{9.0F, 0.0F, 0.0F});
    stretched.scale = Vec3{QUIET_NAN, 1.0F, 1.0F};
    const Entity third = make(source, "Third", Entity{}, stretched);

    // A NaN scale puts the third out through its world position, though its local x of 9 is finite.
    REQUIRE(std::isnan(worldPositionOf(source, third).x));

    const std::array<Entity, 3> roots{first, second, third};
    const std::optional<Prefab> prefab = Prefab::capture(source, roots, "Group");
    REQUIRE(prefab.has_value());
    const World& stored = prefab->world();
    const std::vector<Entity> kids = childrenOf(stored, prefab->root());
    REQUIRE(kids.size() == 3U);
    CHECK(transformOf(stored, kids[0]).position == Vec3{-1.0F, 0.0F, 0.0F});
    CHECK(transformOf(stored, kids[1]).position == Vec3{1.0F, 0.0F, 0.0F});
    CHECK(transformOf(stored, kids[2]).position == Vec3{});

    const std::vector<Entity> grand = childrenOf(stored, kids[0]);
    REQUIRE(grand.size() == 1U);
    CHECK(std::isnan(transformOf(stored, grand[0]).position.x));
    CHECK(std::isnan(transformOf(stored, kids[2]).scale.x));
}

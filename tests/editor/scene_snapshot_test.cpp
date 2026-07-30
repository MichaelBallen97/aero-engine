// tests/editor/scene_snapshot_test.cpp -- task 2.4.2: SubtreeSnapshot / ComponentSnapshot's tier-0
// battery, plus RL1's ERROR accounting for World::recreate. Thirteenth TU of aero_editor_shell_test,
// which supplies main() from shell_test.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
// Tier-0/ungated, and REFLECTION-FREE (AC-8): no line below names entt::meta or reflect-gen, so this
// TU must pass identically under -DAERO_REFLECT_TOOLS=OFF.
#include <aero/core/log.hpp>
#include <aero/editor/console_model.hpp>
#include <aero/editor/scene_snapshot.hpp>
#include <aero/scene/internal/world_access.hpp>  // registerComponent<T> -- N6/N11/N12's probe fixtures
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using engine::Camera;
using engine::ComponentTypeId;
using engine::DirectionalLight;
using engine::Entity;
using engine::MeshRenderer;
using engine::PointLight;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::editor::ComponentSnapshot;
using engine::editor::SubtreeSnapshot;
using engine::scene::internal::registerComponent;

namespace {

// File-local copies, per the established one-per-TU rule (command_stack_test.cpp's own comment).
[[nodiscard]] std::size_t countAtLevel(const std::vector<engine::editor::LogEntry>& records, engine::LogLevel level) {
    return static_cast<std::size_t>(std::count_if(
        records.begin(), records.end(), [level](const engine::editor::LogEntry& e) { return e.level == level; }));
}

struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;
};

// N6/N11's probe: a genuinely EMPTY (tag) component type no built-in World registration ever knows.
// N12's probe: a FIELDED type, for the same reason -- the WARN-dedup case wants a real payload.
struct TagProbe {};
struct FieldedProbe {
    int value = 0;
};

}  // namespace

TEST_CASE("scene_snapshot: single-entity round trip (N1/AC-5/AC-6)") {
    World world;
    const Entity e = world.create();
    REQUIRE(world.setName(e, "Widget"));
    const Transform t{.position = Vec3{1.0F, 2.0F, 3.0F},
                      .rotation = engine::fromAxisAngle(Vec3::unitY(), engine::radians(30.0F)),
                      .scale = Vec3{2.0F, 3.0F, 4.0F}};
    REQUIRE(world.add<Transform>(e, t) != nullptr);

    SubtreeSnapshot snap;
    REQUIRE(snap.capture(world, std::vector<Entity>{e}));
    REQUIRE(world.destroy(e));
    REQUIRE_FALSE(world.alive(e));

    REQUIRE(snap.restore(world));
    CHECK(world.alive(e));
    CHECK(world.name(e) == "Widget");
    REQUIRE(world.has<Transform>(e));
    CHECK(*world.get<Transform>(e) == t);

    std::size_t visits = 0;
    world.eachEntity([&](Entity visited) {
        if (visited == e) {
            ++visits;
        }
    });
    CHECK(visits == 1);
}

TEST_CASE("scene_snapshot: a three-level subtree preserves every link and child order (N2/A2)") {
    World world;
    const Entity root = world.create();
    const Entity c0 = world.create();
    const Entity c1 = world.create();
    const Entity c2 = world.create();
    REQUIRE(world.setParent(c0, root));
    REQUIRE(world.setParent(c1, root));
    REQUIRE(world.setParent(c2, root));
    const Entity g0 = world.create();
    const Entity g1 = world.create();
    REQUIRE(world.setParent(g0, c1));
    REQUIRE(world.setParent(g1, c1));

    SubtreeSnapshot snap;
    REQUIRE(snap.capture(world, std::vector<Entity>{root}));
    REQUIRE(world.destroy(root));
    REQUIRE(snap.restore(world));

    CHECK(world.alive(root));
    std::vector<Entity> rootChildren;
    world.eachChild(root, [&rootChildren](Entity c) { rootChildren.push_back(c); });
    REQUIRE(rootChildren.size() == 3);
    CHECK(rootChildren[0] == c0);
    CHECK(rootChildren[1] == c1);
    CHECK(rootChildren[2] == c2);

    std::vector<Entity> grandchildren;
    world.eachChild(c1, [&grandchildren](Entity c) { grandchildren.push_back(c); });
    REQUIRE(grandchildren.size() == 2);
    CHECK(grandchildren[0] == g0);
    CHECK(grandchildren[1] == g1);
}

TEST_CASE("scene_snapshot: a captured middle child restores to its own sibling position (N3/S3)") {
    World world;
    const Entity parent = world.create();
    std::vector<Entity> kids;
    for (int i = 0; i < 5; ++i) {
        const Entity k = world.create();
        REQUIRE(world.setParent(k, parent));
        kids.push_back(k);
    }
    const Entity middle = kids[2];

    SubtreeSnapshot snap;
    REQUIRE(snap.capture(world, std::vector<Entity>{middle}));
    REQUIRE(world.destroy(middle));

    std::vector<Entity> remaining;
    world.eachChild(parent, [&remaining](Entity c) { remaining.push_back(c); });
    REQUIRE(remaining.size() == 4);

    REQUIRE(snap.restore(world));
    std::vector<Entity> after;
    world.eachChild(parent, [&after](Entity c) { after.push_back(c); });
    REQUIRE(after.size() == 5);
    CHECK(after[0] == kids[0]);
    CHECK(after[1] == kids[1]);
    CHECK(after[2] == middle);  // back at index 2, not appended at 4
    CHECK(after[3] == kids[3]);
    CHECK(after[4] == kids[4]);
}

// N14 -- code-review Gap 1 (task 2.4.2). N3 alone cannot catch this: it captures/restores exactly
// ONE entity per parent, so the position-restore pass never has a second sibling to corrupt.
// Capturing TWO siblings of one parent in DESCENDING slot order -- exactly what Ctrl-clicking the
// lower one before the higher one produces -- and restoring them naively (one placeAt call per
// record, in capture order) leaves an UNTOUCHED sibling shifted: parking the higher slot first
// appends past where the lower slot's later placeAt call expects the list to still be. Asserted
// element-wise over the WHOLE list, not just the two restored entries, because the untouched
// sibling is exactly what silently moves.
TEST_CASE(
    "scene_snapshot: two captured siblings of one parent restore in slot order regardless of "
    "capture order (N14/code-review Gap 1)") {
    World world;
    const Entity parent = world.create();
    std::vector<Entity> kids;
    for (int i = 0; i < 4; ++i) {
        const Entity k = world.create();
        REQUIRE(world.setParent(k, parent));
        kids.push_back(k);
    }
    const Entity a = kids[0];
    const Entity b = kids[1];
    const Entity c = kids[2];
    const Entity d = kids[3];

    // Captured DESCENDING by slot (c is slot 2, b is slot 1) -- the input order a naive per-record
    // placeAt pass would replay verbatim.
    SubtreeSnapshot snap;
    REQUIRE(snap.capture(world, std::vector<Entity>{c, b}));
    REQUIRE(world.destroy(c));
    REQUIRE(world.destroy(b));

    std::vector<Entity> remaining;
    world.eachChild(parent, [&remaining](Entity ch) { remaining.push_back(ch); });
    REQUIRE(remaining.size() == 2);  // a, d

    REQUIRE(snap.restore(world));
    std::vector<Entity> after;
    world.eachChild(parent, [&after](Entity ch) { after.push_back(ch); });
    REQUIRE(after.size() == 4);
    CHECK(after[0] == a);
    CHECK(after[1] == b);
    CHECK(after[2] == c);
    CHECK(after[3] == d);  // the UNTOUCHED sibling -- this is what a wrong replay order moves
}

// N15 -- N14's ASCENDING mirror, added by a SECOND code-review round (task 2.4.2) that found N14's
// fix (the first review round's own fix) was itself wrong: it split the position-restore pass into
// an append-every-subtree-root pass followed by a SEPARATE sorted placeAt pass, which only keeps
// `child == scratch.back()` (placeAt's own precondition) for the LAST subtree root appended -- every
// earlier one silently no-ops. Descending capture (N14) happens to still come back correct under
// that split; ASCENDING capture -- a shift-click range or a top-down Ctrl-click, the ORDINARY
// gesture -- does not. Same assertions as N14, capture order reversed, so the two together prove
// the fix is order-INDEPENDENT rather than merely fixing the one order N14 happens to name.
TEST_CASE(
    "scene_snapshot: two captured siblings of one parent restore in slot order in ASCENDING "
    "capture order too (N15/second code-review round)") {
    World world;
    const Entity parent = world.create();
    std::vector<Entity> kids;
    for (int i = 0; i < 4; ++i) {
        const Entity k = world.create();
        REQUIRE(world.setParent(k, parent));
        kids.push_back(k);
    }
    const Entity a = kids[0];
    const Entity b = kids[1];
    const Entity c = kids[2];
    const Entity d = kids[3];

    // Captured ASCENDING by slot (b is slot 1, c is slot 2) -- the ordinary top-down multi-select.
    SubtreeSnapshot snap;
    REQUIRE(snap.capture(world, std::vector<Entity>{b, c}));
    REQUIRE(world.destroy(b));
    REQUIRE(world.destroy(c));

    std::vector<Entity> remaining;
    world.eachChild(parent, [&remaining](Entity ch) { remaining.push_back(ch); });
    REQUIRE(remaining.size() == 2);  // a, d

    REQUIRE(snap.restore(world));
    std::vector<Entity> after;
    world.eachChild(parent, [&after](Entity ch) { after.push_back(ch); });
    REQUIRE(after.size() == 4);
    CHECK(after[0] == a);
    CHECK(after[1] == b);
    CHECK(after[2] == c);
    CHECK(after[3] == d);  // the UNTOUCHED sibling -- this is what a wrong replay order moves
}

// N16 -- a THREE-sibling case with a MIXED capture order, added alongside N15: a two-element case
// cannot distinguish "restored in the order they were sorted" from "restored in the order they were
// reversed" (both look identical for exactly two entries). Capturing {c,b,d} of five siblings
// [a,b,c,d,e] is neither ascending nor descending in capture order, so only a genuinely correct
// ascending-by-slot replay reproduces the original list.
TEST_CASE(
    "scene_snapshot: three captured siblings of one parent restore in slot order under a MIXED "
    "capture order (N16)") {
    World world;
    const Entity parent = world.create();
    std::vector<Entity> kids;
    for (int i = 0; i < 5; ++i) {
        const Entity k = world.create();
        REQUIRE(world.setParent(k, parent));
        kids.push_back(k);
    }
    const Entity a = kids[0];
    const Entity b = kids[1];
    const Entity c = kids[2];
    const Entity d = kids[3];
    const Entity e = kids[4];

    // Captured MIXED by slot: c (slot 2), then b (slot 1), then d (slot 3).
    SubtreeSnapshot snap;
    REQUIRE(snap.capture(world, std::vector<Entity>{c, b, d}));
    REQUIRE(world.destroy(c));
    REQUIRE(world.destroy(b));
    REQUIRE(world.destroy(d));

    std::vector<Entity> remaining;
    world.eachChild(parent, [&remaining](Entity ch) { remaining.push_back(ch); });
    REQUIRE(remaining.size() == 2);  // a, e

    REQUIRE(snap.restore(world));
    std::vector<Entity> after;
    world.eachChild(parent, [&after](Entity ch) { after.push_back(ch); });
    REQUIRE(after.size() == 5);
    CHECK(after[0] == a);
    CHECK(after[1] == b);
    CHECK(after[2] == c);
    CHECK(after[3] == d);
    CHECK(after[4] == e);  // the UNTOUCHED sibling -- this is what a wrong replay order moves
}

TEST_CASE("scene_snapshot: topMost collapses a parent captured with its own child (N4/S17)") {
    World world;
    const Entity parent = world.create();
    const Entity child = world.create();
    REQUIRE(world.setParent(child, parent));

    SubtreeSnapshot snap;
    REQUIRE(snap.capture(world, std::vector<Entity>{parent, child}));
    CHECK(snap.roots().size() == 1);
    CHECK(snap.roots()[0] == parent);
    CHECK(snap.entityCount() == 2);  // parent + child, each counted once

    REQUIRE(world.destroy(parent));
    REQUIRE(snap.restore(world));
    CHECK(world.alive(parent));
    CHECK(world.alive(child));
    CHECK(world.parent(child) == parent);
}

TEST_CASE("scene_snapshot: every built-in component round-trips value-equal (N5)") {
    World world;
    const Entity e = world.create();
    const Transform t{.position = Vec3{1.0F, 0.0F, 0.0F}, .scale = Vec3{2.0F, 2.0F, 2.0F}};
    const Camera cam{.fovYRadians = 1.0F, .nearPlane = 0.5F, .farPlane = 50.0F};
    const DirectionalLight dl{.color = Vec3{1.0F, 0.5F, 0.25F}, .intensity = 2.5F};
    const PointLight pl{.color = Vec3{0.1F, 0.2F, 0.3F}, .intensity = 3.0F, .range = 20.0F};
    const MeshRenderer mr{.primitive = 1U, .color = Vec3{0.9F, 0.8F, 0.7F}};
    REQUIRE(world.add<Transform>(e, t) != nullptr);
    REQUIRE(world.add<Camera>(e, cam) != nullptr);
    REQUIRE(world.add<DirectionalLight>(e, dl) != nullptr);
    REQUIRE(world.add<PointLight>(e, pl) != nullptr);
    REQUIRE(world.add<MeshRenderer>(e, mr) != nullptr);

    SubtreeSnapshot snap;
    REQUIRE(snap.capture(world, std::vector<Entity>{e}));
    REQUIRE(world.destroy(e));
    REQUIRE(snap.restore(world));

    REQUIRE(world.has<Transform>(e));
    CHECK(*world.get<Transform>(e) == t);
    REQUIRE(world.has<Camera>(e));
    CHECK(*world.get<Camera>(e) == cam);
    REQUIRE(world.has<DirectionalLight>(e));
    CHECK(*world.get<DirectionalLight>(e) == dl);
    REQUIRE(world.has<PointLight>(e));
    CHECK(*world.get<PointLight>(e) == pl);
    REQUIRE(world.has<MeshRenderer>(e));
    CHECK(*world.get<MeshRenderer>(e) == mr);
}

// N6 (AC-10) literally asks for "a registered empty fixture type" whose PRESENCE survives a
// SubtreeSnapshot round trip WARN-free. That is only possible for a type BOTH `world` and the
// snapshot's PRIVATE World know -- and the private World is registration-complete (F10) for EXACTLY
// the five engine built-ins (verified by reading transform.hpp/camera.hpp/light.hpp/
// mesh_renderer.hpp: is_empty_v is false for all five), none of which is a tag. D24 itself records
// this: "F10 makes this unreachable today. It becomes reachable the day project-defined components
// arrive (H3)." So a genuinely-present, non-built-in tag DEGRADES exactly like N12's fielded probe --
// one WARN, dropped -- rather than round-tripping. This case proves that degrade is SAFE specifically
// for the TAG (zero-payload) shape N12 does not exercise, and that a sibling entity without the tag
// is entirely unaffected. AC-10's full promise (present survives WARN-free) is deferred to H3.
TEST_CASE("scene_snapshot: an unmirrorable TAG degrades to absent with one WARN; siblings unaffected (N6/D24/H3)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    World world;
    const ComponentTypeId tagId = registerComponent<TagProbe>(world, "N6.TagProbe");
    REQUIRE(tagId.valid());
    const Entity root = world.create();
    const Entity withTag = world.create();
    const Entity withoutTag = world.create();
    REQUIRE(world.setParent(withTag, root));
    REQUIRE(world.setParent(withoutTag, root));
    world.add<TagProbe>(withTag);  // a tag's add<T> returns nullptr on success too (E13) -- hasRaw next
    CHECK(world.has<TagProbe>(withTag));
    CHECK_FALSE(world.has<TagProbe>(withoutTag));

    SubtreeSnapshot snap;
    scope.sink()->take(records);
    records.clear();  // LogSink::take requires `out` empty on entry
    REQUIRE(snap.capture(world, std::vector<Entity>{root}));
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 1);  // exactly one, for withTag's TagProbe

    REQUIRE(world.destroy(root));
    REQUIRE(snap.restore(world));
    CHECK(world.alive(withTag));
    CHECK(world.alive(withoutTag));
    CHECK_FALSE(world.has<TagProbe>(withTag));     // dropped -- D24's degrade, not a round trip
    CHECK_FALSE(world.has<TagProbe>(withoutTag));  // never had it; absent stays absent
}

TEST_CASE("scene_snapshot: restore is atomic on failure -- nothing survives a blocked recreate (N7/AC-7/S6)") {
    World world;
    const Entity a = world.create();
    const Entity b = world.create();
    REQUIRE(world.setParent(b, a));

    SubtreeSnapshot snap;
    REQUIRE(snap.capture(world, std::vector<Entity>{a}));
    REQUIRE(world.destroy(a));  // both a and b die (subtree destroy)
    CHECK_FALSE(world.alive(a));
    CHECK_FALSE(world.alive(b));

    // Pre-occupy a's slot: create() until it recycles a's index, at a NEW generation. `occupant`
    // starts invalid (generation 0), which never equals a LIVE entity's index/generation pairing on
    // its own -- but `a.index` can legitimately be 0, so the loop condition compares INDEX after each
    // draw, not before the first one (a do-while, not a for-with-a-pre-check).
    Entity occupant{};
    for (int i = 0; i < 8; ++i) {
        occupant = world.create();
        if (occupant.index == a.index) {
            break;
        }
    }
    REQUIRE(occupant.index == a.index);
    REQUIRE(occupant.generation != a.generation);

    CHECK_FALSE(snap.restore(world));
    CHECK_FALSE(world.alive(a));
    CHECK_FALSE(world.alive(b));
    CHECK(world.alive(occupant));  // the occupant is entirely untouched
}

TEST_CASE("scene_snapshot: an empty capture is legal and its restore is a no-op (N8)") {
    World world;
    SubtreeSnapshot snap;
    CHECK(snap.capture(world, std::vector<Entity>{}));
    CHECK(snap.empty());
    CHECK(snap.entityCount() == 0);
    CHECK(snap.roots().empty());
    const std::size_t before = world.entityCount();
    CHECK(snap.restore(world));
    CHECK(world.entityCount() == before);
}

TEST_CASE("scene_snapshot: dead and null handles in the input are dropped silently (N9)") {
    World world;
    const Entity e = world.create();
    REQUIRE(world.destroy(e));

    SubtreeSnapshot snap;
    CHECK(snap.capture(world, std::vector<Entity>{e, Entity{}}));
    CHECK(snap.empty());
    CHECK(snap.restore(world));
}

TEST_CASE("scene_snapshot: the snapshot survives world.clear() (N10/G4)") {
    World world;
    const Entity e = world.create();
    REQUIRE(world.setName(e, "Survivor"));

    SubtreeSnapshot snap;
    REQUIRE(snap.capture(world, std::vector<Entity>{e}));
    world.clear();
    CHECK_FALSE(world.alive(e));

    REQUIRE(snap.restore(world));
    CHECK(world.alive(e));
    CHECK(world.name(e) == "Survivor");
}

TEST_CASE("scene_snapshot: ComponentSnapshot round-trips one component's value (N11/AC-11)") {
    World world;
    const Entity e = world.create();
    const Transform t{.position = Vec3{4.0F, 5.0F, 6.0F}};
    REQUIRE(world.add<Transform>(e, t) != nullptr);
    const ComponentTypeId transformId = world.findComponentType("engine::Transform");
    REQUIRE(transformId.valid());

    SUBCASE("round-trip onto the same entity") {
        ComponentSnapshot snap;
        REQUIRE(snap.capture(world, e, transformId));
        REQUIRE(world.remove<Transform>(e));
        REQUIRE_FALSE(world.has<Transform>(e));
        REQUIRE(snap.restore(world, e));
        REQUIRE(world.has<Transform>(e));
        CHECK(*world.get<Transform>(e) == t);
    }

    SUBCASE("round-trip onto a different entity") {
        ComponentSnapshot snap;
        REQUIRE(snap.capture(world, e, transformId));
        const Entity other = world.create();
        REQUIRE(snap.restore(world, other));
        REQUIRE(world.has<Transform>(other));
        CHECK(*world.get<Transform>(other) == t);
    }

    SUBCASE("the empty path -- capture of an absent component") {
        const Entity bare = world.create();
        ComponentSnapshot snap;
        CHECK_FALSE(snap.capture(world, bare, transformId));
        CHECK(snap.empty());
    }

    SUBCASE("restore from an empty snapshot") {
        const ComponentSnapshot snap;
        CHECK_FALSE(snap.restore(world, e));
    }

    // AC-11's "tags included" is the ComponentSnapshot sibling of N6's finding: a tag's OWN private
    // store still needs to know the type (F10), so a genuinely unmirrorable tag is refused SILENTLY
    // at capture -- never a corrupted/zeroed restore. Full WARN-free round-trip is H3's future case.
    SUBCASE("a tag the private store cannot mirror is refused silently, not corrupted (H3)") {
        const ComponentTypeId tagId = registerComponent<TagProbe>(world, "N11.TagProbe");
        REQUIRE(tagId.valid());
        const Entity tagged = world.create();
        world.add<TagProbe>(tagged);
        REQUIRE(world.has<TagProbe>(tagged));

        ComponentSnapshot snap;
        CHECK_FALSE(snap.capture(world, tagged, tagId));
        CHECK(snap.empty());
        CHECK(world.has<TagProbe>(tagged));  // capture's refusal touched nothing
    }
}

TEST_CASE("scene_snapshot: an unmirrorable type logs exactly one WARN per capture, not per entity (N12/A15/D24)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    World world;
    const ComponentTypeId probeId = registerComponent<FieldedProbe>(world, "N12.FieldedProbe");
    REQUIRE(probeId.valid());

    const Entity root = world.create();
    const Entity child = world.create();
    REQUIRE(world.setParent(child, root));
    world.add<FieldedProbe>(root, FieldedProbe{.value = 1});
    world.add<FieldedProbe>(child, FieldedProbe{.value = 2});
    REQUIRE(world.setName(root, "Root"));

    SubtreeSnapshot snap;
    scope.sink()->take(records);
    records.clear();
    REQUIRE(snap.capture(world, std::vector<Entity>{root}));
    scope.sink()->take(records);
    CHECK(countAtLevel(records, engine::LogLevel::Warn) == 1);  // ONE WARN, not two

    REQUIRE(world.destroy(root));
    REQUIRE(snap.restore(world));
    CHECK(world.alive(root));
    CHECK(world.alive(child));
    CHECK(world.name(root) == "Root");           // the rest of the entity still round-trips (AC-9)
    CHECK_FALSE(world.has<FieldedProbe>(root));  // the unmirrorable component did not
    CHECK_FALSE(world.has<FieldedProbe>(child));
}

// N13 is the OTHER half of AC-7's atomicity, and it exists because the sabotage matrix found N7 alone
// could not prove it: N7 pre-occupies the FIRST record's index, so phase A refuses on record 0, the
// rollback loop iterates zero times, and seeding S6 (the rollback deleted outright) left the WHOLE
// suite green. A `world.clear()` probe in that loop confirmed it is never entered with created > 0
// anywhere in the inventory. This case fails on the SECOND record instead: phase A must recreate `a`,
// refuse `b`, then DESTROY `a` again and return false, leaving no survivor of the attempt.
//
// Two INDEPENDENT roots, not a parent/child pair, because that makes the destroy ORDER this test's own
// to choose -- entt's entity free list is LIFO, so destroying `b` last is what makes the single
// create() below land on b's index while a's stays free.
TEST_CASE("scene_snapshot: a failure on a LATER record rolls back what phase A already made (N13/AC-7/S6)") {
    World world;
    const Entity a = world.create();
    const Entity b = world.create();

    SubtreeSnapshot snap;
    REQUIRE(snap.capture(world, std::vector<Entity>{a, b}));  // topMost preserves input order: records [a, b]
    REQUIRE(snap.entityCount() == 2);

    REQUIRE(world.destroy(a));  // freed FIRST
    REQUIRE(world.destroy(b));  // freed LAST -- so it is the first index handed back out
    REQUIRE(world.entityCount() == 0);

    // ONE create(), with the LIFO assumption ASSERTED rather than looped around (N7's do-while shape is
    // wrong here): were entt to stop recycling b's slot first, this REQUIRE fails loudly instead of
    // leaving the case vacuous -- a loop would occupy a's index too and phase A would then refuse on
    // record 0, silently degrading N13 back into N7.
    const Entity occupant = world.create();
    REQUIRE(occupant.index == b.index);
    REQUIRE(occupant.generation != b.generation);
    REQUIRE(world.entityCount() == 1);

    CHECK_FALSE(snap.restore(world));
    CHECK_FALSE(world.alive(a));      // THE ROLLBACK: `a` WAS recreated, then destroyed again
    CHECK_FALSE(world.alive(b));      // never recreated at all
    CHECK(world.alive(occupant));     // the occupant is entirely untouched
    CHECK(world.entityCount() == 1);  // exactly the occupant -- no survivor of the failed attempt
}

// AC-2's ERROR accounting for World::recreate, driven from here (not tests/scene_test.cpp -- see
// that TU's W3 comment) because aero_editor_shell_test already carries a LogSinkScope and links
// aero::scene. One TEST_CASE, six SUBCASEs: a LogFixture declared FIRST (destructs LAST), then a
// LogSinkScope, then one SUBCASE per refusal. records.clear() between takes inside one scope
// (LogSink::take's debug-asserted empty-`out` precondition). Counted BY LEVEL, never records.size().
TEST_CASE("scene_snapshot: World::recreate's refusals log exactly one ERROR each (RL1/AC-2)") {
    const LogFixture fixture;
    const engine::editor::LogSinkScope scope;
    std::vector<engine::editor::LogEntry> records;

    SUBCASE("moved-from World") {
        // Wrapped in std::optional (the shell_test.cpp / command_stack_test.cpp PanelRegistry/
        // CommandStack precedent): moving *source rather than a bare local, and reading the
        // moved-from state back through source-> afterward, is what keeps bugprone-use-after-move
        // from flagging a DELIBERATE moved-from-state assertion.
        std::optional<World> source;
        source.emplace();
        const Entity e = source->create();
        const World drained = std::move(*source);
        (void)drained;
        scope.sink()->take(records);
        records.clear();
        CHECK(source->recreate(e) == Entity{});
        scope.sink()->take(records);
        CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
    }

    SUBCASE("an occupied index") {
        World w;
        const Entity a = w.create();
        REQUIRE(w.destroy(a));
        const Entity recycled = w.create();  // recycles a's slot at a new generation
        REQUIRE(recycled.index == a.index);
        scope.sink()->take(records);
        records.clear();
        CHECK(w.recreate(a) == Entity{});
        scope.sink()->take(records);
        CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
        CHECK(w.alive(recycled));
    }

    SUBCASE("an index never issued") {
        World w;
        const Entity seed = w.create();
        REQUIRE(seed.valid());
        const std::size_t before = w.entityCount();
        scope.sink()->take(records);
        records.clear();
        CHECK(w.recreate(Entity{9999, 1}) == Entity{});
        scope.sink()->take(records);
        CHECK(countAtLevel(records, engine::LogLevel::Error) == 1);
        CHECK(w.entityCount() == before);
    }

    SUBCASE("Entity{}") {
        World w;
        scope.sink()->take(records);
        records.clear();
        CHECK(w.recreate(Entity{}) == Entity{});
        scope.sink()->take(records);
        CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
    }

    SUBCASE("an already-live handle") {
        World w;
        const Entity a = w.create();
        scope.sink()->take(records);
        records.clear();
        CHECK(w.recreate(a) == Entity{});
        scope.sink()->take(records);
        CHECK(countAtLevel(records, engine::LogLevel::Error) == 0);
        CHECK(w.alive(a));
    }

    SUBCASE("ANTI-VACUITY: the sink IS listening") {
        AERO_LOG_ERROR("scene_snapshot_test: deliberate canary record");
        scope.sink()->take(records);
        CHECK_FALSE(records.empty());
    }
}

// tests/editor/inspector_test.cpp -- task 2.2.2: the reflection-driven inspector's tier-0 battery,
// including the zero-per-component-editor-code proof (D18, AC-11): every case below drives
// InspectorProbe, a fixture no line of editor code has ever heard of
// (`git grep InspectorProbe -- editor/` is empty).
//
// GATED (unlike aero_editor_shell_test): this target compiles GENERATED entt::meta for its own
// fixture. Tier-0 -- no GPU, no on-screen surface, no UI-layer bootstrap; must pass identically
// with AERO_REQUIRE_GPU set or unset (it builds none of the platform/RHI/UI-shell machinery).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <aero/editor/component_ops.hpp>
#include <aero/editor/inspector_model.hpp>
#include <aero/scene/internal/world_access.hpp>  // registerComponent<T> -- the D18 proof fixture's seam
#include <aero/scene/transform.hpp>

#include "inspector_probe.hpp"

#include <doctest/doctest.h>
#include <entt/entt.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

using engine::ComponentTypeId;
using engine::Entity;
using engine::World;
using engine::editor::buildInspectorModel;
using engine::editor::FieldKind;
using engine::editor::FieldValue;
using engine::editor::InspectorModel;
using engine::editor::readComponentField;
using engine::editor::writeComponentField;
using engine::scene::internal::registerComponent;

// Forward-declared here; DEFINED by the GENERATED aero_editor_inspector_test.aggregator.gen.cpp
// (cmake/reflect.cmake's aero_reflect_generate(), task 1.1.4) that calls
// aero_reflect_register_inspector_probe() -- the ONE header this target generates meta for. The
// snake_case name is the frozen cross-boundary contract (spec D3/D7), the same treatment
// meta_test.cpp already uses five times.
// NOLINTNEXTLINE(readability-identifier-naming)
void aero_reflect_register_all_aero_editor_inspector_test();

// Declared here (NOT included from any header): engine::editor::registerEditorReflection is a
// normal, hand-written function with external linkage, defined in editor/src/editor_reflection.cpp
// and compiled into aero::editor_core -- this target links that library, so calling it here pulls
// in the REAL aggregator for the 4 built-in headers (transform/camera/light/mesh_renderer.hpp),
// the same one the shipping bootstrap calls. Only the AC-12 drift-pin case below calls it.
namespace engine::editor {
void registerEditorReflection();
}  // namespace engine::editor

namespace {

ComponentTypeId registerProbe(World& world) { return registerComponent<InspectorProbe>(world, "InspectorProbe"); }

// Writes `input` (as the WIDE type T) through the seam, reads it back, and widens whatever came
// back into T for comparison -- this is what lets a single helper express BOTH a same-kind
// round-trip (e.g. writeThenRead<uint64_t>(..., 300) against tiny, a UInt field) and the
// cross-kind-tolerant width-clamp cases the seam's own four-row table defines (e.g.
// writeThenRead<int64_t>(..., -1) against that SAME uint8_t field -- an int64 input into an
// unsigned destination is a DEFINED pairing, not a kind mismatch, per C6/the seam's own comment).
template <typename T>
T writeThenRead(World& world, Entity entity, ComponentTypeId id, std::string_view field, T input) {
    REQUIRE(writeComponentField(world, entity, id, field, FieldValue{input}));
    const std::optional<FieldValue> result = readComponentField(world, entity, id, field);
    REQUIRE(result.has_value());
    return std::visit(
        [](auto&& v) -> T {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_arithmetic_v<V>) {
                return static_cast<T>(v);
            } else {
                FAIL("writeThenRead: read-back value is not arithmetic");
                return T{};
            }
        },
        *result);
}

const engine::editor::FieldEntry& findField(const std::vector<engine::editor::FieldEntry>& fields,
                                            std::string_view name) {
    for (const engine::editor::FieldEntry& f : fields) {
        if (f.name == name) {
            return f;
        }
    }
    FAIL("field not found: ", name);
    return fields.front();
}

}  // namespace

TEST_CASE("inspector: model lists present components in registration order, fields in declaration order (AC-8)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    REQUIRE(probeId.valid());
    CHECK(world.componentTypeCount() == 6);  // 5 built-ins + InspectorProbe

    const Entity e = world.create();
    world.add<engine::Transform>(e, engine::Transform{});
    world.addRaw(probeId, e, nullptr);

    InspectorModel model;
    // O1 pin (compile-time, not runtime): a caller holding a World& binds to buildInspectorModel's
    // `const World&` parameter for free -- this line would fail to COMPILE if the signature ever
    // regressed to a non-const or reference-incompatible shape.
    const World& constWorld = world;
    buildInspectorModel(constWorld, e, model);

    REQUIRE(model.components.size() == 2);
    // Transform is registered BEFORE InspectorProbe (the 5 built-ins occupy indices 0..4); THIS
    // test binary generates entt::meta for InspectorProbe only (its one HEADERS entry), so
    // Transform correctly shows hasFields == false here -- not a defect, a scope fact of this
    // specific test target (the dedicated engine-side proof lives in meta_test.cpp).
    CHECK(model.components[0].name == "engine::Transform");
    CHECK_FALSE(model.components[0].hasFields);
    CHECK(model.components[0].fields.empty());

    const engine::editor::ComponentEntry& probeEntry = model.components[1];
    CHECK(probeEntry.name == "InspectorProbe");
    REQUIRE(probeEntry.hasFields);
    REQUIRE(probeEntry.fields.size() == 10);

    const std::vector<std::string> expectedOrder{"speed",   "tint", "label", "gear", "tiny",
                                                 "enabled", "aim",  "mass",  "tick", "glyph"};
    for (std::size_t i = 0; i < expectedOrder.size(); ++i) {
        CHECK(probeEntry.fields[i].name == expectedOrder[i]);
    }

    entt::meta_reset();
}

TEST_CASE("inspector: model over InspectorProbe -- every field's kind, range and colour (AC-8, AC-11, S3)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);

    InspectorModel model;
    buildInspectorModel(world, e, model);
    REQUIRE(model.components.size() == 1);
    const std::vector<engine::editor::FieldEntry>& fields = model.components[0].fields;
    REQUIRE(fields.size() == 10);

    const engine::editor::FieldEntry& speed = findField(fields, "speed");
    CHECK(speed.kind == FieldKind::Float);
    CHECK(speed.hasRange);
    CHECK(speed.rangeMin == doctest::Approx(0.0));
    CHECK(speed.rangeMax == doctest::Approx(10.0));
    CHECK_FALSE(speed.color);

    const engine::editor::FieldEntry& tint = findField(fields, "tint");
    CHECK(tint.kind == FieldKind::Vec3);
    CHECK(tint.color);
    CHECK_FALSE(tint.hasRange);

    const engine::editor::FieldEntry& label = findField(fields, "label");
    CHECK(label.kind == FieldKind::String);
    CHECK_FALSE(label.hasRange);
    CHECK_FALSE(label.color);

    const engine::editor::FieldEntry& gear = findField(fields, "gear");
    CHECK(gear.kind == FieldKind::Int);
    CHECK_FALSE(gear.hasRange);

    const engine::editor::FieldEntry& tiny = findField(fields, "tiny");
    CHECK(tiny.kind == FieldKind::UInt);
    CHECK_FALSE(tiny.hasRange);

    CHECK(findField(fields, "enabled").kind == FieldKind::Bool);
    CHECK(findField(fields, "aim").kind == FieldKind::Quat);

    const engine::editor::FieldEntry& mass = findField(fields, "mass");
    CHECK(mass.kind == FieldKind::Float);
    CHECK_FALSE(mass.hasRange);

    entt::meta_reset();
}

TEST_CASE("inspector: O2 pins -- long tick is Int, char16_t glyph is UInt, both round-trip (S7)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);

    InspectorModel model;
    buildInspectorModel(world, e, model);
    REQUIRE(model.components.size() == 1);
    const std::vector<engine::editor::FieldEntry>& fields = model.components[0].fields;

    // O2 (plan decision, 2026-07-26): the spec's 15-type list omits `long` and `char16_t`, which
    // would leave these two fields SILENTLY invisible -- accepted by reflect-gen, registered in
    // entt::meta, and rendered by NOTHING. Truncating ArithmeticTypes back to 15 reds these two
    // checks (sabotage S7).
    CHECK(findField(fields, "tick").kind == FieldKind::Int);
    CHECK(findField(fields, "glyph").kind == FieldKind::UInt);

    CHECK(writeThenRead<std::int64_t>(world, e, probeId, "tick", 42) == 42);
    CHECK(writeThenRead<std::uint64_t>(world, e, probeId, "glyph", 7) == 7);

    entt::meta_reset();
}

TEST_CASE("inspector: buildInspectorModel binds through a const World& (O1, compile-time)") {
    World world;
    const ComponentTypeId probeId = registerProbe(world);
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);

    const World& cw = world;
    InspectorModel model;
    buildInspectorModel(cw, e, model);
    CHECK(model.entity == e);
}

TEST_CASE("inspector: the seam round-trips every kind (AC-7)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);

    CHECK(writeThenRead<bool>(world, e, probeId, "enabled", true));
    CHECK(writeThenRead<std::int64_t>(world, e, probeId, "gear", -7) == -7);
    CHECK(writeThenRead<std::uint64_t>(world, e, probeId, "tiny", 200) == 200);
    CHECK(writeThenRead<double>(world, e, probeId, "mass", 3.5) == doctest::Approx(3.5));

    REQUIRE(writeComponentField(world, e, probeId, "tint", FieldValue{engine::Vec3{1.0F, 2.0F, 3.0F}}));
    const std::optional<FieldValue> tintRead = readComponentField(world, e, probeId, "tint");
    REQUIRE(tintRead.has_value());
    REQUIRE(std::holds_alternative<engine::Vec3>(*tintRead));
    CHECK(std::get<engine::Vec3>(*tintRead).x == doctest::Approx(1.0F));

    REQUIRE(writeComponentField(world, e, probeId, "aim", FieldValue{engine::Quat{0.0F, 0.0F, 0.0F, 1.0F}}));
    const std::optional<FieldValue> aimRead = readComponentField(world, e, probeId, "aim");
    REQUIRE(aimRead.has_value());
    REQUIRE(std::holds_alternative<engine::Quat>(*aimRead));

    REQUIRE(writeComponentField(world, e, probeId, "label", FieldValue{std::string{"hello"}}));
    const std::optional<FieldValue> labelRead = readComponentField(world, e, probeId, "label");
    REQUIRE(labelRead.has_value());
    REQUIRE(std::holds_alternative<std::string>(*labelRead));
    CHECK(std::get<std::string>(*labelRead) == "hello");

    entt::meta_reset();
}

TEST_CASE("inspector: range clamp -- write 99 reads 10, write -5 reads 0 (S2)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);

    CHECK(writeThenRead<double>(world, e, probeId, "speed", 99.0) == doctest::Approx(10.0));
    CHECK(writeThenRead<double>(world, e, probeId, "speed", -5.0) == doctest::Approx(0.0));

    entt::meta_reset();
}

TEST_CASE("inspector: width clamp on tiny -- 300 reads 255 (S5)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);

    // Only the 300 case can DISCRIMINATE. Measured against the pinned EnTT, an unclamped
    // meta_data::set(uint8Member, int64{300}) returns true and stores 44 -- so removing the clamp
    // reds this line. An unclamped -1 also stores 0, which is the clamped answer too, so the -1
    // line below is COVERAGE, NOT PROOF: it can never fail. Sabotage S5's verdict is read off the
    // 300 line alone. (The 2.2.1 C1 lesson: a criterion that cannot fail must never be presented
    // as one.)
    CHECK(writeThenRead<std::uint64_t>(world, e, probeId, "tiny", 300) == 255);  // discriminating
    CHECK(writeThenRead<std::int64_t>(world, e, probeId, "tiny", -1) == 0);      // coverage only -- cannot fail

    entt::meta_reset();
}

TEST_CASE("inspector: seam rejections -- kind mismatch, unknown field, unregistered id, dead/null entity (AC-7)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);

    // kind mismatch: a Vec3 into a float field -- never mutates.
    CHECK_FALSE(writeComponentField(world, e, probeId, "speed", FieldValue{engine::Vec3{}}));
    CHECK(writeThenRead<double>(world, e, probeId, "speed", 1.0) == doctest::Approx(1.0));

    // unknown field
    CHECK_FALSE(writeComponentField(world, e, probeId, "nope", FieldValue{1.0}));
    CHECK_FALSE(readComponentField(world, e, probeId, "nope").has_value());

    // unregistered component id
    const ComponentTypeId bogus{};
    CHECK_FALSE(writeComponentField(world, e, bogus, "speed", FieldValue{1.0}));
    CHECK_FALSE(readComponentField(world, e, bogus, "speed").has_value());

    // dead entity
    const Entity dead = world.create();
    world.destroy(dead);
    CHECK_FALSE(writeComponentField(world, dead, probeId, "speed", FieldValue{1.0}));
    CHECK_FALSE(readComponentField(world, dead, probeId, "speed").has_value());

    // null entity
    CHECK_FALSE(writeComponentField(world, Entity{}, probeId, "speed", FieldValue{1.0}));
    CHECK_FALSE(readComponentField(world, Entity{}, probeId, "speed").has_value());

    entt::meta_reset();
}

TEST_CASE(
    "inspector: addComponent refuses a present type; adds a default-constructed absent one; "
    "removeComponent is idempotent-false (AC-10, D10)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    const Entity e = world.create();

    CHECK_FALSE(world.hasRaw(probeId, e));
    CHECK(engine::editor::addComponent(world, e, probeId));
    CHECK(world.hasRaw(probeId, e));
    CHECK_FALSE(engine::editor::addComponent(world, e, probeId));  // refuses -- already present (D10)

    CHECK(engine::editor::removeComponent(world, e, probeId));
    CHECK_FALSE(world.hasRaw(probeId, e));
    CHECK_FALSE(engine::editor::removeComponent(world, e, probeId));  // idempotent-false

    entt::meta_reset();
}

namespace {
struct InspectorMarker {
    int payload = 0;
};
}  // namespace

TEST_CASE("inspector: hasFields is false for a runtime-registered, meta-less type (E4)") {
    World world;
    const ComponentTypeId markerId = registerComponent<InspectorMarker>(world, "InspectorMarker");
    REQUIRE(markerId.valid());
    const Entity e = world.create();
    world.addRaw(markerId, e, nullptr);

    InspectorModel model;
    buildInspectorModel(world, e, model);
    REQUIRE(model.components.size() == 1);
    CHECK(model.components[0].name == "InspectorMarker");
    CHECK_FALSE(model.components[0].hasFields);
    CHECK(model.components[0].fields.empty());
}

TEST_CASE("inspector: model scratch is reused across builds -- content equal, capacity retained (D15)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);

    InspectorModel model;
    buildInspectorModel(world, e, model);
    REQUIRE(model.components.size() == 1);
    REQUIRE(model.components[0].fields.size() == 10);
    const std::size_t componentCapacity = model.components.capacity();
    const std::size_t fieldCapacity = model.components[0].fields.capacity();

    buildInspectorModel(world, e, model);
    CHECK(model.components.size() == 1);
    CHECK(model.components[0].fields.size() == 10);
    CHECK(model.components.capacity() == componentCapacity);
    CHECK(model.components[0].fields.capacity() == fieldCapacity);

    // The two builds' content is equal.
    CHECK(std::get<double>(model.components[0].fields[7].value) == doctest::Approx(0.0));  // mass, untouched

    entt::meta_reset();
}

// AC-12's drift pin: registers the ACTUAL 5 built-in components' entt::meta via the real
// production aggregator (aero::editor_core's registerEditorReflection), then proves every one of
// them has fields in the model. Placed LAST: registerEditorReflection's registration is
// process-lifetime and permanent (no teardown, F24), so no earlier case may assume the 5
// built-ins' meta is ABSENT after this one runs.
TEST_CASE("inspector: AC-12 drift pin -- every registered built-in component has fields (S6)") {
    engine::editor::registerEditorReflection();

    World world;
    const Entity e = world.create();
    const std::size_t count = world.componentTypeCount();
    REQUIRE(count == 5);  // the 5 built-ins -- no InspectorProbe registered on THIS World
    for (std::size_t i = 0; i < count; ++i) {
        const ComponentTypeId id = world.componentTypeAt(i);
        world.addRaw(id, e, nullptr);
    }

    InspectorModel model;
    buildInspectorModel(world, e, model);
    CHECK(model.components.size() == count);
    for (const engine::editor::ComponentEntry& entry : model.components) {
        CHECK(entry.hasFields);
    }
}

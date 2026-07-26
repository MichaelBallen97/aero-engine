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
#include "inspector_tag.hpp"

#include <doctest/doctest.h>
#include <entt/entt.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
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
    REQUIRE(probeEntry.fields.size() == 12);

    const std::vector<std::string> expectedOrder{"speed", "tint", "label", "gear",  "tiny",         "enabled",
                                                 "aim",   "mass", "tick",  "glyph", "clampedRange", "hugeRange"};
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
    REQUIRE(fields.size() == 12);

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

    // Review finding 2's coverage pins: both carry a range whose bounds are OUTSIDE their own
    // destination's domain (a negative min on an unsigned field; a magnitude far too large for a
    // 16-bit destination) -- the model build itself must not UB just by reading the annotation's
    // raw doubles back (that only happens on a WRITE, covered by the seam round-trip case below).
    const engine::editor::FieldEntry& clampedRange = findField(fields, "clampedRange");
    CHECK(clampedRange.kind == FieldKind::UInt);
    CHECK(clampedRange.hasRange);
    CHECK(clampedRange.rangeMin == doctest::Approx(-10.0));
    CHECK(clampedRange.rangeMax == doctest::Approx(-5.0));

    const engine::editor::FieldEntry& hugeRange = findField(fields, "hugeRange");
    CHECK(hugeRange.kind == FieldKind::Int);
    CHECK(hugeRange.hasRange);
    CHECK(hugeRange.rangeMin == doctest::Approx(1e300));
    CHECK(hugeRange.rangeMax == doctest::Approx(2e300));

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

TEST_CASE(
    "inspector: range bounds outside the destination's own domain do not UB the write path "
    "(review finding 2)") {
    // Debug builds compile with -fsanitize=undefined, and static_cast<uint64_t>(-1.0) /
    // static_cast<int64_t>(1e300) are BOTH undefined behaviour ([conv.fpint]) -- this case's whole
    // point is to reach the exact branch that used to perform that cast unconditionally. A green run
    // under ASan/UBSan is the proof; there is no return value that could distinguish "clamped
    // correctly" from "the sanitizer merely didn't trip this time."
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);

    // clampedRange's range is WHOLLY NEGATIVE (-10..-5) on an unsigned field: every non-negative
    // write exceeds rangeMax, which clampRangeUint64 used to cast via std::floor(rangeMax) with NO
    // guard (only the rangeMin <= 0.0 branch was guarded -- the asymmetry the review caught). The
    // clamped result saturates to 0 (T's own lowest), not -5.
    CHECK(writeThenRead<std::uint64_t>(world, e, probeId, "clampedRange", 7) == 0);

    // hugeRange's range (1e300..2e300) exceeds int64_t's own domain entirely: every write's widened
    // int64 falls below rangeMin, which clampRangeInt64 used to cast via std::ceil(rangeMin) with NO
    // guard at all. The clamped result saturates to int16_t's own max, not a garbage truncation.
    CHECK(writeThenRead<std::int64_t>(world, e, probeId, "hugeRange", 42) == std::numeric_limits<std::int16_t>::max());

    entt::meta_reset();
}

TEST_CASE(
    "inspector: seam rejections -- kind mismatch, unknown field, unregistered id, dead/null "
    "entity, AND NEVER MUTATE (AC-7, review finding 5)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);

    // A known baseline, established BEFORE any rejection attempt -- component_ops.hpp's own
    // contract promises every rejection "NEVER mutates", so the proof is reading the field back
    // and asserting it is EXACTLY the baseline, not merely that some LATER good write succeeded
    // (a good write afterward would silently paper over a mutate-before-validate defect: moving
    // member.set() above the kind check in ArithmeticWriter would leave a return-value-only
    // check fully green, since `ok` is still computed correctly afterward -- only re-reading the
    // field catches the mutation itself).
    REQUIRE(writeComponentField(world, e, probeId, "speed", FieldValue{double{3.5}}));
    const std::optional<FieldValue> baseline = readComponentField(world, e, probeId, "speed");
    REQUIRE(baseline.has_value());
    REQUIRE(std::holds_alternative<double>(*baseline));

    // kind mismatch: a Vec3 into a float field -- must NEVER mutate.
    CHECK_FALSE(writeComponentField(world, e, probeId, "speed", FieldValue{engine::Vec3{}}));
    const std::optional<FieldValue> afterKindMismatch = readComponentField(world, e, probeId, "speed");
    REQUIRE(afterKindMismatch.has_value());
    CHECK(std::get<double>(*afterKindMismatch) == doctest::Approx(std::get<double>(*baseline)));

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

    // Final re-read: "speed" is STILL exactly the baseline -- no rejection attempt above ever
    // mutated it, regardless of which specific rejection path ran in between.
    const std::optional<FieldValue> after = readComponentField(world, e, probeId, "speed");
    REQUIRE(after.has_value());
    CHECK(std::get<double>(*after) == doctest::Approx(std::get<double>(*baseline)));

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

TEST_CASE(
    "inspector: a zero-field, meta-registered tag component -- addComponent's nullptr-on-success "
    "signal and the model's (no fields) branch (E13, review finding 6)") {
    // InspectorProbe (10-12 fields) and the five built-ins (all field-bearing) leave two things
    // unexercised: addComponent's documented raison d'etre -- a tag's addRaw returns nullptr ON
    // SUCCESS too, so hasRaw AFTERWARDS is the only correct signal (component_ops.cpp) -- and the
    // model's hasFields == true / fields.empty() combination (distinct from the E4 case below, where
    // hasFields == false because the type carries no entt::meta at ALL).
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId tagId = registerComponent<InspectorTag>(world, "InspectorTag");
    REQUIRE(tagId.valid());
    const Entity e = world.create();

    CHECK_FALSE(world.hasRaw(tagId, e));
    CHECK(engine::editor::addComponent(world, e, tagId));  // succeeds despite addRaw returning nullptr
    CHECK(world.hasRaw(tagId, e));
    CHECK_FALSE(engine::editor::addComponent(world, e, tagId));  // refuses -- already present (D10)

    InspectorModel model;
    buildInspectorModel(world, e, model);
    REQUIRE(model.components.size() == 1);
    CHECK(model.components[0].name == "InspectorTag");
    CHECK(model.components[0].hasFields);  // meta IS registered -- distinct from the E4 case
    CHECK(model.components[0].fields.empty());

    CHECK(engine::editor::removeComponent(world, e, tagId));
    CHECK_FALSE(world.hasRaw(tagId, e));

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

TEST_CASE(
    "inspector: model scratch is reused in place across builds -- an injected over-reservation "
    "survives (D15, review finding 7)") {
    // Plain capacity() equality (the original shape of this case) CANNOT discriminate: clear() +
    // push_back(fresh ComponentEntry{}) retains the OUTER vector's own capacity too, and a fresh
    // INNER `fields` vector regrowing from empty to 12 elements lands on the SAME deterministic
    // capacity every time (std::vector's own growth factor), so the exact implementation D15
    // exists to forbid would still pass a bare capacity comparison. Manually over-reserving past
    // anything a same-shape rebuild would naturally need, THEN checking the reservation survived
    // a second build, does discriminate: a clear()-and-rebuild implementation destroys and
    // recreates each ComponentEntry (and its OWN `fields` vector) from scratch every call, which
    // would throw the injected reservation away; the in-place index-based overwrite this class
    // actually uses only ever .clear()s (never destroys) an existing slot's own vector, so the
    // reservation -- and the buffer address itself -- survive.
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);

    InspectorModel model;
    buildInspectorModel(world, e, model);
    REQUIRE(model.components.size() == 1);
    REQUIRE(model.components[0].fields.size() == 12);

    model.components.reserve(64);
    model.components[0].fields.reserve(512);
    const void* componentsData = model.components.data();
    const void* fieldsData = model.components[0].fields.data();
    const std::size_t componentCapacity = model.components.capacity();
    const std::size_t fieldCapacity = model.components[0].fields.capacity();
    REQUIRE(componentCapacity >= 64);
    REQUIRE(fieldCapacity >= 512);

    buildInspectorModel(world, e, model);
    CHECK(model.components.size() == 1);
    CHECK(model.components[0].fields.size() == 12);
    CHECK(model.components.data() == componentsData);
    CHECK(model.components[0].fields.data() == fieldsData);
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

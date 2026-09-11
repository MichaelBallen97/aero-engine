// tests/editor/inspector_test.cpp -- task 2.2.2: the reflection-driven inspector's tier-0 battery,
// including the zero-per-component-editor-code proof (D18, AC-11): every case below drives
// InspectorProbe, a fixture no line of editor code has ever heard of
// (`git grep InspectorProbe -- editor/` is empty).
//
// GATED (unlike aero_editor_shell_test): this target compiles GENERATED entt::meta for its own
// fixture. Tier-0 -- no GPU, no on-screen surface, no UI-layer bootstrap; must pass identically
// with AERO_REQUIRE_GPU set or unset (it builds none of the platform/RHI/UI-shell machinery).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <aero/core/guid.hpp>              // task 3.1.5: formatGuid, GuidGenerator
#include <aero/core/log.hpp>               // task E.3.1, FD10: setLogCallback -- the seam's rejection records
#include <aero/editor/asset_database.hpp>  // task 3.1.5: the Guid row resolves against a real scan
#include <aero/editor/component_ops.hpp>
#include <aero/editor/inspector_model.hpp>
#include <aero/editor/text_file.hpp>             // task 3.1.5: writeTextFileAtomic, for the scanned fixture
#include <aero/scene/internal/world_access.hpp>  // registerComponent<T> -- the D18 proof fixture's seam
#include <aero/scene/transform.hpp>

#include "inspector_probe.hpp"
#include "inspector_tag.hpp"

#include <doctest/doctest.h>
#include <entt/entt.hpp>

#include <array>  // task E.3.1: the axis row's three shown components
#include <cstddef>
#include <cstdint>
#include <filesystem>  // task 3.1.5: the scanned-project fixture
#include <fstream>     // task 3.1.5: IR8's source-text pin
#include <limits>
#include <memory>  // task 3.1.5: std::unique_ptr<ScannedAssets>
#include <optional>
#include <ostream>  // MSVC alone needs the complete type to stringify a string_view inside a CHECK
#include <string>
#include <string_view>
#include <system_error>  // task 3.1.5: std::error_code -- the non-throwing filesystem overloads
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
    CHECK(world.componentTypeCount() == 11);  // 10 built-ins (E.2.2) + InspectorProbe

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
    REQUIRE(probeEntry.fields.size() == 13);  // task 3.1.5 appended `asset`

    const std::vector<std::string> expectedOrder{"speed",        "tint",      "label", "gear", "tiny",
                                                 "enabled",      "aim",       "mass",  "tick", "glyph",
                                                 "clampedRange", "hugeRange", "asset"};
    REQUIRE(expectedOrder.size() == probeEntry.fields.size());  // the list IS the declaration order
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
    REQUIRE(fields.size() == 13);  // task 3.1.5 appended `asset`

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
    REQUIRE(model.components[0].fields.size() == 13);

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
    CHECK(model.components[0].fields.size() == 13);
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
    REQUIRE(count == 10);  // the 10 built-ins (E.2.2) -- no InspectorProbe registered on THIS World
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

// ================================================================================================
// task 3.1.5 (IR1-IR8) -- the Guid category: the row's three display states, the Clear decision, the
// exact-type write, and the pin that says an inspector row is never a drop target.
//
// The row is asserted as a VALUE rather than as pixels, because guidFieldRow computes both of its
// decisions outside the draw walk and the panel renders exactly what it returns. That is what lets a
// tier-0 binary with no ImGui context test what a user sees.
// ================================================================================================
namespace {

// A real, scanned project in the OS temp directory. This TU is tier-0 -- no GPU, no window, no ImGui
// context -- which is a statement about DEVICES, not about the filesystem: AssetDatabase has no
// setter, so a record carrying a real GUID can only come from a real scan. remove_all FIRST, the
// project_files_test.cpp TempDir precedent, so a second ctest invocation cannot inherit the first's
// tree.
[[nodiscard]] std::string utf8Of(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

struct ScannedAssets {
    std::string projectRoot;
    std::string assetsRoot;
    engine::GuidGenerator generator{0x1A5EULL};
    engine::editor::AssetDatabase database;
};

[[nodiscard]] std::unique_ptr<ScannedAssets> scanOneModel() {
    static int counter = 0;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("aero_inspector_guid_" + std::to_string(++counter));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "assets", ec);

    auto scanned = std::make_unique<ScannedAssets>();
    scanned->projectRoot = utf8Of(dir);
    scanned->assetsRoot = utf8Of(dir / "assets");
    REQUIRE(engine::editor::writeTextFileAtomic(scanned->assetsRoot + "/hero.glb", "glTF-not-really").empty());
    const engine::editor::AssetScanReport report =
        scanned->database.rescan(scanned->projectRoot, scanned->assetsRoot, scanned->generator);
    REQUIRE(report.status == engine::editor::ScanStatus::Ok);
    return scanned;
}

}  // namespace

TEST_CASE("inspector: the Guid row's three display states and its Clear decision (task 3.1.5, IR1-IR5)") {
    const std::unique_ptr<ScannedAssets> scanned = scanOneModel();
    const std::optional<engine::Guid> known = scanned->database.guidForPath("hero.glb");
    REQUIRE(known.has_value());
    REQUIRE(known->valid());

    // IR1 -- nil is "no reference", a legal ordinary value. IR5 -- and Clear is DISABLED for it,
    // because clearing nothing would push an undo entry that changes no byte.
    const engine::editor::GuidFieldRow none = engine::editor::guidFieldRow(engine::Guid{}, &scanned->database);
    CHECK(none.text == "None");
    CHECK_FALSE(none.clearEnabled);
    // The nil row says the same thing with no database at all: absence of a reference is not a
    // question the database is asked.
    CHECK(engine::editor::guidFieldRow(engine::Guid{}, nullptr).text == "None");
    CHECK_FALSE(engine::editor::guidFieldRow(engine::Guid{}, nullptr).clearEnabled);

    // IR2 -- a guid this project knows: the leaf name and the kind label, never the path and never
    // the guid. classifyAssetKind puts a .glb in Model.
    const engine::editor::GuidFieldRow resolved = engine::editor::guidFieldRow(*known, &scanned->database);
    CHECK(resolved.text == "hero.glb  (Model)");
    CHECK(resolved.clearEnabled);

    // IR3 -- a guid the project does NOT know. The 8-hex prefix comes from formatGuid, so it is the
    // same spelling every other surface in the tree uses, and the reference stays CLEARABLE: a
    // dangling reference is the one a user most wants to remove.
    const engine::Guid stranger{0xFEEDFACECAFEBEEFULL, 0x0123456789ABCDEFULL};
    REQUIRE(scanned->database.findByGuid(stranger) == nullptr);
    const engine::editor::GuidFieldRow missing = engine::editor::guidFieldRow(stranger, &scanned->database);
    CHECK(missing.text == engine::formatGuid(stranger).substr(0, 8) + "...  (missing)");
    CHECK(missing.clearEnabled);

    // IR4 -- no database at all (a -DAERO_REFLECT_TOOLS=OFF shell, or a frame before the reconcile
    // has run) renders the SAME row as a missing record, deliberately: from the user's seat both mean
    // "this project cannot tell you what that is", and a second sentence would be a distinction
    // nobody can act on.
    const engine::editor::GuidFieldRow noDatabase = engine::editor::guidFieldRow(*known, nullptr);
    CHECK(noDatabase.text == engine::formatGuid(*known).substr(0, 8) + "...  (missing)");
    CHECK(noDatabase.clearEnabled);
    CHECK(noDatabase.text == engine::editor::guidFieldRow(*known, nullptr).text);
}

TEST_CASE("inspector: a Guid field reads, writes and REFUSES a wrong type (task 3.1.5, IR7, seed S36)") {
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    REQUIRE(probeId.valid());
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);

    // The model classifies it by TYPE, with no name special-casing anywhere: `asset` is the 13th
    // field in declaration order and comes back as its own kind rather than as a missing row.
    InspectorModel model;
    buildInspectorModel(world, e, model);
    REQUIRE(model.components.size() == 1);
    const engine::editor::FieldEntry& asset = findField(model.components[0].fields, "asset");
    CHECK(asset.kind == FieldKind::Guid);
    CHECK_FALSE(asset.hasRange);  // a Guid has no range and AERO_RANGE cannot be written on one
    REQUIRE(std::holds_alternative<engine::Guid>(asset.value));
    CHECK_FALSE(std::get<engine::Guid>(asset.value).valid());  // nil by default, and nil is a VALUE

    const engine::Guid written{0x0011223344556677ULL, 0x8899AABBCCDDEEFFULL};
    REQUIRE(writeComponentField(world, e, probeId, "asset", FieldValue{written}));
    {
        const std::optional<FieldValue> readBack = readComponentField(world, e, probeId, "asset");
        REQUIRE(readBack.has_value());
        REQUIRE(std::holds_alternative<engine::Guid>(*readBack));
        CHECK((std::get<engine::Guid>(*readBack) == written));
    }

    // S36: the seed is `member.set(handle, value)` on the RAW variant, which lets EnTT convert. Every
    // one of these is a genuine kind mismatch and must leave the field byte-identical -- a refusal
    // that half-wrote would be worse than a crash, because it looks like a successful edit.
    CHECK_FALSE(writeComponentField(world, e, probeId, "asset", FieldValue{3.5}));
    CHECK_FALSE(writeComponentField(world, e, probeId, "asset", FieldValue{std::int64_t{7}}));
    CHECK_FALSE(writeComponentField(world, e, probeId, "asset", FieldValue{std::uint64_t{7}}));
    CHECK_FALSE(writeComponentField(world, e, probeId, "asset", FieldValue{true}));
    CHECK_FALSE(writeComponentField(world, e, probeId, "asset", FieldValue{std::string{"deadbeef"}}));
    CHECK_FALSE(writeComponentField(world, e, probeId, "asset", FieldValue{engine::Vec3{}}));
    {
        const std::optional<FieldValue> untouched = readComponentField(world, e, probeId, "asset");
        REQUIRE(untouched.has_value());
        REQUIRE(std::holds_alternative<engine::Guid>(*untouched));
        CHECK((std::get<engine::Guid>(*untouched) == written));
    }

    // And the converse: a Guid input into a field that is NOT a Guid is refused just as hard, so the
    // new alternative cannot become a wildcard on the way in.
    CHECK_FALSE(writeComponentField(world, e, probeId, "mass", FieldValue{written}));
    CHECK_FALSE(writeComponentField(world, e, probeId, "label", FieldValue{written}));
    CHECK_FALSE(writeComponentField(world, e, probeId, "tint", FieldValue{written}));
}

TEST_CASE("inspector: no inspector row is a drop target (task 3.1.5, IR8, seed S35's twin)") {
    // D14, and the only tier that can state it: assignment happens on the Hierarchy row, in the
    // viewport and on a material slot -- never on an inspector field. A drop target here would be a
    // fourth assignment surface with its own accept rules, and no runtime tier in this tree can drive
    // an ImGui drag, so the pin is the panel's own source text.
    std::ifstream file(AERO_EDITOR_SRC_DIR "/inspector_panel.cpp", std::ios::binary);
    REQUIRE(file.is_open());
    std::string line;
    std::size_t scanned = 0;
    while (std::getline(file, line)) {
        ++scanned;
        const std::size_t comment = line.find("//");
        const std::string code = comment == std::string::npos ? line : line.substr(0, comment);
        CHECK(code.find("BeginDragDropTarget") == std::string::npos);
        CHECK(code.find("AcceptDragDropPayload") == std::string::npos);
    }
    CHECK(scanned > 100);  // the scan really traversed the file, rather than passing on an empty read
}

// D16's claim made machine-checkable, and placed immediately after the drift pin for the reason that
// case's own comment gives: registerEditorReflection's registration is process-lifetime and
// permanent, so a case appended AFTER it inherits a fully registered meta context by construction
// (every entt::meta_reset() in this TU is above the drift pin, and there is none below it).
//
// engine::AnimationPlayer (task 3.5.2) is the SIXTH built-in and the first with a bool field. It gets
// no bespoke inspector code -- the four fields the panel draws are the reflection spine doing its job
// -- so this case asserts the four FIELDS rather than merely that a lookup did not crash. A case that
// silently resolves nothing is exactly the vacuous green this check exists to avoid.
TEST_CASE("inspector: engine::AnimationPlayer reflects four fields, in declaration order (3.5.2/D16)") {
    engine::editor::registerEditorReflection();

    World world;
    const ComponentTypeId id = world.findComponentType("engine::AnimationPlayer");
    REQUIRE(id.valid());

    const Entity e = world.create();
    REQUIRE(world.addRaw(id, e, nullptr) != nullptr);

    InspectorModel model;
    buildInspectorModel(world, e, model);
    REQUIRE(model.components.size() == 1);
    const engine::editor::ComponentEntry& entry = model.components[0];
    CHECK(entry.name == "engine::AnimationPlayer");
    CHECK(entry.typeId == id);
    CHECK(entry.hasFields);

    REQUIRE(entry.fields.size() == 4);
    CHECK(entry.fields[0].name == "time");
    CHECK(entry.fields[1].name == "speed");
    CHECK(entry.fields[2].name == "loop");
    CHECK(entry.fields[3].name == "playing");
    CHECK((entry.fields[0].kind == FieldKind::Float));
    CHECK((entry.fields[1].kind == FieldKind::Float));
    CHECK((entry.fields[2].kind == FieldKind::Bool));
    CHECK((entry.fields[3].kind == FieldKind::Bool));

    // The defaults come through as VALUES, not just as kinds -- speed's 1.0 is what proves the model
    // read the component rather than a default-constructed FieldValue.
    CHECK(std::get<double>(entry.fields[0].value) == doctest::Approx(0.0));
    CHECK(std::get<double>(entry.fields[1].value) == doctest::Approx(1.0));
    CHECK(std::get<bool>(entry.fields[2].value));
    CHECK(std::get<bool>(entry.fields[3].value));

    // No AERO_RANGE on any of them (the camera near/far rule): an invented speed bound would show up
    // here as a range the panel would then clamp to.
    for (const engine::editor::FieldEntry& field : entry.fields) {
        INFO(field.name);
        CHECK_FALSE(field.hasRange);
        CHECK_FALSE(field.color);
    }
}

TEST_CASE("inspector: engine::AudioSource's eight reflected fields all resolve (task 3.7.2)") {
    // ALL EIGHT FIELDS, never merely "a lookup did not crash" -- 3.5.2's D16 lesson stated as a rule.
    // F9 verified inspector_panel.cpp already renders Bool, Float and Guid, so this component needs
    // NO editor work and none may be added: /editor is byte-identical across this task.
    //
    // THE REGISTRATION CALL IS NOT OPTIONAL, and its absence is why this case first passed for the
    // wrong reason: `entry.fields` is populated from the entt::meta registry, which only exists once
    // registerEditorReflection() has run. Without this line the case still passed in the full binary,
    // because the AnimationPlayer case above happens to call it and the registry is global -- so the
    // assertion was riding a neighbour's side effect and read 0 fields the moment it was run alone.
    // Every case that reads reflected fields calls this itself.
    engine::editor::registerEditorReflection();

    World world;
    const ComponentTypeId id = world.findComponentType("engine::AudioSource");
    REQUIRE(id.valid());

    const Entity e = world.create();
    REQUIRE(world.addRaw(id, e, nullptr) != nullptr);

    InspectorModel model;
    buildInspectorModel(world, e, model);
    REQUIRE(model.components.size() == 1);
    const engine::editor::ComponentEntry& entry = model.components[0];
    CHECK(entry.name == "engine::AudioSource");
    CHECK(entry.typeId == id);
    CHECK(entry.hasFields);

    REQUIRE(entry.fields.size() == 8);
    CHECK(entry.fields[0].name == "clip");  // declaration order IS inspector row order
    CHECK(entry.fields[1].name == "volume");
    CHECK(entry.fields[2].name == "pitch");
    CHECK(entry.fields[3].name == "minDistance");
    CHECK(entry.fields[4].name == "maxDistance");
    CHECK(entry.fields[5].name == "loop");
    CHECK(entry.fields[6].name == "playing");
    CHECK(entry.fields[7].name == "spatialize");

    CHECK((entry.fields[0].kind == FieldKind::Guid));
    CHECK((entry.fields[1].kind == FieldKind::Float));
    CHECK((entry.fields[2].kind == FieldKind::Float));
    CHECK((entry.fields[3].kind == FieldKind::Float));
    CHECK((entry.fields[4].kind == FieldKind::Float));
    CHECK((entry.fields[5].kind == FieldKind::Bool));
    CHECK((entry.fields[6].kind == FieldKind::Bool));
    CHECK((entry.fields[7].kind == FieldKind::Bool));

    // The defaults come through as VALUES, not just as kinds.
    CHECK(std::get<double>(entry.fields[1].value) == doctest::Approx(1.0));
    CHECK(std::get<double>(entry.fields[2].value) == doctest::Approx(1.0));
    CHECK(std::get<double>(entry.fields[3].value) == doctest::Approx(1.0));
    CHECK(std::get<double>(entry.fields[4].value) == doctest::Approx(50.0));
    CHECK_FALSE(std::get<bool>(entry.fields[5].value));  // loop defaults FALSE
    CHECK(std::get<bool>(entry.fields[6].value));        // playing defaults TRUE
    CHECK(std::get<bool>(entry.fields[7].value));        // spatialize defaults TRUE

    // The two AERO_RANGE bounds, and the four fields that deliberately carry none.
    CHECK(entry.fields[1].hasRange);
    CHECK(entry.fields[1].rangeMin == doctest::Approx(0.0));
    CHECK(entry.fields[1].rangeMax == doctest::Approx(1.0));
    CHECK(entry.fields[2].hasRange);
    CHECK(entry.fields[2].rangeMin == doctest::Approx(0.0));
    // 4.0 MIRRORS engine::audio::MAX_PITCH; SA1 is the static_assert that pins the two together.
    CHECK(entry.fields[2].rangeMax == doctest::Approx(4.0));
    CHECK_FALSE(entry.fields[3].hasRange);  // 1.3.3's D19: min > 0 is not a two-sided bound
    CHECK_FALSE(entry.fields[4].hasRange);

    for (const engine::editor::FieldEntry& field : entry.fields) {
        INFO(field.name);
        CHECK_FALSE(field.color);
    }
}

TEST_CASE("inspector: engine::AudioListener's one reflected field resolves (task 3.7.2)") {
    // See the note in the AudioSource case above: without this call the assertions ride whichever
    // neighbouring case registered the meta first, and read 0 fields when run alone.
    engine::editor::registerEditorReflection();

    World world;
    const ComponentTypeId id = world.findComponentType("engine::AudioListener");
    REQUIRE(id.valid());

    const Entity e = world.create();
    REQUIRE(world.addRaw(id, e, nullptr) != nullptr);

    InspectorModel model;
    buildInspectorModel(world, e, model);
    REQUIRE(model.components.size() == 1);
    const engine::editor::ComponentEntry& entry = model.components[0];
    CHECK(entry.name == "engine::AudioListener");
    CHECK(entry.hasFields);

    REQUIRE(entry.fields.size() == 1);
    CHECK(entry.fields[0].name == "volume");
    CHECK((entry.fields[0].kind == FieldKind::Float));
    CHECK(std::get<double>(entry.fields[0].value) == doctest::Approx(1.0));
    CHECK(entry.fields[0].hasRange);
    CHECK(entry.fields[0].rangeMin == doctest::Approx(0.0));
    CHECK(entry.fields[0].rangeMax == doctest::Approx(1.0));
    CHECK_FALSE(entry.fields[0].color);
}

TEST_CASE("inspector: engine::Environment's eight reflected fields all resolve (task E.2.1)") {
    // See the note in the AudioSource case above: without this call the assertions ride whichever
    // neighbouring case registered the meta first, and read 0 fields when run alone.
    engine::editor::registerEditorReflection();

    World world;
    const ComponentTypeId id = world.findComponentType("engine::Environment");
    REQUIRE(id.valid());

    const Entity e = world.create();
    REQUIRE(world.addRaw(id, e, nullptr) != nullptr);

    InspectorModel model;
    buildInspectorModel(world, e, model);
    REQUIRE(model.components.size() == 1);
    const engine::editor::ComponentEntry& entry = model.components[0];
    CHECK(entry.name == "engine::Environment");
    CHECK(entry.typeId == id);
    CHECK(entry.hasFields);

    REQUIRE(entry.fields.size() == 8);
    CHECK(entry.fields[0].name == "backgroundMode");  // declaration order IS inspector row order
    CHECK(entry.fields[1].name == "skyColor");
    CHECK(entry.fields[2].name == "horizonColor");
    CHECK(entry.fields[3].name == "groundColor");
    CHECK(entry.fields[4].name == "solidColor");
    CHECK(entry.fields[5].name == "ambientMode");
    CHECK(entry.fields[6].name == "ambientColor");
    CHECK(entry.fields[7].name == "ambientIntensity");

    CHECK((entry.fields[0].kind == FieldKind::UInt));
    CHECK((entry.fields[1].kind == FieldKind::Vec3));
    CHECK((entry.fields[2].kind == FieldKind::Vec3));
    CHECK((entry.fields[3].kind == FieldKind::Vec3));
    CHECK((entry.fields[4].kind == FieldKind::Vec3));
    CHECK((entry.fields[5].kind == FieldKind::UInt));
    CHECK((entry.fields[6].kind == FieldKind::Vec3));
    CHECK((entry.fields[7].kind == FieldKind::Float));

    // The two selectors carry the AERO_RANGE that makes the inspector clamp them (0..1, the two
    // modes each has today).
    CHECK(entry.fields[0].hasRange);
    CHECK(entry.fields[0].rangeMin == doctest::Approx(0.0));
    CHECK(entry.fields[0].rangeMax == doctest::Approx(1.0));
    CHECK(entry.fields[5].hasRange);
    CHECK(entry.fields[5].rangeMin == doctest::Approx(0.0));
    CHECK(entry.fields[5].rangeMax == doctest::Approx(1.0));

    // The five colours carry AERO_COLOR, so each renders a picker rather than three drag fields.
    CHECK(entry.fields[1].color);
    CHECK(entry.fields[2].color);
    CHECK(entry.fields[3].color);
    CHECK(entry.fields[4].color);
    CHECK(entry.fields[6].color);

    // ambientIntensity carries NEITHER -- 1.3.3's D19: an HDR multiplier has no defensible upper
    // bound. BOTH directions, because an absent flag asserted only one way is half a statement.
    CHECK_FALSE(entry.fields[7].hasRange);
    CHECK_FALSE(entry.fields[7].color);

    // ...and the two selectors are NOT colours, which is the other half of the same statement.
    CHECK_FALSE(entry.fields[0].color);
    CHECK_FALSE(entry.fields[5].color);
    // ...nor do the five colours carry a range.
    CHECK_FALSE(entry.fields[1].hasRange);
    CHECK_FALSE(entry.fields[2].hasRange);
    CHECK_FALSE(entry.fields[3].hasRange);
    CHECK_FALSE(entry.fields[4].hasRange);
    CHECK_FALSE(entry.fields[6].hasRange);
}

TEST_CASE("inspector: engine::SpotLight's five reflected fields all resolve (task E.2.2)") {
    // See the note in the AudioSource case above: without this call the assertions ride whichever
    // neighbouring case registered the meta first, and read 0 fields when run alone.
    engine::editor::registerEditorReflection();

    World world;
    const ComponentTypeId id = world.findComponentType("engine::SpotLight");
    REQUIRE(id.valid());

    const Entity e = world.create();
    REQUIRE(world.addRaw(id, e, nullptr) != nullptr);

    InspectorModel model;
    buildInspectorModel(world, e, model);
    REQUIRE(model.components.size() == 1);
    const engine::editor::ComponentEntry& entry = model.components[0];
    CHECK(entry.name == "engine::SpotLight");
    CHECK(entry.typeId == id);
    CHECK(entry.hasFields);

    REQUIRE(entry.fields.size() == 5);
    CHECK(entry.fields[0].name == "color");  // declaration order IS inspector row order
    CHECK(entry.fields[1].name == "intensity");
    CHECK(entry.fields[2].name == "range");
    CHECK(entry.fields[3].name == "innerConeRadians");
    CHECK(entry.fields[4].name == "outerConeRadians");

    CHECK((entry.fields[0].kind == FieldKind::Vec3));
    CHECK((entry.fields[1].kind == FieldKind::Float));
    CHECK((entry.fields[2].kind == FieldKind::Float));
    CHECK((entry.fields[3].kind == FieldKind::Float));
    CHECK((entry.fields[4].kind == FieldKind::Float));

    // The colour carries AERO_COLOR, so it renders a picker rather than three drag fields.
    CHECK(entry.fields[0].color);

    // The two cone angles carry the AERO_RANGE that clamps them to [0, pi/2] -- a hemisphere at
    // most. 1.5707964 is the float nearest pi/2, bit for bit engine::HALF_PI.
    CHECK(entry.fields[3].hasRange);
    CHECK(entry.fields[3].rangeMin == doctest::Approx(0.0));
    CHECK(entry.fields[3].rangeMax == doctest::Approx(1.5707964));
    CHECK(entry.fields[4].hasRange);
    CHECK(entry.fields[4].rangeMin == doctest::Approx(0.0));
    CHECK(entry.fields[4].rangeMax == doctest::Approx(1.5707964));

    // intensity and range carry NEITHER a range nor a colour -- 1.3.3's D19. BOTH directions on
    // each, because an absent flag asserted only one way is half a statement.
    CHECK_FALSE(entry.fields[1].hasRange);
    CHECK_FALSE(entry.fields[1].color);
    CHECK_FALSE(entry.fields[2].hasRange);
    CHECK_FALSE(entry.fields[2].color);

    // ...and the colour carries no range, nor do the two cone angles render as pickers.
    CHECK_FALSE(entry.fields[0].hasRange);
    CHECK_FALSE(entry.fields[3].color);
    CHECK_FALSE(entry.fields[4].color);
}

// ================================================================================================
// task E.3.1 (VF1-VF15) -- the axis row, asserted as VALUES. No ImGui context exists in this target
// and none is needed: every decision the Vec3/Quat rows make -- which kinds get axes, which letter,
// which colour, what a reset writes, whether it is live, how wide the label column is -- is a pure
// function here, so a tier-0 case asserts exactly what a person sees.
//
// PLACED AT THE END, below the AC-12 drift pin, for that case's own reason: registerEditorReflection
// is process-lifetime and permanent, and every entt::meta_reset() in this TU is ABOVE the pin. These
// cases need no registry at all -- they are functions on FieldValue -- so NONE of them resets.
// ================================================================================================
namespace {

using engine::editor::AXIS_ROW_COMPONENTS;
using engine::editor::AxisResetAction;
using engine::editor::axisResetAction;
using engine::editor::axisRowColor;
using engine::editor::axisRowFieldValue;
using engine::editor::axisRowLabel;
using engine::editor::axisRowValues;
using engine::editor::inspectorLabelColumnWidth;
using engine::editor::isAxisRow;

// The three bytes, compared as INTEGERS: a std::uint8_t inside a CHECK stringifies as a character,
// which turns a readable "226 == 125" failure into two unprintable glyphs.
void checkAxisColor(std::size_t index, const std::array<std::uint8_t, 3>& expected) {
    const std::array<std::uint8_t, 3> actual = axisRowColor(index);
    CAPTURE(index);
    CHECK(static_cast<int>(actual[0]) == static_cast<int>(expected[0]));
    CHECK(static_cast<int>(actual[1]) == static_cast<int>(expected[1]));
    CHECK(static_cast<int>(actual[2]) == static_cast<int>(expected[2]));
}

// A Quat from a euler triple in DEGREES -- the shape every rotation case below poses with.
engine::Quat quatFromDegrees(float x, float y, float z) {
    return engine::normalize(
        engine::fromEulerAngles(engine::Vec3{engine::radians(x), engine::radians(y), engine::radians(z)}));
}

}  // namespace

TEST_CASE("inspector: isAxisRow over all eight kinds, both colour flags (task E.3.1, VF1)") {
    // SIXTEEN ARMS, WRITTEN OUT rather than looped: the point is that each kind's answer is stated
    // here, so a switch that grew a wrong arm reddens on that arm's own line. A loop over an
    // expectation table would restate the implementation's own switch and assert nothing.
    CHECK_FALSE(isAxisRow(FieldKind::Bool, false));
    CHECK_FALSE(isAxisRow(FieldKind::Bool, true));
    CHECK_FALSE(isAxisRow(FieldKind::Int, false));
    CHECK_FALSE(isAxisRow(FieldKind::Int, true));
    CHECK_FALSE(isAxisRow(FieldKind::UInt, false));
    CHECK_FALSE(isAxisRow(FieldKind::UInt, true));
    CHECK_FALSE(isAxisRow(FieldKind::Float, false));
    CHECK_FALSE(isAxisRow(FieldKind::Float, true));

    // The two that matter, and the ONE discriminator: an AERO_COLOR Vec3 keeps its picker.
    CHECK(isAxisRow(FieldKind::Vec3, false));
    CHECK_FALSE(isAxisRow(FieldKind::Vec3, true));

    // A Quat has no colour flag to carry, so it is an axis row either way -- asserted in BOTH
    // directions, because "true when false" alone would not catch an implementation that keyed a
    // Quat off `color` too.
    CHECK(isAxisRow(FieldKind::Quat, false));
    CHECK(isAxisRow(FieldKind::Quat, true));

    CHECK_FALSE(isAxisRow(FieldKind::String, false));
    CHECK_FALSE(isAxisRow(FieldKind::String, true));
    CHECK_FALSE(isAxisRow(FieldKind::Guid, false));
    CHECK_FALSE(isAxisRow(FieldKind::Guid, true));
}

TEST_CASE("inspector: axisRowLabel is X/Y/Z and TOTAL past the last axis (task E.3.1, VF2)") {
    CHECK(axisRowLabel(0) == "X");
    CHECK(axisRowLabel(1) == "Y");
    CHECK(axisRowLabel(2) == "Z");

    // TOTAL, not merely "not asserted": this is read inside a draw walk, where an out-of-bounds read
    // is a wrong glyph at best and a crash at worst. Three out-of-range indices, including the one an
    // unsigned underflow produces.
    CHECK(axisRowLabel(AXIS_ROW_COMPONENTS).empty());
    CHECK(axisRowLabel(4).empty());
    CHECK(axisRowLabel(std::numeric_limits<std::size_t>::max()).empty());
}

TEST_CASE("inspector: axisRowColor derives from the palette and is TOTAL (task E.3.1, VF3)") {
    // Compared against THE PALETTE'S OWN CONSTANTS, never against restated bytes: a palette edit then
    // moves both sides together and only a MAPPING error (X's colour on Y, say) moves one of them.
    // That is the difference between a pin and a copy.
    checkAxisColor(0, engine::editor::AXIS_X_SRGB);
    checkAxisColor(1, engine::editor::AXIS_Y_SRGB);
    checkAxisColor(2, engine::editor::AXIS_Z_SRGB);

    // ...and the three are actually DIFFERENT, without which the three checks above would all pass
    // for an implementation that returned one colour for every index.
    CHECK(static_cast<int>(axisRowColor(0)[0]) != static_cast<int>(axisRowColor(1)[0]));
    CHECK(static_cast<int>(axisRowColor(1)[1]) != static_cast<int>(axisRowColor(2)[1]));

    // Past the last axis: ImGui's own neutral fourth marker, NOT X's red. An implementation that
    // clamped to index 0 would be invisible without this line.
    checkAxisColor(AXIS_ROW_COMPONENTS, std::array<std::uint8_t, 3>{140U, 140U, 140U});
    checkAxisColor(std::numeric_limits<std::size_t>::max(), std::array<std::uint8_t, 3>{140U, 140U, 140U});
}

TEST_CASE("inspector: axisRowValues carries a Vec3 through bit-exactly (task E.3.1, VF4)") {
    // EXACT equality, not Approx: the Vec3 path performs no arithmetic at all, so any tolerance here
    // would admit an implementation that did.
    const engine::Vec3 v{-3.5F, 0.0F, 17.25F};
    const std::array<float, 3> shown = axisRowValues(FieldValue{v}, FieldKind::Vec3);
    CHECK(shown[0] == -3.5F);
    CHECK(shown[1] == 0.0F);
    CHECK(shown[2] == 17.25F);

    // A non-axis kind answers zeros rather than reading the variant -- it is TOTAL, like the label.
    const std::array<float, 3> notAxis = axisRowValues(FieldValue{v}, FieldKind::Float);
    CHECK(notAxis[0] == 0.0F);
    CHECK(notAxis[1] == 0.0F);
    CHECK(notAxis[2] == 0.0F);

    // ...and so is a kind/variant MISMATCH, which must answer rather than throw (no exception may
    // cross this API).
    const std::array<float, 3> mismatched = axisRowValues(FieldValue{double{7.0}}, FieldKind::Vec3);
    CHECK(mismatched[0] == 0.0F);
    CHECK(mismatched[1] == 0.0F);
    CHECK(mismatched[2] == 0.0F);
}

TEST_CASE("inspector: axisRowValues shows a Quat as euler DEGREES (task E.3.1, VF5)") {
    // The expectation is computed off `q` itself, never read back out of the function under test
    // (GR8's rule: a case that compares two values from the same source asserts nothing).
    const engine::Quat q{0.1F, 0.2F, 0.3F, 0.927F};
    const engine::Vec3 radiansTriple = engine::eulerAngles(q);
    const std::array<float, 3> shown = axisRowValues(FieldValue{q}, FieldKind::Quat);
    CHECK(shown[0] == doctest::Approx(engine::degrees(radiansTriple.x)).epsilon(1e-5));
    CHECK(shown[1] == doctest::Approx(engine::degrees(radiansTriple.y)).epsilon(1e-5));
    CHECK(shown[2] == doctest::Approx(engine::degrees(radiansTriple.z)).epsilon(1e-5));

    // DEGREES, not radians, stated as a magnitude a radian answer could never reach: 0.3 rad is
    // 17.2 degrees, and the two differ by 57x.
    CHECK(std::abs(shown[2]) > 5.0F);

    // A SECOND, genuinely independent witness: pose from a known benign euler triple and read it
    // back. This one does not go through eulerAngles on the expectation side at all.
    const std::array<float, 3> roundTrip =
        axisRowValues(FieldValue{quatFromDegrees(30.0F, 20.0F, 40.0F)}, FieldKind::Quat);
    CHECK(roundTrip[0] == doctest::Approx(30.0F).epsilon(1e-4));
    CHECK(roundTrip[1] == doctest::Approx(20.0F).epsilon(1e-4));
    CHECK(roundTrip[2] == doctest::Approx(40.0F).epsilon(1e-4));

    // eulerAngles(identity).y is -0.0F on this backend, which compares EQUAL to +0.0F -- recorded so
    // the next reader does not chase a sign that is not a defect.
    const std::array<float, 3> identityShown = axisRowValues(FieldValue{engine::Quat::identity()}, FieldKind::Quat);
    CHECK(identityShown[0] == 0.0F);
    CHECK(identityShown[1] == 0.0F);
    CHECK(identityShown[2] == 0.0F);
}

TEST_CASE("inspector: axisRowFieldValue rebuilds a Vec3 exactly and a Quat NORMALIZED (task E.3.1, VF6)") {
    const std::array<float, 3> shown{-3.5F, 0.0F, 17.25F};
    const FieldValue rebuilt = axisRowFieldValue(shown, FieldKind::Vec3);
    REQUIRE(std::holds_alternative<engine::Vec3>(rebuilt));
    CHECK(std::get<engine::Vec3>(rebuilt).x == -3.5F);
    CHECK(std::get<engine::Vec3>(rebuilt).y == 0.0F);
    CHECK(std::get<engine::Vec3>(rebuilt).z == 17.25F);

    // fromEulerAngles does NOT normalize (quat.hpp says so: measured length 0.99999994), so the
    // normalize() this function applies is load-bearing, not decoration -- a stored non-unit rotation
    // drifts every time it is composed.
    const std::array<float, 3> degreesTriple{30.0F, 20.0F, 40.0F};
    const FieldValue rotation = axisRowFieldValue(degreesTriple, FieldKind::Quat);
    REQUIRE(std::holds_alternative<engine::Quat>(rotation));
    CHECK(engine::length(std::get<engine::Quat>(rotation)) == doctest::Approx(1.0F).epsilon(1e-6));

    // THE LENGTH CHECK ABOVE CANNOT DISCRIMINATE, and saying so is the point rather than a caveat:
    // GLM's euler constructor is already unit to about 6e-8, which any sane relative tolerance
    // admits, so dropping normalize() leaves that line GREEN. Measured directly -- a sabotage seed
    // that removed the call reddened nothing at all in this whole battery. What discriminates is the
    // BITS.
    const engine::Vec3 radiansTriple{engine::radians(degreesTriple[0]), engine::radians(degreesTriple[1]),
                                     engine::radians(degreesTriple[2])};
    const engine::Quat raw = engine::fromEulerAngles(radiansTriple);
    // The DIFFERENCE, not the length: at default ostream precision 0.99999994 prints as "1",
    // which would make this line read as evidence of the opposite of what it measures.
    MESSAGE("VF6 euler-constructor length minus one: " << (engine::length(raw) - 1.0F));
    const engine::Quat got = std::get<engine::Quat>(rotation);
    const engine::Quat expected = engine::normalize(raw);
    const bool matchesNormalized =
        got.x == expected.x && got.y == expected.y && got.z == expected.z && got.w == expected.w;
    CHECK(matchesNormalized);

    // ANTI-VACUITY: normalize() really does move the bits at this pose, which is what makes the check
    // above a statement rather than a tautology. On a toolchain whose euler constructor returned an
    // exactly unit quaternion this would redden and the MESSAGE above would say why -- a loud, honest
    // failure in place of a silently vacuous pass.
    const bool differsFromRaw = got.x != raw.x || got.y != raw.y || got.z != raw.z || got.w != raw.w;
    CHECK(differsFromRaw);

    // ...and it is the RIGHT rotation, not merely a unit one: the triple comes back out.
    const std::array<float, 3> back = axisRowValues(rotation, FieldKind::Quat);
    CHECK(back[0] == doctest::Approx(30.0F).epsilon(1e-4));
    CHECK(back[1] == doctest::Approx(20.0F).epsilon(1e-4));
    CHECK(back[2] == doctest::Approx(40.0F).epsilon(1e-4));
}

TEST_CASE("inspector: a per-axis Vec3 reset moves ONE component (task E.3.1, VF7)") {
    const FieldValue current{engine::Vec3{3.0F, 4.0F, 5.0F}};
    const std::optional<FieldValue> defaultValue{FieldValue{engine::Vec3{1.0F, 1.0F, 1.0F}}};

    for (std::size_t axis = 0; axis < AXIS_ROW_COMPONENTS; ++axis) {
        CAPTURE(axis);
        const AxisResetAction action = axisResetAction(axis, FieldKind::Vec3, current, defaultValue);
        CHECK(action.enabled);
        REQUIRE(std::holds_alternative<engine::Vec3>(action.result));
        const std::array<float, 3> after = axisRowValues(action.result, FieldKind::Vec3);
        const std::array<float, 3> before = axisRowValues(current, FieldKind::Vec3);
        const std::array<float, 3> defaults = axisRowValues(*defaultValue, FieldKind::Vec3);
        for (std::size_t i = 0; i < AXIS_ROW_COMPONENTS; ++i) {
            CAPTURE(i);
            // BIT-EXACT on all three: the reset axis takes the default's own component and the other
            // two are untouched, so a "reset all three" implementation reddens on the other two and a
            // "reset a rounded default" one reddens on this one.
            CHECK(after[i] == (i == axis ? defaults[i] : before[i]));
        }
    }
}

TEST_CASE("inspector: a whole-field Quat reset writes the default BITWISE (task E.3.1, VF8)") {
    // Routing the whole-field case through euler would land a value that is approxEquals to the
    // default but not == to it, which would leave `enabled` true FOREVER -- the menu entry would stay
    // live after the reset and every click would push another undo entry. Bitwise is the assertion.
    const engine::Quat defaultQuat = quatFromDegrees(5.0F, -7.0F, 11.0F);
    const FieldValue current{quatFromDegrees(30.0F, 20.0F, 40.0F)};
    const std::optional<FieldValue> defaultValue{FieldValue{defaultQuat}};

    const AxisResetAction all = axisResetAction(std::nullopt, FieldKind::Quat, current, defaultValue);
    CHECK(all.enabled);
    CHECK((all.result == FieldValue{defaultQuat}));
    CHECK(all.label == "Reset to default");

    // ...and the SECOND reset from there is disabled, which is the property the bitwise write buys.
    const AxisResetAction again = axisResetAction(std::nullopt, FieldKind::Quat, all.result, defaultValue);
    CHECK_FALSE(again.enabled);
}

TEST_CASE("inspector: no default means a DISABLED entry that still reads sensibly (task E.3.1, VF9)") {
    const FieldValue current{engine::Vec3{3.0F, 4.0F, 5.0F}};
    const std::optional<FieldValue> noDefault;

    const AxisResetAction one = axisResetAction(std::size_t{0}, FieldKind::Vec3, current, noDefault);
    CHECK_FALSE(one.enabled);
    CHECK_FALSE(one.label.empty());  // greyed out, never blank

    const AxisResetAction all = axisResetAction(std::nullopt, FieldKind::Vec3, current, noDefault);
    CHECK_FALSE(all.enabled);
    CHECK_FALSE(all.label.empty());
    CHECK(all.label == "Reset to default");
}

TEST_CASE("inspector: a reset that would change NOTHING is disabled (task E.3.1, VF10)") {
    const engine::Vec3 one{1.0F, 1.0F, 1.0F};
    const std::optional<FieldValue> defaultValue{FieldValue{one}};

    const FieldValue atDefault{one};
    CHECK_FALSE(axisResetAction(std::size_t{0}, FieldKind::Vec3, atDefault, defaultValue).enabled);
    CHECK_FALSE(axisResetAction(std::nullopt, FieldKind::Vec3, atDefault, defaultValue).enabled);

    // ANTI-VACUITY: move ONE component off the default and the SAME two calls report enabled. Without
    // this arm, an implementation that hardcoded `enabled = false` would pass the two checks above.
    const FieldValue moved{engine::Vec3{1.0F, 2.0F, 1.0F}};
    CHECK(axisResetAction(std::size_t{1}, FieldKind::Vec3, moved, defaultValue).enabled);
    CHECK(axisResetAction(std::nullopt, FieldKind::Vec3, moved, defaultValue).enabled);

    // ...and the axis that is ALREADY at its default stays disabled even while the field as a whole
    // differs -- the per-axis decision is per axis, not "does anything differ".
    CHECK_FALSE(axisResetAction(std::size_t{0}, FieldKind::Vec3, moved, defaultValue).enabled);
}

TEST_CASE("inspector: the reset entries' exact text (task E.3.1, VF11)") {
    // BYTE-EXACT, because this is what a person reads. "%.3f" is ImGuiDataType_Float's own PrintFmt,
    // so the number in the menu is the number in the drag box beside it.
    const FieldValue current{engine::Vec3{3.0F, 4.0F, 5.0F}};
    const std::optional<FieldValue> defaultValue{FieldValue{engine::Vec3{1.0F, -2.5F, 0.0F}}};

    CHECK(axisResetAction(std::size_t{0}, FieldKind::Vec3, current, defaultValue).label == "Reset X to 1.000");
    CHECK(axisResetAction(std::size_t{1}, FieldKind::Vec3, current, defaultValue).label == "Reset Y to -2.500");
    CHECK(axisResetAction(std::size_t{2}, FieldKind::Vec3, current, defaultValue).label == "Reset Z to 0.000");
    CHECK(axisResetAction(std::nullopt, FieldKind::Vec3, current, defaultValue).label == "Reset to default");

    // The whole-field label carries NO number on purpose: a Vec3 or Quat default has no one-number
    // spelling, and "Reset to (1.000, 1.000, 1.000)" in a context menu is noise.
    CHECK(axisResetAction(std::nullopt, FieldKind::Quat, FieldValue{engine::Quat::identity()},
                          std::optional<FieldValue>{FieldValue{engine::Quat::identity()}})
              .label == "Reset to default");

    // NOT ASSERTED, deliberately: a non-finite value's "%.3f" is "nan" on libc++ and can be
    // "-nan(ind)" on MSVC, so a label assertion over one would redden on Windows alone.
}

TEST_CASE("inspector: `enabled` compares with ==, so NaN stays live and -0.0 does not (task E.3.1, VF12)") {
    // Vec3 ONLY. A non-finite triple must never reach the Quat arm of axisRowFieldValue:
    // normalize(Quat) ASSERTS on a zero/NaN quaternion and would abort the Debug/sanitizer binary.
    const std::optional<FieldValue> defaultValue{FieldValue{engine::Vec3{0.0F, 0.0F, 0.0F}}};

    const FieldValue withNan{engine::Vec3{std::numeric_limits<float>::quiet_NaN(), 1.0F, 1.0F}};
    // NaN != NaN, so a field carrying one never equals its default and the reset stays LIVE -- which
    // is exactly the rescue a user looking at "nan" in a box wants.
    CHECK(axisResetAction(std::nullopt, FieldKind::Vec3, withNan, defaultValue).enabled);
    CHECK(axisResetAction(std::size_t{1}, FieldKind::Vec3, withNan, defaultValue).enabled);

    // -0.0F == +0.0F, so a sign-bit-only write costs NO undo entry. An approxEquals-based comparison
    // would agree here, which is why the arm above (NaN) is the one that discriminates; this arm is
    // what a `!=`-on-bits implementation would redden.
    const FieldValue negativeZero{engine::Vec3{-0.0F, -0.0F, -0.0F}};
    CHECK_FALSE(axisResetAction(std::nullopt, FieldKind::Vec3, negativeZero, defaultValue).enabled);
    CHECK_FALSE(axisResetAction(std::size_t{0}, FieldKind::Vec3, negativeZero, defaultValue).enabled);

    // A DIFFERENCE SMALLER THAN EPSILON IS STILL A DIFFERENCE, and this is the ONLY arm that tells
    // `==` from approxEquals. Neither arm above can: both comparators call NaN unequal to everything
    // and both call -0.0F equal to +0.0F. Measured -- a sabotage seed swapping the comparator for
    // approxEquals reddened nothing until this arm existed.
    const engine::Vec3 unitDefault{1.0F, 1.0F, 1.0F};
    const std::optional<FieldValue> unitDefaultValue{FieldValue{unitDefault}};
    const engine::Vec3 nudged{1.0F + (engine::EPSILON * 0.5F), 1.0F, 1.0F};
    REQUIRE(nudged.x != unitDefault.x);                  // the nudge really moved the bits
    REQUIRE(engine::approxEquals(nudged, unitDefault));  // ...and approxEquals calls the two EQUAL
    CHECK(axisResetAction(std::nullopt, FieldKind::Vec3, FieldValue{nudged}, unitDefaultValue).enabled);
    CHECK(axisResetAction(std::size_t{0}, FieldKind::Vec3, FieldValue{nudged}, unitDefaultValue).enabled);
}

TEST_CASE("inspector: a non-axis kind resets as a WHOLE FIELD, with or without an axis (task E.3.1, VF13)") {
    // Defensive, and the reason it is one line rather than an assert: drawFieldResetMenu already gates
    // on isAxisRow, so this path is unreachable from the panel -- but a future caller that forgets is
    // better served by a correct whole-field reset than by a wrong per-axis one.
    struct Case {
        FieldKind kind;
        FieldValue current;
        FieldValue defaultValue;
    };
    const std::array<Case, 4> cases{
        Case{FieldKind::Float, FieldValue{double{9.5}}, FieldValue{double{1.0}}},
        Case{FieldKind::Bool, FieldValue{true}, FieldValue{false}},
        Case{FieldKind::String, FieldValue{std::string{"typed"}}, FieldValue{std::string{}}},
        Case{FieldKind::Guid, FieldValue{engine::Guid{1ULL, 2ULL}}, FieldValue{engine::Guid{}}},
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        CAPTURE(i);
        const Case& one = cases[i];
        const std::optional<FieldValue> defaultValue{one.defaultValue};
        const AxisResetAction all = axisResetAction(std::nullopt, one.kind, one.current, defaultValue);
        CHECK(all.enabled);
        CHECK((all.result == one.defaultValue));
        CHECK(all.label == "Reset to default");

        // ...and naming an axis on a kind that has none changes NOTHING, including the label.
        const AxisResetAction withAxis = axisResetAction(std::size_t{0}, one.kind, one.current, defaultValue);
        CHECK(withAxis.enabled);
        CHECK((withAxis.result == one.defaultValue));
        CHECK(withAxis.label == "Reset to default");
    }

    // An OUT-OF-RANGE axis on a kind that DOES have axes falls to the whole field too, rather than
    // indexing past a std::array.
    const std::optional<FieldValue> vecDefault{FieldValue{engine::Vec3{1.0F, 1.0F, 1.0F}}};
    const FieldValue vecCurrent{engine::Vec3{3.0F, 4.0F, 5.0F}};
    const AxisResetAction past = axisResetAction(AXIS_ROW_COMPONENTS, FieldKind::Vec3, vecCurrent, vecDefault);
    CHECK(past.enabled);
    CHECK((past.result == *vecDefault));
    CHECK(past.label == "Reset to default");
}

TEST_CASE("inspector: three per-axis Quat resets converge on the default (task E.3.1, VF14)") {
    // A BENIGN POSE, and the choice is measured rather than taste. The euler triplet is
    // (X = pitch, Y = yaw, Z = roll) and the YAW (Y) component comes from asin(), confined to
    // [-pi/2, pi/2]: near |Y| = 90 degrees the decomposition is ill-conditioned and a three-reset
    // sequence from (10, 89.999, 25) leaves a 0.838-DEGREE residual, which no sane tolerance admits.
    // At the poses below the worst residual measured off-tree is 9.54e-07 deg, so epsilon(1e-4) in
    // DEGREES has roughly 105x headroom. (Approx's scale term is what keeps the identity arm, whose
    // expectation is 0.0, from needing an absolute tolerance of its own.)
    const FieldValue start{quatFromDegrees(30.0F, 20.0F, 40.0F)};

    SUBCASE("to identity") {
        const std::optional<FieldValue> defaultValue{FieldValue{engine::Quat::identity()}};
        FieldValue current = start;
        for (std::size_t axis = 0; axis < AXIS_ROW_COMPONENTS; ++axis) {
            const AxisResetAction action = axisResetAction(axis, FieldKind::Quat, current, defaultValue);
            CAPTURE(axis);
            CHECK(action.enabled);
            current = action.result;
        }
        const std::array<float, 3> shown = axisRowValues(current, FieldKind::Quat);
        CHECK(shown[0] == doctest::Approx(0.0F).epsilon(1e-4));
        CHECK(shown[1] == doctest::Approx(0.0F).epsilon(1e-4));
        CHECK(shown[2] == doctest::Approx(0.0F).epsilon(1e-4));
    }

    SUBCASE("to a non-identity default") {
        const std::optional<FieldValue> defaultValue{FieldValue{quatFromDegrees(5.0F, -7.0F, 11.0F)}};
        FieldValue current = start;
        for (std::size_t axis = 0; axis < AXIS_ROW_COMPONENTS; ++axis) {
            const AxisResetAction action = axisResetAction(axis, FieldKind::Quat, current, defaultValue);
            CAPTURE(axis);
            CHECK(action.enabled);
            current = action.result;
        }
        const std::array<float, 3> shown = axisRowValues(current, FieldKind::Quat);
        CHECK(shown[0] == doctest::Approx(5.0F).epsilon(1e-4));
        CHECK(shown[1] == doctest::Approx(-7.0F).epsilon(1e-4));
        CHECK(shown[2] == doctest::Approx(11.0F).epsilon(1e-4));
    }
}

TEST_CASE("inspector: the label column's width is clamped, and the clamp cannot cross (task E.3.1, VF15)") {
    // The panel measures the four inputs with ImGui and this decides, which is what makes the clamp
    // reachable from a tier-0 binary with no ImGui context at all.
    constexpr float FONT = 13.0F;
    constexpr float PADDING = 4.0F;
    constexpr float FLOOR = FONT * 5.0F;  // 65

    SUBCASE("(a) an ordinary case is the measured width plus both cell paddings") {
        const float width = inspectorLabelColumnWidth(100.0F, PADDING, FONT, 400.0F);
        CHECK(width == doctest::Approx(108.0F).epsilon(1e-6));
        // ANTI-VACUITY: (a) is neither already at the floor nor already at the ceiling, so it really
        // is the unclamped arm. Without this, (a) would also pass for an implementation that always
        // returned the floor.
        CHECK(width > FLOOR);
        CHECK(width < 400.0F * 0.5F);
    }

    SUBCASE("(b) a tiny label is raised to the floor") {
        CHECK(inspectorLabelColumnWidth(4.0F, PADDING, FONT, 400.0F) == doctest::Approx(FLOOR).epsilon(1e-6));
        CHECK(inspectorLabelColumnWidth(0.0F, 0.0F, FONT, 400.0F) == doctest::Approx(FLOOR).epsilon(1e-6));
    }

    SUBCASE("(c) a huge label is capped at half the available width") {
        CHECK(inspectorLabelColumnWidth(5000.0F, PADDING, FONT, 400.0F) == doctest::Approx(200.0F).epsilon(1e-6));
    }

    SUBCASE("(d) a zero- or negative-width dock returns the floor and never crosses the clamp") {
        // std::clamp with lo > hi is UNDEFINED BEHAVIOUR, and both of these reach it without the
        // std::max in the ceiling. A green run under the Debug lane's UBSan is half the proof; the
        // returned value is the other half.
        CHECK(inspectorLabelColumnWidth(100.0F, PADDING, FONT, 0.0F) == doctest::Approx(FLOOR).epsilon(1e-6));
        CHECK(inspectorLabelColumnWidth(100.0F, PADDING, FONT, -250.0F) == doctest::Approx(FLOOR).epsilon(1e-6));
        CHECK(inspectorLabelColumnWidth(1.0F, 0.0F, FONT, -1.0F) == doctest::Approx(FLOOR).epsilon(1e-6));
    }

    SUBCASE("(e) the floor scales with the font, so it is a font-relative rule rather than a constant") {
        CHECK(inspectorLabelColumnWidth(4.0F, PADDING, 26.0F, 4000.0F) == doctest::Approx(130.0F).epsilon(1e-6));
    }
}

// ================================================================================================
// task E.3.1 (FD1-FD10) -- defaultComponentField: the value a reset writes.
//
// Appended below the VF battery, i.e. below the AC-12 drift pin, where registerEditorReflection's
// process-lifetime registration is already in place. NO CASE HERE CALLS entt::meta_reset(); each one
// that needs a built-in calls registerEditorReflection() itself, the shape the AudioSource case above
// states as a rule (a case that rides a neighbour's registration reads zero fields when run alone).
// ================================================================================================
namespace {

using engine::editor::defaultComponentField;

// The RAII log-capture guard: its destructor detaches, which a code-review round required after the
// bare form was found to be a latent use-after-free (log.hpp -- detaching does NOT guarantee the
// captured state may be destroyed). render_sky_test.cpp's own shape, copied rather than shared
// because this TU links neither that file nor a test-support library that could hold it.
struct LogCallbackGuard {
    ~LogCallbackGuard() { engine::setLogCallback({}); }
    LogCallbackGuard() = default;
    LogCallbackGuard(const LogCallbackGuard&) = delete;
    LogCallbackGuard& operator=(const LogCallbackGuard&) = delete;
    LogCallbackGuard(LogCallbackGuard&&) = delete;
    LogCallbackGuard& operator=(LogCallbackGuard&&) = delete;
};

// A component type the World can hold (default-constructible, so World::addRaw works) registered
// under a name whose entt::meta belongs to a DIFFERENT, non-default-constructible type. That
// mismatch is the only way to reach defaultComponentField's not-default-constructible arm at all:
// resolveComponentMeta joins the two registries by NAME, and World::addRaw default-constructs, so an
// ordinary registration can never produce a meta type whose construct() fails.
struct FdNoDefaultCarrier {
    int value = 0;
};
struct FdNoDefault {
    explicit FdNoDefault(int v) : value(v) {}
    int value;
};

// Once per process: entt::meta_factory APPENDS, so a second `.data<>` call would register the member
// twice. Safe below the drift pin, where nothing calls entt::meta_reset().
void registerFdNoDefaultMeta() {
    static const bool registered = [] {
        using namespace entt::literals;
        entt::meta_factory<FdNoDefault>{}
            .type("FdNoDefaultProbe"_hs, "FdNoDefaultProbe")
            .data<&FdNoDefault::value>("value"_hs, "value");
        return true;
    }();
    // NOT a CHECK: `registered` is true by construction, and an assertion that cannot fail must never
    // be presented as one. It exists to name the initialiser's side effect.
    (void)registered;
}

}  // namespace

TEST_CASE("inspector: engine::Transform's defaults -- scale is (1,1,1), not zero (task E.3.1, FD1)") {
    engine::editor::registerEditorReflection();

    World world;
    const ComponentTypeId id = world.findComponentType("engine::Transform");
    REQUIRE(id.valid());
    const Entity e = world.create();
    REQUIRE(world.addRaw(id, e, nullptr) != nullptr);

    // SEEDED OFF-DEFAULT FIRST, and the case is vacuous without this: an implementation that read the
    // LIVE component would return (1,1,1) too, because a freshly added Transform already holds it.
    REQUIRE(writeComponentField(world, e, id, "scale", FieldValue{engine::Vec3{7.0F, 8.0F, 9.0F}}));
    REQUIRE(writeComponentField(world, e, id, "position", FieldValue{engine::Vec3{4.0F, 5.0F, 6.0F}}));

    const std::optional<FieldValue> scale = defaultComponentField(world, id, "scale");
    REQUIRE(scale.has_value());
    REQUIRE(std::holds_alternative<engine::Vec3>(*scale));
    // THE HEADLINE. A reset that wrote zero would leave the object invisible, which is the single
    // most user-hostile thing this feature could do.
    CHECK(std::get<engine::Vec3>(*scale).x == 1.0F);
    CHECK(std::get<engine::Vec3>(*scale).y == 1.0F);
    CHECK(std::get<engine::Vec3>(*scale).z == 1.0F);

    const std::optional<FieldValue> position = defaultComponentField(world, id, "position");
    REQUIRE(position.has_value());
    REQUIRE(std::holds_alternative<engine::Vec3>(*position));
    CHECK(std::get<engine::Vec3>(*position).x == 0.0F);
    CHECK(std::get<engine::Vec3>(*position).y == 0.0F);
    CHECK(std::get<engine::Vec3>(*position).z == 0.0F);

    const std::optional<FieldValue> rotation = defaultComponentField(world, id, "rotation");
    REQUIRE(rotation.has_value());
    REQUIRE(std::holds_alternative<engine::Quat>(*rotation));
    CHECK((std::get<engine::Quat>(*rotation) == engine::Quat::identity()));
}

TEST_CASE("inspector: every reflected field of every built-in resolves a default (task E.3.1, FD2)") {
    // A DRIFT PIN, not a proof: it cannot reach the not-default-constructible branch, because every
    // built-in IS default-constructible -- FD9 below is what closes that. What this catches is a
    // future built-in, or a future field, that stops resolving one.
    engine::editor::registerEditorReflection();

    World world;
    const std::size_t count = world.componentTypeCount();
    REQUIRE(count == 10);  // the 10 built-ins (E.2.2) -- the AC-12 case's own shape

    const Entity e = world.create();
    for (std::size_t i = 0; i < count; ++i) {
        world.addRaw(world.componentTypeAt(i), e, nullptr);
    }

    InspectorModel model;
    buildInspectorModel(world, e, model);
    REQUIRE(model.components.size() == count);

    std::size_t fieldsChecked = 0;
    for (const engine::editor::ComponentEntry& entry : model.components) {
        REQUIRE(entry.hasFields);
        REQUIRE_FALSE(entry.fields.empty());
        for (const engine::editor::FieldEntry& field : entry.fields) {
            CAPTURE(entry.name);
            CAPTURE(field.name);
            const std::optional<FieldValue> value = defaultComponentField(world, entry.typeId, field.name);
            REQUIRE(value.has_value());
            // ...and the ALTERNATIVE matches the kind the model reports, so a default that resolved
            // as some other type would redden here rather than reaching the panel as a kind mismatch.
            CHECK(value->index() == field.value.index());
            ++fieldsChecked;
        }
    }
    // ANTI-VACUITY: a loop over an empty model would pass every line above.
    CHECK(fieldsChecked > 30);
}

TEST_CASE("inspector: defaultComponentField's four rejections (task E.3.1, FD3, FD4, FD5, FD9)") {
    engine::editor::registerEditorReflection();

    World world;
    const ComponentTypeId transformId = world.findComponentType("engine::Transform");
    REQUIRE(transformId.valid());
    const Entity e = world.create();
    REQUIRE(world.addRaw(transformId, e, nullptr) != nullptr);
    REQUIRE(writeComponentField(world, e, transformId, "scale", FieldValue{engine::Vec3{7.0F, 8.0F, 9.0F}}));

    SUBCASE("FD3: an unknown field name, and the component on the entity is untouched") {
        CHECK_FALSE(defaultComponentField(world, transformId, "nope").has_value());
        const std::optional<FieldValue> live = readComponentField(world, e, transformId, "scale");
        REQUIRE(live.has_value());
        REQUIRE(std::holds_alternative<engine::Vec3>(*live));
        CHECK(std::get<engine::Vec3>(*live).x == 7.0F);
        CHECK(std::get<engine::Vec3>(*live).y == 8.0F);
        CHECK(std::get<engine::Vec3>(*live).z == 9.0F);
    }

    SUBCASE("FD4: an unregistered component id") {
        CHECK_FALSE(defaultComponentField(world, ComponentTypeId{}, "scale").has_value());
    }

    SUBCASE("FD5: a World-registered but META-LESS type -- 2.2.2's E4 asymmetry, one layer over") {
        const ComponentTypeId markerId = registerComponent<InspectorMarker>(world, "InspectorMarker");
        REQUIRE(markerId.valid());
        CHECK_FALSE(defaultComponentField(world, markerId, "payload").has_value());
    }

    SUBCASE("FD9: a meta type that is NOT default-constructible") {
        registerFdNoDefaultMeta();
        const ComponentTypeId id = registerComponent<FdNoDefaultCarrier>(world, "FdNoDefaultProbe");
        REQUIRE(id.valid());
        // The name resolves to real meta -- so this is NOT the FD5 arm wearing a different hat, and
        // the field really exists on that meta type.
        REQUIRE(engine::editor::componentFieldsAreReflected(world, id));
        CHECK_FALSE(defaultComponentField(world, id, "value").has_value());
    }
}

TEST_CASE("inspector: defaultComponentField MUTATES NOTHING (task E.3.1, FD6)") {
    // construct() builds a SEPARATE instance rather than a view of a live one. Seeded off-default so
    // "the live value is unchanged" and "the default is (1,1,1)" are two distinguishable facts.
    engine::editor::registerEditorReflection();

    World world;
    const ComponentTypeId id = world.findComponentType("engine::Transform");
    REQUIRE(id.valid());
    const Entity e = world.create();
    REQUIRE(world.addRaw(id, e, nullptr) != nullptr);
    REQUIRE(writeComponentField(world, e, id, "scale", FieldValue{engine::Vec3{7.0F, 8.0F, 9.0F}}));

    const std::optional<FieldValue> before = readComponentField(world, e, id, "scale");
    REQUIRE(before.has_value());

    const std::optional<FieldValue> defaults = defaultComponentField(world, id, "scale");
    REQUIRE(defaults.has_value());
    REQUIRE(std::holds_alternative<engine::Vec3>(*defaults));
    CHECK(std::get<engine::Vec3>(*defaults).x == 1.0F);

    const std::optional<FieldValue> after = readComponentField(world, e, id, "scale");
    REQUIRE(after.has_value());
    CHECK((*after == *before));  // byte-identical, through the variant's own ==
    CHECK(std::get<engine::Vec3>(*after).x == 7.0F);
    CHECK(std::get<engine::Vec3>(*after).y == 8.0F);
    CHECK(std::get<engine::Vec3>(*after).z == 9.0F);
}

TEST_CASE("inspector: the default is the MEMBER INITIALISER, never the AERO_RANGE minimum (task E.3.1, FD7)") {
    // TWO fields, because one cannot discriminate both wrong readings.
    World world;
    aero_reflect_register_all_aero_editor_inspector_test();
    const ComponentTypeId probeId = registerProbe(world);
    REQUIRE(probeId.valid());
    const Entity e = world.create();
    world.addRaw(probeId, e, nullptr);

    // `speed`: member init 1.0f, AERO_RANGE(0.0f, 10.0f). Separates the real default from BOTH
    // "zero" and "rangeMin" at once, since those two coincide here.
    const std::optional<FieldValue> speed = defaultComponentField(world, probeId, "speed");
    REQUIRE(speed.has_value());
    REQUIRE(std::holds_alternative<double>(*speed));
    CHECK(std::get<double>(*speed) == doctest::Approx(1.0).epsilon(1e-9));

    // `hugeRange`: member init 0, AERO_RANGE(1e300, 2e300), destination std::int16_t. THE DECISIVE
    // ONE -- a rangeMin implementation clamps to 32767, which nothing else in this fixture produces.
    const std::optional<FieldValue> huge = defaultComponentField(world, probeId, "hugeRange");
    REQUIRE(huge.has_value());
    REQUIRE(std::holds_alternative<std::int64_t>(*huge));
    CHECK(std::get<std::int64_t>(*huge) == 0);
    CHECK(std::get<std::int64_t>(*huge) != std::numeric_limits<std::int16_t>::max());

    // ...and the colour Vec3, whose default is Vec3::one() -- the fixture's own (1,1,1) witness.
    const std::optional<FieldValue> tint = defaultComponentField(world, probeId, "tint");
    REQUIRE(tint.has_value());
    REQUIRE(std::holds_alternative<engine::Vec3>(*tint));
    CHECK(std::get<engine::Vec3>(*tint).x == 1.0F);
    CHECK(std::get<engine::Vec3>(*tint).y == 1.0F);
    CHECK(std::get<engine::Vec3>(*tint).z == 1.0F);

    // A Guid default is NIL, and nil is a VALUE rather than an absence.
    const std::optional<FieldValue> asset = defaultComponentField(world, probeId, "asset");
    REQUIRE(asset.has_value());
    REQUIRE(std::holds_alternative<engine::Guid>(*asset));
    CHECK_FALSE(std::get<engine::Guid>(*asset).valid());
}

TEST_CASE("inspector: defaultComponentField binds through a const World& (task E.3.1, FD8, compile-time)") {
    // The O1 shape, one seam entry over: this line would fail to COMPILE if the signature ever
    // widened to World&, which is what makes the constness a compiler fact rather than a promise.
    engine::editor::registerEditorReflection();

    World world;
    const ComponentTypeId id = world.findComponentType("engine::Transform");
    REQUIRE(id.valid());
    // A genuinely MUTABLE World, seeded through a mutating call -- a `const World` local would bind
    // trivially and would not state the property this case exists for.
    const Entity e = world.create();
    REQUIRE(world.addRaw(id, e, nullptr) != nullptr);

    const World& cw = world;
    const std::optional<FieldValue> scale = defaultComponentField(cw, id, "scale");
    REQUIRE(scale.has_value());
    CHECK(std::holds_alternative<engine::Vec3>(*scale));
}

TEST_CASE("inspector: every defaultComponentField rejection logs EXACTLY ONE error (task E.3.1, FD10)") {
    // The log assertion lives HERE, in one case, rather than bolted onto FD3/FD4/FD5/FD9 -- the sink
    // is a single global slot, and installing one inside four cases would put four global-state
    // installs where this TU has none today.
    engine::editor::registerEditorReflection();
    registerFdNoDefaultMeta();

    World world;
    const ComponentTypeId transformId = world.findComponentType("engine::Transform");
    REQUIRE(transformId.valid());
    const ComponentTypeId markerId = registerComponent<InspectorMarker>(world, "InspectorMarker");
    REQUIRE(markerId.valid());
    const ComponentTypeId noDefaultId = registerComponent<FdNoDefaultCarrier>(world, "FdNoDefaultProbe");
    REQUIRE(noDefaultId.valid());

    std::vector<std::string> records;
    {
        const LogCallbackGuard guard;
        engine::setLogCallback([&records](const engine::LogRecord& record) {
            // LogRecord::message is a view onto a caller-owned buffer, INVALID once the callback
            // returns -- copy it.
            records.emplace_back(record.message);
        });

        CHECK_FALSE(defaultComponentField(world, transformId, "nope").has_value());         // FD3
        CHECK_FALSE(defaultComponentField(world, ComponentTypeId{}, "scale").has_value());  // FD4
        CHECK_FALSE(defaultComponentField(world, markerId, "payload").has_value());         // FD5
        CHECK_FALSE(defaultComponentField(world, noDefaultId, "value").has_value());        // FD9

        REQUIRE(records.size() == 4);
        for (const std::string& record : records) {
            CAPTURE(record);
            CHECK(record.find("defaultComponentField") != std::string::npos);
        }
        // The four are DISTINCT sentences, so one arm's message cannot be standing in for another's.
        CHECK(records[0].find("unknown field") != std::string::npos);
        CHECK(records[1].find("unregistered component id") != std::string::npos);
        CHECK(records[2].find("no entt::meta registered") != std::string::npos);
        CHECK(records[3].find("not default-constructible") != std::string::npos);

        // ANTI-VACUITY CONTROL: a SUCCESSFUL call inside the same sink scope adds nothing at all.
        // Without it, a sink that was never actually installed -- or a seam that logged nothing --
        // would be indistinguishable from one that logs exactly on rejection.
        const std::size_t beforeSuccess = records.size();
        CHECK(defaultComponentField(world, transformId, "scale").has_value());
        CHECK(records.size() == beforeSuccess);
    }

    // The guard detached; a call after the scope adds nothing more.
    const std::size_t afterDetach = records.size();
    CHECK_FALSE(defaultComponentField(world, transformId, "nope").has_value());
    CHECK(records.size() == afterDetach);
}

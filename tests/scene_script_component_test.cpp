// tests/scene_script_component_test.cpp -- task 4.4.1: engine::ScriptComponent, the ELEVENTH built-in.
// Tier-0 throughout: no GPU, no reflect-gen, no generated code, no files, no randomness. Rides
// aero_tests unconditionally and passes identically with -DAERO_REFLECT_TOOLS=OFF: registration is
// hand-written, so the component has no codegen dependency (only its serialization does). The
// spot_light_test.cpp shape.

#include <aero/core/guid.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

// <ostream> is required by MSVC, not by libc++ (the 0.4.1 trap): doctest stringifies a failing CHECK
// involving a std::string_view through operator<<, and MSVC's overload needs a COMPLETE std::ostream.
#include <ostream>
#include <string_view>
#include <type_traits>

using engine::Entity;
using engine::Guid;
using engine::ScriptComponent;
using engine::World;

TEST_CASE("script component: one nil Guid, a plain aggregate, size 16 (SE1)") {
    static_assert(std::is_trivially_copyable_v<ScriptComponent>);
    static_assert(std::is_standard_layout_v<ScriptComponent>);
    static_assert(std::is_aggregate_v<ScriptComponent>);
    static_assert(sizeof(ScriptComponent) == 16);
    static_assert(alignof(ScriptComponent) == 8);
    CHECK(std::is_trivially_copyable_v<ScriptComponent>);
    CHECK(std::is_standard_layout_v<ScriptComponent>);
    CHECK(std::is_aggregate_v<ScriptComponent>);
    CHECK(sizeof(ScriptComponent) == 16);
    CHECK(alignof(ScriptComponent) == 8);

    // Nil is the ordinary state of a fresh component, and the only default: a second field or a
    // non-nil default would show here.
    const ScriptComponent fresh{};
    CHECK(fresh.script == Guid{});

    ScriptComponent a{};
    ScriptComponent b{};
    CHECK(a == b);
    a.script = Guid{1U, 2U};
    CHECK_FALSE(a == b);
    b.script = Guid{1U, 2U};
    CHECK(a == b);
    b.script = Guid{1U, 3U};
    CHECK_FALSE(a == b);
}

TEST_CASE("script component: registered ELEVENTH, after engine::SpotLight (SE2)") {
    World w;
    CHECK(w.componentTypeCount() == 11);
    CHECK(w.findComponentType("engine::ScriptComponent").valid());
    CHECK(w.findComponentType("engine::ScriptComponent") == engine::componentTypeId<ScriptComponent>());

    // "Eleventh" asserted as an ORDINAL: registration order is the save-emission order and the
    // fixture's key order. Index 9 is pinned alongside 10 so an INSERTION before SpotLight, which
    // would leave index 10 wrong only by accident, cannot pass.
    REQUIRE(w.componentTypeCount() >= 11);
    CHECK(w.componentTypeName(w.componentTypeAt(9)) == std::string_view{"engine::SpotLight"});
    CHECK(w.componentTypeName(w.componentTypeAt(10)) == std::string_view{"engine::ScriptComponent"});

    const Entity e = w.create();
    const ScriptComponent* added = w.add<ScriptComponent>(e, ScriptComponent{});
    REQUIRE(added != nullptr);
    CHECK(added->script == Guid{});
    CHECK(w.has<ScriptComponent>(e));
}

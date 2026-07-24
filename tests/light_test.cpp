// tests/light_test.cpp — task 1.3.3: engine::DirectionalLight and engine::PointLight, both reflected
// light components, and their built-in registration. Tier-0 throughout: no GPU, no reflect-gen, no
// generated code, no files, no randomness. Rides aero_tests unconditionally and passes identically
// with -DAERO_REFLECT_TOOLS=OFF — the structural proof that both components have zero codegen
// dependency.

#include <aero/core/math.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <type_traits>

using engine::DirectionalLight;
using engine::Entity;
using engine::PointLight;
using engine::Vec3;
using engine::World;

TEST_CASE("light: layout") {
    static_assert(std::is_trivially_copyable_v<DirectionalLight>);
    static_assert(std::is_standard_layout_v<DirectionalLight>);
    static_assert(std::is_aggregate_v<DirectionalLight>);
    static_assert(sizeof(DirectionalLight) == 4 * sizeof(float));

    static_assert(std::is_trivially_copyable_v<PointLight>);
    static_assert(std::is_standard_layout_v<PointLight>);
    static_assert(std::is_aggregate_v<PointLight>);
    static_assert(sizeof(PointLight) == 5 * sizeof(float));
}

TEST_CASE("light: default values") {
    const DirectionalLight dir{};
    CHECK(dir.color == Vec3::one());
    CHECK(dir.intensity == 1.0F);
    CHECK(DirectionalLight{} == DirectionalLight{});

    const PointLight point{};
    CHECK(point.color == Vec3::one());
    CHECK(point.intensity == 1.0F);
    CHECK(point.range == 10.0F);
    CHECK(PointLight{} == PointLight{});

    PointLight other{};
    other.range = 5.0F;
    CHECK_FALSE(PointLight{} == other);
}

TEST_CASE("light: registration (AC-4)") {
    World w;
    CHECK(w.findComponentType("engine::DirectionalLight").valid());
    CHECK(w.findComponentType("engine::DirectionalLight") == engine::componentTypeId<DirectionalLight>());
    CHECK(w.findComponentType("engine::PointLight").valid());
    CHECK(w.findComponentType("engine::PointLight") == engine::componentTypeId<PointLight>());

    // Both light types coexist on the SAME entity.
    const Entity e = w.create();
    auto* dir = w.add<DirectionalLight>(e, DirectionalLight{Vec3{0.9F, 0.8F, 0.7F}, 2.0F});
    REQUIRE(dir != nullptr);
    auto* point = w.add<PointLight>(e, PointLight{Vec3{0.1F, 0.2F, 0.3F}, 3.0F, 25.0F});
    REQUIRE(point != nullptr);

    CHECK(w.has<DirectionalLight>(e));
    CHECK(w.has<PointLight>(e));

    const DirectionalLight* gotDir = w.get<DirectionalLight>(e);
    REQUIRE(gotDir != nullptr);
    CHECK(gotDir->intensity == 2.0F);
    const PointLight* gotPoint = w.get<PointLight>(e);
    REQUIRE(gotPoint != nullptr);
    CHECK(gotPoint->range == 25.0F);

    CHECK(w.componentCount<DirectionalLight>() == 1);
    CHECK(w.componentCount<PointLight>() == 1);

    CHECK(w.remove<DirectionalLight>(e));
    CHECK_FALSE(w.has<DirectionalLight>(e));
    CHECK(w.has<PointLight>(e));  // independent — removing one leaves the other untouched
    CHECK(w.componentCount<DirectionalLight>() == 0);
    CHECK(w.componentCount<PointLight>() == 1);
}

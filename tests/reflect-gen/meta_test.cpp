// aero_reflect_meta_test is a standalone single-TU target (no shared tests/test_main.cpp) -- it
// provides doctest's own main() here, unlike aero_tests' TEST_CASE files.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// The four narrow <entt/meta/{resolve,meta}.hpp> + <entt/core/hashed_string.hpp> headers proved
// insufficient during local de-risking: entt::meta_reset() is declared in <entt/meta/factory.hpp>, not
// resolve.hpp/meta.hpp -- so this TU consolidates to the umbrella header per the plan's documented
// fallback (spec §3.6). The assertions below are unaffected either way.
#include <aero/reflect/annotations.hpp>  // engine::reflect::FieldUiMeta (task 2.2.2)
#include <aero/scene/camera.hpp>
#include <aero/scene/light.hpp>
#include <aero/scene/mesh_renderer.hpp>  // task 2.2.2
#include <aero/scene/transform.hpp>

#include "component_codegen.hpp"
#include "component_wiring.hpp"

#include <entt/entt.hpp>

#include <string>
#include <vector>

// Forward-declared here; DEFINED by the GENERATED component_codegen.meta.gen.cpp that
// aero_reflect_generate() (task 1.1.4, cmake/reflect.cmake) builds from component_codegen.hpp
// (name = aero_reflect_register_<stem>, D3/D7). The snake_case name is the frozen cross-boundary
// contract (spec D3/D7) between hand-written and generated code -- not a C++-style identifier
// subject to docs/04's camelCase law.
// NOLINTNEXTLINE(readability-identifier-naming)
void aero_reflect_register_component_codegen();

// Forward-declared here; DEFINED by the GENERATED transform.meta.gen.cpp that aero_reflect_generate()
// builds from the REAL engine header engine/scene/include/aero/scene/transform.hpp (task 1.3.2). Same
// frozen snake_case cross-boundary contract as the two declarations above.
// NOLINTNEXTLINE(readability-identifier-naming)
void aero_reflect_register_transform();

// Forward-declared here; DEFINED by the GENERATED camera.meta.gen.cpp / light.meta.gen.cpp that
// aero_reflect_generate() builds from the REAL engine headers engine/scene/include/aero/scene/
// {camera,light}.hpp (task 1.3.3). aero_reflect_register_light() registers BOTH DirectionalLight
// and PointLight — one header, one register function (D10). Same frozen snake_case cross-boundary
// contract as the declarations above.
// NOLINTNEXTLINE(readability-identifier-naming)
void aero_reflect_register_camera();
// NOLINTNEXTLINE(readability-identifier-naming)
void aero_reflect_register_light();

// Forward-declared here; DEFINED by the GENERATED mesh_renderer.meta.gen.cpp that
// aero_reflect_generate() builds from the REAL engine header engine/scene/include/aero/scene/
// mesh_renderer.hpp (task 1.4.1). Same frozen snake_case cross-boundary contract (task 2.2.2).
// NOLINTNEXTLINE(readability-identifier-naming)
void aero_reflect_register_mesh_renderer();

// Forward-declared here; DEFINED by the GENERATED aero_reflect_meta_test.aggregator.gen.cpp (task
// 1.1.4, D4) that calls every per-header register function (both above) in HEADERS-list order.
// NOLINTNEXTLINE(readability-identifier-naming)
void aero_reflect_register_all_aero_reflect_meta_test();

TEST_CASE("generated entt::meta registration reflects the component and its supported fields") {
    using namespace entt::literals;
    aero_reflect_register_component_codegen();  // "register at startup"

    auto byType = entt::resolve<ReflectSample>();
    REQUIRE(static_cast<bool>(byType));
    CHECK(static_cast<bool>(entt::resolve("ReflectSample"_hs)));  // .type() id took effect

    CHECK(static_cast<bool>(byType.data("position"_hs)));
    CHECK(static_cast<bool>(byType.data("rotation"_hs)));
    CHECK(static_cast<bool>(byType.data("mass"_hs)));
    CHECK(static_cast<bool>(byType.data("hitPoints"_hs)));
    CHECK(static_cast<bool>(byType.data("active"_hs)));
    CHECK_FALSE(static_cast<bool>(byType.data("velocity"_hs)));        // unsupported -> skipped
    CHECK_FALSE(static_cast<bool>(byType.data("SCHEMA_VERSION"_hs)));  // static -> excluded

    std::size_t count = 0;
    for (auto&& d : byType.data()) {
        (void)d;
        ++count;
    }
    CHECK(count == 5);

    entt::meta_reset();  // release the global meta context (LSan hygiene, spec §3.11 item 2)
}

TEST_CASE("the first REAL engine component reflects: engine::Transform (task 1.3.2)") {
    using namespace entt::literals;
    aero_reflect_register_transform();

    auto byType = entt::resolve<engine::Transform>();
    REQUIRE(static_cast<bool>(byType));
    CHECK(static_cast<bool>(entt::resolve("engine::Transform"_hs)));  // the .type() id took effect

    CHECK(static_cast<bool>(byType.data("position"_hs)));
    CHECK(static_cast<bool>(byType.data("rotation"_hs)));
    CHECK(static_cast<bool>(byType.data("scale"_hs)));
    CHECK_FALSE(static_cast<bool>(byType.data("parent"_hs)));  // hierarchy is NOT component data (D4)

    // Order pin (task 2.2.2, F6/S1): iterating .data() yields EXACTLY position, rotation, scale, in
    // declaration order — S1 (reversing emitMeta's .data chain) must red this.
    std::vector<std::string> names;
    for (auto&& d : byType.data()) {
        names.emplace_back(d.second.name());
    }
    CHECK(names == std::vector<std::string>{"position", "rotation", "scale"});

    entt::meta_reset();
}

TEST_CASE("the Camera/Light engine components reflect (task 1.3.3)") {
    using namespace entt::literals;
    aero_reflect_register_camera();
    aero_reflect_register_light();          // registers BOTH DirectionalLight and PointLight
    aero_reflect_register_mesh_renderer();  // task 2.2.2

    auto cam = entt::resolve<engine::Camera>();
    REQUIRE(static_cast<bool>(cam));
    CHECK(static_cast<bool>(entt::resolve("engine::Camera"_hs)));
    CHECK(static_cast<bool>(cam.data("fovYRadians"_hs)));
    CHECK(static_cast<bool>(cam.data("nearPlane"_hs)));
    CHECK(static_cast<bool>(cam.data("farPlane"_hs)));

    // Task 2.2.2 (D6/AC-4): fovYRadians carries a FieldUiMeta custom, at runtime.
    const engine::reflect::FieldUiMeta* fovMeta = cam.data("fovYRadians"_hs).custom();
    REQUIRE(fovMeta != nullptr);
    CHECK(fovMeta->hasRange);
    CHECK(fovMeta->rangeMin == doctest::Approx(0.0175));
    CHECK(fovMeta->rangeMax == doctest::Approx(3.1241));
    CHECK_FALSE(fovMeta->color);

    // nearPlane carries NO custom -- D6's sparsity, at runtime (F7).
    const engine::reflect::FieldUiMeta* nearMeta = cam.data("nearPlane"_hs).custom();
    CHECK(nearMeta == nullptr);

    // F7's type_hash check: retrieving through the WRONG type also yields nullptr.
    const double* wrongTypeMeta = cam.data("fovYRadians"_hs).custom();
    CHECK(wrongTypeMeta == nullptr);

    // Order pin (task 2.2.2, F6/S1): Camera's .data() yields EXACTLY fovYRadians, nearPlane, farPlane.
    std::vector<std::string> camNames;
    for (auto&& d : cam.data()) {
        camNames.emplace_back(d.second.name());
    }
    CHECK(camNames == std::vector<std::string>{"fovYRadians", "nearPlane", "farPlane"});

    auto dir = entt::resolve<engine::DirectionalLight>();
    REQUIRE(static_cast<bool>(dir));
    CHECK(static_cast<bool>(dir.data("color"_hs)));
    CHECK(static_cast<bool>(dir.data("intensity"_hs)));

    // Task 2.2.2 (D19): both lights' color carries an AERO_COLOR custom.
    const engine::reflect::FieldUiMeta* dirColorMeta = dir.data("color"_hs).custom();
    REQUIRE(dirColorMeta != nullptr);
    CHECK(dirColorMeta->color);
    CHECK_FALSE(dirColorMeta->hasRange);

    auto pt = entt::resolve<engine::PointLight>();
    REQUIRE(static_cast<bool>(pt));
    CHECK(static_cast<bool>(pt.data("range"_hs)));

    const engine::reflect::FieldUiMeta* ptColorMeta = pt.data("color"_hs).custom();
    REQUIRE(ptColorMeta != nullptr);
    CHECK(ptColorMeta->color);

    // Task 2.2.2 (D19): MeshRenderer's primitive carries a 0..2 range; color carries AERO_COLOR.
    auto meshRenderer = entt::resolve<engine::MeshRenderer>();
    REQUIRE(static_cast<bool>(meshRenderer));
    const engine::reflect::FieldUiMeta* primitiveMeta = meshRenderer.data("primitive"_hs).custom();
    REQUIRE(primitiveMeta != nullptr);
    CHECK(primitiveMeta->hasRange);
    CHECK(primitiveMeta->rangeMin == doctest::Approx(0.0));
    CHECK(primitiveMeta->rangeMax == doctest::Approx(2.0));

    const engine::reflect::FieldUiMeta* meshColorMeta = meshRenderer.data("color"_hs).custom();
    REQUIRE(meshColorMeta != nullptr);
    CHECK(meshColorMeta->color);

    entt::meta_reset();  // per-case hygiene
}

TEST_CASE("the generated aggregator registers every header's components in one call") {
    using namespace entt::literals;
    aero_reflect_register_all_aero_reflect_meta_test();  // ONE call registers every header's components

    auto sample = entt::resolve<ReflectSample>();
    auto wiring = entt::resolve<ReflectWiring>();
    REQUIRE(static_cast<bool>(sample));
    REQUIRE(static_cast<bool>(wiring));
    CHECK(static_cast<bool>(entt::resolve("ReflectWiring"_hs)));
    CHECK(static_cast<bool>(wiring.data("target"_hs)));
    CHECK(static_cast<bool>(wiring.data("speed"_hs)));
    CHECK(static_cast<bool>(wiring.data("gear"_hs)));
    CHECK(static_cast<bool>(wiring.data("engaged"_hs)));
    std::size_t count = 0;
    for (auto&& d : wiring.data()) {
        (void)d;
        ++count;
    }
    CHECK(count == 4);

    auto transform = entt::resolve<engine::Transform>();
    REQUIRE(static_cast<bool>(transform));
    CHECK(static_cast<bool>(entt::resolve("engine::Transform"_hs)));
    CHECK(static_cast<bool>(transform.data("scale"_hs)));

    entt::meta_reset();  // per-case hygiene, matching the 1.1.3 case
}

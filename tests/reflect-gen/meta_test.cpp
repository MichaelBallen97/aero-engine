// aero_reflect_meta_test is a standalone single-TU target (no shared tests/test_main.cpp) -- it
// provides doctest's own main() here, unlike aero_tests' TEST_CASE files.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// The four narrow <entt/meta/{resolve,meta}.hpp> + <entt/core/hashed_string.hpp> headers proved
// insufficient during local de-risking: entt::meta_reset() is declared in <entt/meta/factory.hpp>, not
// resolve.hpp/meta.hpp -- so this TU consolidates to the umbrella header per the plan's documented
// fallback (spec §3.6). The assertions below are unaffected either way.
#include <aero/scene/transform.hpp>

#include "component_codegen.hpp"
#include "component_wiring.hpp"

#include <entt/entt.hpp>

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

    std::size_t count = 0;
    for (auto&& d : byType.data()) {
        (void)d;
        ++count;
    }
    CHECK(count == 3);  // exactly three fields, zero skips — the zero-unsupported claim at runtime

    entt::meta_reset();
}

TEST_CASE("the generated aggregator registers every header's components in one call") {
    using namespace entt::literals;
    aero_reflect_register_all_aero_reflect_meta_test();  // ONE call registers ALL THREE headers

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

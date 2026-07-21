// aero_reflect_meta_test is a standalone single-TU target (no shared tests/test_main.cpp) -- it
// provides doctest's own main() here, unlike aero_tests' TEST_CASE files.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// The four narrow <entt/meta/{resolve,meta}.hpp> + <entt/core/hashed_string.hpp> headers proved
// insufficient during local de-risking: entt::meta_reset() is declared in <entt/meta/factory.hpp>, not
// resolve.hpp/meta.hpp -- so this TU consolidates to the umbrella header per the plan's documented
// fallback (spec §3.6). The assertions below are unaffected either way.
#include "component_codegen.hpp"

#include <entt/entt.hpp>

// Forward-declared here; DEFINED by the GENERATED component_codegen.meta.gen.cpp that the add_custom_command in
// tests/CMakeLists.txt builds from component_codegen.hpp (name = aero_reflect_register_<stem>, D3/D7).
// The snake_case name is the frozen cross-boundary contract (spec D3/D7) between hand-written and
// generated code -- not a C++-style identifier subject to docs/04's camelCase law.
// NOLINTNEXTLINE(readability-identifier-naming)
void aero_reflect_register_component_codegen();

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

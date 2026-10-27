// Aero Engine -- the generated built-in binding table (task 4.2.1; BN6, and from the later steps BN37-BN49).
//
// Compiled into aero_tests only when AERO_REFLECT_TOOLS is ON: with the tools OFF the table is empty by design and
// there is nothing generated to check.

#include <aero/scene/world.hpp>
#include <aero/script/internal/component_binding.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <ostream>  // a CHECK on std::string_view needs the complete std::ostream on MSVC (ci-portability.md)
#include <string>
#include <string_view>

TEST_CASE("script: the generated table binds exactly a fresh World's registrations, in order (BN6)") {
    const engine::World world;
    const auto components = engine::script::internal::builtinComponentBindings().components();

    REQUIRE(world.componentTypeCount() >= 1U);
    REQUIRE(components.size() == world.componentTypeCount());
    for (std::size_t i = 0; i < components.size(); ++i) {
        CAPTURE(i);
        CAPTURE(components[i].scriptName);
        CHECK(components[i].type == world.componentTypeAt(i));
        const std::string expected = "engine::" + std::string{components[i].scriptName};
        CHECK(std::string_view{expected} == world.componentTypeName(components[i].type));
    }
}

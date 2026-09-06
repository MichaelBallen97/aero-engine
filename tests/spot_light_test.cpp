// tests/spot_light_test.cpp — task E.2.2: engine::SpotLight, the TENTH built-in. Tier-0 throughout:
// no GPU, no reflect-gen, no generated code, no files, no randomness. Rides aero_tests
// unconditionally and passes identically with -DAERO_REFLECT_TOOLS=OFF — the structural proof that
// the component has zero codegen dependency (only its SERIALIZATION does, with every other
// built-in's). The environment_test.cpp shape, extended where this component has more to say.

#include <aero/core/math.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <cmath>

// <ostream> is required by MSVC, not by libc++ (the 0.4.1 trap): doctest stringifies a failing CHECK
// involving a std::string_view through operator<<, and MSVC's overload needs a COMPLETE std::ostream.
#include <ostream>
#include <string_view>
#include <type_traits>

using engine::DirectionalLight;
using engine::Entity;
using engine::PointLight;
using engine::SpotLight;
using engine::Vec3;
using engine::World;

TEST_CASE("spot light: layout (SC1)") {
    static_assert(std::is_trivially_copyable_v<SpotLight>);
    static_assert(std::is_standard_layout_v<SpotLight>);
    static_assert(std::is_aggregate_v<SpotLight>);
    // 12 (Vec3 colour) + 4 (intensity) + 4 (range) + 4 (inner) + 4 (outer) = 28. Every member is
    // 4-aligned, so there is NO padding anywhere and the sum is exact rather than rounded up.
    static_assert(sizeof(SpotLight) == 28);
    static_assert(sizeof(SpotLight) == 7 * sizeof(float));
    static_assert(alignof(SpotLight) == alignof(float));
    static_assert(alignof(SpotLight) == 4);
    CHECK(sizeof(SpotLight) == 28);  // the runtime half, so the case is not vacuous when it passes
}

TEST_CASE("spot light: every one of the five defaults, by value (SC2)") {
    // BY VALUE, never "a default exists": these five numbers are the lamp a freshly added component
    // makes, and the render side mirrors them (the bridge's witness case pins the two together). A
    // silent constant edit on either side has to move BOTH.
    const SpotLight light{};
    CHECK(light.color == Vec3::one());
    CHECK(light.intensity == 1.0F);
    CHECK(light.range == 10.0F);
    CHECK(light.innerConeRadians == engine::radians(20.0F));
    CHECK(light.outerConeRadians == engine::radians(30.0F));
    // ...and BY NUMBER, so a SWAPPED inner/outer pair reddens here rather than passing on a pair of
    // radians() calls that were swapped in the assertion too.
    CHECK(std::fabs(light.innerConeRadians - 0.34906585F) < 1e-7F);
    CHECK(std::fabs(light.outerConeRadians - 0.52359878F) < 1e-7F);
    CHECK(light.innerConeRadians < light.outerConeRadians);
    CHECK(SpotLight{} == SpotLight{});
}

TEST_CASE("spot light: operator== sees every one of the five fields (SC3)") {
    // FIVE NAMED BLOCKS, not a loop: a loop over offsets would compare bytes and would still pass if
    // a future field were left out of the defaulted operator==.
    {
        SpotLight mutant{};
        mutant.color = Vec3{0.5F, 1.0F, 1.0F};
        CHECK_FALSE(SpotLight{} == mutant);
    }
    {
        SpotLight mutant{};
        mutant.intensity = 2.0F;
        CHECK_FALSE(SpotLight{} == mutant);
    }
    {
        SpotLight mutant{};
        mutant.range = 11.0F;
        CHECK_FALSE(SpotLight{} == mutant);
    }
    {
        SpotLight mutant{};
        mutant.innerConeRadians = 0.3F;
        CHECK_FALSE(SpotLight{} == mutant);
    }
    {
        SpotLight mutant{};
        mutant.outerConeRadians = 0.6F;
        CHECK_FALSE(SpotLight{} == mutant);
    }
}

TEST_CASE("spot light: registered TENTH, after engine::Environment (SC4)") {
    const World w;
    CHECK(w.findComponentType("engine::SpotLight").valid());
    CHECK(w.findComponentType("engine::SpotLight") == engine::componentTypeId<SpotLight>());

    // "Tenth" asserted as an ORDINAL, not as mere presence: registration order is the save-emission
    // order and the fixture's key order, so a component appended anywhere but last is a format
    // change. Index 8 is pinned alongside index 9 so an INSERTION before Environment — which would
    // leave index 9 correct — cannot pass.
    REQUIRE(w.componentTypeCount() >= 10);
    CHECK(w.componentTypeName(w.componentTypeAt(8)) == std::string_view{"engine::Environment"});
    CHECK(w.componentTypeName(w.componentTypeAt(9)) == std::string_view{"engine::SpotLight"});
}

TEST_CASE("spot light: add / get / remove, beside a bulb and a sun on one carrier (SC5)") {
    World w;
    const Entity e = w.create();

    const SpotLight authored{Vec3{0.2F, 0.4F, 0.6F}, 3.0F, 25.0F, 0.1F, 0.2F};
    auto* added = w.add<SpotLight>(e, authored);
    REQUIRE(added != nullptr);
    CHECK(w.has<SpotLight>(e));
    CHECK(w.componentCount<SpotLight>() == 1);

    // A bulb, a lamp and a sun on ONE carrier. Nothing forbids it: all three derive their placement
    // from the same Transform by three different rules, and all three are legal data.
    REQUIRE(w.add<PointLight>(e, PointLight{}) != nullptr);
    REQUIRE(w.add<DirectionalLight>(e, DirectionalLight{}) != nullptr);

    const SpotLight* got = w.get<SpotLight>(e);
    REQUIRE(got != nullptr);
    CHECK(got->color == Vec3{0.2F, 0.4F, 0.6F});
    CHECK(got->intensity == 3.0F);
    CHECK(got->range == 25.0F);
    CHECK(got->innerConeRadians == 0.1F);
    CHECK(got->outerConeRadians == 0.2F);

    CHECK(w.remove<SpotLight>(e));
    CHECK_FALSE(w.has<SpotLight>(e));
    CHECK(w.has<PointLight>(e));        // independent — removing one leaves the others untouched
    CHECK(w.has<DirectionalLight>(e));  //
    CHECK(w.componentCount<SpotLight>() == 0);
    CHECK_FALSE(w.remove<SpotLight>(e));  // removing twice is false, never a crash
}

TEST_CASE("spot light: the five fields are PLAIN DATA -- the component never clamps (SC6)") {
    // WHERE THE CLAMPS LIVE IS WRITTEN DOWN HERE. The component stores whatever it is given. The
    // AERO_RANGE on the two cone angles is an INSPECTOR bound, not a validation rule; the render
    // side (aero/render/light_falloff.hpp) is what folds inner >= outer into a hard edge and what
    // makes range <= 0 contribute nothing. A component that clamped on write would silently rewrite
    // a scene file the editor merely opened.
    World w;
    const Entity e = w.create();
    const SpotLight wild{.range = -1.0F, .intensity = -2.0F, .innerConeRadians = 1.2F, .outerConeRadians = 0.3F};
    REQUIRE(w.add<SpotLight>(e, wild) != nullptr);

    const SpotLight* got = w.get<SpotLight>(e);
    REQUIRE(got != nullptr);
    CHECK(got->range == -1.0F);
    CHECK(got->intensity == -2.0F);
    CHECK(got->innerConeRadians == 1.2F);    // inner > outer: legal data, a hard edge at the render side
    CHECK(got->outerConeRadians == 0.3F);    //
    CHECK(got->color == SpotLight{}.color);  // the un-set field kept its default

    // ...and a cone angle PAST THE AERO_RANGE is stored unchanged too. THROUGH THE WORLD, like the
    // fields above and for the same reason: asserted on a local aggregate, this arm would run
    // nothing at all between the write and the read.
    const Entity wide = w.create();
    REQUIRE(w.add<SpotLight>(wide, SpotLight{.outerConeRadians = 3.0F}) != nullptr);
    const SpotLight* stored = w.get<SpotLight>(wide);
    REQUIRE(stored != nullptr);
    CHECK(stored->outerConeRadians == 3.0F);
    // ...and the entity that carries it is not the one above, so neither reading can be the other's.
    // Re-fetched rather than read through `got`, which the add above may have invalidated.
    const SpotLight* untouched = w.get<SpotLight>(e);
    REQUIRE(untouched != nullptr);
    CHECK(untouched->outerConeRadians == 0.3F);
}

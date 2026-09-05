// tests/environment_test.cpp — task E.2.1: engine::Environment, the NINTH built-in. Tier-0
// throughout: no GPU, no reflect-gen, no generated code, no files, no randomness. Rides aero_tests
// unconditionally and passes identically with -DAERO_REFLECT_TOOLS=OFF — the structural proof that
// the component has zero codegen dependency (only its SERIALIZATION does, with every other
// built-in's). The light_test.cpp shape, extended where this component has more to say.

#include <aero/core/math.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
// <ostream> is required by MSVC, not by libc++ (the 0.4.1 trap): doctest stringifies a failing CHECK
// involving a std::string_view through operator<<, and MSVC's overload needs a COMPLETE std::ostream.
#include <ostream>
#include <string_view>
#include <type_traits>

using engine::DirectionalLight;
using engine::Entity;
using engine::Environment;
using engine::Vec3;
using engine::World;

TEST_CASE("environment: layout (EV1)") {
    static_assert(std::is_trivially_copyable_v<Environment>);
    static_assert(std::is_standard_layout_v<Environment>);
    static_assert(std::is_aggregate_v<Environment>);
    // 4 (backgroundMode) + 12 + 12 + 12 + 12 (four Vec3) + 4 (ambientMode) + 12 (ambientColor)
    // + 4 (ambientIntensity) = 72. Every member is 4-aligned, so there is NO padding anywhere and
    // the sum is exact rather than rounded up — unlike MeshRenderer, whose 8-aligned Guid costs it 4.
    static_assert(sizeof(Environment) == 72);
    static_assert(sizeof(Environment) == 18 * sizeof(float));
    static_assert(alignof(Environment) == alignof(float));
    static_assert(alignof(Environment) == 4);
    CHECK(sizeof(Environment) == 72);  // the runtime half, so the case is not vacuous when it passes
}

TEST_CASE("environment: every one of the eight defaults, by value (EV2)") {
    // BY VALUE, never "a default exists": these eight numbers are the picture a scene with no
    // Environment renders, and the render side mirrors them (the bridge's witness case pins the two
    // together). A silent constant edit on either side has to move BOTH.
    const Environment env{};
    CHECK(env.backgroundMode == 0U);  // Sky
    CHECK(env.skyColor == Vec3{0.16F, 0.26F, 0.48F});
    CHECK(env.horizonColor == Vec3{0.52F, 0.58F, 0.68F});
    CHECK(env.groundColor == Vec3{0.10F, 0.09F, 0.085F});
    CHECK(env.solidColor == Vec3{0.06F, 0.06F, 0.07F});
    CHECK(env.ambientMode == 0U);  // Hemisphere
    CHECK(env.ambientColor == Vec3{0.03F, 0.03F, 0.03F});
    CHECK(env.ambientIntensity == 0.5F);

    CHECK(Environment{} == Environment{});
}

TEST_CASE("environment: the defaulted operator== sees every field (EV3)") {
    // EIGHT NAMED BLOCKS, not a loop: a failure has to name the field that stopped being compared.
    // A defaulted operator== cannot silently lose a field, but a field APPENDED without extending
    // this case would leave the next reader unable to tell which are covered.
    {
        Environment m{};
        m.backgroundMode = 1U;
        CHECK_FALSE(Environment{} == m);
    }
    {
        Environment m{};
        m.skyColor = Vec3{0.9F, 0.1F, 0.2F};
        CHECK_FALSE(Environment{} == m);
    }
    {
        Environment m{};
        m.horizonColor = Vec3{0.9F, 0.1F, 0.2F};
        CHECK_FALSE(Environment{} == m);
    }
    {
        Environment m{};
        m.groundColor = Vec3{0.9F, 0.1F, 0.2F};
        CHECK_FALSE(Environment{} == m);
    }
    {
        Environment m{};
        m.solidColor = Vec3{0.9F, 0.1F, 0.2F};
        CHECK_FALSE(Environment{} == m);
    }
    {
        Environment m{};
        m.ambientMode = 1U;
        CHECK_FALSE(Environment{} == m);
    }
    {
        Environment m{};
        m.ambientColor = Vec3{0.9F, 0.1F, 0.2F};
        CHECK_FALSE(Environment{} == m);
    }
    {
        Environment m{};
        m.ambientIntensity = 1.5F;
        CHECK_FALSE(Environment{} == m);
    }
}

TEST_CASE("environment: registered NINTH, after engine::AudioListener (EV4)") {
    const World w;
    CHECK(w.findComponentType("engine::Environment").valid());
    CHECK(w.findComponentType("engine::Environment") == engine::componentTypeId<Environment>());

    // "Ninth" asserted as an ORDINAL, not as mere presence: registration order is the save-emission
    // order and the fixture's key order, so a component appended anywhere but last is a format
    // change. Index 7 is pinned alongside index 8 so an INSERTION before AudioListener — which would
    // leave index 8 correct — cannot pass.
    REQUIRE(w.componentTypeCount() >= 9);
    CHECK(w.componentTypeName(w.componentTypeAt(7)) == std::string_view{"engine::AudioListener"});
    CHECK(w.componentTypeName(w.componentTypeAt(8)) == std::string_view{"engine::Environment"});
}

TEST_CASE("environment: add / get / remove, and it coexists with a light (EV5)") {
    World w;
    const Entity e = w.create();

    Environment authored{};
    authored.backgroundMode = 1U;
    authored.solidColor = Vec3{0.2F, 0.4F, 0.6F};
    authored.ambientMode = 1U;
    authored.ambientIntensity = 2.5F;
    auto* added = w.add<Environment>(e, authored);
    REQUIRE(added != nullptr);
    CHECK(w.has<Environment>(e));
    CHECK(w.componentCount<Environment>() == 1);

    // It carries no position and never consults a Transform, but nothing STOPS it sharing an entity
    // with a light — and the scene the editor seeds pairs neither. Both directions are legal data.
    auto* light = w.add<DirectionalLight>(e, DirectionalLight{});
    REQUIRE(light != nullptr);

    const Environment* got = w.get<Environment>(e);
    REQUIRE(got != nullptr);
    CHECK(got->backgroundMode == 1U);
    CHECK(got->solidColor == Vec3{0.2F, 0.4F, 0.6F});
    CHECK(got->ambientMode == 1U);
    CHECK(got->ambientIntensity == 2.5F);
    CHECK(got->skyColor == Environment{}.skyColor);  // the un-set fields kept their defaults

    CHECK(w.remove<Environment>(e));
    CHECK_FALSE(w.has<Environment>(e));
    CHECK(w.has<DirectionalLight>(e));  // independent — removing one leaves the other untouched
    CHECK(w.componentCount<Environment>() == 0);
    CHECK_FALSE(w.remove<Environment>(e));  // removing twice is false, never a crash
}

TEST_CASE("environment: the two selectors are PLAIN DATA -- the component never clamps (EV6)") {
    // WHERE THE CLAMP LIVES IS WRITTEN DOWN HERE. The component stores whatever it is given: an
    // out-of-range selector survives a save/load round trip byte for byte, and it is the SCENE-RENDER
    // BRIDGE that folds anything out of range down to 0 (which is the default mode on both
    // selectors, deliberately, so "out of range" and "the default" are one number). A component that
    // clamped on write would silently rewrite a scene file the editor merely opened.
    World w;
    const Entity e = w.create();
    const Environment wild{.backgroundMode = 7U, .ambientMode = std::numeric_limits<std::uint32_t>::max()};
    auto* added = w.add<Environment>(e, wild);
    REQUIRE(added != nullptr);

    const Environment* got = w.get<Environment>(e);
    REQUIRE(got != nullptr);
    CHECK(got->backgroundMode == 7U);
    CHECK(got->ambientMode == std::numeric_limits<std::uint32_t>::max());

    // And a negative intensity is stored unchanged too: nothing sanitises an HDR knob, exactly as
    // DirectionalLight::intensity already does not. THROUGH THE WORLD, like the two selectors above
    // and for the same reason: asserted on a local aggregate, this arm would run nothing at all
    // between the write and the read and would say only that assigning a float and reading it back
    // yields the same float. The SAVE path's half of the claim is covered where it belongs, by
    // scene_serialize's Environment round trip.
    const Entity dark = w.create();
    REQUIRE(w.add<Environment>(dark, Environment{.ambientIntensity = -1.0F}) != nullptr);
    const Environment* stored = w.get<Environment>(dark);
    REQUIRE(stored != nullptr);
    CHECK(stored->ambientIntensity == -1.0F);
    // ...and the entity that carries it is not the one above, so neither reading can be the other's.
    // Re-fetched rather than read through `got`, which the add above may have invalidated.
    const Environment* untouched = w.get<Environment>(e);
    REQUIRE(untouched != nullptr);
    CHECK(untouched->ambientIntensity == Environment{}.ambientIntensity);
}

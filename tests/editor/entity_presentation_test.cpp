// Aero Engine -- task E.6.3: what an entity looks like in panels (EK1-EK10). Tier 0: a real World, no ImGui, no #if.
#include <aero/core/log.hpp>
#include <aero/editor/console_model.hpp>  // LogSinkScope -- EK3's no-log arm
#include <aero/editor/editor_icons.hpp>
#include <aero/editor/entity_presentation.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <array>
#include <initializer_list>
#include <ostream>  // MSVC: a CHECK over a std::string_view needs the complete std::ostream (the 0.4.1 trap)
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ed = engine::editor;
using ed::ChromeRole;
using ed::EntityKind;
using engine::Entity;
using engine::World;

struct LogFixture {
    LogFixture() { engine::initLogging(engine::LogConfig{.level = engine::LogLevel::Trace, .console = false}); }
    ~LogFixture() { engine::shutdownLogging(); }
    LogFixture(const LogFixture&) = delete;
    LogFixture& operator=(const LogFixture&) = delete;
    LogFixture(LogFixture&&) = delete;
    LogFixture& operator=(LogFixture&&) = delete;
};

[[nodiscard]] std::string humanized(std::string_view identifier) {
    std::string out;
    ed::humanizeIdentifier(identifier, out);
    return out;
}

}  // namespace

TEST_CASE("entity presentation: each component decides its entity's kind (task E.6.3, EK1)") {
    World world;
    const auto with = [&world]<typename T>(T component) {
        const Entity e = world.create();
        world.add<T>(e, component);
        return e;
    };
    CHECK((ed::entityKindFor(world, with(engine::Camera{})) == EntityKind::Camera));
    CHECK((ed::entityKindFor(world, with(engine::DirectionalLight{})) == EntityKind::Light));
    CHECK((ed::entityKindFor(world, with(engine::PointLight{})) == EntityKind::Light));
    CHECK((ed::entityKindFor(world, with(engine::SpotLight{})) == EntityKind::Light));
    CHECK((ed::entityKindFor(world, with(engine::MeshRenderer{})) == EntityKind::Mesh));
    CHECK((ed::entityKindFor(world, with(engine::AudioSource{})) == EntityKind::Audio));
    CHECK((ed::entityKindFor(world, with(engine::AudioListener{})) == EntityKind::Audio));
    CHECK((ed::entityKindFor(world, with(engine::Environment{})) == EntityKind::Environment));
    CHECK((ed::entityKindFor(world, with(engine::Transform{})) == EntityKind::Empty));
}

TEST_CASE("entity presentation: the kind precedence, pair by pair (task E.6.3, EK2)") {
    World world;
    const Entity cameraLight = world.create();
    world.add<engine::Camera>(cameraLight, engine::Camera{});
    world.add<engine::DirectionalLight>(cameraLight, engine::DirectionalLight{});
    const Entity lightMesh = world.create();
    world.add<engine::DirectionalLight>(lightMesh, engine::DirectionalLight{});
    world.add<engine::MeshRenderer>(lightMesh, engine::MeshRenderer{});
    const Entity meshAudio = world.create();
    world.add<engine::MeshRenderer>(meshAudio, engine::MeshRenderer{});
    world.add<engine::AudioSource>(meshAudio, engine::AudioSource{});
    const Entity audioEnvironment = world.create();
    world.add<engine::AudioSource>(audioEnvironment, engine::AudioSource{});
    world.add<engine::Environment>(audioEnvironment, engine::Environment{});
    CHECK((ed::entityKindFor(world, cameraLight) == EntityKind::Camera));  // camera BEFORE light (seed S13)
    CHECK((ed::entityKindFor(world, lightMesh) == EntityKind::Light));
    CHECK((ed::entityKindFor(world, meshAudio) == EntityKind::Mesh));
    CHECK((ed::entityKindFor(world, audioEnvironment) == EntityKind::Audio));
}

TEST_CASE("entity presentation: a dead or null handle is Empty, silently (task E.6.3, EK3)") {
    const LogFixture fixture;  // declared FIRST: destructs LAST, after the LogSinkScope (VI10's idiom)
    const ed::LogSinkScope scope;
    std::vector<ed::LogEntry> records;
    World world;
    const Entity doomed = world.create();
    world.add<engine::Camera>(doomed, engine::Camera{});
    REQUIRE((ed::entityKindFor(world, doomed) == EntityKind::Camera));  // ANTI-VACUITY: alive, it is a camera
    REQUIRE(world.destroy(doomed));
    CHECK((ed::entityKindFor(world, doomed) == EntityKind::Empty));
    CHECK((ed::entityKindFor(world, Entity{}) == EntityKind::Empty));
    scope.sink()->take(records);
    CHECK(records.empty());
}

TEST_CASE("entity presentation: each kind's dot role, and accent when selected (task E.6.3, EK4)") {
    CHECK((ed::entityKindRole(EntityKind::Light, false) == ChromeRole::Warning));
    CHECK((ed::entityKindRole(EntityKind::Mesh, false) == ChromeRole::Info));
    CHECK((ed::entityKindRole(EntityKind::Camera, false) == ChromeRole::TextSecondary));
    CHECK((ed::entityKindRole(EntityKind::Audio, false) == ChromeRole::Success));
    CHECK((ed::entityKindRole(EntityKind::Environment, false) == ChromeRole::TextFaint));
    CHECK((ed::entityKindRole(EntityKind::Empty, false) == ChromeRole::TextFaint));
    for (const EntityKind kind : {EntityKind::Camera, EntityKind::Light, EntityKind::Mesh, EntityKind::Audio,
                                  EntityKind::Environment, EntityKind::Empty}) {
        CAPTURE(static_cast<int>(kind));
        CHECK((ed::entityKindRole(kind, true) == ChromeRole::Accent));  // seed S14
    }
}

TEST_CASE("entity presentation: the ten built-ins' icons, by registration name (task E.6.3, EK5)") {
    CHECK(ed::componentIconFor("engine::Transform") == std::string_view(AERO_ICON_AXIS_3D));
    CHECK(ed::componentIconFor("engine::Camera") == std::string_view(AERO_ICON_CAMERA));
    CHECK(ed::componentIconFor("engine::DirectionalLight") == std::string_view(AERO_ICON_SUN));
    CHECK(ed::componentIconFor("engine::PointLight") == std::string_view(AERO_ICON_LIGHTBULB));
    CHECK(ed::componentIconFor("engine::SpotLight") == std::string_view(AERO_ICON_LAMP_CEILING));
    CHECK(ed::componentIconFor("engine::MeshRenderer") == std::string_view(AERO_ICON_BOX));
    CHECK(ed::componentIconFor("engine::AnimationPlayer") == std::string_view(AERO_ICON_FILM));
    CHECK(ed::componentIconFor("engine::AudioSource") == std::string_view(AERO_ICON_VOLUME_2));
    CHECK(ed::componentIconFor("engine::AudioListener") == std::string_view(AERO_ICON_EAR));
    CHECK(ed::componentIconFor("engine::Environment") == std::string_view(AERO_ICON_CLOUD_SUN));
    for (const std::string_view other : {"game::Camera", "", "Transform"}) {  // "Transform": the short name (S40)
        CAPTURE(other);
        CHECK(ed::componentIconFor(other) == std::string_view(AERO_ICON_SHAPES));
    }
    // A COMPONENT-COUNT PIN: the next built-in adds a row (the RF7 / IR21 class, docs/10's sweep list).
    CHECK(ed::componentIconTable().size() == 10U);
    CHECK(ed::COMPONENT_ICON_COUNT == 10U);
}

TEST_CASE("entity presentation: identifiers split into words (task E.6.3, EK6)") {
    CHECK(humanized("castsShadows") == "Casts Shadows");
    CHECK(humanized("meshIndex") == "Mesh Index");
    CHECK(humanized("position") == "Position");
    CHECK(humanized("HDRExposure") == "HDR Exposure");
    CHECK(humanized("fovYRadians") == "Fov Y Radians");
    CHECK(humanized("shadowNormalBias") == "Shadow Normal Bias");
    CHECK(humanized("uv0Scale") == "Uv0 Scale");  // a digit stays with its letters, and splits before a capital (S39)
}

TEST_CASE("entity presentation: separators drop, and no word is empty (task E.6.3, EK7)") {
    CHECK(humanized("snake_case_name") == "Snake Case Name");
    CHECK(humanized("_lead") == "Lead");
    CHECK(humanized("a__b_") == "A B");
    CHECK(humanized("").empty());
    CHECK(humanized("x") == "X");
}

TEST_CASE("entity presentation: high bytes, idempotence, and a cleared out string (task E.6.3, EK8)") {
    CHECK(humanized("caf\xC3\xA9Size") == "Caf\xC3\xA9 Size");  // a byte >= 0x80 is "not a capital": a split
    CHECK(humanized("Casts Shadows") == "Casts Shadows");       // idempotent: never a doubled space
    CHECK(humanized(humanized("fovYRadians")) == "Fov Y Radians");
    std::string out = "zzz";
    ed::humanizeIdentifier("meshIndex", out);
    CHECK(out == "Mesh Index");  // the prior contents did not survive
}

TEST_CASE("entity presentation: the id text is decimal (task E.6.3, EK9)") {
    std::string out = "stale";
    ed::entityIdText(Entity{.index = 0U, .generation = 1U}, out);
    CHECK(out == "e#0");
    ed::entityIdText(Entity{.index = 12U, .generation = 3U}, out);
    CHECK(out == "e#12");
    ed::entityIdText(Entity{.index = 4294967295U, .generation = 1U}, out);
    CHECK(out == "e#4294967295");  // never hexadecimal
}

TEST_CASE("entity presentation: the id tooltip names index and generation (task E.6.3, EK10)") {
    std::string out = "stale";
    ed::entityIdTooltip(Entity{.index = 12U, .generation = 3U}, out);
    CHECK(out == "Entity 12, generation 3");
}

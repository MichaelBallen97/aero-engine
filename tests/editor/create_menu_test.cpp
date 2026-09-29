// tests/editor/create_menu_test.cpp -- task E.5.2: the Create menu's pure table, anchor and seeds (CR1-CR9).
// TIER 0, EVERY CONFIGURATION: create_menu.hpp is pure, so nothing here needs a device, a window or
// generated meta. NO #if of any kind (the 3.6.3 rule). Every create case reads the SEED's own fields,
// because the thing under test here IS the seed -- the World-side reads are X25-X31's and I246's.
#include <aero/core/math.hpp>
#include <aero/editor/create_menu.hpp>
#include <aero/editor/editor_camera.hpp>  // DEFAULT_PIVOT
#include <aero/editor/entity_ops.hpp>     // seedDefaultScene, the two shared constants
#include <aero/editor/scene_bounds.hpp>   // primitiveLocalBounds
#include <aero/render/debug_grid.hpp>     // DEBUG_GRID_PLANE_HEIGHT
#include <aero/render/mesh.hpp>           // PrimitiveId
#include <aero/scene/camera.hpp>
#include <aero/scene/light.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/spot_light.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>  // std::memcmp -- the bit-for-bit arms
#include <limits>
#include <ostream>  // MSVC: a CHECK over std::string_view needs the complete std::ostream (the 0.4.1 trap)
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

using engine::Entity;
using engine::Quat;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::editor::CREATE_PLANE_EXTENT;
using engine::editor::createAnchor;
using engine::editor::CreateKind;
using engine::editor::createKindLabel;
using engine::editor::createMenuEntries;
using engine::editor::CreateMenuEntry;
using engine::editor::CreateMenuGroup;
using engine::editor::createMenuGroupLabel;
using engine::editor::createSeed;
using engine::editor::EntitySeed;
using engine::render::PrimitiveId;

// Bit for bit, through const std::byte* (render_material_test.cpp's precedent): the claim IS about object
// representations, and bugprone-suspicious-memory-comparison rejects a memcmp over a float-carrying type.
static_assert(sizeof(Vec3) == 12U);
static_assert(sizeof(Quat) == 16U);
[[nodiscard]] bool bitsEqual(const Vec3& a, const Vec3& b) {
    const auto* lhs = reinterpret_cast<const std::byte*>(&a);
    const auto* rhs = reinterpret_cast<const std::byte*>(&b);
    return std::memcmp(lhs, rhs, sizeof(Vec3)) == 0;
}
[[nodiscard]] bool bitsEqual(const Quat& a, const Quat& b) {
    const auto* lhs = reinterpret_cast<const std::byte*>(&a);
    const auto* rhs = reinterpret_cast<const std::byte*>(&b);
    return std::memcmp(lhs, rhs, sizeof(Quat)) == 0;
}

// The eachEntity + name() idiom; Entity{} when no entity carries the name.
[[nodiscard]] Entity findNamed(const World& world, std::string_view name) {
    Entity found{};
    world.eachEntity([&](Entity e) {
        if (world.name(e) == name) {
            found = e;
        }
    });
    return found;
}

// Every kind, in enum order: CreateKind is 0..7 with Count the bound, so the index IS the kind.
static_assert(engine::editor::CREATE_KIND_COUNT == 8U);
constexpr std::array<CreateKind, 8> ALL_KINDS = [] {
    std::array<CreateKind, 8> kinds{};
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        kinds[i] = static_cast<CreateKind>(i);
    }
    return kinds;
}();

// One position, compared EXACTLY (every value below is exact in float).
void checkPosition(const EntitySeed& seed, float x, float y, float z) {
    CHECK(seed.transform.position.x == x);
    CHECK(seed.transform.position.y == y);
    CHECK(seed.transform.position.z == z);
}

}  // namespace

TEST_CASE("create menu: the table -- seven typed rows, in order, groups contiguous (task E.5.2, CR1)") {
    const auto rows = createMenuEntries();
    REQUIRE(rows.size() == 7U);
    constexpr std::array<CreateKind, 7> KINDS{
        CreateKind::Cube,       CreateKind::Sphere,    CreateKind::Plane, CreateKind::DirectionalLight,
        CreateKind::PointLight, CreateKind::SpotLight, CreateKind::Camera};
    using G = CreateMenuGroup;
    constexpr std::array<CreateMenuGroup, 7> GROUPS{G::Object3D, G::Object3D, G::Object3D, G::Light,
                                                    G::Light,    G::Light,    G::TopLevel};
    for (std::size_t i = 0; i < rows.size(); ++i) {
        CAPTURE(i);
        CHECK((rows[i].kind == KINDS[i]));
        CHECK((rows[i].group == GROUPS[i]));
    }
    // Every non-Empty kind EXACTLY once; Empty and Count never.
    for (const CreateKind kind : ALL_KINDS) {
        CAPTURE(static_cast<int>(kind));
        std::size_t seen = 0;
        for (const CreateMenuEntry& row : rows) {
            if (row.kind == kind) {
                ++seen;
            }
        }
        CHECK(seen == (kind == CreateKind::Empty ? 0U : 1U));
    }
    for (const CreateMenuEntry& row : rows) {
        CHECK((row.kind != CreateKind::Count));
    }
    // CONTIGUITY: once a group's run ends, that group never reappears.
    std::array<bool, 3> closed{false, false, false};
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto group = static_cast<std::size_t>(rows[i].group);
        REQUIRE(group < closed.size());
        CAPTURE(i);
        CHECK_FALSE(closed[group]);
        if (i + 1U < rows.size() && rows[i + 1U].group != rows[i].group) {
            closed[group] = true;
        }
    }
}

TEST_CASE("create menu: every label, and the out-of-range ones (task E.5.2, CR2)") {
    constexpr std::array<std::string_view, 8> LABELS{
        "Empty", "Cube", "Sphere", "Plane", "Directional Light", "Point Light", "Spot Light", "Camera"};
    for (std::size_t i = 0; i < ALL_KINDS.size(); ++i) {
        const char* const label = createKindLabel(ALL_KINDS[i]);
        REQUIRE(label != nullptr);
        CHECK(std::string_view(label) == LABELS[i]);
    }
    for (const CreateKind beyond : {CreateKind::Count, static_cast<CreateKind>(200)}) {
        const char* const label = createKindLabel(beyond);
        REQUIRE(label != nullptr);  // "" -- a string ImGui can take, never null
        CHECK(std::string_view(label).empty());
    }
    REQUIRE(createMenuGroupLabel(CreateMenuGroup::Object3D) != nullptr);
    CHECK(std::string_view(createMenuGroupLabel(CreateMenuGroup::Object3D)) == "3D Object");
    CHECK(std::string_view(createMenuGroupLabel(CreateMenuGroup::Light)) == "Light");
    REQUIRE(createMenuGroupLabel(CreateMenuGroup::TopLevel) != nullptr);
    CHECK(std::string_view(createMenuGroupLabel(CreateMenuGroup::TopLevel)).empty());
}

TEST_CASE("create menu: the anchor is the pivot on the ground, and it is total (task E.5.2, CR3)") {
    const Vec3 anchor = createAnchor(Vec3{3.0F, 7.0F, -2.0F});
    CHECK(anchor.x == 3.0F);
    CHECK(anchor.z == -2.0F);
    CHECK(anchor.y == engine::render::DEBUG_GRID_PLANE_HEIGHT);  // the CONSTANT...
    CHECK(anchor.y == 0.0F);                                     // ...and a moved constant is noticed

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const Vec3 nanX = createAnchor(Vec3{nan, 1.0F, 5.0F});
    CHECK(nanX.x == 0.0F);
    CHECK(nanX.z == 5.0F);
    const Vec3 infZ = createAnchor(Vec3{2.0F, 1.0F, inf});
    CHECK(infZ.x == 2.0F);
    CHECK(infZ.z == 0.0F);
    const Vec3 allBad = createAnchor(Vec3{-inf, nan, nan});  // y is never read
    CHECK(allBad.x == 0.0F);
    CHECK(allBad.y == 0.0F);
    CHECK(allBad.z == 0.0F);
    CHECK(bitsEqual(createAnchor(engine::editor::DEFAULT_PIVOT), Vec3{0.0F, 0.0F, 0.0F}));
}

TEST_CASE("create menu: every kind's seed at a non-trivial anchor (task E.5.2, CR4)") {
    const Vec3 a{4.0F, 0.0F, -6.0F};
    const Vec3 unitScale{1.0F, 1.0F, 1.0F};
    const Quat identity{};

    SUBCASE("Empty") {
        const EntitySeed seed = createSeed(CreateKind::Empty, a);
        CHECK(seed.name.empty());
        CHECK(seed.label == "Create Empty");
        CHECK(std::holds_alternative<std::monostate>(seed.component));
        checkPosition(seed, 4.0F, 0.0F, -6.0F);
        CHECK(bitsEqual(seed.transform.scale, unitScale));
        CHECK(bitsEqual(seed.transform.rotation, identity));
    }
    SUBCASE("Cube, Sphere and Plane") {
        struct Primitive {
            CreateKind kind;
            const char* name;
            PrimitiveId id;
            float y;
            Vec3 scale;
        };
        const std::array<Primitive, 3> primitives{{
            {CreateKind::Cube, "Cube", PrimitiveId::Cube, 0.5F, unitScale},
            {CreateKind::Sphere, "Sphere", PrimitiveId::Sphere, 0.5F, unitScale},
            {CreateKind::Plane, "Plane", PrimitiveId::Plane, 0.0F, Vec3{10.0F, 1.0F, 10.0F}},
        }};
        for (const Primitive& p : primitives) {
            CAPTURE(p.name);
            const EntitySeed seed = createSeed(p.kind, a);
            CHECK(seed.name == p.name);
            CHECK(seed.label == std::string("Create ") + p.name);
            REQUIRE(std::holds_alternative<engine::MeshRenderer>(seed.component));
            const engine::MeshRenderer expected{.primitive = static_cast<std::uint32_t>(p.id)};
            CHECK(std::get<engine::MeshRenderer>(seed.component) == expected);
            checkPosition(seed, 4.0F, p.y, -6.0F);
            CHECK(bitsEqual(seed.transform.scale, p.scale));
            CHECK(bitsEqual(seed.transform.rotation, identity));
        }
    }
    SUBCASE("Directional Light") {
        const EntitySeed seed = createSeed(CreateKind::DirectionalLight, a);
        CHECK(seed.name == "Directional Light");
        CHECK(seed.label == "Create Directional Light");
        REQUIRE(std::holds_alternative<engine::DirectionalLight>(seed.component));
        CHECK(std::get<engine::DirectionalLight>(seed.component) == engine::DirectionalLight{});
        checkPosition(seed, 4.0F, 3.0F, -6.0F);
        CHECK(bitsEqual(seed.transform.scale, unitScale));
    }
    SUBCASE("Point Light") {
        const EntitySeed seed = createSeed(CreateKind::PointLight, a);
        CHECK(seed.name == "Point Light");
        CHECK(seed.label == "Create Point Light");
        REQUIRE(std::holds_alternative<engine::PointLight>(seed.component));
        CHECK(std::get<engine::PointLight>(seed.component) == engine::PointLight{});
        checkPosition(seed, 4.0F, 2.0F, -6.0F);
        CHECK(bitsEqual(seed.transform.scale, unitScale));
        CHECK(bitsEqual(seed.transform.rotation, identity));
    }
    SUBCASE("Spot Light") {
        const EntitySeed seed = createSeed(CreateKind::SpotLight, a);
        CHECK(seed.name == "Spot Light");
        CHECK(seed.label == "Create Spot Light");
        REQUIRE(std::holds_alternative<engine::SpotLight>(seed.component));
        CHECK(std::get<engine::SpotLight>(seed.component) == engine::SpotLight{});
        checkPosition(seed, 4.0F, 3.0F, -6.0F);
        CHECK(bitsEqual(seed.transform.scale, unitScale));
    }
    SUBCASE("Camera") {
        const EntitySeed seed = createSeed(CreateKind::Camera, a);
        CHECK(seed.name == "Camera");
        CHECK(seed.label == "Create Camera");
        REQUIRE(std::holds_alternative<engine::Camera>(seed.component));
        CHECK(std::get<engine::Camera>(seed.component) == engine::Camera{});
        checkPosition(seed, 4.0F, 1.0F, -1.0F);  // RELATIVE to the anchor, never the absolute (0, 1, 5)
        CHECK(bitsEqual(seed.transform.scale, unitScale));
        CHECK(bitsEqual(seed.transform.rotation, identity));
    }
}

TEST_CASE("create menu: the resting height is DERIVED from the catalog mirror (task E.5.2, CR5)") {
    for (const Vec3 anchor : {Vec3{4.0F, 0.0F, -6.0F}, Vec3{4.0F, 2.0F, -6.0F}}) {
        CAPTURE(anchor.y);
        constexpr std::array<std::pair<CreateKind, PrimitiveId>, 2> RESTING{{
            {CreateKind::Cube, PrimitiveId::Cube},
            {CreateKind::Sphere, PrimitiveId::Sphere},
        }};
        for (const std::pair<CreateKind, PrimitiveId>& entry : RESTING) {
            CAPTURE(static_cast<int>(entry.first));
            const auto primitive = static_cast<std::uint32_t>(entry.second);
            const float rest = -engine::editor::primitiveLocalBounds(primitive).min.y;
            CHECK(createSeed(entry.first, anchor).transform.position.y == anchor.y + rest);
        }
        const EntitySeed plane = createSeed(CreateKind::Plane, anchor);
        CHECK(plane.transform.position.y == anchor.y);
        CHECK(bitsEqual(plane.transform.scale, Vec3{CREATE_PLANE_EXTENT, 1.0F, CREATE_PLANE_EXTENT}));
    }
}

TEST_CASE("create menu: the sun and the camera ARE the default scene's (task E.5.2, CR6)") {
    World scratch;
    engine::editor::seedDefaultScene(scratch);
    const Entity sun = findNamed(scratch, "Directional Light");
    const Entity cam = findNamed(scratch, "Main Camera");
    REQUIRE(sun.valid());
    REQUIRE(cam.valid());
    REQUIRE(scratch.get<Transform>(sun) != nullptr);
    REQUIRE(scratch.get<Transform>(cam) != nullptr);
    REQUIRE(scratch.get<engine::DirectionalLight>(sun) != nullptr);
    REQUIRE(scratch.get<engine::Camera>(cam) != nullptr);

    const Vec3 a{4.0F, 0.0F, -6.0F};  // every sum below is exact in float
    const EntitySeed sunSeed = createSeed(CreateKind::DirectionalLight, a);
    const EntitySeed camSeed = createSeed(CreateKind::Camera, a);
    CHECK(bitsEqual(sunSeed.transform.rotation, scratch.get<Transform>(sun)->rotation));
    CHECK(bitsEqual(camSeed.transform.position - a, scratch.get<Transform>(cam)->position));
    const engine::DirectionalLight& liveSun = *scratch.get<engine::DirectionalLight>(sun);
    CHECK(std::get<engine::DirectionalLight>(sunSeed.component) == liveSun);
    CHECK(std::get<engine::Camera>(camSeed.component) == *scratch.get<engine::Camera>(cam));
}

TEST_CASE("create menu: the two lights point where D3 says (task E.5.2, CR7)") {
    const Vec3 a{4.0F, 0.0F, -6.0F};
    const Vec3 forward{0.0F, 0.0F, -1.0F};
    const Vec3 spot = createSeed(CreateKind::SpotLight, a).transform.rotation * forward;
    CHECK(std::fabs(spot.x) <= 1e-6F);  // straight DOWN, within 1e-6
    CHECK(std::fabs(spot.y + 1.0F) <= 1e-6F);
    CHECK(std::fabs(spot.z) <= 1e-6F);
    const Vec3 sun = createSeed(CreateKind::DirectionalLight, a).transform.rotation * forward;
    CHECK(sun.y < 0.0F);  // it shines down...
    CHECK(sun.z < 0.0F);  // ...and forward
}

TEST_CASE("create menu: a seed is a pure function of (kind, anchor) (task E.5.2, CR8)") {
    const Vec3 a{4.0F, 0.0F, -6.0F};
    const Vec3 t{3.0F, 1.0F, -2.0F};  // every sum exact in float
    for (const CreateKind kind : ALL_KINDS) {
        CAPTURE(static_cast<int>(kind));
        const EntitySeed s1 = createSeed(kind, a);
        const EntitySeed s2 = createSeed(kind, a + t);
        CHECK(s2.name == s1.name);
        CHECK(s2.label == s1.label);
        CHECK((s2.component == s1.component));
        CHECK(bitsEqual(s2.transform.rotation, s1.transform.rotation));
        CHECK(bitsEqual(s2.transform.scale, s1.transform.scale));
        CHECK(bitsEqual(s2.transform.position, s1.transform.position + t));
    }
}

TEST_CASE("create menu: an out-of-range kind is Empty's seed (task E.5.2, CR9)") {
    const Vec3 a{4.0F, 0.0F, -6.0F};
    const EntitySeed empty = createSeed(CreateKind::Empty, a);
    for (const CreateKind beyond : {CreateKind::Count, static_cast<CreateKind>(200)}) {
        CAPTURE(static_cast<int>(beyond));
        const EntitySeed seed = createSeed(beyond, a);
        CHECK(seed.name.empty());
        CHECK(seed.label == "Create Empty");
        CHECK(std::holds_alternative<std::monostate>(seed.component));
        CHECK(bitsEqual(seed.transform.position, a));
        CHECK(bitsEqual(seed.transform.rotation, Quat{}));
        CHECK(bitsEqual(seed.transform.scale, Vec3{1.0F, 1.0F, 1.0F}));
        // ...i.e. Empty's seed, field for field.
        CHECK(seed.name == empty.name);
        CHECK(seed.label == empty.label);
        CHECK((seed.component == empty.component));
        CHECK(bitsEqual(seed.transform.position, empty.transform.position));
    }
}

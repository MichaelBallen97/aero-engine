// editor/src/create_menu.cpp -- task E.5.2: the Create menu's pure half. No ImGui, no entt, no World access,
// no logging. createSeed is D3's table; every derived quantity is DERIVED here, never restated.
#include <aero/editor/create_menu.hpp>
#include <aero/editor/editor_icons.hpp>  // task E.6.1 -- the roster createKindIcon picks from
#include <aero/editor/entity_ops.hpp>    // DEFAULT_SCENE_CAMERA_POSITION, DEFAULT_SCENE_SUN_PITCH_RADIANS
#include <aero/editor/scene_bounds.hpp>  // primitiveLocalBounds -- the render catalog's local boxes
#include <aero/render/debug_grid.hpp>    // DEBUG_GRID_PLANE_HEIGHT -- the ground, named once
#include <aero/render/mesh.hpp>          // PrimitiveId -- never a bare 0/1/2
#include <aero/scene/camera.hpp>
#include <aero/scene/light.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/spot_light.hpp>
#include <aero/scene/transform.hpp>

#include <array>
#include <cmath>  // std::isfinite
#include <cstdint>
#include <span>
#include <string>

namespace engine::editor {

namespace {

// The TYPED entries, in menu order; each group's run contiguous (D9).
constexpr std::array<CreateMenuEntry, 7> CREATE_MENU_ENTRIES{{
    {.kind = CreateKind::Cube, .group = CreateMenuGroup::Object3D},
    {.kind = CreateKind::Sphere, .group = CreateMenuGroup::Object3D},
    {.kind = CreateKind::Plane, .group = CreateMenuGroup::Object3D},
    {.kind = CreateKind::DirectionalLight, .group = CreateMenuGroup::Light},
    {.kind = CreateKind::PointLight, .group = CreateMenuGroup::Light},
    {.kind = CreateKind::SpotLight, .group = CreateMenuGroup::Light},
    {.kind = CreateKind::Camera, .group = CreateMenuGroup::TopLevel},
}};

// The height that RESTS a primitive's local box on the ground: -min.y * scale.y. 0.5 for the Cube and the
// Sphere, 0 for the Plane -- DERIVED from primitiveLocalBounds, the editor's one mirror of the render
// catalog, so a second copy of 0.5 never exists (forward_renderer.cpp's own warning about exactly that).
[[nodiscard]] float restHeight(render::PrimitiveId id, float scaleY) noexcept {
    return -primitiveLocalBounds(static_cast<std::uint32_t>(id)).min.y * scaleY;
}

[[nodiscard]] EntitySeed primitiveSeed(CreateKind kind, render::PrimitiveId id, Vec3 anchor, Vec3 scale) {
    EntitySeed seed;
    seed.name = createKindLabel(kind);
    seed.label = std::string("Create ") + createKindLabel(kind);
    seed.transform.position = anchor + Vec3{0.0F, restHeight(id, scale.y), 0.0F};
    seed.transform.scale = scale;
    seed.component = MeshRenderer{.primitive = static_cast<std::uint32_t>(id)};
    return seed;
}

}  // namespace

std::span<const CreateMenuEntry> createMenuEntries() noexcept { return CREATE_MENU_ENTRIES; }

const char* createKindLabel(CreateKind kind) noexcept {
    switch (kind) {  // NO default: a new kind without a label is a clang-diagnostic-switch error in CI's lint
        case CreateKind::Empty:
            return "Empty";
        case CreateKind::Cube:
            return "Cube";
        case CreateKind::Sphere:
            return "Sphere";
        case CreateKind::Plane:
            return "Plane";
        case CreateKind::DirectionalLight:
            return "Directional Light";
        case CreateKind::PointLight:
            return "Point Light";
        case CreateKind::SpotLight:
            return "Spot Light";
        case CreateKind::Camera:
            return "Camera";
        case CreateKind::Count:
            break;
    }
    return "";  // Count, and any out-of-range value a cast can produce
}

const char* createMenuGroupLabel(CreateMenuGroup group) noexcept {
    switch (group) {
        case CreateMenuGroup::Object3D:
            return "3D Object";
        case CreateMenuGroup::Light:
            return "Light";
        case CreateMenuGroup::TopLevel:
            break;
    }
    return "";
}

// task E.6.1: what each kind and submenu DRAWS beside its label. The roster names glyphs; this table is
// what a glyph MEANS here. NO default in either switch: a new kind or group without an icon is a
// clang-diagnostic-switch error in CI's lint, never a silent blank.
const char* createKindIcon(CreateKind kind) noexcept {
    switch (kind) {
        case CreateKind::Empty:
            return AERO_ICON_CIRCLE_DASHED;
        case CreateKind::Cube:
            return AERO_ICON_BOX;
        case CreateKind::Sphere:
            return AERO_ICON_CIRCLE;
        case CreateKind::Plane:
            return AERO_ICON_SQUARE;
        case CreateKind::DirectionalLight:
            return AERO_ICON_SUN;
        case CreateKind::PointLight:
            return AERO_ICON_LIGHTBULB;
        case CreateKind::SpotLight:
            return AERO_ICON_LAMP_CEILING;
        case CreateKind::Camera:
            return AERO_ICON_CAMERA;
        case CreateKind::Count:
            break;
    }
    return "";  // Count, and any out-of-range value a cast can produce
}

const char* createMenuGroupIcon(CreateMenuGroup group) noexcept {
    switch (group) {
        case CreateMenuGroup::Object3D:
            return AERO_ICON_SHAPES;
        case CreateMenuGroup::Light:
            return AERO_ICON_LIGHTBULB;
        case CreateMenuGroup::TopLevel:
            break;
    }
    return "";
}

Vec3 createAnchor(Vec3 cameraPivot) noexcept {
    return Vec3{std::isfinite(cameraPivot.x) ? cameraPivot.x : 0.0F, render::DEBUG_GRID_PLANE_HEIGHT,
                std::isfinite(cameraPivot.z) ? cameraPivot.z : 0.0F};
}

EntitySeed createSeed(CreateKind kind, Vec3 anchor) {
    switch (kind) {
        case CreateKind::Cube:
            return primitiveSeed(kind, render::PrimitiveId::Cube, anchor, Vec3::one());
        case CreateKind::Sphere:
            return primitiveSeed(kind, render::PrimitiveId::Sphere, anchor, Vec3::one());
        case CreateKind::Plane:
            return primitiveSeed(kind, render::PrimitiveId::Plane, anchor,
                                 Vec3{CREATE_PLANE_EXTENT, 1.0F, CREATE_PLANE_EXTENT});
        case CreateKind::DirectionalLight: {
            EntitySeed seed{.name = "Directional Light", .label = "Create Directional Light"};
            seed.transform.position = anchor + Vec3{0.0F, CREATE_DIRECTIONAL_LIGHT_HEIGHT, 0.0F};
            // THE SAME EXPRESSION seedDefaultScene evaluates, so the two quaternions are bit-identical (CR6).
            seed.transform.rotation = fromAxisAngle(Vec3{1.0F, 0.0F, 0.0F}, DEFAULT_SCENE_SUN_PITCH_RADIANS);
            seed.component = DirectionalLight{};
            return seed;
        }
        case CreateKind::PointLight: {
            EntitySeed seed{.name = "Point Light", .label = "Create Point Light"};
            seed.transform.position = anchor + Vec3{0.0F, CREATE_POINT_LIGHT_HEIGHT, 0.0F};
            seed.component = PointLight{};
            return seed;
        }
        case CreateKind::SpotLight: {
            EntitySeed seed{.name = "Spot Light", .label = "Create Spot Light"};
            seed.transform.position = anchor + Vec3{0.0F, CREATE_SPOT_LIGHT_HEIGHT, 0.0F};
            seed.transform.rotation = fromAxisAngle(Vec3{1.0F, 0.0F, 0.0F}, CREATE_SPOT_PITCH_RADIANS);
            seed.component = SpotLight{};
            return seed;
        }
        case CreateKind::Camera: {
            EntitySeed seed{.name = "Camera", .label = "Create Camera"};
            seed.transform.position = anchor + DEFAULT_SCENE_CAMERA_POSITION;
            seed.component = Camera{};
            return seed;
        }
        case CreateKind::Empty:
        case CreateKind::Count:
            break;
    }
    // Empty -- and TOTALITY: Count, or any value a cast can produce, is Empty's seed. Unnamed, as Create
    // Empty always was, so its Hierarchy row reads "Entity <index>".
    EntitySeed seed{.name = "", .label = "Create Empty"};
    seed.transform.position = anchor;
    return seed;
}

}  // namespace engine::editor

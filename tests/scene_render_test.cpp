// tests/scene_render_test.cpp — task 1.4.1: buildRenderView (tier-0, always compiled, no GPU) plus a
// gated GPU draw-smoke section (tier-2, mirrors render_cube_test.cpp). aero_tests gains
// aero::scene_render on its link line — the only link-line change (AC-9).

#include <aero/core/math.hpp>
#include <aero/render/render.hpp>
#include <aero/scene/scene.hpp>
#include <aero/scene_render/scene_renderer.hpp>

#include <doctest/doctest.h>

#include <cstdint>

using engine::Camera;
using engine::DirectionalLight;
using engine::Entity;
using engine::Mat3;
using engine::Mat4;
using engine::MeshRenderer;
using engine::PointLight;
using engine::Quat;
using engine::Transform;
using engine::Vec3;
using engine::Vec4;
using engine::World;
using engine::render::MeshInstance;
using engine::render::PrimitiveId;
using engine::render::RenderView;
using engine::scene_render::buildRenderView;
using engine::scene_render::RenderViewScratch;

namespace {
constexpr engine::rhi::Extent2D VIEWPORT{1920U, 1080U};
}  // namespace

TEST_CASE("scene_render: buildRenderView picks the lowest-index camera (D5)") {
    World w;
    RenderViewScratch scratch;

    SUBCASE("one camera") {
        const Entity cam = w.create();
        REQUIRE(w.add<Transform>(cam, Transform{Vec3{1.0F, 2.0F, 3.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.add<Camera>(cam, Camera{}) != nullptr);

        const RenderView view = buildRenderView(w, scratch, VIEWPORT);
        CHECK(view.hasCamera);
        CHECK(view.cameraCount == 1);
        CHECK(engine::approxEquals(view.camera.eyePosition, Vec3{1.0F, 2.0F, 3.0F}));
        CHECK(engine::approxEquals(view.camera.view, engine::viewMatrix(w, cam)));
    }

    SUBCASE("two cameras: the lowest entity index wins") {
        const Entity camA = w.create();  // created first -> the lower index
        REQUIRE(w.add<Transform>(camA, Transform{Vec3{1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.add<Camera>(camA, Camera{}) != nullptr);

        const Entity camB = w.create();
        REQUIRE(w.add<Transform>(camB, Transform{Vec3{9.0F, 9.0F, 9.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.add<Camera>(camB, Camera{}) != nullptr);

        const RenderView view = buildRenderView(w, scratch, VIEWPORT);
        CHECK(view.hasCamera);
        CHECK(view.cameraCount == 2);
        CHECK(engine::approxEquals(view.camera.eyePosition, Vec3{1.0F, 0.0F, 0.0F}));
    }

    SUBCASE("zero cameras: nothing to draw, but instances/diagnostics are still filled") {
        const Entity mesh = w.create();
        REQUIRE(w.add<Transform>(mesh) != nullptr);
        REQUIRE(w.add<MeshRenderer>(mesh) != nullptr);

        const RenderView view = buildRenderView(w, scratch, VIEWPORT);
        CHECK_FALSE(view.hasCamera);
        CHECK(view.cameraCount == 0);
        CHECK(view.instances.size() == 1);  // instances are resolved before the camera check
    }
}

TEST_CASE("scene_render: directional light direction resolves from the entity's -Z world axis (D6)") {
    World w;
    RenderViewScratch scratch;
    // Lights are resolved only once a camera exists (buildRenderView's own early-return path when
    // !hasCamera skips straight to returning — nothing would be drawn anyway); every light case below
    // needs one.
    const Entity cam = w.create();
    REQUIRE(w.add<Transform>(cam) != nullptr);
    REQUIRE(w.add<Camera>(cam) != nullptr);

    const Entity e = w.create();
    const Quat rotateXBy90 = engine::fromAxisAngle(Vec3::unitX(), engine::radians(90.0F));
    REQUIRE(w.add<Transform>(e, Transform{Vec3::zero(), rotateXBy90, Vec3::one()}) != nullptr);
    REQUIRE(w.add<DirectionalLight>(e, DirectionalLight{Vec3{0.5F, 0.6F, 0.7F}, 2.5F}) != nullptr);

    const RenderView view = buildRenderView(w, scratch, VIEWPORT);
    CHECK(view.directionalCount == 1);
    // Ground truth via the same primitive buildRenderView itself uses (transformDirection), proving the
    // WIRING (camera pick, normalization, color/intensity passthrough) rather than re-deriving trig.
    const Vec3 expectedDirection =
        engine::normalize(engine::transformDirection(engine::worldMatrix(w, e), Vec3{0.0F, 0.0F, -1.0F}));
    CHECK(engine::approxEquals(view.directional.direction, expectedDirection));
    CHECK(view.directional.color == Vec3{0.5F, 0.6F, 0.7F});
    CHECK(view.directional.intensity == 2.5F);
}

TEST_CASE("scene_render: multiple DirectionalLights -> lowest entity index wins (D6)") {
    World w;
    RenderViewScratch scratch;
    const Entity cam = w.create();
    REQUIRE(w.add<Transform>(cam) != nullptr);
    REQUIRE(w.add<Camera>(cam) != nullptr);

    const Entity first = w.create();
    REQUIRE(w.add<Transform>(first) != nullptr);
    REQUIRE(w.add<DirectionalLight>(first, DirectionalLight{Vec3{1.0F, 0.0F, 0.0F}, 1.0F}) != nullptr);

    const Entity second = w.create();
    REQUIRE(w.add<Transform>(second) != nullptr);
    REQUIRE(w.add<DirectionalLight>(second, DirectionalLight{Vec3{0.0F, 1.0F, 0.0F}, 9.0F}) != nullptr);

    const RenderView view = buildRenderView(w, scratch, VIEWPORT);
    CHECK(view.directionalCount == 2);
    CHECK(view.directional.color == Vec3{1.0F, 0.0F, 0.0F});  // `first` (lowest index) wins
    CHECK(view.directional.intensity == 1.0F);
}

TEST_CASE("scene_render: point light position resolves from world translation (D6)") {
    World w;
    RenderViewScratch scratch;
    const Entity cam = w.create();
    REQUIRE(w.add<Transform>(cam) != nullptr);
    REQUIRE(w.add<Camera>(cam) != nullptr);

    const Entity e = w.create();
    REQUIRE(w.add<Transform>(e, Transform{Vec3{2.0F, 3.0F, 4.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(w.add<PointLight>(e, PointLight{Vec3{0.9F, 0.8F, 0.7F}, 3.0F, 12.0F}) != nullptr);

    const RenderView view = buildRenderView(w, scratch, VIEWPORT);
    REQUIRE(view.points.size() == 1);
    CHECK(view.points[0].position == Vec3{2.0F, 3.0F, 4.0F});
    CHECK(view.points[0].color == Vec3{0.9F, 0.8F, 0.7F});
    CHECK(view.points[0].intensity == 3.0F);
    CHECK(view.points[0].range == 12.0F);
    CHECK_FALSE(view.pointsTruncated);
}

TEST_CASE("scene_render: more than MAX_POINT_LIGHTS truncates, WARN-latched by SceneRenderer (D6)") {
    World w;
    RenderViewScratch scratch;
    const Entity cam = w.create();
    REQUIRE(w.add<Transform>(cam) != nullptr);
    REQUIRE(w.add<Camera>(cam) != nullptr);

    for (int i = 0; i < 10; ++i) {
        const Entity e = w.create();
        REQUIRE(w.add<Transform>(
                    e, Transform{Vec3{static_cast<float>(i), 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) != nullptr);
        REQUIRE(w.add<PointLight>(e) != nullptr);
    }
    const RenderView view = buildRenderView(w, scratch, VIEWPORT);
    CHECK(view.points.size() == engine::render::MAX_POINT_LIGHTS);
    CHECK(view.pointsTruncated);
}

TEST_CASE("scene_render: buildRenderView matrices (model/normalMatrix/mvp)") {
    World w;
    RenderViewScratch scratch;

    const Entity camEntity = w.create();
    REQUIRE(w.add<Transform>(camEntity, Transform{Vec3{0.0F, 0.0F, 5.0F}, Quat::identity(), Vec3::one()}) != nullptr);
    REQUIRE(w.add<Camera>(camEntity, Camera{}) != nullptr);

    const Entity meshEntity = w.create();
    // Non-uniform scale: a plain toMat3(model) would get the normal matrix wrong here (D6/spec 3.7).
    const Transform meshTransform{Vec3{1.0F, 2.0F, 3.0F}, engine::fromAxisAngle(Vec3::unitY(), engine::radians(30.0F)),
                                  Vec3{2.0F, 1.0F, 0.5F}};
    REQUIRE(w.add<Transform>(meshEntity, meshTransform) != nullptr);
    REQUIRE(w.add<MeshRenderer>(meshEntity, MeshRenderer{0, Vec3{0.2F, 0.4F, 0.6F}}) != nullptr);

    const RenderView view = buildRenderView(w, scratch, VIEWPORT);
    REQUIRE(view.instances.size() == 1);
    const MeshInstance& instance = view.instances[0];

    const Mat4 expectedModel = engine::worldMatrix(w, meshEntity);
    CHECK(engine::approxEquals(instance.model, expectedModel));
    CHECK(instance.color == Vec3{0.2F, 0.4F, 0.6F});

    const Mat3 expectedNormal = engine::transpose(engine::inverse(engine::toMat3(expectedModel)));
    CHECK(engine::approxEquals(instance.normalMatrix.columns[0], engine::toVec4(expectedNormal.columns[0], 0.0F)));
    CHECK(engine::approxEquals(instance.normalMatrix.columns[1], engine::toVec4(expectedNormal.columns[1], 0.0F)));
    CHECK(engine::approxEquals(instance.normalMatrix.columns[2], engine::toVec4(expectedNormal.columns[2], 0.0F)));
    CHECK(instance.normalMatrix.columns[3] == Vec4{0.0F, 0.0F, 0.0F, 1.0F});

    REQUIRE(view.hasCamera);
    const Mat4 expectedView = engine::viewMatrix(w, camEntity);
    const float aspect = static_cast<float>(VIEWPORT.width) / static_cast<float>(VIEWPORT.height);
    const Mat4 expectedProj = engine::projectionMatrix(Camera{}, aspect);
    const Mat4 expectedMvp = expectedProj * expectedView * expectedModel;
    CHECK(engine::approxEquals(instance.mvp, expectedMvp));
}

TEST_CASE("scene_render: out-of-range MeshRenderer::primitive clamps to Cube (D3/AC-12)") {
    World w;
    RenderViewScratch scratch;
    const Entity e = w.create();
    REQUIRE(w.add<Transform>(e) != nullptr);
    REQUIRE(w.add<MeshRenderer>(e, MeshRenderer{99, Vec3::one()}) != nullptr);

    const RenderView view = buildRenderView(w, scratch, VIEWPORT);
    REQUIRE(view.instances.size() == 1);
    CHECK((view.instances[0].primitive == PrimitiveId::Cube));  // extra parens: PrimitiveId has no operator<<
}

TEST_CASE("scene_render: empty world, and a MeshRenderer without a Transform (AC-8)") {
    World w;
    RenderViewScratch scratch;

    SUBCASE("empty world: zero instances, no crash") {
        const RenderView view = buildRenderView(w, scratch, VIEWPORT);
        CHECK(view.instances.empty());
        CHECK_FALSE(view.hasCamera);
    }

    SUBCASE("a MeshRenderer without a Transform is excluded — the query requires both") {
        const Entity e = w.create();
        REQUIRE(w.add<MeshRenderer>(e) != nullptr);
        const RenderView view = buildRenderView(w, scratch, VIEWPORT);
        CHECK(view.instances.empty());
    }
}

#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/core/vfs.hpp>
    #include <aero/platform/platform.hpp>
    #include <aero/rhi/rhi.hpp>

    #include "rhi_test_support.hpp"

    // <ostream> is load-bearing on MSVC for string_view/enum CHECKs (see render_cube_test.cpp's note).
    #include <memory>
    #include <optional>
    #include <ostream>
    #include <utility>

TEST_CASE("scene_render: GPU draw smoke — SceneRenderer draws a world for >=3 frames incl. a resize (AC-9)") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = engine::rhi::Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero scene-render test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }
    auto renderer = engine::render::Renderer::create(*device, *window, {.depth = true});
    REQUIRE(renderer.has_value());

    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));  // shared define (CORRECTION 1)
    auto sceneRenderer =
        engine::scene_render::SceneRenderer::create(*device, vfs, renderer->colorFormat(), renderer->depthFormat());
    REQUIRE(sceneRenderer.has_value());

    // An in-code World: a camera looking toward the origin, a cube + a sphere either side of it, a
    // directional light, and a point light — every consumer of the shading model, drawn together.
    World world;
    const Entity camEntity = world.create();
    REQUIRE(world.add<Transform>(camEntity, Transform{Vec3{0.0F, 1.5F, 4.0F}, Quat::identity(), Vec3::one()}) !=
            nullptr);
    REQUIRE(world.add<Camera>(camEntity, Camera{}) != nullptr);

    const Entity cubeEntity = world.create();
    REQUIRE(world.add<Transform>(cubeEntity, Transform{Vec3{-1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) !=
            nullptr);
    REQUIRE(world.add<MeshRenderer>(cubeEntity, MeshRenderer{static_cast<std::uint32_t>(PrimitiveId::Cube),
                                                             Vec3{0.85F, 0.30F, 0.30F}}) != nullptr);

    const Entity sphereEntity = world.create();
    REQUIRE(world.add<Transform>(sphereEntity, Transform{Vec3{1.0F, 0.0F, 0.0F}, Quat::identity(), Vec3::one()}) !=
            nullptr);
    REQUIRE(world.add<MeshRenderer>(sphereEntity, MeshRenderer{static_cast<std::uint32_t>(PrimitiveId::Sphere),
                                                               Vec3{0.30F, 0.80F, 0.30F}}) != nullptr);

    const Entity dirEntity = world.create();
    REQUIRE(world.add<Transform>(dirEntity,
                                 Transform{Vec3::zero(), engine::fromAxisAngle(Vec3::unitX(), engine::radians(45.0F)),
                                           Vec3::one()}) != nullptr);
    REQUIRE(world.add<DirectionalLight>(dirEntity, DirectionalLight{Vec3::one(), 1.0F}) != nullptr);

    const Entity pointEntity = world.create();
    REQUIRE(world.add<Transform>(pointEntity, Transform{Vec3{0.0F, 2.0F, 2.0F}, Quat::identity(), Vec3::one()}) !=
            nullptr);
    REQUIRE(world.add<PointLight>(pointEntity, PointLight{Vec3{1.0F, 0.6F, 0.2F}, 3.0F, 8.0F}) != nullptr);

    const engine::rhi::Color sky{0.05F, 0.07F, 0.10F, 1.0F};
    for (int i = 0; i < 3; ++i) {
        std::optional<engine::render::Frame> f = renderer->beginFrame(sky);
        REQUIRE(f.has_value());
        sceneRenderer->render(world, *f);
        CHECK(renderer->endFrame(std::move(*f)));
    }

    // Exercise the resize path (render_cube_test.cpp's C-4 pattern): the depth target and the scene's
    // projection aspect both must survive a live extent change.
    window->setSize(640, 360);
    ctx.newFrame();
    engine::platform::Event ev;
    while (ctx.pollEvent(ev)) {
    }
    if (std::optional<engine::render::Frame> f = renderer->beginFrame(sky)) {
        sceneRenderer->render(world, *f);
        CHECK(renderer->endFrame(std::move(*f)));
    }
}

TEST_CASE("scene_render: SceneRenderer::create fails on a non-depth Renderer (D11/AC-12)") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = engine::rhi::Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero scene-render test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }
    auto renderer = engine::render::Renderer::create(*device, *window, {.depth = false});
    REQUIRE(renderer.has_value());
    CHECK((renderer->depthFormat() ==
           engine::rhi::TextureFormat::Invalid));  // extra parens: no operator<< (rhi_types_test.cpp precedent)

    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    auto sceneRenderer =
        engine::scene_render::SceneRenderer::create(*device, vfs, renderer->colorFormat(), renderer->depthFormat());
    CHECK_FALSE(sceneRenderer.has_value());
}

#endif  // AERO_SHADER_TOOLS_ENABLED

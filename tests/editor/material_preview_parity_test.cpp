// tests/editor/material_preview_parity_test.cpp — task E.2.4: the material preview's path and the
// viewport's path, drawn from the SAME lighting into two identical HDR targets, compared BYTE FOR
// BYTE (PX1–PX4).
//
// WHY THIS TU EXISTS AT ALL. Every other arm of this task asserts a struct crossing a seam. "They
// match" is the deliverable, and the only reading a test can assert is: the same inputs to the same
// shaders produce the same bytes. The comparison is made BEFORE the tonemap, on the RGBA16Float scene
// target, where both sides run the same shaders, the same packSkyCamera, an identity model and the
// same (proj * view) * model association -- so exactness is available, and E.2.1's lesson is that an
// assertion is only as strong as the tolerance it is written at.
//
// WHY IT LIVES ON aero_editor_imgui_test: it is the ONE editor test target that links
// aero::scene_render, which side B needs for SceneRenderer::create and the two resolvers (the SL1-SL10
// precedent). aero_tests cannot name MaterialPreviewRig without linking aero::editor_core, and a new
// target would move ctest -N, which this task forbids.
//
// NO `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` -- imgui_layer_test.cpp already defines it for this
// target and a second is a duplicate-main link error on every lane. NO `toString` anywhere: doctest's
// DOCTEST_STRINGIFY expands to an unqualified toString(...) found by ADL, which hard-errors inside
// doctest.h.

#include <aero/core/vfs.hpp>
#include <aero/editor/material_preview_rig.hpp>
#include <aero/platform/platform.hpp>
#include <aero/render/render.hpp>
#include <aero/rhi/device.hpp>
#include <aero/scene/scene.hpp>
#include <aero/scene/world.hpp>
#include <aero/scene_render/scene_renderer.hpp>

#include "../rhi_test_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#if AERO_SHADER_TOOLS_ENABLED

namespace {

// ---- the readback harness, copied from tests/render_sky_test.cpp ---------------------------------
// Nothing is shared between aero_tests and this target, so these are this TU's own. Half4 lives at
// file-anonymous-namespace scope, NOT inside a TEST_CASE: [class.friend]/6 forbids defining a friend
// in a local class, and without the operator<< below every assertion prints {?} == {?} on the one run
// that matters. The anonymous namespace IS the type's own namespace, which is what makes the printer
// reachable by ADL from inside doctest::detail.
struct Half4 {
    std::uint16_t r = 0;
    std::uint16_t g = 0;
    std::uint16_t b = 0;
    std::uint16_t a = 0;
    [[nodiscard]] bool operator==(const Half4&) const = default;
};

// HEX, because these are bit patterns rather than numbers. An operator<<, NEVER a toString.
std::ostream& operator<<(std::ostream& out, const Half4& value) {
    out << "half4(0x" << std::hex << value.r << ", 0x" << value.g << ", 0x" << value.b << ", 0x" << value.a << ")"
        << std::dec;
    return out;
}

// ROUND-TO-NEAREST-EVEN float -> half, so an assertion can say "the drawn texel IS the CPU's encoding
// of the oracle" rather than "close to".
[[nodiscard]] std::uint16_t encodeHalf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    const std::uint32_t rawExponent = (bits >> 23U) & 0xFFU;
    std::uint32_t mantissa = bits & 0x007FFFFFU;

    if (rawExponent == 0xFFU) {  // inf / NaN -- a quiet NaN keeps its quiet bit
        return static_cast<std::uint16_t>(sign | 0x7C00U | (mantissa != 0U ? 0x0200U : 0U));
    }
    const std::int32_t exponent = static_cast<std::int32_t>(rawExponent) - 127 + 15;
    if (exponent >= 0x1F) {  // overflows the half range
        return static_cast<std::uint16_t>(sign | 0x7C00U);
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<std::uint16_t>(sign);  // underflows to a signed zero
        }
        mantissa |= 0x00800000U;  // the float's implicit leading bit becomes explicit
        const auto shift = static_cast<std::uint32_t>(14 - exponent);
        const std::uint32_t truncated = mantissa >> shift;
        const std::uint32_t roundBit = 1U << (shift - 1U);
        const bool roundUp =
            (mantissa & roundBit) != 0U && (((mantissa & (roundBit - 1U)) != 0U) || ((truncated & 1U) != 0U));
        return static_cast<std::uint16_t>(sign | (truncated + (roundUp ? 1U : 0U)));
    }
    const std::uint32_t truncated = (static_cast<std::uint32_t>(exponent) << 10U) | (mantissa >> 13U);
    constexpr std::uint32_t ROUND_BIT = 1U << 12U;
    const bool roundUp =
        (mantissa & ROUND_BIT) != 0U && (((mantissa & (ROUND_BIT - 1U)) != 0U) || ((truncated & 1U) != 0U));
    return static_cast<std::uint16_t>(sign | (truncated + (roundUp ? 1U : 0U)));
}

// "N half-ulps", for SAME-SIGN FINITE halves: two such values differ by N ulps iff their bit patterns,
// read as integers, differ by N.
[[nodiscard]] std::uint32_t halfUlpDistance(std::uint16_t a, std::uint16_t b) {
    return a >= b ? static_cast<std::uint32_t>(a - b) : static_cast<std::uint32_t>(b - a);
}

[[nodiscard]] Half4 halfAt(const std::vector<std::byte>& pixels, std::uint32_t textureWidth, std::uint32_t row,
                           std::uint32_t column) {
    const std::size_t base = (((static_cast<std::size_t>(row) * textureWidth) + column) * 8U);
    const auto at = [&pixels](std::size_t index) {
        return static_cast<std::uint16_t>(static_cast<std::uint8_t>(pixels[index]) |
                                          (static_cast<std::uint8_t>(pixels[index + 1U]) << 8U));
    };
    return Half4{at(base), at(base + 2U), at(base + 4U), at(base + 6U)};
}

[[nodiscard]] Half4 encodeHalf4(engine::Vec3 rgb, float alpha) {
    return Half4{encodeHalf(rgb.x), encodeHalf(rgb.y), encodeHalf(rgb.z), encodeHalf(alpha)};
}

// ---- the A/B's own constants ---------------------------------------------------------------------
constexpr std::uint32_t EXTENT = 64;
constexpr std::size_t READBACK_BYTES = static_cast<std::size_t>(EXTENT) * EXTENT * 8U;
// UNEQUAL TO EVERY COLOUR COMPARED AGAINST, so "A is not just the clear" is a real question. Both
// sides clear to it, so it cancels out of the identity anyway.
constexpr engine::rhi::Color CLEAR{0.9F, 0.1F, 0.9F, 1.0F};
// Off every axis, so an axis-aligned bug cannot hide.
constexpr float ANGLE = 0.7F;

// Names the FIRST differing byte and both values before the CHECK -- a bare CHECK over 32 768 bytes
// is unreadable at exactly the wrong moment.
void checkBytesEqual(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
    REQUIRE(a.size() == b.size());
    std::size_t firstDiff = a.size();
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            firstDiff = i;
            break;
        }
    }
    if (firstDiff != a.size()) {
        const auto texel = firstDiff / 8U;
        INFO("first differing byte at offset ", firstDiff, " (texel ", texel, " = row ", texel / EXTENT, ", column ",
             texel % EXTENT, "): A=", static_cast<int>(static_cast<std::uint8_t>(a[firstDiff])),
             " B=", static_cast<int>(static_cast<std::uint8_t>(b[firstDiff])));
        CHECK(firstDiff == a.size());
    } else {
        CHECK(firstDiff == a.size());
    }
}

[[nodiscard]] std::size_t differingTexels(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
    std::size_t differing = 0;
    for (std::uint32_t row = 0; row < EXTENT; ++row) {
        for (std::uint32_t column = 0; column < EXTENT; ++column) {
            if (!(halfAt(a, EXTENT, row, column) == halfAt(b, EXTENT, row, column))) {
                ++differing;
            }
        }
    }
    return differing;
}

// The Environment each arm seeds. `useDefault` takes engine::Environment's own defaults verbatim.
struct ArmEnvironment {
    bool useDefault = true;
    std::uint32_t backgroundMode = 0;
    engine::Vec3 solidColor{0.06F, 0.06F, 0.07F};
    std::uint32_t ambientMode = 0;
};

}  // namespace

namespace {

// The whole A/B, so PX1-PX4 differ only in their inputs and every arm runs the identical code.
struct ParityResult {
    std::vector<std::byte> bytesA;
    std::vector<std::byte> bytesB;
    engine::editor::MaterialPreviewLighting lighting;
};

}  // namespace

    // A macro cannot hold the preamble: AERO_SKIP_OR_FAIL returns from the ENCLOSING function.
    #define AERO_PX_PREAMBLE()                                                                     \
        const engine::platform::Context ctx{{.headless = false}};                                  \
        if (!ctx.valid()) {                                                                        \
            AERO_SKIP_OR_FAIL("no real video driver available");                                   \
        }                                                                                          \
        std::optional<engine::rhi::Device> device = engine::rhi::Device::create();                 \
        if (!device) {                                                                             \
            AERO_SKIP_OR_FAIL("no GPU device available");                                          \
        }                                                                                          \
        if (!device->supportsTextureFormat(                                                        \
                engine::rhi::TextureFormat::RGBA16Float,                                           \
                engine::rhi::TextureUsage::Sampler | engine::rhi::TextureUsage::ColorTarget)) {    \
            AERO_SKIP_OR_FAIL("device does not support RGBA16Float as a sampleable color target"); \
        }                                                                                          \
        engine::VirtualFileSystem vfs;                                                             \
        vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR))

namespace {

// Seeds the World BOTH sides read: side B draws it directly, and side A's lighting is resolved OUT of
// it by the editor's own three lines. Built with world.create() + world.add<T> directly, which is this
// tree's idiom for a seeded world.
void seedParityWorld(engine::World& world, const ArmEnvironment& environment, bool withSun) {
    const engine::Entity camera = world.create();
    // PRESENT so the walk's 0-camera arm is moot whether or not the override skips it. Measured at
    // commit 1: the override arm of buildRenderView's three-arm decision comes FIRST, so an override
    // with zero Camera entities does NOT take the early return -- but seeding one costs nothing and
    // means the harness does not depend on that reading.
    world.add<engine::Transform>(
        camera, engine::Transform{engine::Vec3{0.0F, 0.0F, 5.0F}, engine::Quat::identity(), engine::Vec3::one()});
    world.add<engine::Camera>(camera, engine::Camera{});

    // NO Transform: createEntity would always add one, so an entity that must not carry one is built
    // with world.create() + add<T> directly (entity_ops.cpp's precedent).
    const engine::Entity environmentEntity = world.create();
    engine::Environment component{};
    if (!environment.useDefault) {
        component.backgroundMode = environment.backgroundMode;
        component.solidColor = environment.solidColor;
        component.ambientMode = environment.ambientMode;
    }
    world.add<engine::Environment>(environmentEntity, component);

    if (withSun) {
        const engine::Entity sun = world.create();
        world.add<engine::Transform>(
            sun, engine::Transform{engine::Vec3::zero(),
                                   engine::fromAxisAngle(engine::Vec3::unitX(), engine::radians(-50.0F)),
                                   engine::Vec3::one()});
        // castsShadows = false is THE HARNESS'S OWN PRECONDITION, not a convenience: side A's view sets
        // shadowsEnabled = false, and side B's SceneRenderer would otherwise run a shadow pass side A
        // cannot. Seed S28 exists to keep a future edit from "fixing" this by weakening an assertion,
        // and MEASURED, it reddens PX4 ALONE of the four: at angle 0.7 the shadow changes no texel of
        // this sphere, and only PX4's second angle exposes it. Deleting PX4 would make S28 invisible.
        world.add<engine::DirectionalLight>(sun, engine::DirectionalLight{.castsShadows = false});
    }

    const engine::Entity sphere = world.create();
    world.add<engine::Transform>(sphere, engine::Transform{});
    world.add<engine::MeshRenderer>(sphere, engine::MeshRenderer{.primitive = 1U});  // 1 == Sphere
}

}  // namespace

TEST_CASE("editor: the preview's path and the viewport's path agree BYTE FOR BYTE (task E.2.4, PX1)") {
    AERO_PX_PREAMBLE();

    engine::World world;
    seedParityWorld(world, ArmEnvironment{}, true);

    // THE EDITOR'S OWN THREE LINES, verbatim -- the ones EditorApp::tick runs.
    const engine::scene_render::ResolvedEnvironment resolvedEnvironment =
        engine::scene_render::resolveEnvironment(world);
    const engine::scene_render::ResolvedDirectionalLight resolvedSun =
        engine::scene_render::resolveDirectionalLight(world);
    const engine::editor::MaterialPreviewLighting lighting{
        .environment = resolvedEnvironment.data, .sun = resolvedSun.data, .hasSun = resolvedSun.entity.valid()};
    REQUIRE(lighting.hasSun);

    const engine::render::CameraView camera =
        engine::editor::materialPreviewCamera(engine::editor::DEFAULT_MATERIAL_PREVIEW_RIG, ANGLE, 1.0F);

    // ---- side A: the preview's path ---------------------------------------------------------------
    std::vector<std::byte> bytesA(READBACK_BYTES, std::byte{0xAB});  // a readback that wrote nothing shows
    {
        std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(
            *device, {EXTENT, EXTENT},
            {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
        REQUIRE(target.has_value());
        std::optional<engine::render::ForwardRenderer> forward = engine::render::ForwardRenderer::create(
            *device, vfs,
            {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat(), .shadowMapResolution = 0});
        REQUIRE(forward.has_value());
        std::optional<engine::render::SkyPass> sky = engine::render::SkyPass::create(
            *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
        REQUIRE(sky.has_value());

        // DEFAULT_MATERIAL_PARAMS, NEVER MaterialParams{}: the latter defaults metallicFactor to
        // glTF's 1.0, and a metal with no environment to reflect renders near-black -- which would
        // make every colour assertion here vacuous (E.1.4's lesson).
        const engine::render::MaterialHandle material =
            forward->createMaterial(engine::render::DEFAULT_MATERIAL_PARAMS, {});

        std::array<engine::render::MeshInstance, 1> instances{};
        instances.at(0).primitive = engine::render::PrimitiveId::Sphere;
        instances.at(0).model = engine::Mat4::identity();
        instances.at(0).normalMatrix = engine::Mat4::identity();
        instances.at(0).color = engine::Vec3::one();
        instances.at(0).material = material;
        const engine::render::RenderView view = engine::editor::materialPreviewView(camera, lighting, instances);

        std::optional<engine::render::Frame> frame = target->beginFrame(CLEAR);
        REQUIRE(frame.has_value());
        sky->draw(*frame, view);
        forward->draw(*frame, view);
        REQUIRE(target->endFrame(std::move(*frame)));
        REQUIRE(device->readbackTexture(target->colorTexture(), 0U, bytesA));
        forward->destroyMaterial(material);
    }

    // ---- side B: the viewport's path ---------------------------------------------------------------
    std::vector<std::byte> bytesB(READBACK_BYTES, std::byte{0xAB});
    {
        std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(
            *device, {EXTENT, EXTENT},
            {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
        REQUIRE(target.has_value());
        std::optional<engine::scene_render::SceneRenderer> scene =
            engine::scene_render::SceneRenderer::create(*device, vfs, target->colorFormat(), target->depthFormat());
        REQUIRE(scene.has_value());

        std::optional<engine::render::Frame> frame = target->beginFrame(CLEAR);
        REQUIRE(frame.has_value());
        scene->render(world, *frame, &camera);  // THE SAME camera, by pointer, as the override
        REQUIRE(target->endFrame(std::move(*frame)));
        REQUIRE(device->readbackTexture(target->colorTexture(), 0U, bytesB));
    }

    checkBytesEqual(bytesA, bytesB);

    // ---- the anti-vacuity trio ---------------------------------------------------------------------
    SUBCASE("A is not merely the clear colour") {
        const Half4 clear = encodeHalf4(engine::Vec3{CLEAR.r, CLEAR.g, CLEAR.b}, CLEAR.a);
        std::size_t unclearedTexels = 0;
        for (std::uint32_t row = 0; row < EXTENT; ++row) {
            for (std::uint32_t column = 0; column < EXTENT; ++column) {
                if (!(halfAt(bytesA, EXTENT, row, column) == clear)) {
                    ++unclearedTexels;
                }
            }
        }
        INFO("texels differing from the clear: ", unclearedTexels);
        CHECK(unclearedTexels > 0U);
    }

    SUBCASE("doubling the sun moves the picture, so the comparison is sensitive to it") {
        engine::Entity sunEntity{};
        world.eachEntity([&](engine::Entity e) {
            if (world.has<engine::DirectionalLight>(e)) {
                sunEntity = e;
            }
        });
        REQUIRE(sunEntity.valid());
        auto* const light = world.get<engine::DirectionalLight>(sunEntity);
        REQUIRE(light != nullptr);
        light->intensity *= 2.0F;

        std::vector<std::byte> brighter(READBACK_BYTES, std::byte{0xAB});
        std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(
            *device, {EXTENT, EXTENT},
            {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
        REQUIRE(target.has_value());
        std::optional<engine::scene_render::SceneRenderer> scene =
            engine::scene_render::SceneRenderer::create(*device, vfs, target->colorFormat(), target->depthFormat());
        REQUIRE(scene.has_value());
        std::optional<engine::render::Frame> frame = target->beginFrame(CLEAR);
        REQUIRE(frame.has_value());
        scene->render(world, *frame, &camera);
        REQUIRE(target->endFrame(std::move(*frame)));
        REQUIRE(device->readbackTexture(target->colorTexture(), 0U, brighter));

        const std::size_t moved = differingTexels(bytesB, brighter);
        INFO("texels moved by doubling the sun: ", moved, " of ", EXTENT * EXTENT);
        CHECK(moved > (static_cast<std::size_t>(EXTENT) * EXTENT) / 100U);  // > 1 %
    }

    SUBCASE("changing skyColor moves a corner, so the comparison is sensitive to the environment") {
        engine::Entity environmentEntity{};
        world.eachEntity([&](engine::Entity e) {
            if (world.has<engine::Environment>(e)) {
                environmentEntity = e;
            }
        });
        REQUIRE(environmentEntity.valid());
        auto* const component = world.get<engine::Environment>(environmentEntity);
        REQUIRE(component != nullptr);
        component->skyColor = engine::Vec3{0.85F, 0.15F, 0.05F};

        std::vector<std::byte> recoloured(READBACK_BYTES, std::byte{0xAB});
        std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(
            *device, {EXTENT, EXTENT},
            {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
        REQUIRE(target.has_value());
        std::optional<engine::scene_render::SceneRenderer> scene =
            engine::scene_render::SceneRenderer::create(*device, vfs, target->colorFormat(), target->depthFormat());
        REQUIRE(scene.has_value());
        std::optional<engine::render::Frame> frame = target->beginFrame(CLEAR);
        REQUIRE(frame.has_value());
        scene->render(world, *frame, &camera);
        REQUIRE(target->endFrame(std::move(*frame)));
        REQUIRE(device->readbackTexture(target->colorTexture(), 0U, recoloured));

        CHECK_FALSE(halfAt(bytesB, EXTENT, 1U, 1U) == halfAt(recoloured, EXTENT, 1U, 1U));
    }
}

TEST_CASE("editor: the preview and the viewport agree under Solid + Flat, exactly (task E.2.4, PX2)") {
    AERO_PX_PREAMBLE();

    const engine::Vec3 solid{0.125F, 0.375F, 0.875F};
    engine::World world;
    seedParityWorld(
        world, ArmEnvironment{.useDefault = false, .backgroundMode = 1U, .solidColor = solid, .ambientMode = 1U}, true);

    const engine::scene_render::ResolvedEnvironment resolvedEnvironment =
        engine::scene_render::resolveEnvironment(world);
    const engine::scene_render::ResolvedDirectionalLight resolvedSun =
        engine::scene_render::resolveDirectionalLight(world);
    const engine::editor::MaterialPreviewLighting lighting{
        .environment = resolvedEnvironment.data, .sun = resolvedSun.data, .hasSun = resolvedSun.entity.valid()};
    const engine::render::CameraView camera =
        engine::editor::materialPreviewCamera(engine::editor::DEFAULT_MATERIAL_PREVIEW_RIG, ANGLE, 1.0F);

    std::vector<std::byte> bytesA(READBACK_BYTES, std::byte{0xAB});
    {
        std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(
            *device, {EXTENT, EXTENT},
            {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
        REQUIRE(target.has_value());
        std::optional<engine::render::ForwardRenderer> forward = engine::render::ForwardRenderer::create(
            *device, vfs,
            {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat(), .shadowMapResolution = 0});
        REQUIRE(forward.has_value());
        std::optional<engine::render::SkyPass> sky = engine::render::SkyPass::create(
            *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
        REQUIRE(sky.has_value());
        const engine::render::MaterialHandle material =
            forward->createMaterial(engine::render::DEFAULT_MATERIAL_PARAMS, {});
        std::array<engine::render::MeshInstance, 1> instances{};
        instances.at(0).primitive = engine::render::PrimitiveId::Sphere;
        instances.at(0).model = engine::Mat4::identity();
        instances.at(0).normalMatrix = engine::Mat4::identity();
        instances.at(0).color = engine::Vec3::one();
        instances.at(0).material = material;
        const engine::render::RenderView view = engine::editor::materialPreviewView(camera, lighting, instances);
        std::optional<engine::render::Frame> frame = target->beginFrame(CLEAR);
        REQUIRE(frame.has_value());
        sky->draw(*frame, view);
        forward->draw(*frame, view);
        REQUIRE(target->endFrame(std::move(*frame)));
        REQUIRE(device->readbackTexture(target->colorTexture(), 0U, bytesA));
        forward->destroyMaterial(material);
    }

    std::vector<std::byte> bytesB(READBACK_BYTES, std::byte{0xAB});
    {
        std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(
            *device, {EXTENT, EXTENT},
            {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
        REQUIRE(target.has_value());
        std::optional<engine::scene_render::SceneRenderer> scene =
            engine::scene_render::SceneRenderer::create(*device, vfs, target->colorFormat(), target->depthFormat());
        REQUIRE(scene.has_value());
        std::optional<engine::render::Frame> frame = target->beginFrame(CLEAR);
        REQUIRE(frame.has_value());
        scene->render(world, *frame, &camera);
        REQUIRE(target->endFrame(std::move(*frame)));
        REQUIRE(device->readbackTexture(target->colorTexture(), 0U, bytesB));
    }

    checkBytesEqual(bytesA, bytesB);

    // ...AND EXACTLY THE SOLID COLOUR AT EVERY CORNER. A Solid sky is two exactly-zero deltas, and
    // `x + 0*w` is exact on every backend (E.2.1's delta rule), so this is available here as an
    // equality rather than a tolerance -- the strongest form the sky's own contract permits.
    const Half4 expected = encodeHalf4(solid, 1.0F);
    CHECK(halfAt(bytesA, EXTENT, 0U, 0U) == expected);
    CHECK(halfAt(bytesA, EXTENT, 0U, EXTENT - 1U) == expected);
    CHECK(halfAt(bytesA, EXTENT, EXTENT - 1U, 0U) == expected);
    CHECK(halfAt(bytesA, EXTENT, EXTENT - 1U, EXTENT - 1U) == expected);
    // Anti-vacuity: the picture is NOT uniformly the solid colour -- the sphere is in it.
    CHECK_FALSE(halfAt(bytesA, EXTENT, EXTENT / 2U, EXTENT / 2U) == expected);
}

TEST_CASE("editor: with NO directional light both paths agree and the sphere is darker (task E.2.4, PX3)") {
    AERO_PX_PREAMBLE();

    const engine::render::CameraView camera =
        engine::editor::materialPreviewCamera(engine::editor::DEFAULT_MATERIAL_PREVIEW_RIG, ANGLE, 1.0F);

    // Renders side A and side B for a world with or without a sun, and hands back both readbacks.
    const auto runArm = [&](bool withSun) {
        engine::World world;
        seedParityWorld(world, ArmEnvironment{}, withSun);
        const engine::scene_render::ResolvedEnvironment resolvedEnvironment =
            engine::scene_render::resolveEnvironment(world);
        const engine::scene_render::ResolvedDirectionalLight resolvedSun =
            engine::scene_render::resolveDirectionalLight(world);
        const engine::editor::MaterialPreviewLighting lighting{
            .environment = resolvedEnvironment.data, .sun = resolvedSun.data, .hasSun = resolvedSun.entity.valid()};
        ParityResult result{std::vector<std::byte>(READBACK_BYTES, std::byte{0xAB}),
                            std::vector<std::byte>(READBACK_BYTES, std::byte{0xAB}), lighting};
        {
            std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(
                *device, {EXTENT, EXTENT},
                {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
            REQUIRE(target.has_value());
            std::optional<engine::render::ForwardRenderer> forward = engine::render::ForwardRenderer::create(
                *device, vfs,
                {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat(), .shadowMapResolution = 0});
            REQUIRE(forward.has_value());
            std::optional<engine::render::SkyPass> sky = engine::render::SkyPass::create(
                *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
            REQUIRE(sky.has_value());
            const engine::render::MaterialHandle material =
                forward->createMaterial(engine::render::DEFAULT_MATERIAL_PARAMS, {});
            std::array<engine::render::MeshInstance, 1> instances{};
            instances.at(0).primitive = engine::render::PrimitiveId::Sphere;
            instances.at(0).model = engine::Mat4::identity();
            instances.at(0).normalMatrix = engine::Mat4::identity();
            instances.at(0).color = engine::Vec3::one();
            instances.at(0).material = material;
            const engine::render::RenderView view = engine::editor::materialPreviewView(camera, lighting, instances);
            std::optional<engine::render::Frame> frame = target->beginFrame(CLEAR);
            REQUIRE(frame.has_value());
            sky->draw(*frame, view);
            forward->draw(*frame, view);
            REQUIRE(target->endFrame(std::move(*frame)));
            REQUIRE(device->readbackTexture(target->colorTexture(), 0U, result.bytesA));
            forward->destroyMaterial(material);
        }
        {
            std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(
                *device, {EXTENT, EXTENT},
                {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
            REQUIRE(target.has_value());
            std::optional<engine::scene_render::SceneRenderer> scene =
                engine::scene_render::SceneRenderer::create(*device, vfs, target->colorFormat(), target->depthFormat());
            REQUIRE(scene.has_value());
            std::optional<engine::render::Frame> frame = target->beginFrame(CLEAR);
            REQUIRE(frame.has_value());
            scene->render(world, *frame, &camera);
            REQUIRE(target->endFrame(std::move(*frame)));
            REQUIRE(device->readbackTexture(target->colorTexture(), 0U, result.bytesB));
        }
        return result;
    };

    const ParityResult lit = runArm(true);
    const ParityResult unlit = runArm(false);
    REQUIRE(lit.lighting.hasSun);
    REQUIRE_FALSE(unlit.lighting.hasSun);
    CHECK(unlit.lighting.sun.intensity == 0.0F);  // the bridge's own "none" encoding

    checkBytesEqual(unlit.bytesA, unlit.bytesB);

    // ...AND THE SPHERE IS DARKER, stated as a DISTANCE GREATER THAN THE AGREEMENT TOLERANCE rather
    // than as `!=` (E.2.1's SB9/SB16 lesson: "different" at a tolerance the siblings need is not a
    // finding). Two half-ulps is the tolerance the sky battery states for its own oracles, and the
    // MEASURED headroom is enormous: 1969 / 1584 / 1095 half-ulps on r/g/b. So the `> 2` form and a
    // bare `!=` agree on THIS picture -- the tolerance is the right thing to ship, and its own
    // mutant is unobservable here, which is a fact about the headroom rather than about the arm.
    const Half4 litCentre = halfAt(lit.bytesA, EXTENT, EXTENT / 2U, EXTENT / 2U);
    const Half4 unlitCentre = halfAt(unlit.bytesA, EXTENT, EXTENT / 2U, EXTENT / 2U);
    INFO("lit centre ", litCentre, " unlit centre ", unlitCentre);
    const bool darkerBeyondTolerance = halfUlpDistance(litCentre.r, unlitCentre.r) > 2U ||
                                       halfUlpDistance(litCentre.g, unlitCentre.g) > 2U ||
                                       halfUlpDistance(litCentre.b, unlitCentre.b) > 2U;
    CHECK(darkerBeyondTolerance);
    // ...and DARKER, not merely different, on at least one channel.
    const bool darker = unlitCentre.r < litCentre.r || unlitCentre.g < litCentre.g || unlitCentre.b < litCentre.b;
    CHECK(darker);
}

TEST_CASE("editor: the two paths agree at two further orbit angles (task E.2.4, PX4)") {
    AERO_PX_PREAMBLE();

    engine::World world;
    seedParityWorld(world, ArmEnvironment{}, true);
    const engine::scene_render::ResolvedEnvironment resolvedEnvironment =
        engine::scene_render::resolveEnvironment(world);
    const engine::scene_render::ResolvedDirectionalLight resolvedSun =
        engine::scene_render::resolveDirectionalLight(world);
    const engine::editor::MaterialPreviewLighting lighting{
        .environment = resolvedEnvironment.data, .sun = resolvedSun.data, .hasSun = resolvedSun.entity.valid()};

    // So the mvp association is not an accident of one pose.
    const std::array<float, 2> angles{0.0F, 3.9F};
    std::array<std::vector<std::byte>, 2> perAngle{};
    for (std::size_t i = 0; i < angles.size(); ++i) {
        CAPTURE(angles.at(i));
        const engine::render::CameraView camera =
            engine::editor::materialPreviewCamera(engine::editor::DEFAULT_MATERIAL_PREVIEW_RIG, angles.at(i), 1.0F);

        std::vector<std::byte> bytesA(READBACK_BYTES, std::byte{0xAB});
        {
            std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(
                *device, {EXTENT, EXTENT},
                {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
            REQUIRE(target.has_value());
            std::optional<engine::render::ForwardRenderer> forward = engine::render::ForwardRenderer::create(
                *device, vfs,
                {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat(), .shadowMapResolution = 0});
            REQUIRE(forward.has_value());
            std::optional<engine::render::SkyPass> sky = engine::render::SkyPass::create(
                *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
            REQUIRE(sky.has_value());
            const engine::render::MaterialHandle material =
                forward->createMaterial(engine::render::DEFAULT_MATERIAL_PARAMS, {});
            std::array<engine::render::MeshInstance, 1> instances{};
            instances.at(0).primitive = engine::render::PrimitiveId::Sphere;
            instances.at(0).model = engine::Mat4::identity();
            instances.at(0).normalMatrix = engine::Mat4::identity();
            instances.at(0).color = engine::Vec3::one();
            instances.at(0).material = material;
            const engine::render::RenderView view = engine::editor::materialPreviewView(camera, lighting, instances);
            std::optional<engine::render::Frame> frame = target->beginFrame(CLEAR);
            REQUIRE(frame.has_value());
            sky->draw(*frame, view);
            forward->draw(*frame, view);
            REQUIRE(target->endFrame(std::move(*frame)));
            REQUIRE(device->readbackTexture(target->colorTexture(), 0U, bytesA));
            forward->destroyMaterial(material);
        }

        std::vector<std::byte> bytesB(READBACK_BYTES, std::byte{0xAB});
        {
            std::optional<engine::render::RenderTarget> target = engine::render::RenderTarget::create(
                *device, {EXTENT, EXTENT},
                {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
            REQUIRE(target.has_value());
            std::optional<engine::scene_render::SceneRenderer> scene =
                engine::scene_render::SceneRenderer::create(*device, vfs, target->colorFormat(), target->depthFormat());
            REQUIRE(scene.has_value());
            std::optional<engine::render::Frame> frame = target->beginFrame(CLEAR);
            REQUIRE(frame.has_value());
            scene->render(world, *frame, &camera);
            REQUIRE(target->endFrame(std::move(*frame)));
            REQUIRE(device->readbackTexture(target->colorTexture(), 0U, bytesB));
        }

        checkBytesEqual(bytesA, bytesB);
        perAngle.at(i) = std::move(bytesA);
    }

    // Anti-vacuity: the two angles really are two different pictures, so "both agree" is a statement
    // about the association rather than about one pose rendered twice.
    CHECK(differingTexels(perAngle.at(0), perAngle.at(1)) > 0U);
}

#else

TEST_CASE("editor: with no cooked shaders this TU registers no GPU parity case (task E.2.4, PX)") {
    // ASSERTED, NEVER SILENTLY ABSENT -- the 3.4.2 near-miss rule. In a -DAERO_SHADER_TOOLS=OFF build
    // there is no cooked sky.vert / scene.frag, so neither side of the A/B can be built at all and the
    // four cases above do not exist. What still holds, and is worth stating rather than skipping, is
    // that MaterialPreview latches Unavailable in its CONSTRUCTOR in this configuration, so the
    // preview never renders and the parity claim is vacuously safe rather than untested.
    //
    // The rig itself is PURE and is fully covered here by PV1-PV12 on aero_editor_shell_test, which is
    // ungated and runs identically in both configurations.
    MESSAGE("AERO_SHADER_TOOLS=OFF: no cooked shaders, so the preview/viewport byte A/B cannot run");
    CHECK(engine::editor::DEFAULT_MATERIAL_PREVIEW_RIG == engine::editor::MaterialPreviewRig{});
}

#endif

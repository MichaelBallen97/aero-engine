// Aero Engine — task 3.6.2's deliverable: directional shadows made visible, and measurable.
//
// What it draws: a 30 x 30 ground plane under six casters, lit by ONE directional light whose
// ELEVATION sweeps 10-80 degrees on a 20-second triangle wave at a fixed azimuth of 30 degrees. The
// camera is FIXED on purpose — the sun is the only thing moving, so shadow length and direction are
// continuously checkable against it, and a shadow that lags, mirrors or inverts is obvious.
//
// Three instances are marked for the validation pass and are the only ones that are not grey:
//   * a RED cube is MIRRORED (scale -1 on X), with a GREEN unmirrored twin beside it. Both carry a
//     doubleSided material, so a winding flip cannot fake a disappearance. This is transformAabb's
//     absolute value under test again, in the LIGHT frustum this time.
//   * an ORANGE two-bone strip, cooked IN MEMORY at startup and waving on a 3-second cycle, is the
//     ONLY path into the skinned depth pipeline — draw()'s built-in primitive arm ignores a palette
//     entirely, so a marked cube would witness the `!palette.empty()` predicate and nothing about
//     skinning. Cooked rather than committed: 3.3.3 makes the cook deterministic cross-lane, so it
//     is as stable as a golden and adds no fixture to the tree.
//
// Two things the scene carries that exist for one validation row each, and would otherwise look
// like decoration:
//   * ONE point light near the casters. Row 5 asks that the shadowed region is still lit by it and
//     by ambient — darker, never black — which is what says the shadow term multiplies ONLY the
//     directional contribution.
//   * a PROCEDURAL NORMAL MAP on the ground plane. Row 6 compares the geometric normal against the
//     normal-mapped one in the shadow lookup, and with the renderer's built-in 1x1 FLAT normal
//     default that comparison is a no-op: nxy == (0, 0) leaves N bit-identical to geoN. The ripple
//     is what makes "the shadow wobbles with the bump pattern" visible at all.
//
// Arguments, in any order, each a SEPARATE shell word: `--no-shadows` (the A/B twin),
// `--resolution N` (the map size), `--distance D` (the light's shadowDistance) and `--elevation DEG`
// (freeze the sun instead of sweeping). There is no positional argument here, unlike
// phase-3-culling — the first non-flag argument is ignored with a WARN.
//
// CI builds this on three OSes (compile-proof only — no display there); run it locally for the
// visual pass and record the result in editor/validation/3.6.2-directional-shadow-map.md. Requires
// AERO_SHADER_TOOLS (the cooked scene.* and shadow.*); without it this compiles a stub main that
// logs and returns 1, the phase-0-cube precedent.
#include <aero/core/log.hpp>
#include <aero/core/time.hpp>
#include <aero/core/vfs.hpp>
#include <aero/platform/platform.hpp>
#include <aero/render/renderer.hpp>
#include <aero/rhi/rhi.hpp>

#ifdef AERO_PHASE3_SHADOWS_ENABLED
    #include <aero/render/render.hpp>
#endif

#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <string>

#ifdef AERO_PHASE3_SHADOWS_ENABLED

    #include <aero/assets/cooked_mesh.hpp>
    #include <aero/assets/mesh_cook.hpp>

    #include <algorithm>
    #include <array>
    #include <cstddef>
    #include <cstdint>
    #include <cstdlib>
    #include <span>
    #include <string_view>
    #include <vector>

namespace {

using namespace engine;  // sample TU (not a header) — docs/04 forbids this only in headers

constexpr float SUN_PERIOD_SECONDS = 20.0F;     // one full 10 -> 80 -> 10 sweep
constexpr float SUN_MIN_ELEVATION_DEG = 10.0F;  // long shadows: cot(10 deg) == 5.67 x the height
constexpr float SUN_MAX_ELEVATION_DEG = 80.0F;  // short shadows: cot(80 deg) == 0.18 x
constexpr float SUN_AZIMUTH_DEG = 30.0F;        // diagonal, so the mirrored pair's shadows separate
constexpr std::uint32_t DEFAULT_RESOLUTION = 2048;
constexpr float DEFAULT_SHADOW_DISTANCE = 40.0F;
constexpr float GROUND_HALF_EXTENT = 15.0F;  // a 30 x 30 plane
constexpr float FOV_Y_DEGREES = 50.0F;
constexpr float Z_NEAR = 0.1F;
constexpr float Z_FAR = 200.0F;
constexpr float RIG_CYCLE_SECONDS = 3.0F;
constexpr float RIG_SWING_DEGREES = 40.0F;
constexpr std::uint32_t NORMAL_MAP_SIZE = 64;
constexpr double TITLE_UPDATE_SECONDS = 0.25;

constexpr Vec3 EYE{0.0F, 6.0F, 14.0F};
constexpr Vec3 LOOK_AT{0.0F, 1.0F, 0.0F};

// The sun's TRAVEL direction from an elevation and an azimuth. Elevation is measured up from the
// horizon, azimuth clockwise from -Z, so at azimuth 30 the shadows fall toward -X and -Z.
[[nodiscard]] Vec3 sunDirection(float elevationRadians, float azimuthRadians) {
    const float c = std::cos(elevationRadians);
    return Vec3{-c * std::sin(azimuthRadians), -std::sin(elevationRadians), -c * std::cos(azimuthRadians)};
}

// The normal matrix scene_render computes for every instance, spelled the same way rather than
// assumed to be the identity — the mirrored instance is exactly the case where it is not.
[[nodiscard]] Mat4 normalMatrixOf(const Mat4& model) {
    const Mat3 m = transpose(inverse(toMat3(model)));
    return Mat4{std::array<Vec4, 4>{Vec4{m.columns[0].x, m.columns[0].y, m.columns[0].z, 0.0F},
                                    Vec4{m.columns[1].x, m.columns[1].y, m.columns[1].z, 0.0F},
                                    Vec4{m.columns[2].x, m.columns[2].y, m.columns[2].z, 0.0F},
                                    Vec4{0.0F, 0.0F, 0.0F, 1.0F}}};
}

struct Options {
    bool shadows = true;
    std::uint32_t resolution = DEFAULT_RESOLUTION;
    float distance = DEFAULT_SHADOW_DISTANCE;
    float frozenElevation = -1.0F;  // < 0 means "sweep"
};

[[nodiscard]] Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        const auto takeValue = [&](float& out) {
            if (i + 1 < argc) {
                out = std::strtof(argv[i + 1], nullptr);
                ++i;
            } else {
                AERO_LOG_WARN("phase-3-shadows: '{}' needs a value; ignored", arg);
            }
        };
        if (arg == "--no-shadows") {
            options.shadows = false;
        } else if (arg == "--resolution") {
            float value = 0.0F;
            takeValue(value);
            options.resolution = static_cast<std::uint32_t>(std::max(0.0F, value));
        } else if (arg == "--distance") {
            takeValue(options.distance);
        } else if (arg == "--elevation") {
            takeValue(options.frozenElevation);
        } else {
            AERO_LOG_WARN("phase-3-shadows: ignoring unrecognised argument '{}'", arg);
        }
    }
    return options;
}

// A two-bone strip: eight vertices in four rings up +Y, every vertex weighted between joint 0 (the
// base) and joint 1 (the tip) by its height, twelve triangles. Cooked in memory so the sample reads
// no artifact and commits no fixture.
struct RigGeometry {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<std::array<std::uint16_t, 4>> joints;
    std::vector<Vec4> weights;
    std::vector<std::uint32_t> indices;
};

[[nodiscard]] RigGeometry makeRigGeometry() {
    RigGeometry g;
    constexpr int RINGS = 4;
    constexpr float HEIGHT = 3.0F;
    constexpr float HALF_WIDTH = 0.25F;
    for (int r = 0; r < RINGS; ++r) {
        const float t = static_cast<float>(r) / static_cast<float>(RINGS - 1);
        const float y = t * HEIGHT;
        for (int side = 0; side < 2; ++side) {
            const float x = side == 0 ? -HALF_WIDTH : HALF_WIDTH;
            g.positions.push_back(Vec3{x, y, 0.0F});
            g.normals.push_back(Vec3{0.0F, 0.0F, 1.0F});
            // Joint 0 holds the base, joint 1 the tip; the blend IS the height fraction, so the
            // strip bends smoothly rather than hinging at one ring.
            g.joints.push_back(std::array<std::uint16_t, 4>{0, 1, 0, 0});
            g.weights.push_back(Vec4{1.0F - t, t, 0.0F, 0.0F});
        }
    }
    for (int r = 0; r + 1 < RINGS; ++r) {
        const auto base = static_cast<std::uint32_t>(r * 2);
        for (const std::uint32_t offset : {0U, 1U, 2U, 2U, 1U, 3U}) {
            g.indices.push_back(base + offset);
        }
    }
    return g;
}

// A 64x64 RGBA8 ripple, LINEAR (never sRGB): a tangent-space normal map whose xy wobble with two
// sine terms. Row 6 is the only reason it exists — see the header.
[[nodiscard]] std::vector<std::uint8_t> makeNormalMapTexels() {
    std::vector<std::uint8_t> texels(static_cast<std::size_t>(NORMAL_MAP_SIZE) * NORMAL_MAP_SIZE * 4U);
    for (std::uint32_t y = 0; y < NORMAL_MAP_SIZE; ++y) {
        for (std::uint32_t x = 0; x < NORMAL_MAP_SIZE; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(NORMAL_MAP_SIZE);
            const float v = static_cast<float>(y) / static_cast<float>(NORMAL_MAP_SIZE);
            const float nx = 0.5F * std::sin(u * TWO_PI * 3.0F);
            const float ny = 0.5F * std::sin(v * TWO_PI * 3.0F);
            const Vec3 n = normalize(Vec3{nx, ny, 1.0F});
            const std::size_t i = ((static_cast<std::size_t>(y) * NORMAL_MAP_SIZE) + x) * 4U;
            texels[i + 0] = static_cast<std::uint8_t>(std::lround((n.x * 0.5F + 0.5F) * 255.0F));
            texels[i + 1] = static_cast<std::uint8_t>(std::lround((n.y * 0.5F + 0.5F) * 255.0F));
            texels[i + 2] = static_cast<std::uint8_t>(std::lround((n.z * 0.5F + 0.5F) * 255.0F));
            texels[i + 3] = 255;
        }
    }
    return texels;
}

int runSample(int argc, char** argv) {
    const Options options = parseOptions(argc, argv);

    platform::Context ctx;  // real driver (headless=false) — needed for GPU
    if (!ctx.valid()) {
        AERO_LOG_CRITICAL("platform init failed");
        return 1;
    }
    std::optional<platform::Window> window =
        ctx.createWindow({.title = "Aero — Phase 3 Shadows", .width = 1280, .height = 720});
    if (!window) {
        return 1;
    }
    std::optional<rhi::Device> device = rhi::Device::create();
    if (!device) {
        AERO_LOG_CRITICAL("no GPU device");
        return 1;
    }
    std::optional<render::Renderer> renderer = render::Renderer::create(*device, *window, {.depth = true});
    if (!renderer) {
        AERO_LOG_CRITICAL("renderer creation failed");
        return 1;
    }

    VirtualFileSystem vfs;
    vfs.mount(std::make_unique<DirectoryBackend>(AERO_SHADERS_DIR));
    std::optional<render::ForwardRenderer> forward =
        render::ForwardRenderer::create(*device, vfs,
                                        {.colorFormat = renderer->colorFormat(),
                                         .depthFormat = renderer->depthFormat(),
                                         .shadowMapResolution = options.resolution});
    if (!forward) {
        AERO_LOG_CRITICAL("forward renderer creation failed");
        return 1;
    }

    // --- the ground plane's procedural normal map. Renderer-EXTERNAL: the sample creates it, the
    // material BORROWS it (material.hpp's own rule), and the sample destroys it below.
    const std::vector<std::uint8_t> normalTexels = makeNormalMapTexels();
    const rhi::TextureHandle normalMap = device->createTexture({.format = rhi::TextureFormat::RGBA8Unorm,
                                                                .usage = rhi::TextureUsage::Sampler,
                                                                .width = NORMAL_MAP_SIZE,
                                                                .height = NORMAL_MAP_SIZE});
    if (!normalMap.valid() || !device->uploadTexture(normalMap, 0, std::as_bytes(std::span{normalTexels}))) {
        AERO_LOG_CRITICAL("phase-3-shadows: the procedural normal map could not be created");
        return 1;
    }

    const render::MaterialHandle solid = forward->createMaterial(
        {.baseColorFactor = Vec4{1.0F, 1.0F, 1.0F, 1.0F}, .metallicFactor = 0.0F, .roughnessFactor = 0.6F}, {});
    const render::MaterialHandle twoSided = forward->createMaterial({.baseColorFactor = Vec4{1.0F, 1.0F, 1.0F, 1.0F},
                                                                     .metallicFactor = 0.0F,
                                                                     .roughnessFactor = 0.6F,
                                                                     .doubleSided = true},
                                                                    {});
    // Repeat on both axes so the 64x64 ripple tiles across the 30 x 30 plane rather than stretching.
    const render::MaterialHandle ground = forward->createMaterial(
        {.baseColorFactor = Vec4{1.0F, 1.0F, 1.0F, 1.0F}, .metallicFactor = 0.0F, .roughnessFactor = 0.85F},
        {.normal = {.texture = normalMap,
                    .sampler = {.addressU = rhi::AddressMode::Repeat, .addressV = rhi::AddressMode::Repeat}}});

    // --- the rig, cooked IN MEMORY. `bytes` must OUTLIVE the CookedMesh parsed from it (the
    // retained-span contract), so it lives for the whole of runSample.
    const RigGeometry rig = makeRigGeometry();
    assets::MeshCookPrimitive rigPrimitive;
    rigPrimitive.positions = rig.positions;
    rigPrimitive.normals = rig.normals;
    rigPrimitive.joints = rig.joints;
    rigPrimitive.weights = rig.weights;
    rigPrimitive.indices = rig.indices;
    const std::array<assets::MeshCookPrimitive, 1> rigPrimitives{rigPrimitive};
    assets::MeshCookResult cooked = assets::cookMesh({.sourceGuid = {}, .primitives = rigPrimitives});
    if (cooked.status != assets::MeshCookStatus::Ok) {
        AERO_LOG_CRITICAL("phase-3-shadows: the in-memory rig cook failed");
        return 1;
    }
    const std::vector<std::byte> rigBytes = std::move(cooked.bytes);
    const assets::CookedMeshParseResult rigParse = assets::parseCookedMesh(rigBytes);
    if (rigParse.status != assets::CookedMeshStatus::Ok) {
        AERO_LOG_CRITICAL("phase-3-shadows: the in-memory rig parse failed");
        return 1;
    }
    const render::MeshHandle rigMesh = forward->createMesh(rigParse.mesh);
    if (!rigMesh.valid()) {
        AERO_LOG_CRITICAL("phase-3-shadows: the rig mesh could not be registered");
        return 1;
    }

    // --- the six instances, built ONCE. Only mvp and the rig's palette change per frame.
    enum Slot : std::uint8_t { GROUND = 0, CUBE_L, BALL, MIRROR, TWIN, RIG, SLOT_COUNT };
    std::vector<render::MeshInstance> instances(SLOT_COUNT);

    const auto place = [&](std::size_t slot, render::PrimitiveId primitive, const Mat4& model, Vec3 color,
                           render::MaterialHandle material) {
        render::MeshInstance& instance = instances[slot];
        instance.primitive = primitive;
        instance.model = model;
        instance.normalMatrix = normalMatrixOf(model);
        instance.color = color;
        instance.material = material;
    };
    place(GROUND, render::PrimitiveId::Plane,
          compose({.scale = Vec3{GROUND_HALF_EXTENT * 2.0F, 1.0F, GROUND_HALF_EXTENT * 2.0F}}),
          Vec3{0.72F, 0.72F, 0.74F}, ground);
    place(CUBE_L, render::PrimitiveId::Cube, compose({.translation = Vec3{-3.0F, 1.0F, 0.0F}}),
          Vec3{0.62F, 0.64F, 0.66F}, solid);
    place(BALL, render::PrimitiveId::Sphere,
          compose({.translation = Vec3{0.0F, 1.2F, 0.0F}, .scale = Vec3{1.6F, 1.6F, 1.6F}}), Vec3{0.62F, 0.64F, 0.66F},
          solid);
    // MIRRORED: scale -1 on X, beside its unmirrored twin. doubleSided on both, so a winding flip
    // cannot be mistaken for a disappearance.
    place(MIRROR, render::PrimitiveId::Cube,
          compose({.translation = Vec3{3.0F, 1.0F, 0.0F}, .scale = Vec3{-1.0F, 1.0F, 1.0F}}), Vec3{0.85F, 0.15F, 0.15F},
          twoSided);
    place(TWIN, render::PrimitiveId::Cube, compose({.translation = Vec3{5.0F, 1.0F, 0.0F}}), Vec3{0.15F, 0.75F, 0.25F},
          twoSided);

    const Mat4 rigModel = compose({.translation = Vec3{0.0F, 1.0F, 4.0F}});
    instances[RIG].mesh = rigMesh;
    instances[RIG].submesh = 0;
    instances[RIG].model = rigModel;
    instances[RIG].normalMatrix = normalMatrixOf(rigModel);
    instances[RIG].color = Vec3{0.95F, 0.55F, 0.15F};
    instances[RIG].material = twoSided;
    std::array<Mat4, 2> rigPalette{Mat4::identity(), Mat4::identity()};

    // ONE point light, warm and moderate, near the casters. Row 5's only reason to exist: the
    // shadowed region must stay lit by it, which is what says the shadow term multiplies ONLY the
    // directional contribution.
    const std::array<render::PointLightData, 1> pointLights{render::PointLightData{
        .position = Vec3{-2.0F, 2.5F, 3.0F}, .color = Vec3{1.0F, 0.85F, 0.65F}, .intensity = 8.0F, .range = 14.0F}};

    AERO_LOG_INFO("phase-3-shadows: shadows {}, map requested {} -> allocated {}, shadowDistance {:.1f}",
                  options.shadows ? "ON" : "OFF (--no-shadows)", options.resolution, forward->shadowMapResolution(),
                  options.distance);
    if (options.frozenElevation >= 0.0F) {
        AERO_LOG_INFO("phase-3-shadows: sun FROZEN at elevation {:.1f} deg, azimuth {:.1f} deg",
                      options.frozenElevation, SUN_AZIMUTH_DEG);
    } else {
        AERO_LOG_INFO("phase-3-shadows: sun elevation sweeps {:.0f}-{:.0f} deg every {:.0f} s at azimuth {:.0f} deg",
                      SUN_MIN_ELEVATION_DEG, SUN_MAX_ELEVATION_DEG, SUN_PERIOD_SECONDS, SUN_AZIMUTH_DEG);
    }
    AERO_LOG_INFO("phase-3-shadows: RED cube = mirrored, GREEN = its twin, ORANGE = the skinned rig");

    FrameClock clock;
    double lastTitleAt = 0.0;
    bool running = true;
    while (running) {
        ctx.newFrame();
        platform::Event ev;
        while (ctx.pollEvent(ev)) {
            if (ev.type == platform::EventType::Quit || ev.type == platform::EventType::WindowClose) {
                running = false;
            }
        }
        if (ctx.input().keyDown(platform::Key::Escape)) {
            running = false;
        }
        clock.tick();
        const auto seconds = static_cast<float>(clock.totalSeconds());

        // A TRIANGLE wave, so the sun turns around at both ends rather than snapping back.
        float elevationDeg = options.frozenElevation;
        if (elevationDeg < 0.0F) {
            const float phase = std::fmod(seconds / SUN_PERIOD_SECONDS, 1.0F);
            const float ramp = phase < 0.5F ? phase * 2.0F : (1.0F - phase) * 2.0F;
            elevationDeg = SUN_MIN_ELEVATION_DEG + ((SUN_MAX_ELEVATION_DEG - SUN_MIN_ELEVATION_DEG) * ramp);
        }
        const Vec3 sun = sunDirection(radians(elevationDeg), radians(SUN_AZIMUTH_DEG));

        // The rig's palette: joint 0 identity, joint 1 a rotation about Z sweeping +-40 degrees on a
        // 3-second cycle about the strip's mid-height. The tip swings through roughly 1.4 world
        // units — well inside the fit, and large enough that a shadow which did NOT follow the
        // animation would be obvious.
        const float swing = std::sin((seconds / RIG_CYCLE_SECONDS) * TWO_PI) * radians(RIG_SWING_DEGREES);
        const Vec3 hinge{0.0F, 1.5F, 0.0F};
        rigPalette[0] = Mat4::identity();
        // Rotate ABOUT the hinge, not about the origin: translate the hinge to the origin, turn,
        // translate back. Spelled as three composes so the order is readable rather than implied.
        rigPalette[1] = translation(hinge) * compose({.rotation = fromAxisAngle(Vec3{0.0F, 0.0F, 1.0F}, swing)}) *
                        translation(hinge * -1.0F);
        instances[RIG].palette = std::span<const Mat4>{rigPalette};

        const rhi::Color sky{0.05F, 0.06F, 0.09F, 1.0F};
        if (std::optional<render::Frame> frame = renderer->beginFrame(sky)) {
            render::Frame& openFrame = *frame;
            const rhi::Extent2D extent = openFrame.extent();
            const float aspect =
                extent.height == 0 ? 1.0F : static_cast<float>(extent.width) / static_cast<float>(extent.height);

            render::RenderView view;
            view.camera = {lookAt(EYE, LOOK_AT, Vec3{0.0F, 1.0F, 0.0F}),
                           perspective(radians(FOV_Y_DEGREES), aspect, Z_NEAR, Z_FAR), EYE};
            view.directional = {.direction = sun,
                                .color = Vec3{1.0F, 0.97F, 0.92F},
                                .intensity = 3.0F,
                                .castsShadows = true,
                                .shadowDistance = options.distance};
            view.points = pointLights;
            view.ambient = Vec3{0.06F, 0.07F, 0.09F};
            view.shadowsEnabled = options.shadows;

            const Mat4 viewProj = view.camera.proj * view.camera.view;
            for (render::MeshInstance& instance : instances) {
                instance.mvp = viewProj * instance.model;
            }
            view.instances = instances;

            // BOTH are CPU RECORD times, and the page says so: nothing exposes the depth pass's GPU
            // cost separately. `--no-shadows` is what isolates it — run the same scene both ways.
            const double shadowStartedAt = monotonicSeconds();
            view.shadow = forward->renderShadowMap(view);
            const double shadowMs = (monotonicSeconds() - shadowStartedAt) * 1000.0;
            const double drawStartedAt = monotonicSeconds();
            forward->draw(openFrame, view);
            const double drawMs = (monotonicSeconds() - drawStartedAt) * 1000.0;

            if (renderer->endFrame(std::move(openFrame))) {
                AERO_LOG_INFO(
                    "elev {:5.1f}  sun ({:6.3f},{:6.3f},{:6.3f})  shadow drawn {} / culled {}  "
                    "cam drawn {} / culled {}  shadowMs {:.3f}  drawMs {:.3f}  map {}  fps {:.1f}",
                    elevationDeg, sun.x, sun.y, sun.z, forward->lastFrameShadowDrawn(),
                    forward->lastFrameShadowCulled(), forward->lastFrameDrawn(), forward->lastFrameCulled(), shadowMs,
                    drawMs, forward->shadowMapResolution(), clock.fps());
                const double now = monotonicSeconds();
                if (now - lastTitleAt >= TITLE_UPDATE_SECONDS) {
                    window->setTitle("Aero — Phase 3 Shadows · " + std::to_string(std::lround(clock.fps())) +
                                     " fps · elev " + std::to_string(std::lround(elevationDeg)));
                    lastTitleAt = now;
                }
            }
        }
    }

    forward->destroyMesh(rigMesh);
    device->destroyTexture(normalMap);  // renderer-EXTERNAL: the sample owns it, the material borrowed it
    AERO_LOG_INFO("phase-3-shadows: the shadow-fit warning {}",
                  forward->hasWarnedShadowFit() ? "FIRED at least once" : "never fired");
    AERO_LOG_INFO("phase-3-shadows: the shadow map allocated {0}x{0}", forward->shadowMapResolution());
    AERO_LOG_INFO("closing after {} frames, {:.1f}s", clock.frameCount(), clock.totalSeconds());
    AERO_LOG_INFO("record this run in editor/validation/3.6.2-directional-shadow-map.md (this OS)");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return runSample(argc, argv);
    } catch (const std::exception& e) {
        AERO_LOG_CRITICAL("phase-3-shadows: unexpected exception: {}", e.what());
        return 1;
    } catch (...) {
        AERO_LOG_CRITICAL("phase-3-shadows: unexpected exception");
        return 1;
    }
}

#else  // AERO_PHASE3_SHADOWS_ENABLED

int main() {
    AERO_LOG_CRITICAL("phase-3-shadows needs AERO_SHADER_TOOLS");
    return 1;
}

#endif  // AERO_PHASE3_SHADOWS_ENABLED

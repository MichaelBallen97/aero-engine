// Aero Engine — task 3.6.1's deliverable: frustum culling made visible, and measurable.
//
// What it draws: an N x N x N grid of built-in primitives (cube/sphere/plane, cycling by linear
// index) around the origin, seen from a FIXED eye just outside the grid whose LOOK DIRECTION yaws a
// full 360 degrees every 12 seconds. So the drawn fraction sweeps from its maximum down to zero and
// back on every cycle, and every instance crosses a screen edge twice per cycle — which is the whole
// point: an edge is where a culling bug shows, and a static camera would never visit one.
//
// Every frame it prints the yaw, the drawn/culled split, and the wall time of the
// ForwardRenderer::draw call itself. That last number is CPU RECORD time, not "cull time": the cull
// runs inside draw and nothing exposes it separately. `--no-cull` is what isolates the cull term —
// run the same grid both ways and compare the record times at the same yaw.
//
// Two instances are marked for the validation pass and are the only ones that are not grey:
//   * a RED cube at the near-bottom-left grid corner is MIRRORED (scale -1 on X), with a GREEN
//     unmirrored twin beside it. Both carry a doubleSided material, so the mirrored winding flip
//     cannot fake a disappearance. A mirror is the fixture that separates a correct conservative box
//     transform from one that drops the absolute value and produces an inside-out box, which the
//     cull then reads as "nothing to draw".
//   * a BLUE instance at the far top-right corner carries a one-entry identity PALETTE. An instance
//     with a palette is exempt from culling, so with the camera facing fully away it is the only
//     thing still drawn. It witnesses the `!palette.empty()` predicate, NOT skinning — the built-in
//     primitive path ignores a palette entirely.
//
// Arguments, in any order: `--no-cull` turns culling off for the whole run; the first non-flag
// argument overrides N (clamped to [2, 32]; 32^3 = 32768 instances is the stress row).
//
// CI builds this on three OSes (compile-proof only — no display there); run it locally for the
// visual pass and record the result in editor/validation/3.6.1-frustum-culling.md. Requires
// AERO_SHADER_TOOLS (the cooked scene.{vert,frag}); without it this compiles a stub main that logs
// and returns 1, the phase-0-cube precedent.
#include <aero/core/log.hpp>
#include <aero/core/time.hpp>
#include <aero/core/vfs.hpp>
#include <aero/platform/platform.hpp>
#include <aero/render/renderer.hpp>
#include <aero/rhi/rhi.hpp>

#ifdef AERO_PHASE3_CULLING_ENABLED
    #include <aero/render/render.hpp>
#endif

#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <string>

#ifdef AERO_PHASE3_CULLING_ENABLED

    #include <algorithm>  // std::clamp — the N argument
    #include <array>
    #include <cstddef>
    #include <cstdlib>  // std::strtol — the N argument
    #include <span>
    #include <string_view>
    #include <vector>

namespace {

using namespace engine;  // sample TU (not a header) — docs/04 forbids this only in headers

constexpr int DEFAULT_N = 10;                 // 1000 instances — the default row
constexpr int MIN_N = 2;                      // the smallest grid that still has a corner to mark
constexpr int MAX_N = 32;                     // 32^3 = 32768 — the stress row
constexpr float SPACING = 2.0F;               // world units between neighbours (primitives are ~1 unit)
constexpr float YAW_PERIOD_SECONDS = 12.0F;   // one full turn; slow enough to read a screen edge
constexpr float LOOK_PITCH = -0.15F;          // a slight downward tilt so the grid is not edge-on
constexpr float EYE_HEIGHT_FACTOR = 0.6F;     // x the grid's half-extent
constexpr float EYE_DISTANCE_FACTOR = 1.35F;  // x the grid's half-extent — just OUTSIDE the grid
constexpr float FOV_Y_DEGREES = 60.0F;
constexpr float Z_NEAR = 0.1F;
constexpr float Z_FAR = 200.0F;  // the N=32 grid's far corner sits at ~84 units; this clears it
constexpr double TITLE_UPDATE_SECONDS = 0.25;

// The palette the exempt instance borrows. One identity entry: the built-in primitive path ignores
// a palette entirely, so the CONTENTS never matter — only that the span is non-empty.
constexpr std::array<Mat4, 1> IDENTITY_PALETTE{Mat4::identity()};

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
    int gridSize = DEFAULT_N;
    bool cull = true;
};

[[nodiscard]] Options parseOptions(int argc, char** argv) {
    Options options;
    bool tookSize = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--no-cull") {
            options.cull = false;
        } else if (!tookSize && !arg.starts_with("--")) {
            options.gridSize = std::clamp(static_cast<int>(std::strtol(argv[i], nullptr, 10)), MIN_N, MAX_N);
            tookSize = true;
        } else {
            AERO_LOG_WARN("phase-3-culling: ignoring unrecognised argument '{}'", arg);
        }
    }
    return options;
}

int runSample(int argc, char** argv) {
    const Options options = parseOptions(argc, argv);
    const int n = options.gridSize;
    const auto instanceCount = static_cast<std::size_t>(n) * static_cast<std::size_t>(n) * static_cast<std::size_t>(n);
    // The grid spans [-half, +half] on every axis.
    const float half = static_cast<float>(n - 1) * SPACING * 0.5F;

    platform::Context ctx;  // real driver (headless=false) — needed for GPU
    if (!ctx.valid()) {
        AERO_LOG_CRITICAL("platform init failed");
        return 1;
    }

    std::optional<platform::Window> window =
        ctx.createWindow({.title = "Aero — Phase 3 Culling", .width = 1280, .height = 720});
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

    std::optional<render::ForwardRenderer> forward = render::ForwardRenderer::create(
        *device, vfs, {.colorFormat = renderer->colorFormat(), .depthFormat = renderer->depthFormat()});
    if (!forward) {
        AERO_LOG_CRITICAL("forward renderer creation failed");
        return 1;
    }

    // Two materials only. The per-instance `color` tint multiplies baseColorFactor in the shader, so
    // red/green/blue/grey need no material of their own — but doubleSided IS a material property,
    // and the mirrored pair needs it so a winding flip cannot be mistaken for a disappearance.
    const render::MaterialHandle solid = forward->createMaterial(
        {.baseColorFactor = Vec4{1.0F, 1.0F, 1.0F, 1.0F}, .metallicFactor = 0.0F, .roughnessFactor = 0.55F}, {});
    const render::MaterialHandle twoSided = forward->createMaterial({.baseColorFactor = Vec4{1.0F, 1.0F, 1.0F, 1.0F},
                                                                     .metallicFactor = 0.0F,
                                                                     .roughnessFactor = 0.55F,
                                                                     .doubleSided = true},
                                                                    {});

    // --- the grid, built once. Only mvp changes per frame.
    const auto linearIndex = [n](int ix, int iy, int iz) {
        // Widened BEFORE the arithmetic, not after: a cast applied to an int product is either
        // pointless or too late (clang-tidy's bugprone-misplaced-widening-cast says so).
        const auto side = static_cast<std::size_t>(n);
        return (static_cast<std::size_t>(iz) * side * side) + (static_cast<std::size_t>(iy) * side) +
               static_cast<std::size_t>(ix);
    };
    const std::size_t mirroredIndex = linearIndex(0, 0, n - 1);     // near-bottom-left corner
    const std::size_t twinIndex = linearIndex(1, 0, n - 1);         // its +X neighbour
    const std::size_t paletteIndex = linearIndex(n - 1, n - 1, 0);  // the far top-right corner

    std::vector<render::MeshInstance> instances(instanceCount);
    for (int iz = 0; iz < n; ++iz) {
        for (int iy = 0; iy < n; ++iy) {
            for (int ix = 0; ix < n; ++ix) {
                const std::size_t i = linearIndex(ix, iy, iz);
                const Vec3 position{(static_cast<float>(ix) * SPACING) - half,
                                    (static_cast<float>(iy) * SPACING) - half,
                                    (static_cast<float>(iz) * SPACING) - half};
                render::MeshInstance& instance = instances[i];
                instance.primitive = static_cast<render::PrimitiveId>(i % 3);
                instance.model = compose({.translation = position});
                instance.color = Vec3{0.62F, 0.64F, 0.66F};
                instance.material = solid;
                if (i == mirroredIndex) {
                    // MIRRORED: scale -1 on X. Its world box is the mirror image of the local one,
                    // and the conservative transform must keep it valid rather than inside out.
                    instance.primitive = render::PrimitiveId::Cube;
                    instance.model = compose({.translation = position, .scale = Vec3{-1.0F, 1.0F, 1.0F}});
                    instance.color = Vec3{0.85F, 0.15F, 0.15F};
                    instance.material = twoSided;
                } else if (i == twinIndex) {
                    instance.primitive = render::PrimitiveId::Cube;
                    instance.color = Vec3{0.15F, 0.75F, 0.25F};
                    instance.material = twoSided;
                } else if (i == paletteIndex) {
                    // EXEMPT: a non-empty palette means "this instance's vertices may move under a
                    // matrix this renderer never sees", so its bind-pose box bounds nothing and it
                    // is never culled. Facing fully away, this is the only instance still drawn.
                    instance.primitive = render::PrimitiveId::Cube;
                    instance.color = Vec3{0.20F, 0.35F, 0.95F};
                    instance.palette = std::span<const Mat4>{IDENTITY_PALETTE};
                }
                instance.normalMatrix = normalMatrixOf(instance.model);
            }
        }
    }

    // FIXED eye, just outside the grid on +Z. Only the look DIRECTION turns, which is what makes the
    // drawn count sweep the whole range without ever changing what is where.
    const Vec3 eye{0.0F, half * EYE_HEIGHT_FACTOR, half * EYE_DISTANCE_FACTOR};

    AERO_LOG_INFO("phase-3-culling: {0}x{0}x{0} = {1} instances, spacing {2}, culling {3}", n, instanceCount, SPACING,
                  options.cull ? "ON" : "OFF (--no-cull)");
    AERO_LOG_INFO("phase-3-culling: eye ({:.2f}, {:.2f}, {:.2f}); the look direction turns 360 deg every {} s", eye.x,
                  eye.y, eye.z, YAW_PERIOD_SECONDS);
    AERO_LOG_INFO("phase-3-culling: RED cube = mirrored, GREEN = its unmirrored twin, BLUE = palette-exempt");

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
        const float yaw = std::fmod(seconds / YAW_PERIOD_SECONDS, 1.0F) * TWO_PI;
        const Vec3 forwardDir{std::sin(yaw), LOOK_PITCH, -std::cos(yaw)};

        const rhi::Color sky{0.02F, 0.02F, 0.03F, 1.0F};
        if (std::optional<render::Frame> frame = renderer->beginFrame(sky)) {
            render::Frame& openFrame = *frame;
            const rhi::Extent2D extent = openFrame.extent();
            const float aspect =
                extent.height == 0 ? 1.0F : static_cast<float>(extent.width) / static_cast<float>(extent.height);

            render::RenderView view;
            view.camera = {lookAt(eye, eye + forwardDir, Vec3{0.0F, 1.0F, 0.0F}),
                           perspective(radians(FOV_Y_DEGREES), aspect, Z_NEAR, Z_FAR), eye};
            view.directional = {
                .direction = normalize(Vec3{-0.5F, -1.0F, -0.3F}), .color = Vec3::one(), .intensity = 3.0F};
            // task E.2.1: Flat at the sample's own constant, intensity 1 -- byte-identical shade.
            view.environment = {.ambientMode = render::AmbientMode::Flat,
                                .ambientColor = Vec3{0.05F, 0.05F, 0.06F},
                                .ambientIntensity = 1.0F};
            view.cullingEnabled = options.cull;

            // THE CONTRACT the cull rests on, honoured explicitly: mvp == viewProj * model for every
            // instance, with the camera of THIS view.
            const Mat4 viewProj = view.camera.proj * view.camera.view;
            for (render::MeshInstance& instance : instances) {
                instance.mvp = viewProj * instance.model;
            }
            view.instances = instances;

            // CPU RECORD time, which is what `--no-cull` compares against. The cull itself is not
            // separately observable — it runs inside this call.
            const double drawStartedAt = monotonicSeconds();
            forward->draw(openFrame, view);
            const double recordMs = (monotonicSeconds() - drawStartedAt) * 1000.0;

            if (renderer->endFrame(std::move(openFrame))) {
                const std::size_t drawn = forward->lastFrameDrawn();
                const std::size_t culled = forward->lastFrameCulled();
                const double culledPercent =
                    instanceCount == 0 ? 0.0
                                       : (static_cast<double>(culled) / static_cast<double>(instanceCount)) * 100.0;
                AERO_LOG_INFO("yaw {:5.1f}  drawn {:5} / {}  culled {:5} ({:4.1f}%)  record {:.3f} ms", degrees(yaw),
                              drawn, instanceCount, culled, culledPercent, recordMs);
                const double now = monotonicSeconds();
                if (now - lastTitleAt >= TITLE_UPDATE_SECONDS) {
                    window->setTitle("Aero — Phase 3 Culling · " + std::to_string(std::lround(clock.fps())) +
                                     " fps · " + std::to_string(drawn) + "/" + std::to_string(instanceCount));
                    lastTitleAt = now;
                }
            }
        }
    }

    AERO_LOG_INFO("phase-3-culling: the degenerate-projection warning {}",
                  forward->hasWarnedDegenerateFrustum() ? "FIRED at least once" : "never fired");
    AERO_LOG_INFO("closing after {} frames, {:.1f}s", clock.frameCount(), clock.totalSeconds());
    AERO_LOG_INFO("record this run in editor/validation/3.6.1-frustum-culling.md (this OS)");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return runSample(argc, argv);
    } catch (const std::exception& e) {
        AERO_LOG_CRITICAL("phase-3-culling: unexpected exception: {}", e.what());
        return 1;
    } catch (...) {
        AERO_LOG_CRITICAL("phase-3-culling: unexpected exception");
        return 1;
    }
}

#else  // AERO_PHASE3_CULLING_ENABLED

int main() {
    AERO_LOG_CRITICAL("phase-3-culling needs AERO_SHADER_TOOLS");
    return 1;
}

#endif  // AERO_PHASE3_CULLING_ENABLED

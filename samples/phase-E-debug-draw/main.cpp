// Aero Engine — task E.1.1's deliverable: the world-space line renderer made visible, and MEASURABLE.
//
// What it draws, and every element earns its place by witnessing one claim:
//   * A lit CUBE at the origin, a SPHERE beside it and a FLOOR PLANE below -- the three built-in
//     primitives through ForwardRenderer, so the debug batch has real geometry to be tested against.
//   * A yellow WIRE BOX exactly on the cube (Tested). Its far edges are HIDDEN by the cube and its
//     near edges are not: that is the depth test, and it is also LessOrEqual doing its job, because
//     the box's edges lie on the cube's own vertices.
//   * A magenta CIRCLE inside the cube (Overlay). It shows THROUGH the solid cube, which is the
//     other half of the same claim.
//   * A floor LATTICE whose alpha fades with distance from the origin, pushed through the
//     lines(span) overload with PER-VERTEX colour -- the mechanism E.1.2's grid will use.
//   * Five white DISC BILLBOARDS marching away from the camera. They MEASURE THE SAME WIDTH at every
//     depth while the camera dollies, which is the screen-constant-size claim and cannot be judged
//     from a still image.
//   * One disc BEHIND the cube (Tested -> hidden) and one AT its centre (Overlay -> visible).
//   * The three world AXES from the origin, X red / Y green / Z blue (Overlay).
//   * One PURE WHITE horizontal line across the upper third (Overlay), away from everything else.
//     That is the colour meter's target and the only EXACT numeric claim this sample makes.
//
// The startup table is the point of the sample, and it is 3.6.3's method: it prints the expected
// 8-bit output for all three operators AND for --raw, at this run's exposure, computed from
// render::tonemapAndEncode itself -- so a validation pass compares a screen reading against a number
// THIS BUILD produced rather than a number copied from a document.
//
// Arguments, in any order:
//   --tonemap=none|reinhard|aces   the tone curve (default aces)
//   --exposure=<float>             sanitized through render::sanitizeTonemapParams (default 1.0)
//   --raw                          BYPASS PostProcess entirely -- the batch draws straight into the
//                                  swapchain frame, which CARRIES DEPTH. The A/B control.
//   --overflow                     push exactly 1000 segments PAST the budget every frame, so the
//                                  per-frame line reads a steady drop count and the WARN fires ONCE.
//   --no-lines                     skip the flush entirely. Not "push an empty batch" -- an empty
//                                  flush is already free, so that would measure nothing. This is the
//                                  cost A/B: the whole mechanism against none of it.
//
// CI builds this on three OSes (compile-proof only -- no display there); run it locally for the
// visual pass and record the result in editor/validation/E.1.1-debug-line-renderer.md. Requires
// AERO_SHADER_TOOLS (the cooked scene.{vert,frag}, fullscreen.vert/tonemap.frag and
// debug_line.*/debug_billboard.*); without it this compiles a stub main that logs and returns 1,
// the phase-0-cube precedent.
#include <aero/core/log.hpp>
#include <aero/core/time.hpp>
#include <aero/core/vfs.hpp>
#include <aero/platform/platform.hpp>
#include <aero/render/renderer.hpp>
#include <aero/rhi/rhi.hpp>

#ifdef AERO_PHASEE_DEBUG_DRAW_ENABLED
    #include <aero/render/render.hpp>
#endif

#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <string>

#ifdef AERO_PHASEE_DEBUG_DRAW_ENABLED

    #include <algorithm>  // std::clamp -- the --raw column, the exposure argument and the fade
    #include <array>
    #include <cstddef>
    #include <cstdint>
    #include <cstdlib>  // std::strtof -- the exposure argument
    #include <span>
    #include <string_view>
    #include <vector>

namespace {

using namespace engine;  // sample TU (not a header) -- docs/04 forbids this only in headers

// The two linear values the startup table describes. 1.0 is the pure white line's own value and is
// the HEADLINE: it is drawn with no lighting, no material and no interpolation, so the HDR buffer
// holds exactly 1.0 there and the meter reading is the transfer chain's answer and nothing else.
// 0.25 is the lattice line's own colour, printed so the fade has a number beside it -- a reference
// rather than an exact expectation, because a lattice line is ALPHA-BLENDED over the floor.
constexpr float MEASURED_WHITE = 1.0F;
constexpr float MEASURED_LATTICE = 0.25F;

constexpr float LATTICE_HALF_EXTENT = 6.0F;  // the lattice spans [-6, +6] in X and Z
constexpr int LATTICE_LINES = 21;            // per axis, so 21 x 21
constexpr int LATTICE_SEGMENTS = 20;         // per line -- a subdivided line is what FADES along it
constexpr float LATTICE_FADE_DISTANCE = 12.0F;
constexpr float FLOOR_Y = -1.01F;   // the plane sits just BELOW the lattice on purpose:
constexpr float LATTICE_Y = -1.0F;  // a line exactly on a surface z-fights (see the README)
constexpr float FLOOR_SCALE = 24.0F;

constexpr float CUBE_HALF = 0.5F;  // the Cube primitive spans 1 unit
constexpr float SPHERE_X = 2.2F;
constexpr float SPHERE_SCALE = 0.75F;
constexpr float CIRCLE_RADIUS = 0.35F;
constexpr std::uint32_t CIRCLE_SEGMENTS = 48;
constexpr float AXIS_LENGTH = 2.0F;
constexpr float WHITE_LINE_Y = 2.2F;  // the meter target: upper third, away from everything
constexpr float WHITE_LINE_HALF = 3.0F;

constexpr float BILLBOARD_SIZE_PX = 24.0F;
constexpr float BILLBOARD_X = -3.0F;
constexpr float BILLBOARD_Y = 1.2F;
constexpr std::array<float, 5> BILLBOARD_Z{0.0F, -4.0F, -8.0F, -12.0F, -16.0F};
constexpr float HIDDEN_DISC_Z = -2.0F;  // BEHIND the cube: Tested, therefore invisible

constexpr float EYE_NEAR_DISTANCE = 6.0F;
constexpr float EYE_FAR_DISTANCE = 14.0F;
constexpr double DOLLY_PERIOD_SECONDS = 6.0;
constexpr float FOV_Y_DEGREES = 60.0F;
constexpr float Z_NEAR = 0.1F;
constexpr float Z_FAR = 100.0F;
constexpr float BYTES_PER_HDR_TEXEL = 8.0F;  // RGBA16Float
constexpr float BYTES_PER_MB = 1024.0F * 1024.0F;
constexpr double TITLE_UPDATE_SECONDS = 0.25;
constexpr std::uint32_t OVERFLOW_MARGIN = 1000;  // segments past the budget, EVERY frame, CONSTANT

struct Options {
    render::TonemapParams tonemap{};
    bool usePass = true;    // false == --raw
    bool overflow = false;  // --overflow
    bool drawLines = true;  // false == --no-lines
};

[[nodiscard]] std::optional<render::TonemapOperator> parseOperator(std::string_view name) {
    if (name == "none") {
        return render::TonemapOperator::None;
    }
    if (name == "reinhard") {
        return render::TonemapOperator::Reinhard;
    }
    if (name == "aces") {
        return render::TonemapOperator::AcesApprox;
    }
    return std::nullopt;
}

[[nodiscard]] Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--raw") {
            options.usePass = false;
        } else if (arg == "--overflow") {
            options.overflow = true;
        } else if (arg == "--no-lines") {
            options.drawLines = false;
        } else if (arg.starts_with("--tonemap=")) {
            const std::string_view name = arg.substr(std::string_view("--tonemap=").size());
            if (const std::optional<render::TonemapOperator> op = parseOperator(name)) {
                options.tonemap.curve = *op;
            } else {
                AERO_LOG_WARN("phase-E-debug-draw: unknown operator '{}' -- keeping the default", name);
            }
        } else if (arg.starts_with("--exposure=")) {
            const std::string value{arg.substr(std::string_view("--exposure=").size())};
            options.tonemap.exposure = std::strtof(value.c_str(), nullptr);
        } else {
            AERO_LOG_WARN("phase-E-debug-draw: ignoring unrecognised argument '{}'", arg);
        }
    }
    // SANITIZED HERE, once, so a typed --exposure=1e9 or --exposure=nonsense (strtof -> 0) reaches
    // the uniform as a clamped value and the printed table describes what will actually be drawn.
    options.tonemap = render::sanitizeTonemapParams(options.tonemap);
    return options;
}

// THE ONE PLACE THIS SAMPLE NARROWS A FLOAT TO AN INTEGER (phase-3-tonemap's own guard, verbatim in
// spirit): std::lround, never static_cast<int>(x + 0.5F), and an isfinite arm because
// std::clamp(NaN, 0, 1) returns NaN and std::lround of a NaN is unspecified.
[[nodiscard]] long to8Bit(float encoded) {
    if (!std::isfinite(encoded)) {
        return 0;
    }
    return std::lround(255.0F * std::clamp(encoded, 0.0F, 1.0F));
}

[[nodiscard]] long expectedByte(float linear, float exposure, render::TonemapOperator op) {
    return to8Bit(render::tonemapAndEncode(Vec3{linear, linear, linear}, {exposure, op}).x);
}

// What --raw produces: the unorm target clamps the raw linear value and nothing encodes it.
[[nodiscard]] long expectedRawByte(float linear) { return to8Bit(linear); }

void printExpectedTable(const Options& options, const render::DebugDrawBudget& budget) {
    AERO_LOG_INFO("phase-E-debug-draw: allocated budget {} lines / {} billboards", budget.maxLines,
                  budget.maxBillboards);
    AERO_LOG_INFO("phase-E-debug-draw: expected 8-bit output (exposure {:.3f})",
                  static_cast<double>(options.tonemap.exposure));
    AERO_LOG_INFO("  linear      raw    none  reinh    aces");
    for (const float value : std::array<float, 2>{MEASURED_WHITE, MEASURED_LATTICE}) {
        const char* note = value == MEASURED_WHITE ? "      <- the PURE WHITE line (the headline)"
                                                   : "      <- the lattice line's own colour";
        AERO_LOG_INFO("  {:7.5f} {:7} {:7} {:6} {:7}{}", static_cast<double>(value), expectedRawByte(value),
                      expectedByte(value, options.tonemap.exposure, render::TonemapOperator::None),
                      expectedByte(value, options.tonemap.exposure, render::TonemapOperator::Reinhard),
                      expectedByte(value, options.tonemap.exposure, render::TonemapOperator::AcesApprox), note);
    }
    if (options.usePass) {
        AERO_LOG_INFO("phase-E-debug-draw: this run draws the '{}' column at exposure {:.3f}",
                      render::tonemapOperatorLabel(options.tonemap.curve),
                      static_cast<double>(options.tonemap.exposure));
    } else {
        AERO_LOG_INFO("phase-E-debug-draw: this run draws the 'raw' column (--raw: NO PostProcess at all)");
    }
}

// The floor lattice, as PER-VERTEX coloured vertices pushed through the lines(span) overload. Each
// line is SUBDIVIDED so the alpha really varies ALONG it rather than only between lines -- two
// endpoints of an unsubdivided 12-unit line are the same distance from the origin, so an unsubdivided
// lattice fades between lines and not across them, which is not the effect E.1.2 needs.
void buildLattice(std::vector<render::DebugLineVertex>& out) {
    out.clear();
    const auto fadedVertex = [](Vec3 position) {
        const float distance = std::sqrt((position.x * position.x) + (position.z * position.z));
        const float alpha = 1.0F - std::clamp(distance / LATTICE_FADE_DISTANCE, 0.0F, 1.0F);
        return render::DebugLineVertex{
            .position = position,
            .rgba = render::packDebugColor(Vec4{MEASURED_LATTICE, MEASURED_LATTICE, MEASURED_LATTICE, alpha})};
    };
    const float step = (2.0F * LATTICE_HALF_EXTENT) / static_cast<float>(LATTICE_LINES - 1);
    const float segment = (2.0F * LATTICE_HALF_EXTENT) / static_cast<float>(LATTICE_SEGMENTS);
    for (int i = 0; i < LATTICE_LINES; ++i) {
        const float offset = -LATTICE_HALF_EXTENT + (static_cast<float>(i) * step);
        for (int s = 0; s < LATTICE_SEGMENTS; ++s) {
            const float a = -LATTICE_HALF_EXTENT + (static_cast<float>(s) * segment);
            const float b = a + segment;
            out.push_back(fadedVertex(Vec3{a, LATTICE_Y, offset}));  // running along X
            out.push_back(fadedVertex(Vec3{b, LATTICE_Y, offset}));
            out.push_back(fadedVertex(Vec3{offset, LATTICE_Y, a}));  // running along Z
            out.push_back(fadedVertex(Vec3{offset, LATTICE_Y, b}));
        }
    }
}

// Rebuilt EVERY FRAME, because a debug batch is immediate-mode -- that is the API's whole shape, and
// flush() drains it. Nothing here is cached between frames.
void buildBatch(render::DebugDrawBatch& batch, const std::vector<render::DebugLineVertex>& lattice, bool overflow) {
    constexpr Vec4 YELLOW{1.0F, 0.85F, 0.1F, 1.0F};
    constexpr Vec4 MAGENTA{1.0F, 0.0F, 1.0F, 1.0F};
    constexpr Vec4 WHITE{1.0F, 1.0F, 1.0F, 1.0F};

    // The wire box, exactly on the cube: far edges HIDDEN, near edges not, and the edges lie on the
    // cube's own vertices, which is what LessOrEqual is for.
    batch.wireBox(Mat4::identity(), Vec3{-CUBE_HALF, -CUBE_HALF, -CUBE_HALF}, Vec3{CUBE_HALF, CUBE_HALF, CUBE_HALF},
                  YELLOW, render::DebugDepth::Tested);
    // ...and a circle INSIDE it that shows through.
    batch.wireCircle(Vec3{}, Vec3{0.0F, 1.0F, 0.0F}, CIRCLE_RADIUS, MAGENTA, CIRCLE_SEGMENTS,
                     render::DebugDepth::Overlay);

    batch.lines(lattice, render::DebugDepth::Tested);

    batch.line(Vec3{}, Vec3{AXIS_LENGTH, 0.0F, 0.0F}, Vec4{1.0F, 0.15F, 0.15F, 1.0F}, render::DebugDepth::Overlay);
    batch.line(Vec3{}, Vec3{0.0F, AXIS_LENGTH, 0.0F}, Vec4{0.15F, 1.0F, 0.15F, 1.0F}, render::DebugDepth::Overlay);
    batch.line(Vec3{}, Vec3{0.0F, 0.0F, AXIS_LENGTH}, Vec4{0.3F, 0.4F, 1.0F, 1.0F}, render::DebugDepth::Overlay);

    // THE METER TARGET. Pure white, Overlay so nothing can occlude it, and long enough that a meter
    // aperture lands on a run of its pixels rather than on an edge.
    batch.line(Vec3{-WHITE_LINE_HALF, WHITE_LINE_Y, 0.0F}, Vec3{WHITE_LINE_HALF, WHITE_LINE_Y, 0.0F}, WHITE,
               render::DebugDepth::Overlay);

    for (const float z : BILLBOARD_Z) {
        batch.billboard(Vec3{BILLBOARD_X, BILLBOARD_Y, z}, BILLBOARD_SIZE_PX, WHITE, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F},
                        render::DebugDepth::Overlay);
    }
    batch.billboard(Vec3{0.0F, 0.0F, HIDDEN_DISC_Z}, BILLBOARD_SIZE_PX, WHITE, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F},
                    render::DebugDepth::Tested);  // BEHIND the cube -> hidden
    batch.billboard(Vec3{}, BILLBOARD_SIZE_PX, MAGENTA, Vec2{0.0F, 0.0F}, Vec2{1.0F, 1.0F},
                    render::DebugDepth::Overlay);  // AT its centre -> visible through it

    if (!overflow) {
        return;
    }
    // EXACTLY OVERFLOW_MARGIN over the budget, computed from what is already in the batch, so the
    // per-frame line reads a STEADY drop count rather than a number that drifts with the content.
    const std::uint32_t attempts = batch.budget().maxLines + OVERFLOW_MARGIN;
    for (std::uint32_t i = batch.lineCount(); i < attempts; ++i) {
        batch.line(Vec3{-8.0F, 3.0F, 0.0F}, Vec3{-8.0F, 3.5F, 0.0F}, YELLOW, render::DebugDepth::Overlay);
    }
}

int runSample(int argc, char** argv) {
    const Options options = parseOptions(argc, argv);

    platform::Context ctx;  // real driver (headless=false) -- needed for GPU
    if (!ctx.valid()) {
        AERO_LOG_CRITICAL("platform init failed");
        return 1;
    }

    std::optional<platform::Window> window =
        ctx.createWindow({.title = "Aero — Phase E Debug Draw", .width = 1280, .height = 720});
    if (!window) {
        return 1;
    }

    std::optional<rhi::Device> device = rhi::Device::create();
    if (!device) {
        AERO_LOG_CRITICAL("no GPU device");
        return 1;
    }

    // The swapchain frame CARRIES DEPTH, which is what makes --raw a real A/B: with the pass off the
    // batch still has something to depth-test against.
    std::optional<render::Renderer> renderer = render::Renderer::create(*device, *window, {.depth = true});
    if (!renderer) {
        AERO_LOG_CRITICAL("renderer creation failed");
        return 1;
    }

    VirtualFileSystem vfs;
    vfs.mount(std::make_unique<DirectoryBackend>(AERO_SHADERS_DIR));

    const platform::WindowSize firstPixels = window->pixelSize();
    rhi::Extent2D sceneExtent{static_cast<std::uint32_t>(firstPixels.width),
                              static_cast<std::uint32_t>(firstPixels.height)};

    std::optional<render::PostProcess> post;
    if (options.usePass) {
        post = render::PostProcess::create(
            *device, vfs, sceneExtent,
            {.outputColorFormat = renderer->colorFormat(), .outputDepthFormat = renderer->depthFormat()});
        if (!post) {
            AERO_LOG_CRITICAL("post-process creation failed (are res://fullscreen.vert / res://tonemap.frag cooked?)");
            return 1;
        }
    }

    const rhi::TextureFormat sceneColor = post ? post->sceneColorFormat() : renderer->colorFormat();
    const rhi::TextureFormat sceneDepth = post ? post->sceneDepthFormat() : renderer->depthFormat();
    std::optional<render::ForwardRenderer> forward = render::ForwardRenderer::create(
        *device, vfs, {.colorFormat = sceneColor, .depthFormat = sceneDepth, .shadowMapResolution = 0});
    if (!forward) {
        AERO_LOG_CRITICAL("forward renderer creation failed");
        return 1;
    }

    // Built against the SAME pair the ForwardRenderer was, which is the editor's own rule: a debug
    // pipeline records into the scene pass, so its formats are the scene pass's.
    std::optional<render::DebugDraw> debugDraw =
        render::DebugDraw::create(*device, vfs, {.colorFormat = sceneColor, .depthFormat = sceneDepth});
    if (!debugDraw) {
        AERO_LOG_CRITICAL("debug draw creation failed (are res://debug_line.* / res://debug_billboard.* cooked?)");
        return 1;
    }

    std::vector<render::MeshInstance> instances;
    instances.reserve(3);
    const render::MaterialHandle floorMaterial = forward->createMaterial(
        {.baseColorFactor = Vec4{0.16F, 0.17F, 0.19F, 1.0F}, .metallicFactor = 0.0F, .roughnessFactor = 0.95F}, {});
    render::MeshInstance floor;
    floor.primitive = render::PrimitiveId::Plane;
    floor.model = compose({.translation = Vec3{0.0F, FLOOR_Y, 0.0F}, .scale = Vec3{FLOOR_SCALE, 1.0F, FLOOR_SCALE}});
    floor.normalMatrix = Mat4::identity();
    floor.color = Vec3::one();
    floor.material = floorMaterial;
    instances.push_back(floor);

    const render::MaterialHandle cubeMaterial = forward->createMaterial(
        {.baseColorFactor = Vec4{0.55F, 0.35F, 0.25F, 1.0F}, .metallicFactor = 0.0F, .roughnessFactor = 0.6F}, {});
    render::MeshInstance cube;
    cube.primitive = render::PrimitiveId::Cube;
    cube.model = Mat4::identity();
    cube.normalMatrix = Mat4::identity();
    cube.color = Vec3::one();
    cube.material = cubeMaterial;
    instances.push_back(cube);

    const render::MaterialHandle sphereMaterial = forward->createMaterial(
        {.baseColorFactor = Vec4{0.3F, 0.45F, 0.7F, 1.0F}, .metallicFactor = 0.0F, .roughnessFactor = 0.35F}, {});
    render::MeshInstance sphere;
    sphere.primitive = render::PrimitiveId::Sphere;
    sphere.model =
        compose({.translation = Vec3{SPHERE_X, 0.0F, 0.0F}, .scale = Vec3{SPHERE_SCALE, SPHERE_SCALE, SPHERE_SCALE}});
    sphere.normalMatrix = Mat4::identity();
    sphere.color = Vec3::one();
    sphere.material = sphereMaterial;
    instances.push_back(sphere);

    std::vector<render::DebugLineVertex> lattice;
    buildLattice(lattice);

    printExpectedTable(options, debugDraw->budget());
    AERO_LOG_INFO(
        "phase-E-debug-draw: the camera DOLLIES between {:.0f} and {:.0f} units every {:.0f}s -- the "
        "five discs must keep the same on-screen width the whole time.",
        static_cast<double>(EYE_NEAR_DISTANCE), static_cast<double>(EYE_FAR_DISTANCE), DOLLY_PERIOD_SECONDS);
    if (!options.drawLines) {
        AERO_LOG_INFO("phase-E-debug-draw: --no-lines -- flush() is NEVER CALLED this run (the cost A/B)");
    }

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

        const rhi::Color clearColor{0.02F, 0.02F, 0.03F, 1.0F};
        if (post) {
            post->resize(sceneExtent);
        }

        std::optional<render::Frame> sceneFrame =
            post ? post->beginScene(clearColor) : renderer->beginFrame(clearColor);
        if (!sceneFrame) {
            continue;
        }
        // ONE checked access, then a reference. clang-tidy's bugprone-unchecked-optional-access
        // loses the optional's state across the dolly's branch and the instance loop below and
        // reports every later `*sceneFrame` -- binding here makes the single check the only one
        // there is, which is what the code means anyway.
        render::Frame& frame = *sceneFrame;
        const rhi::Extent2D extent = frame.extent();
        const float aspect =
            extent.height == 0 ? 1.0F : static_cast<float>(extent.width) / static_cast<float>(extent.height);

        // THE DOLLY. The constant-size claim is about a MOVING camera and cannot be judged from a
        // still, so the sample moves it rather than asking anyone to.
        const double phase = clock.totalSeconds() / DOLLY_PERIOD_SECONDS;
        const float sweep = 0.5F * (1.0F + std::sin(static_cast<float>(phase) * TWO_PI));
        const float distance = EYE_NEAR_DISTANCE + ((EYE_FAR_DISTANCE - EYE_NEAR_DISTANCE) * sweep);
        const Vec3 eye = normalize(Vec3{0.0F, 0.42F, 1.0F}) * distance;

        render::RenderView view;
        view.camera = {lookAt(eye, Vec3{}, Vec3{0.0F, 1.0F, 0.0F}),
                       perspective(radians(FOV_Y_DEGREES), aspect, Z_NEAR, Z_FAR), eye};
        view.directional = {
            .direction = normalize(Vec3{-0.4F, -0.7F, -0.55F}), .color = Vec3::one(), .intensity = 2.6F};
        view.ambient = Vec3{0.06F, 0.06F, 0.07F};
        const Mat4 viewProj = view.camera.proj * view.camera.view;
        for (render::MeshInstance& instance : instances) {
            instance.mvp = viewProj * instance.model;
        }
        view.instances = instances;
        forward->draw(frame, view);

        // THE SLOT, and it is the editor's own: AFTER the geometry so a Tested line is depth-tested
        // against it, BEFORE the pass closes so the lines go through the resolve.
        if (options.drawLines) {
            buildBatch(debugDraw->batch(), lattice, options.overflow);
            debugDraw->flush(frame, view.camera);
        }

        bool presented = false;
        if (post) {
            post->endScene(std::move(frame));  // submits command buffer A
            std::optional<render::Frame> outFrame = renderer->beginFrame(clearColor);
            if (!outFrame) {
                continue;
            }
            sceneExtent = outFrame->extent();  // for the NEXT frame
            post->resolve(*outFrame, options.tonemap);
            presented = renderer->endFrame(std::move(*outFrame));  // submits B, strictly after A
        } else {
            presented = renderer->endFrame(std::move(frame));
        }

        if (presented) {
            const double now = monotonicSeconds();
            if (now - lastTitleAt >= TITLE_UPDATE_SECONDS) {
                const float megabytes = post ? static_cast<float>(post->sceneTextureExtent().width) *
                                                   static_cast<float>(post->sceneTextureExtent().height) *
                                                   BYTES_PER_HDR_TEXEL / BYTES_PER_MB
                                             : 0.0F;
                AERO_LOG_INFO("{:.2f} ms  {:.1f} fps  lines {} (dropped {})  billboards {}  draws {}  hdr {:.2f} MB",
                              clock.deltaSeconds() * 1000.0, clock.fps(), debugDraw->lastFrameLines(),
                              debugDraw->lastFrameDroppedLines(), debugDraw->lastFrameBillboards(),
                              debugDraw->lastFrameDrawCalls(), static_cast<double>(megabytes));
                window->setTitle("Aero — Phase E Debug Draw · " + std::to_string(std::lround(clock.fps())) + " fps");
                lastTitleAt = now;
            }
        }
    }

    AERO_LOG_INFO("phase-E-debug-draw: the budget warning {}; the upload warning {}",
                  debugDraw->hasWarnedBudget() ? "FIRED" : "never fired",
                  debugDraw->hasWarnedUploadFailure() ? "FIRED" : "never fired");
    AERO_LOG_INFO("phase-E-debug-draw: {} flushes, {} upload command buffers acquired", debugDraw->flushCount(),
                  debugDraw->uploadCount());
    AERO_LOG_INFO("closing after {} frames, {:.1f}s", clock.frameCount(), clock.totalSeconds());
    AERO_LOG_INFO("record this run in editor/validation/E.1.1-debug-line-renderer.md (this OS)");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return runSample(argc, argv);
    } catch (const std::exception& e) {
        AERO_LOG_CRITICAL("phase-E-debug-draw: unexpected exception: {}", e.what());
        return 1;
    } catch (...) {
        AERO_LOG_CRITICAL("phase-E-debug-draw: unexpected exception");
        return 1;
    }
}

#else  // AERO_PHASEE_DEBUG_DRAW_ENABLED

int main() {
    AERO_LOG_CRITICAL("phase-E-debug-draw needs AERO_SHADER_TOOLS");
    return 1;
}

#endif  // AERO_PHASEE_DEBUG_DRAW_ENABLED

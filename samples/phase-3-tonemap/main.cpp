// Aero Engine — task 3.6.3's deliverable: the tonemap/gamma pass made visible, and MEASURABLE.
//
// What it draws, in two rows, and the asymmetry is deliberate:
//   * ROW A, across the TOP: eleven emissive-only patches running DARK-LEFT to BRIGHT-RIGHT, whose
//     linear values are known EXACTLY. With ambient 0, directional intensity 0, no point lights, a
//     black baseColorFactor, metallic 0 and slot 4's built-in white default, scene.frag.hlsl's whole
//     shading reduces to `lit = emissive = uEmissiveFactor`, so the HDR buffer holds exactly the
//     patch value and nothing else. That is what makes a colour-meter reading comparable to a
//     computed number rather than to an impression.
//   * ROW B, across the BOTTOM: five lit spheres sweeping roughness and metallic under one
//     directional light — the qualitative "does a GGX highlight read correctly now" judgment a flat
//     patch cannot answer.
// A vertically flipped fullscreen triangle is therefore unambiguous at a glance: the ramp is on top,
// it runs dark to bright left to right, and the spheres are below. NO AUTOMATED TIER IN THIS TREE
// CAN SEE A FLIP -- this layout is the witness.
//
// The startup table is the point of the sample. It prints the expected 8-bit output for ALL THREE
// operators AND for --raw, at this run's exposure, computed from render::tonemapAndEncode itself --
// so the validation pass compares a screen reading against a number THIS BUILD produced.
//
// Arguments, in any order:
//   --tonemap=none|reinhard|aces   the tone curve (default aces)
//   --exposure=<float>             sanitized through render::sanitizeTonemapParams (default 1.0)
//   --raw                          BYPASS PostProcess entirely -- draw straight into the swapchain,
//                                  exactly as every sample did before this task. The A/B control.
//
// `--raw` is a branch on the whole render path, not a flag on the pass: with it, no PostProcess is
// created at all. There is deliberately no way to disable the sRGB encode from INSIDE the pass --
// `--tonemap=none` means no tone CURVE, never no ENCODE.
//
// Without --raw this resolves into the swapchain frame, which CARRIES DEPTH
// (RendererConfig{.depth = true}), so this sample is the one production caller exercising
// PostProcessConfig::outputDepthFormat's non-Invalid arm.
//
// CI builds this on three OSes (compile-proof only -- no display there); run it locally for the
// visual pass and record the result in editor/validation/3.6.3-tonemap-gamma.md. Requires
// AERO_SHADER_TOOLS (the cooked scene.{vert,frag} plus fullscreen.vert/tonemap.frag); without it
// this compiles a stub main that logs and returns 1, the phase-0-cube precedent.
#include <aero/core/log.hpp>
#include <aero/core/time.hpp>
#include <aero/core/vfs.hpp>
#include <aero/platform/platform.hpp>
#include <aero/render/renderer.hpp>
#include <aero/rhi/rhi.hpp>

#ifdef AERO_PHASE3_TONEMAP_ENABLED
    #include <aero/render/render.hpp>
#endif

#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <string>

#ifdef AERO_PHASE3_TONEMAP_ENABLED

    #include <algorithm>  // std::clamp -- the --raw column and the exposure argument
    #include <array>
    #include <cstddef>
    #include <cstdint>
    #include <cstdlib>  // std::strtof -- the exposure argument
    #include <span>
    #include <string_view>
    #include <vector>

namespace {

using namespace engine;  // sample TU (not a header) -- docs/04 forbids this only in headers

// The eleven patch values. 0.18 is glTF's middle grey; 0.21404 is the linear value the sRGB OETF
// maps to exactly one half, which is the headline measurement (--raw reads it near 55/255, the pass
// near 127/255). The rest bracket both ends so the roll-off is readable.
constexpr std::array<float, 11> PATCH_VALUES{0.0F, 0.01F, 0.05F, 0.18F, 0.21404F, 0.5F, 1.0F, 2.0F, 4.0F, 8.0F, 16.0F};
constexpr std::size_t SPHERE_COUNT = 5;

constexpr float PATCH_SPACING = 1.05F;  // world units between patch centres
constexpr float PATCH_SCALE = 0.5F;     // the Plane primitive spans 1 unit; this is its half-width
constexpr float PATCH_ROW_Y = 1.6F;     // above the axis -- ROW A is the TOP row
constexpr float SPHERE_SPACING = 2.2F;  // world units between sphere centres
constexpr float SPHERE_ROW_Y = -1.7F;   // below the axis -- ROW B is the BOTTOM row
constexpr float SPHERE_SCALE = 0.75F;
constexpr float EYE_DISTANCE = 12.5F;
constexpr float FOV_Y_DEGREES = 60.0F;
constexpr float Z_NEAR = 0.1F;
constexpr float Z_FAR = 100.0F;
constexpr float BYTES_PER_HDR_TEXEL = 8.0F;  // RGBA16Float
constexpr float BYTES_PER_MB = 1024.0F * 1024.0F;
constexpr double TITLE_UPDATE_SECONDS = 0.25;

struct Options {
    render::TonemapParams tonemap{};
    bool usePass = true;  // false == --raw
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
        } else if (arg.starts_with("--tonemap=")) {
            const std::string_view name = arg.substr(std::string_view("--tonemap=").size());
            if (const std::optional<render::TonemapOperator> op = parseOperator(name)) {
                options.tonemap.curve = *op;
            } else {
                AERO_LOG_WARN("phase-3-tonemap: unknown operator '{}' -- keeping the default", name);
            }
        } else if (arg.starts_with("--exposure=")) {
            const std::string value{arg.substr(std::string_view("--exposure=").size())};
            options.tonemap.exposure = std::strtof(value.c_str(), nullptr);
        } else {
            AERO_LOG_WARN("phase-3-tonemap: ignoring unrecognised argument '{}'", arg);
        }
    }
    // SANITIZED HERE, once, so a typed --exposure=1e9 or --exposure=nonsense (strtof -> 0) reaches
    // the uniform as a clamped value and the printed table describes what will actually be drawn.
    options.tonemap = render::sanitizeTonemapParams(options.tonemap);
    return options;
}

// THE ONE PLACE THIS SAMPLE NARROWS A FLOAT TO AN INTEGER, so the guard lives here rather than at two
// call sites. std::lround, never static_cast<int>(x + 0.5F) -- clang-tidy's bugprone-incorrect-roundings
// is about exactly that. The isfinite check is what makes the narrowing safe BY CONSTRUCTION rather
// than by an argument about the inputs: the transfer chain deliberately PROPAGATES a NaN channel
// (tonemap.hpp property 3), and std::lround of a NaN is unspecified and may raise FE_INVALID. Every
// value fed in below is finite -- the eleven patch constants and a sanitized exposure -- so this never
// fires; it is here so the day one of them stops being finite is a printed 0 and not undefined
// behaviour. std::clamp is NOT the guard: clamp(NaN, 0, 1) returns NaN.
[[nodiscard]] long to8Bit(float encoded) {
    if (!std::isfinite(encoded)) {
        return 0;
    }
    return std::lround(255.0F * std::clamp(encoded, 0.0F, 1.0F));
}

// The 8-bit reading a correct chain produces for one linear value under one operator.
[[nodiscard]] long expectedByte(float linear, float exposure, render::TonemapOperator op) {
    return to8Bit(render::tonemapAndEncode(Vec3{linear, linear, linear}, {exposure, op}).x);
}

// What --raw produces: the unorm target clamps the raw linear value and nothing encodes it. This is
// the pre-3.6.3 behaviour of every sample in this tree, spelled out so the A/B has a printed side.
[[nodiscard]] long expectedRawByte(float linear) { return to8Bit(linear); }

void printExpectedTable(const Options& options) {
    AERO_LOG_INFO("phase-3-tonemap: expected 8-bit output per patch (exposure {:.3f})",
                  static_cast<double>(options.tonemap.exposure));
    AERO_LOG_INFO("  linear      raw    none  reinh    aces");
    for (const float value : PATCH_VALUES) {
        const char* note = "";
        if (value == 0.18F) {
            note = "      <- glTF middle grey";
        } else if (value == 0.21404F) {
            note = "      <- encodes to sRGB 0.5";
        }
        AERO_LOG_INFO("  {:7.5f} {:7} {:7} {:6} {:7}{}", static_cast<double>(value), expectedRawByte(value),
                      expectedByte(value, options.tonemap.exposure, render::TonemapOperator::None),
                      expectedByte(value, options.tonemap.exposure, render::TonemapOperator::Reinhard),
                      expectedByte(value, options.tonemap.exposure, render::TonemapOperator::AcesApprox), note);
    }
    if (options.usePass) {
        AERO_LOG_INFO("phase-3-tonemap: this run draws the '{}' column at exposure {:.3f}",
                      render::tonemapOperatorLabel(options.tonemap.curve),
                      static_cast<double>(options.tonemap.exposure));
    } else {
        AERO_LOG_INFO("phase-3-tonemap: this run draws the 'raw' column (--raw: NO PostProcess at all)");
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
        ctx.createWindow({.title = "Aero — Phase 3 Tonemap", .width = 1280, .height = 720});
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

    // --raw IS A BRANCH ON THE WHOLE RENDER PATH, not a flag on the pass. With it, nothing is created
    // and the forward renderer is built against the SWAPCHAIN's formats, exactly as before this task.
    // The scene target follows the OUTPUT frame's real extent, which is only knowable AFTER the
    // swapchain image is acquired -- and the cycle requires the scene pass to be submitted BEFORE that
    // acquire. So the extent is carried one frame: seeded from the window's pixel size, then updated
    // from each output frame for the next one. In steady state the two always agree; during a LIVE
    // window drag they disagree for exactly one frame, which stretches that frame and latches the
    // INV-1 warning once. That is the pass reporting honestly, not a defect -- the exit line says so.
    const platform::WindowSize firstPixels = window->pixelSize();
    rhi::Extent2D sceneExtent{static_cast<std::uint32_t>(firstPixels.width),
                              static_cast<std::uint32_t>(firstPixels.height)};

    std::optional<render::PostProcess> post;
    if (options.usePass) {
        post = render::PostProcess::create(*device, vfs, sceneExtent,
                                           {.outputColorFormat = renderer->colorFormat(),
                                            // The swapchain frame CARRIES DEPTH here, unlike both
                                            // editor consumers -- this is outputDepthFormat's second
                                            // arm, exercised in production by this sample alone.
                                            .outputDepthFormat = renderer->depthFormat()});
        if (!post) {
            AERO_LOG_CRITICAL("post-process creation failed (are res://fullscreen.vert / res://tonemap.frag cooked?)");
            return 1;
        }
    }

    const rhi::TextureFormat sceneColor = post ? post->sceneColorFormat() : renderer->colorFormat();
    const rhi::TextureFormat sceneDepth = post ? post->sceneDepthFormat() : renderer->depthFormat();
    // shadowMapResolution 0 is EXACT and means OFF (task 3.6.2's D16), and this sample never calls
    // renderShadowMap: ROW A is emissive-only with the directional light at intensity 0, and ROW B is
    // five spheres with no caster and no receiver. At the 2048 default it would allocate ~16.8 MB of
    // dead VRAM plus a comparison sampler, three shader loads and two pipeline compiles for a map
    // nothing writes or samples -- and this sample's whole job is to print a MEASURABLE memory figure,
    // so 16.8 MB it never uses would make its own per-frame line misleading. The 1x1 placeholder slot
    // 5 still needs is what 0 leaves behind.
    std::optional<render::ForwardRenderer> forward = render::ForwardRenderer::create(
        *device, vfs, {.colorFormat = sceneColor, .depthFormat = sceneDepth, .shadowMapResolution = 0});
    if (!forward) {
        AERO_LOG_CRITICAL("forward renderer creation failed");
        return 1;
    }

    // --- ROW A: one material per patch, emissive-only. The reduction that makes the output EXACTLY
    // the patch value needs all of: a black base colour, metallic 0, and slot 4 left at its built-in
    // white default (so `emissive == uEmissiveFactor`). MaterialParams::emissiveFactor is an
    // unclamped Vec3 and nothing between here and the shader clamps it, which is what lets the ramp
    // reach 16.0.
    std::vector<render::MeshInstance> instances;
    instances.reserve(PATCH_VALUES.size() + SPHERE_COUNT);
    const float patchHalfSpan = static_cast<float>(PATCH_VALUES.size() - 1) * PATCH_SPACING * 0.5F;
    for (std::size_t i = 0; i < PATCH_VALUES.size(); ++i) {
        const float value = PATCH_VALUES[i];
        const render::MaterialHandle material =
            forward->createMaterial({.baseColorFactor = Vec4{0.0F, 0.0F, 0.0F, 1.0F},
                                     .emissiveFactor = Vec3{value, value, value},
                                     .metallicFactor = 0.0F,
                                     .roughnessFactor = 1.0F},
                                    {});
        render::MeshInstance patch;
        patch.primitive = render::PrimitiveId::Plane;
        // The Plane primitive is flat in Y, so it is rotated to FACE the camera (+Z) rather than lie
        // in the ground plane -- a 90-degree pitch about X.
        patch.model =
            compose({.translation = Vec3{(static_cast<float>(i) * PATCH_SPACING) - patchHalfSpan, PATCH_ROW_Y, 0.0F},
                     .rotation = fromAxisAngle(Vec3{1.0F, 0.0F, 0.0F}, radians(90.0F)),
                     .scale = Vec3{PATCH_SCALE, 1.0F, PATCH_SCALE}});
        patch.normalMatrix = Mat4::identity();  // unused: this material emits and reflects nothing
        patch.color = Vec3::one();              // the per-instance tint multiplies baseColorFactor (0)
        patch.material = material;
        instances.push_back(patch);
    }

    // --- ROW B: the lit sweep. Roughness 0.1 -> 0.9, the last two metallic, under one directional
    // light -- the highlight roll-off a flat patch cannot show.
    const float sphereHalfSpan = static_cast<float>(SPHERE_COUNT - 1) * SPHERE_SPACING * 0.5F;
    for (std::size_t i = 0; i < SPHERE_COUNT; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(SPHERE_COUNT - 1);
        const render::MaterialHandle material =
            forward->createMaterial({.baseColorFactor = Vec4{0.85F, 0.82F, 0.78F, 1.0F},
                                     .metallicFactor = i >= SPHERE_COUNT - 2 ? 1.0F : 0.0F,
                                     .roughnessFactor = 0.1F + (0.8F * t)},
                                    {});
        render::MeshInstance sphere;
        sphere.primitive = render::PrimitiveId::Sphere;
        sphere.model =
            compose({.translation = Vec3{(static_cast<float>(i) * SPHERE_SPACING) - sphereHalfSpan, SPHERE_ROW_Y, 0.0F},
                     .scale = Vec3{SPHERE_SCALE, SPHERE_SCALE, SPHERE_SCALE}});
        sphere.normalMatrix = Mat4::identity();  // uniform scale + no rotation -> the identity holds
        sphere.color = Vec3::one();
        sphere.material = material;
        instances.push_back(sphere);
    }

    const Vec3 eye{0.0F, 0.0F, EYE_DISTANCE};

    printExpectedTable(options);
    AERO_LOG_INFO(
        "phase-3-tonemap: TOP row = 11 emissive-only patches, dark LEFT to bright RIGHT; BOTTOM row = "
        "5 lit spheres. That asymmetry is how a vertically flipped picture is spotted.");

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
            // Both targets follow the swapchain, so the resolve's blit stays 1:1.
            post->resize(sceneExtent);
        }

        // The SCENE pass. With the pass on, it goes into the HDR target; with --raw it goes straight
        // into the swapchain and no resolve happens at all.
        std::optional<render::Frame> sceneFrame =
            post ? post->beginScene(clearColor) : renderer->beginFrame(clearColor);
        if (!sceneFrame) {
            continue;
        }
        const rhi::Extent2D extent = sceneFrame->extent();
        const float aspect =
            extent.height == 0 ? 1.0F : static_cast<float>(extent.width) / static_cast<float>(extent.height);

        render::RenderView view;
        view.camera = {lookAt(eye, Vec3{}, Vec3{0.0F, 1.0F, 0.0F}),
                       perspective(radians(FOV_Y_DEGREES), aspect, Z_NEAR, Z_FAR), eye};
        // ROW A's reduction depends on ALL THREE of these being zero: with any ambient, any
        // directional intensity or any point light, the patches stop being exactly their own value.
        view.directional = {.direction = normalize(Vec3{-0.4F, -0.5F, -1.0F}), .color = Vec3::one(), .intensity = 0.0F};
        view.ambient = Vec3{};
        const Mat4 viewProj = view.camera.proj * view.camera.view;
        for (render::MeshInstance& instance : instances) {
            instance.mvp = viewProj * instance.model;
        }
        view.instances = instances;

        // ROW B needs a light, and ROW A must have none. They are drawn in TWO passes into the SAME
        // frame for exactly that reason: one view with intensity 0 covering the patches, one with the
        // real light covering the spheres. The second pass does not clear -- it records into the
        // frame the first opened.
        view.instances = std::span{instances}.first(PATCH_VALUES.size());
        forward->draw(*sceneFrame, view);

        render::RenderView litView = view;
        litView.directional.intensity = 3.0F;
        litView.ambient = Vec3{0.03F, 0.03F, 0.035F};
        litView.instances = std::span{instances}.subspan(PATCH_VALUES.size());
        forward->draw(*sceneFrame, litView);

        bool presented = false;
        if (post) {
            post->endScene(std::move(*sceneFrame));  // submits command buffer A
            std::optional<render::Frame> outFrame = renderer->beginFrame(clearColor);
            if (!outFrame) {
                continue;
            }
            sceneExtent = outFrame->extent();  // for the NEXT frame -- see the seeding note above
            post->resolve(*outFrame, options.tonemap);
            presented = renderer->endFrame(std::move(*outFrame));  // submits B, strictly after A
        } else {
            presented = renderer->endFrame(std::move(*sceneFrame));
        }

        if (presented) {
            const double now = monotonicSeconds();
            if (now - lastTitleAt >= TITLE_UPDATE_SECONDS) {
                if (post) {
                    const rhi::Extent2D hdr = post->sceneTextureExtent();
                    const float megabytes = static_cast<float>(hdr.width) * static_cast<float>(hdr.height) *
                                            BYTES_PER_HDR_TEXEL / BYTES_PER_MB;
                    AERO_LOG_INFO(
                        "tonemap {}  exposure {:.3f}  frame {:.2f} ms  {:.1f} fps  hdr {}x{} RGBA16Float "
                        "{:.2f} MB",
                        render::tonemapOperatorLabel(options.tonemap.curve),
                        static_cast<double>(options.tonemap.exposure), clock.deltaSeconds() * 1000.0, clock.fps(),
                        hdr.width, hdr.height, static_cast<double>(megabytes));
                } else {
                    AERO_LOG_INFO("tonemap raw (no pass)  frame {:.2f} ms  {:.1f} fps", clock.deltaSeconds() * 1000.0,
                                  clock.fps());
                }
                window->setTitle("Aero — Phase 3 Tonemap · " + std::to_string(std::lround(clock.fps())) + " fps · " +
                                 std::string(options.usePass ? render::tonemapOperatorLabel(options.tonemap.curve)
                                                             : std::string_view{"raw"}));
                lastTitleAt = now;
            }
        }
    }

    if (post) {
        AERO_LOG_INFO("phase-3-tonemap: the extent-mismatch warning {}; the resolve-before-endScene warning {}",
                      post->hasWarnedExtentMismatch() ? "FIRED" : "never fired",
                      post->hasWarnedResolveBeforeEndScene() ? "FIRED" : "never fired");
        AERO_LOG_INFO("phase-3-tonemap: {} resolves issued", post->resolveCount());
    }
    AERO_LOG_INFO("closing after {} frames, {:.1f}s", clock.frameCount(), clock.totalSeconds());
    AERO_LOG_INFO("record this run in editor/validation/3.6.3-tonemap-gamma.md (this OS)");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return runSample(argc, argv);
    } catch (const std::exception& e) {
        AERO_LOG_CRITICAL("phase-3-tonemap: unexpected exception: {}", e.what());
        return 1;
    } catch (...) {
        AERO_LOG_CRITICAL("phase-3-tonemap: unexpected exception");
        return 1;
    }
}

#else  // AERO_PHASE3_TONEMAP_ENABLED

int main() {
    AERO_LOG_CRITICAL("phase-3-tonemap needs AERO_SHADER_TOOLS");
    return 1;
}

#endif  // AERO_PHASE3_TONEMAP_ENABLED

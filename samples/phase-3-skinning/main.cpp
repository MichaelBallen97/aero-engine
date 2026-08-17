// Aero Engine — task 3.5.1's deliverable: GPU skinning made visible, and three firsts. It is the
// first time this project draws a skinned vertex, the first end-to-end read of a `.aeroskel`, and
// the first caller of `ForwardRenderer::createMesh` — the mesh registry the whole cooked-mesh chain
// has been building toward since 3.3.1.
//
// What it draws: the SAME registered mesh twice, side by side, under one directional light plus
// ambient with a slow orbit camera. The left copy is drawn with an EMPTY palette, which is the draw
// path's degradation arm shown on purpose — a skinned mesh with no pose is its authored bind pose,
// which is the right picture rather than a fallback, and it never moves. The right copy is posed
// every frame: bindPose once at load, then a sine-phased rotation on every non-root joint composed
// onto the bind rotation, then computeJointPalette into a palette the draw borrows. Same bytes, same
// pipeline family, palette on and off.
//
// The RenderView is assembled BY HAND — no World, no buildRenderView — because nothing in a scene
// can reference a mesh or a skeleton yet (3.1.5 owns that), which makes this file the documentation
// of "what a caller with a rig does".
//
// An optional argv[1] overrides the fixture directory (fixed names `arm.aeromesh`/`arm.aeroskel`
// inside it), which is what the validation page's cap-refusal and real-model rows drive.
//
// CI builds this on three OSes (compile-proof only — no display there); run it locally for the
// visual pass and record the result in editor/validation/3.5.1-skeleton-gpu-skinning.md. Requires
// AERO_SHADER_TOOLS (the cooked scene.{vert,frag} and scene_skinned.vert); without it this compiles
// a stub main that logs and returns 1, the phase-0-cube precedent.
#include <aero/core/log.hpp>
#include <aero/core/time.hpp>
#include <aero/core/vfs.hpp>
#include <aero/platform/platform.hpp>
#include <aero/render/renderer.hpp>
#include <aero/rhi/rhi.hpp>

#ifdef AERO_PHASE3_SKINNING_ENABLED
    #include <aero/assets/cooked_mesh.hpp>
    #include <aero/assets/cooked_skeleton.hpp>
    #include <aero/render/render.hpp>
#endif

#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <string>

#ifdef AERO_PHASE3_SKINNING_ENABLED

    #include <array>
    #include <cstddef>
    #include <span>
    #include <string_view>
    #include <vector>

namespace {

using namespace engine;  // sample TU (not a header) — docs/04 forbids this only in headers

constexpr float ORBIT_RADIUS = 6.0F;     // camera distance from the pair's centre
constexpr float ORBIT_HEIGHT = 2.5F;     // camera height
constexpr float ORBIT_SPEED = 0.35F;     // rad/s — the materials sample's proven constant
constexpr float LOOK_AT_HEIGHT = 1.0F;   // the tubes run 0 -> 2 along +Y, so aim at their middle
constexpr float INSTANCE_OFFSET = 1.4F;  // +/- X, wide enough that a full swing never overlaps
constexpr float WAVE_AMPLITUDE = 0.5F;   // radians per joint at the sine's peak
constexpr float WAVE_SPEED = 2.0F;       // rad/s
constexpr float WAVE_PHASE = 0.6F;       // radians of lag per joint — what makes it a travelling wave
constexpr double TITLE_UPDATE_SECONDS = 0.25;
constexpr double LOG_INTERVAL_SECONDS = 1.0;

// The two committed artifacts, read as res://skinning/<name> through the second VFS mount. The
// names are FIXED so that pointing argv[1] at another directory needs no second argument.
constexpr std::string_view MESH_PATH = "res://skinning/arm.aeromesh";
constexpr std::string_view SKELETON_PATH = "res://skinning/arm.aeroskel";

// The normal matrix scene_render computes for every instance, spelled the same way rather than
// assumed to be the identity: these transforms are translation-only today, and a future rotated
// instance must not need this line rewritten. (Copied from phase-3-materials, deliberately: the two
// samples are the two worked examples of a hand-assembled RenderView.)
[[nodiscard]] Mat4 normalMatrixOf(const Mat4& model) {
    const Mat3 m = transpose(inverse(toMat3(model)));
    return Mat4{std::array<Vec4, 4>{Vec4{m.columns[0].x, m.columns[0].y, m.columns[0].z, 0.0F},
                                    Vec4{m.columns[1].x, m.columns[1].y, m.columns[1].z, 0.0F},
                                    Vec4{m.columns[2].x, m.columns[2].y, m.columns[2].z, 0.0F},
                                    Vec4{0.0F, 0.0F, 0.0F, 1.0F}}};
}

[[nodiscard]] Mat4 translation(Vec3 position) {
    Mat4 m = Mat4::identity();
    m.columns[3] = Vec4{position.x, position.y, position.z, 1.0F};
    return m;
}

// The pose driver, and the whole of this sample's animation: every joint but the root gets a
// sine-phased rotation about Z COMPOSED onto its bind rotation. Composition only — no matrix is
// built here and none is multiplied, because turning poses into skinning matrices is
// computeJointPalette's job and duplicating any of it in a sample would make the sample the second
// implementation of it.
//
// Record 0 is left at its bind pose deliberately: it is the chain's root, so rotating it would
// swing the whole tube rigidly and hide the blending this sample exists to show. Every OTHER record
// is driven, so pointing argv[1] at a real rig animates that rig rather than its first four joints.
void poseJoints(std::span<const render::JointPose> bind, std::span<render::JointPose> out, float seconds) {
    for (std::size_t j = 0; j < bind.size(); ++j) {
        out[j] = bind[j];
        if (j == 0) {
            continue;
        }
        const float phase = (WAVE_SPEED * seconds) + (static_cast<float>(j) * WAVE_PHASE);
        const Quat wave = fromAxisAngle(Vec3{0.0F, 0.0F, 1.0F}, WAVE_AMPLITUDE * std::sin(phase));
        out[j].rotation = wave * bind[j].rotation;
    }
}

int runSample(int argc, char** argv) {
    // argv[1] overrides where the two artifacts are read from; absent, the committed ones beside
    // this file are used. The names inside the directory never change.
    const std::string fixtureDir = argc > 1 ? std::string(argv[1]) : std::string(AERO_PHASE3_SKINNING_DIR);

    platform::Context ctx;  // real driver (headless=false) — needed for GPU
    if (!ctx.valid()) {
        AERO_LOG_CRITICAL("platform init failed");
        return 1;
    }

    std::optional<platform::Window> window =
        ctx.createWindow({.title = "Aero — Phase 3 Skinning", .width = 1280, .height = 720});
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

    // Two mounts on ONE VirtualFileSystem: the cooked shaders at the res:// root (ForwardRenderer's
    // res://scene.* and res://scene_skinned.vert defaults) and this sample's artifacts under
    // res://skinning. Mounts are searched most-recent-first and never collide here — the prefixes
    // are disjoint. The sub-prefix is spelled in FULL virtual form: mount() normalizes it through
    // the same res:// parser every read uses, so a bare "skinning" is unaddressable.
    VirtualFileSystem vfs;
    vfs.mount(std::make_unique<DirectoryBackend>(AERO_SHADERS_DIR));
    vfs.mount("res://skinning", std::make_unique<DirectoryBackend>(fixtureDir));
    AERO_LOG_INFO("phase-3-skinning: reading arm.aeromesh and arm.aeroskel from {}", fixtureDir);

    std::optional<render::ForwardRenderer> forward = render::ForwardRenderer::create(
        *device, vfs, {.colorFormat = renderer->colorFormat(), .depthFormat = renderer->depthFormat()});
    if (!forward) {
        AERO_LOG_CRITICAL("forward renderer creation failed");
        return 1;
    }

    // --- the cooked mesh. The BUFFER stays in this scope for the whole run: a CookedMesh's bulk
    // data is a retained span into these bytes (the docs/09 section 9 contract), and createMesh
    // reads it through sectionVertexBytes/indexBytes.
    const std::optional<ByteBuffer> meshBytes = vfs.readFile(MESH_PATH);
    if (!meshBytes) {
        AERO_LOG_CRITICAL("phase-3-skinning: could not read {}", MESH_PATH);
        return 1;
    }
    const assets::CookedMeshParseResult meshParse = assets::parseCookedMesh(*meshBytes);
    if (meshParse.status != assets::CookedMeshStatus::Ok) {
        AERO_LOG_CRITICAL("phase-3-skinning: {} is not a readable cooked mesh: {} ({})", MESH_PATH,
                          assets::cookedMeshStatusLabel(meshParse.status), meshParse.message);
        return 1;
    }

    const double meshStartedAt = monotonicSeconds();
    const render::MeshHandle mesh = forward->createMesh(meshParse.mesh);
    const double meshUploadMs = (monotonicSeconds() - meshStartedAt) * 1000.0;
    if (!mesh.valid()) {
        AERO_LOG_CRITICAL("phase-3-skinning: arm.aeromesh could not be uploaded");
        return 1;
    }
    AERO_LOG_INFO("phase-3-skinning: createMesh(arm.aeromesh) took {:.2f} ms — {} submesh(es)", meshUploadMs,
                  forward->meshSubmeshCount(mesh));

    // --- the cooked skeleton. Fully owned by the parse result, unlike the mesh (cooked_skeleton.hpp
    // states the asymmetry), so nothing here has a lifetime rule to honour.
    const std::optional<ByteBuffer> skeletonBytes = vfs.readFile(SKELETON_PATH);
    if (!skeletonBytes) {
        AERO_LOG_CRITICAL("phase-3-skinning: could not read {}", SKELETON_PATH);
        return 1;
    }
    const assets::CookedSkeletonParseResult skeletonParse = assets::parseCookedSkeleton(*skeletonBytes);
    if (skeletonParse.status != assets::CookedSkeletonStatus::Ok) {
        AERO_LOG_CRITICAL("phase-3-skinning: {} is not a readable cooked skeleton: {} ({})", SKELETON_PATH,
                          assets::cookedSkeletonStatusLabel(skeletonParse.status), skeletonParse.message);
        return 1;
    }
    const assets::CookedSkeleton& skeleton = skeletonParse.skeleton;
    AERO_LOG_INFO("phase-3-skinning: skeleton — {} joints, {} palette slots", skeleton.joints.size(),
                  skeleton.paletteJointCount);

    // bindPose ONCE at load: it is the pose every frame poses away from, and re-reading it per frame
    // would hide a pose driver that forgot to reset a channel.
    std::vector<render::JointPose> bind(skeleton.joints.size());
    render::bindPose(skeleton, bind);
    std::vector<render::JointPose> posed(skeleton.joints.size());
    std::vector<Mat4> palette(skeleton.paletteJointCount);

    // --- two materials, built in code (no .aeromat here): a warm dielectric for the posed copy and
    // a cool one for its frozen twin, at a roughness that keeps the GGX highlight readable.
    const render::MaterialHandle animatedMaterial = forward->createMaterial(
        {.baseColorFactor = Vec4{0.85F, 0.52F, 0.24F, 1.0F}, .metallicFactor = 0.0F, .roughnessFactor = 0.42F}, {});
    const render::MaterialHandle bindMaterial = forward->createMaterial(
        {.baseColorFactor = Vec4{0.34F, 0.56F, 0.85F, 1.0F}, .metallicFactor = 0.0F, .roughnessFactor = 0.42F}, {});

    // --- the two instances: the SAME MeshHandle and the same submesh, differing only in their
    // palette. instances[0] keeps palette empty forever (D8's degradation arm); instances[1]
    // borrows the vector recomputed every frame below.
    std::array<render::MeshInstance, 2> instances{};
    instances[0].mesh = mesh;
    instances[0].model = translation({-INSTANCE_OFFSET, 0.0F, 0.0F});
    instances[0].normalMatrix = normalMatrixOf(instances[0].model);
    instances[0].material = bindMaterial;
    instances[1].mesh = mesh;
    instances[1].model = translation({INSTANCE_OFFSET, 0.0F, 0.0F});
    instances[1].normalMatrix = normalMatrixOf(instances[1].model);
    instances[1].material = animatedMaterial;
    instances[1].palette = std::span<const Mat4>{palette};

    FrameClock clock;
    double lastTitleAt = 0.0;
    double lastLogAt = 0.0;
    double paletteSeconds = 0.0;  // accumulated since the last log line — row 9's number
    std::size_t paletteCalls = 0;
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
        poseJoints(bind, posed, seconds);
        const double paletteStartedAt = monotonicSeconds();
        render::computeJointPalette(skeleton, posed, palette);
        paletteSeconds += monotonicSeconds() - paletteStartedAt;
        ++paletteCalls;

        const float angle = seconds * ORBIT_SPEED;
        const Vec3 eye{ORBIT_RADIUS * std::cos(angle), ORBIT_HEIGHT, ORBIT_RADIUS * std::sin(angle)};
        const Vec3 target{0.0F, LOOK_AT_HEIGHT, 0.0F};

        const rhi::Color sky{0.02F, 0.02F, 0.03F, 1.0F};
        if (std::optional<render::Frame> frame = renderer->beginFrame(sky)) {
            // Bound ONCE, immediately: everything below works on the open frame by reference rather
            // than by re-dereferencing the optional. phase-3-materials dereferences it three times
            // and lints clean, but this loop body is longer, and past some size
            // bugprone-unchecked-optional-access stops being able to prove the later derefs safe and
            // reports them — a lint budget, not a defect, and one line settles it either way.
            render::Frame& openFrame = *frame;
            // The aspect comes from the OPEN FRAME (phase-0-cube's own idiom), which is what makes a
            // drag-resize track without stretching — Renderer has no extent() of its own.
            const rhi::Extent2D extent = openFrame.extent();
            const float aspect =
                extent.height == 0 ? 1.0F : static_cast<float>(extent.width) / static_cast<float>(extent.height);
            render::RenderView view;
            view.camera = {lookAt(eye, target, Vec3{0.0F, 1.0F, 0.0F}),
                           perspective(radians(60.0F), aspect, 0.1F, 100.0F), eye};
            view.directional = {
                .direction = normalize(Vec3{-0.5F, -1.0F, -0.3F}), .color = Vec3::one(), .intensity = 3.0F};
            view.ambient = Vec3{0.03F, 0.03F, 0.03F};
            for (render::MeshInstance& instance : instances) {
                instance.mvp = view.camera.proj * view.camera.view * instance.model;
            }
            view.instances = instances;

            forward->draw(openFrame, view);
            if (renderer->endFrame(std::move(openFrame))) {
                const double now = monotonicSeconds();
                if (now - lastTitleAt >= TITLE_UPDATE_SECONDS) {
                    const std::string fps = std::to_string(std::lround(clock.fps()));
                    window->setTitle("Aero — Phase 3 Skinning · " + fps + " fps");
                    lastTitleAt = now;
                }
                if (now - lastLogAt >= LOG_INTERVAL_SECONDS && paletteCalls > 0) {
                    const double meanMicros = (paletteSeconds / static_cast<double>(paletteCalls)) * 1e6;
                    AERO_LOG_INFO("fps {:.1f} · dt {:.2f} ms · computeJointPalette mean {:.2f} us over {} calls",
                                  clock.fps(), clock.deltaSeconds() * 1000.0F, meanMicros, paletteCalls);
                    paletteSeconds = 0.0;
                    paletteCalls = 0;
                    lastLogAt = now;
                }
            }
        }
    }

    AERO_LOG_INFO("phase-3-skinning: {} skinned draws recorded; the joint cap {}", forward->skinnedDrawCount(),
                  forward->hasWarnedSkinningCap() ? "REFUSED at least one instance" : "never fired");
    AERO_LOG_INFO("closing after {} frames, {:.1f}s", clock.frameCount(), clock.totalSeconds());
    AERO_LOG_INFO("record this run in editor/validation/3.5.1-skeleton-gpu-skinning.md (this OS)");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return runSample(argc, argv);
    } catch (const std::exception& e) {
        AERO_LOG_CRITICAL("phase-3-skinning: unexpected exception: {}", e.what());
        return 1;
    } catch (...) {
        AERO_LOG_CRITICAL("phase-3-skinning: unexpected exception");
        return 1;
    }
}

#else  // AERO_PHASE3_SKINNING_ENABLED

int main() {
    AERO_LOG_CRITICAL("phase-3-skinning needs AERO_SHADER_TOOLS");
    return 1;
}

#endif  // AERO_PHASE3_SKINNING_ENABLED

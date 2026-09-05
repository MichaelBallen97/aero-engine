// Aero Engine — task 3.5.2's deliverable: keyframe playback made visible. The phase-3-skinning mold
// with one addition and one substitution — aero::scene joins the link line, and the procedural sine
// that posed the last sample is replaced by real keyframe data read off disk.
//
// What it draws: the SAME registered mesh three times, under one directional light plus ambient with
// a slow orbit camera. The LEFT copy is drawn with an EMPTY palette — its authored bind pose, frozen,
// and the reference the other two are judged against. The CENTRE copy plays the clip at speed 1 and
// loops. The RIGHT copy plays it at speed 0.25 and does NOT loop, so it runs a quarter as fast and
// then holds its final pose forever. Same bytes, same mesh handle, same pipeline family; three
// clocks.
//
// The clip drives all three glTF paths through all three interpolation modes, chosen so each is
// unmistakable: joints 1–3 rotate LINEARly for the wave, the tip pops between two scales on a STEP
// channel (no interpolating sampler can produce a pop), and the root bobs on a CUBICSPLINE
// translation whose ease-in and ease-out no linear sampler can produce.
//
// THE SAMPLE BUILDS A REAL WORLD. Each copy is an entity with a Transform — which is where its place
// on screen comes from — and the two driven ones also carry an engine::AnimationPlayer. Every frame
// the program advances those players through advanceAnimationPlayer and reads `time` back OUT of the
// World to sample with. It does NOT call buildRenderView, because nothing in a scene can reference a
// mesh, a skeleton or a clip yet (3.1.5 owns that seam), which is why the RenderView is still
// assembled by hand. Driving the picture from data living in an ECS component is what makes the
// component's participation real rather than notional.
//
// An optional argv[1] overrides the fixture directory (fixed names `wave.aeromesh`, `wave.aeroskel`
// and `wave.aeroanim` inside it), which is what the validation page's real-model row drives; argv[2]
// picks the submesh, for the reason 3.5.1 added it.
//
// CI builds this on three OSes (compile-proof only — no display there); run it locally for the visual
// pass and record the result in editor/validation/3.5.2-clip-playback.md. Requires AERO_SHADER_TOOLS
// (the cooked scene.{vert,frag} and scene_skinned.vert); without it this compiles a stub main that
// logs and returns 1, the phase-0-cube precedent.
#include <aero/core/log.hpp>
#include <aero/core/time.hpp>
#include <aero/core/vfs.hpp>
#include <aero/platform/platform.hpp>
#include <aero/render/renderer.hpp>
#include <aero/rhi/rhi.hpp>

#ifdef AERO_PHASE3_ANIMATION_ENABLED
    #include <aero/assets/cooked_animation.hpp>
    #include <aero/assets/cooked_mesh.hpp>
    #include <aero/assets/cooked_skeleton.hpp>
    #include <aero/render/render.hpp>
    #include <aero/scene/scene.hpp>
#endif

#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <string>

#ifdef AERO_PHASE3_ANIMATION_ENABLED

    #include <algorithm>  // std::min — the submesh clamp; std::copy — the per-frame bind reset
    #include <array>
    #include <cstddef>
    #include <cstdint>
    #include <cstdlib>  // std::strtoul — argv[2]
    #include <span>
    #include <string_view>
    #include <vector>

namespace {

using namespace engine;  // sample TU (not a header) — docs/04 forbids this only in headers

constexpr float ORBIT_RADIUS = 7.5F;     // camera distance from the row's centre
constexpr float ORBIT_HEIGHT = 2.5F;     // camera height
constexpr float ORBIT_SPEED = 0.35F;     // rad/s — the materials sample's proven constant
constexpr float LOOK_AT_HEIGHT = 1.0F;   // the tubes run 0 -> 2 along +Y, so aim at their middle
constexpr float INSTANCE_OFFSET = 1.9F;  // +/- X, wide enough that a full swing never overlaps
constexpr float CENTRE_SPEED = 1.0F;     // the reference clock
constexpr float RIGHT_SPEED = 0.25F;     // a quarter of it, so "speed is a multiplier" is countable
constexpr double TITLE_UPDATE_SECONDS = 0.25;
constexpr double LOG_INTERVAL_SECONDS = 1.0;

// The three committed artifacts, read as res://animation/<name> through the second VFS mount. The
// names are FIXED so that pointing argv[1] at another directory needs no further arguments.
constexpr std::string_view MESH_PATH = "res://animation/wave.aeromesh";
constexpr std::string_view SKELETON_PATH = "res://animation/wave.aeroskel";
constexpr std::string_view CLIP_PATH = "res://animation/wave.aeroanim";

// The normal matrix scene_render computes for every instance, spelled the same way rather than
// assumed to be the identity: these transforms are translation-only today, and a future rotated
// instance must not need this line rewritten. (Copied from phase-3-skinning, deliberately: the
// hand-assembled RenderView has three worked examples now and they should stay one shape.)
[[nodiscard]] Mat4 normalMatrixOf(const Mat4& model) {
    const Mat3 m = transpose(inverse(toMat3(model)));
    return Mat4{std::array<Vec4, 4>{Vec4{m.columns[0].x, m.columns[0].y, m.columns[0].z, 0.0F},
                                    Vec4{m.columns[1].x, m.columns[1].y, m.columns[1].z, 0.0F},
                                    Vec4{m.columns[2].x, m.columns[2].y, m.columns[2].z, 0.0F},
                                    Vec4{0.0F, 0.0F, 0.0F, 1.0F}}};
}

// One driven copy. The playback STATE is not in here — it lives on the entity, in an
// engine::AnimationPlayer, and is read back out of the World every frame. What is here is the
// per-instance scratch a shared clip cannot own: the pose the sampler writes into and the palette
// the draw borrows. Both are sized once at load and never resized, so the spans handed to
// MeshInstance stay valid for the whole run.
struct DrivenCopy {
    Entity entity;
    std::vector<render::JointPose> pose;
    std::vector<Mat4> palette;
};

// One entity per copy: a name, and the Transform its model matrix is built from below. The two
// driven ones get their AnimationPlayer from the caller, because that is the only field that
// differs between them.
[[nodiscard]] Entity makeCopy(World& world, std::string_view name, float x) {
    const Entity entity = world.create();
    world.setName(entity, name);
    world.add<Transform>(entity, Transform{.position = Vec3{x, 0.0F, 0.0F}});
    return entity;
}

int runSample(int argc, char** argv) {
    // argv[1] overrides where the three artifacts are read from; absent, the committed ones beside
    // this file are used. The names inside the directory never change.
    const std::string fixtureDir = argc > 1 ? std::string(argv[1]) : std::string(AERO_PHASE3_ANIMATION_DIR);

    // argv[2] picks WHICH submesh the three instances draw; absent, submesh 0, which is what the
    // committed single-submesh wave rig wants. A real model needs it, for 3.5.1's reason: this
    // program draws ONE submesh per instance by design, and a character exported with several
    // materials splits into several submeshes whose ORDER is the exporter's. Out of range is
    // clamped, with the real count logged beside it.
    const unsigned long submeshArg = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 0UL;

    platform::Context ctx;  // real driver (headless=false) — needed for GPU
    if (!ctx.valid()) {
        AERO_LOG_CRITICAL("platform init failed");
        return 1;
    }

    std::optional<platform::Window> window =
        ctx.createWindow({.title = "Aero — Phase 3 Animation", .width = 1280, .height = 720});
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
    // res://animation. Mounts are searched most-recent-first and never collide here — the prefixes
    // are disjoint. The sub-prefix is spelled in FULL virtual form: mount() normalizes it through
    // the same res:// parser every read uses, so a bare "animation" is unaddressable.
    VirtualFileSystem vfs;
    vfs.mount(std::make_unique<DirectoryBackend>(AERO_SHADERS_DIR));
    vfs.mount("res://animation", std::make_unique<DirectoryBackend>(fixtureDir));
    AERO_LOG_INFO("phase-3-animation: reading the three wave.* artifacts from {}", fixtureDir);

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
        AERO_LOG_CRITICAL("phase-3-animation: could not read {}", MESH_PATH);
        return 1;
    }
    const assets::CookedMeshParseResult meshParse = assets::parseCookedMesh(*meshBytes);
    if (meshParse.status != assets::CookedMeshStatus::Ok) {
        AERO_LOG_CRITICAL("phase-3-animation: {} is not a readable cooked mesh: {} ({})", MESH_PATH,
                          assets::cookedMeshStatusLabel(meshParse.status), meshParse.message);
        return 1;
    }
    const render::MeshHandle mesh = forward->createMesh(meshParse.mesh);
    if (!mesh.valid()) {
        AERO_LOG_CRITICAL("phase-3-animation: wave.aeromesh could not be uploaded");
        return 1;
    }

    // --- the cooked skeleton. Fully owned by the parse result, unlike the mesh and the clip
    // (cooked_skeleton.hpp states the asymmetry), so nothing here has a lifetime rule to honour.
    const std::optional<ByteBuffer> skeletonBytes = vfs.readFile(SKELETON_PATH);
    if (!skeletonBytes) {
        AERO_LOG_CRITICAL("phase-3-animation: could not read {}", SKELETON_PATH);
        return 1;
    }
    const assets::CookedSkeletonParseResult skeletonParse = assets::parseCookedSkeleton(*skeletonBytes);
    if (skeletonParse.status != assets::CookedSkeletonStatus::Ok) {
        AERO_LOG_CRITICAL("phase-3-animation: {} is not a readable cooked skeleton: {} ({})", SKELETON_PATH,
                          assets::cookedSkeletonStatusLabel(skeletonParse.status), skeletonParse.message);
        return 1;
    }
    const assets::CookedSkeleton& skeleton = skeletonParse.skeleton;

    // --- the cooked clip. Its BUFFER outlives it too, for the mesh's reason: a CookedAnimation's two
    // bulk regions are retained spans into these bytes, never copies.
    const std::optional<ByteBuffer> clipBytes = vfs.readFile(CLIP_PATH);
    if (!clipBytes) {
        AERO_LOG_CRITICAL("phase-3-animation: could not read {}", CLIP_PATH);
        return 1;
    }
    const assets::CookedAnimationParseResult clipParse = assets::parseCookedAnimation(*clipBytes);
    if (clipParse.status != assets::CookedAnimationStatus::Ok) {
        AERO_LOG_CRITICAL("phase-3-animation: {} is not a readable cooked animation: {} ({})", CLIP_PATH,
                          assets::cookedAnimationStatusLabel(clipParse.status), clipParse.message);
        return 1;
    }
    const assets::CookedAnimation& clip = clipParse.animation;

    // --- bind ONCE, at load: which skeleton record does each channel drive? A channel that names a
    // node this rig does not have is a normal outcome and is reported rather than refused, which is
    // why the four numbers below are printed instead of checked.
    const std::size_t clipChannels = clip.channels.size();
    std::vector<std::uint32_t> binding(clipChannels);
    const double bindStartedAt = monotonicSeconds();
    const render::AnimationBindStats bindStats = render::bindAnimation(clip, skeleton, binding);
    const double bindMicros = (monotonicSeconds() - bindStartedAt) * 1e6;

    AERO_LOG_INFO("phase-3-animation: artifacts — mesh {} B, skeleton {} B, clip {} B", meshBytes->size(),
                  skeletonBytes->size(), clipBytes->size());
    AERO_LOG_INFO("phase-3-animation: skeleton — {} joints, {} palette slots; mesh — {} submesh(es)",
                  skeleton.joints.size(), skeleton.paletteJointCount, forward->meshSubmeshCount(mesh));
    AERO_LOG_INFO("phase-3-animation: clip — {} channels, {:.3f} s", clipChannels, clip.durationSeconds);
    AERO_LOG_INFO("phase-3-animation: bindAnimation — {} channels, {} bound, {} unbound ({:.1f} us)",
                  bindStats.channelCount, bindStats.boundChannels, bindStats.unboundChannels, bindMicros);
    AERO_LOG_INFO("phase-3-animation: bindAnimation — the clip's sourceGuid {} the skeleton's",
                  bindStats.sourceGuidMatches ? "matches" : "DIFFERS");

    // bindPose ONCE at load: it is the pose every frame poses away from, and the pose the LEFT copy
    // is frozen in. Each driven copy's scratch is COPIED from it before every sample, which is
    // sampleAnimation's contract exercised by the deliverable rather than only by a test — the
    // sampler writes only the T, R or S a channel drives and leaves everything else exactly as
    // bindPose left it.
    std::vector<render::JointPose> bind(skeleton.joints.size());
    render::bindPose(skeleton, bind);

    // --- the World. Three entities, each with a Transform; the two driven ones also carry an
    // engine::AnimationPlayer, which is the whole of their playback state.
    World world;
    const Entity bindCopy = makeCopy(world, "BindPose", -INSTANCE_OFFSET);
    std::array<DrivenCopy, 2> driven{DrivenCopy{makeCopy(world, "Playing", 0.0F), {}, {}},
                                     DrivenCopy{makeCopy(world, "QuarterSpeed", INSTANCE_OFFSET), {}, {}}};
    world.add<AnimationPlayer>(driven[0].entity, AnimationPlayer{.speed = CENTRE_SPEED, .loop = true});
    world.add<AnimationPlayer>(driven[1].entity, AnimationPlayer{.speed = RIGHT_SPEED, .loop = false});
    for (DrivenCopy& copy : driven) {
        copy.pose.resize(skeleton.joints.size());
        copy.palette.resize(skeleton.paletteJointCount);
    }

    // --- three materials, built in code (no .aeromat here), at a roughness that keeps the GGX
    // highlight readable: cool blue for the frozen reference, warm orange for the full-speed copy
    // and green for the quarter-speed one, so a screenshot names which is which without a caption.
    const render::MaterialHandle bindMaterial = forward->createMaterial(
        {.baseColorFactor = Vec4{0.34F, 0.56F, 0.85F, 1.0F}, .metallicFactor = 0.0F, .roughnessFactor = 0.42F}, {});
    const render::MaterialHandle playingMaterial = forward->createMaterial(
        {.baseColorFactor = Vec4{0.85F, 0.52F, 0.24F, 1.0F}, .metallicFactor = 0.0F, .roughnessFactor = 0.42F}, {});
    const render::MaterialHandle slowMaterial = forward->createMaterial(
        {.baseColorFactor = Vec4{0.42F, 0.74F, 0.38F, 1.0F}, .metallicFactor = 0.0F, .roughnessFactor = 0.42F}, {});

    // Clamp the submesh against what the mesh actually holds rather than trusting the argument: an
    // out-of-range submesh is a skipped instance and a latched WARN inside the renderer, which from
    // out here looks exactly like a black window.
    const std::uint32_t submeshCount = forward->meshSubmeshCount(mesh);
    const std::uint32_t submesh =
        submeshCount == 0 ? 0U : static_cast<std::uint32_t>(std::min<unsigned long>(submeshArg, submeshCount - 1));
    if (submesh != submeshArg) {
        AERO_LOG_WARN("phase-3-animation: submesh {} requested but the mesh holds {} — drawing {}", submeshArg,
                      submeshCount, submesh);
    }

    // --- the three instances: the SAME MeshHandle and the same submesh, differing in their palette
    // and their material. instances[0] keeps palette empty forever (3.5.1's degradation arm, and the
    // bind-pose reference); instances[1] and [2] borrow the vectors recomputed every frame below.
    // The model matrix comes from the ENTITY's Transform, so the row's layout is scene data too.
    std::array<render::MeshInstance, 3> instances{};
    const std::array<Entity, 3> entities{bindCopy, driven[0].entity, driven[1].entity};
    const std::array<render::MaterialHandle, 3> materials{bindMaterial, playingMaterial, slowMaterial};
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const Transform* transform = world.get<Transform>(entities[i]);
        if (transform == nullptr) {
            AERO_LOG_CRITICAL("phase-3-animation: instance {} has no Transform", i);
            return 1;
        }
        instances[i].mesh = mesh;
        instances[i].submesh = submesh;
        instances[i].model = localMatrix(*transform);
        instances[i].normalMatrix = normalMatrixOf(instances[i].model);
        instances[i].material = materials[i];
    }
    instances[1].palette = std::span<const Mat4>{driven[0].palette};
    instances[2].palette = std::span<const Mat4>{driven[1].palette};

    FrameClock clock;
    double lastTitleAt = 0.0;
    double lastLogAt = 0.0;
    double sampleSeconds = 0.0;  // accumulated since the last log line — row 9's number
    std::size_t sampleCalls = 0;
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

        // THE FRAME'S ANIMATION, and every step of it goes through the World. The clock advances the
        // component; the component's `time` is read back out of it; the sampler is handed that time
        // and nothing else. Nothing here knows whether a player looped — that is the split
        // animation.hpp states: engine/scene owns what time it is, engine/render owns what pose that
        // is.
        const auto deltaSeconds = static_cast<float>(clock.deltaSeconds());
        for (DrivenCopy& copy : driven) {
            auto* player = world.get<AnimationPlayer>(copy.entity);
            if (player == nullptr) {
                continue;
            }
            advanceAnimationPlayer(*player, deltaSeconds, clip.durationSeconds);
            std::copy(bind.begin(), bind.end(), copy.pose.begin());
            const double sampleStartedAt = monotonicSeconds();
            render::sampleAnimation(clip, binding, player->time, copy.pose);
            sampleSeconds += monotonicSeconds() - sampleStartedAt;
            ++sampleCalls;
            render::computeJointPalette(skeleton, copy.pose, copy.palette);
        }

        const float angle = static_cast<float>(clock.totalSeconds()) * ORBIT_SPEED;
        const Vec3 eye{ORBIT_RADIUS * std::cos(angle), ORBIT_HEIGHT, ORBIT_RADIUS * std::sin(angle)};
        const Vec3 target{0.0F, LOOK_AT_HEIGHT, 0.0F};

        const rhi::Color sky{0.02F, 0.02F, 0.03F, 1.0F};
        if (std::optional<render::Frame> frame = renderer->beginFrame(sky)) {
            // Bound ONCE, immediately: everything below works on the open frame by reference rather
            // than by re-dereferencing the optional — a bugprone-unchecked-optional-access budget in
            // a long loop body, not a defect (phase-3-skinning's own note).
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
            // task E.2.1: Flat at the sample's own constant, intensity 1 -- byte-identical shade.
            view.environment = {.ambientMode = render::AmbientMode::Flat,
                                .ambientColor = Vec3{0.03F, 0.03F, 0.03F},
                                .ambientIntensity = 1.0F};
            for (render::MeshInstance& instance : instances) {
                instance.mvp = view.camera.proj * view.camera.view * instance.model;
            }
            view.instances = instances;

            forward->draw(openFrame, view);
            if (renderer->endFrame(std::move(openFrame))) {
                const double now = monotonicSeconds();
                if (now - lastTitleAt >= TITLE_UPDATE_SECONDS) {
                    const std::string fps = std::to_string(std::lround(clock.fps()));
                    window->setTitle("Aero — Phase 3 Animation · " + fps + " fps");
                    lastTitleAt = now;
                }
                if (now - lastLogAt >= LOG_INTERVAL_SECONDS && sampleCalls > 0) {
                    const double meanMicros = (sampleSeconds / static_cast<double>(sampleCalls)) * 1e6;
                    // The two times are read back OUT of the World, never from a local mirror: a
                    // mirror would still print convincing numbers with the component untouched.
                    const AnimationPlayer* centre = world.get<AnimationPlayer>(driven[0].entity);
                    const AnimationPlayer* right = world.get<AnimationPlayer>(driven[1].entity);
                    const float centreTime = centre == nullptr ? 0.0F : centre->time;
                    const float rightTime = right == nullptr ? 0.0F : right->time;
                    AERO_LOG_INFO("fps {:.1f} · dt {:.2f} ms · sampleAnimation mean {:.2f} us over {} calls",
                                  clock.fps(), clock.deltaSeconds() * 1000.0, meanMicros, sampleCalls);
                    AERO_LOG_INFO("players — centre {:.3f} s (x{:.2f}, loops), right {:.3f} s (x{:.2f}, once)",
                                  centreTime, CENTRE_SPEED, rightTime, RIGHT_SPEED);
                    sampleSeconds = 0.0;
                    sampleCalls = 0;
                    lastLogAt = now;
                }
            }
        }
    }

    AERO_LOG_INFO("phase-3-animation: {} skinned draws recorded; the joint cap {}", forward->skinnedDrawCount(),
                  forward->hasWarnedSkinningCap() ? "REFUSED at least one instance" : "never fired");
    AERO_LOG_INFO("closing after {} frames, {:.1f}s", clock.frameCount(), clock.totalSeconds());
    AERO_LOG_INFO("record this run in editor/validation/3.5.2-clip-playback.md (this OS)");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return runSample(argc, argv);
    } catch (const std::exception& e) {
        AERO_LOG_CRITICAL("phase-3-animation: unexpected exception: {}", e.what());
        return 1;
    } catch (...) {
        AERO_LOG_CRITICAL("phase-3-animation: unexpected exception");
        return 1;
    }
}

#else  // AERO_PHASE3_ANIMATION_ENABLED

int main() {
    AERO_LOG_CRITICAL("phase-3-animation needs AERO_SHADER_TOOLS");
    return 1;
}

#endif  // AERO_PHASE3_ANIMATION_ENABLED

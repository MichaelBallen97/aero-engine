// Aero Engine — Phase 1 gate artifact: load & draw a JSON scene (task 1.4.2, closing Epic 1.4). Opens
// a window + GPU device + a depth-enabled engine::render::Renderer, loads the committed scene.json
// from disk through the VFS into a fresh World (engine::scene_serialize::loadSceneText), and draws it
// every frame with engine::scene_render::SceneRenderer — the whole Phase 1 stack (reflect -> scene ->
// scene_serialize -> scene_render) driven end to end from a file. The World is never mutated after
// load: "it's live" is proven by resize (aspect tracks) + a stable fps readout, not motion (mirrors
// samples/phase-0-cube minus the spin). CI builds this on 3 OSes (compile-proof only — no display
// there); a human runs it and records the result in VALIDATION.md. Requires AERO_REFLECT_TOOLS +
// AERO_SHADER_TOOLS (the generated component serializers + the cooked scene.{vert,frag}).
#include <aero/core/log.hpp>
#include <aero/core/time.hpp>
#include <aero/core/vfs.hpp>
#include <aero/platform/platform.hpp>
#include <aero/render/renderer.hpp>
#include <aero/rhi/rhi.hpp>

#ifdef AERO_PHASE1_SCENE_ENABLED
    #include <aero/scene/world.hpp>
    #include <aero/scene_render/scene_renderer.hpp>
    #include <aero/scene_serialize/scene_serialize.hpp>
#endif

#include <cmath>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <string>

#ifdef AERO_PHASE1_SCENE_ENABLED

namespace {

constexpr double TITLE_UPDATE_SECONDS = 0.25;  // <= 4 Hz title refresh (no thrash/flicker)
constexpr double LOG_INTERVAL_SECONDS = 1.0;   // 1 Hz fps log line

// The real sample logic, split out of main() (docs/04: no exceptions across a public API boundary —
// main() is this sample's outermost one). Mirrors phase-0-cube's runSample()/main() split.
int runSample() {
    using namespace engine;  // sample TU (not a header) — docs/04 forbids this only in headers
    platform::Context ctx;   // real driver (headless=false) — needed for GPU
    if (!ctx.valid()) {
        AERO_LOG_CRITICAL("platform init failed");
        return 1;
    }

    std::optional<platform::Window> window =
        ctx.createWindow({.title = "Aero — Phase 1 Scene", .width = 1280, .height = 720});
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

    // --- shaders (cooked; scene.{vert,frag} — ForwardRenderer's res:// defaults, shared shaders dir) ---
    VirtualFileSystem shaderVfs;
    shaderVfs.mount(std::make_unique<DirectoryBackend>(AERO_SHADERS_DIR));

    // --- the committed scene, loaded from disk through the VFS ---------------------------------
    VirtualFileSystem sceneVfs;
    sceneVfs.mount(std::make_unique<DirectoryBackend>(AERO_PHASE1_SCENE_DIR));
    const std::optional<std::string> sceneText = sceneVfs.readText("res://scene.json");
    if (!sceneText) {
        AERO_LOG_CRITICAL("could not read res://scene.json");
        return 1;
    }

    World world;
    const scene_serialize::SceneLoadResult loadResult = scene_serialize::loadSceneText(world, *sceneText);
    if (loadResult.error) {
        AERO_LOG_CRITICAL("scene load failed: {}", loadResult.error->message);
        return 1;
    }
    AERO_LOG_INFO("scene: {} entities, {} components ({} skipped, {} bad-field)", loadResult.report.entitiesCreated,
                  loadResult.report.componentsAttached, loadResult.report.componentsSkipped,
                  loadResult.report.componentsFailed);

    std::optional<scene_render::SceneRenderer> sceneRenderer =
        scene_render::SceneRenderer::create(*device, shaderVfs, renderer->colorFormat(), renderer->depthFormat());
    if (!sceneRenderer) {
        AERO_LOG_CRITICAL("scene renderer creation failed");
        return 1;
    }

    // (gate-narrative touch, non-fatal) exercise SAVE end to end: re-serialize the loaded World and log
    // its size, proving the round-trip path runs against the real loaded scene, not just the tests.
    AERO_LOG_INFO("scene: re-saved text is {} bytes (SAVE path exercised, not written to disk)",
                  scene_serialize::saveWorldText(world).size());

    FrameClock clock;
    double lastTitleAt = 0.0;
    double lastLogAt = 0.0;
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

        const rhi::Color sky{0.05F, 0.06F, 0.08F, 1.0F};
        if (std::optional<render::Frame> frame = renderer->beginFrame(sky)) {
            sceneRenderer->render(world, *frame);  // static data; live render (resize + fps prove it)
            if (renderer->endFrame(std::move(*frame))) {
                const double now = monotonicSeconds();
                if (now - lastTitleAt >= TITLE_UPDATE_SECONDS) {
                    window->setTitle("Aero — Phase 1 Scene · " + std::to_string(std::lround(clock.fps())) + " fps");
                    lastTitleAt = now;
                }
                if (now - lastLogAt >= LOG_INTERVAL_SECONDS) {
                    AERO_LOG_INFO("fps {:.1f} · dt {:.2f} ms", clock.fps(), clock.deltaSeconds() * 1000.0F);
                    lastLogAt = now;
                }
            }
        }
    }

    AERO_LOG_INFO("closing after {} frames, {:.1f}s", clock.frameCount(), clock.totalSeconds());
    AERO_LOG_INFO("record this run in samples/phase-1-scene/VALIDATION.md (this OS)");
    return 0;
}

}  // namespace

int main() {
    try {
        return runSample();
    } catch (const std::exception& e) {
        AERO_LOG_CRITICAL("phase-1-scene: unexpected exception: {}", e.what());
        return 1;
    } catch (...) {
        AERO_LOG_CRITICAL("phase-1-scene: unexpected exception");
        return 1;
    }
}

#else  // AERO_PHASE1_SCENE_ENABLED

int main() {
    AERO_LOG_CRITICAL("phase-1-scene needs AERO_REFLECT_TOOLS + AERO_SHADER_TOOLS");
    return 1;
}

#endif  // AERO_PHASE1_SCENE_ENABLED

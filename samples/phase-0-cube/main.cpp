// Aero Engine — Phase 0 gate artifact: the spinning textured cube (task 0.5.3, finalizing 0.5.2). Opens
// a window + GPU device + a depth-enabled engine::render::Renderer, uploads a unit cube (24 verts / 36
// indices) + a procedural checkerboard texture, and draws it every frame with a time-based MVP transform
// — the canonical hello-world of the whole stack: geometry, uniforms, texture, depth, draw, animation.
// STATIC in 0.5.2 (fixed 3/4 rotation); 0.5.3 makes it SPIN (elapsed-time angle, vsync-paced — never
// uncapped), adds a live fps counter (window title + a 1 Hz log line, both from FrameClock::fps()), and
// prints an objective 60fps gate verdict (fps_gate.hpp) at exit. CI builds this on all 3 OSes
// (compile-proof only — no vsync/display there); the actual 60fps gate is a human on-hardware sign-off,
// recorded per-OS in VALIDATION.md. macOS is validated; Windows/Linux are tracked pending. Run it by
// hand: watch it spin, drag-resize it (aspect tracks, no stretch), Esc quits and prints the verdict.
// Requires AERO_SHADER_TOOLS=ON (cooked cube shaders).
#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/time.hpp>
#include <aero/core/vfs.hpp>
#include <aero/platform/platform.hpp>
#include <aero/render/renderer.hpp>
#include <aero/rhi/rhi.hpp>
#include <aero/rhi/shader_loader.hpp>

#include "cube_mesh.hpp"
#include "fps_gate.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>

#ifdef AERO_CUBE_SHADERS_DIR

namespace {

constexpr float ROTATION_RAD_PER_SEC = 0.6F;   // ~one revolution / 10.5 s — calm, clearly visible
constexpr float TWO_PI = 6.283185307179586F;   // wrap the spin angle so the float stays small
constexpr double TITLE_UPDATE_SECONDS = 0.25;  // <= 4 Hz title refresh (no thrash/flicker)
constexpr double LOG_INTERVAL_SECONDS = 1.0;   // 1 Hz fps log line

// The real sample logic, split out of main() (docs/04: no exceptions across a public API boundary —
// main() is this sample's outermost one; std::vector/std::string/etc. can theoretically throw
// bad_alloc in extreme conditions this function does not individually guard against). Mirrors
// tools/shaderc's runMain()/main() split.
int runSample() {
    using namespace engine;  // sample TU (not a header) — docs/04 forbids this only in headers
    platform::Context ctx;   // real driver (headless=false) — needed for GPU
    if (!ctx.valid()) {
        AERO_LOG_CRITICAL("platform init failed");
        return 1;
    }

    std::optional<platform::Window> window =
        ctx.createWindow({.title = "Aero — Phase 0 Cube", .width = 1280, .height = 720});
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

    // --- shaders (cooked; loaded from the build tree) -----------------------------------------
    VirtualFileSystem vfs;
    vfs.mount(std::make_unique<DirectoryBackend>(AERO_CUBE_SHADERS_DIR));
    const rhi::ShaderHandle vs = rhi::loadShader(*device, vfs, "res://cube.vert");
    const rhi::ShaderHandle fs = rhi::loadShader(*device, vfs, "res://cube.frag");
    if (!vs.valid() || !fs.valid()) {
        AERO_LOG_CRITICAL("cube shader load failed");
        return 1;
    }

    // --- geometry + texture (static; uploaded once) -------------------------------------------
    const cube::CubeMesh mesh = cube::makeCube();
    const rhi::BufferHandle vbuf =
        device->createBuffer({.usage = rhi::BufferUsage::Vertex, .size = sizeof(mesh.vertices)});
    const rhi::BufferHandle ibuf =
        device->createBuffer({.usage = rhi::BufferUsage::Index, .size = sizeof(mesh.indices)});
    device->uploadBuffer(vbuf, 0, std::as_bytes(std::span{mesh.vertices}));
    device->uploadBuffer(ibuf, 0, std::as_bytes(std::span{mesh.indices}));

    constexpr std::uint32_t TEX_SIZE = 256;
    const std::vector<std::byte> texels = cube::makeCheckerboard(TEX_SIZE, 32);
    const rhi::TextureHandle tex = device->createTexture({.format = rhi::TextureFormat::RGBA8Unorm,
                                                          .usage = rhi::TextureUsage::Sampler,
                                                          .width = TEX_SIZE,
                                                          .height = TEX_SIZE});
    device->uploadTexture(tex, 0, texels);
    const rhi::SamplerHandle sampler = device->createSampler({});  // linear/linear/repeat defaults

    // --- pipeline -----------------------------------------------------------------------------
    const rhi::VertexBufferLayout vbLayout{.slot = 0, .pitch = sizeof(cube::Vertex)};
    const std::array<rhi::VertexAttribute, 3> attrs{{
        {.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offset = 0},
        {.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offset = 12},
        {.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offset = 20},
    }};
    const rhi::ColorTargetDesc colorTarget{.format = renderer->colorFormat()};
    const rhi::GraphicsPipelineDesc pd{
        .vertexShader = vs,
        .fragmentShader = fs,
        .vertexBuffers = std::span{&vbLayout, 1},
        .vertexAttributes = attrs,
        .depthStencil = {.enableDepthTest = true, .enableDepthWrite = true, .compareOp = rhi::CompareOp::Less},
        .colorTargets = std::span{&colorTarget, 1},
        .depthStencilFormat = renderer->depthFormat(),
    };
    const rhi::GraphicsPipelineHandle pipeline = device->createGraphicsPipeline(pd);
    device->destroyShader(vs);  // safe after pipeline creation (device.hpp)
    device->destroyShader(fs);
    if (!pipeline.valid()) {
        AERO_LOG_CRITICAL("cube pipeline creation failed");
        return 1;
    }

    // --- fixed camera + spin axis (0.5.3: the model angle is now time-based, recomputed per frame) ---
    const Mat4 view = lookAt(Vec3{0.0F, 0.0F, 2.5F}, Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 spinAxis = normalize(Vec3{0.35F, 1.0F, 0.18F});  // tilted => the tumble shows several faces

    FrameClock clock;
    cube::FpsGate gate;
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
        const float angle = std::fmod(static_cast<float>(clock.totalSeconds()) * ROTATION_RAD_PER_SEC, TWO_PI);
        const Mat4 model = toMat4(fromAxisAngle(spinAxis, angle));

        const rhi::Color sky{0.08F, 0.10F, 0.14F, 1.0F};
        if (std::optional<render::Frame> frame = renderer->beginFrame(sky)) {
            const rhi::Extent2D e = frame->extent();
            const float aspect = e.height != 0 ? static_cast<float>(e.width) / static_cast<float>(e.height) : 1.0F;
            const Mat4 mvp = perspective(radians(60.0F), aspect, 0.1F, 100.0F) * view * model;
            device->pushVertexUniforms(frame->commandBuffer(), 0, std::as_bytes(std::span{mvp.data(), 16}));

            const rhi::RenderPassHandle pass = frame->pass();
            device->bindGraphicsPipeline(pass, pipeline);
            device->bindVertexBuffer(pass, 0, vbuf);
            device->bindIndexBuffer(pass, ibuf, rhi::IndexType::Uint16);
            const rhi::TextureSamplerBinding bind{.texture = tex, .sampler = sampler};
            device->bindFragmentSamplers(pass, 0, std::span{&bind, 1});
            device->drawIndexed(pass, 36);

            if (renderer->endFrame(std::move(*frame))) {
                const double now = monotonicSeconds();
                gate.recordPresent(now);
                if (now - lastTitleAt >= TITLE_UPDATE_SECONDS) {
                    window->setTitle("Aero — Phase 0 Cube · " + std::to_string(std::lround(clock.fps())) + " fps");
                    lastTitleAt = now;
                }
                if (now - lastLogAt >= LOG_INTERVAL_SECONDS) {
                    AERO_LOG_INFO("fps {:.1f} · dt {:.2f} ms", clock.fps(), clock.deltaSeconds() * 1000.0F);
                    lastLogAt = now;
                }
            }
        } else {
            // Not presentable (minimized): beginFrame returns immediately (no vsync wait on a nullopt
            // acquire), so idle a beat to avoid pegging a core until restore (AC-7). Mirrors phase-0-clear.
            gate.noteInterruption();  // break the interval chain so the next present skips the idle gap
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        AERO_PROFILE_FRAME_MARK;
    }

    // --- explicit teardown (clean console; no ~Device leak WARNs) — before renderer/device RAII ---
    device->destroyGraphicsPipeline(pipeline);
    device->destroySampler(sampler);
    device->destroyTexture(tex);
    device->destroyBuffer(ibuf);
    device->destroyBuffer(vbuf);
    const cube::GateSummary gateSummary = gate.summary();
    AERO_LOG_INFO("closing after {} frames, {:.1f}s", clock.frameCount(), clock.totalSeconds());
    AERO_LOG_INFO("60fps gate: {} — avg {:.1f} fps, worst {:.1f} fps over {} frames",
                  cube::toString(gateSummary.verdict), gateSummary.avgFps, gateSummary.worstFps, gateSummary.samples);
    AERO_LOG_INFO("record this run in samples/phase-0-cube/VALIDATION.md (this OS + your display refresh)");
    return 0;
}

}  // namespace

#endif  // AERO_CUBE_SHADERS_DIR

int main() {
#ifndef AERO_CUBE_SHADERS_DIR
    AERO_LOG_CRITICAL("phase-0-cube built without AERO_SHADER_TOOLS — no cooked shaders; exiting");
    return 1;
#else
    try {
        return runSample();
    } catch (const std::exception& e) {
        AERO_LOG_CRITICAL("phase-0-cube: unexpected exception: {}", e.what());
        return 1;
    } catch (...) {
        AERO_LOG_CRITICAL("phase-0-cube: unexpected exception");
        return 1;
    }
#endif
}

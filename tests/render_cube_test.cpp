// Aero Engine — render cube tests (task 0.5.2). C-0 is tier-0 (runs on every CI lane, no GPU). C-1..C-5
// are tier-2: they need a real-video Context + GPU Device + a visible 320x180 window "aero cube test"
// (mirrors render_clear_test.cpp's gate). Gated by AERO_REQUIRE_GPU via rhi_test_support.hpp: unset ->
// skip loudly; set (CI) -> a missing GPU FAILS. Declaration order per case: ctx -> device -> window ->
// renderer (Renderer innermost), matching render_clear_test.cpp / rhi_swapchain_test.cpp.
//
// The whole TU compiles to nothing when AERO_SHADER_TOOLS is OFF (no aero_shaders artifacts exist to
// load the cube shaders from), mirroring rhi_shader_pipeline_test.cpp.
//
// D9 (spec): the test does NOT share samples/phase-0-cube/cube_mesh.hpp. It builds an equivalent-layout
// cube inline — the shared contract (the vertex layout) is authoritative in shaders/cube.vert.hlsl;
// this only needs a same-stride mesh to prove the record->submit pipeline executes error-free.
#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/core/math.hpp>
    #include <aero/core/vfs.hpp>
    #include <aero/platform/platform.hpp>
    #include <aero/render/renderer.hpp>
    #include <aero/rhi/rhi.hpp>
    #include <aero/rhi/shader_loader.hpp>

    #include "rhi_test_support.hpp"

    #include <doctest/doctest.h>

    #include <array>
    #include <cstddef>
    // <ostream> is load-bearing on MSVC for enum/string_view CHECKs (see rhi_device_test.cpp's comment).
    #include <memory>
    #include <optional>
    #include <ostream>
    #include <span>

using engine::render::Frame;
using engine::render::Renderer;
using engine::render::RendererConfig;
using namespace engine::rhi;

namespace {

// Equivalent-layout cube (D9): same 32-byte stride / attribute offsets as shaders/cube.vert.hlsl and
// samples/phase-0-cube/cube_mesh.hpp, but NOT included from there — a small, throwaway, same-shape
// fixture is enough to prove the pipeline records and submits without error.
struct TestVertex {
    engine::Vec3 position;
    engine::Vec2 uv;
    engine::Vec3 color;
};
static_assert(sizeof(TestVertex) == 32);

struct TestCube {
    std::array<TestVertex, 24> vertices;
    std::array<std::uint16_t, 36> indices;
};

TestCube makeTestCube() {
    TestCube mesh{};
    // 6 faces * 4 verts = 24; positions/uv/color values are structurally arbitrary (this test never
    // asserts pixels, D10) but every vertex is populated and every index is in range.
    for (std::size_t face = 0; face < 6; ++face) {
        const auto base = static_cast<std::uint16_t>(face * 4);
        for (std::size_t corner = 0; corner < 4; ++corner) {
            mesh.vertices[(face * 4) + corner] = TestVertex{
                engine::Vec3{static_cast<float>(face), static_cast<float>(corner), 0.0F},
                engine::Vec2{static_cast<float>(corner & 1U), static_cast<float>((corner >> 1U) & 1U)},
                engine::Vec3{1.0F, 1.0F, 1.0F},
            };
        }
        const std::array<std::uint16_t, 6> faceIndices{
            base, static_cast<std::uint16_t>(base + 1), static_cast<std::uint16_t>(base + 2),
            base, static_cast<std::uint16_t>(base + 2), static_cast<std::uint16_t>(base + 3),
        };
        for (std::size_t i = 0; i < 6; ++i) {
            mesh.indices[(face * 6) + i] = faceIndices[i];
        }
    }
    return mesh;
}

// Builds the cube pipeline (attrs/depth-test/color+depth targets) against `renderer`, loading the cooked
// cube shaders from AERO_SHADERS_DIR (the same cooked dir the triangle pair lives in). Returns an invalid
// handle on any failure (the caller REQUIREs).
GraphicsPipelineHandle buildCubePipeline(Device& dev, const Renderer& renderer, const VertexBufferLayout& vbLayout,
                                         const std::array<VertexAttribute, 3>& attrs) {
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    const ShaderHandle vs = loadShader(dev, vfs, "res://cube.vert");
    const ShaderHandle fs = loadShader(dev, vfs, "res://cube.frag");
    if (!vs.valid() || !fs.valid()) {
        return {};
    }
    const ColorTargetDesc colorTarget{.format = renderer.colorFormat()};
    const GraphicsPipelineDesc pd{
        .vertexShader = vs,
        .fragmentShader = fs,
        .vertexBuffers = std::span{&vbLayout, 1},
        .vertexAttributes = attrs,
        .depthStencil = {.enableDepthTest = true, .enableDepthWrite = true, .compareOp = CompareOp::Less},
        .colorTargets = std::span{&colorTarget, 1},
        .depthStencilFormat = renderer.depthFormat(),
    };
    const GraphicsPipelineHandle pipeline = dev.createGraphicsPipeline(pd);
    dev.destroyShader(vs);  // safe after pipeline creation (device.hpp)
    dev.destroyShader(fs);
    return pipeline;
}

}  // namespace

TEST_CASE("render cube: C-0 mesh invariants + RendererConfig default (tier-0, no GPU)") {
    CHECK(sizeof(TestVertex) == 32);
    const TestCube mesh = makeTestCube();
    CHECK(mesh.vertices.size() == 24);
    CHECK(mesh.indices.size() == 36);
    CHECK_FALSE(RendererConfig{}.depth);
}

TEST_CASE("render cube: C-1 depth format + colorFormat") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero cube test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }

    auto renderer = Renderer::create(*device, *window, {.depth = true});
    REQUIRE(renderer.has_value());
    const TextureFormat colorFmt = renderer->colorFormat();
    CHECK((colorFmt == TextureFormat::RGBA8Unorm || colorFmt == TextureFormat::RGBA8UnormSrgb ||
           colorFmt == TextureFormat::BGRA8Unorm || colorFmt == TextureFormat::BGRA8UnormSrgb));
    const TextureFormat depthFmt = renderer->depthFormat();
    CHECK((depthFmt == TextureFormat::D32Float || depthFmt == TextureFormat::D24Unorm ||
           depthFmt == TextureFormat::D16Unorm));
    CHECK(isDepthFormat(depthFmt));

    auto noDepthRenderer = Renderer::create(*device, *window, {.depth = false});
    REQUIRE(noDepthRenderer.has_value());
    CHECK((noDepthRenderer->depthFormat() == TextureFormat::Invalid));
}

TEST_CASE("render cube: C-2 full cube pipeline builds (valid handle on this backend)") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero cube test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }
    auto renderer = Renderer::create(*device, *window, {.depth = true});
    REQUIRE(renderer.has_value());

    const TestCube mesh = makeTestCube();
    const BufferHandle vbuf = device->createBuffer({.usage = BufferUsage::Vertex, .size = sizeof(mesh.vertices)});
    const BufferHandle ibuf = device->createBuffer({.usage = BufferUsage::Index, .size = sizeof(mesh.indices)});
    REQUIRE(vbuf.valid());
    REQUIRE(ibuf.valid());
    CHECK(device->uploadBuffer(vbuf, 0, std::as_bytes(std::span{mesh.vertices})));
    CHECK(device->uploadBuffer(ibuf, 0, std::as_bytes(std::span{mesh.indices})));

    constexpr std::uint32_t TEX_SIZE = 4;
    const std::array<std::byte, static_cast<std::size_t>(TEX_SIZE) * TEX_SIZE * 4> texels{};
    const TextureHandle tex = device->createTexture(
        {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::Sampler, .width = TEX_SIZE, .height = TEX_SIZE});
    REQUIRE(tex.valid());
    CHECK(device->uploadTexture(tex, 0, texels));
    const SamplerHandle sampler = device->createSampler({});
    REQUIRE(sampler.valid());

    const VertexBufferLayout vbLayout{.slot = 0, .pitch = sizeof(TestVertex)};
    const std::array<VertexAttribute, 3> attrs{{
        {.location = 0, .bufferSlot = 0, .format = VertexFormat::Float3, .offset = 0},
        {.location = 1, .bufferSlot = 0, .format = VertexFormat::Float2, .offset = 12},
        {.location = 2, .bufferSlot = 0, .format = VertexFormat::Float3, .offset = 20},
    }};
    const GraphicsPipelineHandle pipeline = buildCubePipeline(*device, *renderer, vbLayout, attrs);
    CHECK(pipeline.valid());  // THE cross-backend deliverable (0.4.4's E12 inheritance on Windows/WARP)

    device->destroyGraphicsPipeline(pipeline);
    device->destroySampler(sampler);
    device->destroyTexture(tex);
    device->destroyBuffer(ibuf);
    device->destroyBuffer(vbuf);
}

TEST_CASE("render cube: C-3 draw loop, >=3 frames (AC-5)") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero cube test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }
    auto renderer = Renderer::create(*device, *window, {.depth = true});
    REQUIRE(renderer.has_value());

    const TestCube mesh = makeTestCube();
    const BufferHandle vbuf = device->createBuffer({.usage = BufferUsage::Vertex, .size = sizeof(mesh.vertices)});
    const BufferHandle ibuf = device->createBuffer({.usage = BufferUsage::Index, .size = sizeof(mesh.indices)});
    REQUIRE(vbuf.valid());
    REQUIRE(ibuf.valid());
    CHECK(device->uploadBuffer(vbuf, 0, std::as_bytes(std::span{mesh.vertices})));
    CHECK(device->uploadBuffer(ibuf, 0, std::as_bytes(std::span{mesh.indices})));

    constexpr std::uint32_t TEX_SIZE = 4;
    const std::array<std::byte, static_cast<std::size_t>(TEX_SIZE) * TEX_SIZE * 4> texels{};
    const TextureHandle tex = device->createTexture(
        {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::Sampler, .width = TEX_SIZE, .height = TEX_SIZE});
    REQUIRE(tex.valid());
    CHECK(device->uploadTexture(tex, 0, texels));
    const SamplerHandle sampler = device->createSampler({});
    REQUIRE(sampler.valid());

    const VertexBufferLayout vbLayout{.slot = 0, .pitch = sizeof(TestVertex)};
    const std::array<VertexAttribute, 3> attrs{{
        {.location = 0, .bufferSlot = 0, .format = VertexFormat::Float3, .offset = 0},
        {.location = 1, .bufferSlot = 0, .format = VertexFormat::Float2, .offset = 12},
        {.location = 2, .bufferSlot = 0, .format = VertexFormat::Float3, .offset = 20},
    }};
    const GraphicsPipelineHandle pipeline = buildCubePipeline(*device, *renderer, vbLayout, attrs);
    REQUIRE(pipeline.valid());

    const engine::Mat4 mvp = engine::Mat4::identity();
    for (int i = 0; i < 3; ++i) {
        std::optional<Frame> f = renderer->beginFrame(Color{});
        REQUIRE(f.has_value());
        CHECK(f->extent().width == static_cast<std::uint32_t>(window->pixelSize().width));
        CHECK(f->extent().height == static_cast<std::uint32_t>(window->pixelSize().height));
        CHECK(f->pass().valid());
        CHECK(f->commandBuffer().valid());

        device->pushVertexUniforms(f->commandBuffer(), 0, std::as_bytes(std::span{mvp.data(), 16}));
        device->bindGraphicsPipeline(f->pass(), pipeline);
        device->bindVertexBuffer(f->pass(), 0, vbuf);
        device->bindIndexBuffer(f->pass(), ibuf, IndexType::Uint16);
        const TextureSamplerBinding bind{.texture = tex, .sampler = sampler};
        device->bindFragmentSamplers(f->pass(), 0, std::span{&bind, 1});
        device->drawIndexed(f->pass(), 36);

        CHECK(renderer->endFrame(std::move(*f)));
    }

    device->destroyGraphicsPipeline(pipeline);
    device->destroySampler(sampler);
    device->destroyTexture(tex);
    device->destroyBuffer(ibuf);
    device->destroyBuffer(vbuf);
}

TEST_CASE("render cube: C-4 resize + depth-resize (AC-8)") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero cube test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }
    auto renderer = Renderer::create(*device, *window, {.depth = true});
    REQUIRE(renderer.has_value());

    // Run `frames` begin/end cycles; assert only survival + positive extents (safe through a resize
    // transient — a WM may clamp the requested size, R-3's precedent).
    auto survives = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            ctx.newFrame();
            engine::platform::Event ev;
            while (ctx.pollEvent(ev)) {
            }
            if (std::optional<Frame> f = renderer->beginFrame(Color{})) {
                CHECK(f->extent().width > 0U);
                CHECK(f->extent().height > 0U);
                CHECK(renderer->endFrame(std::move(*f)));
            }
            // nullopt (transient non-presentable) is acceptable — the loop must not crash.
        }
    };
    auto steadyExtentMatches = [&]() {
        ctx.newFrame();
        engine::platform::Event ev;
        while (ctx.pollEvent(ev)) {
        }
        std::optional<Frame> f = renderer->beginFrame(Color{});
        REQUIRE(f.has_value());
        CHECK(f->extent().width == static_cast<std::uint32_t>(window->pixelSize().width));
        CHECK(f->extent().height == static_cast<std::uint32_t>(window->pixelSize().height));
        CHECK(renderer->endFrame(std::move(*f)));
    };

    steadyExtentMatches();  // baseline 320x180
    survives(2);
    window->setSize(640, 360);
    survives(3);  // exercises the Renderer's lazy depth-texture recreate at the new extent
    window->setSize(320, 180);
    survives(2);
    steadyExtentMatches();  // settled back at the original size
}

TEST_CASE("render cube: C-5 resource + Renderer teardown is clean") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero cube test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }
    auto renderer = Renderer::create(*device, *window, {.depth = true});
    REQUIRE(renderer.has_value());

    const TestCube mesh = makeTestCube();
    const BufferHandle vbuf = device->createBuffer({.usage = BufferUsage::Vertex, .size = sizeof(mesh.vertices)});
    const BufferHandle ibuf = device->createBuffer({.usage = BufferUsage::Index, .size = sizeof(mesh.indices)});
    REQUIRE(vbuf.valid());
    REQUIRE(ibuf.valid());
    CHECK(device->uploadBuffer(vbuf, 0, std::as_bytes(std::span{mesh.vertices})));
    CHECK(device->uploadBuffer(ibuf, 0, std::as_bytes(std::span{mesh.indices})));

    constexpr std::uint32_t TEX_SIZE = 4;
    const std::array<std::byte, static_cast<std::size_t>(TEX_SIZE) * TEX_SIZE * 4> texels{};
    const TextureHandle tex = device->createTexture(
        {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::Sampler, .width = TEX_SIZE, .height = TEX_SIZE});
    REQUIRE(tex.valid());
    CHECK(device->uploadTexture(tex, 0, texels));
    const SamplerHandle sampler = device->createSampler({});
    REQUIRE(sampler.valid());

    const VertexBufferLayout vbLayout{.slot = 0, .pitch = sizeof(TestVertex)};
    const std::array<VertexAttribute, 3> attrs{{
        {.location = 0, .bufferSlot = 0, .format = VertexFormat::Float3, .offset = 0},
        {.location = 1, .bufferSlot = 0, .format = VertexFormat::Float2, .offset = 12},
        {.location = 2, .bufferSlot = 0, .format = VertexFormat::Float3, .offset = 20},
    }};
    const GraphicsPipelineHandle pipeline = buildCubePipeline(*device, *renderer, vbLayout, attrs);
    REQUIRE(pipeline.valid());

    {
        std::optional<Frame> f = renderer->beginFrame(Color{});
        REQUIRE(f.has_value());
        device->bindGraphicsPipeline(f->pass(), pipeline);
        device->bindVertexBuffer(f->pass(), 0, vbuf);
        device->bindIndexBuffer(f->pass(), ibuf, IndexType::Uint16);
        const TextureSamplerBinding bind{.texture = tex, .sampler = sampler};
        device->bindFragmentSamplers(f->pass(), 0, std::span{&bind, 1});
        device->drawIndexed(f->pass(), 36);
        CHECK(renderer->endFrame(std::move(*f)));
    }

    // Explicit teardown, then let renderer (incl. its depth texture) and device destruct — no crash,
    // no double-free (WARN text not asserted; absence of a crash is the check, per D10).
    device->destroyGraphicsPipeline(pipeline);
    device->destroySampler(sampler);
    device->destroyTexture(tex);
    device->destroyBuffer(ibuf);
    device->destroyBuffer(vbuf);
}

#endif  // AERO_SHADER_TOOLS_ENABLED

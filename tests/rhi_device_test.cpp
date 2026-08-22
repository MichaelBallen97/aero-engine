// Aero Engine — rhi Device tests, tiers 0-1 (task 0.4.2; AC-4, AC-5, AC-6, AC-7 minus content-proof).
//
// D9 gap list (deliberately NOT tested here, with named owners):
//   * createShader/createGraphicsPipeline SUCCESS paths and all draws — no compiled shader bytecode
//     exists until 0.4.3/0.4.4; the positive path is 0.4.4's deliverable. The E20 mismatch rejection
//     (below) IS tested here — 0.4.4 tests the acceptance side.
//   * upload CONTENT verification (no public readback path by 0.4.1 D14 design) — 0.5.2's textured
//     cube proves content; here we assert the bool result, size validation, and fence-waited
//     completion only.
//   * no garbage-bytecode createShader test: feeding random bytes to a driver without validation
//     layers is documented backend-defined-failure territory (0.4.1 §3.5) — only the engine-side
//     rejections (empty bytecode, E20 format mismatch) are tested.
#include <aero/platform/platform.hpp>
#include <aero/rhi/rhi.hpp>

#include "rhi_test_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
// <ostream> is load-bearing on MSVC: doctest stringifies std::string_view operands (backendName()
// comparisons) via the stdlib's operator<<(std::ostream&, std::string_view), which MS STL defines
// inline in <string_view> against an INCOMPLETE std::basic_ostream — only <ostream> completes it.
#include <ostream>
#include <utility>

// docs/04 forbids `using namespace` in HEADERS; this is a test TU (rhi_types_test.cpp precedent).
using namespace engine::rhi;

// -------------------------------------------------------------------------------------------
// Tier 0 — always runs, no GPU/display needed (a headless Context always works).
// -------------------------------------------------------------------------------------------

TEST_CASE("rhi device: T0-1 headless-create platform matrix (D6/E21/E22)") {
    const engine::platform::Context ctx{{.headless = true}};
    REQUIRE(ctx.valid());  // the dummy driver always works
    auto dev = Device::create();
#if defined(_WIN32)
    // D6: D3D12's PrepareDriver has no video-driver gate, so headless creation may succeed.
    if (dev.has_value()) {
        CHECK(dev->backendName() == "direct3d12");
        CHECK(dev->shaderFormat() == ShaderFormat::Dxil);
    }
#else
    // D6: Metal/Vulkan's PrepareDriver requires a real video driver — the dummy one has none.
    CHECK_FALSE(dev.has_value());
#endif
    // Either way: no crash — the graceful-failure proof.
}

// -------------------------------------------------------------------------------------------
// Tier 1 — gated: real-video Context + Device per TEST_CASE, no window.
// -------------------------------------------------------------------------------------------

TEST_CASE("rhi device: T1-1 per-OS backend/shaderFormat identity pinning (E19, C-5 evidence)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
#if defined(__APPLE__)
    CHECK(dev->backendName() == "metal");
    CHECK(dev->shaderFormat() == ShaderFormat::Msl);
#elif defined(_WIN32)
    CHECK(dev->backendName() == "direct3d12");
    CHECK(dev->shaderFormat() == ShaderFormat::Dxil);
#else
    CHECK(dev->backendName() == "vulkan");
    CHECK(dev->shaderFormat() == ShaderFormat::SpirV);
#endif
}

TEST_CASE("rhi device: T1-2 one Device per process in all configs (D12)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto first = Device::create();
    if (!first.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto second = Device::create();
    CHECK_FALSE(second.has_value());  // rejected while `first` lives — Debug AND Release
    first.reset();
    auto third = Device::create();
    CHECK(third.has_value());  // succeeds once the first has died
}

TEST_CASE("rhi device: T1-3 move semantics and moved-from inertness (E14)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto maybeDev = Device::create();
    if (!maybeDev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    Device moved = std::move(*maybeDev);
    CHECK(moved.waitIdle());

    // Moved-from is inert — never a crash.
    CHECK_FALSE(maybeDev->waitIdle());
    CHECK_FALSE(maybeDev->createBuffer({.usage = BufferUsage::Vertex, .size = 16}).valid());
    maybeDev->destroyBuffer(BufferHandle{});  // no-crash

    // Move-assign back into the moved-from shell — never two live Devices at once.
    *maybeDev = std::move(moved);
    CHECK(maybeDev->waitIdle());
}

TEST_CASE("rhi device: T1-4 supportsTextureFormat sanity (E7)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    CHECK(dev->supportsTextureFormat(TextureFormat::RGBA8Unorm, TextureUsage::Sampler));
    const bool d24 = dev->supportsTextureFormat(TextureFormat::D24Unorm, TextureUsage::DepthStencilTarget);
    const bool d32 = dev->supportsTextureFormat(TextureFormat::D32Float, TextureUsage::DepthStencilTarget);
    CHECK((d24 || d32));  // the either-or guarantee (format.hpp)
    CHECK_FALSE(dev->supportsTextureFormat(TextureFormat::Invalid, TextureUsage::Sampler));
}

TEST_CASE("rhi device: T1-5 buffer lifecycle — create/destroy/double-destroy/stale-upload") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    const BufferHandle buffer = dev->createBuffer({.usage = BufferUsage::Vertex, .size = 64});
    REQUIRE(buffer.valid());
    dev->destroyBuffer(buffer);
    dev->destroyBuffer(buffer);  // double-destroy safe, no crash
    const std::array<std::byte, 4> bytes{};
    CHECK_FALSE(dev->uploadBuffer(buffer, 0, bytes));  // stale, observable via bool
}

TEST_CASE("rhi device: T1-6 texture/sampler create + setDebugName (live and stale)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    const TextureHandle texture = dev->createTexture(
        {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::Sampler, .width = 4, .height = 4});
    REQUIRE(texture.valid());
    const SamplerHandle sampler = dev->createSampler({});
    REQUIRE(sampler.valid());
    const BufferHandle buffer = dev->createBuffer({.usage = BufferUsage::Vertex, .size = 16});
    REQUIRE(buffer.valid());

    dev->setDebugName(texture, "probe-texture");  // no-crash
    dev->setDebugName(buffer, "probe-buffer");    // no-crash

    dev->destroyTexture(texture);
    dev->destroySampler(sampler);
    dev->destroyBuffer(buffer);

    dev->setDebugName(texture, "stale-texture");  // now stale — logged no-op, no-crash
    dev->setDebugName(buffer, "stale-buffer");    // now stale — logged no-op, no-crash
}

TEST_CASE("rhi device: T1-7 the E8/D5 rejection battery — BufferDesc") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    SUBCASE("usage None is rejected") {
        CHECK_FALSE(dev->createBuffer({.usage = BufferUsage::None, .size = 64}).valid());
    }
    SUBCASE("size 0 is rejected") { CHECK_FALSE(dev->createBuffer({.usage = BufferUsage::Vertex, .size = 0}).valid()); }
}

TEST_CASE("rhi device: T1-7 the E8/D5 rejection battery — TextureDesc") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    SUBCASE("format Invalid is rejected") {
        CHECK_FALSE(dev->createTexture(
                           {.format = TextureFormat::Invalid, .usage = TextureUsage::Sampler, .width = 4, .height = 4})
                        .valid());
    }
    SUBCASE("usage None is rejected") {
        CHECK_FALSE(dev->createTexture(
                           {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::None, .width = 4, .height = 4})
                        .valid());
    }
    SUBCASE("zero width is rejected") {
        CHECK_FALSE(
            dev->createTexture(
                   {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::Sampler, .width = 0, .height = 4})
                .valid());
    }
    SUBCASE("zero height is rejected") {
        CHECK_FALSE(
            dev->createTexture(
                   {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::Sampler, .width = 4, .height = 0})
                .valid());
    }
    // Task 2.2.3 — the UPPER dimension bound is a CRASH guard, not bookkeeping. SDL_CreateGPUTexture
    // performs no 2D dimension check, so without validateDesc rejecting these first, Metal's own
    // descriptor validation aborts the PROCESS from inside it and takes the whole test binary down.
    // Removing the check in sdl_gpu_backend.cpp does not make these fail -- it makes them SIGABRT.
    SUBCASE("width beyond MAX_TEXTURE_DIMENSION_2D is rejected, not aborted") {
        CHECK_FALSE(dev->createTexture({.format = TextureFormat::RGBA8Unorm,
                                        .usage = TextureUsage::Sampler,
                                        .width = MAX_TEXTURE_DIMENSION_2D + 1,
                                        .height = 4})
                        .valid());
    }
    SUBCASE("height beyond MAX_TEXTURE_DIMENSION_2D is rejected, not aborted") {
        CHECK_FALSE(dev->createTexture({.format = TextureFormat::RGBA8Unorm,
                                        .usage = TextureUsage::Sampler,
                                        .width = 4,
                                        .height = MAX_TEXTURE_DIMENSION_2D + 1})
                        .valid());
    }
    SUBCASE("a wildly over-limit request is rejected, not aborted") {
        CHECK_FALSE(dev->createTexture({.format = TextureFormat::RGBA8Unorm,
                                        .usage = TextureUsage::Sampler,
                                        .width = 100000,
                                        .height = 100000})
                        .valid());
    }
    SUBCASE("exactly MAX_TEXTURE_DIMENSION_2D is ACCEPTED (the bound is inclusive)") {
        // Guards against a `>=` typo silently costing every caller half the usable range. Skipped
        // rather than failed where the device cannot back a 16384x1 allocation.
        const TextureHandle atLimit = dev->createTexture({.format = TextureFormat::RGBA8Unorm,
                                                          .usage = TextureUsage::Sampler,
                                                          .width = MAX_TEXTURE_DIMENSION_2D,
                                                          .height = 1});
        CHECK(atLimit.valid());
        if (atLimit.valid()) {
            dev->destroyTexture(atLimit);
        }
    }
    SUBCASE("mip 0 is rejected") {
        CHECK_FALSE(dev->createTexture({.format = TextureFormat::RGBA8Unorm,
                                        .usage = TextureUsage::Sampler,
                                        .width = 4,
                                        .height = 4,
                                        .mipLevels = 0})
                        .valid());
    }
    SUBCASE("mip beyond the cap for 4x4 (cap 3) is rejected") {
        CHECK_FALSE(dev->createTexture({.format = TextureFormat::RGBA8Unorm,
                                        .usage = TextureUsage::Sampler,
                                        .width = 4,
                                        .height = 4,
                                        .mipLevels = 4})
                        .valid());
    }
    SUBCASE("MSAA with mipLevels > 1 is rejected (E11)") {
        CHECK_FALSE(dev->createTexture({.format = TextureFormat::RGBA8Unorm,
                                        .usage = TextureUsage::ColorTarget,
                                        .width = 4,
                                        .height = 4,
                                        .mipLevels = 2,
                                        .sampleCount = SampleCount::Four})
                        .valid());
    }
    SUBCASE("MSAA without a target usage is rejected (E11)") {
        CHECK_FALSE(dev->createTexture({.format = TextureFormat::RGBA8Unorm,
                                        .usage = TextureUsage::Sampler,
                                        .width = 4,
                                        .height = 4,
                                        .sampleCount = SampleCount::Four})
                        .valid());
    }
    SUBCASE("a depth format with ColorTarget usage is rejected") {
        CHECK_FALSE(
            dev->createTexture(
                   {.format = TextureFormat::D16Unorm, .usage = TextureUsage::ColorTarget, .width = 4, .height = 4})
                .valid());
    }
    SUBCASE("a color format with DepthStencilTarget usage is rejected") {
        CHECK_FALSE(dev->createTexture({.format = TextureFormat::RGBA8Unorm,
                                        .usage = TextureUsage::DepthStencilTarget,
                                        .width = 4,
                                        .height = 4})
                        .valid());
    }
    SUBCASE("depth + Sampler is ACCEPTED (shadow-map combo, not impossible)") {
        CHECK(dev->createTexture({.format = TextureFormat::D16Unorm,
                                  .usage = TextureUsage::DepthStencilTarget | TextureUsage::Sampler,
                                  .width = 4,
                                  .height = 4})
                  .valid());
    }
    // Task 3.4.1 (D3) — a block-compressed TOP level must be block-aligned on EVERY backend,
    // including the two (Vulkan, Metal) that would have accepted it. The rule is engine-owned and
    // uniform rather than per-OS, so these subcases assert the same verdict on all three lanes.
    SUBCASE("a 5x3 BC5 top level is rejected (the committed cooked golden's own shape)") {
        CHECK_FALSE(
            dev->createTexture(
                   {.format = TextureFormat::BC5RGUnorm, .usage = TextureUsage::Sampler, .width = 5, .height = 3})
                .valid());
    }
    SUBCASE("a 2x2 BC3 top level is rejected (a legal cooked artifact, not a legal texture)") {
        CHECK_FALSE(
            dev->createTexture(
                   {.format = TextureFormat::BC3RGBAUnormSrgb, .usage = TextureUsage::Sampler, .width = 2, .height = 2})
                .valid());
    }
    SUBCASE("one unaligned axis is enough to reject") {
        CHECK_FALSE(
            dev->createTexture(
                   {.format = TextureFormat::BC1RGBAUnorm, .usage = TextureUsage::Sampler, .width = 8, .height = 6})
                .valid());
        CHECK_FALSE(
            dev->createTexture(
                   {.format = TextureFormat::BC1RGBAUnorm, .usage = TextureUsage::Sampler, .width = 6, .height = 8})
                .valid());
    }
    SUBCASE("a 4x4 BC1 top level is ACCEPTED (the contrast that keeps the refusals honest)") {
        const TextureHandle aligned = dev->createTexture(
            {.format = TextureFormat::BC1RGBAUnorm, .usage = TextureUsage::Sampler, .width = 4, .height = 4});
        CHECK(aligned.valid());
        if (aligned.valid()) {
            dev->destroyTexture(aligned);
        }
    }
    SUBCASE("an 8x8 BC1 with the FULL 4-level chain is ACCEPTED — sub-top levels are exempt") {
        // The whole point of "top level" in the rule: levels 2 and 3 of this chain are 2x2 and 1x1,
        // neither block-aligned. Applying the check per mip level would refuse this create outright.
        const TextureHandle chained = dev->createTexture({.format = TextureFormat::BC1RGBAUnorm,
                                                          .usage = TextureUsage::Sampler,
                                                          .width = 8,
                                                          .height = 8,
                                                          .mipLevels = 4});
        CHECK(chained.valid());
        if (chained.valid()) {
            dev->destroyTexture(chained);
        }
    }
}

TEST_CASE("rhi device: T1-7 the E8/D5 rejection battery — SamplerDesc") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    SUBCASE("maxLod < minLod is rejected") {
        CHECK_FALSE(dev->createSampler({.minLod = 5.0F, .maxLod = 1.0F}).valid());
    }
    SUBCASE("enableAnisotropy with maxAnisotropy < 1 is rejected") {
        CHECK_FALSE(dev->createSampler({.enableAnisotropy = true, .maxAnisotropy = 0.5F}).valid());
    }
}

TEST_CASE("rhi device: T1-7 the E8/D5 rejection battery — ShaderDesc") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    SUBCASE("empty bytecode is rejected") { CHECK_FALSE(dev->createShader({.format = dev->shaderFormat()}).valid()); }
    SUBCASE("wrong format is rejected before any SDL call (E20)") {
        const ShaderFormat wrongFormat =
            dev->shaderFormat() == ShaderFormat::SpirV ? ShaderFormat::Msl : ShaderFormat::SpirV;
        const std::array<std::byte, 4> dummyBytecode{};
        CHECK_FALSE(dev->createShader({.format = wrongFormat, .bytecode = dummyBytecode}).valid());
    }
}

TEST_CASE("rhi device: T1-7 the E8/D5 rejection battery — GraphicsPipelineDesc (structure-first, C-6)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    SUBCASE("empty colorTargets is rejected") { CHECK_FALSE(dev->createGraphicsPipeline({}).valid()); }
    SUBCASE("more than MAX_COLOR_ATTACHMENTS targets is rejected") {
        std::array<ColorTargetDesc, MAX_COLOR_ATTACHMENTS + 1> targets{};
        for (auto& target : targets) {
            target.format = TextureFormat::RGBA8Unorm;
        }
        CHECK_FALSE(dev->createGraphicsPipeline({.colorTargets = targets}).valid());
    }
    SUBCASE("a colorTargets entry with an Invalid format is rejected") {
        const std::array<ColorTargetDesc, 1> targets{ColorTargetDesc{.format = TextureFormat::Invalid}};
        CHECK_FALSE(dev->createGraphicsPipeline({.colorTargets = targets}).valid());
    }
    SUBCASE("more than MAX_VERTEX_BUFFER_SLOTS layouts is rejected") {
        const std::array<ColorTargetDesc, 1> targets{ColorTargetDesc{.format = TextureFormat::RGBA8Unorm}};
        std::array<VertexBufferLayout, MAX_VERTEX_BUFFER_SLOTS + 1> layouts{};
        for (std::size_t i = 0; i < layouts.size(); ++i) {
            layouts[i] = VertexBufferLayout{.slot = static_cast<std::uint32_t>(i), .pitch = 12};
        }
        CHECK_FALSE(dev->createGraphicsPipeline({.vertexBuffers = layouts, .colorTargets = targets}).valid());
    }
    SUBCASE("more than MAX_VERTEX_ATTRIBUTES attributes is rejected") {
        const std::array<ColorTargetDesc, 1> targets{ColorTargetDesc{.format = TextureFormat::RGBA8Unorm}};
        const std::array<VertexBufferLayout, 1> layouts{VertexBufferLayout{.slot = 0, .pitch = 12}};
        std::array<VertexAttribute, MAX_VERTEX_ATTRIBUTES + 1> attributes{};
        for (std::size_t i = 0; i < attributes.size(); ++i) {
            attributes[i] = VertexAttribute{.location = static_cast<std::uint32_t>(i), .bufferSlot = 0};
        }
        CHECK_FALSE(dev->createGraphicsPipeline(
                           {.vertexBuffers = layouts, .vertexAttributes = attributes, .colorTargets = targets})
                        .valid());
    }
    SUBCASE("an attribute referencing an undeclared bufferSlot is rejected") {
        const std::array<ColorTargetDesc, 1> targets{ColorTargetDesc{.format = TextureFormat::RGBA8Unorm}};
        const std::array<VertexAttribute, 1> attributes{VertexAttribute{.location = 0, .bufferSlot = 0}};
        CHECK_FALSE(dev->createGraphicsPipeline({.vertexAttributes = attributes, .colorTargets = targets}).valid());
    }
    SUBCASE("duplicate attribute locations are rejected") {
        const std::array<ColorTargetDesc, 1> targets{ColorTargetDesc{.format = TextureFormat::RGBA8Unorm}};
        const std::array<VertexBufferLayout, 1> layouts{VertexBufferLayout{.slot = 0, .pitch = 12}};
        const std::array<VertexAttribute, 2> attributes{VertexAttribute{.location = 0, .bufferSlot = 0},
                                                        VertexAttribute{.location = 0, .bufferSlot = 0, .offset = 4}};
        CHECK_FALSE(dev->createGraphicsPipeline(
                           {.vertexBuffers = layouts, .vertexAttributes = attributes, .colorTargets = targets})
                        .valid());
    }
    SUBCASE("pitch == 0 on a referenced layout is rejected") {
        const std::array<ColorTargetDesc, 1> targets{ColorTargetDesc{.format = TextureFormat::RGBA8Unorm}};
        const std::array<VertexBufferLayout, 1> layouts{VertexBufferLayout{.slot = 0, .pitch = 0}};
        const std::array<VertexAttribute, 1> attributes{VertexAttribute{.location = 0, .bufferSlot = 0}};
        CHECK_FALSE(dev->createGraphicsPipeline(
                           {.vertexBuffers = layouts, .vertexAttributes = attributes, .colorTargets = targets})
                        .valid());
    }
    SUBCASE("invalid/stale shader handles with otherwise-valid structure are rejected") {
        const std::array<ColorTargetDesc, 1> targets{ColorTargetDesc{.format = TextureFormat::RGBA8Unorm}};
        CHECK_FALSE(dev->createGraphicsPipeline({.colorTargets = targets}).valid());
    }
}

TEST_CASE("rhi device: neither a colour target nor a depth target is refused (SW1)") {
    // Task 3.6.2's widening keeps an `iff`, and this is the half of it no other case can see. A
    // DEPTH-ONLY pipeline (zero colour targets, a real depthStencilFormat) is now legal; one with
    // NEITHER must still be refused, and refused HERE rather than by SDL, whose own guard for it is
    // an SDL_assert_release -- an ABORT, not a NULL.
    //
    // Structure is validated BEFORE handle liveness (C-6), so never-valid shader handles are enough
    // and this needs no shader artifacts at all.
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    SUBCASE("createGraphicsPipeline: no colour target and no depth format") {
        GraphicsPipelineDesc desc;
        desc.colorTargets = {};
        desc.depthStencilFormat = TextureFormat::Invalid;
        CHECK_FALSE(dev->createGraphicsPipeline(desc).valid());
        // ...and the same desc WITH a depth format gets past the structural arm and fails later, on
        // the never-valid shader handles. Both return an invalid handle, so this line alone cannot
        // discriminate -- what brackets the `iff` from the other side is the render tier's SM1, which
        // builds two REAL depth-only pipelines on a real device and succeeds.
        desc.depthStencilFormat = TextureFormat::D32Float;
        CHECK_FALSE(dev->createGraphicsPipeline(desc).valid());
    }

    SUBCASE("beginRenderPass: no colour attachment and no depth attachment") {
        // The pass half, and unlike the pipeline half it IS distinguishable: with a real depth
        // attachment the very same shape returns a VALID handle, which is what makes the refusal
        // below a statement about "neither" rather than about "empty colour".
        const CommandBufferHandle neither = dev->acquireCommandBuffer();
        REQUIRE(neither.valid());
        CHECK_FALSE(dev->beginRenderPass(neither, {}).valid());
        dev->cancel(neither);

        TextureFormat depthFormat = TextureFormat::Invalid;
        for (const TextureFormat candidate :
             {TextureFormat::D32Float, TextureFormat::D24Unorm, TextureFormat::D16Unorm}) {
            if (dev->supportsTextureFormat(candidate, TextureUsage::DepthStencilTarget)) {
                depthFormat = candidate;
                break;
            }
        }
        // Double parentheses: engine::rhi::toString(TextureFormat) is found by ADL and beats
        // doctest's own stringifier, whose decomposer then tries std::string_view + const char*.
        // The standing trap (.claude/rules/ci-portability.md); rhi_format_test.cpp's own fix.
        REQUIRE((depthFormat != TextureFormat::Invalid));
        const TextureHandle depth = dev->createTexture(
            {.format = depthFormat, .usage = TextureUsage::DepthStencilTarget, .width = 8, .height = 8});
        REQUIRE(depth.valid());
        const CommandBufferHandle cmd = dev->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        const DepthStencilAttachment attachment{.texture = depth};
        const RenderPassHandle pass = dev->beginRenderPass(cmd, {.depthStencil = attachment});
        CHECK(pass.valid());  // a DEPTH-ONLY pass: this is what the widening exists for
        if (pass.valid()) {
            dev->endRenderPass(pass);
        }
        dev->cancel(cmd);
        dev->destroyTexture(depth);
    }
}

TEST_CASE("rhi device: T1-8 upload happy paths and rejections (AC-7)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    SUBCASE("buffer full write") {
        const BufferHandle buffer = dev->createBuffer({.usage = BufferUsage::Vertex, .size = 64});
        REQUIRE(buffer.valid());
        const std::array<std::byte, 64> data{};
        CHECK(dev->uploadBuffer(buffer, 0, data));
        dev->destroyBuffer(buffer);
    }
    SUBCASE("buffer partial write at offset 32") {
        const BufferHandle buffer = dev->createBuffer({.usage = BufferUsage::Vertex, .size = 64});
        REQUIRE(buffer.valid());
        const std::array<std::byte, 32> data{};
        CHECK(dev->uploadBuffer(buffer, 32, data));
        dev->destroyBuffer(buffer);
    }
    SUBCASE("texture RGBA8 4x4 mip0 = 64B") {
        const TextureHandle texture = dev->createTexture(
            {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::Sampler, .width = 4, .height = 4});
        REQUIRE(texture.valid());
        const std::array<std::byte, 64> data{};
        CHECK(dev->uploadTexture(texture, 0, data));
        dev->destroyTexture(texture);
    }
    SUBCASE("mips=3 texture, mip1 = 2x2x4 = 16B (E10 arithmetic)") {
        const TextureHandle texture = dev->createTexture({.format = TextureFormat::RGBA8Unorm,
                                                          .usage = TextureUsage::Sampler,
                                                          .width = 4,
                                                          .height = 4,
                                                          .mipLevels = 3});
        REQUIRE(texture.valid());
        const std::array<std::byte, 16> data{};
        CHECK(dev->uploadTexture(texture, 1, data));
        dev->destroyTexture(texture);
    }
    SUBCASE("buffer offset+size overflow rejected") {
        const BufferHandle buffer = dev->createBuffer({.usage = BufferUsage::Vertex, .size = 64});
        REQUIRE(buffer.valid());
        const std::array<std::byte, 32> data{};
        CHECK_FALSE(dev->uploadBuffer(buffer, 48, data));
        dev->destroyBuffer(buffer);
    }
    SUBCASE("wrong texture byte count rejected") {
        const TextureHandle texture = dev->createTexture(
            {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::Sampler, .width = 4, .height = 4});
        REQUIRE(texture.valid());
        const std::array<std::byte, 63> data{};
        CHECK_FALSE(dev->uploadTexture(texture, 0, data));
        dev->destroyTexture(texture);
    }
    SUBCASE("mipLevel >= mipLevels rejected") {
        const TextureHandle texture = dev->createTexture(
            {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::Sampler, .width = 4, .height = 4});
        REQUIRE(texture.valid());
        const std::array<std::byte, 64> data{};
        CHECK_FALSE(dev->uploadTexture(texture, 1, data));
        dev->destroyTexture(texture);
    }
    SUBCASE("depth-format target rejected") {
        // The 0-return path is doubly load-bearing since task 3.4.1: texelBlockSize answers 0 (the
        // guard that fires here) and textureLevelByteSize answers 0 for the same formats, so a
        // depth upload can never accidentally match an expected size of 0 either.
        const TextureHandle texture = dev->createTexture(
            {.format = TextureFormat::D16Unorm, .usage = TextureUsage::DepthStencilTarget, .width = 4, .height = 4});
        REQUIRE(texture.valid());
        const std::array<std::byte, 64> data{};
        CHECK_FALSE(dev->uploadTexture(texture, 0, data));
        dev->destroyTexture(texture);

        const TextureHandle depth32 = dev->createTexture(
            {.format = TextureFormat::D32Float, .usage = TextureUsage::DepthStencilTarget, .width = 4, .height = 4});
        if (depth32.valid()) {  // D24 or D32 is guaranteed, not both (E7) — skip the one this GPU lacks
            const std::array<std::byte, 64> depthData{};
            CHECK_FALSE(dev->uploadTexture(depth32, 0, depthData));
            dev->destroyTexture(depth32);
        }
    }
    // --- task 3.4.1: the block-unit upload contract, on a real GPU ------------------------------
    SUBCASE("a 4x4 BC1 mip0 is exactly ONE 8-byte block") {
        const TextureHandle texture = dev->createTexture(
            {.format = TextureFormat::BC1RGBAUnorm, .usage = TextureUsage::Sampler, .width = 4, .height = 4});
        REQUIRE(texture.valid());
        const std::array<std::byte, 8> block{};
        CHECK(dev->uploadTexture(texture, 0, block));
        dev->destroyTexture(texture);
    }
    SUBCASE("the PER-TEXEL byte count is rejected for a BC format (32B for a 4x4 BC1)") {
        // The regression this whole contract change exists to prevent: 4 * 4 * 2 (or any per-texel
        // reading) is not a legal BC1 level size. 32 is what the pre-3.4.1 formula would produce if
        // texelBlockSize were still read as bytes per texel with BC1's value of 8.
        const TextureHandle texture = dev->createTexture(
            {.format = TextureFormat::BC1RGBAUnorm, .usage = TextureUsage::Sampler, .width = 4, .height = 4});
        REQUIRE(texture.valid());
        const std::array<std::byte, 32> perTexel{};
        CHECK_FALSE(dev->uploadTexture(texture, 0, perTexel));
        const std::array<std::byte, 128> wayTooBig{};
        CHECK_FALSE(dev->uploadTexture(texture, 0, wayTooBig));
        dev->destroyTexture(texture);
    }
    SUBCASE("the BC mip tail uploads as ONE block at 2x2 and at 1x1") {
        const TextureHandle texture = dev->createTexture({.format = TextureFormat::BC1RGBAUnorm,
                                                          .usage = TextureUsage::Sampler,
                                                          .width = 8,
                                                          .height = 8,
                                                          .mipLevels = 4});
        REQUIRE(texture.valid());
        const std::array<std::byte, 32> level0{};  // 2x2 blocks
        const std::array<std::byte, 8> level1{};   // 4x4 -> 1x1 blocks
        const std::array<std::byte, 8> level2{};   // 2x2 -> one partial block
        const std::array<std::byte, 8> level3{};   // 1x1 -> one partial block
        CHECK(dev->uploadTexture(texture, 0, level0));
        CHECK(dev->uploadTexture(texture, 1, level1));
        CHECK(dev->uploadTexture(texture, 2, level2));
        CHECK(dev->uploadTexture(texture, 3, level3));
        dev->destroyTexture(texture);
    }
    SUBCASE("the 16-byte block families upload 16 bytes for a 4x4 level, and 8 is rejected") {
        const std::array<TextureFormat, 2> wide{TextureFormat::BC3RGBAUnorm, TextureFormat::BC5RGUnorm};
        std::size_t checked = 0;
        for (const TextureFormat format : wide) {
            INFO("format ", toString(format));
            const TextureHandle texture =
                dev->createTexture({.format = format, .usage = TextureUsage::Sampler, .width = 4, .height = 4});
            REQUIRE(texture.valid());
            const std::array<std::byte, 16> block{};
            CHECK(dev->uploadTexture(texture, 0, block));
            const std::array<std::byte, 8> halfBlock{};
            CHECK_FALSE(dev->uploadTexture(texture, 0, halfBlock));
            dev->destroyTexture(texture);
            ++checked;
        }
        CHECK(checked == 2);  // LITERAL: proves the loop ran
    }
    SUBCASE("BC4 is an 8-byte block family, and 16 is rejected") {
        const TextureHandle texture = dev->createTexture(
            {.format = TextureFormat::BC4RUnorm, .usage = TextureUsage::Sampler, .width = 4, .height = 4});
        REQUIRE(texture.valid());
        const std::array<std::byte, 8> block{};
        CHECK(dev->uploadTexture(texture, 0, block));
        const std::array<std::byte, 16> doubleBlock{};
        CHECK_FALSE(dev->uploadTexture(texture, 0, doubleBlock));
        dev->destroyTexture(texture);
    }
    SUBCASE("stale handles rejected") {
        const std::array<std::byte, 4> data{};
        CHECK_FALSE(dev->uploadBuffer(BufferHandle{}, 0, data));
        CHECK_FALSE(dev->uploadTexture(TextureHandle{}, 0, data));
    }
    SUBCASE("empty data span rejected") {
        const BufferHandle buffer = dev->createBuffer({.usage = BufferUsage::Vertex, .size = 64});
        REQUIRE(buffer.valid());
        CHECK_FALSE(dev->uploadBuffer(buffer, 0, {}));
        dev->destroyBuffer(buffer);
    }
}

TEST_CASE("rhi device: T1-9 command buffers — acquire/submit/cancel/multiple (E18)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    const CommandBufferHandle cmd1 = dev->acquireCommandBuffer();
    REQUIRE(cmd1.valid());
    CHECK(dev->submit(cmd1));
    CHECK_FALSE(dev->submit(cmd1));  // E3: stale now

    const CommandBufferHandle cmd2 = dev->acquireCommandBuffer();
    REQUIRE(cmd2.valid());
    CHECK(dev->cancel(cmd2));
    CHECK_FALSE(dev->cancel(cmd2));  // stale now

    const CommandBufferHandle a = dev->acquireCommandBuffer();
    const CommandBufferHandle b = dev->acquireCommandBuffer();
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    CHECK(dev->submit(a));
    CHECK(dev->submit(b));
}

TEST_CASE("rhi device: T1-10 offscreen render pass records without a real window (AC-6's heart)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    const TextureHandle colorTarget = dev->createTexture(
        {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::ColorTarget, .width = 64, .height = 64});
    REQUIRE(colorTarget.valid());
    const BufferHandle vertexBuffer = dev->createBuffer({.usage = BufferUsage::Vertex, .size = 64});
    REQUIRE(vertexBuffer.valid());
    const BufferHandle indexBuffer = dev->createBuffer({.usage = BufferUsage::Index, .size = 64});
    REQUIRE(indexBuffer.valid());

    const CommandBufferHandle cmd = dev->acquireCommandBuffer();
    REQUIRE(cmd.valid());
    const ColorAttachment colorAttachment{.texture = colorTarget};
    const RenderPassHandle pass = dev->beginRenderPass(cmd, {.colorAttachments = {&colorAttachment, 1}});
    REQUIRE(pass.valid());

    dev->setViewport(pass, {});
    dev->setScissor(pass, {});
    dev->draw(pass, 3);  // no pipeline bound -> logged no-op, no crash
    dev->bindVertexBuffer(pass, 0, vertexBuffer);
    dev->bindIndexBuffer(pass, indexBuffer, IndexType::Uint16);
    dev->bindVertexBuffer(pass, 0, indexBuffer);  // wrong usage -> logged no-op, no crash

    const std::array<TextureSamplerBinding, 1> oobBindings{};
    dev->bindFragmentSamplers(pass, MAX_FRAGMENT_SAMPLER_SLOTS, oobBindings);  // out of range -> no-op

    dev->endRenderPass(pass);
    CHECK(dev->submit(cmd));

    dev->destroyBuffer(vertexBuffer);
    dev->destroyBuffer(indexBuffer);
    dev->destroyTexture(colorTarget);
}

TEST_CASE("rhi device: T1-11 E4 a second beginRenderPass on an open pass is rejected") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    const TextureHandle colorTarget = dev->createTexture(
        {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::ColorTarget, .width = 4, .height = 4});
    REQUIRE(colorTarget.valid());
    const CommandBufferHandle cmd = dev->acquireCommandBuffer();
    REQUIRE(cmd.valid());
    const ColorAttachment attachment{.texture = colorTarget};
    const RenderPassHandle first = dev->beginRenderPass(cmd, {.colorAttachments = {&attachment, 1}});
    REQUIRE(first.valid());
    const RenderPassHandle second = dev->beginRenderPass(cmd, {.colorAttachments = {&attachment, 1}});
    CHECK_FALSE(second.valid());  // the first pass is unaffected
    dev->endRenderPass(first);
    CHECK(dev->submit(cmd));
    dev->destroyTexture(colorTarget);
}

TEST_CASE("rhi device: T1-12 E5 an open pass at submit is force-ended, submit still returns its real result") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    const TextureHandle colorTarget = dev->createTexture(
        {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::ColorTarget, .width = 4, .height = 4});
    REQUIRE(colorTarget.valid());
    const CommandBufferHandle cmd = dev->acquireCommandBuffer();
    REQUIRE(cmd.valid());
    const ColorAttachment attachment{.texture = colorTarget};
    const RenderPassHandle pass = dev->beginRenderPass(cmd, {.colorAttachments = {&attachment, 1}});
    REQUIRE(pass.valid());
    // Do NOT end the pass — submit() must force-end it and still submit.
    CHECK(dev->submit(cmd));
    CHECK_FALSE(dev->submit(cmd));                                                           // cmd is stale now
    CHECK_FALSE(dev->beginRenderPass(cmd, {.colorAttachments = {&attachment, 1}}).valid());  // stale cmd
    dev->destroyTexture(colorTarget);
}

TEST_CASE("rhi device: T1-13 stale render-pass handles are logged no-ops after an E5 force-end") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    const TextureHandle colorTarget = dev->createTexture(
        {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::ColorTarget, .width = 4, .height = 4});
    REQUIRE(colorTarget.valid());
    const CommandBufferHandle cmd = dev->acquireCommandBuffer();
    REQUIRE(cmd.valid());
    const ColorAttachment attachment{.texture = colorTarget};
    const RenderPassHandle pass = dev->beginRenderPass(cmd, {.colorAttachments = {&attachment, 1}});
    REQUIRE(pass.valid());
    CHECK(dev->submit(cmd));  // force-ends `pass`, invalidates it

    // `pass` is now stale — every call below must be a no-crash logged no-op (C-1: no assert(false)).
    dev->endRenderPass(pass);
    dev->bindGraphicsPipeline(pass, GraphicsPipelineHandle{});
    dev->setViewport(pass, {});

    dev->destroyTexture(colorTarget);
}

TEST_CASE("rhi device: T1-14 push uniforms — in/out of a pass, slot range, empty data, stale cmd") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    const TextureHandle colorTarget = dev->createTexture(
        {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::ColorTarget, .width = 4, .height = 4});
    REQUIRE(colorTarget.valid());
    const CommandBufferHandle cmd = dev->acquireCommandBuffer();
    REQUIRE(cmd.valid());
    const std::array<std::byte, 16> data{};  // 16 aligned bytes

    dev->pushVertexUniforms(cmd, 0, data);  // outside any pass — no crash

    const ColorAttachment attachment{.texture = colorTarget};
    const RenderPassHandle pass = dev->beginRenderPass(cmd, {.colorAttachments = {&attachment, 1}});
    REQUIRE(pass.valid());
    dev->pushFragmentUniforms(cmd, 0, data);                     // callable during a pass too
    dev->pushVertexUniforms(cmd, MAX_PUSH_UNIFORM_SLOTS, data);  // slot out of range -> no-op
    dev->pushVertexUniforms(cmd, 0, {});                         // empty data -> no-op
    dev->endRenderPass(pass);
    CHECK(dev->submit(cmd));

    dev->pushVertexUniforms(cmd, 0, data);  // stale cmd -> no-op
    dev->destroyTexture(colorTarget);
}

TEST_CASE("rhi device: T1-15 render-pass attachment rejections") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    SUBCASE("a stale texture in a color attachment is rejected") {
        const CommandBufferHandle cmd = dev->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        const ColorAttachment attachment{.texture = TextureHandle{}};
        CHECK_FALSE(dev->beginRenderPass(cmd, {.colorAttachments = {&attachment, 1}}).valid());
        dev->cancel(cmd);
    }
    SUBCASE("a Sampler-only texture as a color target is rejected") {
        const TextureHandle samplerOnly = dev->createTexture(
            {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::Sampler, .width = 4, .height = 4});
        REQUIRE(samplerOnly.valid());
        const CommandBufferHandle cmd = dev->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        const ColorAttachment attachment{.texture = samplerOnly};
        CHECK_FALSE(dev->beginRenderPass(cmd, {.colorAttachments = {&attachment, 1}}).valid());
        dev->cancel(cmd);
        dev->destroyTexture(samplerOnly);
    }
    SUBCASE("a depth attachment texture without DepthStencilTarget usage is rejected") {
        const TextureHandle colorTarget = dev->createTexture(
            {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::ColorTarget, .width = 4, .height = 4});
        const TextureHandle notDepth = dev->createTexture(
            {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::ColorTarget, .width = 4, .height = 4});
        REQUIRE(colorTarget.valid());
        REQUIRE(notDepth.valid());
        const CommandBufferHandle cmd = dev->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        const ColorAttachment colorAttachment{.texture = colorTarget};
        const DepthStencilAttachment depthAttachment{.texture = notDepth};
        CHECK_FALSE(
            dev->beginRenderPass(cmd, {.colorAttachments = {&colorAttachment, 1}, .depthStencil = depthAttachment})
                .valid());
        dev->cancel(cmd);
        dev->destroyTexture(colorTarget);
        dev->destroyTexture(notDepth);
    }
    SUBCASE("two color targets with different extents are rejected") {
        const TextureHandle a = dev->createTexture(
            {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::ColorTarget, .width = 4, .height = 4});
        const TextureHandle b = dev->createTexture(
            {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::ColorTarget, .width = 8, .height = 8});
        REQUIRE(a.valid());
        REQUIRE(b.valid());
        const CommandBufferHandle cmd = dev->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        const std::array<ColorAttachment, 2> attachments{ColorAttachment{.texture = a}, ColorAttachment{.texture = b}};
        CHECK_FALSE(dev->beginRenderPass(cmd, {.colorAttachments = attachments}).valid());
        dev->cancel(cmd);
        dev->destroyTexture(a);
        dev->destroyTexture(b);
    }
    SUBCASE("mixed sample counts are rejected") {
        const TextureHandle a = dev->createTexture({.format = TextureFormat::RGBA8Unorm,
                                                    .usage = TextureUsage::ColorTarget,
                                                    .width = 4,
                                                    .height = 4,
                                                    .sampleCount = SampleCount::One});
        const TextureHandle b = dev->createTexture({.format = TextureFormat::RGBA8Unorm,
                                                    .usage = TextureUsage::ColorTarget,
                                                    .width = 4,
                                                    .height = 4,
                                                    .sampleCount = SampleCount::Four});
        REQUIRE(a.valid());
        REQUIRE(b.valid());
        const CommandBufferHandle cmd = dev->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        const std::array<ColorAttachment, 2> attachments{ColorAttachment{.texture = a}, ColorAttachment{.texture = b}};
        CHECK_FALSE(dev->beginRenderPass(cmd, {.colorAttachments = attachments}).valid());
        dev->cancel(cmd);
        dev->destroyTexture(a);
        dev->destroyTexture(b);
    }
    SUBCASE("zero color attachments is rejected") {
        const CommandBufferHandle cmd = dev->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        CHECK_FALSE(dev->beginRenderPass(cmd, {}).valid());
        dev->cancel(cmd);
    }
}

TEST_CASE("rhi device: T1-16 acquireSwapchainTexture on a never-valid swapchain handle") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    const CommandBufferHandle cmd = dev->acquireCommandBuffer();
    REQUIRE(cmd.valid());
    CHECK_FALSE(dev->acquireSwapchainTexture(cmd, SwapchainHandle{}).has_value());
    dev->cancel(cmd);
}

TEST_CASE("rhi device: T1-17 dropping the Device with live resources and an un-submitted cmd does not crash") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    {
        auto dev = Device::create();
        if (!dev.has_value()) {
            AERO_SKIP_OR_FAIL("no GPU device available");
        }
        const BufferHandle buffer = dev->createBuffer({.usage = BufferUsage::Vertex, .size = 64});
        REQUIRE(buffer.valid());
        const TextureHandle texture = dev->createTexture(
            {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::Sampler, .width = 4, .height = 4});
        REQUIRE(texture.valid());
        const CommandBufferHandle cmd = dev->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        // `dev` goes out of scope here WITHOUT destroying buffer/texture or submitting/cancelling cmd.
    }
    // No crash, no sanitizer report — the WARN/ERROR text is log-side, not asserted (E12/E23).
}

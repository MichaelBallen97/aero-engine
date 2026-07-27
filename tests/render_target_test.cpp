// Aero Engine — render::RenderTarget tests (task 2.2.3). Tier-0 (this section, no GPU, always
// compiled): nextTargetExtent is a pure, GPU-free, total sizing policy — the framePaceSleepMs
// precedent. Tier-2 (below): GPU-gated RenderTarget lifecycle/resize/draw cases — a real-video
// Context + Device, NO window and NO swapchain (RenderTarget is a general engine type, AC-13).
// Declaration order per case: ctx -> device -> target (the render_clear_test.cpp C3 rule).
#include <aero/platform/platform.hpp>
#include <aero/render/render_target.hpp>
#include <aero/rhi/rhi.hpp>

#include "rhi_test_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
// <ostream> is load-bearing on MSVC for enum/string_view CHECKs (see rhi_device_test.cpp's comment).
#include <optional>
#include <ostream>
#include <utility>
#include <vector>

using engine::render::nextTargetExtent;
using engine::render::RENDER_TARGET_MAX_EXTENT;
using engine::render::RenderTarget;
using engine::render::RenderTargetConfig;
using namespace engine::rhi;

namespace {

struct SizingCase {
    Extent2D requested;
    Extent2D current;
    std::uint32_t quantum;
    std::uint32_t maxExtent;
    Extent2D expected;
};

}  // namespace

TEST_CASE("render target: nextTargetExtent is total, quantised and hysteretic (tier-0, no GPU)") {
    constexpr std::uint32_t MAX = RENDER_TARGET_MAX_EXTENT;
    constexpr std::uint32_t U32_MAX = 4294967295U;
    // clang-format off
    const std::array<SizingCase, 13> cases{{
        // requested       current        quantum  maxExtent  expected         // why
        {{100, 100},       {0, 0},        64,      MAX,       {128, 128}},     // 1: first allocation
        {{1280, 720},      {0, 0},        1,       MAX,       {1280, 720}},    // 2: q==1 is exact (D6)
        {{1300, 720},      {1280, 768},   64,      MAX,       {1344, 768}},    // 3: x grows, y kept
        {{1200, 700},      {1280, 768},   64,      MAX,       {1280, 768}},    // 4: keep (AC-6)
        {{640, 384},       {1280, 768},   64,      MAX,       {640, 384}},     // 5: shrink at half
        {{641, 385},       {1280, 768},   64,      MAX,       {1280, 768}},    // 6: no shrink above half
        {{200, 2000},      {1280, 768},   64,      MAX,       {256, 2048}},    // 7: shrink+grow, per axis
        {{0, 0},           {0, 0},        64,      MAX,       {64, 64}},       // 8: zero clamped to >=1
        {{9000, 9000},     {0, 0},        64,      8192,      {8192, 8192}},   // 9: clamp to max
        {{8192, 8192},     {0, 0},        100,     8192,      {8192, 8192}},   // 10: E18 clamp AFTER round-up
        {{100, 100},       {0, 0},        0,       MAX,       {100, 100}},     // 11: E17 no divide-by-zero
        {{64, 64},         {0, 0},        64,      64,        {64, 64}},       // 12: maxExtent == quantum
        {{U32_MAX, U32_MAX}, {0, 0},      64,      U32_MAX,   {U32_MAX, U32_MAX}}, // 13: C2 64-bit round-up
    }};
    // clang-format on

    for (const SizingCase& c : cases) {
        const Extent2D result = nextTargetExtent(c.requested, c.current, c.quantum, c.maxExtent);
        CHECK(result.width == c.expected.width);
        CHECK(result.height == c.expected.height);
    }
}

TEST_CASE("render target: nextTargetExtent postcondition — result >= clamp(req,1,max) (AC-14)") {
    const std::array<std::uint32_t, 5> currents{0, 64, 640, 1280, 4096};
    const std::array<std::uint32_t, 3> quanta{1, 16, 64};

    for (const std::uint32_t current : currents) {
        for (const std::uint32_t quantum : quanta) {
            for (std::uint32_t req = 0; req <= 2048; req += 7) {
                const std::uint32_t maxExtent = RENDER_TARGET_MAX_EXTENT;
                const Extent2D result = nextTargetExtent({req, req}, {current, current}, quantum, maxExtent);
                const std::uint32_t clampedReq = req < 1 ? 1 : (req > maxExtent ? maxExtent : req);
                CHECK(result.width >= clampedReq);
                CHECK(result.height >= clampedReq);
                CHECK(result.width <= maxExtent);
                CHECK(result.height <= maxExtent);
            }
        }
    }

    // C2's max-boundary sweep: maxExtent at the extreme end of uint32_t, req at/just under it.
    const std::array<std::uint32_t, 3> extremeMax{8192, 4294967294U, 4294967295U};
    const std::array<std::uint32_t, 3> extremeQuanta{1, 16, 64};
    for (const std::uint32_t maxExtent : extremeMax) {
        for (const std::uint32_t quantum : extremeQuanta) {
            for (const std::uint32_t req : {maxExtent - 1, maxExtent}) {
                const Extent2D result = nextTargetExtent({req, req}, {0, 0}, quantum, maxExtent);
                const std::uint32_t clampedReq = req < 1 ? 1 : req;
                CHECK(result.width >= clampedReq);
                CHECK(result.height >= clampedReq);
                CHECK(result.width <= maxExtent);
                CHECK(result.height <= maxExtent);
            }
        }
    }
}

TEST_CASE("render target: RenderTargetConfig defaults are load-bearing") {
    const RenderTargetConfig config{};
    CHECK(config.quantum == 1);
    CHECK(config.depth == true);
    CHECK((config.colorFormat == TextureFormat::RGBA8Unorm));
    CHECK(config.maxExtent == RENDER_TARGET_MAX_EXTENT);
    CHECK(RENDER_TARGET_MAX_EXTENT == 8192);
}

// ================================================================================================
// Tier-2 (GPU-gated): RenderTarget lifetime, resize and draw round-trip (task 2.2.3, Step 3).
// Case 7 (NativeDeviceAccessor::texture's swapchain refusal) lives in rhi_swapchain_test.cpp (§O-1).
// ================================================================================================

TEST_CASE("render target: create -> formats -> extents") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    auto target = RenderTarget::create(*device, {200, 150}, {.quantum = 64});
    REQUIRE(target.has_value());
    CHECK((target->colorFormat() == TextureFormat::RGBA8Unorm));
    const TextureFormat df = target->depthFormat();
    CHECK((df == TextureFormat::D32Float || df == TextureFormat::D24Unorm || df == TextureFormat::D16Unorm));
    CHECK(target->textureExtent().width >= target->drawExtent().width);
    CHECK(target->textureExtent().height >= target->drawExtent().height);
    CHECK(target->drawExtent().width == 200);
    CHECK(target->drawExtent().height == 150);
    CHECK(target->colorTexture().valid());

    // F12/S3: the Frame beginFrame() hands out must report the DRAWN sub-rect, not the (larger,
    // quantised) allocation — this is what makes a scene's projection aspect correct for free.
    std::optional<engine::render::Frame> frame = target->beginFrame(Color{});
    REQUIRE(frame.has_value());
    CHECK(frame->extent().width == target->drawExtent().width);
    CHECK(frame->extent().height == target->drawExtent().height);
    CHECK(frame->extent().width != target->textureExtent().width);  // 200 != 256 (quantum 64) — discriminates
    CHECK(target->endFrame(std::move(*frame)));
}

TEST_CASE("render target: depth = false") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    auto target = RenderTarget::create(*device, {64, 64}, {.depth = false});
    REQUIRE(target.has_value());
    CHECK((target->depthFormat() == TextureFormat::Invalid));
    std::optional<engine::render::Frame> frame = target->beginFrame(Color{});
    REQUIRE(frame.has_value());
    CHECK(target->endFrame(std::move(*frame)));
}

TEST_CASE("render target: resize never changes formats (INV-2/D10)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    auto target = RenderTarget::create(*device, {320, 180}, {.quantum = 64});
    REQUIRE(target.has_value());
    const TextureFormat colorFmt = target->colorFormat();
    const TextureFormat depthFmt = target->depthFormat();

    const std::array<Extent2D, 4> sequence{Extent2D{1280, 720}, Extent2D{640, 360}, Extent2D{1281, 721},
                                           Extent2D{320, 180}};
    for (const Extent2D e : sequence) {
        CHECK(target->resize(e));
        CHECK((target->colorFormat() == colorFmt));
        CHECK((target->depthFormat() == depthFmt));
        CHECK(target->textureExtent().width >= target->drawExtent().width);
        CHECK(target->textureExtent().height >= target->drawExtent().height);
    }
}

TEST_CASE("render target: AC-6 reallocation count — a simulated resize drag, not case 3's four big jumps") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    const RenderTargetConfig cfg{.quantum = 64};
    auto target = RenderTarget::create(*device, {400, 400}, cfg);
    REQUIRE(target.has_value());

    // width 400 -> 1200 in ~13px steps (~62 resizes), height fixed — a CONTINUOUS drag, unlike the
    // four-big-jump sequence of the case above (§DN-17: do not read AC-6 off that case).
    std::vector<std::uint32_t> up;
    for (std::uint32_t w = 400; w <= 1200; w += 13) {
        up.push_back(w);
    }

    // The self-checking oracle: a PURE replay of nextTargetExtent over the identical sequence must
    // predict the exact same reallocation count as the real object — if policy and object ever
    // disagree, this reds.
    Extent2D pureCurrent = target->textureExtent();
    int pureUpCount = 0;
    for (const std::uint32_t w : up) {
        const Extent2D want = nextTargetExtent({w, 400}, pureCurrent, cfg.quantum, cfg.maxExtent);
        if (want != pureCurrent) {
            ++pureUpCount;
            pureCurrent = want;
        }
    }

    Extent2D realCurrent = target->textureExtent();
    int realUpCount = 0;
    for (const std::uint32_t w : up) {
        REQUIRE(target->resize({w, 400}));
        const Extent2D now = target->textureExtent();
        if (now != realCurrent) {
            ++realUpCount;
            realCurrent = now;
        }
    }

    CHECK(realUpCount == pureUpCount);  // object and policy must never disagree
    CHECK(realUpCount <= 14);           // ceil((1200-400)/64) + 1 == 14 — far fewer than the 61 resizes

    // Sweep back down: shrink hysteresis must also keep the count low in that direction.
    std::vector<std::uint32_t> down;
    for (std::uint32_t w = 1200; w >= 401; w -= 13) {
        down.push_back(w);
    }
    down.push_back(400);

    pureCurrent = target->textureExtent();
    int pureDownCount = 0;
    for (const std::uint32_t w : down) {
        const Extent2D want = nextTargetExtent({w, 400}, pureCurrent, cfg.quantum, cfg.maxExtent);
        if (want != pureCurrent) {
            ++pureDownCount;
            pureCurrent = want;
        }
    }

    realCurrent = target->textureExtent();
    int realDownCount = 0;
    for (const std::uint32_t w : down) {
        REQUIRE(target->resize({w, 400}));
        const Extent2D now = target->textureExtent();
        if (now != realCurrent) {
            ++realDownCount;
            realCurrent = now;
        }
    }

    CHECK(realDownCount == pureDownCount);
    CHECK(realDownCount <= 6);  // the hysteresis admits only a handful of shrink transitions over this span
}

TEST_CASE("render target: colorTexture() is genuinely sampleable") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    auto target = RenderTarget::create(*device, {64, 64}, {.depth = false});
    REQUIRE(target.has_value());

    const SamplerHandle sampler = device->createSampler({});
    REQUIRE(sampler.valid());
    const TextureHandle scratchColor = device->createTexture(
        {.format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::ColorTarget, .width = 64, .height = 64});
    REQUIRE(scratchColor.valid());

    const CommandBufferHandle cmd = device->acquireCommandBuffer();
    REQUIRE(cmd.valid());
    const ColorAttachment scratchAttachment{.texture = scratchColor};
    const RenderPassHandle pass = device->beginRenderPass(cmd, {.colorAttachments = {&scratchAttachment, 1}});
    REQUIRE(pass.valid());
    const std::array<TextureSamplerBinding, 1> bindings{TextureSamplerBinding{target->colorTexture(), sampler}};
    device->bindFragmentSamplers(pass, 0, bindings);  // green = Sampler usage honoured, no rhi ERROR
    device->endRenderPass(pass);
    CHECK(device->submit(cmd));

    device->destroyTexture(scratchColor);
    device->destroySampler(sampler);
}

TEST_CASE("render target: move semantics (E15)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    auto first = RenderTarget::create(*device, {64, 64});
    REQUIRE(first.has_value());
    const TextureHandle originalColor = first->colorTexture();

    RenderTarget movedTo = std::move(*first);
    CHECK_FALSE(first->colorTexture().valid());  // move-construct: source inert
    CHECK(movedTo.colorTexture() == originalColor);

    auto second = RenderTarget::create(*device, {32, 32});
    REQUIRE(second.has_value());
    const TextureHandle secondColor = second->colorTexture();

    // move-assign a LIVE target over another LIVE target: exactly one release of each GPU object
    // (no double-free under ASan), destination usable afterwards.
    movedTo = std::move(*second);
    CHECK(movedTo.colorTexture() == secondColor);
    CHECK_FALSE(second->colorTexture().valid());

    std::optional<engine::render::Frame> frame = movedTo.beginFrame(Color{});
    REQUIRE(frame.has_value());
    CHECK(movedTo.endFrame(std::move(*frame)));
}

TEST_CASE("render target: not-renderable path (E8) — forced by RAISING the ceiling, not an absurd quantum") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    // E18 makes an absurd quantum incapable of forcing this (want is clamped to maxExtent AFTER
    // rounding up) — raising the ceiling itself is the only way to make createTexture fail.
    CHECK_FALSE(RenderTarget::create(*device, {100000, 100000}, {.maxExtent = 100000}).has_value());

    auto live = RenderTarget::create(*device, {64, 64}, {.maxExtent = 100000});
    REQUIRE(live.has_value());
    CHECK_FALSE(live->resize({100000, 100000}));
    CHECK_FALSE(live->colorTexture().valid());
    CHECK_FALSE(live->beginFrame(Color{}).has_value());
    // `live` still destructs cleanly at scope exit — no ~Device leak WARN (verified by ASan/LSan).
}

#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/core/vfs.hpp>
    #include <aero/scene/scene.hpp>
    #include <aero/scene_render/scene_renderer.hpp>

    #include <memory>

TEST_CASE("render target: draw-into-target round trip, incl. a resize between frames (AC-9)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    auto target = RenderTarget::create(*device, {320, 180}, {.quantum = 64});
    REQUIRE(target.has_value());

    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    auto sceneRenderer =
        engine::scene_render::SceneRenderer::create(*device, vfs, target->colorFormat(), target->depthFormat());
    REQUIRE(sceneRenderer.has_value());

    engine::World world;
    const engine::Entity camEntity = world.create();
    REQUIRE(world.add<engine::Transform>(camEntity, engine::Transform{engine::Vec3{0.0F, 1.0F, 4.0F},
                                                                      engine::Quat::identity(), engine::Vec3::one()}) !=
            nullptr);
    REQUIRE(world.add<engine::Camera>(camEntity, engine::Camera{}) != nullptr);
    const engine::Entity cubeEntity = world.create();
    REQUIRE(world.add<engine::Transform>(cubeEntity) != nullptr);
    REQUIRE(world.add<engine::MeshRenderer>(cubeEntity, engine::MeshRenderer{}) != nullptr);

    const Color clear{0.05F, 0.07F, 0.10F, 1.0F};
    for (int i = 0; i < 3; ++i) {
        if (i == 2) {
            REQUIRE(target->resize({640, 360}));  // resize BETWEEN frames 2 and 3
        }
        std::optional<engine::render::Frame> frame = target->beginFrame(clear);
        REQUIRE(frame.has_value());
        sceneRenderer->render(world, *frame);
        CHECK(target->endFrame(std::move(*frame)));
    }
}

#endif  // AERO_SHADER_TOOLS_ENABLED

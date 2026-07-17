// Aero Engine — rhi swapchain tests, tier 2 (task 0.4.2; AC-8). Gated on a REAL, VISIBLE 320x180
// window (title "aero rhi test") in addition to the tier-1 real-video Context + Device gate. A
// small window flashes for ~a second per ctest run — by design (spec D8): minimized/hidden-window
// acquire behavior is not verifiable from source, so this tier doesn't gamble on it.
#include <aero/platform/platform.hpp>
#include <aero/rhi/rhi.hpp>

#include "rhi_test_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
// <ostream> is load-bearing on MSVC (see rhi_device_test.cpp's comment).
#include <optional>
#include <ostream>
#include <vector>

using namespace engine::rhi;

TEST_CASE("rhi swapchain: T2-1 claim + identity") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero rhi test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }

    const SwapchainHandle sc = dev->createSwapchain(*window);
    REQUIRE(sc.valid());
    const TextureFormat format = dev->swapchainFormat(sc);
    // E17: one of the four SDR 8-bit swapchain formats — double-parens (TextureFormat has toString).
    CHECK((format == TextureFormat::RGBA8Unorm || format == TextureFormat::RGBA8UnormSrgb ||
           format == TextureFormat::BGRA8Unorm || format == TextureFormat::BGRA8UnormSrgb));
    CHECK(dev->supportsPresentMode(sc, PresentMode::Vsync));
    CHECK(dev->setPresentMode(sc, PresentMode::Vsync));
    dev->destroySwapchain(sc);
}

TEST_CASE("rhi swapchain: T2-2 the frame loop (>=3 frames — this IS 0.5.1's sample body, H3)") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero rhi test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }
    const SwapchainHandle sc = dev->createSwapchain(*window);
    REQUIRE(sc.valid());

    TextureHandle lastAcquired{};
    for (int frame = 0; frame < 3; ++frame) {
        const CommandBufferHandle cmd = dev->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        const std::optional<SwapchainTexture> acquired = dev->acquireSwapchainTexture(cmd, sc);
        REQUIRE(acquired.has_value());
        CHECK(acquired->extent.width == static_cast<std::uint32_t>(window->pixelSize().width));
        CHECK(acquired->extent.height == static_cast<std::uint32_t>(window->pixelSize().height));

        const ColorAttachment color{.texture = acquired->texture,
                                    .clearColor = Color{static_cast<float>(frame) / 3.0F, 0.0F, 0.0F, 1.0F}};
        const RenderPassHandle pass = dev->beginRenderPass(cmd, {.colorAttachments = {&color, 1}});
        REQUIRE(pass.valid());
        dev->endRenderPass(pass);
        CHECK(dev->submit(cmd));

        lastAcquired = acquired->texture;
    }

    // E3 on the real path: the last frame's acquired texture is stale after submit.
    const CommandBufferHandle probeCmd = dev->acquireCommandBuffer();
    REQUIRE(probeCmd.valid());
    const ColorAttachment staleAttachment{.texture = lastAcquired};
    CHECK_FALSE(dev->beginRenderPass(probeCmd, {.colorAttachments = {&staleAttachment, 1}}).valid());
    dev->cancel(probeCmd);

    dev->destroySwapchain(sc);
}

TEST_CASE("rhi swapchain: T2-3 D10 cancel-after-acquire disposes via submit, returns false") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero rhi test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }
    const SwapchainHandle sc = dev->createSwapchain(*window);
    REQUIRE(sc.valid());

    const CommandBufferHandle cmd = dev->acquireCommandBuffer();
    REQUIRE(cmd.valid());
    const std::optional<SwapchainTexture> acquired = dev->acquireSwapchainTexture(cmd, sc);
    REQUIRE(acquired.has_value());

    CHECK_FALSE(dev->cancel(cmd));  // D10: illegal after an acquire — logged, buffer still consumed
    CHECK_FALSE(dev->submit(cmd));  // cmd is stale now regardless

    dev->destroySwapchain(sc);
}

TEST_CASE("rhi swapchain: T2-4 E6 write-only — uploadTexture on an acquired image is rejected") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero rhi test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }
    const SwapchainHandle sc = dev->createSwapchain(*window);
    REQUIRE(sc.valid());

    const CommandBufferHandle cmd = dev->acquireCommandBuffer();
    REQUIRE(cmd.valid());
    const std::optional<SwapchainTexture> acquired = dev->acquireSwapchainTexture(cmd, sc);
    REQUIRE(acquired.has_value());

    // All four SDR 8-bit swapchain formats are 4 bytes/texel — size correctly regardless of which
    // one this platform returned.
    const std::vector<std::byte> bytes(static_cast<std::size_t>(acquired->extent.width) * acquired->extent.height * 4,
                                       std::byte{0});
    CHECK_FALSE(dev->uploadTexture(acquired->texture, 0, bytes));  // write-only — no pass needed to prove it

    CHECK(dev->submit(cmd));  // dispose the frame
    dev->destroySwapchain(sc);
}

TEST_CASE("rhi swapchain: T2-5 D15/E24 destroy-while-acquired is refused, then succeeds after disposal") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero rhi test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }
    const SwapchainHandle sc = dev->createSwapchain(*window);
    REQUIRE(sc.valid());

    const CommandBufferHandle cmd = dev->acquireCommandBuffer();
    REQUIRE(cmd.valid());
    const std::optional<SwapchainTexture> acquired = dev->acquireSwapchainTexture(cmd, sc);
    REQUIRE(acquired.has_value());

    dev->destroySwapchain(sc);  // refused while `cmd` holds an acquire from it (D15)
    CHECK((dev->swapchainFormat(sc) != TextureFormat::Invalid));

    CHECK(dev->submit(cmd));  // dispose the outstanding acquire

    dev->destroySwapchain(sc);  // now succeeds
    CHECK((dev->swapchainFormat(sc) == TextureFormat::Invalid));
}

TEST_CASE("rhi swapchain: T2-6 destroy then re-create on the same window") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero rhi test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }
    const SwapchainHandle first = dev->createSwapchain(*window);
    REQUIRE(first.valid());
    dev->destroySwapchain(first);

    const SwapchainHandle second = dev->createSwapchain(*window);
    CHECK(second.valid());
    dev->destroySwapchain(second);
}

TEST_CASE("rhi swapchain: T2-7 non-Vsync fails, never downgrades (C-13)") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero rhi test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }

    const SwapchainHandle vsyncHandle = dev->createSwapchain(*window);
    REQUIRE(vsyncHandle.valid());
    const bool immediateOk = dev->supportsPresentMode(vsyncHandle, PresentMode::Immediate);
    dev->destroySwapchain(vsyncHandle);

    const SwapchainHandle immediateHandle = dev->createSwapchain(*window, {.presentMode = PresentMode::Immediate});
    CHECK(immediateHandle.valid() == immediateOk);
    if (immediateHandle.valid()) {
        dev->destroySwapchain(immediateHandle);
    }
}

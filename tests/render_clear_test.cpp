// Aero Engine — render clear-pass tests (task 0.5.1). R-0 is tier-0 (runs everywhere). R-1..R-5 are
// tier-2: they need a real-video Context + GPU Device + a visible 320x180 window "aero render test"
// (mirrors rhi_swapchain_test.cpp's gate; a small window flashes ~1s per case, by design). Gated by
// AERO_REQUIRE_GPU via rhi_test_support.hpp: unset -> skip loudly; set (CI) -> a missing GPU FAILS.
// Declaration order per case: ctx -> device -> window -> renderer (Renderer innermost; C3), matching
// rhi_swapchain_test.cpp.
#include <aero/platform/platform.hpp>
#include <aero/render/renderer.hpp>
#include <aero/rhi/rhi.hpp>

#include "rhi_test_support.hpp"

#include <doctest/doctest.h>

#include <cstdint>
// <ostream> is load-bearing on MSVC for enum/string_view CHECKs (see rhi_device_test.cpp's comment).
#include <optional>
#include <ostream>
#include <utility>

using engine::render::Frame;
using engine::render::Renderer;
using engine::render::RendererConfig;
using namespace engine::rhi;

TEST_CASE("render clear: R-0 RendererConfig default is Vsync (tier-0, no GPU)") {
    const RendererConfig config{};
    CHECK((config.presentMode == PresentMode::Vsync));
}

TEST_CASE("render clear: R-1 create + colorFormat is an SDR swapchain format") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero render test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }

    auto renderer = Renderer::create(*device, *window);
    REQUIRE(renderer.has_value());
    const TextureFormat f = renderer->colorFormat();
    CHECK((f == TextureFormat::RGBA8Unorm || f == TextureFormat::RGBA8UnormSrgb || f == TextureFormat::BGRA8Unorm ||
           f == TextureFormat::BGRA8UnormSrgb));
}

TEST_CASE("render clear: R-2 the clear loop (>=3 frames)") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero render test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }
    auto renderer = Renderer::create(*device, *window);
    REQUIRE(renderer.has_value());

    for (int frame = 0; frame < 3; ++frame) {
        std::optional<Frame> f = renderer->beginFrame(Color{static_cast<float>(frame) / 3.0F, 0.2F, 0.4F, 1.0F});
        REQUIRE(f.has_value());
        CHECK(f->extent().width == static_cast<std::uint32_t>(window->pixelSize().width));
        CHECK(f->extent().height == static_cast<std::uint32_t>(window->pixelSize().height));
        CHECK(f->pass().valid());
        CHECK(f->commandBuffer().valid());
        CHECK(renderer->endFrame(std::move(*f)));
    }
}

TEST_CASE("render clear: R-3 stable across resize") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero render test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }
    auto renderer = Renderer::create(*device, *window);
    REQUIRE(renderer.has_value());

    // Run `frames` clears; assert only survival + positive extents (safe through a resize transient).
    auto survives = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            ctx.newFrame();
            engine::platform::Event ev;
            while (ctx.pollEvent(ev)) {
            }
            if (std::optional<Frame> f = renderer->beginFrame(Color{0.1F, 0.2F, 0.3F, 1.0F})) {
                CHECK(f->extent().width > 0U);
                CHECK(f->extent().height > 0U);
                CHECK(renderer->endFrame(std::move(*f)));
            }
            // nullopt (transient non-presentable) is acceptable — the point is the loop never crashes.
        }
    };
    // At a SETTLED size, the swapchain extent equals the window's pixel size (rhi swapchain-test steady
    // state). Assert this only at the original size; NOT at the requested new size (a WM may clamp). If
    // this flakes on a CI lane, relax to `survives` per the plan's §J — do not chase the WM.
    auto steadyExtentMatches = [&]() {
        ctx.newFrame();
        engine::platform::Event ev;
        while (ctx.pollEvent(ev)) {
        }
        std::optional<Frame> f = renderer->beginFrame(Color{0.1F, 0.2F, 0.3F, 1.0F});
        REQUIRE(f.has_value());
        CHECK(f->extent().width == static_cast<std::uint32_t>(window->pixelSize().width));
        CHECK(f->extent().height == static_cast<std::uint32_t>(window->pixelSize().height));
        CHECK(renderer->endFrame(std::move(*f)));
    };

    steadyExtentMatches();  // baseline 320x180 (== swapchain-test steady state)
    window->setSize(640, 360);
    survives(6);  // ride out the resize; no crash, extents positive
    window->setSize(320, 180);
    survives(6);
    steadyExtentMatches();  // settled back at the original size
}

TEST_CASE("render clear: R-4 move-only — moved-from Renderer is inert") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero render test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }

    auto first = Renderer::create(*device, *window);
    REQUIRE(first.has_value());
    Renderer moved = std::move(*first);
    CHECK((first->colorFormat() == TextureFormat::Invalid));  // moved-from: inert, no crash
    CHECK((moved.colorFormat() != TextureFormat::Invalid));   // moved-to: live
    std::optional<Frame> frame = moved.beginFrame(Color{});
    REQUIRE(frame.has_value());
    CHECK(moved.endFrame(std::move(*frame)));
}

TEST_CASE("render clear: R-5 dropped frame is disposed, loop recovers") {
    engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto device = Device::create();
    if (!device.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }
    auto window = ctx.createWindow({.title = "aero render test", .width = 320, .height = 180});
    if (!window.has_value()) {
        AERO_SKIP_OR_FAIL("no real window available");
    }
    auto renderer = Renderer::create(*device, *window);
    REQUIRE(renderer.has_value());

    {
        std::optional<Frame> dropped = renderer->beginFrame(Color{0.9F, 0.1F, 0.1F, 1.0F});
        REQUIRE(dropped.has_value());
        // Leave scope WITHOUT endFrame: ~Frame disposes (submit) + logs one WARN (expected).
    }
    // The swapchain image was disposed, not leaked: a fresh frame still works.
    std::optional<Frame> next = renderer->beginFrame(Color{0.1F, 0.9F, 0.1F, 1.0F});
    REQUIRE(next.has_value());
    CHECK(renderer->endFrame(std::move(*next)));
}

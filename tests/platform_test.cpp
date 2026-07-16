// Black-box lifecycle tests for engine::platform (task 0.3.1), against the headless "dummy" SDL
// video driver (ContextConfig{headless=true}) — no display server needed, works on every CI runner.
// This TU does NOT include SDL: it only exercises the public API.
//
// Traps: the dummy driver may queue a startup event or two, so drain the queue before asserting it
// is empty. size()/pixelSize() are compared with >=, never == (a real HiDPI runner would differ).
#include <aero/platform/platform.hpp>

#include <doctest/doctest.h>

#include <utility>

TEST_CASE("headless Context is valid") {
    const engine::platform::Context ctx{{.headless = true}};
    CHECK(ctx.valid());
}

TEST_CASE("createWindow returns a sized window") {
    engine::platform::Context ctx{{.headless = true}};
    auto window = ctx.createWindow({.title = "probe", .width = 640, .height = 480, .hidden = true});
    REQUIRE(window.has_value());
    CHECK(window->id().valid());
    CHECK(window->size() == engine::platform::WindowSize{640, 480});
    CHECK(window->pixelSize().width >= 640);
    CHECK(window->pixelSize().height >= 480);
}

TEST_CASE("two windows have distinct ids") {
    engine::platform::Context ctx{{.headless = true}};
    auto a = ctx.createWindow({.title = "a", .hidden = true});
    auto b = ctx.createWindow({.title = "b", .hidden = true});
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->id().valid());
    CHECK(b->id().valid());
    CHECK_FALSE(a->id() == b->id());
}

TEST_CASE("setSize changes logical size") {
    engine::platform::Context ctx{{.headless = true}};
    auto window = ctx.createWindow({.title = "resize", .width = 640, .height = 480, .hidden = true});
    REQUIRE(window.has_value());
    window->setSize(800, 600);
    // Drain: the dummy driver may surface the resize as an event before size() reflects it.
    for (engine::platform::Event drain; ctx.pollEvent(drain);) {
    }
    CHECK(window->size() == engine::platform::WindowSize{800, 600});
}

TEST_CASE("setTitle / show / hide are callable headless") {
    engine::platform::Context ctx{{.headless = true}};
    auto window = ctx.createWindow({.title = "title", .hidden = true});
    REQUIRE(window.has_value());
    window->setTitle("a new title");
    window->show();
    window->hide();
    // No crash is the whole assertion here.
}

TEST_CASE("pollEvent on a drained queue is false") {
    engine::platform::Context ctx{{.headless = true}};
    engine::platform::Event ev;
    while (ctx.pollEvent(ev)) {
    }  // drain any startup noise first
    CHECK_FALSE(ctx.pollEvent(ev));
}

TEST_CASE("~Window and ~Context are clean") {
    {
        engine::platform::Context ctx{{.headless = true}};
        auto window = ctx.createWindow({.title = "scoped", .hidden = true});
        REQUIRE(window.has_value());
        // window and ctx both go out of scope here — ASan proves no leak.
    }
}

TEST_CASE("move-assigning onto an engaged Window does not use-after-free") {
    // Regression for the code-review fix: cleanup moved from ~Window into ~Window::Impl so the
    // defaulted move-assignment destroys the LHS's Impl (and its SDL_Window) before adopting the
    // RHS's, instead of silently orphaning it. The orphan leak itself isn't directly assertable here
    // (SDL_Quit backstops any leaked window at Context teardown), but a regression in the move
    // operation itself — e.g. reintroducing a double-destroy or a use-after-free of the overwritten
    // window — is exactly what ASan catches, which is what this test guards against.
    engine::platform::Context ctx{{.headless = true}};
    auto a = ctx.createWindow({.title = "a", .hidden = true});
    auto b = ctx.createWindow({.title = "b", .hidden = true});
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    const engine::platform::WindowId bId = b->id();

    a = std::move(*b);  // move-assign onto an already-engaged Window: must not double-free/UAF `a`

    CHECK(a->id().valid());
    CHECK(a->id() == bId);
}

TEST_CASE("a fresh Context has empty input state and newFrame is callable") {
    engine::platform::Context ctx{{.headless = true}};
    CHECK_FALSE(ctx.input().keyDown(engine::platform::Key::W));
    CHECK(ctx.input().mousePosition().x == doctest::Approx(0.0f));
    ctx.newFrame();  // no-op on empty state; must be callable
    CHECK_FALSE(ctx.input().keyPressed(engine::platform::Key::W));
}

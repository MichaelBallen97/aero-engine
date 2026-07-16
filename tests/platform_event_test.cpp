#include <aero/platform/platform.hpp>

#include <SDL3/SDL.h>  // WHITE-BOX: this TU drives the backend directly to inject events.
#include <doctest/doctest.h>

#include <optional>

namespace {
// Push one synthetic SDL event and pull the single engine event it should translate to. Drains any
// startup noise first so we read OUR event, not the driver's.
std::optional<engine::platform::Event> roundTrip(engine::platform::Context& ctx, SDL_Event in) {
    for (engine::platform::Event drain; ctx.pollEvent(drain);) {
    }
    REQUIRE(SDL_PushEvent(&in));  // SDL3 SDL_PushEvent returns bool (true == queued)
    engine::platform::Event out;
    if (ctx.pollEvent(out)) {
        return out;
    }
    return std::nullopt;
}
}  // namespace

TEST_CASE("pump translates SDL_EVENT_QUIT to Quit") {
    engine::platform::Context ctx{{.headless = true}};
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    auto ev = roundTrip(ctx, e);
    REQUIRE(ev.has_value());
    CHECK(ev->type == engine::platform::EventType::Quit);
}

TEST_CASE("pump translates a logical resize with dimensions") {
    engine::platform::Context ctx{{.headless = true}};
    SDL_Event e{};
    e.type = SDL_EVENT_WINDOW_RESIZED;
    e.window.windowID = 7;
    e.window.data1 = 800;
    e.window.data2 = 600;
    auto ev = roundTrip(ctx, e);
    REQUIRE(ev.has_value());
    CHECK(ev->type == engine::platform::EventType::WindowResized);
    CHECK(ev->window.value == 7);
    CHECK(ev->width == 800);
    CHECK(ev->height == 600);
}

TEST_CASE("pump translates a pixel-size change as a DISTINCT type from a logical resize") {
    engine::platform::Context ctx{{.headless = true}};
    SDL_Event e{};
    e.type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
    e.window.windowID = 7;
    e.window.data1 = 1600;
    e.window.data2 = 1200;
    auto ev = roundTrip(ctx, e);
    REQUIRE(ev.has_value());
    CHECK(ev->type == engine::platform::EventType::WindowPixelSizeChanged);
    CHECK(ev->type != engine::platform::EventType::WindowResized);  // AC-9: distinct types
    CHECK(ev->window.value == 7);
    CHECK(ev->width == 1600);
    CHECK(ev->height == 1200);
}

TEST_CASE("pump translates SDL_EVENT_WINDOW_CLOSE_REQUESTED to WindowClose") {
    engine::platform::Context ctx{{.headless = true}};
    SDL_Event e{};
    e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    e.window.windowID = 3;
    auto ev = roundTrip(ctx, e);
    REQUIRE(ev.has_value());
    CHECK(ev->type == engine::platform::EventType::WindowClose);
    CHECK(ev->window.value == 3);
}

TEST_CASE("pump translates SDL_EVENT_WINDOW_FOCUS_GAINED to WindowFocusGained") {
    engine::platform::Context ctx{{.headless = true}};
    SDL_Event e{};
    e.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    e.window.windowID = 5;
    auto ev = roundTrip(ctx, e);
    REQUIRE(ev.has_value());
    CHECK(ev->type == engine::platform::EventType::WindowFocusGained);
    CHECK(ev->window.value == 5);
}

TEST_CASE("pump translates SDL_EVENT_WINDOW_FOCUS_LOST to WindowFocusLost") {
    engine::platform::Context ctx{{.headless = true}};
    SDL_Event e{};
    e.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    e.window.windowID = 5;
    auto ev = roundTrip(ctx, e);
    REQUIRE(ev.has_value());
    CHECK(ev->type == engine::platform::EventType::WindowFocusLost);
    CHECK(ev->window.value == 5);
}

TEST_CASE("pump translates SDL_EVENT_WINDOW_MINIMIZED to WindowMinimized") {
    engine::platform::Context ctx{{.headless = true}};
    SDL_Event e{};
    e.type = SDL_EVENT_WINDOW_MINIMIZED;
    e.window.windowID = 9;
    auto ev = roundTrip(ctx, e);
    REQUIRE(ev.has_value());
    CHECK(ev->type == engine::platform::EventType::WindowMinimized);
    CHECK(ev->window.value == 9);
}

TEST_CASE("pump translates SDL_EVENT_WINDOW_RESTORED to WindowRestored") {
    engine::platform::Context ctx{{.headless = true}};
    SDL_Event e{};
    e.type = SDL_EVENT_WINDOW_RESTORED;
    e.window.windowID = 9;
    auto ev = roundTrip(ctx, e);
    REQUIRE(ev.has_value());
    CHECK(ev->type == engine::platform::EventType::WindowRestored);
    CHECK(ev->window.value == 9);
}

TEST_CASE("pump drops events it does not model") {
    engine::platform::Context ctx{{.headless = true}};
    for (engine::platform::Event drain; ctx.pollEvent(drain);) {
    }
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;  // 0.3.2's business, not 0.3.1's
    REQUIRE(SDL_PushEvent(&e));
    engine::platform::Event out;
    CHECK_FALSE(ctx.pollEvent(out));  // drained but never surfaced ⇒ queue reads empty
}

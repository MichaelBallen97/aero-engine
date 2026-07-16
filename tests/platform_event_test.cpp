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
    CHECK(ev->size.width == 800);
    CHECK(ev->size.height == 600);
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
    CHECK(ev->size.width == 1600);
    CHECK(ev->size.height == 1200);
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
    // Gamepad input is explicitly out of scope (task 0.3.2 §8) and translate() still has no case for
    // it, so it falls through to `default` and is dropped. (SDL_EVENT_MOUSE_MOTION used to be the
    // example here, but 0.3.2 now MODELS mouse motion — using it would make this test fail at
    // runtime, since pollEvent would return true instead of false.)
    e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    REQUIRE(SDL_PushEvent(&e));
    engine::platform::Event out;
    CHECK_FALSE(ctx.pollEvent(out));  // drained but never surfaced ⇒ queue reads empty
}

TEST_CASE("pump translates SDL_EVENT_KEY_DOWN to KeyDown with a physical Key") {
    engine::platform::Context ctx{{.headless = true}};
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_W;
    e.key.down = true;
    e.key.repeat = false;
    auto ev = roundTrip(ctx, e);
    REQUIRE(ev.has_value());
    CHECK(ev->type == engine::platform::EventType::KeyDown);
    CHECK(ev->key.code == engine::platform::Key::W);  // physical position (D1)
    CHECK_FALSE(ev->key.repeat);
}

TEST_CASE("pump maps SDL modifiers onto the key event") {
    engine::platform::Context ctx{{.headless = true}};
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_S;
    e.key.down = true;
    e.key.mod = SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT;
    auto ev = roundTrip(ctx, e);
    REQUIRE(ev.has_value());
    CHECK(ev->key.mods.ctrl());
    CHECK(ev->key.mods.shift());
    CHECK_FALSE(ev->key.mods.alt());
}

TEST_CASE("pump maps mouse buttons explicitly (RIGHT==3, not by arithmetic)") {
    engine::platform::Context ctx{{.headless = true}};
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.button = SDL_BUTTON_RIGHT;  // 3
    e.button.x = 12.0f;
    e.button.y = 34.0f;
    auto ev = roundTrip(ctx, e);
    REQUIRE(ev.has_value());
    CHECK(ev->type == engine::platform::EventType::MouseButtonDown);
    CHECK(ev->mouseButton.button == engine::platform::MouseButton::Right);
    CHECK(ev->mouseButton.x == doctest::Approx(12.0f));
    CHECK(ev->mouseButton.y == doctest::Approx(34.0f));
}

TEST_CASE("pump translates mouse motion with relative delta") {
    engine::platform::Context ctx{{.headless = true}};
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.x = 200.0f;
    e.motion.y = 100.0f;
    e.motion.xrel = 5.0f;
    e.motion.yrel = -3.0f;
    auto ev = roundTrip(ctx, e);
    REQUIRE(ev.has_value());
    CHECK(ev->type == engine::platform::EventType::MouseMoved);
    CHECK(ev->mouseMotion.x == doctest::Approx(200.0f));
    CHECK(ev->mouseMotion.dx == doctest::Approx(5.0f));
    CHECK(ev->mouseMotion.dy == doctest::Approx(-3.0f));
}

TEST_CASE("pump normalizes a FLIPPED mouse wheel") {
    engine::platform::Context ctx{{.headless = true}};
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.x = 1.0f;
    e.wheel.y = -3.0f;
    e.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    auto ev = roundTrip(ctx, e);
    REQUIRE(ev.has_value());
    CHECK(ev->type == engine::platform::EventType::MouseWheel);
    CHECK(ev->mouseWheel.x == doctest::Approx(-1.0f));  // flip undone (D14)
    CHECK(ev->mouseWheel.y == doctest::Approx(3.0f));
}

TEST_CASE("ctx.input() reflects a key after the pump translates it") {
    engine::platform::Context ctx{{.headless = true}};
    ctx.newFrame();
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = SDL_SCANCODE_SPACE;
    e.key.down = true;
    REQUIRE(SDL_PushEvent(&e));
    for (engine::platform::Event drain; ctx.pollEvent(drain);) {  // draining feeds inputState
    }
    CHECK(ctx.input().keyDown(engine::platform::Key::Space));
    CHECK(ctx.input().keyPressed(engine::platform::Key::Space));
}

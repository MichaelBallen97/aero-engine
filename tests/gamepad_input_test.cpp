// Aero Engine -- InputState's gamepad fold and the stick deadzone (task 4.4.3). Pure fold: construct an
// InputState, feed it engine Events, assert. No Context, no SDL, no display.
#include <aero/platform/event.hpp>
#include <aero/platform/input.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>

namespace {
using engine::platform::Event;
using engine::platform::EventType;
using engine::platform::GamepadAxis;
using engine::platform::GamepadAxisData;
using engine::platform::GamepadButton;
using engine::platform::GamepadButtonData;
using engine::platform::GamepadDeviceData;
using engine::platform::GamepadStick;
using engine::platform::InputState;
using engine::platform::Key;
using engine::platform::KeyData;
using engine::platform::KeyMods;
using engine::platform::MAX_GAMEPADS;
using engine::platform::MouseButton;
using engine::platform::MouseButtonData;
using engine::platform::radialDeadzone;
using engine::platform::STICK_DEADZONE;
using engine::platform::StickPosition;

Event device(EventType type, std::uint8_t slot) {
    Event e;
    e.type = type;
    e.gamepad = GamepadDeviceData{slot};
    return e;
}
Event connected(std::uint8_t slot) { return device(EventType::GamepadConnected, slot); }
Event disconnected(std::uint8_t slot) { return device(EventType::GamepadDisconnected, slot); }
Event button(EventType type, std::uint8_t slot, GamepadButton b) {
    Event e;
    e.type = type;
    e.gamepadButton = GamepadButtonData{slot, b};
    return e;
}
Event axis(std::uint8_t slot, GamepadAxis a, std::int16_t value) {
    Event e;
    e.type = EventType::GamepadAxisMoved;
    e.gamepadAxis = GamepadAxisData{slot, a, value};
    return e;
}
Event bare(EventType type) {
    Event e;
    e.type = type;
    return e;
}

float lengthOf(StickPosition p) { return std::hypot(p.x, p.y); }
}  // namespace

TEST_CASE("gamepad: a default state has nothing connected, held, edged or deflected; bad indices read 0 (GP1)") {
    const InputState in;
    for (std::size_t slot = 0; slot < MAX_GAMEPADS; ++slot) {
        CHECK_FALSE(in.gamepadConnected(slot));
        for (std::size_t b = 0; b < static_cast<std::size_t>(GamepadButton::Count); ++b) {
            const auto button = static_cast<GamepadButton>(b);
            CHECK_FALSE(in.gamepadButtonDown(button, slot));
            CHECK_FALSE(in.gamepadButtonPressed(button, slot));
            CHECK_FALSE(in.gamepadButtonReleased(button, slot));
        }
        for (std::size_t a = 0; a < static_cast<std::size_t>(GamepadAxis::Count); ++a) {
            CHECK(in.gamepadAxis(static_cast<GamepadAxis>(a), slot) == 0.0F);
        }
        CHECK(in.gamepadStick(GamepadStick::Left, slot) == StickPosition{});
        CHECK(in.gamepadStick(GamepadStick::Right, slot) == StickPosition{});
    }
    CHECK_FALSE(in.gamepadConnected());
    CHECK_FALSE(in.gamepadConnected(MAX_GAMEPADS));
    CHECK_FALSE(in.gamepadConnected(SIZE_MAX));
    CHECK_FALSE(in.gamepadButtonDown(GamepadButton::South, MAX_GAMEPADS));
    CHECK_FALSE(in.gamepadButtonPressed(GamepadButton::South, SIZE_MAX));
    CHECK_FALSE(in.gamepadButtonReleased(GamepadButton::South, SIZE_MAX));
    CHECK_FALSE(in.gamepadButtonDown(GamepadButton::Count));
    CHECK_FALSE(in.gamepadButtonPressed(GamepadButton::Count));
    CHECK_FALSE(in.gamepadButtonReleased(GamepadButton::Count));
    CHECK(in.gamepadAxis(GamepadAxis::Count) == 0.0F);
    CHECK(in.gamepadAxis(GamepadAxis::LeftX, MAX_GAMEPADS) == 0.0F);
    CHECK(in.gamepadStick(GamepadStick::Count) == StickPosition{});
    CHECK(in.gamepadStick(GamepadStick::Left, SIZE_MAX) == StickPosition{});
}

TEST_CASE("gamepad: a connect marks its own slot only, and a slot past the table changes nothing (GP2)") {
    InputState in;
    in.process(connected(2));
    CHECK(in.gamepadConnected(2));
    CHECK_FALSE(in.gamepadConnected(0));
    CHECK_FALSE(in.gamepadConnected(1));
    CHECK_FALSE(in.gamepadConnected(3));
    in.process(connected(4));
    in.process(connected(255));
    for (std::size_t slot = 0; slot < MAX_GAMEPADS; ++slot) {
        CHECK(in.gamepadConnected(slot) == (slot == 2));
    }
}

TEST_CASE("gamepad: held state survives newFrame, edges do not, and slots are independent (GP3)") {
    InputState in;
    in.process(connected(0));
    in.process(connected(1));
    in.process(button(EventType::GamepadButtonDown, 1, GamepadButton::South));
    CHECK(in.gamepadButtonDown(GamepadButton::South, 1));
    CHECK(in.gamepadButtonPressed(GamepadButton::South, 1));
    CHECK_FALSE(in.gamepadButtonDown(GamepadButton::South, 0));
    CHECK_FALSE(in.gamepadButtonPressed(GamepadButton::South, 0));
    in.newFrame();
    CHECK(in.gamepadButtonDown(GamepadButton::South, 1));
    CHECK_FALSE(in.gamepadButtonPressed(GamepadButton::South, 1));
    in.process(button(EventType::GamepadButtonUp, 1, GamepadButton::South));
    CHECK_FALSE(in.gamepadButtonDown(GamepadButton::South, 1));
    CHECK(in.gamepadButtonReleased(GamepadButton::South, 1));
    CHECK_FALSE(in.gamepadButtonReleased(GamepadButton::South, 0));
    in.newFrame();
    CHECK_FALSE(in.gamepadButtonDown(GamepadButton::South, 1));
    CHECK_FALSE(in.gamepadButtonPressed(GamepadButton::South, 1));
    CHECK_FALSE(in.gamepadButtonReleased(GamepadButton::South, 1));
    CHECK_FALSE(in.gamepadButtonDown(GamepadButton::South, 0));
}

TEST_CASE("gamepad: a repeated down makes no new edge and an up for a button not held changes nothing (GP4)") {
    InputState in;
    in.process(connected(0));
    in.process(button(EventType::GamepadButtonDown, 0, GamepadButton::East));
    in.newFrame();
    in.process(button(EventType::GamepadButtonDown, 0, GamepadButton::East));
    CHECK(in.gamepadButtonDown(GamepadButton::East));
    CHECK_FALSE(in.gamepadButtonPressed(GamepadButton::East));

    in.process(button(EventType::GamepadButtonUp, 0, GamepadButton::West));
    CHECK_FALSE(in.gamepadButtonDown(GamepadButton::West));
    CHECK_FALSE(in.gamepadButtonReleased(GamepadButton::West));
    CHECK(in.gamepadButtonDown(GamepadButton::East));
}

TEST_CASE("gamepad: a down and an up in one frame leave both edges and the button not held (GP5)") {
    InputState in;
    in.process(connected(0));
    in.process(button(EventType::GamepadButtonDown, 0, GamepadButton::North));
    in.process(button(EventType::GamepadButtonUp, 0, GamepadButton::North));
    CHECK(in.gamepadButtonPressed(GamepadButton::North));
    CHECK(in.gamepadButtonReleased(GamepadButton::North));
    CHECK_FALSE(in.gamepadButtonDown(GamepadButton::North));
}

TEST_CASE("gamepad: a stick reads exactly -1, 0 and +1 at its ends and a trigger 0 and +1 (GP6)") {
    InputState in;
    in.process(connected(0));
    in.process(axis(0, GamepadAxis::LeftX, -32768));
    CHECK(in.gamepadAxis(GamepadAxis::LeftX) == -1.0F);
    in.process(axis(0, GamepadAxis::LeftX, 32767));
    CHECK(in.gamepadAxis(GamepadAxis::LeftX) == 1.0F);
    in.process(axis(0, GamepadAxis::LeftX, 0));
    CHECK(in.gamepadAxis(GamepadAxis::LeftX) == 0.0F);
    in.process(axis(0, GamepadAxis::LeftX, -16384));
    CHECK(in.gamepadAxis(GamepadAxis::LeftX) == -0.5F);
    in.process(axis(0, GamepadAxis::LeftX, 16384));
    CHECK(in.gamepadAxis(GamepadAxis::LeftX) == 16384.0F / 32767.0F);
    in.process(axis(0, GamepadAxis::LeftX, -1));
    CHECK(in.gamepadAxis(GamepadAxis::LeftX) == -1.0F / 32768.0F);

    in.process(axis(0, GamepadAxis::LeftTrigger, 0));
    CHECK(in.gamepadAxis(GamepadAxis::LeftTrigger) == 0.0F);
    in.process(axis(0, GamepadAxis::LeftTrigger, 32767));
    CHECK(in.gamepadAxis(GamepadAxis::LeftTrigger) == 1.0F);
    in.process(axis(0, GamepadAxis::RightTrigger, 16383));
    CHECK(in.gamepadAxis(GamepadAxis::RightTrigger) == 16383.0F / 32767.0F);
    in.process(axis(0, GamepadAxis::RightTrigger, -5));
    CHECK(in.gamepadAxis(GamepadAxis::RightTrigger) == 0.0F);
}

TEST_CASE("gamepad: a disconnect releases what was held and zeroes the axes, and a connect resets the slot (GP7)") {
    InputState in;
    in.process(connected(0));
    in.process(button(EventType::GamepadButtonDown, 0, GamepadButton::South));
    in.process(axis(0, GamepadAxis::LeftX, 20000));
    in.newFrame();
    in.process(disconnected(0));
    CHECK(in.gamepadButtonReleased(GamepadButton::South));
    CHECK_FALSE(in.gamepadButtonDown(GamepadButton::South));
    CHECK(in.gamepadAxis(GamepadAxis::LeftX) == 0.0F);
    CHECK_FALSE(in.gamepadConnected(0));
    in.newFrame();
    CHECK_FALSE(in.gamepadButtonReleased(GamepadButton::South));

    // The edges stay until newFrame(), and a connect for the same slot in that frame clears them too.
    in.process(connected(0));
    in.process(button(EventType::GamepadButtonDown, 0, GamepadButton::East));
    in.process(axis(0, GamepadAxis::RightY, 30000));
    in.newFrame();
    in.process(button(EventType::GamepadButtonUp, 0, GamepadButton::East));
    in.process(button(EventType::GamepadButtonDown, 0, GamepadButton::North));
    in.process(disconnected(0));
    CHECK(in.gamepadButtonReleased(GamepadButton::North));
    CHECK(in.gamepadButtonPressed(GamepadButton::North));
    in.process(connected(0));
    CHECK(in.gamepadConnected(0));
    CHECK_FALSE(in.gamepadButtonReleased(GamepadButton::East));
    CHECK_FALSE(in.gamepadButtonReleased(GamepadButton::North));
    CHECK_FALSE(in.gamepadButtonPressed(GamepadButton::North));
    CHECK_FALSE(in.gamepadButtonDown(GamepadButton::North));
    CHECK(in.gamepadAxis(GamepadAxis::RightY) == 0.0F);
}

TEST_CASE("gamepad: events for a slot that is not connected, past the table, or a Count value change nothing (GP8)") {
    InputState in;
    in.process(button(EventType::GamepadButtonDown, 1, GamepadButton::South));
    in.process(axis(1, GamepadAxis::LeftX, 32767));
    in.process(button(EventType::GamepadButtonDown, 4, GamepadButton::South));
    in.process(axis(4, GamepadAxis::LeftX, 32767));
    in.process(disconnected(1));
    CHECK_FALSE(in.gamepadButtonDown(GamepadButton::South, 1));
    CHECK_FALSE(in.gamepadButtonPressed(GamepadButton::South, 1));
    CHECK(in.gamepadAxis(GamepadAxis::LeftX, 1) == 0.0F);

    in.process(connected(1));
    in.process(button(EventType::GamepadButtonDown, 1, GamepadButton::Count));
    in.process(button(EventType::GamepadButtonUp, 1, GamepadButton::Count));
    in.process(axis(1, GamepadAxis::Count, 32767));
    for (std::size_t b = 0; b < static_cast<std::size_t>(GamepadButton::Count); ++b) {
        CHECK_FALSE(in.gamepadButtonDown(static_cast<GamepadButton>(b), 1));
        CHECK_FALSE(in.gamepadButtonPressed(static_cast<GamepadButton>(b), 1));
        CHECK_FALSE(in.gamepadButtonReleased(static_cast<GamepadButton>(b), 1));
    }
    for (std::size_t a = 0; a < static_cast<std::size_t>(GamepadAxis::Count); ++a) {
        CHECK(in.gamepadAxis(static_cast<GamepadAxis>(a), 1) == 0.0F);
    }
}

TEST_CASE("gamepad: a focus loss clears keys and mouse buttons and leaves the pads alone (GP9)") {
    InputState in;
    Event key;
    key.type = EventType::KeyDown;
    key.key = KeyData{Key::W, KeyMods{}, false};
    in.process(key);
    Event click;
    click.type = EventType::MouseButtonDown;
    click.mouseButton = MouseButtonData{MouseButton::Left, 1.0F, 2.0F, 1};
    in.process(click);
    in.process(connected(0));
    in.process(button(EventType::GamepadButtonDown, 0, GamepadButton::South));
    in.process(axis(0, GamepadAxis::LeftX, 16384));
    CHECK(in.keyDown(Key::W));
    CHECK(in.mouseButtonDown(MouseButton::Left));

    in.process(bare(EventType::WindowFocusLost));
    CHECK_FALSE(in.keyDown(Key::W));
    CHECK_FALSE(in.mouseButtonDown(MouseButton::Left));
    CHECK(in.gamepadConnected(0));
    CHECK(in.gamepadButtonDown(GamepadButton::South));
    CHECK(in.gamepadAxis(GamepadAxis::LeftX) == 16384.0F / 32767.0F);
}

TEST_CASE("gamepad: the radial deadzone is round, rescaled, exact at its ends and zero for non-finite input (GP10)") {
    CHECK(radialDeadzone(StickPosition{8000.0F / 32767.0F, 0.0F}) == StickPosition{});
    const StickPosition above = radialDeadzone(StickPosition{8001.0F / 32767.0F, 0.0F});
    CHECK(above.x > 0.0F);
    CHECK(above.y == 0.0F);

    CHECK(radialDeadzone(StickPosition{1.0F, 0.0F}) == StickPosition{1.0F, 0.0F});
    CHECK(radialDeadzone(StickPosition{-1.0F, 0.0F}) == StickPosition{-1.0F, 0.0F});
    CHECK(radialDeadzone(StickPosition{0.0F, -1.0F}) == StickPosition{0.0F, -1.0F});

    const StickPosition corner = radialDeadzone(StickPosition{1.0F, 1.0F});
    CHECK(corner.x == corner.y);
    CHECK(doctest::Approx(lengthOf(corner)).epsilon(1e-6) == 1.0);

    const StickPosition diagonal = radialDeadzone(StickPosition{0.36F, 0.48F});
    CHECK(doctest::Approx(diagonal.x / diagonal.y).epsilon(1e-6) == 0.75);
    CHECK(doctest::Approx(lengthOf(diagonal)).epsilon(1e-6) == (0.6 - STICK_DEADZONE) / (1.0 - STICK_DEADZONE));

    CHECK(radialDeadzone(StickPosition{STICK_DEADZONE + 1e-3F, 0.0F}).x < 2e-3F);

    constexpr float NAN_VALUE = std::numeric_limits<float>::quiet_NaN();
    constexpr float INF_VALUE = std::numeric_limits<float>::infinity();
    CHECK(radialDeadzone(StickPosition{NAN_VALUE, 0.0F}) == StickPosition{});
    CHECK(radialDeadzone(StickPosition{INF_VALUE, 0.0F}) == StickPosition{});
}

TEST_CASE("gamepad: a stick reads its own axes, down is positive, and the raw axis has no deadzone (GP11)") {
    InputState in;
    in.process(connected(0));
    in.process(axis(0, GamepadAxis::LeftY, -32768));
    CHECK(in.gamepadStick(GamepadStick::Left) == StickPosition{0.0F, -1.0F});
    CHECK(in.gamepadStick(GamepadStick::Right) == StickPosition{});
    in.process(axis(0, GamepadAxis::RightX, 32767));
    CHECK(in.gamepadStick(GamepadStick::Right) == StickPosition{1.0F, 0.0F});

    in.process(axis(0, GamepadAxis::LeftY, 0));
    in.process(axis(0, GamepadAxis::LeftX, 4000));
    CHECK(in.gamepadStick(GamepadStick::Left) == StickPosition{});
    CHECK(in.gamepadAxis(GamepadAxis::LeftX) == 4000.0F / 32767.0F);

    CHECK(in.gamepadStick(GamepadStick::Left, 2) == StickPosition{});
}

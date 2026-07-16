// Aero Engine — InputState unit tests (task 0.3.2). Pure fold: construct an InputState, feed it
// engine Events, assert. No Context, no SDL, no display — this is why InputState is SDL-free (D4).
#include <aero/platform/input.hpp>

#include <doctest/doctest.h>

namespace {
using engine::platform::Event;
using engine::platform::EventType;
using engine::platform::InputState;
using engine::platform::Key;
using engine::platform::KeyMods;
using engine::platform::MouseButton;

Event keyEvent(EventType type, Key code, bool repeat = false) {
    Event e;
    e.type = type;
    e.key = engine::platform::KeyData{code, KeyMods{}, repeat};
    return e;
}
Event buttonEvent(EventType type, MouseButton button, float x = 0.0f, float y = 0.0f) {
    Event e;
    e.type = type;
    e.mouseButton = engine::platform::MouseButtonData{button, x, y, 1};
    return e;
}
Event motionEvent(float x, float y, float dx, float dy) {
    Event e;
    e.type = EventType::MouseMoved;
    e.mouseMotion = engine::platform::MouseMotionData{x, y, dx, dy};
    return e;
}
Event wheelEvent(float x, float y) {
    Event e;
    e.type = EventType::MouseWheel;
    e.mouseWheel = engine::platform::MouseWheelData{x, y};
    return e;
}
}  // namespace

TEST_CASE("keyDown reflects held state across frames") {
    InputState in;
    in.process(keyEvent(EventType::KeyDown, Key::W));
    CHECK(in.keyDown(Key::W));
    in.newFrame();
    CHECK(in.keyDown(Key::W));  // still held with no key-up
    in.process(keyEvent(EventType::KeyUp, Key::W));
    CHECK_FALSE(in.keyDown(Key::W));
}

TEST_CASE("keyPressed is true only on the frame the key goes down") {
    InputState in;
    in.process(keyEvent(EventType::KeyDown, Key::Space));
    CHECK(in.keyPressed(Key::Space));
    CHECK(in.keyDown(Key::Space));
    in.newFrame();
    CHECK_FALSE(in.keyPressed(Key::Space));  // edge cleared
    CHECK(in.keyDown(Key::Space));           // still held
}

TEST_CASE("keyPressed ignores auto-repeat") {
    InputState in;
    in.process(keyEvent(EventType::KeyDown, Key::A, /*repeat=*/false));
    in.newFrame();
    in.process(keyEvent(EventType::KeyDown, Key::A, /*repeat=*/true));  // OS auto-repeat
    CHECK_FALSE(in.keyPressed(Key::A));                                 // a repeat is not a new press
    CHECK(in.keyDown(Key::A));
}

TEST_CASE("keyReleased is true only on the frame the key goes up") {
    InputState in;
    in.process(keyEvent(EventType::KeyDown, Key::Q));
    in.newFrame();
    in.process(keyEvent(EventType::KeyUp, Key::Q));
    CHECK(in.keyReleased(Key::Q));
    in.newFrame();
    CHECK_FALSE(in.keyReleased(Key::Q));
}

TEST_CASE("a tap down-and-up within one frame records both edges") {
    InputState in;
    in.process(keyEvent(EventType::KeyDown, Key::E));
    in.process(keyEvent(EventType::KeyUp, Key::E));
    CHECK(in.keyPressed(Key::E));
    CHECK(in.keyReleased(Key::E));
    CHECK_FALSE(in.keyDown(Key::E));
}

TEST_CASE("focus loss clears held keys (no stuck keys on alt-tab)") {
    InputState in;
    in.process(keyEvent(EventType::KeyDown, Key::W));
    in.process(keyEvent(EventType::KeyDown, Key::LeftShift));
    CHECK(in.keyDown(Key::W));
    Event focusLost;
    focusLost.type = EventType::WindowFocusLost;
    in.process(focusLost);
    CHECK_FALSE(in.keyDown(Key::W));
    CHECK_FALSE(in.keyDown(Key::LeftShift));
}

TEST_CASE("mouse buttons: down / pressed / released across frames") {
    InputState in;
    in.process(buttonEvent(EventType::MouseButtonDown, MouseButton::Right, 10.0f, 20.0f));
    CHECK(in.mouseButtonDown(MouseButton::Right));
    CHECK(in.mouseButtonPressed(MouseButton::Right));
    CHECK(in.mousePosition().x == doctest::Approx(10.0f));
    in.newFrame();
    CHECK(in.mouseButtonDown(MouseButton::Right));           // still held
    CHECK_FALSE(in.mouseButtonPressed(MouseButton::Right));  // edge cleared
    in.process(buttonEvent(EventType::MouseButtonUp, MouseButton::Right, 10.0f, 20.0f));
    CHECK_FALSE(in.mouseButtonDown(MouseButton::Right));
    CHECK(in.mouseButtonReleased(MouseButton::Right));
}

TEST_CASE("mouse position persists; delta and wheel accumulate then reset") {
    InputState in;
    in.process(motionEvent(100.0f, 50.0f, 4.0f, 2.0f));
    in.process(motionEvent(103.0f, 51.0f, 3.0f, 1.0f));  // deltas sum within the frame
    CHECK(in.mousePosition().x == doctest::Approx(103.0f));
    CHECK(in.mouseDelta().x == doctest::Approx(7.0f));
    CHECK(in.mouseDelta().y == doctest::Approx(3.0f));
    in.process(wheelEvent(0.0f, 2.0f));
    in.process(wheelEvent(0.0f, 1.0f));
    CHECK(in.mouseWheel().y == doctest::Approx(3.0f));
    in.newFrame();
    CHECK(in.mousePosition().x == doctest::Approx(103.0f));  // position persists
    CHECK(in.mouseDelta().x == doctest::Approx(0.0f));       // delta reset
    CHECK(in.mouseWheel().y == doctest::Approx(0.0f));       // wheel reset
}

TEST_CASE("out-of-range key queries are safe") {
    const InputState in;
    CHECK_FALSE(in.keyDown(Key::Unknown));
    CHECK_FALSE(in.keyDown(Key::Count));  // sentinel index == array size -> guarded
    CHECK_FALSE(in.keyPressed(Key::Count));
    CHECK_FALSE(in.mouseButtonDown(MouseButton::Count));
}

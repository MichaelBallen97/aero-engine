// Aero Engine -- virtual gamepads and synthetic events through a real Context (task 4.4.3).
// WHITE-BOX: this TU includes <SDL3/SDL.h> like platform_event_test.cpp. Every case builds a headless Context and
// creates no window, so SDL's focus filter drops nothing. SDL keeps one process-wide state, so a case holds exactly
// one Context at a time (the one-per-process rule).
#include <aero/core/log.hpp>
#include <aero/platform/platform.hpp>

#include "script_binding_support.hpp"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using engine::platform::Context;
using engine::platform::Event;
using engine::platform::EventType;
using engine::platform::GamepadAxis;
using engine::platform::GamepadButton;
using engine::platform::GamepadDrivers;

// A pad SDL gives its standard mapping: type GAMEPAD, 15 buttons (South..DpadRight), 6 axes, both masks 0, so SDL
// fills them in enum order (SDL_virtualjoystick.c:197-213). Attached after the Context started the subsystem, it
// queues JOYSTICK_ADDED then GAMEPAD_ADDED at once (Q4a).
struct VirtualPad {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;  // the test's own reference; the Context holds its own on the gamepad

    VirtualPad() = default;
    VirtualPad(const VirtualPad&) = delete;
    VirtualPad& operator=(const VirtualPad&) = delete;
    VirtualPad(VirtualPad&&) = delete;
    VirtualPad& operator=(VirtualPad&&) = delete;

    void attach() {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.nbuttons = 15;
        desc.naxes = 6;
        id = SDL_AttachVirtualJoystick(&desc);
        REQUIRE(id != 0);
    }
    // After the Context's GamepadConnected: open the test's reference, then teach SDL the rest of every axis
    // (S3: the first value SDL sees becomes the rest). Joystick-level triggers rest at -32768, which the mapping
    // reads as 0 (SDL_virtualjoystick.c:248-254). The caller then drains: the first update after an open delivers
    // every axis twice (Q4b), which the fold absorbs.
    void openAndPrime() {
        joystick = SDL_OpenJoystick(id);
        REQUIRE(joystick != nullptr);
        for (int axis = 0; axis < 4; ++axis) {
            SDL_SetJoystickVirtualAxis(joystick, axis, 0);
        }
        SDL_SetJoystickVirtualAxis(joystick, 4, -32768);
        SDL_SetJoystickVirtualAxis(joystick, 5, -32768);
    }
    void button(int index, bool down) const { SDL_SetJoystickVirtualButton(joystick, index, down); }
    void axis(int index, std::int16_t value) const { SDL_SetJoystickVirtualAxis(joystick, index, value); }
    void closeJoystick() {
        if (joystick != nullptr) {
            SDL_CloseJoystick(joystick);
            joystick = nullptr;
        }
    }
    void detach() {
        if (id != 0) {
            SDL_DetachVirtualJoystick(id);
            id = 0;
        }
    }
    void forget() noexcept {  // the Context is going away with SDL: touch nothing
        id = 0;
        joystick = nullptr;
    }
    ~VirtualPad() {
        closeJoystick();
        detach();
    }
};

std::vector<Event> drain(Context& ctx) {
    std::vector<Event> out;
    Event ev;
    while (ctx.pollEvent(ev)) {
        out.push_back(ev);
    }
    return out;
}

std::vector<Event> ofType(const std::vector<Event>& events, EventType type) {
    std::vector<Event> out;
    for (const Event& ev : events) {
        if (ev.type == type) {
            out.push_back(ev);
        }
    }
    return out;
}

// Only the connect and disconnect events, in order, as (type, slot) pairs.
struct DeviceEvent {
    EventType type = EventType::Quit;
    std::uint8_t slot = 0;
    bool operator==(const DeviceEvent&) const = default;
};
std::vector<DeviceEvent> deviceEvents(const std::vector<Event>& events) {
    std::vector<DeviceEvent> out;
    for (const Event& ev : events) {
        if (ev.type == EventType::GamepadConnected || ev.type == EventType::GamepadDisconnected) {
            out.push_back(DeviceEvent{ev.type, ev.gamepad.slot});
        }
    }
    return out;
}

SDL_Event gamepadButtonEvent(SDL_JoystickID id, int button, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = id;
    e.gbutton.button = static_cast<Uint8>(button);
    e.gbutton.down = down;
    return e;
}

SDL_Event gamepadAxisEvent(SDL_JoystickID id, int axis, std::int16_t value) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = id;
    e.gaxis.axis = static_cast<Uint8>(axis);
    e.gaxis.value = value;
    return e;
}

SDL_Event gamepadDeviceEvent(SDL_EventType type, SDL_JoystickID id) {
    SDL_Event e{};
    e.type = type;
    e.gdevice.which = id;
    return e;
}

constexpr engine::platform::ContextConfig VIRTUAL_ONLY{.headless = true, .gamepads = GamepadDrivers::VirtualOnly};

// Restores the process environment SDL copies at SDL_GetEnvironment(): the variables are set for one case only.
struct EnvVariable {
    const char* name;
    explicit EnvVariable(const char* variable, const char* value) : name(variable) {
        SDL_SetEnvironmentVariable(SDL_GetEnvironment(), name, value, true);
    }
    EnvVariable(const EnvVariable&) = delete;
    EnvVariable& operator=(const EnvVariable&) = delete;
    EnvVariable(EnvVariable&&) = delete;
    EnvVariable& operator=(EnvVariable&&) = delete;
    ~EnvVariable() { SDL_UnsetEnvironmentVariable(SDL_GetEnvironment(), name); }
};

bool hintReads(const char* name, const char* expected) {
    const char* value = SDL_GetHint(name);
    return value != nullptr && std::string_view{value} == expected;
}

bool virtualOnlyHintsRead() {
    return hintReads(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0xFFFF/0xFFFF") &&
           hintReads(SDL_HINT_JOYSTICK_HIDAPI, "0") && hintReads(SDL_HINT_JOYSTICK_IOKIT, "0") &&
           hintReads(SDL_HINT_JOYSTICK_MFI, "0") && hintReads(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
}

double millisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
}  // namespace

TEST_CASE("pump: the default config starts no gamepad subsystem and drops a gamepad event (GP12)") {
    Context ctx{{.headless = true}};
    REQUIRE(ctx.valid());
    CHECK(SDL_WasInit(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK) == 0);
    drain(ctx);
    SDL_Event added = gamepadDeviceEvent(SDL_EVENT_GAMEPAD_ADDED, 1);
    REQUIRE(SDL_PushEvent(&added));
    CHECK(drain(ctx).empty());
    CHECK_FALSE(ctx.input().gamepadConnected(0));
}

TEST_CASE("pump: VirtualOnly sets its hints at override priority on every construction (GP13)") {
    double offMs = 0.0;
    double virtualMs = 0.0;
    {
        const auto start = std::chrono::steady_clock::now();
        const Context ctx{{.headless = true}};
        offMs = millisecondsSince(start);
        REQUIRE(ctx.valid());
        CHECK_FALSE(hintReads(SDL_HINT_JOYSTICK_HIDAPI, "0"));
    }
    {
        const auto start = std::chrono::steady_clock::now();
        const Context ctx{VIRTUAL_ONLY};
        virtualMs = millisecondsSince(start);
        REQUIRE(ctx.valid());
        CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) != 0);
        CHECK(virtualOnlyHintsRead());
    }
    CHECK(SDL_WasInit(0) == 0);
    MESSAGE("Context construction, gamepads Off: ", offMs, " ms");
    MESSAGE("Context construction, gamepads VirtualOnly: ", virtualMs, " ms");
    {
        // SDL_Quit cleared every hint; a second Context sets them again.
        const Context ctx{VIRTUAL_ONLY};
        REQUIRE(ctx.valid());
        CHECK(virtualOnlyHintsRead());
    }
    {
        // A shell variable cannot let a real pad into a test: override priority beats the environment.
        const EnvVariable hidapi{"SDL_JOYSTICK_HIDAPI", "1"};
        const EnvVariable allow{"SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "0x045E/0x028E"};
        const Context ctx{VIRTUAL_ONLY};
        REQUIRE(ctx.valid());
        CHECK(virtualOnlyHintsRead());
    }
}

TEST_CASE("pump: two virtual pads connect into slots 0 and 1 and the Context opened them (GP14)") {
    Context ctx{VIRTUAL_ONLY};
    REQUIRE(ctx.valid());
    drain(ctx);
    VirtualPad a;
    VirtualPad b;
    a.attach();
    b.attach();
    const std::vector<DeviceEvent> events = deviceEvents(drain(ctx));
    REQUIRE(events.size() == 2);
    CHECK(events[0] == DeviceEvent{EventType::GamepadConnected, 0});
    CHECK(events[1] == DeviceEvent{EventType::GamepadConnected, 1});
    CHECK(ctx.input().gamepadConnected(0));
    CHECK(ctx.input().gamepadConnected(1));
    CHECK_FALSE(ctx.input().gamepadConnected(2));
    CHECK(SDL_GetGamepadFromID(a.id) != nullptr);
    CHECK(SDL_GetGamepadFromID(b.id) != nullptr);
}

TEST_CASE("pump: a button on the slot-1 pad surfaces in slot 1 only (GP15)") {
    Context ctx{VIRTUAL_ONLY};
    REQUIRE(ctx.valid());
    drain(ctx);
    VirtualPad a;
    VirtualPad b;
    a.attach();
    b.attach();
    drain(ctx);
    b.openAndPrime();
    drain(ctx);

    ctx.newFrame();
    b.button(0, true);
    const std::vector<Event> down = ofType(drain(ctx), EventType::GamepadButtonDown);
    REQUIRE(down.size() == 1);
    CHECK(down[0].gamepadButton.slot == 1);
    CHECK(down[0].gamepadButton.button == GamepadButton::South);
    CHECK(ctx.input().gamepadButtonPressed(GamepadButton::South, 1));
    CHECK(ctx.input().gamepadButtonDown(GamepadButton::South, 1));
    CHECK_FALSE(ctx.input().gamepadButtonDown(GamepadButton::South, 0));
    CHECK_FALSE(ctx.input().gamepadButtonPressed(GamepadButton::South, 0));

    ctx.newFrame();
    b.button(0, false);
    const std::vector<Event> up = ofType(drain(ctx), EventType::GamepadButtonUp);
    REQUIRE(up.size() == 1);
    CHECK(up[0].gamepadButton.slot == 1);
    CHECK(up[0].gamepadButton.button == GamepadButton::South);
    CHECK(ctx.input().gamepadButtonReleased(GamepadButton::South, 1));
    CHECK_FALSE(ctx.input().gamepadButtonDown(GamepadButton::South, 1));
    CHECK_FALSE(ctx.input().gamepadButtonReleased(GamepadButton::South, 0));
}

TEST_CASE("pump: every backend button and axis maps to the enumerator of the same position (GP16)") {
    Context ctx{VIRTUAL_ONLY};
    REQUIRE(ctx.valid());
    drain(ctx);
    VirtualPad a;
    a.attach();
    drain(ctx);
    REQUIRE(ctx.input().gamepadConnected(0));

    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
        SDL_Event e = gamepadButtonEvent(a.id, button, true);
        REQUIRE(SDL_PushEvent(&e));
        const std::vector<Event> events = drain(ctx);
        REQUIRE(events.size() == 1);
        CHECK(events[0].type == EventType::GamepadButtonDown);
        CHECK(events[0].gamepadButton.slot == 0);
        CHECK(static_cast<int>(events[0].gamepadButton.button) == button);
    }
    // SDL puts RIGHT_PADDLE1 before LEFT_PADDLE1; the engine names them by the same positions.
    SDL_Event rightPaddle = gamepadButtonEvent(a.id, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, false);
    REQUIRE(SDL_PushEvent(&rightPaddle));
    std::vector<Event> events = drain(ctx);
    REQUIRE(events.size() == 1);
    CHECK(events[0].gamepadButton.button == GamepadButton::RightPaddle1);
    SDL_Event leftPaddle = gamepadButtonEvent(a.id, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, false);
    REQUIRE(SDL_PushEvent(&leftPaddle));
    events = drain(ctx);
    REQUIRE(events.size() == 1);
    CHECK(events[0].gamepadButton.button == GamepadButton::LeftPaddle1);

    SDL_Event beyondButtons = gamepadButtonEvent(a.id, SDL_GAMEPAD_BUTTON_COUNT, true);
    REQUIRE(SDL_PushEvent(&beyondButtons));
    CHECK(drain(ctx).empty());

    for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
        const auto value = static_cast<std::int16_t>(1000 + axis);
        SDL_Event e = gamepadAxisEvent(a.id, axis, value);
        REQUIRE(SDL_PushEvent(&e));
        events = drain(ctx);
        REQUIRE(events.size() == 1);
        CHECK(events[0].type == EventType::GamepadAxisMoved);
        CHECK(events[0].gamepadAxis.slot == 0);
        CHECK(static_cast<int>(events[0].gamepadAxis.axis) == axis);
        CHECK(events[0].gamepadAxis.value == value);
    }
    SDL_Event beyondAxes = gamepadAxisEvent(a.id, SDL_GAMEPAD_AXIS_COUNT, 1000);
    REQUIRE(SDL_PushEvent(&beyondAxes));
    CHECK(drain(ctx).empty());
}

TEST_CASE("pump: sticks reach exactly -1 and +1 and a trigger is read at the gamepad level (GP17)") {
    Context ctx{VIRTUAL_ONLY};
    REQUIRE(ctx.valid());
    drain(ctx);
    VirtualPad a;
    a.attach();
    drain(ctx);
    a.openAndPrime();
    drain(ctx);
    REQUIRE(ctx.input().gamepadConnected(0));
    CHECK(ctx.input().gamepadAxis(GamepadAxis::LeftTrigger, 0) == 0.0F);  // a released trigger is not -1

    a.axis(0, -32768);
    std::vector<Event> events = ofType(drain(ctx), EventType::GamepadAxisMoved);
    REQUIRE(events.size() == 1);
    CHECK(events[0].gamepadAxis.axis == GamepadAxis::LeftX);
    CHECK(events[0].gamepadAxis.value == -32768);
    CHECK(ctx.input().gamepadAxis(GamepadAxis::LeftX, 0) == -1.0F);

    a.axis(0, 32767);
    events = ofType(drain(ctx), EventType::GamepadAxisMoved);
    REQUIRE(events.size() == 1);
    CHECK(events[0].gamepadAxis.value == 32767);
    CHECK(ctx.input().gamepadAxis(GamepadAxis::LeftX, 0) == 1.0F);

    a.axis(1, -32768);
    events = ofType(drain(ctx), EventType::GamepadAxisMoved);
    REQUIRE(events.size() == 1);
    CHECK(events[0].gamepadAxis.axis == GamepadAxis::LeftY);
    CHECK(events[0].gamepadAxis.value == -32768);
    CHECK(ctx.input().gamepadAxis(GamepadAxis::LeftY, 0) == -1.0F);

    a.axis(4, 32767);
    events = ofType(drain(ctx), EventType::GamepadAxisMoved);
    REQUIRE(events.size() == 1);
    CHECK(events[0].gamepadAxis.axis == GamepadAxis::LeftTrigger);
    CHECK(events[0].gamepadAxis.value == 32767);
    CHECK(ctx.input().gamepadAxis(GamepadAxis::LeftTrigger, 0) == 1.0F);
}

TEST_CASE("pump: a detach while held releases first, then disconnects, and the Context closes its pad (GP18)") {
    Context ctx{VIRTUAL_ONLY};
    REQUIRE(ctx.valid());
    drain(ctx);
    VirtualPad a;
    a.attach();
    drain(ctx);
    a.openAndPrime();
    drain(ctx);
    a.button(0, true);
    a.button(9, true);
    a.axis(0, 20000);
    drain(ctx);
    REQUIRE(ctx.input().gamepadButtonDown(GamepadButton::South, 0));
    REQUIRE(ctx.input().gamepadButtonDown(GamepadButton::LeftShoulder, 0));
    REQUIRE(ctx.input().gamepadAxis(GamepadAxis::LeftX, 0) > 0.0F);

    ctx.newFrame();
    const SDL_JoystickID id = a.id;
    a.detach();
    const std::vector<Event> events = drain(ctx);
    std::optional<std::size_t> disconnect;
    std::size_t upsBefore = 0;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].type == EventType::GamepadDisconnected) {
            disconnect = i;
        } else if (events[i].type == EventType::GamepadButtonUp && !disconnect.has_value()) {
            ++upsBefore;
        }
    }
    REQUIRE(disconnect.has_value());
    CHECK(events[*disconnect].gamepad.slot == 0);
    CHECK(upsBefore >= 2);
    CHECK_FALSE(ctx.input().gamepadConnected(0));
    CHECK_FALSE(ctx.input().gamepadButtonDown(GamepadButton::South, 0));
    CHECK_FALSE(ctx.input().gamepadButtonDown(GamepadButton::LeftShoulder, 0));
    CHECK(ctx.input().gamepadButtonReleased(GamepadButton::South, 0));
    CHECK(ctx.input().gamepadButtonReleased(GamepadButton::LeftShoulder, 0));
    CHECK(ctx.input().gamepadAxis(GamepadAxis::LeftX, 0) == 0.0F);

    a.closeJoystick();
    CHECK(SDL_GetGamepadFromID(id) == nullptr);
}

TEST_CASE("pump: pads past slot 3 wait, and the longest waiter takes the next freed slot (GP19)") {
    Context ctx{VIRTUAL_ONLY};
    REQUIRE(ctx.valid());
    drain(ctx);
    std::array<VirtualPad, 6> pads;
    for (VirtualPad& pad : pads) {
        pad.attach();
    }
    const std::vector<DeviceEvent> connects = deviceEvents(drain(ctx));
    REQUIRE(connects.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(connects[i] == DeviceEvent{EventType::GamepadConnected, static_cast<std::uint8_t>(i)});
        CHECK(ctx.input().gamepadConnected(i));
    }
    CHECK(SDL_GetGamepadFromID(pads[4].id) == nullptr);
    CHECK(SDL_GetGamepadFromID(pads[5].id) == nullptr);

    pads[2].detach();
    const std::vector<DeviceEvent> swap = deviceEvents(drain(ctx));
    REQUIRE(swap.size() == 2);
    CHECK(swap[0] == DeviceEvent{EventType::GamepadDisconnected, 2});
    CHECK(swap[1] == DeviceEvent{EventType::GamepadConnected, 2});
    CHECK(SDL_GetGamepadFromID(pads[4].id) != nullptr);
    CHECK(SDL_GetGamepadFromID(pads[5].id) == nullptr);
    pads[4].openAndPrime();
    drain(ctx);
    ctx.newFrame();
    pads[4].button(0, true);
    drain(ctx);
    CHECK(ctx.input().gamepadButtonDown(GamepadButton::South, 2));

    pads[0].detach();
    const std::vector<DeviceEvent> second = deviceEvents(drain(ctx));
    REQUIRE(second.size() == 2);
    CHECK(second[0] == DeviceEvent{EventType::GamepadDisconnected, 0});
    CHECK(second[1] == DeviceEvent{EventType::GamepadConnected, 0});
    pads[5].openAndPrime();
    drain(ctx);
    ctx.newFrame();
    pads[5].button(0, true);
    drain(ctx);
    CHECK(ctx.input().gamepadButtonDown(GamepadButton::South, 0));
}

TEST_CASE("pump: a waiting pad's events, a failed open and an unknown removal change nothing (GP20)") {
    Context ctx{VIRTUAL_ONLY};
    REQUIRE(ctx.valid());
    drain(ctx);
    const script_test::LogCapture capture;
    std::array<VirtualPad, 5> pads;
    pads[0].attach();
    pads[1].attach();
    drain(ctx);

    // A free slot exists, so the unknown id is opened at once; the open fails.
    constexpr SDL_JoystickID UNKNOWN_ID = 0xFFFFFFF0U;
    SDL_Event added = gamepadDeviceEvent(SDL_EVENT_GAMEPAD_ADDED, UNKNOWN_ID);
    REQUIRE(SDL_PushEvent(&added));
    CHECK(drain(ctx).empty());
    const auto warnings = capture.atLeast(engine::LogLevel::Warn);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].message.find(std::to_string(UNKNOWN_ID)) != std::string::npos);
    CHECK_FALSE(ctx.input().gamepadConnected(2));

    // Fill the remaining slots and queue a fifth pad.
    pads[2].attach();
    pads[3].attach();
    pads[4].attach();
    CHECK(deviceEvents(drain(ctx)).size() == 2);
    REQUIRE(ctx.input().gamepadConnected(3));
    CHECK(SDL_GetGamepadFromID(pads[4].id) == nullptr);

    SDL_Event press = gamepadButtonEvent(pads[4].id, 0, true);
    REQUIRE(SDL_PushEvent(&press));
    SDL_Event axis = gamepadAxisEvent(pads[4].id, 0, 20000);
    REQUIRE(SDL_PushEvent(&axis));
    CHECK(drain(ctx).empty());
    for (std::size_t slot = 0; slot < 4; ++slot) {
        CHECK_FALSE(ctx.input().gamepadButtonDown(GamepadButton::South, slot));
        CHECK(ctx.input().gamepadAxis(GamepadAxis::LeftX, slot) == 0.0F);
    }

    SDL_Event removed = gamepadDeviceEvent(SDL_EVENT_GAMEPAD_REMOVED, 0xFFFFFFF1U);
    REQUIRE(SDL_PushEvent(&removed));
    CHECK(drain(ctx).empty());
    for (std::size_t slot = 0; slot < 4; ++slot) {
        CHECK(ctx.input().gamepadConnected(slot));
    }
    CHECK(capture.countAtLeast(engine::LogLevel::Warn) == 1);
}

TEST_CASE("pump: a Context destroyed with pads slotted and waiting leaves SDL down and the next one clean (GP21)") {
    {
        Context ctx{VIRTUAL_ONLY};
        REQUIRE(ctx.valid());
        drain(ctx);
        std::array<VirtualPad, 5> pads;
        for (VirtualPad& pad : pads) {
            pad.attach();
        }
        CHECK(deviceEvents(drain(ctx)).size() == 4);
        for (VirtualPad& pad : pads) {
            pad.forget();
        }
    }
    CHECK(SDL_WasInit(0) == 0);

    Context ctx{VIRTUAL_ONLY};
    REQUIRE(ctx.valid());
    CHECK(drain(ctx).empty());
    VirtualPad pad;
    pad.attach();
    const std::vector<Event> events = drain(ctx);
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == EventType::GamepadConnected);
    CHECK(events[0].gamepad.slot == 0);
    CHECK_FALSE(ctx.input().gamepadConnected(1));
}

TEST_CASE("pump: System mode sets no hint and starts and stops the subsystem on macOS (GP22)") {
    if (std::string_view{SDL_GetPlatform()} != "macOS") {
        MESSAGE("GP22 runs on macOS only; nothing is asserted on ", SDL_GetPlatform());
        return;
    }
    {
        Context ctx{{.headless = true, .gamepads = GamepadDrivers::System}};
        REQUIRE(ctx.valid());
        CHECK(SDL_WasInit(SDL_INIT_GAMEPAD) != 0);
        CHECK(SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT) == nullptr);
        drain(ctx);
    }
    CHECK(SDL_WasInit(0) == 0);
}

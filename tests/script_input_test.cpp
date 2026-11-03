// Aero Engine -- engine.input: the name tables, the binding and the functions (task 4.4.3; SI1-SI17).
//
// SI3-SI11 drive the keyboard and mouse functions through a VM over the built-in table and a test-owned InputState fed
// Events directly (the 0.3.2 pure tier: no Context, no SDL).
//
// The five name tables (engine/script/internal/aero/script/internal/input_names.hpp) are the script-visible spelling
// of the platform enums. They are written by hand, in enum order, and SI1 holds them to the enums' own text so a
// misspelt, reordered or forgotten name reddens here and nowhere later. SI2 holds the lookups to an exact,
// case-sensitive match.

#include <aero/core/log.hpp>
#include <aero/core/vfs.hpp>
#include <aero/platform/event.hpp>
#include <aero/platform/input.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/internal/input_names.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <ostream>  // a CHECK on std::string_view needs the complete std::ostream on MSVC (ci-portability.md)
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace names = engine::script::internal;
namespace platform = engine::platform;

// ---- SI1's reader: an enum's enumerators, read off event.hpp as text. Iterative: misc-no-recursion. -----------------

struct EnumText {
    bool found = false;
    std::vector<std::string> enumerators;  // in declaration order, without any `= value`
    std::string firstValue;                // the first enumerator's `= value` text, empty when it has none
};

[[nodiscard]] std::string trimmed(std::string_view text) {
    constexpr std::string_view SPACE = " \t\r\n";
    const std::size_t first = text.find_first_not_of(SPACE);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(SPACE);
    return std::string{text.substr(first, last - first + 1)};
}

[[nodiscard]] EnumText readEnum(const std::string& source, std::string_view name) {
    EnumText result;
    const std::string head = "enum class " + std::string{name} + " : std::uint8_t {";
    const std::size_t open = source.find(head);
    if (open == std::string::npos) {
        return result;
    }
    const std::size_t bodyStart = open + head.size();
    const std::size_t close = source.find("};", bodyStart);
    if (close == std::string::npos) {
        return result;
    }
    // Drop each line's `//` tail, then split the rest on commas.
    std::string body;
    std::size_t position = bodyStart;
    while (position < close) {
        std::size_t newline = source.find('\n', position);
        if (newline == std::string::npos || newline > close) {
            newline = close;
        }
        std::string line = source.substr(position, newline - position);
        const std::size_t comment = line.find("//");
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        body += line;
        body += '\n';
        position = newline + 1;
    }
    std::size_t start = 0;
    while (start <= body.size()) {
        const std::size_t comma = body.find(',', start);
        const std::size_t stop = comma == std::string::npos ? body.size() : comma;
        std::string piece = trimmed(std::string_view{body}.substr(start, stop - start));
        const std::size_t equals = piece.find('=');
        if (equals != std::string::npos) {
            if (result.enumerators.empty()) {
                result.firstValue = trimmed(std::string_view{piece}.substr(equals + 1));
            }
            piece = trimmed(std::string_view{piece}.substr(0, equals));
        }
        if (!piece.empty()) {
            result.enumerators.push_back(piece);
        }
        start = stop + 1;
    }
    result.found = true;
    return result;
}

// `table` must spell the enumerators from `skipFront` up to, not including, the final `Count`, in order.
template <std::size_t N>
void checkTable(const std::string& source, std::string_view enumName, const std::array<std::string_view, N>& table,
                std::size_t skipFront) {
    CAPTURE(enumName);
    const EnumText parsed = readEnum(source, enumName);
    REQUIRE(parsed.found);
    CHECK(parsed.firstValue == "0");
    REQUIRE(parsed.enumerators.size() >= skipFront + 1U);
    CHECK(parsed.enumerators.back() == "Count");
    const std::size_t named = parsed.enumerators.size() - skipFront - 1U;
    REQUIRE(named == N);
    for (std::size_t i = 0; i < N; ++i) {
        CAPTURE(i);
        CHECK(parsed.enumerators[skipFront + i] == table[i]);
    }
}

}  // namespace

TEST_CASE("input: the five name tables spell the platform enums' enumerators in order (SI1)") {
    std::ifstream in{std::string{AERO_PLATFORM_INCLUDE_DIR} + "/aero/platform/event.hpp", std::ios::binary};
    REQUIRE(in.is_open());
    const std::string source{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};

    // Key opens with Unknown, which has no name.
    const EnumText keys = readEnum(source, "Key");
    REQUIRE(keys.found);
    REQUIRE(keys.enumerators.size() >= 2U);
    CHECK(keys.enumerators.front() == "Unknown");
    CHECK(names::KEY_NAMES.size() == 102U);
    checkTable(source, "Key", names::KEY_NAMES, 1);
    checkTable(source, "MouseButton", names::MOUSE_BUTTON_NAMES, 0);
    checkTable(source, "GamepadButton", names::GAMEPAD_BUTTON_NAMES, 0);
    checkTable(source, "GamepadAxis", names::GAMEPAD_AXIS_NAMES, 0);
    checkTable(source, "GamepadStick", names::GAMEPAD_STICK_NAMES, 0);
}

namespace {

// Every entry looks up to the enumerator at its index (`offset` is 1 for Key, whose first value has no name).
template <typename Enum, std::size_t N, typename Lookup>
void checkRoundTrip(const std::array<std::string_view, N>& table, Lookup lookup, std::size_t offset) {
    for (std::size_t i = 0; i < N; ++i) {
        CAPTURE(table[i]);
        const std::optional<Enum> found = lookup(table[i]);
        REQUIRE(found.has_value());
        CHECK(static_cast<std::size_t>(*found) == i + offset);
    }
}

static_assert(names::keyFromName("W") == platform::Key::W);
static_assert(names::keyFromName("Pause") == platform::Key::Pause);
static_assert(!names::keyFromName("w").has_value());
static_assert(names::mouseButtonFromName("X2") == platform::MouseButton::X2);
static_assert(names::gamepadButtonFromName("South") == platform::GamepadButton::South);
static_assert(names::gamepadAxisFromName("RightTrigger") == platform::GamepadAxis::RightTrigger);
static_assert(names::gamepadStickFromName("Right") == platform::GamepadStick::Right);

}  // namespace

TEST_CASE("input: a name looks up to its enumerator and to nothing else (SI2)") {
    checkRoundTrip<platform::Key>(names::KEY_NAMES, names::keyFromName, 1);
    checkRoundTrip<platform::MouseButton>(names::MOUSE_BUTTON_NAMES, names::mouseButtonFromName, 0);
    checkRoundTrip<platform::GamepadButton>(names::GAMEPAD_BUTTON_NAMES, names::gamepadButtonFromName, 0);
    checkRoundTrip<platform::GamepadAxis>(names::GAMEPAD_AXIS_NAMES, names::gamepadAxisFromName, 0);
    checkRoundTrip<platform::GamepadStick>(names::GAMEPAD_STICK_NAMES, names::gamepadStickFromName, 0);

    // Exact and case-sensitive: no prefix, no trim, no NUL-terminated match, no sentinel, and a name of another table.
    CHECK_FALSE(names::keyFromName("").has_value());
    CHECK_FALSE(names::keyFromName("Unknown").has_value());
    CHECK_FALSE(names::keyFromName("Count").has_value());
    CHECK_FALSE(names::keyFromName("w").has_value());
    CHECK_FALSE(names::keyFromName("W ").has_value());
    CHECK_FALSE(names::keyFromName(" W").has_value());
    CHECK_FALSE(names::keyFromName("space").has_value());
    CHECK_FALSE(names::keyFromName("Spac").has_value());
    CHECK_FALSE(names::keyFromName(std::string_view{"W\0", 2}).has_value());
    CHECK_FALSE(names::keyFromName("South").has_value());
    CHECK_FALSE(names::mouseButtonFromName("Count").has_value());
    CHECK_FALSE(names::mouseButtonFromName("left").has_value());
    CHECK_FALSE(names::gamepadButtonFromName("Left").has_value());
    CHECK_FALSE(names::gamepadButtonFromName("Count").has_value());
    CHECK_FALSE(names::gamepadAxisFromName("Left").has_value());
    CHECK_FALSE(names::gamepadAxisFromName("Count").has_value());
    CHECK_FALSE(names::gamepadStickFromName("LeftX").has_value());
    CHECK_FALSE(names::gamepadStickFromName("Count").has_value());
    CHECK_FALSE(names::gamepadStickFromName("").has_value());
}

// ---- SI3-SI11: the binding and the keyboard and mouse functions ---------------------------------------------------

namespace {

using engine::LogLevel;
using engine::script::BindStatus;
using engine::script::Vm;

[[nodiscard]] platform::Event keyEvent(platform::EventType type, platform::Key key, bool repeat = false) {
    platform::Event event;
    event.type = type;
    event.key = platform::KeyData{key, {}, repeat};
    return event;
}

[[nodiscard]] platform::Event buttonEvent(platform::EventType type, platform::MouseButton button) {
    platform::Event event;
    event.type = type;
    event.mouseButton = platform::MouseButtonData{button, 1.0F, 2.0F, 1};
    return event;
}

[[nodiscard]] platform::Event motionEvent(float x, float y, float dx, float dy) {
    platform::Event event;
    event.type = platform::EventType::MouseMoved;
    event.mouseMotion = platform::MouseMotionData{x, y, dx, dy};
    return event;
}

[[nodiscard]] platform::Event wheelEvent(float x, float y) {
    platform::Event event;
    event.type = platform::EventType::MouseWheel;
    event.mouseWheel = platform::MouseWheelData{x, y};
    return event;
}

void press(platform::InputState& state, platform::Key key, bool repeat = false) {
    state.process(keyEvent(platform::EventType::KeyDown, key, repeat));
}

void release(platform::InputState& state, platform::Key key) {
    state.process(keyEvent(platform::EventType::KeyUp, key));
}

// What an expression does: "ok:<String(value)>", or "<error name>|<error message>" when it throws.
[[nodiscard]] std::string outcome(Vm& vm, std::string_view expression) {
    const std::string source = "(() => { try { return 'ok:' + String(" + std::string{expression} +
                               "); } catch (e) { return e.name + '|' + e.message; } })()";
    return script_test::completion(vm, source);
}

struct InputFunctionRow {
    std::string_view name;
    int length;
    std::string_view validCall;  // arguments that pass every check but the binding
};

// Every engine.input function in definition order, with the length each is made with. The table they mirror is private
// to the script layer's sources; DT31 pins the same names and lengths to the declarations.
constexpr std::array<InputFunctionRow, 15> INPUT_ROWS{{{"keyDown", 1, "'W'"},
                                                       {"keyPressed", 1, "'W'"},
                                                       {"keyReleased", 1, "'W'"},
                                                       {"mouseButtonDown", 1, "'Left'"},
                                                       {"mouseButtonPressed", 1, "'Left'"},
                                                       {"mouseButtonReleased", 1, "'Left'"},
                                                       {"mousePosition", 0, ""},
                                                       {"mouseDelta", 0, ""},
                                                       {"mouseWheel", 0, ""},
                                                       {"gamepadConnected", 1, "0"},
                                                       {"gamepadButtonDown", 2, "'South', 0"},
                                                       {"gamepadButtonPressed", 2, "'South', 0"},
                                                       {"gamepadButtonReleased", 2, "'South', 0"},
                                                       {"gamepadAxis", 2, "'LeftX', 0"},
                                                       {"gamepadStick", 2, "'Left', 0"}}};

}  // namespace

TEST_CASE("input: bindInput and unbindInput refuse an invalid Vm and another thread, and keep the binding (SI3)") {
    script_test::LogCapture log;
    const platform::InputState state;
    Vm invalid;
    CHECK(invalid.bindInput(state) == BindStatus::InvalidVm);
    {
        const auto errors = log.atLeast(LogLevel::Error);
        REQUIRE(errors.size() == 1U);
        CHECK(errors[0].message == "script: bindInput on an invalid Vm (a failed create or a moved-from Vm)");
    }
    log.clear();
    invalid.unbindInput();
    CHECK(log.countAtLeast(LogLevel::Error) == 1U);

    Vm vm = script_test::makeBuiltinVm();
    platform::InputState held;
    press(held, platform::Key::W);
    platform::InputState other;
    log.clear();
    REQUIRE(vm.bindInput(held) == BindStatus::Ok);
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);  // success logs nothing

    BindStatus fromThread = BindStatus::Ok;
    std::thread worker{[&] {
        fromThread = vm.bindInput(other);
        vm.unbindInput();
    }};
    worker.join();
    CHECK(fromThread == BindStatus::WrongThread);
    {
        const auto errors = log.atLeast(LogLevel::Error);
        REQUIRE(errors.size() == 2U);
        CHECK(errors[0].message == "script: bindInput called from a thread that is not the VM's; refused");
        CHECK(errors[1].message == "script: unbindInput called from a thread that is not the VM's; refused");
    }
    CHECK(outcome(vm, "engine.input.keyDown('W')") == "ok:true");  // the earlier binding survived both refusals

    log.clear();
    vm.unbindInput();
    vm.unbindInput();  // idempotent, and silent
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

TEST_CASE("input: with no input bound every function throws after its arguments are checked (SI4)") {
    const script_test::LogCapture log;
    Vm vm = script_test::makeBuiltinVm();
    const platform::InputState state;
    const auto expectUnbound = [&vm] {
        for (const InputFunctionRow& row : INPUT_ROWS) {
            CAPTURE(row.name);
            const std::string call = "engine.input." + std::string{row.name} + "(" + std::string{row.validCall} + ")";
            CHECK(outcome(vm, call) == "TypeError|engine.input." + std::string{row.name} + ": no input is bound");
        }
        // The argument is checked first: a bad one names itself, whatever the binding.
        CHECK(outcome(vm, "engine.input.keyDown(3)") ==
              "TypeError|engine.input.keyDown: expected a string, got a number");
        CHECK(outcome(vm, "engine.input.keyDown('Nope')") == "RangeError|engine.input.keyDown: not a key name");
    };
    expectUnbound();  // before any bind
    REQUIRE(vm.bindInput(state) == BindStatus::Ok);
    CHECK(outcome(vm, "engine.input.keyDown('W')") == "ok:false");
    vm.unbindInput();
    expectUnbound();  // and after an unbind
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

TEST_CASE("input: every read is live against the bound state and edges clear with its frame (SI5)") {
    const script_test::LogCapture log;
    Vm vm = script_test::makeBuiltinVm();
    platform::InputState state;
    REQUIRE(vm.bindInput(state) == BindStatus::Ok);  // once: nothing below rebinds

    const auto read = [&vm] {
        return script_test::completion(vm,
                                       "[engine.input.keyDown('W'), engine.input.keyPressed('W'),"
                                       " engine.input.keyDown('Space'), engine.input.keyPressed('Space'),"
                                       " engine.input.keyDown('Escape'), engine.input.keyReleased('Escape'),"
                                       " engine.input.keyPressed('Tab'), engine.input.keyDown('Tab')].join()");
    };
    state.newFrame();  // frame 1
    press(state, platform::Key::W);
    press(state, platform::Key::Escape);
    CHECK(read() == "true,true,false,false,true,false,false,false");

    state.newFrame();  // frame 2
    press(state, platform::Key::Space);
    release(state, platform::Key::Escape);
    CHECK(read() == "true,false,true,true,false,true,false,false");

    state.newFrame();  // frame 3: the edges are gone, the held keys stay
    CHECK(read() == "true,false,true,false,false,false,false,false");

    // An auto-repeat of a key that is not down is held but never a press.
    state.newFrame();
    press(state, platform::Key::Tab, true);
    CHECK(read() == "true,false,true,false,false,false,false,true");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

TEST_CASE("input: each mouse button reads itself and no other (SI6)") {
    const script_test::LogCapture log;
    Vm vm = script_test::makeBuiltinVm();
    constexpr std::array<std::string_view, 5> BUTTONS{"Left", "Right", "Middle", "X1", "X2"};
    static_assert(BUTTONS.size() == static_cast<std::size_t>(platform::MouseButton::Count));
    for (std::size_t held = 0; held < BUTTONS.size(); ++held) {
        CAPTURE(BUTTONS[held]);
        platform::InputState state;
        REQUIRE(vm.bindInput(state) == BindStatus::Ok);
        state.newFrame();
        state.process(buttonEvent(platform::EventType::MouseButtonDown, static_cast<platform::MouseButton>(held)));
        const auto read = [&vm](std::string_view function) {
            return script_test::completion(vm, "['Left', 'Right', 'Middle', 'X1', 'X2'].map((n) => engine.input." +
                                                   std::string{function} + "(n) ? 1 : 0).join('')");
        };
        std::string only(BUTTONS.size(), '0');
        only[held] = '1';
        CHECK(read("mouseButtonDown") == only);
        CHECK(read("mouseButtonPressed") == only);
        CHECK(read("mouseButtonReleased") == std::string(BUTTONS.size(), '0'));

        state.newFrame();
        state.process(buttonEvent(platform::EventType::MouseButtonUp, static_cast<platform::MouseButton>(held)));
        CHECK(read("mouseButtonDown") == std::string(BUTTONS.size(), '0'));
        CHECK(read("mouseButtonPressed") == std::string(BUTTONS.size(), '0'));
        CHECK(read("mouseButtonReleased") == only);
    }
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

TEST_CASE("input: the pointer functions return a new Vec3 each call (SI7)") {
    const script_test::LogCapture log;
    Vm vm = script_test::makeBuiltinVm();
    platform::InputState state;
    REQUIRE(vm.bindInput(state) == BindStatus::Ok);
    state.newFrame();
    state.process(motionEvent(10.0F, 20.0F, 10.0F, 20.0F));
    state.process(motionEvent(15.0F, 18.0F, 5.0F, -2.0F));
    state.process(wheelEvent(1.0F, 2.0F));
    state.process(wheelEvent(0.0F, 3.0F));

    const std::string vectors =
        "[engine.input.mousePosition(), engine.input.mouseDelta(), engine.input.mouseWheel()]"
        ".map((v) => [v instanceof engine.Vec3, v.x, v.y, v.z].join(':')).join('|')";
    CHECK(script_test::completion(vm, vectors) == "true:15:18:0|true:15:18:0|true:1:5:0");
    CHECK(script_test::completion(vm, "(engine.input.mousePosition() === engine.input.mousePosition()).toString()") ==
          "false");
    // A write to one object changes nothing the next call reads.
    CHECK(script_test::completion(vm,
                                  "(() => { const p = engine.input.mousePosition(); p.x = 99; p.z = 7;"
                                  " const q = engine.input.mousePosition(); return [q.x, q.z].join(); })()") == "15,0");

    state.newFrame();  // the delta and the wheel clear, the position stays
    CHECK(script_test::completion(vm, vectors) == "true:15:18:0|true:0:0:0|true:0:0:0");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

TEST_CASE("input: a name that is not a string or not in the table is refused without running script code (SI8)") {
    const script_test::LogCapture log;
    Vm vm = script_test::makeBuiltinVm();
    const platform::InputState state;
    REQUIRE(vm.bindInput(state) == BindStatus::Ok);
    script_test::completion(vm,
                            "globalThis.n = 0; globalThis.o = { toString() { globalThis.n++; return 'W'; },"
                            " [Symbol.toPrimitive]() { globalThis.n++; return 'W'; } }; 'ready'");

    const std::string typeError = "TypeError|engine.input.keyDown: expected a string, got ";
    CHECK(outcome(vm, "engine.input.keyDown(3)") == typeError + "a number");
    CHECK(outcome(vm, "engine.input.keyDown(undefined)") == typeError + "undefined");
    CHECK(outcome(vm, "engine.input.keyDown()") == typeError + "undefined");
    CHECK(outcome(vm, "engine.input.keyDown(null)") == typeError + "null");
    CHECK(outcome(vm, "engine.input.keyDown(new String('W'))") == typeError + "an object");
    CHECK(outcome(vm, "engine.input.keyDown(Symbol('W'))") == typeError + "a symbol");
    CHECK(outcome(vm, "engine.input.keyDown(o)") == typeError + "an object");
    CHECK(outcome(vm, "engine.input.mouseButtonPressed(true)") ==
          "TypeError|engine.input.mouseButtonPressed: expected a string, got a boolean");

    const std::string rangeError = "RangeError|engine.input.keyDown: not a key name";
    CHECK(outcome(vm, "engine.input.keyDown('w')") == rangeError);
    CHECK(outcome(vm, "engine.input.keyDown('Unknown')") == rangeError);
    CHECK(outcome(vm, "engine.input.keyDown('Count')") == rangeError);
    CHECK(outcome(vm, "engine.input.keyDown('')") == rangeError);
    CHECK(outcome(vm, "engine.input.keyDown('W\\0')") == rangeError);
    CHECK(outcome(vm, "engine.input.mouseButtonDown('South')") ==
          "RangeError|engine.input.mouseButtonDown: not a mouse button name");
    CHECK(outcome(vm, "engine.input.keyReleased('\\u00e9')") == "RangeError|engine.input.keyReleased: not a key name");
    CHECK(script_test::completion(vm, "String(globalThis.n)") == "0");  // nothing coerced the object
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

TEST_CASE("input: engine.input is a frozen object of frozen functions on a frozen namespace (SI9)") {
    const script_test::LogCapture log;
    Vm vm = script_test::makeBuiltinVm();

    std::string names;
    std::string lengths;
    for (const InputFunctionRow& row : INPUT_ROWS) {
        names += (names.empty() ? "" : ",") + std::string{row.name};
        lengths += (lengths.empty() ? "" : ",") + std::to_string(row.length);
    }
    CHECK(script_test::completion(vm,
                                  "(() => { const d = Object.getOwnPropertyDescriptor(engine, 'input');"
                                  " return [Object.isFrozen(engine.input),"
                                  " Object.getPrototypeOf(engine.input) === Object.prototype,"
                                  " d.writable, d.configurable, d.enumerable].join(); })()") ==
          "true,true,false,false,true");
    CHECK(script_test::completion(vm, "Object.keys(engine.input).join()") == names);
    CHECK(script_test::completion(vm, "Object.keys(engine.input).map((k) => engine.input[k].length).join()") ==
          lengths);
    CHECK(
        script_test::completion(vm,
                                "Object.keys(engine.input).every((k) => typeof engine.input[k] === 'function'"
                                " && Object.isFrozen(engine.input[k]) && !Object.hasOwn(engine.input[k], 'prototype'))"
                                ".toString()") == "true");
    CHECK(script_test::completion(vm,
                                  "(() => { 'use strict'; try { engine.input = 1; return 'no'; }"
                                  " catch (e) { return e.name; } })()") == "TypeError");
    CHECK(script_test::completion(vm,
                                  "(() => { 'use strict'; try { engine.input.keyDown = 1; return 'no'; }"
                                  " catch (e) { return e.name; } })()") == "TypeError");
    CHECK(script_test::completion(vm,
                                  "(() => { 'use strict'; try { engine.input.extra = 1; return 'no'; }"
                                  " catch (e) { return e.name; } })()") == "TypeError");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

TEST_CASE("input: the input binding and the World binding do not touch each other (SI10)") {
    const script_test::LogCapture log;
    Vm vm = script_test::makeBuiltinVm();
    engine::World world;
    platform::InputState first;
    press(first, platform::Key::W);
    platform::InputState second;
    press(second, platform::Key::Space);

    REQUIRE(vm.bindInput(first) == BindStatus::Ok);
    REQUIRE(vm.bindWorld(world) == BindStatus::Ok);
    CHECK(outcome(vm, "engine.input.keyDown('W')") == "ok:true");
    vm.unbindWorld();
    CHECK(outcome(vm, "engine.input.keyDown('W')") == "ok:true");
    CHECK(outcome(vm, "engine.spawn()").starts_with("TypeError|engine.spawn: no World is bound"));

    REQUIRE(vm.bindInput(second) == BindStatus::Ok);  // a second bind replaces the first
    CHECK(outcome(vm, "[engine.input.keyDown('W'), engine.input.keyDown('Space')].join()") == "ok:false,true");
    REQUIRE(vm.bindWorld(world) == BindStatus::Ok);
    CHECK(outcome(vm, "engine.input.keyDown('Space')") == "ok:true");
    vm.unbindInput();
    CHECK(outcome(vm, "engine.spawn().alive") == "ok:true");  // the World is still bound
    CHECK(outcome(vm, "engine.input.keyDown('Space')") == "TypeError|engine.input.keyDown: no input is bound");

    REQUIRE(vm.bindInput(first) == BindStatus::Ok);  // shut down with the input still bound
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

TEST_CASE("input: every key name reads its own key and the other names read false (SI11)") {
    const script_test::LogCapture log;
    Vm vm = script_test::makeBuiltinVm();
    std::string list = "globalThis.NAMES = [";
    for (const std::string_view name : names::KEY_NAMES) {
        list += "'" + std::string{name} + "',";
    }
    list += "]; NAMES.length";
    CHECK(script_test::completion(vm, list) == std::to_string(names::KEY_NAMES.size()));

    for (std::size_t i = 0; i < names::KEY_NAMES.size(); ++i) {
        CAPTURE(names::KEY_NAMES[i]);
        platform::InputState state;
        press(state, static_cast<platform::Key>(i + 1U));  // [i] names Key(i + 1)
        REQUIRE(vm.bindInput(state) == BindStatus::Ok);
        std::string expected(names::KEY_NAMES.size(), '0');
        expected[i] = '1';
        CHECK(script_test::completion(vm, "NAMES.map((n) => engine.input.keyDown(n) ? 1 : 0).join('')") == expected);
    }
    const platform::InputState empty;
    REQUIRE(vm.bindInput(empty) == BindStatus::Ok);
    for (const std::string_view name : names::MOUSE_BUTTON_NAMES) {
        CAPTURE(name);
        CHECK(outcome(vm, "engine.input.mouseButtonDown('" + std::string{name} + "')") == "ok:false");
    }
    for (const std::string_view wrong : {"Return", "Esc", "Digit1", "KeyW", "ArrowLeft", "Shift", "Ctrl", "Button4"}) {
        CAPTURE(wrong);
        CHECK(outcome(vm, "engine.input.keyDown('" + std::string{wrong} + "')") ==
              "RangeError|engine.input.keyDown: not a key name");
    }
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

// ---- SI12-SI17: the gamepad functions ------------------------------------------------------------------------------

namespace {

[[nodiscard]] platform::Event padEvent(platform::EventType type, std::uint8_t slot) {
    platform::Event event;
    event.type = type;
    event.gamepad = platform::GamepadDeviceData{slot};
    return event;
}

[[nodiscard]] platform::Event padButtonEvent(platform::EventType type, std::uint8_t slot, platform::GamepadButton b) {
    platform::Event event;
    event.type = type;
    event.gamepadButton = platform::GamepadButtonData{slot, b};
    return event;
}

[[nodiscard]] platform::Event padAxisEvent(std::uint8_t slot, platform::GamepadAxis axis, std::int16_t value) {
    platform::Event event;
    event.type = platform::EventType::GamepadAxisMoved;
    event.gamepadAxis = platform::GamepadAxisData{slot, axis, value};
    return event;
}

void connectPad(platform::InputState& state, std::uint8_t slot) {
    state.process(padEvent(platform::EventType::GamepadConnected, slot));
}

void pressPad(platform::InputState& state, std::uint8_t slot, platform::GamepadButton button) {
    state.process(padButtonEvent(platform::EventType::GamepadButtonDown, slot, button));
}

void releasePad(platform::InputState& state, std::uint8_t slot, platform::GamepadButton button) {
    state.process(padButtonEvent(platform::EventType::GamepadButtonUp, slot, button));
}

void movePad(platform::InputState& state, std::uint8_t slot, platform::GamepadAxis axis, std::int16_t value) {
    state.process(padAxisEvent(slot, axis, value));
}

constexpr std::string_view SLOT_RANGE = ": the slot must be an integer from 0 to 3";

}  // namespace

TEST_CASE("input: gamepadConnected reads the slot it is given and never throws for an empty one (SI12)") {
    const script_test::LogCapture log;
    Vm vm = script_test::makeBuiltinVm();
    platform::InputState state;
    REQUIRE(vm.bindInput(state) == BindStatus::Ok);

    CHECK(outcome(vm, "engine.input.gamepadConnected()") == "ok:false");
    CHECK(outcome(vm, "[0, 1, 2, 3].map((s) => engine.input.gamepadConnected(s)).join()") ==
          "ok:false,false,false,false");

    connectPad(state, 2);
    CHECK(outcome(vm, "engine.input.gamepadConnected(2)") == "ok:true");
    CHECK(outcome(vm, "engine.input.gamepadConnected(1)") == "ok:false");
    CHECK(outcome(vm, "engine.input.gamepadConnected()") == "ok:false");  // the default slot is 0, not the pad's

    connectPad(state, 0);
    CHECK(outcome(vm, "engine.input.gamepadConnected()") == "ok:true");
    CHECK(outcome(vm, "typeof engine.input.gamepadConnected(0)") == "ok:boolean");

    state.process(padEvent(platform::EventType::GamepadDisconnected, 2));
    CHECK(outcome(vm, "engine.input.gamepadConnected(2)") == "ok:false");
    CHECK(outcome(vm, "engine.input.gamepadConnected(0)") == "ok:true");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

TEST_CASE("input: gamepad buttons read the slot they are given, with edges per frame (SI13)") {
    const script_test::LogCapture log;
    Vm vm = script_test::makeBuiltinVm();
    platform::InputState state;
    REQUIRE(vm.bindInput(state) == BindStatus::Ok);
    connectPad(state, 0);
    connectPad(state, 1);

    const auto read = [&vm](std::string_view slotArgument) {
        const std::string slot{slotArgument};
        return script_test::completion(vm, "[engine.input.gamepadButtonDown('South'" + slot +
                                               "), engine.input.gamepadButtonPressed('South'" + slot +
                                               "), engine.input.gamepadButtonReleased('South'" + slot + ")].join()");
    };
    state.newFrame();  // frame 1
    pressPad(state, 1, platform::GamepadButton::South);
    CHECK(read(", 1") == "true,true,false");
    CHECK(read("") == "false,false,false");  // slot 0 holds nothing
    CHECK(read(", 0") == "false,false,false");

    state.newFrame();  // frame 2: the edge is gone, the button stays held
    CHECK(read(", 1") == "true,false,false");

    state.newFrame();  // frame 3
    releasePad(state, 1, platform::GamepadButton::South);
    CHECK(read(", 1") == "false,false,true");
    state.newFrame();
    CHECK(read(", 1") == "false,false,false");

    // Every button of an empty slot reads false, through all three functions.
    std::string list = "globalThis.GB = [";
    for (const std::string_view name : names::GAMEPAD_BUTTON_NAMES) {
        list += "'" + std::string{name} + "',";
    }
    list += "]; GB.length";
    CHECK(script_test::completion(vm, list) == std::to_string(names::GAMEPAD_BUTTON_NAMES.size()));
    pressPad(state, 1, platform::GamepadButton::Start);
    CHECK(script_test::completion(vm,
                                  "GB.map((n) => [engine.input.gamepadButtonDown(n, 3),"
                                  " engine.input.gamepadButtonPressed(n, 3), engine.input.gamepadButtonReleased(n, 3)]"
                                  ".some((v) => v)).some((v) => v).toString()") == "false");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

TEST_CASE("input: gamepadAxis returns the folded axis with no deadzone (SI14)") {
    const script_test::LogCapture log;
    Vm vm = script_test::makeBuiltinVm();
    platform::InputState state;
    REQUIRE(vm.bindInput(state) == BindStatus::Ok);
    connectPad(state, 0);
    connectPad(state, 1);

    movePad(state, 0, platform::GamepadAxis::LeftX, -32768);
    movePad(state, 0, platform::GamepadAxis::RightX, 32767);
    movePad(state, 0, platform::GamepadAxis::LeftTrigger, 0);
    movePad(state, 0, platform::GamepadAxis::RightTrigger, 32767);
    movePad(state, 1, platform::GamepadAxis::LeftX, 4000);
    CHECK(outcome(vm, "engine.input.gamepadAxis('LeftX') === -1") == "ok:true");
    CHECK(outcome(vm, "engine.input.gamepadAxis('RightX') === 1") == "ok:true");
    CHECK(outcome(vm, "engine.input.gamepadAxis('LeftTrigger') === 0") == "ok:true");
    CHECK(outcome(vm, "engine.input.gamepadAxis('RightTrigger') === 1") == "ok:true");
    // 4000 is inside the stick deadzone and still reads raw: the float division, widened to a double.
    CHECK(outcome(vm, "engine.input.gamepadAxis('LeftX', 1) === Math.fround(4000 / 32767)") == "ok:true");
    CHECK(outcome(vm, "engine.input.gamepadAxis('LeftX', 1) !== 0") == "ok:true");
    CHECK(outcome(vm, "typeof engine.input.gamepadAxis('LeftY')") == "ok:number");
    CHECK(outcome(vm,
                  "engine.input.gamepadAxis('LeftX', 2) === 0 && engine.input.gamepadAxis('RightTrigger', 3) === 0") ==
          "ok:true");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

TEST_CASE("input: gamepadStick returns the dead-zoned stick as a new Vec3 with z 0 (SI15)") {
    const script_test::LogCapture log;
    Vm vm = script_test::makeBuiltinVm();
    platform::InputState state;
    REQUIRE(vm.bindInput(state) == BindStatus::Ok);
    connectPad(state, 0);
    connectPad(state, 1);
    connectPad(state, 2);

    movePad(state, 0, platform::GamepadAxis::LeftY, -32768);  // full tilt up
    movePad(state, 1, platform::GamepadAxis::LeftX, 4000);    // inside the deadzone
    movePad(state, 1, platform::GamepadAxis::LeftY, 4000);
    movePad(state, 2, platform::GamepadAxis::RightX, 32767);  // the right stick only
    const std::string vector = "(v) => [v instanceof engine.Vec3, v.x, v.y, v.z].join(':')";
    CHECK(script_test::completion(vm, "(" + vector + ")(engine.input.gamepadStick('Left'))") == "true:0:-1:0");
    CHECK(script_test::completion(vm, "(" + vector + ")(engine.input.gamepadStick('Right'))") == "true:0:0:0");
    CHECK(script_test::completion(vm, "(" + vector + ")(engine.input.gamepadStick('Left', 1))") == "true:0:0:0");
    CHECK(script_test::completion(vm, "(" + vector + ")(engine.input.gamepadStick('Right', 2))") == "true:1:0:0");
    CHECK(script_test::completion(vm, "(" + vector + ")(engine.input.gamepadStick('Left', 2))") == "true:0:0:0");
    CHECK(script_test::completion(vm, "(" + vector + ")(engine.input.gamepadStick('Right', 3))") == "true:0:0:0");
    CHECK(script_test::completion(vm,
                                  "(engine.input.gamepadStick('Left') === engine.input.gamepadStick('Left'))"
                                  ".toString()") == "false");
    // A write to one object changes nothing the next call reads.
    CHECK(script_test::completion(vm,
                                  "(() => { const p = engine.input.gamepadStick('Left'); p.y = 99; p.z = 7;"
                                  " const q = engine.input.gamepadStick('Left'); return [q.y, q.z].join(); })()") ==
          "-1,0");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

TEST_CASE("input: a slot is an integer from 0 to 3 and every check precedes the binding (SI16)") {
    const script_test::LogCapture log;
    Vm vm = script_test::makeBuiltinVm();
    platform::InputState state;
    connectPad(state, 0);
    connectPad(state, 3);
    REQUIRE(vm.bindInput(state) == BindStatus::Ok);

    // Slot 0 is the only one holding the button; omitted, undefined and -0 all read it.
    pressPad(state, 0, platform::GamepadButton::East);
    CHECK(outcome(vm, "engine.input.gamepadButtonDown('East')") == "ok:true");
    CHECK(outcome(vm, "engine.input.gamepadButtonDown('East', undefined)") == "ok:true");
    CHECK(outcome(vm, "engine.input.gamepadButtonDown('East', -0)") == "ok:true");
    CHECK(outcome(vm, "engine.input.gamepadButtonDown('East', 3)") == "ok:false");
    CHECK(outcome(vm, "engine.input.gamepadConnected(3)") == "ok:true");
    CHECK(outcome(vm, "engine.input.gamepadConnected(-0)") == "ok:true");

    const std::string typeError = "TypeError|engine.input.gamepadConnected: expected a slot number, got ";
    CHECK(outcome(vm, "engine.input.gamepadConnected('1')") == typeError + "a string");
    CHECK(outcome(vm, "engine.input.gamepadConnected(null)") == typeError + "null");
    CHECK(outcome(vm, "engine.input.gamepadConnected(true)") == typeError + "a boolean");
    CHECK(outcome(vm, "engine.input.gamepadConnected(1n)") == typeError + "a bigint");
    CHECK(outcome(vm, "engine.input.gamepadConnected({})") == typeError + "an object");

    const std::string rangeError = "RangeError|engine.input.gamepadConnected" + std::string{SLOT_RANGE};
    for (const std::string_view bad : {"4", "-1", "1.5", "NaN", "Infinity", "-Infinity", "2 ** 53"}) {
        CAPTURE(bad);
        CHECK(outcome(vm, "engine.input.gamepadConnected(" + std::string{bad} + ")") == rangeError);
    }
    // The name is checked before the slot, the slot before the binding.
    CHECK(outcome(vm, "engine.input.gamepadButtonDown('Nope', 9)") ==
          "RangeError|engine.input.gamepadButtonDown: not a gamepad button name");
    CHECK(outcome(vm, "engine.input.gamepadButtonDown(3, 9)") ==
          "TypeError|engine.input.gamepadButtonDown: expected a string, got a number");
    CHECK(outcome(vm, "engine.input.gamepadAxis('LeftX', 9)") ==
          "RangeError|engine.input.gamepadAxis" + std::string{SLOT_RANGE});
    CHECK(outcome(vm, "engine.input.gamepadStick('Left', '0')") ==
          "TypeError|engine.input.gamepadStick: expected a slot number, got a string");

    vm.unbindInput();
    CHECK(outcome(vm, "engine.input.gamepadConnected(9)") == rangeError);
    CHECK(outcome(vm, "engine.input.gamepadConnected(1.5)") == rangeError);
    CHECK(outcome(vm, "engine.input.gamepadConnected()") ==
          "TypeError|engine.input.gamepadConnected: no input is bound");
    CHECK(outcome(vm, "engine.input.gamepadButtonPressed('West', 1)") ==
          "TypeError|engine.input.gamepadButtonPressed: no input is bound");
    CHECK(outcome(vm, "engine.input.gamepadButtonReleased('Nope')") ==
          "RangeError|engine.input.gamepadButtonReleased: not a gamepad button name");
    CHECK(outcome(vm, "engine.input.gamepadAxis('LeftX')") == "TypeError|engine.input.gamepadAxis: no input is bound");
    CHECK(outcome(vm, "engine.input.gamepadStick('Right')") ==
          "TypeError|engine.input.gamepadStick: no input is bound");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

TEST_CASE("input: every gamepad button, axis and stick name reads its own and no other (SI17)") {
    const script_test::LogCapture log;
    Vm vm = script_test::makeBuiltinVm();
    std::string list = "globalThis.GB = [";
    for (const std::string_view name : names::GAMEPAD_BUTTON_NAMES) {
        list += "'" + std::string{name} + "',";
    }
    list += "]; GB.length";
    CHECK(script_test::completion(vm, list) == std::to_string(names::GAMEPAD_BUTTON_NAMES.size()));

    for (std::size_t held = 0; held < names::GAMEPAD_BUTTON_NAMES.size(); ++held) {
        CAPTURE(names::GAMEPAD_BUTTON_NAMES[held]);
        platform::InputState state;
        connectPad(state, 1);
        pressPad(state, 1, static_cast<platform::GamepadButton>(held));
        REQUIRE(vm.bindInput(state) == BindStatus::Ok);
        std::string expected(names::GAMEPAD_BUTTON_NAMES.size(), '0');
        expected[held] = '1';
        CHECK(script_test::completion(vm, "GB.map((n) => engine.input.gamepadButtonDown(n, 1) ? 1 : 0).join('')") ==
              expected);
        CHECK(script_test::completion(vm, "GB.map((n) => engine.input.gamepadButtonPressed(n, 1) ? 1 : 0).join('')") ==
              expected);
    }

    // Each axis, set to a distinct raw value, reads back under its own name.
    platform::InputState state;
    connectPad(state, 0);
    constexpr std::array<std::int16_t, 6> RAW{1000, 2000, 3000, 4000, 5000, 6000};
    for (std::size_t i = 0; i < RAW.size(); ++i) {
        movePad(state, 0, static_cast<platform::GamepadAxis>(i), RAW[i]);
    }
    REQUIRE(vm.bindInput(state) == BindStatus::Ok);
    for (std::size_t i = 0; i < RAW.size(); ++i) {
        CAPTURE(names::GAMEPAD_AXIS_NAMES[i]);
        const std::string call = "engine.input.gamepadAxis('" + std::string{names::GAMEPAD_AXIS_NAMES[i]} + "')";
        CHECK(outcome(vm, call + " === Math.fround(" + std::to_string(RAW[i]) + " / 32767)") == "ok:true");
    }
    for (const std::string_view stick : names::GAMEPAD_STICK_NAMES) {
        CAPTURE(stick);
        CHECK(outcome(vm, "engine.input.gamepadStick('" + std::string{stick} + "') instanceof engine.Vec3") ==
              "ok:true");
    }

    for (const std::string_view wrong : {"A", "B", "Cross", "LB", "Select", "LeftZ", "Up"}) {
        CAPTURE(wrong);
        const std::string quoted = "('" + std::string{wrong} + "')";
        CHECK(outcome(vm, "engine.input.gamepadButtonDown" + quoted) ==
              "RangeError|engine.input.gamepadButtonDown: not a gamepad button name");
        CHECK(outcome(vm, "engine.input.gamepadAxis" + quoted) ==
              "RangeError|engine.input.gamepadAxis: not a gamepad axis name");
        CHECK(outcome(vm, "engine.input.gamepadStick" + quoted) ==
              "RangeError|engine.input.gamepadStick: not a stick name");
    }
    CHECK(outcome(vm, "engine.input.gamepadStick('LeftX')") ==
          "RangeError|engine.input.gamepadStick: not a stick name");
    CHECK(outcome(vm, "engine.input.gamepadAxis('Left')") ==
          "RangeError|engine.input.gamepadAxis: not a gamepad axis name");
    CHECK(log.countAtLeast(LogLevel::Warn) == 0U);
    script_test::expectCleanShutdown(vm);
}

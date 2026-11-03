// Aero Engine -- engine.input: the name tables, the binding and the functions (task 4.4.3; SI1-SI17).
//
// The five name tables (engine/script/internal/aero/script/internal/input_names.hpp) are the script-visible spelling
// of the platform enums. They are written by hand, in enum order, and SI1 holds them to the enums' own text so a
// misspelt, reordered or forgotten name reddens here and nowhere later. SI2 holds the lookups to an exact,
// case-sensitive match.

#include <aero/platform/event.hpp>
#include <aero/script/internal/input_names.hpp>

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

// engine/reflect/src/json_writer.cpp — task 1.2.1: the JsonWriter state machine (spec §3.7). Streams
// text straight into `buffer`; no DOM. Numbers are shortest-round-trip via std::to_chars
// (locale-independent, portable across MSVC 2022/GCC 13+/Clang, F2); non-finite floats/doubles become
// `null` (JSON has no NaN/Inf, E-nonfinite).
#include <aero/reflect/json_writer.hpp>

#include <array>
#include <cassert>
#include <charconv>
#include <cmath>

namespace engine {

namespace {

// A control character (< 0x20) that has no short escape becomes \u00XX -- always exactly two hex
// digits since c < 0x20 fits in one byte's low nibble range.
void appendUnicodeEscape(std::string& out, unsigned char c) {
    constexpr std::array<char, 16> HEX_DIGITS = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                 '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    out += "\\u00";
    out += HEX_DIGITS[(c >> 4) & 0xF];
    out += HEX_DIGITS[c & 0xF];
}

}  // namespace

JsonWriter::JsonWriter(JsonWriterConfig cfg) : config(cfg) {}

void JsonWriter::writeIndent() {
    if (!config.pretty) {
        return;
    }
    buffer += '\n';
    buffer.append(static_cast<std::size_t>(stack.size()) * static_cast<std::size_t>(config.indentWidth), ' ');
}

void JsonWriter::beforeValue() {
    if (expectingValue) {
        expectingValue = false;
        return;
    }
    if (!stack.empty()) {
        if (stack.back().hasChild) {
            buffer += ',';
        }
        writeIndent();
        stack.back().hasChild = true;
    }
}

void JsonWriter::writeEscaped(std::string_view s) {
    buffer += '"';
    for (const unsigned char c : s) {
        switch (c) {
            case '"':
                buffer += "\\\"";
                break;
            case '\\':
                buffer += "\\\\";
                break;
            case '\b':
                buffer += "\\b";
                break;
            case '\f':
                buffer += "\\f";
                break;
            case '\n':
                buffer += "\\n";
                break;
            case '\r':
                buffer += "\\r";
                break;
            case '\t':
                buffer += "\\t";
                break;
            default:
                if (c < 0x20) {
                    appendUnicodeEscape(buffer, c);
                } else {
                    buffer += static_cast<char>(c);
                }
                break;
        }
    }
    buffer += '"';
}

void JsonWriter::beginObject() {
    beforeValue();
    buffer += '{';
    stack.push_back(Frame{.isArray = false, .hasChild = false});
}

void JsonWriter::endObject() {
    assert(!stack.empty() && !stack.back().isArray);
    const bool hadChild = stack.back().hasChild;
    stack.pop_back();
    if (hadChild) {
        writeIndent();
    }
    buffer += '}';
}

void JsonWriter::beginArray() {
    beforeValue();
    buffer += '[';
    stack.push_back(Frame{.isArray = true, .hasChild = false});
}

void JsonWriter::endArray() {
    assert(!stack.empty() && stack.back().isArray);
    const bool hadChild = stack.back().hasChild;
    stack.pop_back();
    if (hadChild) {
        writeIndent();
    }
    buffer += ']';
}

void JsonWriter::key(std::string_view name) {
    assert(!stack.empty() && !stack.back().isArray && !expectingValue);
    if (stack.back().hasChild) {
        buffer += ',';
    }
    writeIndent();
    stack.back().hasChild = true;
    writeEscaped(name);
    buffer += config.pretty ? ": " : ":";
    expectingValue = true;
}

void JsonWriter::value(bool v) {
    beforeValue();
    buffer += v ? "true" : "false";
}

void JsonWriter::value(long long v) {
    beforeValue();
    std::array<char, 32> buf{};
    const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    buffer.append(buf.data(), result.ptr);
}

void JsonWriter::value(unsigned long long v) {
    beforeValue();
    std::array<char, 32> buf{};
    const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    buffer.append(buf.data(), result.ptr);
}

void JsonWriter::value(float v) {
    beforeValue();
    if (!std::isfinite(v)) {
        buffer += "null";
        return;
    }
    std::array<char, 32> buf{};
    const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    buffer.append(buf.data(), result.ptr);
}

void JsonWriter::value(double v) {
    beforeValue();
    if (!std::isfinite(v)) {
        buffer += "null";
        return;
    }
    std::array<char, 32> buf{};
    const auto result = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    buffer.append(buf.data(), result.ptr);
}

void JsonWriter::value(std::string_view v) {
    beforeValue();
    writeEscaped(v);
}

void JsonWriter::valueNull() {
    beforeValue();
    buffer += "null";
}

const std::string& JsonWriter::str() const { return buffer; }

void JsonWriter::clear() {
    buffer.clear();
    stack.clear();
    expectingValue = false;
}

}  // namespace engine

#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

struct JsonWriterConfig {
    int indentWidth = 2;  // spaces per level when pretty
    bool pretty = true;   // false => compact single line
};

// Streaming JSON writer: appends text straight to a std::string (no DOM, D5). Structural calls place commas,
// newlines and indentation; number formatting is shortest-round-trip via std::to_chars (locale-independent).
// Members are plain camelBack, NO trailing underscore (docs/04, F11). No exceptions across the API; debug
// asserts guard hand-misuse (D16) — generated code is balanced by construction and never trips them.
class JsonWriter {
public:
    JsonWriter() = default;
    explicit JsonWriter(JsonWriterConfig config);

    void beginObject();
    void endObject();
    void beginArray();
    void endArray();
    void key(std::string_view name);  // "name":  (escaped; only valid inside an object, before a value)

    void value(bool v);                // true / false
    void value(long long v);           // integer literal
    void value(unsigned long long v);  // integer literal
    void value(float v);               // shortest round-trip; non-finite => null
    void value(double v);              // shortest round-trip; non-finite => null
    void value(std::string_view v);    // escaped JSON string
    void valueNull();

    [[nodiscard]] const std::string& str() const;  // no trailing newline
    void clear();

private:
    // One frame per open object/array: tracks kind + whether a child has been written yet, to place commas /
    // newlines / indentation. `expectingValue` marks the state right after key() (a value must follow).
    struct Frame {
        bool isArray = false;
        bool hasChild = false;
    };
    void writeIndent();                     // pretty: '\n' + (depth * indentWidth) spaces
    void beforeValue();                     // comma/newline/indent housekeeping before a value or nested begin
    void writeEscaped(std::string_view s);  // "..." with JSON escaping

    std::string buffer;
    JsonWriterConfig config;
    std::vector<Frame> stack;
    bool expectingValue = false;  // true between key() and its value (inside an object)
};

}  // namespace engine

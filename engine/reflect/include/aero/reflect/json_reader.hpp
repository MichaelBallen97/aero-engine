#pragma once
// engine/reflect/include/aero/reflect/json_reader.hpp — task 1.2.2: the parse entry point (spec
// §3.7/D7/D8). Strict RFC 8259 with three documented tolerances (a single leading UTF-8 BOM; last-
// wins duplicate object keys; non-key string content not UTF-8-validated), iterative (never
// recursive — misc-no-recursion, F6), depth-capped. No exceptions; the parser itself never logs
// (the caller — editor UI, tests — owns presentation of the error).
#include <aero/reflect/json_value.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace engine {

struct JsonParseConfig {
    std::uint32_t maxDepth = 256;
};

// 1-based line/column (byte column), 0-based offset.
struct JsonParseError {
    std::string message;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::size_t offset = 0;
};

struct JsonParseResult {
    std::optional<JsonValue> value;
    JsonParseError error;

    [[nodiscard]] bool ok() const;
};

[[nodiscard]] JsonParseResult parseJson(std::string_view text, const JsonParseConfig& config = {});

}  // namespace engine

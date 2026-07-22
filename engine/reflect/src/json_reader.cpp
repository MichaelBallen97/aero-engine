// engine/reflect/src/json_reader.cpp — task 1.2.2: the iterative JSON parser (spec §3.7, D7).
// ITERATIVE BY MANDATE (F6, verified live): a mutually-recursive parseValue<->parseArray pair is
// flagged twice by misc-no-recursion, and CI runs clang-tidy --warnings-as-errors='*'. This parser
// therefore drives a single while loop over an explicit frame stack; the only "leaf" helpers
// (lexString/lexNumber/lexHex4) never call back into the loop, so no call cycle exists to flag.
#include <aero/reflect/json_reader.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// codepoint -> UTF-8 bytes (1-4 bytes depending on range) — used for \uXXXX escapes, incl. combined
// UTF-16 surrogate pairs.
void appendUtf8(std::string& out, std::uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out += static_cast<char>(codepoint);
    } else if (codepoint <= 0x7FF) {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
}

enum class FrameKind : std::uint8_t { Array, Object };

// One enum shared by both frame kinds; each kind only ever visits its own subset (F6's skeleton
// shape). "OrClose" states are reachable ONLY at the frame's start (empty object/array); after a
// comma the state has NO close option, so a trailing comma ("[1,]"/`{"a":1,}`) is a hard error
// rather than silently tolerated (D7 — comments/trailing commas stay rejected).
// Object cycles WantKeyOrClose -> WantColon -> WantValue -> WantCommaOrClose -> WantKey (post-comma)
// -> WantColon -> ...; array cycles WantValueOrClose -> WantCommaOrClose -> WantValue (post-comma)
// -> WantCommaOrClose -> ...
enum class FrameState : std::uint8_t {
    ObjectWantKeyOrClose,  // initial only: '}' or a key
    ObjectWantKey,         // after a comma: a key is mandatory, no close (trailing-comma reject)
    ObjectWantColon,
    ObjectWantValue,
    ObjectWantCommaOrClose,
    ArrayWantValueOrClose,  // initial only: ']' or a value
    ArrayWantValue,         // after a comma: a value is mandatory, no close (trailing-comma reject)
    ArrayWantCommaOrClose,
};

struct Frame {
    FrameKind kind;
    FrameState state;
    std::string pendingKey;
    std::vector<JsonValue> elements;
    std::vector<JsonMember> members;
};

// The iterative parser (D7/D8). One instance per parseJson() call; not reused.
class Parser {
public:
    Parser(std::string_view text, const JsonParseConfig& config) : src(text), maxDepth(config.maxDepth) {}

    JsonParseResult run() {
        JsonParseResult result;
        skipBom();
        if (!beginValue(result.error)) {
            return result;
        }
        while (!stack.empty()) {
            if (!step(result.error)) {
                return result;
            }
        }
        skipWhitespace();
        if (!atEnd()) {
            fail(result.error, "unexpected trailing content after JSON value");
            return result;
        }
        result.value = std::move(root);
        return result;
    }

private:
    [[nodiscard]] bool atEnd() const { return pos >= src.size(); }
    [[nodiscard]] char peek() const { return src[pos]; }

    // Advances one byte, tracking 1-based line (on '\n' only, D7 — '\r' is plain whitespace so CRLF
    // input still reports correct positions) and the byte offset of the current line's start.
    void advance() {
        if (src[pos] == '\n') {
            ++line;
            lineStart = pos + 1;
        }
        ++pos;
    }

    void skip(std::size_t count) { pos += count; }  // literal tokens (true/false/null) never contain '\n'

    void skipBom() {
        constexpr std::string_view BOM = "\xEF\xBB\xBF";
        if (src.substr(0, BOM.size()) == BOM) {
            pos += BOM.size();
        }
    }

    void skipWhitespace() {
        while (!atEnd()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
            } else {
                break;
            }
        }
    }

    // Internal invariant: always returns false, so call sites read naturally as `return fail(...)`.
    bool fail(JsonParseError& error, std::string message) {
        error.message = std::move(message);
        error.line = line;
        error.column = static_cast<std::uint32_t>(pos - lineStart + 1);
        error.offset = pos;
        return false;
    }

    [[nodiscard]] bool matchLiteral(std::string_view literal) const {
        if (pos + literal.size() > src.size()) {
            return false;
        }
        return src.substr(pos, literal.size()) == literal;
    }

    // Reads a value at the CURRENT position: either pushes a new Array/Object frame (the value is
    // itself a container, completed later when its close bracket is seen) or reads+attaches a scalar
    // immediately. This is the ONLY place a frame is pushed, so the maxDepth check lives here alone.
    bool beginValue(JsonParseError& error) {
        skipWhitespace();
        if (atEnd()) {
            return fail(error, "unexpected end of input");
        }
        const char c = peek();
        if (c == '{') {
            if (stack.size() >= maxDepth) {
                return fail(error, "nesting exceeds maxDepth (" + std::to_string(maxDepth) + ")");
            }
            advance();
            stack.push_back(Frame{.kind = FrameKind::Object, .state = FrameState::ObjectWantKeyOrClose});
            return true;
        }
        if (c == '[') {
            if (stack.size() >= maxDepth) {
                return fail(error, "nesting exceeds maxDepth (" + std::to_string(maxDepth) + ")");
            }
            advance();
            stack.push_back(Frame{.kind = FrameKind::Array, .state = FrameState::ArrayWantValueOrClose});
            return true;
        }
        JsonValue value;
        if (!lexScalar(value, error)) {
            return false;
        }
        attach(std::move(value));
        return true;
    }

    // Advances the frame stack by exactly one structural decision (comma/colon/close) or delegates to
    // beginValue() for the next value. Called only while the stack is non-empty.
    bool step(JsonParseError& error) {
        Frame& top = stack.back();
        skipWhitespace();
        if (atEnd()) {
            return fail(error, "unexpected end of input");
        }
        if (top.kind == FrameKind::Array) {
            switch (top.state) {
                case FrameState::ArrayWantValueOrClose:
                    if (peek() == ']') {
                        advance();
                        closeArray();
                        return true;
                    }
                    return beginValue(error);
                case FrameState::ArrayWantValue:
                    // NO close permitted here (post-comma): a bare ']' falls into beginValue()'s
                    // "unexpected character" fallback, correctly rejecting a trailing comma.
                    return beginValue(error);
                case FrameState::ArrayWantCommaOrClose: {
                    const char c = peek();
                    if (c == ',') {
                        advance();
                        top.state = FrameState::ArrayWantValue;
                        return true;
                    }
                    if (c == ']') {
                        advance();
                        closeArray();
                        return true;
                    }
                    return fail(error, "expected ',' or ']' in array");
                }
                default:
                    return fail(error, "internal parser error: unexpected array state");  // unreachable
            }
        }
        // Object
        switch (top.state) {
            case FrameState::ObjectWantKeyOrClose:
            case FrameState::ObjectWantKey: {
                const char c = peek();
                // A close is legal ONLY from the initial state (empty object); post-comma, a
                // trailing '}' is a hard error (D7 — trailing commas stay rejected).
                if (c == '}' && top.state == FrameState::ObjectWantKeyOrClose) {
                    advance();
                    closeObject();
                    return true;
                }
                if (c != '"') {
                    return fail(error, top.state == FrameState::ObjectWantKeyOrClose
                                           ? "expected '\"' or '}' in object"
                                           : "expected '\"' (object key) after ','");
                }
                std::string key;
                if (!lexString(key, error)) {
                    return false;
                }
                top.pendingKey = std::move(key);
                top.state = FrameState::ObjectWantColon;
                return true;
            }
            case FrameState::ObjectWantColon: {
                if (peek() != ':') {
                    return fail(error, "expected ':' after object key");
                }
                advance();
                top.state = FrameState::ObjectWantValue;
                return true;
            }
            case FrameState::ObjectWantValue:
                return beginValue(error);
            case FrameState::ObjectWantCommaOrClose: {
                const char c = peek();
                if (c == ',') {
                    advance();
                    top.state = FrameState::ObjectWantKey;
                    return true;
                }
                if (c == '}') {
                    advance();
                    closeObject();
                    return true;
                }
                return fail(error, "expected ',' or '}' in object");
            }
            default:
                return fail(error, "internal parser error: unexpected object state");  // unreachable
        }
    }

    // Attaches a completed value to the top frame (array push_back; object insert-or-overwrite —
    // last-wins, D7 tolerance 2) or, with an empty stack, makes it the root.
    void attach(JsonValue value) {
        if (stack.empty()) {
            root = std::move(value);
            return;
        }
        Frame& top = stack.back();
        if (top.kind == FrameKind::Array) {
            top.elements.push_back(std::move(value));
            top.state = FrameState::ArrayWantCommaOrClose;
            return;
        }
        // Early-return (rather than a replaced-flag + break) so the two `std::move(value)` paths are
        // provably mutually exclusive to clang-tidy's flow analysis, not just to a human reader.
        for (JsonMember& member : top.members) {
            if (member.key == top.pendingKey) {
                member.value = std::move(value);
                top.pendingKey.clear();
                top.state = FrameState::ObjectWantCommaOrClose;
                return;
            }
        }
        top.members.push_back(JsonMember{std::move(top.pendingKey), std::move(value)});
        top.pendingKey.clear();
        top.state = FrameState::ObjectWantCommaOrClose;
    }

    void closeArray() {
        assert(!stack.empty() && stack.back().kind == FrameKind::Array);  // internal invariant, D16
        Frame top = std::move(stack.back());
        stack.pop_back();
        attach(JsonValue::array(std::move(top.elements)));
    }

    void closeObject() {
        assert(!stack.empty() && stack.back().kind == FrameKind::Object);  // internal invariant, D16
        Frame top = std::move(stack.back());
        stack.pop_back();
        attach(JsonValue::object(std::move(top.members)));
    }

    bool lexScalar(JsonValue& out, JsonParseError& error) {
        const char c = peek();
        if (c == '"') {
            std::string text;
            if (!lexString(text, error)) {
                return false;
            }
            out = JsonValue::string(std::move(text));
            return true;
        }
        if (c == '-' || isDigit(c)) {
            return lexNumber(out, error);
        }
        if (matchLiteral("true")) {
            skip(4);
            out = JsonValue::boolean(true);
            return true;
        }
        if (matchLiteral("false")) {
            skip(5);
            out = JsonValue::boolean(false);
            return true;
        }
        if (matchLiteral("null")) {
            skip(4);
            out = JsonValue::null();
            return true;
        }
        return fail(error, "unexpected character at start of value");
    }

    // JSON grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?. Validated, NOT converted (D5) —
    // the matched substring becomes the stored JsonNumber lexeme verbatim.
    bool lexNumber(JsonValue& out, JsonParseError& error) {
        const std::size_t start = pos;
        if (peek() == '-') {
            advance();
        }
        if (atEnd() || !isDigit(peek())) {
            return fail(error, "invalid number: expected a digit");
        }
        if (peek() == '0') {
            advance();
            if (!atEnd() && isDigit(peek())) {
                return fail(error, "number has leading zero");
            }
        } else {
            while (!atEnd() && isDigit(peek())) {
                advance();
            }
        }
        if (!atEnd() && peek() == '.') {
            advance();
            if (atEnd() || !isDigit(peek())) {
                return fail(error, "expected digit after decimal point");
            }
            while (!atEnd() && isDigit(peek())) {
                advance();
            }
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
            advance();
            if (!atEnd() && (peek() == '+' || peek() == '-')) {
                advance();
            }
            if (atEnd() || !isDigit(peek())) {
                return fail(error, "expected digit in exponent");
            }
            while (!atEnd() && isDigit(peek())) {
                advance();
            }
        }
        out = JsonValue::number(std::string(src.substr(start, pos - start)));
        return true;
    }

    // Reads exactly 4 hex digits into a codepoint (the payload of a \uXXXX escape).
    bool lexHex4(std::uint32_t& out, JsonParseError& error) {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            if (atEnd()) {
                return fail(error, "unterminated \\u escape");
            }
            const char c = peek();
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return fail(error, "invalid \\u escape digit");
            }
            value = (value << 4) | digit;
            advance();
        }
        out = value;
        return true;
    }

    // Full RFC 8259 escape set incl. \uXXXX with surrogate-pair recombination -> UTF-8 (D7). Raw
    // control bytes (< 0x20) are errors; bytes >= 0x20 pass through unvalidated (mirrors the writer's
    // own passthrough, F7) — this is D7 tolerance 3.
    bool lexString(std::string& out, JsonParseError& error) {
        advance();  // caller has verified peek() == '"'
        out.clear();
        for (;;) {
            if (atEnd()) {
                return fail(error, "unterminated string");
            }
            const auto c = static_cast<unsigned char>(peek());
            if (c == '"') {
                advance();
                return true;
            }
            if (c == '\\') {
                advance();
                if (atEnd()) {
                    return fail(error, "unterminated escape sequence");
                }
                const char esc = peek();
                switch (esc) {
                    case '"':
                        out += '"';
                        advance();
                        break;
                    case '\\':
                        out += '\\';
                        advance();
                        break;
                    case '/':
                        out += '/';
                        advance();
                        break;
                    case 'b':
                        out += '\b';
                        advance();
                        break;
                    case 'f':
                        out += '\f';
                        advance();
                        break;
                    case 'n':
                        out += '\n';
                        advance();
                        break;
                    case 'r':
                        out += '\r';
                        advance();
                        break;
                    case 't':
                        out += '\t';
                        advance();
                        break;
                    case 'u': {
                        advance();  // consume 'u'
                        std::uint32_t codepoint = 0;
                        if (!lexHex4(codepoint, error)) {
                            return false;
                        }
                        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                            if (atEnd() || peek() != '\\') {
                                return fail(error, "unpaired UTF-16 surrogate");
                            }
                            advance();
                            if (atEnd() || peek() != 'u') {
                                return fail(error, "unpaired UTF-16 surrogate");
                            }
                            advance();
                            std::uint32_t low = 0;
                            if (!lexHex4(low, error)) {
                                return false;
                            }
                            if (low < 0xDC00 || low > 0xDFFF) {
                                return fail(error, "invalid low surrogate");
                            }
                            const std::uint32_t combined = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                            appendUtf8(out, combined);
                        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                            return fail(error, "unpaired UTF-16 surrogate");
                        } else {
                            appendUtf8(out, codepoint);
                        }
                        break;
                    }
                    default:
                        return fail(error, "invalid escape character");
                }
                continue;
            }
            if (c < 0x20) {
                return fail(error, "control character in string");
            }
            out += static_cast<char>(c);
            advance();
        }
    }

    std::string_view src;
    std::size_t pos = 0;
    std::uint32_t line = 1;
    std::size_t lineStart = 0;
    std::uint32_t maxDepth;
    std::vector<Frame> stack;
    JsonValue root;
};

}  // namespace

bool JsonParseResult::ok() const { return value.has_value(); }

JsonParseResult parseJson(std::string_view text, const JsonParseConfig& config) {
    Parser parser(text, config);
    return parser.run();
}

}  // namespace engine

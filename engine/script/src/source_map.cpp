// Aero Engine -- the source-map core (task 4.3.4): the URL and path rules, the version-3 map decoder, the lookup, the
// frame rewrite and the store. No backend header and no VFS read: tested byte for byte at tier 0 (SR1-SR9). Every
// loop here is iterative and every function total over any input.

#include <aero/core/vfs.hpp>  // normalizeVirtualPath: a map key is the very path a VFS read uses
#include <aero/reflect/json_reader.hpp>
#include <aero/script/internal/source_map.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace engine::script::internal {
namespace {

constexpr std::string_view SCHEME = "res://";
constexpr std::int64_t MAX_POSITION = 0x7FFFFFFF;  // every decoded position stays within 31 bits

[[nodiscard]] int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// nullopt for a '%' not followed by two hex digits.
[[nodiscard]] std::optional<std::string> percentDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out.push_back(text[i]);
            continue;
        }
        if (text.size() - i < 3) {
            return std::nullopt;
        }
        const int high = hexValue(text[i + 1]);
        const int low = hexValue(text[i + 2]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        out.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return out;
}

// The key up to and including its last '/'; "" when it has none.
[[nodiscard]] std::string_view directoryPrefix(std::string_view key) noexcept {
    const std::size_t slash = key.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : std::string_view{key.data(), slash + 1};
}

// A ':' before the first '/' (or anywhere, when there is no '/'): a scheme, a drive letter, anything the VM leaves
// alone.
[[nodiscard]] bool namesScheme(std::string_view text) noexcept {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    const std::size_t slash = text.find('/');
    return slash == std::string_view::npos || colon < slash;
}

// "res://<normalised>" for an addressable, non-root path; nullopt otherwise.
[[nodiscard]] std::optional<std::string> canonicalKey(std::string_view path) {
    const std::optional<std::string> normalised = normalizeVirtualPath(path);
    if (!normalised.has_value() || normalised->empty()) {
        return std::nullopt;
    }
    std::string key{SCHEME};
    key += *normalised;
    return key;
}

// ---- the VLQ walk ---------------------------------------------------------------------------------------------

[[nodiscard]] int base64Digit(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

enum class VlqFailure : std::uint8_t { None, NotBase64, Unterminated, Overflow };

// One value: a digit carries 5 data bits, least significant group first; bit 5 says another digit follows; the lowest
// bit of the value is its sign. Seven digits carry 35 bits; the magnitude must fit in 31.
[[nodiscard]] VlqFailure readVlq(std::string_view text, std::size_t& at, std::int64_t& value) noexcept {
    std::uint64_t accumulated = 0;
    unsigned shift = 0;
    for (;;) {
        if (at == text.size()) {
            return VlqFailure::Unterminated;
        }
        const int digit = base64Digit(text[at++]);
        if (digit < 0) {
            return VlqFailure::NotBase64;
        }
        accumulated |= static_cast<std::uint64_t>(digit & 31) << shift;
        if ((digit & 32) == 0) {
            break;
        }
        shift += 5;
        if (shift > 30) {
            return VlqFailure::Overflow;
        }
    }
    const std::uint64_t magnitude = accumulated >> 1U;
    if (magnitude > static_cast<std::uint64_t>(MAX_POSITION)) {
        return VlqFailure::Overflow;
    }
    value = (accumulated & 1U) != 0 ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
    return VlqFailure::None;
}

[[nodiscard]] std::string_view vlqFailureText(VlqFailure failure) noexcept {
    switch (failure) {
        case VlqFailure::NotBase64:
            return "a character outside base64";
        case VlqFailure::Unterminated:
            return "a value that does not end";
        case VlqFailure::Overflow:
            return "a value over 31 bits";
        case VlqFailure::None:
            break;
    }
    return "";
}

[[nodiscard]] std::string mappingsRefusal(std::string_view what, std::size_t offset) {
    return std::format("its mappings do not decode: {} at byte {}", what, offset);
}

// Adds a delta to a running position; the answer text is empty when it stayed in [0, MAX_POSITION].
[[nodiscard]] std::string_view stepPosition(std::int64_t& running, std::int64_t delta) noexcept {
    const std::int64_t next = running + delta;
    if (next < 0) {
        return "a position below zero";
    }
    if (next > MAX_POSITION) {
        return "a position over 31 bits";
    }
    running = next;
    return "";
}

// The whole of `mappings`: "" on success, else the refusal's reason. `map.sources` is already set.
[[nodiscard]] std::string decodeMappings(std::string_view text, DecodedSourceMap& map) {
    map.lineStarts.push_back(0);
    std::int64_t generatedColumn = 0;
    std::int64_t source = 0;
    std::int64_t originalLine = 0;
    std::int64_t originalColumn = 0;
    bool afterComma = false;
    std::size_t at = 0;
    for (;;) {
        if (at == text.size()) {
            if (afterComma) {
                return mappingsRefusal("an empty segment", at);
            }
            break;
        }
        if (text[at] == ';' && !afterComma) {
            map.lineStarts.push_back(static_cast<std::uint32_t>(map.segments.size()));
            generatedColumn = 0;
            ++at;
            continue;
        }
        if (text[at] == ',' || text[at] == ';') {
            return mappingsRefusal("an empty segment", at);
        }
        const std::size_t begin = at;
        std::array<std::int64_t, 5> fields{};
        std::size_t count = 0;
        for (;;) {
            std::int64_t value = 0;
            const VlqFailure failure = readVlq(text, at, value);
            if (failure != VlqFailure::None) {
                return mappingsRefusal(vlqFailureText(failure), begin);
            }
            if (count < fields.size()) {
                fields[count] = value;
            }
            ++count;
            if (at == text.size() || text[at] == ',' || text[at] == ';') {
                break;
            }
        }
        if (count != 1 && count != 4 && count != 5) {
            return mappingsRefusal(std::format("a segment of {} fields", count), begin);
        }
        MapSegment segment;
        std::string_view problem = stepPosition(generatedColumn, fields[0]);
        if (problem.empty() && count >= 4) {
            const std::int64_t nextSource = source + fields[1];
            if (nextSource < 0 || nextSource >= static_cast<std::int64_t>(map.sources.size())) {
                problem = "a source index out of range";
            } else {
                source = nextSource;
                problem = stepPosition(originalLine, fields[2]);
                if (problem.empty()) {
                    problem = stepPosition(originalColumn, fields[3]);
                }
            }
        }
        if (!problem.empty()) {
            return mappingsRefusal(problem, begin);
        }
        segment.generatedColumn = static_cast<std::uint32_t>(generatedColumn);
        if (count >= 4) {
            segment.source = static_cast<std::uint32_t>(source);
            segment.originalLine = static_cast<std::uint32_t>(originalLine);
            segment.originalColumn = static_cast<std::uint32_t>(originalColumn);
        }
        map.segments.push_back(segment);
        afterComma = false;
        if (at < text.size() && text[at] == ',') {
            afterComma = true;
            ++at;
        }
    }
    map.lineStarts.push_back(static_cast<std::uint32_t>(map.segments.size()));
    // A line out of generated-column order is sorted, stably: the lookup binary-searches it, and a tie keeps the
    // map's own order.
    const auto byColumn = [](const MapSegment& a, const MapSegment& b) {
        return a.generatedColumn < b.generatedColumn;
    };
    for (std::size_t line = 0; line + 1 < map.lineStarts.size(); ++line) {
        const auto first = map.segments.begin() + static_cast<std::ptrdiff_t>(map.lineStarts[line]);
        const auto last = map.segments.begin() + static_cast<std::ptrdiff_t>(map.lineStarts[line + 1]);
        if (!std::is_sorted(first, last, byColumn)) {
            std::stable_sort(first, last, byColumn);
        }
    }
    return {};
}

// ---- the frame line --------------------------------------------------------------------------------------------

struct FrameLine {
    std::size_t fileBegin = 0;    // the file name's first byte
    std::size_t positionEnd = 0;  // one past the column's last digit
    std::string_view file;        // empty: positioned, but no "res://" file to look up
    std::uint32_t line = 0;
    std::uint32_t column = 0;
};

[[nodiscard]] bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

// Reads the 1 to 10 digits that end at `end` (exclusive); `begin` is where they start. False when there are none, too
// many, or the value does not fit in 32 bits.
[[nodiscard]] bool digitsBefore(std::string_view text, std::size_t end, std::size_t& begin,
                                std::uint32_t& value) noexcept {
    begin = end;
    while (begin > 0 && isDigit(text[begin - 1])) {
        --begin;
    }
    const std::size_t count = end - begin;
    if (count == 0 || count > 10) {
        return false;
    }
    std::uint64_t parsed = 0;
    const std::from_chars_result result = std::from_chars(text.data() + begin, text.data() + end, parsed);
    if (result.ec != std::errc{} || parsed > UINT32_MAX) {
        return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

// Parenthesised: the line ends ":<line>:<column>)" and holds a " (" -- the file starts after the LAST " (res://"'s
// " (", or is empty when there is none. Compile: the line starts "    at ", does not end ')', and ends
// ":<line>:<column>" -- the file is what lies between. nullopt for any other line (native, "missing", a name continued
// from the previous line, a forged text).
[[nodiscard]] std::optional<FrameLine> parseFrameLine(std::string_view text) noexcept {
    constexpr std::string_view COMPILE_PREFIX = "    at ";
    if (text.empty()) {
        return std::nullopt;
    }
    const bool parenthesised = text.back() == ')';
    FrameLine frame;
    frame.positionEnd = parenthesised ? text.size() - 1 : text.size();
    std::size_t columnBegin = 0;
    if (!digitsBefore(text, frame.positionEnd, columnBegin, frame.column) || columnBegin == 0 ||
        text[columnBegin - 1] != ':') {
        return std::nullopt;
    }
    std::size_t lineBegin = 0;
    if (!digitsBefore(text, columnBegin - 1, lineBegin, frame.line) || lineBegin == 0 || text[lineBegin - 1] != ':') {
        return std::nullopt;
    }
    const std::size_t fileEnd = lineBegin - 1;
    if (parenthesised) {
        const std::string_view head{text.data(), fileEnd};
        const std::size_t resource = head.rfind(" (res://");
        if (resource != std::string_view::npos) {
            frame.fileBegin = resource + 2;
        } else if (head.find(" (") != std::string_view::npos) {
            frame.fileBegin = fileEnd;
        } else {
            return std::nullopt;
        }
    } else {
        if (!text.starts_with(COMPILE_PREFIX) || fileEnd < COMPILE_PREFIX.size()) {
            return std::nullopt;
        }
        frame.fileBegin = COMPILE_PREFIX.size();
    }
    frame.file = std::string_view{text.data() + frame.fileBegin, fileEnd - frame.fileBegin};
    return frame;
}

}  // namespace

// ---- the URL and the paths -------------------------------------------------------------------------------------

std::optional<std::string_view> sourceMappingUrl(std::string_view text) noexcept {
    if (text.ends_with('\n')) {
        text.remove_suffix(1);
    }
    const std::size_t newline = text.rfind('\n');
    const std::string_view last = newline == std::string_view::npos
                                      ? text
                                      : std::string_view{text.data() + newline + 1, text.size() - newline - 1};
    if (!last.starts_with(SOURCE_MAPPING_URL_PREFIX)) {
        return std::nullopt;
    }
    std::string_view url{last.data() + SOURCE_MAPPING_URL_PREFIX.size(),
                         last.size() - SOURCE_MAPPING_URL_PREFIX.size()};
    while (!url.empty() && (url.back() == ' ' || url.back() == '\t' || url.back() == '\r')) {
        url.remove_suffix(1);
    }
    if (url.empty() || url.find_first_of(" \t") != std::string_view::npos) {
        return std::nullopt;
    }
    return url;
}

std::optional<std::string> resolveSourceMapPath(std::string_view moduleKey, std::string_view url) {
    if (url.empty() || url.find_first_of("?#") != std::string_view::npos) {
        return std::nullopt;
    }
    std::string joined;
    if (url.starts_with(SCHEME)) {
        const std::optional<std::string> decoded = percentDecode(url);
        if (!decoded.has_value()) {
            return std::nullopt;
        }
        joined = *decoded;
    } else {
        if (namesScheme(url) || url.front() == '/') {
            return std::nullopt;
        }
        const std::optional<std::string> decoded = percentDecode(url);
        if (!decoded.has_value()) {
            return std::nullopt;
        }
        joined = std::string{directoryPrefix(moduleKey)};
        joined += *decoded;
    }
    return canonicalKey(joined);
}

std::string sourceDisplayName(std::string_view mapPath, std::string_view sourceRoot, std::string_view source) {
    std::string spelled{sourceRoot};
    if (!spelled.empty() && spelled.back() != '/') {
        spelled.push_back('/');
    }
    spelled += source;
    if (spelled.empty()) {
        return spelled;
    }
    const bool absolute = std::string_view{spelled}.starts_with(SCHEME);
    if (!absolute && (namesScheme(spelled) || spelled.front() == '/')) {
        return spelled;
    }
    const std::optional<std::string> decoded = percentDecode(spelled);
    if (!decoded.has_value()) {
        return spelled;
    }
    std::string joined;
    if (!absolute) {
        joined = std::string{directoryPrefix(mapPath)};
    }
    joined += *decoded;
    std::optional<std::string> key = canonicalKey(joined);
    return key.has_value() ? std::move(*key) : spelled;
}

// ---- the decoded map ----------------------------------------------------------------------------------------------

SourceMapDecode decodeSourceMap(std::string_view mapPath, std::string_view json) {
    const auto refuse = [](std::string reason) {
        SourceMapDecode refused;
        refused.refusal = std::move(reason);
        return refused;
    };
    const JsonParseResult parsed = parseJson(json);
    if (!parsed.ok() || !parsed.value.has_value()) {
        return refuse(std::format("it is not JSON: {} (line {}, column {})", parsed.error.message, parsed.error.line,
                                  parsed.error.column));
    }
    const JsonValue& root = *parsed.value;
    if (!root.isObject()) {
        return refuse("it is not a JSON object");
    }
    const JsonValue* version = root.find("version");
    if (version == nullptr || version->asI64() != std::optional<std::int64_t>{3}) {
        return refuse("its version is not 3");
    }
    if (root.find("sections") != nullptr) {
        return refuse("it is an index map (sections), which the VM does not read");
    }
    const JsonValue* sources = root.find("sources");
    bool sourcesOk = sources != nullptr && sources->isArray();
    if (sourcesOk) {
        for (const JsonValue& element : sources->elements()) {
            sourcesOk = sourcesOk && element.isString();
        }
    }
    if (!sourcesOk) {
        return refuse("its sources are not an array of strings");
    }
    std::string_view sourceRoot;
    const JsonValue* rootValue = root.find("sourceRoot");
    if (rootValue != nullptr && !rootValue->isNull()) {
        const std::optional<std::string_view> text = rootValue->asString();
        if (!text.has_value()) {
            return refuse("its sourceRoot is not a string");
        }
        sourceRoot = *text;
    }
    const JsonValue* mappings = root.find("mappings");
    if (mappings == nullptr || !mappings->isString()) {
        return refuse("its mappings are not a string");
    }
    DecodedSourceMap map;
    map.sources.reserve(sources->size());
    for (const JsonValue& element : sources->elements()) {
        map.sources.push_back(sourceDisplayName(mapPath, sourceRoot, element.asString().value_or("")));
    }
    std::string problem = decodeMappings(mappings->asString().value_or(""), map);
    if (!problem.empty()) {
        return refuse(std::move(problem));
    }
    SourceMapDecode out;
    out.map = std::move(map);
    return out;
}

std::optional<OriginalPosition> originalPositionFor(const DecodedSourceMap& map, std::uint32_t line,
                                                    std::uint32_t column) noexcept {
    if (line == 0 || map.lineStarts.empty() || line >= map.lineStarts.size()) {
        return std::nullopt;
    }
    // The backend prints a byte column 0-based on line 1 and 1-based on every later line.
    const std::uint32_t column0 = line == 1 ? column : (column == 0 ? 0 : column - 1);
    const auto first = map.segments.begin() + static_cast<std::ptrdiff_t>(map.lineStarts[line - 1]);
    const auto last = map.segments.begin() + static_cast<std::ptrdiff_t>(map.lineStarts[line]);
    const auto past = std::upper_bound(first, last, column0, [](std::uint32_t wanted, const MapSegment& segment) {
        return wanted < segment.generatedColumn;
    });
    if (past == first) {
        return std::nullopt;
    }
    const MapSegment& segment = *std::prev(past);
    if (segment.source == NO_SOURCE) {
        return std::nullopt;
    }
    return OriginalPosition{segment.source, segment.originalLine + 1, segment.originalColumn + 1};
}

std::string mapTooLargeReason(std::uint64_t bytes, std::size_t capBytes) {
    return std::format("its {} bytes are over the {}-byte limit (the VM's heap limit)", bytes, capBytes);
}

std::string sourceMapNotUsedMessage(std::string_view mapPath, std::string_view reason) {
    return std::format("script: {}: source map not used: {}; stack frames name the JavaScript", mapPath, reason);
}

// ---- the store and the rewrite ---------------------------------------------------------------------------------

void SourceMapStore::keepMap(std::string moduleName, std::string mapPath, std::string bytes) {
    Slot slot;
    slot.state = SourceMapSlotState::Read;
    slot.mapPath = std::move(mapPath);
    slot.bytes = std::move(bytes);
    slots.insert_or_assign(std::move(moduleName), std::move(slot));
}

void SourceMapStore::keepUnusable(std::string moduleName, std::string mapPath, std::string reason) {
    Slot slot;
    slot.state = SourceMapSlotState::Unusable;
    slot.mapPath = std::move(mapPath);
    slot.reason = std::move(reason);
    slots.insert_or_assign(std::move(moduleName), std::move(slot));
}

void SourceMapStore::forget(std::string_view moduleName) {
    const auto found = slots.find(moduleName);
    if (found != slots.end()) {
        slots.erase(found);
    }
}

std::optional<SourceMapSlotState> SourceMapStore::slotState(std::string_view moduleName) const {
    const auto found = slots.find(moduleName);
    if (found == slots.end()) {
        return std::nullopt;
    }
    return found->second.state;
}

MappedStack SourceMapStore::mapStack(std::string_view stack) {
    MappedStack out;
    if (slots.empty()) {
        out.stack.assign(stack);
        return out;
    }
    out.stack.reserve(stack.size());
    bool seenPositioned = false;
    std::size_t pos = 0;
    for (std::size_t lineNumber = 0; pos < stack.size() && lineNumber < MAX_MAPPED_STACK_LINES; ++lineNumber) {
        const std::size_t newline = stack.find('\n', pos);
        const std::size_t lineEnd = newline == std::string_view::npos ? stack.size() : newline;
        const std::string_view line{stack.data() + pos, lineEnd - pos};
        pos = newline == std::string_view::npos ? stack.size() : newline + 1;

        const std::optional<FrameLine> parsed = parseFrameLine(line);
        const bool first = parsed.has_value() && !seenPositioned;
        seenPositioned = seenPositioned || parsed.has_value();
        const FrameLine frame = parsed.value_or(FrameLine{});
        std::optional<std::string> position;
        if (!frame.file.empty()) {
            const auto found = slots.find(frame.file);
            if (found != slots.end()) {
                Slot& slot = found->second;
                if (slot.state == SourceMapSlotState::Read) {
                    SourceMapDecode decoded = decodeSourceMap(slot.mapPath, slot.bytes);
                    std::string{}.swap(slot.bytes);
                    if (decoded.map.has_value()) {
                        slot.state = SourceMapSlotState::Decoded;
                        slot.decoded = std::move(*decoded.map);
                    } else {
                        slot.state = SourceMapSlotState::Unusable;
                        slot.reason = std::move(decoded.refusal);
                    }
                }
                if (slot.state == SourceMapSlotState::Unusable) {
                    if (warnedMaps.insert(slot.mapPath).second) {
                        out.warnings.push_back(sourceMapNotUsedMessage(slot.mapPath, slot.reason));
                    }
                } else {
                    const std::optional<OriginalPosition> original =
                        originalPositionFor(slot.decoded, frame.line, frame.column);
                    if (original.has_value()) {
                        position = std::format("{}:{}:{}", slot.decoded.sources[original->source], original->line,
                                               original->column);
                    }
                }
            }
        }
        if (position.has_value()) {
            out.stack.append(line.substr(0, frame.fileBegin));
            out.stack.append(*position);
            out.stack.append(line.substr(frame.positionEnd));
            if (first) {
                out.site = std::move(position);
            }
        } else {
            out.stack.append(line);
        }
        if (newline != std::string_view::npos) {
            out.stack.push_back('\n');
        }
    }
    if (pos < stack.size()) {
        out.stack.append(stack.substr(pos));
    }
    return out;
}

}  // namespace engine::script::internal

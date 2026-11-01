#pragma once
// Aero Engine -- the source-map core (task 4.3.4; internal to engine/script): the URL a module's last line names, the
// map path it resolves to, a version-3 map decoded into per-line segments, the lookup from a generated position to an
// original one, the frame rewrite over a stack text, and the store that keeps each module generation's map. Pure: no
// backend, no VFS read, no clock, no log, no recursion; every function is total over any byte string. Tested at tier 0
// (tests/script_source_map_test.cpp, SR1-SR9) through aero::script_internal, the module_graph.hpp precedent; the glue
// that reads a map through the VFS and hands a described stack to mapStack lives in src/.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script::internal {

inline constexpr std::string_view SOURCE_MAPPING_URL_PREFIX = "//# sourceMappingURL=";
// The stack lines one rewrite reads; the rest are copied as they are. The backend prints at most 64 frames and this VM
// 10, so the cap loses no frame and bounds the output.
inline constexpr std::size_t MAX_MAPPED_STACK_LINES = 64;
inline constexpr std::uint32_t NO_SOURCE = 0xFFFFFFFFU;  // a one-field segment: a generated span with no source
inline constexpr std::string_view MAP_UNREADABLE_REASON = "it could not be read through the VFS";

// ---- the URL and the paths --------------------------------------------------------------------------------------
// The URL the text's LAST line names: one final "\n" is dropped, the last line is what follows the last remaining
// "\n" (all of it when none), and it must begin with SOURCE_MAPPING_URL_PREFIX. The URL is the rest with trailing ' ',
// '\t' and '\r' removed; nullopt when that is empty or still holds ' ' or '\t'. A view into `text`.
[[nodiscard]] std::optional<std::string_view> sourceMappingUrl(std::string_view text) noexcept;
// The map's canonical key ("res://..."), or nullopt when the VM follows nothing: a ':' before the URL's first '/' (any
// scheme, "data:" included) other than a leading "res://", a '/'-rooted URL, a raw '?' or '#', a '%' not followed by
// two hex digits, or a result normalizeVirtualPath refuses or that names the root. An absolute "res://" URL is decoded
// and normalised; a relative one is decoded and appended to `moduleKey`'s directory (the key up to and including its
// last '/'), which is never decoded itself: a canonical key holds raw bytes.
[[nodiscard]] std::optional<std::string> resolveSourceMapPath(std::string_view moduleKey, std::string_view url);
// What a frame shows for sources[i]: spelled = sourceRoot + ('/' when sourceRoot is non-empty and does not end in '/')
// + source. A spelled text starting "res://", or any other text without a scheme and not '/'-rooted, is
// percent-decoded and normalised -- the latter after being appended to mapPath's directory -- and becomes
// "res://<normalised>"; every other case (a scheme, '/'-rooted, a bad escape, refused by the normaliser, the root) is
// `spelled` exactly.
[[nodiscard]] std::string sourceDisplayName(std::string_view mapPath, std::string_view sourceRoot,
                                            std::string_view source);

// ---- the decoded map --------------------------------------------------------------------------------------------
struct MapSegment {                     // 16 bytes
    std::uint32_t generatedColumn = 0;  // 0-based, the map's unit (UTF-16; esbuild's code lines are ASCII)
    std::uint32_t source = NO_SOURCE;   // an index into DecodedSourceMap::sources
    std::uint32_t originalLine = 0;     // 0-based
    std::uint32_t originalColumn = 0;   // 0-based, UTF-16 units
};
struct DecodedSourceMap {
    std::vector<std::string> sources;       // each already sourceDisplayName's answer
    std::vector<MapSegment> segments;       // by generated line, then by generated column (stable)
    std::vector<std::uint32_t> lineStarts;  // size = generated lines + 1; line i is [lineStarts[i], [i + 1])
};
struct SourceMapDecode {
    std::optional<DecodedSourceMap> map;
    std::string refusal;  // non-empty iff !map
};
// engine::parseJson, the field checks, then the whole of `mappings`; the parsed value is dropped before returning.
[[nodiscard]] SourceMapDecode decodeSourceMap(std::string_view mapPath, std::string_view json);

struct OriginalPosition {
    std::uint32_t source = 0;
    std::uint32_t line = 0;    // 1-based
    std::uint32_t column = 0;  // 1-based, UTF-16 units
};
// `line` and `column` exactly as the backend prints them: `line` 1-based, `column` 0-based on line 1 and 1-based on
// every later line. The last segment of the generated line whose column is not past the position answers; no earlier
// line is searched. nullopt: nothing to map.
[[nodiscard]] std::optional<OriginalPosition> originalPositionFor(const DecodedSourceMap& map, std::uint32_t line,
                                                                  std::uint32_t column) noexcept;

// "its <bytes> bytes are over the <cap>-byte limit (the VM's heap limit)"
[[nodiscard]] std::string mapTooLargeReason(std::uint64_t bytes, std::size_t capBytes);
// "script: <mapPath>: source map not used: <reason>; stack frames name the JavaScript"
[[nodiscard]] std::string sourceMapNotUsedMessage(std::string_view mapPath, std::string_view reason);

// ---- the store and the rewrite ----------------------------------------------------------------------------------
enum class SourceMapSlotState : std::uint8_t { Read, Decoded, Unusable };
struct MappedStack {
    std::string stack;                  // the rewritten text (the input's bytes wherever nothing mapped)
    std::optional<std::string> site;    // "<source>:<line>:<col>" iff the first positioned frame mapped
    std::vector<std::string> warnings;  // sourceMapNotUsedMessage texts first owed by this rewrite
};
class SourceMapStore {
public:
    // Each replaces whatever `moduleName` held: the latest read of a name is the one its live module came from.
    void keepMap(std::string moduleName, std::string mapPath, std::string bytes);        // -> Read
    void keepUnusable(std::string moduleName, std::string mapPath, std::string reason);  // -> Unusable
    void forget(std::string_view moduleName);                                            // the name has no map
    // Rewrites the first MAX_MAPPED_STACK_LINES lines; a Read slot a frame names is decoded here, once, and becomes
    // Decoded (its bytes released) or Unusable. An Unusable slot a frame names owes one warning per map path per
    // store. An empty store returns the input at once.
    [[nodiscard]] MappedStack mapStack(std::string_view stack);
    [[nodiscard]] std::optional<SourceMapSlotState> slotState(std::string_view moduleName) const;

private:
    struct Slot {
        SourceMapSlotState state = SourceMapSlotState::Read;
        std::string mapPath;
        std::string bytes;         // Read only; released on leaving Read
        std::string reason;        // Unusable only
        DecodedSourceMap decoded;  // Decoded only
    };
    std::map<std::string, Slot, std::less<>> slots;  // by the module name the backend holds
    std::set<std::string, std::less<>> warnedMaps;   // map paths already warned about
};

}  // namespace engine::script::internal

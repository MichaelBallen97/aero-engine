// Aero Engine -- esbuild_tool.cpp: the PURE half of the esbuild integration (task 4.3.1). Nothing here logs, touches
// the disk, reads the environment, spawns a process or throws. <array> is included EXPLICITLY: it is not transitive
// on libstdc++ or MSVC.
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/asset_cache.hpp>  // ASSET_CACHE_DIR_NAME -- the one spelling of "Library"
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/blender_tool.hpp>
#include <aero/editor/esbuild_tool.hpp>
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

std::string_view esbuildPinnedVersion() noexcept {
    return AERO_ESBUILD_VERSION;  // unguarded on purpose: a missing definition is a compile error, never a default
}

namespace {

[[nodiscard]] bool endsWith(std::string_view text, std::string_view suffix) noexcept { return text.ends_with(suffix); }

[[nodiscard]] bool startsWith(std::string_view text, std::string_view prefix) noexcept {
    return text.starts_with(prefix);
}

// Slices that cannot throw: a bad position makes the library's own slicing call throw, and these are noexcept.
[[nodiscard]] std::string_view dropFront(std::string_view text, std::size_t count) noexcept {
    text.remove_prefix(std::min(count, text.size()));
    return text;
}

[[nodiscard]] std::string_view takeFront(std::string_view text, std::size_t count) noexcept {
    return {text.data(), std::min(count, text.size())};
}

[[nodiscard]] std::string_view segmentAfter(std::string_view path, std::size_t& cursor) noexcept {
    const std::size_t end = path.find('/', cursor);
    const std::size_t stop = end == std::string_view::npos ? path.size() : end;
    const std::string_view segment = takeFront(dropFront(path, cursor), stop - cursor);
    cursor = end == std::string_view::npos ? path.size() + 1 : end + 1;
    return segment;
}

[[nodiscard]] ContentHash hashOfText(std::string_view text) noexcept {
    return hashBytes(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

}  // namespace

bool isScriptAssetName(std::string_view assetsRelativePath) noexcept {
    const std::size_t slash = assetsRelativePath.rfind('/');
    const std::string_view name =
        slash == std::string_view::npos ? assetsRelativePath : dropFront(assetsRelativePath, slash + 1);
    constexpr std::string_view EXTENSION = ".ts";
    constexpr std::string_view DECLARATION = ".d.ts";
    if (name.size() <= EXTENSION.size() || !endsWith(name, EXTENSION)) {
        return false;
    }
    return !endsWith(assetsRelativePath, DECLARATION);
}

std::string_view esbuildExecutableName(HostOs os) noexcept {
    return os == HostOs::Windows ? std::string_view("esbuild.exe") : std::string_view("esbuild");
}

std::string resolveEsbuildPath(HostOs os, std::string_view configured, std::string_view envValue,
                               std::string_view baseDirectory) {
    if (!configured.empty()) {
        return std::string(configured);
    }
    if (!envValue.empty()) {
        return std::string(envValue);
    }
    if (baseDirectory.empty()) {
        return {};
    }
    std::string path(baseDirectory);
    if (path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path += esbuildExecutableName(os);
    return path;
}

std::string assetsPrefixOf(std::string_view manifestAssetsPath) {
    std::string prefix;
    std::size_t cursor = 0;
    while (cursor <= manifestAssetsPath.size()) {
        const std::string_view segment = segmentAfter(manifestAssetsPath, cursor);
        if (segment.empty() || segment == ".") {
            continue;
        }
        if (!prefix.empty()) {
            prefix += '/';
        }
        prefix += segment;
    }
    return prefix;
}

std::string projectRelativeOf(std::string_view assetsPrefix, std::string_view assetsRelative) {
    std::string path;
    if (!assetsPrefix.empty()) {
        path.append(assetsPrefix);
        path += '/';
    }
    path.append(assetsRelative);
    return path;
}

EsbuildRunPaths esbuildRunPaths(std::string_view assetsPrefix, std::string_view scriptAssetsRelative, Guid guid) {
    const std::string guidText = formatGuid(guid);
    std::string stem(ASSET_CACHE_DIR_NAME);
    stem += '/';
    stem.append(SCRIPT_STAGING_DIR_NAME);
    stem += '/';
    stem += guidText;

    EsbuildRunPaths paths;
    paths.entry = projectRelativeOf(assetsPrefix, scriptAssetsRelative);
    paths.outfile = stem + ".js";
    paths.metafile = stem + ".meta.json";
    return paths;
}

std::vector<std::string> buildEsbuildArgs(std::string_view binary, const EsbuildRunPaths& paths) {
    std::vector<std::string> args;
    args.reserve(ESBUILD_FIXED_FLAGS.size() + 4);
    args.emplace_back(binary);
    args.push_back(paths.entry);
    for (const std::string_view flag : ESBUILD_FIXED_FLAGS) {
        args.emplace_back(flag);
    }
    args.push_back("--metafile=" + paths.metafile);
    args.push_back("--outfile=" + paths.outfile);
    return args;
}

std::string esbuildFlagsFingerprint() {
    std::string joined;
    for (const std::string_view flag : ESBUILD_FIXED_FLAGS) {
        joined.append(flag);
        joined += '\n';
    }
    const std::string text = formatContentHash(hashOfText(joined));
    return text;
}

// ---- input classification ------------------------------------------------------------------------------------------

ScriptInputVerdict classifyScriptInput(std::string_view projectRelativeInput, std::string_view assetsPrefix) noexcept {
    const std::string_view path = projectRelativeInput;
    if (path.empty() || path.front() == '/' || path.front() == '\\') {
        return ScriptInputVerdict::OutsideAssetsRoot;
    }
    bool packageImport = false;
    bool firstSegment = true;
    std::size_t cursor = 0;
    while (cursor <= path.size()) {
        const std::string_view segment = segmentAfter(path, cursor);
        if (segment == "..") {
            return ScriptInputVerdict::OutsideAssetsRoot;
        }
        if (firstSegment && segment.find(':') != std::string_view::npos) {
            return ScriptInputVerdict::OutsideAssetsRoot;
        }
        firstSegment = false;
        packageImport = packageImport || segment == "node_modules";
    }
    if (packageImport) {
        return ScriptInputVerdict::PackageImport;
    }
    std::size_t cursorFirst = 0;
    if (segmentAfter(path, cursorFirst) == ASSET_CACHE_DIR_NAME) {
        return ScriptInputVerdict::InsideLibrary;
    }
    if (assetsPrefix.empty()) {
        return ScriptInputVerdict::Accepted;
    }
    if (path.size() <= assetsPrefix.size() + 1 || !startsWith(path, assetsPrefix) || path[assetsPrefix.size()] != '/' ||
        path[assetsPrefix.size() + 1] == '/') {
        return ScriptInputVerdict::OutsideAssetsRoot;
    }
    return ScriptInputVerdict::Accepted;
}

std::string_view assetsRelativeOf(std::string_view projectRelativeInput, std::string_view assetsPrefix) noexcept {
    if (classifyScriptInput(projectRelativeInput, assetsPrefix) != ScriptInputVerdict::Accepted) {
        return {};
    }
    return assetsPrefix.empty() ? projectRelativeInput : dropFront(projectRelativeInput, assetsPrefix.size() + 1);
}

// ---- the metafile --------------------------------------------------------------------------------------------------

std::optional<EsbuildMetafile> parseEsbuildMetafile(std::string_view text) {
    const JsonParseResult parsed = parseJson(text);
    if (!parsed.ok() || !parsed.value.has_value() || !parsed.value->isObject()) {
        return std::nullopt;
    }
    const JsonValue* inputs = parsed.value->find("inputs");
    if (inputs == nullptr || !inputs->isObject()) {
        return std::nullopt;
    }
    EsbuildMetafile metafile;
    metafile.inputs.reserve(inputs->size());
    for (const JsonMember& member : inputs->members()) {
        if (member.key.empty()) {
            return std::nullopt;
        }
        std::string key = member.key;
        std::replace(key.begin(), key.end(), '\\', '/');
        metafile.inputs.push_back(std::move(key));
    }
    std::sort(metafile.inputs.begin(), metafile.inputs.end());
    metafile.inputs.erase(std::unique(metafile.inputs.begin(), metafile.inputs.end()), metafile.inputs.end());
    return metafile;
}

// ---- the log -------------------------------------------------------------------------------------------------------

namespace {

// esbuild's header marks as UTF-8 bytes: U+2718 and U+25B2. Spelled as byte values rather than as string literals:
// the editor keeps every literal 7-bit (editor.md).
//
// An error has two marks. esbuild's MsgKind.Icon (internal/logger/logger.go) prints the ASCII "X" when it runs on
// Windows with no WT_SESSION in its environment (Windows Terminal sets it), and U+2718 otherwise, so the mark follows
// the environment and not only the OS. Both are read on every host. The warning mark is U+25B2 in both branches.
constexpr std::array<char, 3> ERROR_MARK_BYTES{static_cast<char>(0xE2), static_cast<char>(0x9C),
                                               static_cast<char>(0x98)};
constexpr std::array<char, 3> WARNING_MARK_BYTES{static_cast<char>(0xE2), static_cast<char>(0x96),
                                                 static_cast<char>(0xB2)};
constexpr std::string_view ERROR_MARK{ERROR_MARK_BYTES.data(), ERROR_MARK_BYTES.size()};
constexpr std::string_view ERROR_MARK_ASCII = "X";
constexpr std::string_view WARNING_MARK{WARNING_MARK_BYTES.data(), WARNING_MARK_BYTES.size()};
constexpr std::string_view ERROR_TAG = " [ERROR] ";
constexpr std::string_view WARNING_TAG = " [WARNING] ";

[[nodiscard]] bool isAsciiBlank(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

[[nodiscard]] std::string_view trimBlank(std::string_view text) noexcept {
    while (!text.empty() && isAsciiBlank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isAsciiBlank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool allDigits(std::string_view text) noexcept {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// A decimal no greater than `limit`; nullopt past it. No exception, no std::stoul.
[[nodiscard]] std::optional<std::uint32_t> digitsToU32(std::string_view digits, std::uint32_t limit) noexcept {
    std::uint64_t value = 0;
    for (const char c : digits) {
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
        if (value > limit) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

struct HeaderMatch {
    ScriptDiagnosticSeverity severity = ScriptDiagnosticSeverity::Error;
    std::string_view text;
};

[[nodiscard]] std::optional<HeaderMatch> matchErrorHeader(std::string_view line, std::string_view mark) noexcept {
    if (startsWith(line, mark) && startsWith(dropFront(line, mark.size()), ERROR_TAG)) {
        return HeaderMatch{ScriptDiagnosticSeverity::Error, dropFront(line, mark.size() + ERROR_TAG.size())};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<HeaderMatch> matchHeader(std::string_view line) noexcept {
    for (const std::string_view mark : {ERROR_MARK, ERROR_MARK_ASCII}) {
        if (const std::optional<HeaderMatch> error = matchErrorHeader(line, mark)) {
            return error;
        }
    }
    if (startsWith(line, WARNING_MARK) && startsWith(dropFront(line, WARNING_MARK.size()), WARNING_TAG)) {
        return HeaderMatch{ScriptDiagnosticSeverity::Warning,
                           dropFront(line, WARNING_MARK.size() + WARNING_TAG.size())};
    }
    return std::nullopt;
}

struct LocationShape {
    std::string_view path;
    std::string_view lineDigits;
    std::string_view columnDigits;
};

// Exactly four spaces, then "<path>:<line>:<col>:" read from the right.
[[nodiscard]] std::optional<LocationShape> matchLocationLine(std::string_view line) noexcept {
    constexpr std::size_t INDENT = 4;
    if (line.size() <= INDENT || !startsWith(line, "    ") || line[INDENT] == ' ' || line.back() != ':') {
        return std::nullopt;
    }
    std::string_view rest = takeFront(dropFront(line, INDENT), line.size() - INDENT - 1);
    const std::size_t columnColon = rest.rfind(':');
    if (columnColon == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view columnDigits = dropFront(rest, columnColon + 1);
    rest = takeFront(rest, columnColon);
    const std::size_t lineColon = rest.rfind(':');
    if (lineColon == std::string_view::npos || lineColon == 0) {
        return std::nullopt;
    }
    const std::string_view lineDigits = dropFront(rest, lineColon + 1);
    if (!allDigits(columnDigits) || !allDigits(lineDigits)) {
        return std::nullopt;
    }
    return LocationShape{takeFront(rest, lineColon), lineDigits, columnDigits};
}

// A note paragraph starts at a line indented by exactly two spaces.
[[nodiscard]] bool isNoteLine(std::string_view line) noexcept {
    return line.size() > 2 && line[0] == ' ' && line[1] == ' ' && line[2] != ' ';
}

}  // namespace

EsbuildLogParse parseEsbuildLog(std::string_view logText) {
    EsbuildLogParse parse;
    bool haveFirstLine = false;
    bool locationDone = true;  // true until a header opens a message
    bool storing = false;      // the open message is within MAX_SCRIPT_DIAGNOSTICS
    constexpr std::uint32_t UINT32_LIMIT = std::numeric_limits<std::uint32_t>::max();
    std::size_t cursor = 0;
    while (cursor < logText.size()) {
        const std::size_t newline = logText.find('\n', cursor);
        const std::size_t stop = newline == std::string_view::npos ? logText.size() : newline;
        std::string_view line = takeFront(dropFront(logText, cursor), stop - cursor);
        cursor = newline == std::string_view::npos ? logText.size() : newline + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!haveFirstLine) {
            const std::string_view trimmed = trimBlank(line);
            if (!trimmed.empty()) {
                parse.firstLine = std::string(trimmed);
                haveFirstLine = true;
            }
        }
        if (const std::optional<HeaderMatch> header = matchHeader(line)) {
            ++parse.total;
            locationDone = false;
            storing = parse.diagnostics.size() < MAX_SCRIPT_DIAGNOSTICS;
            if (storing) {
                ScriptDiagnostic diagnostic;
                diagnostic.severity = header->severity;
                diagnostic.text = std::string(header->text);
                parse.diagnostics.push_back(std::move(diagnostic));
            }
            continue;
        }
        if (locationDone) {
            continue;
        }
        if (isNoteLine(line)) {
            locationDone = true;  // a location-shaped line after this belongs to the note
            continue;
        }
        const std::optional<LocationShape> shape = matchLocationLine(line);
        if (!shape.has_value()) {
            continue;
        }
        locationDone = true;
        if (!storing) {
            continue;  // a message past the cap is counted, not stored
        }
        // A line is 1-based and a column 0-based in esbuild's log; the stored column is 1-based, so it must still fit.
        const std::optional<std::uint32_t> lineNumber = digitsToU32(shape->lineDigits, UINT32_LIMIT);
        const std::optional<std::uint32_t> column = digitsToU32(shape->columnDigits, UINT32_LIMIT - 1);
        if (!lineNumber.has_value() || *lineNumber == 0 || !column.has_value()) {
            continue;
        }
        ScriptDiagnostic& diagnostic = parse.diagnostics.back();
        diagnostic.file = std::string(shape->path);
        std::replace(diagnostic.file.begin(), diagnostic.file.end(), '\\', '/');
        diagnostic.line = *lineNumber;
        diagnostic.column = *column + 1;
    }
    return parse;
}

std::string formatScriptDiagnostic(const ScriptDiagnostic& diagnostic, std::string_view scriptProjectRelative) {
    const std::string_view severity = diagnostic.severity == ScriptDiagnosticSeverity::Error ? "error" : "warning";
    std::string out;
    if (diagnostic.file.empty()) {
        out.append(scriptProjectRelative);
    } else {
        out += diagnostic.file;
        out += ':';
        out += std::to_string(diagnostic.line);
        out += ':';
        out += std::to_string(diagnostic.column);
    }
    out += ": ";
    out.append(severity);
    out += ": ";
    out += diagnostic.text;
    return out;
}

// ---- the record ----------------------------------------------------------------------------------------------------

namespace {

constexpr std::string_view KEY_VERSION = "version";
constexpr std::string_view KEY_STATUS = "status";
constexpr std::string_view KEY_SCRIPT = "script";
constexpr std::string_view KEY_ESBUILD_VERSION = "esbuildVersion";
constexpr std::string_view KEY_ESBUILD_BINARY = "esbuildBinary";
constexpr std::string_view KEY_FLAGS = "flags";
constexpr std::string_view KEY_INPUTS = "inputs";
constexpr std::string_view KEY_SOURCE_SET = "sourceSet";
constexpr std::string_view KEY_DIAGNOSTICS = "diagnostics";
constexpr std::string_view KEY_PATH = "path";
constexpr std::string_view KEY_HASH = "hash";
constexpr std::string_view KEY_SEVERITY = "severity";
constexpr std::string_view KEY_FILE = "file";
constexpr std::string_view KEY_LINE = "line";
constexpr std::string_view KEY_COLUMN = "column";
constexpr std::string_view KEY_TEXT = "text";

[[nodiscard]] std::optional<std::string> stringField(const JsonValue& object, std::string_view key) {
    const JsonValue* field = object.find(key);
    if (field == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::string_view> text = field->asString();
    if (!text.has_value()) {
        return std::nullopt;
    }
    return std::string(*text);
}

[[nodiscard]] std::optional<std::uint32_t> u32Field(const JsonValue& object, std::string_view key) {
    const JsonValue* field = object.find(key);
    if (field == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> value = field->asU64();
    if (!value.has_value() || *value > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*value);
}

[[nodiscard]] std::optional<ScriptRecordInput> parseRecordInput(const JsonValue& element) {
    if (!element.isObject()) {
        return std::nullopt;
    }
    std::optional<std::string> path = stringField(element, KEY_PATH);
    const JsonValue* hashField = element.find(KEY_HASH);
    if (!path.has_value() || hashField == nullptr) {
        return std::nullopt;
    }
    ScriptRecordInput input;
    input.path = std::move(*path);
    if (hashField->isString()) {
        const std::optional<std::string_view> hashText = hashField->asString();
        const std::optional<ContentHash> hash = hashText.has_value() ? parseContentHash(*hashText) : std::nullopt;
        if (!hash.has_value()) {
            return std::nullopt;
        }
        input.hash = *hash;
    } else if (!hashField->isNull()) {
        return std::nullopt;
    }
    return input;
}

[[nodiscard]] std::optional<ScriptDiagnostic> parseRecordDiagnostic(const JsonValue& element) {
    if (!element.isObject()) {
        return std::nullopt;
    }
    const std::optional<std::string> severity = stringField(element, KEY_SEVERITY);
    std::optional<std::string> file = stringField(element, KEY_FILE);
    std::optional<std::string> text = stringField(element, KEY_TEXT);
    const std::optional<std::uint32_t> line = u32Field(element, KEY_LINE);
    const std::optional<std::uint32_t> column = u32Field(element, KEY_COLUMN);
    if (!severity.has_value() || !file.has_value() || !text.has_value() || !line.has_value() || !column.has_value()) {
        return std::nullopt;
    }
    ScriptDiagnostic diagnostic;
    if (*severity == "error") {
        diagnostic.severity = ScriptDiagnosticSeverity::Error;
    } else if (*severity == "warning") {
        diagnostic.severity = ScriptDiagnosticSeverity::Warning;
    } else {
        return std::nullopt;
    }
    diagnostic.file = std::move(*file);
    diagnostic.line = *line;
    diagnostic.column = *column;
    diagnostic.text = std::move(*text);
    return diagnostic;
}

}  // namespace

std::optional<ScriptRecord> parseScriptRecord(std::string_view text) {
    const JsonParseResult parsed = parseJson(text);
    if (!parsed.ok() || !parsed.value.has_value() || !parsed.value->isObject()) {
        return std::nullopt;
    }
    const JsonValue& root = *parsed.value;

    const JsonValue* version = root.find(KEY_VERSION);
    if (version == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::int64_t> versionValue = version->asI64();
    if (!versionValue.has_value() || *versionValue != SCRIPT_RECORD_FORMAT_VERSION) {
        return std::nullopt;
    }

    ScriptRecord record;
    const std::optional<std::string> status = stringField(root, KEY_STATUS);
    if (!status.has_value()) {
        return std::nullopt;
    }
    if (*status == "ok") {
        record.status = ScriptRecordStatus::Ok;
    } else if (*status == "failed") {
        record.status = ScriptRecordStatus::Failed;
    } else {
        return std::nullopt;
    }

    std::optional<std::string> script = stringField(root, KEY_SCRIPT);
    std::optional<std::string> esbuildVersion = stringField(root, KEY_ESBUILD_VERSION);
    std::optional<std::string> esbuildBinary = stringField(root, KEY_ESBUILD_BINARY);
    std::optional<std::string> flags = stringField(root, KEY_FLAGS);
    std::optional<std::string> sourceSet = stringField(root, KEY_SOURCE_SET);
    const JsonValue* inputs = root.find(KEY_INPUTS);
    const JsonValue* diagnostics = root.find(KEY_DIAGNOSTICS);
    if (!script.has_value() || !esbuildVersion.has_value() || !esbuildBinary.has_value() || !flags.has_value() ||
        !sourceSet.has_value() || inputs == nullptr || !inputs->isArray() || diagnostics == nullptr ||
        !diagnostics->isArray()) {
        return std::nullopt;
    }
    record.script = std::move(*script);
    record.esbuildVersion = std::move(*esbuildVersion);
    record.esbuildBinary = std::move(*esbuildBinary);
    record.flags = std::move(*flags);
    record.sourceSet = std::move(*sourceSet);

    for (const JsonValue& element : inputs->elements()) {
        std::optional<ScriptRecordInput> input = parseRecordInput(element);
        if (!input.has_value()) {
            return std::nullopt;
        }
        record.inputs.push_back(std::move(*input));
    }
    for (const JsonValue& element : diagnostics->elements()) {
        std::optional<ScriptDiagnostic> diagnostic = parseRecordDiagnostic(element);
        if (!diagnostic.has_value()) {
            return std::nullopt;
        }
        record.diagnostics.push_back(std::move(*diagnostic));
    }
    if (record.status == ScriptRecordStatus::Failed && record.diagnostics.empty()) {
        return std::nullopt;  // a failure with no line to show is a miss: the script runs again
    }
    return record;
}

std::string writeScriptRecordText(const ScriptRecord& record) {
    JsonWriter writer;  // the default config: pretty, 2 spaces
    writer.beginObject();
    writer.key(KEY_VERSION);
    writer.value(static_cast<long long>(SCRIPT_RECORD_FORMAT_VERSION));
    writer.key(KEY_STATUS);
    writer.value(record.status == ScriptRecordStatus::Ok ? std::string_view("ok") : std::string_view("failed"));
    writer.key(KEY_SCRIPT);
    writer.value(std::string_view(record.script));
    writer.key(KEY_ESBUILD_VERSION);
    writer.value(std::string_view(record.esbuildVersion));
    writer.key(KEY_ESBUILD_BINARY);
    writer.value(std::string_view(record.esbuildBinary));
    writer.key(KEY_FLAGS);
    writer.value(std::string_view(record.flags));
    writer.key(KEY_INPUTS);
    writer.beginArray();
    for (const ScriptRecordInput& input : record.inputs) {
        writer.beginObject();
        writer.key(KEY_PATH);
        writer.value(std::string_view(input.path));
        writer.key(KEY_HASH);
        if (input.hash.has_value()) {
            const std::string hashText = formatContentHash(*input.hash);  // a named local: it returns by value
            writer.value(std::string_view(hashText));
        } else {
            writer.valueNull();
        }
        writer.endObject();
    }
    writer.endArray();
    writer.key(KEY_SOURCE_SET);
    writer.value(std::string_view(record.sourceSet));
    writer.key(KEY_DIAGNOSTICS);
    writer.beginArray();
    for (const ScriptDiagnostic& diagnostic : record.diagnostics) {
        writer.beginObject();
        writer.key(KEY_SEVERITY);
        writer.value(diagnostic.severity == ScriptDiagnosticSeverity::Error ? std::string_view("error")
                                                                            : std::string_view("warning"));
        writer.key(KEY_FILE);
        writer.value(std::string_view(diagnostic.file));
        writer.key(KEY_LINE);
        writer.value(static_cast<unsigned long long>(diagnostic.line));
        writer.key(KEY_COLUMN);
        writer.value(static_cast<unsigned long long>(diagnostic.column));
        writer.key(KEY_TEXT);
        writer.value(std::string_view(diagnostic.text));
        writer.endObject();
    }
    writer.endArray();
    writer.endObject();
    std::string text = writer.str();
    text += '\n';
    return text;
}

bool scriptRecordFresh(const ScriptRecord& record, std::string_view scriptProjectRelative,
                       const ScriptFreshnessContext& context) {
    if (record.flags != context.flags) {
        return false;
    }
    // An unknown current binary compares nothing: a project whose scripts are all fresh opens without esbuild.
    if (!context.esbuildBinary.empty() && record.esbuildBinary != context.esbuildBinary) {
        return false;
    }
    if (record.status == ScriptRecordStatus::Failed && record.sourceSet != context.sourceSet) {
        return false;
    }
    if (!context.currentHash) {
        return false;
    }
    bool listsScript = false;
    for (const ScriptRecordInput& input : record.inputs) {
        listsScript = listsScript || input.path == scriptProjectRelative;
        if (!input.hash.has_value()) {
            return false;
        }
        const std::optional<ContentHash> current = context.currentHash(input.path);
        if (!current.has_value() || !(*current == *input.hash)) {
            return false;
        }
    }
    return listsScript;
}

std::string scriptSourceSetFingerprint(std::span<const AssetRecord> records) {
    std::string joined;
    for (const AssetRecord& record : records) {
        joined += record.relativePath;
        joined += '\n';
    }
    const std::string text = formatContentHash(hashOfText(joined));
    return text;
}

}  // namespace engine::editor

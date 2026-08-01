// Aero Engine — the project format's pure half (task 2.6.1). Parses, writes and validates
// project.json v1 and the recent-projects list. Uses engine::parseJson / engine::JsonValue /
// engine::JsonWriter (F3) -- the scene_format.cpp precedent, applied a second time. No
// <filesystem>, no SDL, no ImGui, no logging (INV-P6): status is RETURNED, never printed
// (project_files.hpp:15-16's convention). No recursion anywhere -- the document is two levels
// deep and both are walked with a flat loop.
#include <aero/editor/project.hpp>
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

constexpr std::string_view VERSION_KEY = "version";
constexpr std::string_view NAME_KEY = "name";
constexpr std::string_view ENGINE_VERSION_KEY = "engineVersion";
constexpr std::string_view LANGUAGE_KEY = "language";
constexpr std::string_view PATHS_KEY = "paths";
constexpr std::string_view ASSETS_KEY = "assets";
constexpr std::string_view SCENES_KEY = "scenes";

constexpr std::string_view LANGUAGE_TS_KEY = "ts";
constexpr std::string_view LANGUAGE_CPP_KEY = "cpp";

ProjectParseResult rejected(ProjectError error, std::string message, std::uint32_t line = 0, std::uint32_t column = 0) {
    ProjectParseResult result;
    result.error = error;
    result.message = std::move(message);
    result.line = line;
    result.column = column;
    return result;
}

// "(found <kind>)" -- or, when a Number failed a FORM rule rather than a KIND rule, the quoted
// lexeme instead: `(found "1.5")`. Copied from scene_format.cpp:43-49 (D11's catalog shape).
std::string foundDetail(const JsonValue& v) {
    if (v.isNumber()) {
        return std::format("\"{}\"", v.numberLexeme());
    }
    return std::string(jsonKindName(v.kind()));
}

// Checks one paths.<key> field. Missing, non-string, and lexically-illegal all report
// ProjectError::BadRelativePath (AC-4 lists them as one bucket) but with distinct message text.
// `dotted` is "paths.assets" / "paths.scenes" for the message. On success writes the value into
// `out` and returns nullopt.
std::optional<ProjectParseResult> checkPathField(const JsonValue& pathsObject, std::string_view key,
                                                 std::string_view dotted, std::string& out) {
    const JsonValue* field = pathsObject.find(key);
    if (field == nullptr) {
        return rejected(ProjectError::BadRelativePath, std::format("missing required key \"{}\"", dotted));
    }
    const std::optional<std::string_view> value = field->asString();
    if (!value.has_value()) {
        return rejected(ProjectError::BadRelativePath,
                        std::format("\"{}\" must be a string (found {})", dotted, jsonKindName(field->kind())));
    }
    if (!isLegalRelativePath(*value)) {
        return rejected(ProjectError::BadRelativePath,
                        std::format("\"{}\" must be a non-empty relative path with no leading '/', no '\\', no "
                                    "':' and no \"..\" segment (found \"{}\")",
                                    dotted, *value));
    }
    out = std::string(*value);
    return std::nullopt;
}

// ASCII-only, locale-independent. NEVER std::tolower(char): UTF-8 continuation bytes are NEGATIVE
// as char, which is UB and trips bugprone-signed-char-misuse (project_files.cpp:44-46's precedent).
constexpr unsigned char foldAscii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

// D6: not a reserved DOS device name, with or without an extension. Strips at the FIRST '.' and
// compares ASCII-case-insensitively -- "con", "Con.txt" and "LPT3.a.b" are all rejected.
constexpr std::array<std::string_view, 22> RESERVED_DEVICE_NAMES{{
    "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
    "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
}};

bool isReservedDeviceName(std::string_view name) noexcept {
    std::string_view stem = name;
    if (const std::size_t dot = name.find('.'); dot != std::string_view::npos) {
        // NOT substr(): specified to throw, which would escape this noexcept function
        // (bugprone-exception-escape). The pointer+size constructor is noexcept.
        stem = std::string_view(name.data(), dot);
    }
    for (const std::string_view reserved : RESERVED_DEVICE_NAMES) {
        if (stem.size() != reserved.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t i = 0; i < stem.size(); ++i) {
            if (foldAscii(static_cast<unsigned char>(stem[i])) != foldAscii(static_cast<unsigned char>(reserved[i]))) {
                same = false;
                break;
            }
        }
        if (same) {
            return true;
        }
    }
    return false;
}

constexpr bool isAsciiSpaceOrTab(char c) noexcept { return c == ' ' || c == '\t'; }

// ---- the recents envelope: {"version": 1, "projects": [...]} -------------------------------------
constexpr std::string_view RECENTS_PROJECTS_KEY = "projects";
constexpr int RECENTS_FORMAT_VERSION = 1;

}  // namespace

std::string_view languageKey(ProjectLanguage language) noexcept {
    switch (language) {
        case ProjectLanguage::Ts:
            return LANGUAGE_TS_KEY;
        case ProjectLanguage::Cpp:
            return LANGUAGE_CPP_KEY;
    }
    return LANGUAGE_TS_KEY;  // unreachable; every enumerator handled above
}

std::optional<ProjectLanguage> languageFromKey(std::string_view key) noexcept {
    if (key == LANGUAGE_TS_KEY) {
        return ProjectLanguage::Ts;
    }
    if (key == LANGUAGE_CPP_KEY) {
        return ProjectLanguage::Cpp;
    }
    return std::nullopt;
}

// parseProject's order is load-bearing: it is what makes AC-5's "version-first" property and every
// "exact first error" assertion true. Written in EXACTLY this order; do not reorder for style.
ProjectParseResult parseProject(std::string_view text) {
    const JsonParseResult parsed = parseJson(text);
    // NOT `!parsed.ok()`: bugprone-unchecked-optional-access cannot connect an opaque out-of-line
    // ok() to `value` (scene_format.cpp:317-319's precedent).
    if (!parsed.value.has_value()) {
        return rejected(ProjectError::BadJson, parsed.error.message, parsed.error.line, parsed.error.column);
    }
    const JsonValue& root = *parsed.value;
    if (!root.isObject()) {
        return rejected(ProjectError::NotAnObject,
                        std::format("project root must be a JSON object (found {})", jsonKindName(root.kind())));
    }

    // "version" FIRST (AC-5), before anything else -- a file also missing "name" must still report
    // the version error.
    const JsonValue* version = root.find(VERSION_KEY);
    if (version == nullptr) {
        return rejected(ProjectError::BadVersion, "missing required key \"version\"");
    }
    const std::optional<std::uint64_t> versionValue = version->asU64();
    if (!versionValue.has_value()) {
        return rejected(ProjectError::BadVersion,
                        std::format("\"version\" must be an integer (found {})", foundDetail(*version)));
    }
    if (*versionValue != static_cast<std::uint64_t>(PROJECT_FORMAT_VERSION)) {
        return rejected(ProjectError::UnsupportedVersion,
                        std::format("unsupported project format version {} (this build reads version {})",
                                    *versionValue, PROJECT_FORMAT_VERSION));
    }

    ProjectManifest manifest;

    // "name"
    const JsonValue* name = root.find(NAME_KEY);
    if (name == nullptr) {
        return rejected(ProjectError::BadName, "missing required key \"name\"");
    }
    const std::optional<std::string_view> nameValue = name->asString();
    if (!nameValue.has_value()) {
        return rejected(ProjectError::BadName,
                        std::format("\"name\" must be a string (found {})", jsonKindName(name->kind())));
    }
    if (nameValue->empty()) {
        return rejected(ProjectError::BadName, "\"name\" must not be empty");
    }
    manifest.name = std::string(*nameValue);

    // "engineVersion"
    const JsonValue* engineVersion = root.find(ENGINE_VERSION_KEY);
    if (engineVersion == nullptr) {
        return rejected(ProjectError::BadEngineVersion, "missing required key \"engineVersion\"");
    }
    const std::optional<std::string_view> engineVersionValue = engineVersion->asString();
    if (!engineVersionValue.has_value()) {
        return rejected(ProjectError::BadEngineVersion, std::format("\"engineVersion\" must be a string (found {})",
                                                                    jsonKindName(engineVersion->kind())));
    }
    manifest.engineVersion = std::string(*engineVersionValue);

    // "language"
    const JsonValue* language = root.find(LANGUAGE_KEY);
    if (language == nullptr) {
        return rejected(ProjectError::BadLanguage, "missing required key \"language\"");
    }
    const std::optional<std::string_view> languageValue = language->asString();
    if (!languageValue.has_value()) {
        return rejected(ProjectError::BadLanguage,
                        std::format("\"language\" must be a string (found {})", jsonKindName(language->kind())));
    }
    const std::optional<ProjectLanguage> languageParsed = languageFromKey(*languageValue);
    if (!languageParsed.has_value()) {
        return rejected(ProjectError::BadLanguage,
                        std::format(R"(unsupported language "{}" (expected "ts" or "cpp"))", *languageValue));
    }
    manifest.language = *languageParsed;

    // "paths"
    const JsonValue* paths = root.find(PATHS_KEY);
    if (paths == nullptr) {
        return rejected(ProjectError::BadPaths, "missing required key \"paths\"");
    }
    if (!paths->isObject()) {
        return rejected(ProjectError::BadPaths,
                        std::format("\"paths\" must be an object (found {})", jsonKindName(paths->kind())));
    }
    if (std::optional<ProjectParseResult> failure =
            checkPathField(*paths, ASSETS_KEY, "paths.assets", manifest.assetsPath)) {
        return std::move(*failure);
    }
    if (std::optional<ProjectParseResult> failure =
            checkPathField(*paths, SCENES_KEY, "paths.scenes", manifest.scenesPath)) {
        return std::move(*failure);
    }

    // Unknown keys, in true DOCUMENT ORDER (AC-6/S18) -- never an error. ONE depth-first walk over the
    // root's own members: an unrecognized root key is collected at its own position, and "paths"
    // (a known key) additionally walks ITS members in place, so a "paths"-nested unknown key is
    // collected at the position "paths" itself occupies among the root members -- not after every
    // root-level unknown, regardless of where "paths" sits. This is what makes PG7's expectation
    // {"prefabs", "author", "editorLayout"} (from unknown-keys.project.json, where "paths" precedes
    // "author"/"editorLayout") the correct order, not merely a coincidence of two separate sweeps.
    ProjectParseResult result;
    for (const JsonMember& member : root.members()) {
        if (member.key == PATHS_KEY) {
            for (const JsonMember& pathsMember : paths->members()) {
                if (pathsMember.key != ASSETS_KEY && pathsMember.key != SCENES_KEY) {
                    result.unknownKeys.push_back(pathsMember.key);
                }
            }
        } else if (member.key != VERSION_KEY && member.key != NAME_KEY && member.key != ENGINE_VERSION_KEY &&
                   member.key != LANGUAGE_KEY) {
            result.unknownKeys.push_back(member.key);
        }
    }

    result.manifest = std::move(manifest);
    return result;
}

std::string writeProjectText(const ProjectManifest& manifest) {
    JsonWriter writer;  // the DEFAULT config: pretty, 2-space -- docs/09 §1's canonical form, and the
                        // same object scene_format.cpp:403 uses. Do NOT spell the config out; a second
                        // spelling is a second truth.
    writer.beginObject();
    writer.key("version");
    writer.value(static_cast<long long>(PROJECT_FORMAT_VERSION));
    writer.key("name");
    writer.value(std::string_view(manifest.name));
    writer.key("engineVersion");
    writer.value(std::string_view(manifest.engineVersion));
    writer.key("language");
    writer.value(languageKey(manifest.language));
    writer.key("paths");
    writer.beginObject();
    writer.key("assets");
    writer.value(std::string_view(manifest.assetsPath));
    writer.key("scenes");
    writer.value(std::string_view(manifest.scenesPath));
    writer.endObject();
    writer.endObject();
    std::string text = writer.str();
    text += '\n';  // exactly ONE trailing newline (the writer itself has none, F2; parseJson accepts it)
    return text;
}

// D6's rules, checked in the order of the NameProblem enum so the FIRST problem reported is stable
// and assertable.
NameProblem validateProjectName(std::string_view utf8) noexcept {
    // 1. Empty, AFTER trimming ASCII spaces/tabs from both ends.
    std::size_t begin = 0;
    while (begin < utf8.size() && isAsciiSpaceOrTab(utf8[begin])) {
        ++begin;
    }
    std::size_t end = utf8.size();
    while (end > begin && isAsciiSpaceOrTab(utf8[end - 1U])) {
        --end;
    }
    if (begin == end) {
        return NameProblem::Empty;
    }

    // 2. TooLong -- UTF-8 BYTES of the ORIGINAL (untrimmed) name, not code points.
    if (utf8.size() > MAX_PROJECT_NAME_BYTES) {
        return NameProblem::TooLong;
    }

    // 3. Separator: '/', '\', NUL anywhere.
    for (const char c : utf8) {
        if (c == '/' || c == '\\' || c == '\0') {
            return NameProblem::Separator;
        }
    }

    // 4. DotName: the WHOLE name is "." or "..".
    if (utf8 == "." || utf8 == "..") {
        return NameProblem::DotName;
    }

    // 5. IllegalChar: < > : " | ? * or any byte < 0x20.
    for (const char raw : utf8) {
        const auto c = static_cast<unsigned char>(raw);
        if (c < 0x20U || raw == '<' || raw == '>' || raw == ':' || raw == '"' || raw == '|' || raw == '?' ||
            raw == '*') {
            return NameProblem::IllegalChar;
        }
    }

    // 6. TrailingSpaceOrDot -- on the ORIGINAL (untrimmed) name.
    if (utf8.back() == ' ' || utf8.back() == '.') {
        return NameProblem::TrailingSpaceOrDot;
    }

    // 7. ReservedDeviceName -- ASCII-only folding (every reserved name is ASCII; locale-aware
    // folding would be both wrong and non-deterministic across the three CI lanes).
    if (isReservedDeviceName(utf8)) {
        return NameProblem::ReservedDeviceName;
    }

    return NameProblem::Ok;
}

std::string_view nameProblemMessage(NameProblem problem) noexcept {
    switch (problem) {
        case NameProblem::Ok:
            return "the name is valid";
        case NameProblem::Empty:
            return "the project name cannot be empty";
        case NameProblem::TooLong:
            return "the project name must be at most 64 bytes";
        case NameProblem::Separator:
            return "the project name cannot contain '/' or '\\'";
        case NameProblem::DotName:
            return R"(the project name cannot be "." or "..")";
        case NameProblem::IllegalChar:
            return "the project name cannot contain any of < > : \" | ? * or a control character";
        case NameProblem::TrailingSpaceOrDot:
            return "the project name cannot end with a space or a '.'";
        case NameProblem::ReservedDeviceName:
            return "the project name cannot be a reserved device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9)";
    }
    return "the name is valid";  // unreachable; every enumerator handled above
}

bool isLegalRelativePath(std::string_view relative) noexcept {
    if (relative.empty() || relative.front() == '/') {
        return false;
    }
    for (const char c : relative) {
        if (c == '\\' || c == ':') {
            return false;
        }
    }
    // No ".." segment. Walked with pointer+size views -- never substr() -- so this stays noexcept.
    std::size_t start = 0;
    while (start <= relative.size()) {
        const std::size_t slash = relative.find('/', start);
        const std::size_t segmentEnd = (slash == std::string_view::npos) ? relative.size() : slash;
        const std::string_view segment(relative.data() + start, segmentEnd - start);
        if (segment == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1U;
    }
    return true;
}

// ---- ProjectSession ---------------------------------------------------------------------------

bool ProjectSession::isOpen() const noexcept { return !rootPath.empty(); }
std::string_view ProjectSession::root() const noexcept { return rootPath; }
const ProjectManifest& ProjectSession::manifest() const noexcept { return projectManifest; }
std::string_view ProjectSession::name() const noexcept {
    return isOpen() ? std::string_view(projectManifest.name) : std::string_view{};
}

namespace {
// E6: "assets/" (a trailing separator) is a LEGAL paths.assets value (isLegalRelativePath does not
// reject it) and is normalized away HERE, at the join, rather than at parse time -- so a hand-edited
// file's stored bytes are never silently rewritten by merely OPENING it. isLegalRelativePath already
// forbids a leading '/', so a legally-parsed value can never be all-separators.
std::string_view stripTrailingSeparators(std::string_view relative) noexcept {
    while (!relative.empty() && (relative.back() == '/' || relative.back() == '\\')) {
        relative.remove_suffix(1U);
    }
    return relative;
}
}  // namespace

std::string ProjectSession::assetsRoot() const {
    if (!isOpen()) {
        return {};
    }
    std::string result = rootPath;
    result += '/';
    result += stripTrailingSeparators(projectManifest.assetsPath);
    return result;
}

std::string ProjectSession::scenesRoot() const {
    if (!isOpen()) {
        return {};
    }
    std::string result = rootPath;
    result += '/';
    result += stripTrailingSeparators(projectManifest.scenesPath);
    return result;
}

std::string ProjectSession::manifestPath() const {
    if (!isOpen()) {
        return {};
    }
    std::string result = rootPath;
    result += '/';
    result += std::string(PROJECT_FILE_NAME);
    return result;
}

void ProjectSession::set(ProjectManifest manifest, std::string absoluteRootUtf8) {
    projectManifest = std::move(manifest);
    rootPath = std::move(absoluteRootUtf8);
}

void ProjectSession::close() noexcept {
    rootPath.clear();
    projectManifest = ProjectManifest{};
}

// ---- recents -------------------------------------------------------------------------------------

void promoteRecent(RecentProjects& recents, std::string absoluteRootUtf8) {
    if (absoluteRootUtf8.empty()) {
        return;  // a no-op: nothing to promote
    }
    // Strip a trailing '/' or '\' -- every path reaching here is already normalized by
    // projectRootFromPath upstream, so this covers a hand-edited recents entry only. Guarded by
    // size() > 1 so "/" alone never reduces to "".
    while (absoluteRootUtf8.size() > 1U && (absoluteRootUtf8.back() == '/' || absoluteRootUtf8.back() == '\\')) {
        absoluteRootUtf8.pop_back();
    }
    const auto stripped = [](std::string_view path) noexcept {
        while (path.size() > 1U && (path.back() == '/' || path.back() == '\\')) {
            path.remove_suffix(1U);
        }
        return path;
    };
    const auto existing = std::find_if(recents.paths.begin(), recents.paths.end(),
                                       [&](const std::string& entry) { return stripped(entry) == absoluteRootUtf8; });
    if (existing != recents.paths.end()) {
        recents.paths.erase(existing);
    }
    recents.paths.insert(recents.paths.begin(), std::move(absoluteRootUtf8));
    if (recents.paths.size() > MAX_RECENT_PROJECTS) {
        recents.paths.resize(MAX_RECENT_PROJECTS);
    }
}

RecentProjects parseRecentProjects(std::string_view text, bool& warn) {
    warn = false;
    RecentProjects result;
    if (text.empty()) {
        // Not reachable via readRecentProjects (the missing-file case short-circuits earlier), but
        // parseRecentProjects itself treats "" as "nothing to load", not corruption.
        return result;
    }
    const JsonParseResult parsed = parseJson(text);
    if (!parsed.value.has_value()) {
        warn = true;
        return result;
    }
    const JsonValue& root = *parsed.value;
    if (!root.isObject()) {
        warn = true;
        return result;
    }
    const JsonValue* version = root.find(VERSION_KEY);
    if (version == nullptr) {
        warn = true;
        return result;
    }
    const std::optional<std::uint64_t> versionValue = version->asU64();
    if (!versionValue.has_value() || *versionValue != static_cast<std::uint64_t>(RECENTS_FORMAT_VERSION)) {
        warn = true;
        return result;
    }
    const JsonValue* projects = root.find(RECENTS_PROJECTS_KEY);
    if (projects == nullptr || !projects->isArray()) {
        warn = true;
        return result;
    }
    result.paths.reserve(projects->elements().size());
    for (const JsonValue& element : projects->elements()) {
        const std::optional<std::string_view> value = element.asString();
        if (!value.has_value()) {
            warn = true;  // a corrupt ROW must not cost the whole list
            continue;
        }
        result.paths.emplace_back(*value);
    }
    return result;
}

std::string writeRecentProjectsText(const RecentProjects& recents) {
    JsonWriter writer;  // the DEFAULT config: pretty, 2-space -- the same canonical form (F2)
    writer.beginObject();
    writer.key("version");
    writer.value(static_cast<long long>(RECENTS_FORMAT_VERSION));
    writer.key(RECENTS_PROJECTS_KEY);
    writer.beginArray();
    for (const std::string& path : recents.paths) {
        writer.value(std::string_view(path));
    }
    writer.endArray();
    writer.endObject();
    std::string text = writer.str();
    text += '\n';
    return text;
}

}  // namespace engine::editor

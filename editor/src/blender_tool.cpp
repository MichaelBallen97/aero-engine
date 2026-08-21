// Aero Engine — blender_tool.cpp: the PURE half of the Blender CLI integration (task 3.2.4).
// NOTHING HERE LOGS (INV-B10), NOTHING HERE TOUCHES DISK (INV-B14), NOTHING HERE THROWS, and nothing
// here spawns a process (INV-B15 -- every spawn in this task lives in BlenderService::poll()).
// <array> is included EXPLICITLY: modernize-avoid-c-arrays is live on the Linux lane and <array> is
// not transitive on libstdc++ or MSVC (3.1.1's BLOCKING-1).
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/asset_cache.hpp>  // ASSET_CACHE_DIR_NAME -- the ONE spelling of "Library" (3.1.2)
#include <aero/editor/asset_meta.hpp>   // writeMetaText -- blendExportSettingsFingerprint's serializer
#include <aero/editor/blender_tool.hpp>
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

HostOs currentHostOs() noexcept {
#if defined(_WIN32)
    return HostOs::Windows;
#elif defined(__APPLE__)
    return HostOs::MacOs;
#elif defined(__linux__)
    return HostOs::Linux;
#else
    #error "The Aero editor targets macOS, Windows and Linux only (docs/00's platform matrix)."
#endif
}

namespace {

// ASCII-only, locale-independent. NEVER std::tolower(char): UTF-8 continuation bytes are NEGATIVE as
// char, which is UB and trips bugprone-signed-char-misuse. This TU keeps its OWN copy rather than
// sharing one, matching model_import.cpp's own precedent (which keeps a copy for the same reason).
constexpr unsigned char foldAscii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

// The isMetaFileName / isImportableModelName shape: ".blend" ALONE is not a blend file, something
// must precede the extension.
[[nodiscard]] bool endsWithFolded(std::string_view name, std::string_view ext) noexcept {
    if (name.size() <= ext.size()) {
        return false;
    }
    const std::size_t offset = name.size() - ext.size();
    for (std::size_t i = 0; i < ext.size(); ++i) {
        const unsigned char lhs = foldAscii(static_cast<unsigned char>(name[offset + i]));
        const unsigned char rhs = foldAscii(static_cast<unsigned char>(ext[i]));
        if (lhs != rhs) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr std::string_view blenderExeName(HostOs os) noexcept {
    return os == HostOs::Windows ? std::string_view("blender.exe") : std::string_view("blender");
}

[[nodiscard]] bool isBlankAscii(std::string_view text) noexcept {
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n' && byte != '\v' && byte != '\f') {
            return false;
        }
    }
    return true;
}

// Dedup state: a SORTED std::vector, never a std::set or std::unordered_set (INV-B13). It is a local
// here rather than a member, but the rule is stated once and applied everywhere in this task.
class CandidateSink {
public:
    void add(std::string candidate) {
        if (candidate.empty()) {
            return;
        }
        const auto slot = std::lower_bound(seen.begin(), seen.end(), candidate);
        if (slot != seen.end() && *slot == candidate) {
            return;  // already emitted -- FIRST-SEEN order is what `ordered` preserves
        }
        seen.insert(slot, candidate);
        ordered.push_back(std::move(candidate));
    }
    [[nodiscard]] std::vector<std::string> take() noexcept { return std::move(ordered); }

private:
    std::vector<std::string> seen;     // sorted, for the membership test
    std::vector<std::string> ordered;  // emission order, which is what the caller gets
};

// Windows release lines, DESCENDING -- newest first, so a machine with several installs probes the
// newest one first. Data, not logic (see the header's note): a missing line degrades to "not found
// via this route", and PATH / AERO_BLENDER_PATH / Locate... all bypass it.
constexpr std::array<std::string_view, 13> WINDOWS_RELEASE_LINES = {"5.2", "5.1", "5.0", "4.5", "4.4", "4.3", "4.2",
                                                                    "4.1", "4.0", "3.6", "3.5", "3.4", "3.3"};

constexpr std::array<std::string_view, 2> MACOS_ABSOLUTE_APPS = {
    "/Applications/Blender.app/Contents/MacOS/Blender",
    "/Applications/Blender/Blender.app/Contents/MacOS/Blender",
};

constexpr std::array<std::string_view, 5> LINUX_ABSOLUTE_PATHS = {
    "/usr/bin/blender",     "/usr/local/bin/blender",
    "/snap/bin/blender",    "/var/lib/flatpak/exports/bin/org.blender.Blender",
    "/opt/blender/blender",
};

void appendWindowsWellKnown(const BlenderEnv& env, CandidateSink& sink) {
    // An empty prefix SKIPS the whole family rather than emitting "\Blender Foundation\..." with a
    // missing root -- E2's rule, applied to %PROGRAMFILES% / %LOCALAPPDATA% as well as to $HOME.
    if (!env.programFiles.empty()) {
        for (const std::string_view line : WINDOWS_RELEASE_LINES) {
            sink.add(env.programFiles + "\\Blender Foundation\\Blender " + std::string(line) + "\\blender.exe");
        }
    }
    if (!env.localAppData.empty()) {
        for (const std::string_view line : WINDOWS_RELEASE_LINES) {
            sink.add(env.localAppData + R"(\Programs\Blender Foundation\Blender )" + std::string(line) +
                     "\\blender.exe");
        }
    }
    if (!env.programFiles.empty()) {
        sink.add(env.programFiles + R"(\Steam\steamapps\common\Blender\blender.exe)");
    }
}

void appendMacOsWellKnown(const BlenderEnv& env, CandidateSink& sink) {
    sink.add(std::string(MACOS_ABSOLUTE_APPS[0]));
    if (!env.homeDir.empty()) {
        sink.add(env.homeDir + "/Applications/Blender.app/Contents/MacOS/Blender");
    }
    sink.add(std::string(MACOS_ABSOLUTE_APPS[1]));
}

void appendLinuxWellKnown(const BlenderEnv& env, CandidateSink& sink) {
    // The order is the header's: the three absolute bin paths, the system flatpak export, the per-user
    // flatpak export (skipped entirely when $HOME is unknown), then /opt.
    sink.add(std::string(LINUX_ABSOLUTE_PATHS[0]));
    sink.add(std::string(LINUX_ABSOLUTE_PATHS[1]));
    sink.add(std::string(LINUX_ABSOLUTE_PATHS[2]));
    sink.add(std::string(LINUX_ABSOLUTE_PATHS[3]));
    if (!env.homeDir.empty()) {
        sink.add(env.homeDir + "/.local/share/flatpak/exports/bin/org.blender.Blender");
    }
    sink.add(std::string(LINUX_ABSOLUTE_PATHS[4]));
}

// Digit-by-digit, with an overflow guard. `cursor` is advanced past the digits consumed. Returns
// nullopt when there is no digit at `cursor`, or when the value does not fit in 32 bits.
[[nodiscard]] std::optional<std::uint32_t> parseU32(std::string_view text, std::size_t& cursor) noexcept {
    constexpr std::uint64_t LIMIT = 0xFFFFFFFFULL;
    std::uint64_t value = 0;
    const std::size_t start = cursor;
    while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
        value = (value * 10U) + static_cast<std::uint64_t>(text[cursor] - '0');
        if (value > LIMIT) {
            return std::nullopt;  // a number that does not fit is not a version this parser understands
        }
        ++cursor;
    }
    if (cursor == start) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

}  // namespace

std::vector<std::string> blenderCandidatePaths(HostOs os, const BlenderEnv& env) {
    CandidateSink sink;

    // 1 and 2 are EXCLUSIVE and terminal (AC-3): a configured path that is wrong must report itself,
    // never fall through to a different Blender's output.
    if (!env.overridePath.empty()) {
        sink.add(env.overridePath);
        return sink.take();
    }
    if (!env.envPath.empty()) {
        sink.add(env.envPath);
        return sink.take();
    }

    const std::string exeName(blenderExeName(os));
    std::size_t scanned = 0;
    for (const std::string& entry : env.pathEntries) {
        if (scanned >= MAX_PATH_ENTRIES_SCANNED) {
            break;  // R3 -- a pathological PATH cannot turn one lazy sweep into thousands of stats
        }
        ++scanned;
        if (entry.empty() || isBlankAscii(entry)) {
            continue;  // E2 -- an empty PATH entry must NEVER be joined into "/blender"
        }
        std::string candidate = entry;  // built with append, not a `+` chain: the Linux lane's
        candidate += '/';               // performance-inefficient-string-concatenation is an error
        candidate += exeName;
        sink.add(std::move(candidate));
    }

    switch (os) {
        case HostOs::Windows:
            appendWindowsWellKnown(env, sink);
            break;
        case HostOs::MacOs:
            appendMacOsWellKnown(env, sink);
            break;
        case HostOs::Linux:
            appendLinuxWellKnown(env, sink);
            break;
    }
    return sink.take();
}

std::optional<BlenderVersion> parseBlenderVersion(std::string_view versionOutput) {
    // FIRST LINE ONLY: the tab-indented "build date:"/"build hash:" continuation lines a real
    // `blender --version` prints must never be reached (§G-4's verbatim capture).
    const std::size_t lineEnd = versionOutput.find('\n');
    std::string_view line = versionOutput.substr(0, lineEnd == std::string_view::npos ? versionOutput.size() : lineEnd);
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }

    // The prefix is LITERAL and case-sensitive, and it is at position 0: no leading whitespace is
    // accepted, because a line that does not begin with Blender's own banner is not that banner.
    constexpr std::string_view PREFIX = "Blender ";
    if (line.size() <= PREFIX.size() || line.substr(0, PREFIX.size()) != PREFIX) {
        return std::nullopt;
    }
    std::size_t cursor = PREFIX.size();
    if (line[cursor] < '0' || line[cursor] > '9') {
        return std::nullopt;
    }

    BlenderVersion parsed;
    const std::optional<std::uint32_t> major = parseU32(line, cursor);
    if (!major.has_value()) {
        return std::nullopt;
    }
    parsed.major = *major;
    if (cursor < line.size() && line[cursor] == '.') {
        ++cursor;
        const std::optional<std::uint32_t> minor = parseU32(line, cursor);
        if (!minor.has_value()) {
            return std::nullopt;
        }
        parsed.minor = *minor;
        if (cursor < line.size() && line[cursor] == '.') {
            ++cursor;
            const std::optional<std::uint32_t> patch = parseU32(line, cursor);
            if (!patch.has_value()) {
                return std::nullopt;
            }
            parsed.patch = *patch;
        }
    }
    // Everything after the last parsed number is IGNORED: " LTS" on 5.2, a build suffix elsewhere.
    return parsed;
}

BlenderSupport blenderSupport(const std::optional<BlenderVersion>& version) noexcept {
    if (!version.has_value()) {
        return BlenderSupport::Supported;  // D14/E4 -- attempt, never refuse
    }
    if (*version < BLENDER_ABSOLUTE_MIN) {
        return BlenderSupport::Refused;
    }
    if (*version < BLENDER_MIN_SUPPORTED) {
        return BlenderSupport::Warned;
    }
    return BlenderSupport::Supported;
}

bool isBlendFileName(std::string_view fileName) noexcept { return endsWithFolded(fileName, ".blend"); }

std::string blenderExportDir(std::string_view projectRootUtf8) {
    if (projectRootUtf8.empty()) {
        return {};  // NO PROJECT: there is nowhere to derive anything yet (code-review NOTE 6)
    }
    std::string dir(projectRootUtf8);
    dir += '/';
    dir += ASSET_CACHE_DIR_NAME;
    dir += '/';
    dir += BLENDER_EXPORT_DIR_NAME;
    return dir;
}

std::vector<std::string> buildVersionArgs(std::string_view binary) { return {std::string(binary), "--version"}; }

std::vector<std::string> buildExportArgs(std::string_view binary, std::string_view blendAbs, std::string_view scriptAbs,
                                         std::string_view outAbs, std::string_view statusAbs) {
    // Fifteen entries, and the ORDER is the contract (AC-11). Every path goes in RAW: no quoting, no
    // escaping, no substitution. MEASURED end to end against a real Blender with a path containing a
    // space and a non-ASCII character (§G-4, E20).
    return {
        std::string(binary),
        "-b",  // headless
        std::string(blendAbs),
        "-X",        // --factory-startup: no user add-ons, no user preferences. The bundled glTF
                     // exporter is factory-ENABLED, so no --addons flag is needed (VERIFIED on 5.2).
        "-Y",        // --disable-autoexec: never run a .blend's own embedded Python
        "-noaudio",  // a headless run has no audio device to open
        "--python-exit-code",
        "42",  // an UNCAUGHT script exception exits 42, which is how "the script died" is told apart
               // from "Blender itself refused the file" (which exits 1 and never runs the script)
        "--python",
        std::string(scriptAbs),
        "--",  // Blender's own "stop parsing; the rest is the script's" separator
        "--out",
        std::string(outAbs),
        "--status",
        std::string(statusAbs),
    };
}

namespace {

// The script text, VERBATIM as executed against Blender 5.2.0 LTS this session (§G-4). Do not
// "improve" it:
//   * every kwarg in `wanted` was verified present in that build's 111-property RNA list, and the RNA
//     FILTER below is what lets ONE script span the whole supported version range -- passing a keyword
//     the installed Blender does not know raises TypeError and fails the whole export, so unknown keys
//     are DROPPED and REPORTED instead. There is no version branching in Python and no version table
//     in C++, and this is why.
//   * `export_yup` is deliberately ABSENT (F6). Blender's world is Z-up and glTF is Y-up; the
//     exporter's own export_yup property (default enabled) performs that conversion INSIDE Blender, so
//     what lands in the GLB is specification-conformant glTF. Setting it False would be the mistake and
//     would break 3.2.1's "the importer converts NOTHING" rule. This task therefore contains no
//     coordinate mathematics of any kind.
//   * `--factory-startup` is on the COMMAND LINE, never a line in here.
//   * the try/except writes the status file with a traceback rather than propagating, which is what
//     makes the "exit 0, ok: false" row of the failure table reachable at all.
// NO INTERPOLATION SITE OF ANY KIND (AC-13): no '%', no "{}" pair, no ".format(" call, no f-string
// prefix. `traceback.format_exc()` and "export_format" merely CONTAIN those letters -- neither is a
// substitution point, which is what BT39 asserts and what the plan's own shorthand ("no format")
// overstates.
constexpr std::string_view EXPORT_SCRIPT_TEXT =
    R"PY(# Aero Engine -- Blender -> glTF export (task 3.2.4). Parameters arrive via sys.argv after "--".
import bpy, sys, json, os, traceback

def arg(name, argv):
    return argv[argv.index(name) + 1]

argv   = sys.argv[sys.argv.index("--") + 1:]
out    = arg("--out", argv)
status = arg("--status", argv)
report = {"ok": False, "blender": bpy.app.version_string, "error": "", "dropped": [], "bytes": 0}
try:
    wanted = {
        "filepath": out,
        "export_format": "GLB",
        "export_apply": True,          # evaluate modifiers -- what the user sees is what exports
        "export_animations": True,
        "export_skins": True,
        "export_materials": "EXPORT",
        "export_cameras": False,       # ImportedModel has no camera or light concept
        "export_lights": False,
        "use_selection": False,        # a background run has no selection; be explicit
        "use_visible": False,          # hidden objects export too -- a headless run has no viewport
    }
    known = {p.identifier for p in bpy.ops.export_scene.gltf.get_rna_type().properties}
    report["dropped"] = sorted(k for k in wanted if k not in known)
    result = bpy.ops.export_scene.gltf(**{k: v for k, v in wanted.items() if k in known})
    report["result"] = list(result)
    report["ok"]     = os.path.exists(out) and os.path.getsize(out) > 0
    report["bytes"]  = os.path.getsize(out) if os.path.exists(out) else 0
    if not report["ok"]:
        report["error"] = "the exporter reported " + str(list(result)) + " but wrote no usable file"
except Exception:
    report["error"] = traceback.format_exc()
with open(status, "w", encoding="utf-8") as f:
    json.dump(report, f)
)PY";

}  // namespace

std::string_view blenderExportScriptText() noexcept { return EXPORT_SCRIPT_TEXT; }

// ---- the three machine-local formats ------------------------------------------------------------

namespace {

constexpr std::string_view VERSION_KEY = "version";
constexpr std::string_view BLENDER_PATH_KEY = "blenderPath";
constexpr std::string_view GUID_KEY = "guid";
constexpr std::string_view SOURCE_PATH_KEY = "sourcePath";
constexpr std::string_view SOURCE_HASH_KEY = "sourceHash";
constexpr std::string_view BLENDER_VERSION_KEY = "blenderVersion";
constexpr std::string_view SCRIPT_VERSION_KEY = "scriptVersion";
constexpr std::string_view SETTINGS_FINGERPRINT_KEY = "settingsFingerprint";
constexpr std::string_view ARTIFACT_BYTES_KEY = "artifactBytes";
constexpr std::string_view OK_KEY = "ok";
constexpr std::string_view BLENDER_KEY = "blender";
constexpr std::string_view ERROR_KEY = "error";
constexpr std::string_view DROPPED_KEY = "dropped";
constexpr std::string_view BYTES_KEY = "bytes";

// Every one of these three documents is DISPOSABLE (3.1.2 D7): a structural failure discards the
// WHOLE document rather than repairing a key, because the cost of being wrong is one re-detection or
// one re-conversion. That is why every helper below returns nullopt rather than a partial value.
[[nodiscard]] const JsonValue* objectRoot(const JsonParseResult& parsed) noexcept {
    if (!parsed.value.has_value()) {
        // NOT `!parsed.ok()`: bugprone-unchecked-optional-access cannot connect an opaque out-of-line
        // ok() to `value` (project.cpp's precedent, asset_cache.cpp's second application).
        return nullptr;
    }
    const JsonValue& root = *parsed.value;
    return root.isObject() ? &root : nullptr;
}

[[nodiscard]] bool versionEquals(const JsonValue& root, int expected) noexcept {
    const JsonValue* version = root.find(VERSION_KEY);
    if (version == nullptr) {
        return false;
    }
    const std::optional<std::uint64_t> value = version->asU64();
    return value.has_value() && *value == static_cast<std::uint64_t>(expected);
}

// A REQUIRED string: absent or non-string is a miss for the whole document.
[[nodiscard]] std::optional<std::string> requiredString(const JsonValue& root, std::string_view key) {
    const JsonValue* field = root.find(key);
    if (field == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::string_view> text = field->asString();
    if (!text.has_value()) {
        return std::nullopt;
    }
    return std::string(*text);
}

// An OPTIONAL string: absent is an empty value; PRESENT-BUT-NOT-A-STRING is still a miss, because a
// document whose shape disagrees with this one is not a document this build wrote.
[[nodiscard]] bool optionalString(const JsonValue& root, std::string_view key, std::string& out) {
    const JsonValue* field = root.find(key);
    if (field == nullptr) {
        return true;  // absent -> the default, and that is not a failure
    }
    const std::optional<std::string_view> text = field->asString();
    if (!text.has_value()) {
        return false;
    }
    out = std::string(*text);
    return true;
}

}  // namespace

std::optional<ToolPrefs> parseToolPrefs(std::string_view text) {
    const JsonParseResult parsed = parseJson(text);
    const JsonValue* root = objectRoot(parsed);
    if (root == nullptr || !versionEquals(*root, TOOL_PREFS_FORMAT_VERSION)) {
        return std::nullopt;
    }
    ToolPrefs prefs;
    // "blenderPath" is OPTIONAL: an absent key is an unset preference, which is the whole point of a
    // file that exists to say "the user has not chosen one yet". A non-string is a miss.
    if (!optionalString(*root, BLENDER_PATH_KEY, prefs.blenderPath)) {
        return std::nullopt;
    }
    return prefs;
}

std::string writeToolPrefsText(const ToolPrefs& prefs) {
    JsonWriter writer;  // the DEFAULT config: pretty, 2-space -- docs/09's canonical form. Do NOT
                        // spell the config out; a second spelling is a second truth.
    writer.beginObject();
    writer.key(VERSION_KEY);
    writer.value(static_cast<long long>(TOOL_PREFS_FORMAT_VERSION));
    writer.key(BLENDER_PATH_KEY);
    writer.value(std::string_view(prefs.blenderPath));
    writer.endObject();
    std::string text = writer.str();
    text += '\n';  // exactly ONE trailing newline (the writer itself has none; parseJson accepts it)
    return text;
}

std::optional<ExportProvenance> parseExportProvenance(std::string_view text) {
    const JsonParseResult parsed = parseJson(text);
    const JsonValue* root = objectRoot(parsed);
    if (root == nullptr || !versionEquals(*root, EXPORT_PROVENANCE_FORMAT_VERSION)) {
        return std::nullopt;
    }

    ExportProvenance record;
    // Every key below is REQUIRED. Unlike the tool preferences, a provenance record with a missing
    // field cannot be evaluated at all -- and a miss is exactly the right answer, because it costs one
    // re-conversion and never a wrong model.
    const std::optional<std::string> guidText = requiredString(*root, GUID_KEY);
    const std::optional<std::string> sourceHashText = requiredString(*root, SOURCE_HASH_KEY);
    const std::optional<std::string> sourcePath = requiredString(*root, SOURCE_PATH_KEY);
    const std::optional<std::string> blenderPath = requiredString(*root, BLENDER_PATH_KEY);
    const std::optional<std::string> blenderVersion = requiredString(*root, BLENDER_VERSION_KEY);
    const std::optional<std::string> fingerprint = requiredString(*root, SETTINGS_FINGERPRINT_KEY);
    if (!guidText.has_value() || !sourceHashText.has_value() || !sourcePath.has_value() || !blenderPath.has_value() ||
        !blenderVersion.has_value() || !fingerprint.has_value()) {
        return std::nullopt;
    }
    const std::optional<Guid> guid = parseGuid(*guidText);
    const std::optional<ContentHash> sourceHash = parseContentHash(*sourceHashText);
    if (!guid.has_value() || !sourceHash.has_value()) {
        return std::nullopt;
    }

    const JsonValue* scriptVersion = root->find(SCRIPT_VERSION_KEY);
    const JsonValue* artifactBytes = root->find(ARTIFACT_BYTES_KEY);
    if (scriptVersion == nullptr || artifactBytes == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> scriptVersionValue = scriptVersion->asU64();
    const std::optional<std::uint64_t> artifactBytesValue = artifactBytes->asU64();
    if (!scriptVersionValue.has_value() || !artifactBytesValue.has_value()) {
        return std::nullopt;
    }
    if (*scriptVersionValue > 0xFFFFFFFFULL) {
        return std::nullopt;  // a hand-edited or corrupt value; never a silent truncation
    }

    record.guid = *guid;
    record.sourceHash = *sourceHash;
    record.sourcePath = *sourcePath;
    record.blenderPath = *blenderPath;
    record.blenderVersion = *blenderVersion;
    record.settingsFingerprint = *fingerprint;
    record.scriptVersion = static_cast<std::uint32_t>(*scriptVersionValue);
    record.artifactBytes = *artifactBytesValue;
    return record;
}

std::string writeExportProvenanceText(const ExportProvenance& record) {
    JsonWriter writer;
    writer.beginObject();
    writer.key(VERSION_KEY);
    writer.value(static_cast<long long>(EXPORT_PROVENANCE_FORMAT_VERSION));
    // Named local FIRST, always (the 2.6.1 FileDialogHost::projectRoot lesson, asset_cache.cpp's
    // precedent): formatGuid/formatContentHash return BY VALUE.
    writer.key(GUID_KEY);
    const std::string guidText = formatGuid(record.guid);
    writer.value(std::string_view(guidText));
    writer.key(SOURCE_PATH_KEY);
    writer.value(std::string_view(record.sourcePath));
    writer.key(SOURCE_HASH_KEY);
    const std::string sourceHashText = formatContentHash(record.sourceHash);
    writer.value(std::string_view(sourceHashText));
    writer.key(BLENDER_PATH_KEY);
    writer.value(std::string_view(record.blenderPath));
    writer.key(BLENDER_VERSION_KEY);
    writer.value(std::string_view(record.blenderVersion));
    writer.key(SCRIPT_VERSION_KEY);
    writer.value(static_cast<unsigned long long>(record.scriptVersion));
    writer.key(SETTINGS_FINGERPRINT_KEY);
    writer.value(std::string_view(record.settingsFingerprint));
    writer.key(ARTIFACT_BYTES_KEY);
    writer.value(static_cast<unsigned long long>(record.artifactBytes));
    writer.endObject();
    std::string text = writer.str();
    text += '\n';
    return text;
}

bool provenanceMatches(const ExportProvenance& actual, const ExportProvenance& expected) noexcept {
    if (!(actual.sourceHash == expected.sourceHash)) {
        return false;
    }
    if (actual.scriptVersion != expected.scriptVersion) {
        return false;
    }
    if (actual.settingsFingerprint != expected.settingsFingerprint) {
        return false;
    }
    // CONDITIONAL, and this is the whole of §A-9. An empty expected version means nothing in this
    // session has probed one, so there is no version to compare against -- and demanding one would
    // make a cache hit spawn a process, which is precisely what AC-22 forbids. The recorded value is
    // still DISPLAYED ("produced by Blender 5.2.0 LTS"); it is simply not used as a gate until a probe
    // has happened. Seed S28 makes this comparison unconditional and must redden BT58.
    if (!expected.blenderVersion.empty() && actual.blenderVersion != expected.blenderVersion) {
        return false;
    }
    // sourcePath, blenderPath, guid and artifactBytes are INFORMATIONAL and are deliberately not
    // compared (E24, seed S9): moving a .blend together with its sidecar must not invalidate.
    return true;
}

std::string blendExportSettingsFingerprint(const ImportSettings& settings) {
    // MOVED VERBATIM from model_import_session.cpp's anonymous-namespace copy at task 3.1.5 (§0.9),
    // whose old name is deliberately not spelled anywhere under editor/src: that step's gate greps for
    // it, and a prose mention would turn it red for a reason that is not a violation. It stays a pure
    // string function -- no disk, no SDL, no logging -- so this file's INV-B10/INV-B14 posture is
    // unchanged.
    const std::string text = writeMetaText(Guid{}, settings);
    return formatContentHash(hashBytes(std::as_bytes(std::span<const char>(text))));
}

std::optional<ExportStatus> parseExportStatus(std::string_view text) {
    const JsonParseResult parsed = parseJson(text);
    const JsonValue* root = objectRoot(parsed);
    if (root == nullptr) {
        return std::nullopt;  // unparseable or truncated (E22) -- treated by the caller as "no status file"
    }
    // NO "version" key: this document is written by the SCRIPT, not by this build, and adding a
    // version to it would be a second thing to keep in sync across the process boundary for no gain.
    // "ok" is the only required field; everything else is best-effort reporting.
    const JsonValue* okField = root->find(OK_KEY);
    if (okField == nullptr) {
        return std::nullopt;
    }
    const std::optional<bool> okValue = okField->asBool();
    if (!okValue.has_value()) {
        return std::nullopt;
    }

    ExportStatus status;
    status.ok = *okValue;
    if (!optionalString(*root, BLENDER_KEY, status.blender) || !optionalString(*root, ERROR_KEY, status.error)) {
        return std::nullopt;
    }
    if (const JsonValue* bytesField = root->find(BYTES_KEY); bytesField != nullptr) {
        const std::optional<std::uint64_t> bytesValue = bytesField->asU64();
        if (!bytesValue.has_value()) {
            return std::nullopt;
        }
        status.bytes = *bytesValue;
    }
    if (const JsonValue* droppedField = root->find(DROPPED_KEY); droppedField != nullptr) {
        if (!droppedField->isArray()) {
            return std::nullopt;
        }
        for (const JsonValue& element : droppedField->elements()) {
            const std::optional<std::string_view> name = element.asString();
            if (!name.has_value()) {
                return std::nullopt;
            }
            status.dropped.emplace_back(*name);
        }
    }
    return status;
}

}  // namespace engine::editor

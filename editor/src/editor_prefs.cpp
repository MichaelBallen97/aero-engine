// Aero Engine — editor_prefs.json's codec and its two file operations (task E.3.2). PURE std C++ plus
// the editor's own text-file primitive and aero::reflect's JSON: NO SDL, NO <filesystem>, NO ImGui,
// NO logging. The path this reads and writes is resolved by the caller (project_file.cpp owns the SDL
// half) and the one WARN lives in editor_app.cpp.
#include <aero/editor/editor_prefs.hpp>
#include <aero/editor/text_file.hpp>
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

namespace {

// JsonValue / JsonParseResult / JsonWriter / parseJson live in `engine`, NOT in `engine::reflect`
// (json_value.hpp:15, json_reader.hpp:15, json_writer.hpp:7), so the unqualified names below resolve
// through the enclosing namespace exactly as blender_tool.cpp's codec does.
constexpr std::string_view VERSION_KEY = "version";
constexpr std::string_view FOCUS_FOLLOWS_SELECTION_KEY = "focusFollowsSelection";

// The three readers below are FILE-LOCAL COPIES of blender_tool.cpp's shape (:409, :419, :443), which
// lives in THAT TU's anonymous namespace and is not reachable from here. optionalBool has no
// counterpart anywhere in the tree and is new. Scaffolding is copied; the assertion is shared --
// text_file_test.cpp's TempDir banner states the same posture for tests.

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

// An OPTIONAL bool: ABSENT leaves `out` untouched and succeeds -- which is what makes appending a key
// to this file a non-breaking change. PRESENT-BUT-NOT-A-BOOL is a MISS for the WHOLE document, because
// a file whose shape disagrees with this one is not a file this build wrote. A number or a string is
// never coerced: docs/09 section 8.2's non-string blenderPath rule, one type over.
[[nodiscard]] bool optionalBool(const JsonValue& root, std::string_view key, bool& out) {
    const JsonValue* field = root.find(key);
    if (field == nullptr) {
        return true;  // absent -> the default, and that is not a failure
    }
    const std::optional<bool> value = field->asBool();
    if (!value.has_value()) {
        return false;
    }
    out = *value;
    return true;
}

}  // namespace

std::optional<EditorPrefs> parseEditorPrefs(std::string_view text) {
    const JsonParseResult parsed = parseJson(text);
    const JsonValue* root = objectRoot(parsed);
    if (root == nullptr || !versionEquals(*root, EDITOR_PREFS_FORMAT_VERSION)) {
        return std::nullopt;
    }
    EditorPrefs prefs;
    if (!optionalBool(*root, FOCUS_FOLLOWS_SELECTION_KEY, prefs.focusFollowsSelection)) {
        return std::nullopt;
    }
    return prefs;
}

std::string writeEditorPrefsText(const EditorPrefs& prefs) {
    JsonWriter writer;  // the DEFAULT config: pretty, 2-space -- docs/09's canonical form. Do NOT
                        // spell the config out; a second spelling is a second truth.
    writer.beginObject();
    writer.key(VERSION_KEY);
    writer.value(static_cast<long long>(EDITOR_PREFS_FORMAT_VERSION));
    writer.key(FOCUS_FOLLOWS_SELECTION_KEY);
    writer.value(prefs.focusFollowsSelection);
    writer.endObject();
    std::string text = writer.str();
    text += '\n';  // exactly ONE trailing newline (the writer itself has none; parseJson accepts it)
    return text;
}

EditorPrefs readEditorPrefs(std::string_view pathUtf8, bool& corrupt) {
    corrupt = false;
    if (pathUtf8.empty()) {
        return {};  // this instance does not persist preferences at all (D14) -- not a failure
    }
    const FileReadResult read = readTextFile(pathUtf8);
    if (!read.text.has_value()) {
        return {};  // a MISSING file is defaults, SILENTLY -- the normal state on a fresh machine
    }
    const std::optional<EditorPrefs> parsed = parseEditorPrefs(*read.text);
    if (!parsed.has_value()) {
        corrupt = true;  // it EXISTS and does not parse -- the caller emits the one WARN
        return {};
    }
    return *parsed;
}

std::string writeEditorPrefs(std::string_view pathUtf8, const EditorPrefs& prefs) {
    if (pathUtf8.empty()) {
        return {};  // D14: no path means no file, and that is a success
    }
    // A NAMED LOCAL, FIRST, exactly as the amended INV-A1 requires of every path handed to a write
    // path (blender_service.cpp:175-176). ONE writeTextFileAtomic call in this TU, and section V6 is
    // the grep that keeps it one.
    const std::string prefsPath(pathUtf8);
    return writeTextFileAtomic(prefsPath, writeEditorPrefsText(prefs));
}

}  // namespace engine::editor

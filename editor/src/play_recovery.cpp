// Aero Engine — the play record's codec, its paths, its deciders and its four disk functions
// (task 4.7.3). PURE std C++ plus the editor's own text-file primitives and aero::reflect's JSON:
// NO SDL, NO <filesystem>, NO ImGui, NO entt, NO logging. EditorApp logs the autosave's lines and
// scene_session.cpp the offer's.
//
// Nothing here deletes: a finished Play's copy stays until the next Play that writes one.
#include <aero/editor/asset_cache.hpp>  // ASSET_CACHE_DIR_NAME / ASSET_CACHE_GITIGNORE_NAME / LIBRARY_GITIGNORE_TEXT
#include <aero/editor/play_recovery.hpp>
#include <aero/editor/project.hpp>        // isLegalRelativePath -- the ONE validator for a relative path
#include <aero/editor/project_state.hpp>  // MAX_PROJECT_STATE_SCENE_BYTES
#include <aero/editor/text_file.hpp>      // readTextFile / writeTextFileAtomic / fileExists / ensureDirectory
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace engine::editor {

namespace {

// JsonValue / JsonParseResult / JsonWriter / parseJson live in `engine`, NOT in `engine::reflect`, so the
// unqualified names below resolve through the enclosing namespace exactly as project_state.cpp's do.
constexpr std::string_view VERSION_KEY = "version";
constexpr std::string_view PLAYING_KEY = "playing";
constexpr std::string_view SCENE_KEY = "scene";
constexpr std::string_view COPY_WRITTEN_KEY = "copyWritten";

// objectRoot / versionEquals: FILE-LOCAL COPIES of project_state.cpp's, which live in THAT TU's
// anonymous namespace. Scaffolding is copied; the assertion is shared.
[[nodiscard]] const JsonValue* objectRoot(const JsonParseResult& parsed) noexcept {
    if (!parsed.value.has_value()) {
        return nullptr;  // NOT `!parsed.ok()`: the optional-access check cannot see through an opaque ok()
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

// A required bool key: absent or not a bool is nullopt, never a default.
[[nodiscard]] std::optional<bool> requiredBool(const JsonValue& root, std::string_view key) {
    const JsonValue* field = root.find(key);
    if (field == nullptr) {
        return std::nullopt;
    }
    return field->asBool();
}

// THE one join: strips EVERY trailing '/' from the root, then joins with exactly one.
[[nodiscard]] std::string joinUnderRoot(std::string_view rootUtf8, std::string_view relativeUtf8) {
    std::string joined(rootUtf8);
    while (!joined.empty() && joined.back() == '/') {
        joined.pop_back();
    }
    joined += '/';
    joined += relativeUtf8;
    return joined;
}

[[nodiscard]] std::string libraryPath(std::string_view rootUtf8, std::string_view leaf) {
    std::string relative(ASSET_CACHE_DIR_NAME);
    relative += '/';
    relative += leaf;
    return joinUnderRoot(rootUtf8, relative);
}

// ensureDirectory(<root>/Library), then Library/.gitignore only when absent. Returns "" or the directory's
// OS reason. A `Library` occupied by a FILE fails here, and nothing below it runs.
[[nodiscard]] std::string prepareLibrary(std::string_view rootUtf8) {
    const std::string libraryDir = joinUnderRoot(rootUtf8, ASSET_CACHE_DIR_NAME);
    if (std::string dirError = ensureDirectory(libraryDir); !dirError.empty()) {
        return dirError;
    }
    const std::string ignorePath = libraryDir + '/' + std::string(ASSET_CACHE_GITIGNORE_NAME);
    if (!fileExists(ignorePath)) {  // written ONLY when absent, NEVER overwritten
        // DELIBERATELY DISCARDED: a .gitignore failure does not block the record, whose own outcome is
        // what the caller gets (project_state.cpp's shape).
        (void)writeTextFileAtomic(ignorePath, LIBRARY_GITIGNORE_TEXT);
    }
    return {};
}

}  // namespace

std::optional<PlayRecoveryState> parsePlayRecoveryState(std::string_view text) {
    const JsonParseResult parsed = parseJson(text);
    const JsonValue* root = objectRoot(parsed);
    if (root == nullptr || !versionEquals(*root, PLAY_RECOVERY_FORMAT_VERSION)) {
        return std::nullopt;
    }
    const std::optional<bool> playing = requiredBool(*root, PLAYING_KEY);
    const std::optional<bool> copyWritten = requiredBool(*root, COPY_WRITTEN_KEY);
    const JsonValue* sceneField = root->find(SCENE_KEY);
    if (!playing.has_value() || !copyWritten.has_value() || sceneField == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::string_view> scene = sceneField->asString();
    if (!scene.has_value() || scene->size() > MAX_PROJECT_STATE_SCENE_BYTES) {
        return std::nullopt;  // a FAILURE, never a truncation -- a truncated path names a different file
    }
    if (!scene->empty() && !isLegalRelativePath(*scene)) {
        return std::nullopt;
    }
    return PlayRecoveryState{.playing = *playing, .scene = std::string(*scene), .copyWritten = *copyWritten};
}

std::string writePlayRecoveryStateText(const PlayRecoveryState& state) {
    JsonWriter writer;  // the DEFAULT config: pretty, 2-space. A second spelling is a second truth.
    writer.beginObject();
    writer.key(VERSION_KEY);
    writer.value(static_cast<long long>(PLAY_RECOVERY_FORMAT_VERSION));
    writer.key(PLAYING_KEY);
    writer.value(state.playing);
    writer.key(SCENE_KEY);
    writer.value(std::string_view(state.scene));  // EXPLICIT: JsonWriter also overloads bool and long long
    writer.key(COPY_WRITTEN_KEY);
    writer.value(state.copyWritten);
    writer.endObject();
    std::string text = writer.str();
    text += '\n';  // exactly ONE trailing newline
    return text;
}

std::string playRecoveryStatePath(std::string_view projectRootUtf8) {
    if (projectRootUtf8.empty()) {
        return {};
    }
    return libraryPath(projectRootUtf8, PLAY_RECOVERY_FILE_NAME);
}

std::string playRecoveryScenePath(std::string_view projectRootUtf8) {
    if (projectRootUtf8.empty()) {
        return {};
    }
    return libraryPath(projectRootUtf8, PLAY_RECOVERY_SCENE_FILE_NAME);
}

PlayRecoveryRead readPlayRecoveryState(std::string_view projectRootUtf8) {
    if (projectRootUtf8.empty()) {
        return {};  // no project, no record -- not a failure
    }
    const std::string path = playRecoveryStatePath(projectRootUtf8);
    const FileReadResult read = readTextFile(path);
    if (!read.text.has_value()) {
        // readTextFile disengages `text` for a missing file, a directory and an unreadable file alike, so
        // fileExists is the discriminator: a missing record is the normal state, anything else is corrupt.
        return PlayRecoveryRead{.state = std::nullopt, .corrupt = fileExists(path)};
    }
    std::optional<PlayRecoveryState> parsed = parsePlayRecoveryState(*read.text);
    if (!parsed.has_value()) {
        return PlayRecoveryRead{.state = std::nullopt, .corrupt = true};
    }
    return PlayRecoveryRead{.state = std::move(parsed), .corrupt = false};
}

std::string writePlayRecoveryState(std::string_view projectRootUtf8, const PlayRecoveryState& state) {
    if (projectRootUtf8.empty()) {
        return {};
    }
    if (std::string error = prepareLibrary(projectRootUtf8); !error.empty()) {
        return error;
    }
    const std::string statePath = playRecoveryStatePath(projectRootUtf8);
    return writeTextFileAtomic(statePath, writePlayRecoveryStateText(state));
}

std::string writePlayRecoveryCopy(std::string_view projectRootUtf8, std::string_view sceneText) {
    if (projectRootUtf8.empty()) {
        return {};
    }
    if (std::string error = prepareLibrary(projectRootUtf8); !error.empty()) {
        return error;
    }
    const std::string copyPath = playRecoveryScenePath(projectRootUtf8);
    return writeTextFileAtomic(copyPath, sceneText);
}

PlayAutosavePlan planPlayAutosave(bool dirty, bool sceneIo) noexcept {
    return PlayAutosavePlan{
        .writeCopy = dirty && sceneIo,
        .warnNoSceneIo = dirty && !sceneIo,
        .refuseOnFailure = dirty && sceneIo,
    };
}

bool playRecoveryOffered(const PlayRecoveryFacts& facts) noexcept {
    return facts.sceneIo && facts.found && !facts.corrupt && facts.playing && facts.copyWritten && facts.copyExists;
}

}  // namespace engine::editor

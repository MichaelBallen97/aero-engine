// Aero Engine — editor-state.json's codec, its path arithmetic, its two deciders and its two file
// operations (task E.4.1). PURE std C++ plus the editor's own text-file and directory primitives and
// aero::reflect's JSON: NO SDL, NO <filesystem>, NO ImGui, NO entt, NO logging. The two WARNs this
// format can produce live in scene_session.cpp (a corrupt file) and editor_app.cpp (a failed write).
//
// asset_cache.hpp supplies ASSET_CACHE_DIR_NAME, ASSET_CACHE_GITIGNORE_NAME and LIBRARY_GITIGNORE_TEXT:
// the ONE spelling of "Library" and the ONE ignore text, imported exactly as blender_tool.cpp imports
// the first of them. The RULE (create the directory, write the ignore file when absent) is repeated
// here; no STRING is.
#include <aero/editor/asset_cache.hpp>
#include <aero/editor/project.hpp>        // isLegalRelativePath -- the ONE validator for a relative path
#include <aero/editor/project_files.hpp>  // listDirectory / joinRelative / FileEntry / ScanStatus
#include <aero/editor/project_state.hpp>
#include <aero/editor/text_file.hpp>  // readTextFile / writeTextFileAtomic / fileExists / ensureDirectory
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

namespace {

// JsonValue / JsonParseResult / JsonWriter / parseJson live in `engine`, NOT in `engine::reflect`
// (json_value.hpp:15, json_reader.hpp:15, json_writer.hpp:7), so the unqualified names below resolve
// through the enclosing namespace exactly as editor_prefs.cpp's codec does.
constexpr std::string_view VERSION_KEY = "version";
constexpr std::string_view LAST_SCENE_KEY = "lastScene";
constexpr std::string_view SCENE_FILE_SUFFIX = ".scene.json";

// objectRoot / versionEquals: FILE-LOCAL COPIES of editor_prefs.cpp:31-48, which lives in THAT TU's
// anonymous namespace and is not reachable from here. Scaffolding is copied; the assertion is shared
// (editor_prefs.cpp:26-29's own banner states the posture).

[[nodiscard]] const JsonValue* objectRoot(const JsonParseResult& parsed) noexcept {
    if (!parsed.value.has_value()) {
        // NOT `!parsed.ok()`: bugprone-unchecked-optional-access cannot connect an opaque out-of-line
        // ok() to `value` (project.cpp's precedent, editor_prefs.cpp's third application).
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

// A FOURTH file-local copy of blender_tool.cpp:42-63's foldAscii/endsWithFolded pair, for the same
// reason the other three exist: both live in that TU's anonymous namespace and are not reachable from
// here. ASCII-only and locale-independent -- NEVER std::tolower(char), whose UTF-8 continuation bytes
// are negative as char.
constexpr unsigned char foldAscii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

// `name.size() <= ext.size()` is THE STEM REQUIREMENT -- ".scene.json" alone is not a scene of
// anything, exactly as asset_meta.cpp:172 says of ".meta" and blender_tool.cpp:48-49 of ".blend".
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

// THE one join. Strips EVERY trailing '/' from the root, then joins with exactly one. A root of "/"
// becomes "" and the prefix becomes "/", which is the right answer for a project at the filesystem
// root. Used by projectStatePath, absoluteScenePath and writeProjectState, so a change to the join
// rule cannot reach one of the three and miss the others.
[[nodiscard]] std::string joinUnderRoot(std::string_view rootUtf8, std::string_view relativeUtf8) {
    std::string joined(rootUtf8);
    while (!joined.empty() && joined.back() == '/') {
        joined.pop_back();
    }
    joined += '/';
    joined += relativeUtf8;
    return joined;
}

// '\' -> '/', on a COPY. The scene path arrives from a native dialog on Windows and is
// backslash-separated; the recorded form is always '/'-separated (D3/A20).
[[nodiscard]] std::string unifySeparators(std::string_view pathUtf8) {
    std::string unified(pathUtf8);
    for (char& c : unified) {
        if (c == '\\') {
            c = '/';
        }
    }
    return unified;
}

}  // namespace

std::optional<ProjectState> parseProjectState(std::string_view text) {
    const JsonParseResult parsed = parseJson(text);
    const JsonValue* root = objectRoot(parsed);
    if (root == nullptr || !versionEquals(*root, PROJECT_STATE_FORMAT_VERSION)) {
        return std::nullopt;
    }
    ProjectState state;
    const JsonValue* field = root->find(LAST_SCENE_KEY);
    if (field == nullptr) {
        return state;  // ABSENT is an ANSWER (D2), not a failure -- and it is what makes appending a
                       // key to this file a non-breaking change (H4).
    }
    const std::optional<std::string_view> value = field->asString();
    if (!value.has_value()) {
        return std::nullopt;  // PRESENT but not a string: never coerced (docs/09 8.2's rule, one type over)
    }
    if (value->size() > MAX_PROJECT_STATE_SCENE_BYTES) {
        return std::nullopt;  // a FAILURE, never a truncation -- a truncated path names a different file
    }
    if (!value->empty() && !isLegalRelativePath(*value)) {
        return std::nullopt;  // absolute, backslashed, drive-lettered, or carrying a ".." segment
    }
    state.lastScene = std::string(*value);
    state.lastSceneRecorded = true;  // PRESENCE is the answer, whatever the value
    return state;
}

std::string writeProjectStateText(const ProjectState& state) {
    JsonWriter writer;  // the DEFAULT config: pretty, 2-space -- docs/09's canonical form. Do NOT
                        // spell the config out; a second spelling is a second truth.
    writer.beginObject();
    writer.key(VERSION_KEY);
    writer.value(static_cast<long long>(PROJECT_STATE_FORMAT_VERSION));
    if (state.lastSceneRecorded) {
        writer.key(LAST_SCENE_KEY);
        writer.value(std::string_view(state.lastScene));  // EXPLICIT: JsonWriter also overloads bool
                                                          // and long long, and a future overload must
                                                          // not silently win this call.
    }
    writer.endObject();
    std::string text = writer.str();
    text += '\n';  // exactly ONE trailing newline (the writer itself has none; parseJson accepts it)
    return text;
}

std::string projectStatePath(std::string_view projectRootUtf8) {
    if (projectRootUtf8.empty()) {
        return {};
    }
    // NOT routed through absoluteScenePath even though "Library/editor-state.json" would pass
    // isLegalRelativePath: this is a fixed internal path, not a user value, and borrowing the
    // validator would make a future change to it able to break the state file's own location.
    std::string relative(ASSET_CACHE_DIR_NAME);
    relative += '/';
    relative += PROJECT_STATE_FILE_NAME;
    return joinUnderRoot(projectRootUtf8, relative);
}

std::string projectRelativeScenePath(std::string_view projectRootUtf8, std::string_view absoluteScenePathUtf8) {
    if (projectRootUtf8.empty() || absoluteScenePathUtf8.empty()) {
        return {};
    }
    std::string root = unifySeparators(projectRootUtf8);
    while (!root.empty() && root.back() == '/') {
        root.pop_back();
    }
    const std::string scene = unifySeparators(absoluteScenePathUtf8);
    const std::string prefix = root + '/';
    // STRICTLY greater: a scene equal to `prefix` is the root DIRECTORY, not a file inside it, and a
    // scene equal to `root` is the root itself. Both record "" (PJ14).
    if (scene.size() <= prefix.size() || scene.compare(0, prefix.size(), prefix) != 0) {
        return {};  // the separator-boundary test: "/p/Game2/x" is NOT inside "/p/Game" (PJ13)
    }
    std::string relative = scene.substr(prefix.size());
    // R25 -- THE PRODUCER GUARANTEES WHAT THE PARSER REQUIRES. Without this, a scene path carrying a
    // ".." segment (nothing refuses one today -- that is E.4.2's defect to close) would be WRITTEN and
    // then REFUSED by this build's own parseProjectState on the next open: one WARN and a forgotten
    // position, on a round trip that never left this process. Its failure mode is still D8's "forget".
    if (!isLegalRelativePath(relative)) {
        return {};
    }
    return relative;
}

std::string absoluteScenePath(std::string_view projectRootUtf8, std::string_view relativeUtf8) {
    if (projectRootUtf8.empty() || relativeUtf8.empty() || !isLegalRelativePath(relativeUtf8)) {
        return {};
    }
    return joinUnderRoot(projectRootUtf8, relativeUtf8);
}

ProjectState readProjectState(std::string_view projectRootUtf8, bool& corrupt) {
    corrupt = false;
    if (projectRootUtf8.empty()) {
        return {};  // this instance has no project at all -- not a failure
    }
    const std::string path = projectStatePath(projectRootUtf8);
    const FileReadResult read = readTextFile(path);
    if (!read.text.has_value()) {
        // readTextFile disengages `text` for a MISSING file, a DIRECTORY and an UNREADABLE file alike
        // (text_file.hpp:28), so the read alone cannot tell "the normal state on a machine that has
        // never opened this project" from "this file exists and the OS refused it" -- and the header
        // says those two must never be conflated, or every first open of every project warns.
        // fileExists is the discriminator, and it is std::filesystem::exists, so a DIRECTORY wearing
        // the name reads TRUE and takes the corrupt arm (PJ33).
        corrupt = fileExists(path);
        return {};
    }
    const std::optional<ProjectState> parsed = parseProjectState(*read.text);
    if (!parsed.has_value()) {
        corrupt = true;  // it EXISTS and does not parse -- the caller emits the one WARN
        return {};
    }
    return *parsed;
}

std::string writeProjectState(std::string_view projectRootUtf8, const ProjectState& state) {
    if (projectRootUtf8.empty()) {
        return {};  // no project means no file, and that is a success
    }
    const std::string libraryDir = joinUnderRoot(projectRootUtf8, ASSET_CACHE_DIR_NAME);
    // writeTextFileAtomic does NOT create parent directories (asset_database.cpp:841 calls this first,
    // which is why it is a separate primitive at text_file.hpp:51). A `Library` occupied by a FILE
    // fails here, with an OS reason, and nothing below runs (PJ37).
    if (const std::string dirError = ensureDirectory(libraryDir); !dirError.empty()) {
        return dirError;
    }
    // D9: this may be the FIRST thing ever written into Library/, so the ignore file has to exist or a
    // project could acquire a committed editor-state.json. Today AssetDatabase::rescan writes both in
    // tick 1's reconcile for every open project -- but that coupling is invisible and one reconcile
    // edit away from being false. The RULE is repeated here; no STRING is.
    const std::string ignorePath = libraryDir + '/' + std::string(ASSET_CACHE_GITIGNORE_NAME);
    if (!fileExists(ignorePath)) {  // written ONLY when absent, NEVER overwritten (PJ36)
        // DELIBERATELY DISCARDED: a .gitignore failure does not block the state write, and the state
        // write's own outcome is what is returned (asset_database.cpp:845-851's shape, where the
        // ignore error is recorded and the index is still written). The state file is the point; a
        // missing .gitignore is a git annoyance the next rescan fixes.
        (void)writeTextFileAtomic(ignorePath, LIBRARY_GITIGNORE_TEXT);
    }
    // A NAMED LOCAL, FIRST, exactly as the amended INV-A1 requires of every path handed to a write
    // path. TWO writeTextFileAtomic call sites in this TU, both built from the LIBRARY directory and
    // neither from the assets root -- which is the shape INV-A1 is stated in.
    const std::string statePath = projectStatePath(projectRootUtf8);
    return writeTextFileAtomic(statePath, writeProjectStateText(state));
}

bool isSceneFileName(std::string_view fileNameUtf8) noexcept { return endsWithFolded(fileNameUtf8, SCENE_FILE_SUFFIX); }

std::string firstSceneUnder(std::string_view projectRootUtf8, std::string_view scenesRelativeUtf8) {
    if (projectRootUtf8.empty() || scenesRelativeUtf8.empty()) {
        return {};
    }
    const DirectoryListing listing = listDirectory(projectRootUtf8, scenesRelativeUtf8, /*includeHidden=*/false);
    if (listing.status != ScanStatus::Ok) {
        return {};  // Missing, NotADirectory, Unreadable -- all simply yield no candidate (D7)
    }
    for (const FileEntry& entry : listing.entries) {
        // entryOrderLess sorts DIRECTORIES FIRST (project_files.hpp:109-113), so "take entries.front()"
        // is wrong in the most ordinary case there is -- a scenes/ folder with a subfolder in it.
        if (entry.isDirectory || !isSceneFileName(entry.name)) {
            continue;
        }
        return joinRelative(scenesRelativeUtf8, entry.name);  // ROOT-relative, not scenes-relative
    }
    return {};
}

StartupScene chooseStartupScene(const StartupSceneFacts& facts) noexcept {
    // ARM ORDER IS LOAD-BEARING. The Recorded test comes first and carries !recordedEmpty explicitly,
    // so the (impossible-in-practice, still tested) row recorded && recordedEmpty && recordedExists
    // falls to the second arm and yields NewScene. And `recordedExists` appears in exactly ONE
    // expression, which is what makes PJ25's four !recorded rows a real assertion.
    if (facts.recorded && !facts.recordedEmpty && facts.recordedExists) {
        return StartupScene::Recorded;
    }
    if (facts.recorded && facts.recordedEmpty) {
        return StartupScene::NewScene;  // D2's third state: the user was on an UNSAVED scene
    }
    return facts.firstSceneFound ? StartupScene::FirstUnderScenes : StartupScene::NewScene;
}

ProjectStateStep projectStateStep(std::string_view currentRoot, std::string_view baselineRoot,
                                  std::string_view currentScene, std::string_view baselineScene) noexcept {
    if (currentRoot != baselineRoot) {
        return ProjectStateStep::AdoptBaseline;  // the state was READ, not changed (D5)
    }
    if (currentRoot.empty()) {
        return ProjectStateStep::Nothing;  // no project, nothing to record
    }
    return currentScene != baselineScene ? ProjectStateStep::Write : ProjectStateStep::Nothing;
}

}  // namespace engine::editor

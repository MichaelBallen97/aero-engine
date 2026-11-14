#pragma once
// Aero Engine — <projectRoot>/Library/play-recovery.json (v1) and Library/play-recovery.scene.json, the
// editor's seatbelt around Play (task 4.7.3). DERIVED, MACHINE-LOCAL, NEVER COMMITTED -- beside
// editor-state.json inside the directory that is already gitignored, unscanned and unwatched.
//
// The record says whether a game was started and no Stop has written since (`playing`), which scene the
// document was on (`scene`, project-relative, "" for an untitled one) and whether that Play wrote the
// copy (`copyWritten`). The copy is scene format v1, the bytes Save Scene writes. A project open reads
// the record and, for exactly one combination of facts, offers the copy back.
//
// PUBLIC, and therefore ImGui-free, entt-free, SDL-free and <filesystem>-free BY FILE PLACEMENT
// (.claude/rules/editor.md). Pure apart from the four disk functions, which go through text_file.hpp.
// NOTHING HERE LOGS (project_state.hpp's posture): a status is RETURNED and the caller words it.
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

inline constexpr int PLAY_RECOVERY_FORMAT_VERSION = 1;
inline constexpr std::string_view PLAY_RECOVERY_FILE_NAME = "play-recovery.json";              // the record
inline constexpr std::string_view PLAY_RECOVERY_SCENE_FILE_NAME = "play-recovery.scene.json";  // the copy

struct PlayRecoveryState {
    bool playing = false;      // a game was started and no Stop has written since
    std::string scene;         // project-relative, '/'-separated, isLegalRelativePath or "" (untitled / outside)
    bool copyWritten = false;  // this Play wrote the copy
    friend bool operator==(const PlayRecoveryState&, const PlayRecoveryState&) = default;
};

// nullopt on: unparseable JSON, a non-object root, a missing or wrong "version", a missing or non-bool
// "playing" or "copyWritten", a missing or non-string "scene", a "scene" longer than
// MAX_PROJECT_STATE_SCENE_BYTES, and a non-empty "scene" that is not isLegalRelativePath. EVERY key is
// required: a record is written whole by one writer, so a missing key means a damaged file, and a damaged
// record must never offer. Unknown keys are ignored (append-only evolution).
[[nodiscard]] std::optional<PlayRecoveryState> parsePlayRecoveryState(std::string_view text);

// Deterministic: JsonWriter's default config, keys "version", "playing", "scene", "copyWritten" in that
// order, exactly one trailing '\n'. Re-parses equal.
[[nodiscard]] std::string writePlayRecoveryStateText(const PlayRecoveryState& state);

// <root>/Library/play-recovery.json and <root>/Library/play-recovery.scene.json, '/'-joined with trailing
// separators on the root stripped. "" in, "" out.
[[nodiscard]] std::string playRecoveryStatePath(std::string_view projectRootUtf8);
[[nodiscard]] std::string playRecoveryScenePath(std::string_view projectRootUtf8);

struct PlayRecoveryRead {
    std::optional<PlayRecoveryState> state;
    bool corrupt = false;  // the file exists and could not be read or parsed
};
// No file is `{nullopt, corrupt false}`; a directory, an unreadable file or malformed text is corrupt.
[[nodiscard]] PlayRecoveryRead readPlayRecoveryState(std::string_view projectRootUtf8);

// "" on success, the OS reason otherwise. Both: ensureDirectory(<root>/Library), Library/.gitignore when
// absent (never overwritten, its failure not reported), then ONE writeTextFileAtomic. "" root: nothing, "".
[[nodiscard]] std::string writePlayRecoveryState(std::string_view projectRootUtf8, const PlayRecoveryState& state);
[[nodiscard]] std::string writePlayRecoveryCopy(std::string_view projectRootUtf8, std::string_view sceneText);

struct PlayAutosavePlan {
    bool writeCopy = false;        // dirty && sceneIo
    bool warnNoSceneIo = false;    // dirty && !sceneIo
    bool refuseOnFailure = false;  // dirty && sceneIo: a failed copy OR record refuses Play
    friend constexpr bool operator==(const PlayAutosavePlan&, const PlayAutosavePlan&) = default;
};
[[nodiscard]] PlayAutosavePlan planPlayAutosave(bool dirty, bool sceneIo) noexcept;

struct PlayRecoveryFacts {
    bool sceneIo = false;      // sceneIoAvailable()
    bool found = false;        // the record file exists
    bool corrupt = false;      // it exists and does not read or parse
    bool playing = false;      // the parsed record's
    bool copyWritten = false;  // the parsed record's
    bool copyExists = false;   // fileExists(playRecoveryScenePath(root))
};
// True for exactly one row of the 64: sceneIo && found && !corrupt && playing && copyWritten && copyExists.
[[nodiscard]] bool playRecoveryOffered(const PlayRecoveryFacts& facts) noexcept;

}  // namespace engine::editor

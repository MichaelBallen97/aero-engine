#pragma once
// Aero Engine — <projectRoot>/Library/editor-state.json (v1), the editor's PER-PROJECT machine-local
// working position (task E.4.1). DERIVED, MACHINE-LOCAL, DISPOSABLE -- one file per PROJECT, beside
// asset-cache.json and BlenderExports/ inside the directory that is already gitignored, already
// excluded from the asset scan by canonical path and already excluded from the watcher the same way
// (docs/09 section 4.10).
//
// WHY NOT editor_prefs.json (D1): that file is ONE PER MACHINE and its whole documented nature is "a
// property of a person at a machine, not of a project" (docs/09 section 8.5). A map keyed by project
// root inside it would grow without bound, would have no eviction rule, and would break the moment the
// project directory is renamed -- which is exactly when a user most wants their scene back.
//
// WHY NOT project.json: it is committed and shared. A working position is the worst possible thing to
// hand a teammate; docs/09 section 8.1 already makes this argument for the Blender path.
//
// THE FILE IS THE HOME FOR EVERY FUTURE PER-PROJECT EDITOR POSITION -- the editor camera pose is the
// named candidate (handoff H4). Append a key, bump nothing: an ABSENT key is its own answer by this
// parser's rule below.
//
// NOTHING HERE LOGS (project_files.hpp:15-16's convention, applied for the seventh time in this tree,
// and editor_prefs.cpp's identical posture). Status is RETURNED: scene_session.cpp turns `corrupt`
// into the one WARN and editor_app.cpp turns a write failure into the other.
//
// PUBLIC, and therefore ImGui-free, entt-free, SDL-free and <filesystem>-free BY FILE PLACEMENT
// (.claude/rules/editor.md). Every filesystem call it makes goes through text_file.hpp and
// project_files.hpp, exactly as editor_prefs.cpp and asset_cache.cpp do.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace engine::editor {

inline constexpr int PROJECT_STATE_FORMAT_VERSION = 1;
inline constexpr std::string_view PROJECT_STATE_FILE_NAME = "editor-state.json";
// A project-RELATIVE path. 1024 is generous against every platform's own limit (Windows MAX_PATH is
// 260; a long-path-enabled volume allows 32767 for an ABSOLUTE path) and bounds what a hand-edited
// document can make this parser allocate. A longer value is a parse FAILURE, never a truncation --
// truncating a path silently names a different file.
inline constexpr std::size_t MAX_PROJECT_STATE_SCENE_BYTES = 1024;

// ---- the format ---------------------------------------------------------------------------------

struct ProjectState {
    // Project-relative, '/'-separated, isLegalRelativePath (project.hpp:90-94) or EMPTY.
    std::string lastScene;
    // THE THIRD STATE (D2). `lastScene` empty with `lastSceneRecorded` FALSE means "nothing was ever
    // written here" and resolves to the first scene under paths.scenes; empty with it TRUE means "the
    // user was on an unsaved scene" and resolves to a NEW scene. Collapsing the two hands a person who
    // chose File > New Scene somebody else's scene on the next launch.
    bool lastSceneRecorded = false;
};

// nullopt on: unparseable JSON, a non-object root, a missing or wrong "version", a PRESENT-but-
// non-string "lastScene", a "lastScene" longer than MAX_PROJECT_STATE_SCENE_BYTES, and a non-empty
// "lastScene" that is not isLegalRelativePath (absolute, backslashed, drive-lettered, or carrying a
// ".." segment). An ABSENT "lastScene" is `lastSceneRecorded = false` and is NOT a failure, which is
// what makes appending a key to this file a non-breaking change (the editor_prefs.cpp optionalBool
// rule at :50-65, with its meaning inverted -- here the absence is an ANSWER).
[[nodiscard]] std::optional<ProjectState> parseProjectState(std::string_view text);

// Deterministic: JsonWriter's DEFAULT config, fixed key order with "version" first, exactly one
// trailing '\n'. Re-parses equal, and two writes of the same value are byte-identical.
// `lastSceneRecorded == false` OMITS the key entirely.
[[nodiscard]] std::string writeProjectStateText(const ProjectState& state);

// ---- the path, and the two directions of the relative form --------------------------------------

// <root>/Library/editor-state.json, '/'-joined (project.hpp:110-115's A20 rule -- ALWAYS '/', on every
// OS, because root() is lexically_normal'd and every consumer accepts a mixed-separator path).
// Trailing separators on the root are stripped, so no double separator is ever produced.
// "" in, "" out.
[[nodiscard]] std::string projectStatePath(std::string_view projectRootUtf8);

// THIS IS NOT E.4.2's CONTAINMENT PREDICATE AND MUST NOT BE USED AS ONE (D8). It is PURELY LEXICAL:
// '\' unified to '/' on both sides, trailing separators stripped from the root, then a BYTE-WISE test
// that the scene begins with root + '/'. No symlink resolution, no case folding, no drive-letter rule.
// E.4.2's predicate LANDED and DELIBERATELY DID NOT absorb this one, so the invitation above is
// closed: that one answers "may this proceed?" and REFUSES on doubt; this one answers "what do I
// record?" and FORGETS on doubt, and a refusal where a forget belongs turns a lost bookmark into a
// blocked operation. directoryWithin cannot replace it either -- it returns a bool and takes a
// DIRECTORY, while this takes a FILE and returns the REMAINDER. And this runs once per tick from
// EditorApp::persistProjectState, where resolveSceneContainment's canonical rescue would be two
// filesystem calls EVERY TICK for as long as an out-of-project scene is open.
//
// Its failure mode is "FORGET", never "wrong project": a false negative records "" and the user gets a
// new scene; a false positive records a relative path that either does not exist on the next open (the
// cascade arm) or names the same file anyway.
//
// THE REMAINDER IS ALSO GATED ON isLegalRelativePath (plan R25). The producer must guarantee what
// parseProjectState requires, or this build can write a document it cannot itself read back -- one
// WARN and a forgotten position, on a round trip that never left this process.
//
// "" when either argument is empty, when the scene is not lexically inside the root, when the scene IS
// the root, or when the remainder is not a legal relative path.
[[nodiscard]] std::string projectRelativeScenePath(std::string_view projectRootUtf8,
                                                   std::string_view absoluteScenePathUtf8);

// The inverse join. "" when either argument is empty or `relativeUtf8` is not isLegalRelativePath.
[[nodiscard]] std::string absoluteScenePath(std::string_view projectRootUtf8, std::string_view relativeUtf8);

// ---- the two file operations (the ONLY disk-touching functions here) ------------------------------

// A MISSING file is a default state, SILENTLY (corrupt = false) -- the normal state on a machine that
// has never opened this project. A file that EXISTS and cannot be READ (a directory, or one the OS
// refuses), or that does not parse, or that carries a wrong version, is a default state PLUS
// corrupt = true. The two must never be conflated, or every first open warns.
// `fileExists` is the discriminator, and it is std::filesystem::exists, so it is TRUE for a DIRECTORY
// -- which is what makes a directory the portable stand-in for a permission-refused file in a test
// (editor_prefs.hpp's EP13 precedent).
// An EMPTY root reads nothing and sets corrupt = false.
[[nodiscard]] ProjectState readProjectState(std::string_view projectRootUtf8, bool& corrupt);

// "" on success, the OS reason otherwise. In order: ensureDirectory(<root>/Library); write
// Library/.gitignore IF ABSENT, never overwriting one (asset_database.cpp:845-851's D6/E35 rule, whose
// TEXT and NAMES are imported from asset_cache.hpp rather than restated); then ONE
// writeTextFileAtomic with the path as a NAMED LOCAL on the preceding line (INV-A1).
// A .gitignore failure does NOT block the state write and is not reported -- the state file is the
// point, and the next rescan writes the ignore file anyway.
// An EMPTY root writes nothing and returns "" -- "this instance has no project" is not a failure.
[[nodiscard]] std::string writeProjectState(std::string_view projectRootUtf8, const ProjectState& state);

// ---- the startup-scene resolution ----------------------------------------------------------------

// ASCII-case-folded SUFFIX test on a LEAF name, the isMetaFileName / isBlendFileName /
// isImportableModelName shape: SOMETHING MUST PRECEDE THE EXTENSION. ".scene.json" alone is FALSE --
// it is not a scene "of" anything, exactly as asset_meta.cpp:172 says of ".meta" and
// blender_tool.cpp:48-49 says of ".blend". "scene.json" is FALSE too (it does not carry the suffix at
// all). Case-folded because a case-insensitive volume can serve "Level.SCENE.JSON".
[[nodiscard]] bool isSceneFileName(std::string_view fileNameUtf8) noexcept;

// The first *.scene.json FILE directly under <root>/<scenesRelative>, as a ROOT-relative path.
// NON-RECURSIVE (D7): one listDirectory call, hidden entries excluded, entryOrderLess order, the
// first entry that is NOT a directory and whose leaf isSceneFileName. Directories sort FIRST
// (project_files.hpp:109-113), so the !isDirectory test is load-bearing and not defensive.
// "" when there is none, when the listing's status is not Ok, or when either argument is empty.
// listingIsComplete is deliberately NOT consulted: this caller picks a file to READ, and a truncated
// listing still yields a real scene (project_files.hpp:90-102 draws exactly that line).
// The result is joinRelative(scenesRelative, leaf), VERBATIM -- so a scenesPath of "." legitimately
// yields "./a.scene.json", which is isLegalRelativePath, re-resolves through absoluteScenePath to the
// same file, and is NOT special-cased. A second rule for a cosmetic gain is a second rule.
// AN ENTRY WHOSE JOINED RESULT IS NOT isLegalRelativePath IS SKIPPED AND THE SCAN CONTINUES (plan
// R25, code review): `entry.name` is an OS-supplied leaf, and ':' and '\' are legal POSIX filename
// bytes this tree's relative-path rule refuses anywhere -- so "boss:arena.scene.json" would otherwise
// be returned verbatim and then answer "" from absoluteScenePath, which reaches openSceneFile as an
// EMPTY path and spends the restore's one and only attempt on nothing. The producer must guarantee
// what the consumer requires; projectRelativeScenePath already did.
[[nodiscard]] std::string firstSceneUnder(std::string_view projectRootUtf8, std::string_view scenesRelativeUtf8);

enum class StartupScene : std::uint8_t { NewScene = 0, Recorded, FirstUnderScenes };

struct StartupSceneFacts {
    bool recorded = false;         // ProjectState::lastSceneRecorded
    bool recordedEmpty = true;     // ProjectState::lastScene.empty()
    bool recordedExists = false;   // fileExists(<root>/<lastScene>) -- TRUE for a directory (F8)
    bool firstSceneFound = false;  // firstSceneUnder() returned a name
};

// PURE. Four bools in, one enum out -- a 16-row truth table with no arm without a case.
//   !recorded                                  -> firstSceneFound ? FirstUnderScenes : NewScene
//   recorded && recordedEmpty                  -> NewScene                       (D2's third state)
//   recorded && !empty && recordedExists       -> Recorded
//   recorded && !empty && !recordedExists      -> firstSceneFound ? FirstUnderScenes : NewScene
// `recordedExists` is read in the FIRST arm and nowhere else, so the four !recorded rows ignore it
// completely -- PJ25 asserts that, because a decider that reads a fact it should ignore is a real
// defect, and it is why the caller computes firstSceneUnder UNCONDITIONALLY rather than feeding this
// function a false-but-unread fact.
// IT DOES NOT MODEL "the candidate failed to open" AT ALL, and that is D6: the caller opens the
// candidate once and never re-enters this function. One attempt, always.
[[nodiscard]] StartupScene chooseStartupScene(const StartupSceneFacts& facts) noexcept;

// ---- the write decision ---------------------------------------------------------------------------

enum class ProjectStateStep : std::uint8_t { Nothing = 0, AdoptBaseline, Write };

// PURE. The end-of-tick reconcile, as a decision:
//   currentRoot != baselineRoot                     -> AdoptBaseline  (a project OPEN, SWAP or CLOSE:
//                                                                      the state was READ, not changed)
//   currentRoot.empty()                             -> Nothing        (no project, nothing to record)
//   currentScene != baselineScene                   -> Write
//   otherwise                                       -> Nothing
// `currentScene`/`baselineScene` are the RECORDED (project-relative) form, never the absolute path --
// passing the absolute one both defeats D3 and makes two different out-of-project scenes look like a
// change (sabotage seed S4).
[[nodiscard]] ProjectStateStep projectStateStep(std::string_view currentRoot, std::string_view baselineRoot,
                                                std::string_view currentScene, std::string_view baselineScene) noexcept;

}  // namespace engine::editor

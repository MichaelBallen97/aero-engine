#pragma once
// Aero Engine — the impure driver of the Blender CLI integration (task 3.2.4). PUBLIC, and it names
// NO SDL TYPE AT ALL: the SDL_Process lives behind blender_process.hpp, which is src-private, and
// reaches this header only as an INCOMPLETE type through a named-deleter unique_ptr.
//
// ONE state machine. AT MOST ONE live child process (INV-B5). Polled once per tick.
// NOTHING HERE LOGS (INV-B10) -- every status is RETURNED or exposed as an accessor, and the ONE
// place this task logs is editor_app.cpp.
//
// EVERY PROCESS SPAWN IN THIS TASK HAPPENS INSIDE poll() (INV-B15). resolve() stats candidate paths
// and spawns nothing; setOverridePath() writes a preferences file and spawns nothing;
// requestConversion() records a request and spawns nothing. That is what makes exportRunCount() and
// probeRunCount() statements about ONE function rather than about four.
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/blender_tool.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace engine::editor {

class BlenderProcess;  // src-private (editor/src/blender_process.hpp) -- INCOMPLETE here, on purpose

enum class BlenderState : std::uint8_t {
    Unknown = 0,   // not yet resolved this session
    Probing,       // a binary is resolved; `blender --version` is pending or running
    Ready,         // resolved + probed + support != Refused
    ToolMissing,   // no candidate exists on disk
    ToolUnusable,  // found, but --version failed, or the version is Refused
    Converting,    // an export is running
    // THE RUN EXITED, ITS STATUS FILE EXISTS AND REPORTS ok: true. THE ARTIFACT HAS NOT BEEN IMPORTED
    // YET -- ModelImportSession reads it, imports it, and either writes the provenance record (this
    // state stands) or calls noteArtifactUnusable(). The service alone cannot know, because only the
    // session imports.
    Converted,
    Failed,  // see failure() / message()
};

enum class BlenderFailure : std::uint8_t {
    None = 0,
    SpawnFailed,
    SourceRejected,
    ExportFailed,
    ArtifactUnusable,
    TimedOut,
    Cancelled,
};

// The ONE impure input-gathering function: the environment for AERO_BLENDER_PATH / PATH /
// HOME|USERPROFILE / PROGRAMFILES / LOCALAPPDATA, plus readTextFile on the preferences path. PATH is
// split on ';' for HostOs::Windows and ':' otherwise. Its RESULT IS INJECTABLE, which is what makes
// every candidate-list and transition case tier-0 with no environment manipulation anywhere.
//
// A MISSING preferences file is empty preferences, SILENTLY (AC-8). A corrupt one is empty
// preferences PLUS `prefsCorrupt = true` -- an OUT FLAG, never a log line (INV-B10): editor_app.cpp
// emits the one WARN.
[[nodiscard]] BlenderEnv readBlenderEnv(HostOs os, std::string_view prefsPathUtf8, bool& prefsCorrupt);

// PIMPL over an INCOMPLETE type, and the shape below is load-bearing rather than stylistic.
// BlenderService is a value member of ModelImportSession, whose header already carries
// `static_assert(std::is_nothrow_move_assignable_v<ModelImportSession>)`. That assertion INSTANTIATES
// this class's move assignment IN THAT HEADER, which instantiates unique_ptr's, which CALLS THE
// DELETER -- and the deleter needs BlenderProcess complete, which it is not there. A plain
// std::unique_ptr<BlenderProcess> therefore fails to compile inside a file that never mentions
// Blender. A NAMED deleter DECLARED here and DEFINED in blender_service.cpp (where BlenderProcess is
// complete), plus an out-of-line destructor and out-of-line moves, is what fixes it. A
// defaulted-in-header special member reintroduces exactly the same problem.
struct BlenderProcessDeleter {
    void operator()(BlenderProcess* process) const noexcept;
};

class BlenderService {
public:
    BlenderService();
    ~BlenderService();
    BlenderService(BlenderService&&) noexcept;
    BlenderService& operator=(BlenderService&&) noexcept;
    BlenderService(const BlenderService&) = delete;
    BlenderService& operator=(const BlenderService&) = delete;

    // ---- resolution: impure but PROCESS-FREE. Stats candidates; SPAWNS NOTHING (INV-B15). ----
    //
    // `exportDirUtf8` is the machine-local directory this service may write into --
    // <projectRoot>/Library/BlenderExports. It is a parameter of RESOLUTION rather than only of a
    // conversion because the version probe is itself a child process, and a child process's output
    // goes to a FILE and never to a pipe (INV-B4: SDL's own header documents that a piped child can
    // block forever waiting to be read, and this design never reads one). The probe therefore needs
    // somewhere to write BEFORE any asset GUID exists. With an empty directory the state machine
    // still resolves, but poll() will not spawn the probe -- it has nowhere to put the answer.
    void resolve(HostOs os, const BlenderEnv& env, std::string_view exportDirUtf8);

    // Locate... / Re-detect. Writes the tool-preferences file (the ONE tool-prefs writeTextFileAtomic
    // site, AC-25) and resets state to Unknown so the next resolve() re-runs. Spawns nothing, and
    // kills any live child rather than letting its completion be attributed to a new configuration.
    // "" == success; the OS reason otherwise.
    [[nodiscard]] std::string setOverridePath(std::string_view absolutePathUtf8, std::string_view prefsPathUtf8);

    [[nodiscard]] const std::string& binaryPath() const noexcept;
    [[nodiscard]] const std::vector<std::string>& searchedPaths() const noexcept;  // for the message
    [[nodiscard]] const std::optional<BlenderVersion>& version() const noexcept;
    // "" until something has probed. This is the provenance record's blenderVersion field AND the
    // gate §A-9 makes conditional -- an empty value means "nothing has been probed", so a cache hit
    // compares nothing and spawns nothing. It is the version STRING as Blender prints it
    // ("5.2.0 LTS"), which is also what bpy.app.version_string reports from inside the script.
    [[nodiscard]] const std::string& versionString() const noexcept;

    // ---- conversion: a one-shot request, drained by poll(). ----
    void requestConversion(Guid guid, std::string blendAbsolutePath, ContentHash sourceHash, std::string exportDirUtf8);
    void cancel() noexcept;  // kills GRACEFULLY at once; poll() owns the force escalation
    // The session's verdict on the artifact, coming back in. Moves Converted -> Failed /
    // ArtifactUnusable with `message`. Ignored in every other state: only a Converted run has an
    // artifact to judge, and only the session can judge it, because only the session imports.
    void noteArtifactUnusable(std::string message);

    // THE ONE IMPURE TICK ENTRY POINT, AND THE ONLY SPAWN SITE IN THIS TASK (INV-B15).
    // EXACTLY ONE CALL SITE IN THE TREE: ModelImportSession::service(), which itself runs only from
    // EditorApp::tick()'s post-draw slot. NEVER from onDraw().
    void poll(float deltaSeconds);

    [[nodiscard]] BlenderState state() const noexcept;
    [[nodiscard]] BlenderFailure failure() const noexcept;
    [[nodiscard]] const std::string& message() const noexcept;  // never empty in a failure state
    [[nodiscard]] float elapsedSeconds() const noexcept;
    [[nodiscard]] const std::string& artifactPath() const noexcept;  // "" unless Converted
    [[nodiscard]] const std::string& logPath() const noexcept;
    [[nodiscard]] const std::string& logTail() const noexcept;  // "" when refused by the cap
    [[nodiscard]] std::uint64_t logSizeBytes() const noexcept;  // shown when refusedByCap (AC-35)
    [[nodiscard]] bool logRefusedByCap() const noexcept;
    // SPLIT (§A-9). AC-22 asserts BOTH are zero on a cache hit -- "zero processes" is unsatisfiable
    // as one number, because the version probe is a process too.
    [[nodiscard]] std::size_t exportRunCount() const noexcept;
    [[nodiscard]] std::size_t probeRunCount() const noexcept;

private:
    void startProbe();
    void startExport();
    void finishRun(int exitCode);
    void finishProbe(int exitCode);
    void finishExport(int exitCode);
    void loadLogTail();
    void clearProcess() noexcept;

    std::unique_ptr<BlenderProcess, BlenderProcessDeleter> process;  // INCOMPLETE type here
    // INV-B13: sorted std::vectors and plain strings only -- no std::unordered_map, no std::set. This
    // type is reachable from EditorApp's defaulted noexcept move, and MSVC's node-based containers are
    // not nothrow-move-CONSTRUCTIBLE (3.1.2's R9, measured in CI as C2607).
    std::vector<std::string> searched;
    std::string binary;
    std::string versionText;
    std::string messageText;
    std::string artifact;
    std::string logFile;
    std::string logTailText;
    std::string statusFile;
    std::string exportDir;
    std::string blendPath;
    std::optional<BlenderVersion> parsedVersion;
    Guid targetGuid;
    ContentHash targetHash;
    float elapsed = 0.0F;
    float sinceKillRequest = 0.0F;
    std::uint64_t logBytes = 0;
    std::size_t exportRuns = 0;
    std::size_t probeRuns = 0;
    BlenderState stateValue = BlenderState::Unknown;
    BlenderFailure failureValue = BlenderFailure::None;
    // The verdict a cancel or a timeout has already decided, carried to the moment the child actually
    // exits -- the exit code of a killed process says nothing useful, and on Windows a graceful kill
    // is indistinguishable from a forceful one (R5).
    BlenderFailure pendingFailure = BlenderFailure::None;
    bool conversionRequested = false;
    bool killRequested = false;
    bool logRefused = false;
};

// The aggregate-first assert pair (command_stack.hpp's shape, a SIXTH application), so a regression
// NAMES this type instead of failing an opaque member's assert three headers away.
static_assert(std::is_nothrow_move_constructible_v<BlenderService>);
static_assert(std::is_nothrow_move_assignable_v<BlenderService>);

}  // namespace engine::editor

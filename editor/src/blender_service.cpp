// Aero Engine — blender_service.cpp: the Blender state machine (task 3.2.4). It reads the
// environment and the tool preferences, stats candidate paths, and drives BlenderProcess -- but it
// names SDL only for the environment lookup and NEVER for a process (that is blender_process.cpp's
// sole job).
//
// NOTHING HERE LOGS (INV-B10): editor_app.cpp emits this task's three log lines and nothing else
// does. Nothing here removes, renames or copies a file (INV-B9) -- this TU is in NEITHER of
// check-project-no-delete.sh's two lists, which is exactly what makes a future deletion written here
// a hard CI failure. (Do not spell the delete/rename tokens in prose: INV-B9's own gate grep does not
// strip comments, the AC-5 rule a third time.)
//
// No <thread>, <mutex>, <atomic>, <future> or <condition_variable> anywhere (INV-B7). The OS already
// runs the child concurrently; a per-tick non-blocking wait is the whole mechanism, and a thread
// would buy nothing while costing a mutex, a cross-thread channel and a shutdown-ordering problem.
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/blender_service.hpp>
#include <aero/editor/blender_tool.hpp>
#include <aero/editor/text_file.hpp>

#include "blender_process.hpp"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
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

// The version probe's own redirect target. It sits beside the per-asset artifacts rather than in a
// temp directory because everything this task derives belongs in one machine-local, gitignored place
// a user can delete wholesale -- and because a temp directory is one more thing to get right on three
// platforms for no gain.
constexpr std::string_view PROBE_LOG_FILE_NAME = "blender-version.log";

[[nodiscard]] std::string joinPath(std::string_view directory, std::string_view leaf) {
    std::string path(directory);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path += leaf;
    return path;
}

[[nodiscard]] std::string readEnvVar(const char* name) {
    const char* const value = SDL_getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

// PATH is ';'-separated on Windows and ':'-separated everywhere else. EMPTY ENTRIES ARE KEPT HERE and
// dropped by blenderCandidatePaths (E2) -- splitting and filtering are deliberately different jobs, so
// the pure function owns the whole policy and this one owns only the separator.
[[nodiscard]] std::vector<std::string> splitPathList(std::string_view list, HostOs os) {
    const char separator = os == HostOs::Windows ? ';' : ':';
    std::vector<std::string> entries;
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t at = list.find(separator, start);
        const std::size_t end = at == std::string_view::npos ? list.size() : at;
        entries.emplace_back(list.substr(start, end - start));
        if (at == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return entries;
}

// The first line, '\r' stripped, with Blender's own "Blender " banner prefix removed when present --
// so "Blender 5.2.0 LTS" becomes "5.2.0 LTS", which is exactly what bpy.app.version_string reports
// from INSIDE the script and therefore what the provenance record can be compared against (§W-9).
// A line that does not carry the banner is kept whole: D14 attempts, it never refuses.
[[nodiscard]] std::string versionStringFrom(std::string_view versionOutput) {
    const std::size_t lineEnd = versionOutput.find('\n');
    std::string_view line = versionOutput.substr(0, lineEnd == std::string_view::npos ? versionOutput.size() : lineEnd);
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    constexpr std::string_view PREFIX = "Blender ";
    if (line.size() > PREFIX.size() && line.substr(0, PREFIX.size()) == PREFIX) {
        line.remove_prefix(PREFIX.size());
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.remove_suffix(1);
    }
    return std::string(line);
}

}  // namespace

void BlenderProcessDeleter::operator()(BlenderProcess* process) const noexcept {
    // Defined HERE, where BlenderProcess is COMPLETE. Declaring it in the header and defining it here
    // is the whole of the PIMPL trap's fix -- see blender_service.hpp.
    delete process;  // NOLINT(cppcoreguidelines-owning-memory) -- the one sanctioned delete: it IS the deleter
}

BlenderEnv readBlenderEnv(HostOs os, std::string_view prefsPathUtf8, bool& prefsCorrupt) {
    prefsCorrupt = false;
    BlenderEnv env;

    if (!prefsPathUtf8.empty()) {
        const FileReadResult prefs = readTextFile(prefsPathUtf8);
        if (prefs.text.has_value()) {
            // A file that EXISTS but does not parse is empty preferences PLUS the out-flag. A file
            // that does not exist is empty preferences SILENTLY (AC-8) -- the two must not be
            // conflated, or every machine without a chosen Blender gets a warning on every resolve.
            const std::optional<ToolPrefs> parsed = parseToolPrefs(*prefs.text);
            if (parsed.has_value()) {
                env.overridePath = parsed->blenderPath;
            } else {
                prefsCorrupt = true;
            }
        }
    }

    env.envPath = readEnvVar("AERO_BLENDER_PATH");
    env.pathEntries = splitPathList(readEnvVar("PATH"), os);
    env.homeDir = readEnvVar(os == HostOs::Windows ? "USERPROFILE" : "HOME");
    env.programFiles = readEnvVar("PROGRAMFILES");
    env.localAppData = readEnvVar("LOCALAPPDATA");
    return env;
}

BlenderService::BlenderService() = default;
BlenderService::~BlenderService() = default;
BlenderService::BlenderService(BlenderService&&) noexcept = default;
BlenderService& BlenderService::operator=(BlenderService&&) noexcept = default;

void BlenderService::clearProcess() noexcept {
    process.reset();  // ~BlenderProcess kills if still running, THEN destroys (F1)
    killRequested = false;
    sinceKillRequest = 0.0F;
}

void BlenderService::resolve(HostOs os, const BlenderEnv& env, std::string_view exportDirUtf8) {
    exportDir = std::string(exportDirUtf8);
    searched = blenderCandidatePaths(os, env);
    binary.clear();
    parsedVersion.reset();
    versionText.clear();
    failureValue = BlenderFailure::None;

    // The ONE impure thing resolution does, and it SPAWNS NOTHING (INV-B15): one stat per candidate,
    // first hit wins. The execute bit is required everywhere except Windows, which has none -- and the
    // CALLER decides that from HostOs, which is what keeps text_file.cpp free of the preprocessor.
    const bool requireExecBit = os != HostOs::Windows;
    for (const std::string& candidate : searched) {
        if (isExecutableFile(candidate, requireExecBit)) {
            binary = candidate;
            break;
        }
    }

    if (binary.empty()) {
        stateValue = BlenderState::ToolMissing;
        messageText = std::format("Blender was not found. {} location(s) were searched.", searched.size());
        return;  // searchedPaths() is RETAINED, so the panel can list every path it looked in (AC-30)
    }
    stateValue = BlenderState::Probing;
    messageText.clear();
}

std::string BlenderService::setOverridePath(std::string_view absolutePathUtf8, std::string_view prefsPathUtf8) {
    ToolPrefs prefs;
    prefs.blenderPath = std::string(absolutePathUtf8);
    // Named local FIRST, always (AC-25): this is the ONE writeTextFileAtomic call in the whole tree
    // built from the tool-preferences path, and the invariant stays grep-decidable because of it.
    const std::string prefsPath(prefsPathUtf8);
    // NOT const: a const local defeats the implicit move on `return`, which
    // performance-no-automatic-move rejects on the Linux lane.
    if (std::string reason = writeTextFileAtomic(prefsPath, writeToolPrefsText(prefs)); !reason.empty()) {
        return reason;
    }
    // Back to Unknown so the NEXT resolve() re-runs against the new preference. Any live child is
    // killed rather than left to finish, or its completion would be attributed to a configuration it
    // was not started under.
    clearProcess();
    stateValue = BlenderState::Unknown;
    failureValue = BlenderFailure::None;
    messageText.clear();
    binary.clear();
    parsedVersion.reset();
    versionText.clear();
    conversionRequested = false;
    pendingFailure = BlenderFailure::None;
    return {};
}

const std::string& BlenderService::binaryPath() const noexcept { return binary; }
const std::vector<std::string>& BlenderService::searchedPaths() const noexcept { return searched; }
const std::optional<BlenderVersion>& BlenderService::version() const noexcept { return parsedVersion; }
const std::string& BlenderService::versionString() const noexcept { return versionText; }

void BlenderService::requestConversion(Guid guid, std::string blendAbsolutePath, ContentHash sourceHash,
                                       std::string exportDirUtf8) {
    // E15: a run already in flight makes this a NO-OP, and it is refused HERE rather than at the
    // drain. Refusing at the drain would make the property depend on whether the child happened to
    // exit within the same tick -- a race that would pass on a fast machine and fail on a slow one.
    if (stateValue == BlenderState::Converting) {
        return;
    }
    // RECORDS ONLY. Spawns nothing, writes nothing, stats nothing (INV-B15). poll() drains it, and
    // poll() is the only place a process is ever created.
    targetGuid = guid;
    blendPath = std::move(blendAbsolutePath);
    targetHash = sourceHash;
    if (!exportDirUtf8.empty()) {
        exportDir = std::move(exportDirUtf8);
    }
    conversionRequested = true;
}

void BlenderService::cancel() noexcept {
    if (stateValue != BlenderState::Converting || process == nullptr || !process->running()) {
        conversionRequested = false;  // a request not yet drained is withdrawn rather than honoured
        return;
    }
    // GRACEFUL at once, so the user sees an immediate response. poll() owns the escalation to a
    // forceful kill after PROCESS_FORCE_KILL_SECONDS -- which matters only on POSIX, because on
    // Windows the first kill is already forceful (R5).
    process->kill(false);
    killRequested = true;
    sinceKillRequest = 0.0F;
    pendingFailure = BlenderFailure::Cancelled;
}

void BlenderService::noteArtifactUnusable(std::string message) {
    if (stateValue != BlenderState::Converted) {
        return;  // only a Converted run has an artifact to judge
    }
    stateValue = BlenderState::Failed;
    failureValue = BlenderFailure::ArtifactUnusable;
    messageText = std::move(message);
    artifact.clear();
}

void BlenderService::loadLogTail() {
    logTailText.clear();
    logBytes = 0;
    logRefused = false;
    if (logFile.empty()) {
        return;
    }
    // readFileBytes refuses ANYTHING over the cap from file_size ALONE -- it never opens such a file
    // (AC-35). `size` is filled EVEN ON REFUSAL, so the panel can show the byte count and the absolute
    // path instead of a partial read presented as the whole log.
    const FileBytesResult read = readFileBytes(logFile, MAX_TOOL_LOG_BYTES);
    logBytes = read.size;
    logRefused = read.refusedByCap;
    if (read.bytes.has_value()) {
        logTailText = *read.bytes;
    }
}

void BlenderService::startProbe() {
    logFile = joinPath(exportDir, PROBE_LOG_FILE_NAME);
    const std::string directoryError = ensureDirectory(exportDir);
    if (!directoryError.empty()) {
        stateValue = BlenderState::ToolUnusable;
        messageText =
            std::format("Could not prepare '{}' for the Blender version check -- {}", exportDir, directoryError);
        return;
    }

    auto probe = std::unique_ptr<BlenderProcess, BlenderProcessDeleter>(new BlenderProcess());
    const std::vector<std::string> args = buildVersionArgs(binary);
    const std::string failureReason = probe->start(args, exportDir, logFile);
    ++probeRuns;  // counted whether it started or not: an ATTEMPT is what AC-22 forbids on a cache hit
    if (!failureReason.empty()) {
        stateValue = BlenderState::ToolUnusable;
        failureValue = BlenderFailure::SpawnFailed;
        messageText = std::format("'{}' could not be run -- {}", binary, failureReason);
        return;
    }
    process = std::move(probe);
    elapsed = 0.0F;
}

void BlenderService::startExport() {
    const std::string directoryError = ensureDirectory(exportDir);
    if (!directoryError.empty()) {
        stateValue = BlenderState::Failed;
        failureValue = BlenderFailure::SpawnFailed;
        messageText = std::format("Could not prepare '{}' for the export -- {}", exportDir, directoryError);
        return;
    }

    // Named local FIRST (AC-25): this and the provenance write are the task's TWO
    // writeTextFileAtomic calls built from the library directory.
    const std::string scriptPath = joinPath(exportDir, BLENDER_SCRIPT_FILE_NAME);
    // WRITE ONLY WHAT DIFFERS (D6/INV-B12, a third application): a run whose script is already the
    // current text writes ZERO BYTES. Without this, every conversion would dirty a file for nothing
    // and BS31's "not one byte under the project changed" would be unachievable.
    const FileReadResult currentScript = readTextFile(scriptPath);
    if (!currentScript.text.has_value() || *currentScript.text != blenderExportScriptText()) {
        const std::string writeError = writeTextFileAtomic(scriptPath, blenderExportScriptText());
        if (!writeError.empty()) {
            stateValue = BlenderState::Failed;
            failureValue = BlenderFailure::SpawnFailed;
            messageText = std::format("Could not write '{}' -- {}", scriptPath, writeError);
            return;
        }
    }

    const std::string guidText = formatGuid(targetGuid);  // returns BY VALUE -- named local first
    artifact = joinPath(exportDir, guidText + ".glb");
    statusFile = joinPath(exportDir, guidText + ".json");
    logFile = joinPath(exportDir, guidText + ".log");

    auto run = std::unique_ptr<BlenderProcess, BlenderProcessDeleter>(new BlenderProcess());
    const std::vector<std::string> args = buildExportArgs(binary, blendPath, scriptPath, artifact, statusFile);
    const std::string failureReason = run->start(args, exportDir, logFile);
    ++exportRuns;
    if (!failureReason.empty()) {
        stateValue = BlenderState::Failed;
        failureValue = BlenderFailure::SpawnFailed;
        messageText = std::format("Blender could not be started -- {}", failureReason);
        artifact.clear();
        return;
    }
    process = std::move(run);
    stateValue = BlenderState::Converting;
    failureValue = BlenderFailure::None;
    pendingFailure = BlenderFailure::None;
    messageText.clear();
    elapsed = 0.0F;
}

void BlenderService::finishProbe(int exitCode) {
    if (exitCode != 0) {
        stateValue = BlenderState::ToolUnusable;
        failureValue = BlenderFailure::SpawnFailed;
        messageText = std::format("'{}' exited with code {} when asked for its version.", binary, exitCode);
        return;
    }
    parsedVersion = parseBlenderVersion(logTailText);
    versionText = versionStringFrom(logTailText);

    const BlenderSupport support = blenderSupport(parsedVersion);
    if (support == BlenderSupport::Refused) {
        stateValue = BlenderState::ToolUnusable;
        failureValue = BlenderFailure::None;
        messageText = std::format("Blender {} is too old: {}.{}.{} or newer is required.", versionText,
                                  BLENDER_ABSOLUTE_MIN.major, BLENDER_ABSOLUTE_MIN.minor, BLENDER_ABSOLUTE_MIN.patch);
        return;
    }
    // D14/E4: an UNPARSEABLE version reaches here as Supported and is ATTEMPTED, never refused -- it
    // is far likelier to be a locale, a build suffix or a fork than a Blender from 2011.
    stateValue = BlenderState::Ready;
    failureValue = BlenderFailure::None;
    messageText = support == BlenderSupport::Warned
                      ? std::format("Blender {} is older than the recommended {}.{}; the export may differ.",
                                    versionText, BLENDER_MIN_SUPPORTED.major, BLENDER_MIN_SUPPORTED.minor)
                      : std::string();
}

void BlenderService::finishExport(int exitCode) {
    // A cancel or a timeout has ALREADY decided the verdict. The exit code of a killed process says
    // nothing useful, and on Windows a graceful kill is indistinguishable from a forceful one (R5),
    // so it must not be consulted here at all.
    if (pendingFailure == BlenderFailure::Cancelled) {
        stateValue = BlenderState::Failed;
        failureValue = BlenderFailure::Cancelled;
        messageText = "The conversion was cancelled.";
        artifact.clear();
        return;
    }
    if (pendingFailure == BlenderFailure::TimedOut) {
        stateValue = BlenderState::Failed;
        failureValue = BlenderFailure::TimedOut;
        messageText = std::format("Blender did not finish within {:.0f} seconds and was stopped. Its log is at '{}'.",
                                  BLENDER_TIMEOUT_SECONDS, logFile);
        artifact.clear();
        return;
    }

    // THE STATUS FILE, WHEN IT EXISTS, IS THE AUTHORITY. It is written by the script itself, so it
    // reports what the exporter actually did rather than what an exit code implies.
    std::optional<ExportStatus> status;
    if (const FileReadResult read = readTextFile(statusFile); read.text.has_value()) {
        status = parseExportStatus(*read.text);
    }

    if (exitCode != 0 && !status.has_value()) {
        // MEASURED (F4): Blender aborts at about 0.4 s, BEFORE running --python, when it cannot open
        // the .blend -- so "non-zero exit AND no status file" is a real, reliable discriminator for
        // "Blender rejected the file itself", and it must say something different from "the export
        // failed" (AC-31).
        stateValue = BlenderState::Failed;
        failureValue = BlenderFailure::SourceRejected;
        messageText = std::format(
            "Blender could not open '{}' (it exited with code {} before the export script ran). Its log is at '{}'.",
            blendPath, exitCode, logFile);
        artifact.clear();
        return;
    }
    if (status.has_value() && !status->ok) {
        stateValue = BlenderState::Failed;
        failureValue = BlenderFailure::ExportFailed;
        messageText = status->error.empty() ? std::format("The Blender export failed. Its log is at '{}'.", logFile)
                                            : status->error;  // AC-32/AC-33: the status file's own message WINS
        artifact.clear();
        return;
    }
    if (!status.has_value()) {
        // exit 0 with no status file at all (E23): the script never got far enough to write one, so
        // NAME THE MISSING FILE rather than reporting a success nothing produced.
        stateValue = BlenderState::Failed;
        failureValue = BlenderFailure::ExportFailed;
        messageText = std::format("Blender exited successfully but wrote no status file at '{}'. Its log is at '{}'.",
                                  statusFile, logFile);
        artifact.clear();
        return;
    }
    if (exitCode != 0) {
        // exit != 0 WITH a status file: the status message wins (AC-32).
        stateValue = BlenderState::Failed;
        failureValue = BlenderFailure::ExportFailed;
        messageText = status->error.empty()
                          ? std::format("Blender exited with code {}. Its log is at '{}'.", exitCode, logFile)
                          : status->error;
        artifact.clear();
        return;
    }

    // The run exited, its status file exists, and it reports ok: true. THE ARTIFACT HAS NOT BEEN READ
    // OR IMPORTED YET -- ModelImportSession does that and then either writes the provenance record or
    // calls noteArtifactUnusable(). The service alone cannot know, because only the session imports.
    stateValue = BlenderState::Converted;
    failureValue = BlenderFailure::None;
    messageText.clear();
}

void BlenderService::finishRun(int exitCode) {
    loadLogTail();  // EVERY exit path loads it, so the panel always has something to show
    if (stateValue == BlenderState::Probing) {
        finishProbe(exitCode);
    } else if (stateValue == BlenderState::Converting) {
        finishExport(exitCode);
    }
    pendingFailure = BlenderFailure::None;
    clearProcess();
}

void BlenderService::poll(float deltaSeconds) {
    const float step = deltaSeconds > 0.0F ? deltaSeconds : 0.0F;  // a negative dt must never rewind

    if (process != nullptr) {
        if (process->running()) {
            elapsed += step;
            if (killRequested) {
                sinceKillRequest += step;
                if (sinceKillRequest > PROCESS_FORCE_KILL_SECONDS) {
                    process->kill(true);  // graceful first, forceful only after the grace period
                }
            } else if (stateValue == BlenderState::Converting && elapsed > BLENDER_TIMEOUT_SECONDS) {
                process->kill(false);
                killRequested = true;
                sinceKillRequest = 0.0F;
                pendingFailure = BlenderFailure::TimedOut;
            }
        }
        int exitCode = 0;
        // NON-BLOCKING, always. This is the entire concurrency design: the OS runs the child, and one
        // syscall per tick asks whether it is done.
        if (process->poll(exitCode) == ProcessState::Running) {
            return;  // still going -- nothing else happens this tick
        }
        finishRun(exitCode);
    }

    if (stateValue == BlenderState::Probing) {
        if (!exportDir.empty()) {
            startProbe();
        }
        return;  // a pending conversion waits for the probe's verdict rather than racing it
    }

    if (!conversionRequested) {
        return;
    }
    // AC-30/E15: a request is DROPPED without spawning when there is no usable Blender, and while a
    // conversion is already in flight. exportRunCount() must not move in either case.
    const bool convertible = stateValue == BlenderState::Ready || stateValue == BlenderState::Converted ||
                             stateValue == BlenderState::Failed;
    conversionRequested = false;
    if (convertible) {
        startExport();
    }
}

BlenderState BlenderService::state() const noexcept { return stateValue; }
BlenderFailure BlenderService::failure() const noexcept { return failureValue; }
const std::string& BlenderService::message() const noexcept { return messageText; }
float BlenderService::elapsedSeconds() const noexcept { return elapsed; }
const std::string& BlenderService::artifactPath() const noexcept { return artifact; }
const std::string& BlenderService::logPath() const noexcept { return logFile; }
const std::string& BlenderService::logTail() const noexcept { return logTailText; }
std::uint64_t BlenderService::logSizeBytes() const noexcept { return logBytes; }
bool BlenderService::logRefusedByCap() const noexcept { return logRefused; }
std::size_t BlenderService::exportRunCount() const noexcept { return exportRuns; }
std::size_t BlenderService::probeRunCount() const noexcept { return probeRuns; }

}  // namespace engine::editor

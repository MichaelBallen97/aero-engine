// Aero Engine -- script_transpiler.cpp: the after-scan esbuild driver (task 4.3.1).
//
// NOTHING HERE LOGS: EditorApp::drainScriptLog emits every line this file produces, as values out of takeLog().
// Nothing here removes, renames or copies a file -- this TU is in check-project-no-delete.sh's Check A list, which is
// what makes a future deletion written here a hard CI failure. Publishing a staged file is a read plus
// writeTextFileAtomic.
//
// No threads of any kind: the OS runs the child and one non-blocking poll per tick is the whole wait. The ONLY spawn
// site is service(); noteScan() evaluates and queues, it never starts a process. SDL is named for exactly two
// lookups, the environment and the executable's own directory (the blender_service.cpp precedent), and never for a
// process -- that is tool_process.cpp's sole job.
#include "script_transpiler.hpp"

#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/asset_cache.hpp>  // ASSET_CACHE_DIR_NAME
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/blender_tool.hpp>
#include <aero/editor/text_file.hpp>

#include <SDL3/SDL_filesystem.h>
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

[[nodiscard]] ScriptLogLevel levelOf(ScriptDiagnosticSeverity severity) noexcept {
    return severity == ScriptDiagnosticSeverity::Warning ? ScriptLogLevel::Warn : ScriptLogLevel::Error;
}

[[nodiscard]] std::string refusalText(const std::string& input, ScriptInputVerdict verdict) {
    switch (verdict) {
        case ScriptInputVerdict::PackageImport:
            return "'" + input + "' comes from a package (node_modules); bare imports are not supported";
        case ScriptInputVerdict::InsideLibrary:
            return "'" + input + "' is inside Library/";
        case ScriptInputVerdict::Accepted:
        case ScriptInputVerdict::OutsideAssetsRoot:
            break;
    }
    return "'" + input + "' is outside the assets root";
}

}  // namespace

ScriptTranspiler::ScriptTranspiler(ScriptTranspilerConfig settings) : config(std::move(settings)) {
    flagsFingerprint = esbuildFlagsFingerprint();
}

ScriptTranspiler::~ScriptTranspiler() = default;

const std::string& ScriptTranspiler::esbuildPath() const noexcept { return binaryPath; }

std::size_t ScriptTranspiler::runCount() const noexcept { return runs; }

bool ScriptTranspiler::idle() const noexcept { return queue.empty() && !running.has_value() && !passOpen; }

std::vector<ScriptLogLine> ScriptTranspiler::takeLog() noexcept {
    std::vector<ScriptLogLine> lines = std::move(pending);
    pending.clear();
    return lines;
}

void ScriptTranspiler::resetForProjectSwap() noexcept {
    process = ToolProcess{};
    queue.clear();
    running.reset();
    timedOut = false;
    elapsed = 0.0F;
    passOpen = false;
    outcomes.clear();
    freshAtLatest.clear();
    failedAtLatest.clear();
    view = ScriptProjectView{};
    assetsPrefix.clear();
}

std::string ScriptTranspiler::stagingDirectory() const {
    return joinPath(joinPath(view.projectRoot, ASSET_CACHE_DIR_NAME), SCRIPT_STAGING_DIR_NAME);
}

std::string ScriptTranspiler::artifactDirectory() const {
    return joinPath(joinPath(view.projectRoot, ASSET_CACHE_DIR_NAME), SCRIPT_ARTIFACT_DIR_NAME);
}

void ScriptTranspiler::note(ScriptLogLevel level, const std::string& text) {
    pending.push_back(ScriptLogLine{level, "scripts: " + text});
}

void ScriptTranspiler::resolveBinary() {
    if (binaryResolved) {
        return;
    }
    binaryResolved = true;
    const std::string envValue = config.esbuildPath.empty() ? readEnvVar("AERO_ESBUILD_PATH") : std::string();
    std::string base;
    if (config.esbuildPath.empty() && envValue.empty()) {
        // SDL-cached; must not be freed (project_file.cpp's rule).
        if (const char* const dir = SDL_GetBasePath(); dir != nullptr) {
            base = dir;
        }
    }
    binaryPath = resolveEsbuildPath(currentHostOs(), config.esbuildPath, envValue, base);
}

void ScriptTranspiler::learnIdentity() {
    if (!binaryIdentity.empty() || binaryPath.empty()) {
        return;
    }
    const FileHashResult hashed = hashFileContents(binaryPath);
    if (hashed.hash.has_value()) {
        binaryIdentity = formatContentHash(*hashed.hash);
    }
}

std::vector<ScriptTranspiler::ScriptJob> ScriptTranspiler::collectScripts(const AssetDatabase& database) const {
    std::vector<ScriptJob> scripts;
    for (const AssetRecord& record : database.records()) {
        if (!isScriptAssetName(record.relativePath) || !record.guid.valid() || !assetContentHashUsable(record)) {
            continue;
        }
        scripts.push_back(
            ScriptJob{record.guid, record.relativePath, projectRelativeOf(assetsPrefix, record.relativePath)});
    }
    return scripts;
}

std::optional<ContentHash> ScriptTranspiler::hashOfInput(const AssetDatabase& database,
                                                         std::string_view projectRelative) const {
    const std::string_view assetsRelative = assetsRelativeOf(projectRelative, assetsPrefix);
    if (assetsRelative.empty()) {
        return std::nullopt;
    }
    const AssetRecord* const record = database.findByPath(assetsRelative);
    if (record == nullptr || !assetContentHashUsable(*record)) {
        return std::nullopt;
    }
    return record->contentHash;
}

ScriptFreshnessContext ScriptTranspiler::freshnessContext(const AssetDatabase& database) const {
    return ScriptFreshnessContext{
        binaryIdentity,
        flagsFingerprint,
        scriptSourceSetFingerprint(database.records()),
        [this, &database](std::string_view path) { return hashOfInput(database, path); },
    };
}

std::optional<ScriptRecord> ScriptTranspiler::freshRecord(const ScriptJob& job,
                                                          const ScriptFreshnessContext& context) const {
    const std::string recordPath = joinPath(artifactDirectory(), formatGuid(job.guid) + ".json");
    const FileReadResult read = readTextFile(recordPath);
    if (!read.text.has_value()) {
        return std::nullopt;
    }
    std::optional<ScriptRecord> record = parseScriptRecord(*read.text);
    if (!record.has_value() || !scriptRecordFresh(*record, job.projectRelative, context)) {
        return std::nullopt;
    }
    if (record->status == ScriptRecordStatus::Failed) {
        return record;
    }
    const std::string stem = joinPath(artifactDirectory(), formatGuid(job.guid));
    if (!fileExists(stem + ".js") || !fileExists(stem + ".js.map")) {
        return std::nullopt;
    }
    return record;
}

void ScriptTranspiler::show(const std::string& script, const std::vector<ScriptDiagnostic>& lines) {
    for (const ScriptDiagnostic& line : lines) {
        note(levelOf(line.severity), formatScriptDiagnostic(line, script));
    }
}

bool ScriptTranspiler::hasOutcome(Guid guid) const noexcept {
    const auto at =
        std::lower_bound(outcomes.begin(), outcomes.end(), guid,
                         [](const std::pair<Guid, ScriptOutcome>& entry, Guid key) { return entry.first < key; });
    return at != outcomes.end() && at->first == guid;
}

void ScriptTranspiler::setOutcome(Guid guid, ScriptOutcome outcome) {
    const auto at =
        std::lower_bound(outcomes.begin(), outcomes.end(), guid,
                         [](const std::pair<Guid, ScriptOutcome>& entry, Guid key) { return entry.first < key; });
    if (at != outcomes.end() && at->first == guid) {
        at->second = outcome;
    } else {
        outcomes.insert(at, std::pair<Guid, ScriptOutcome>{guid, outcome});
    }
}

void ScriptTranspiler::enqueue(ScriptJob job) {
    for (ScriptJob& queued : queue) {
        if (queued.guid == job.guid) {
            queued = std::move(job);  // the same script, possibly moved since it was queued
            return;
        }
    }
    queue.push_back(std::move(job));
}

void ScriptTranspiler::endPass() {
    if (!passOpen) {
        return;
    }
    std::size_t transpiled = 0;
    std::size_t failed = 0;
    for (const std::pair<Guid, ScriptOutcome>& entry : outcomes) {
        (entry.second == ScriptOutcome::Transpiled ? transpiled : failed) += 1U;
    }
    // A failure that is still current is a failure at every pass: its stored lines go out again, after the lines of
    // any run and before the summary, once however many scans the pass spanned.
    for (const FreshFailure& entry : failedAtLatest) {
        if (!hasOutcome(entry.guid)) {
            show(entry.script, entry.diagnostics);
            ++failed;
        }
    }
    std::size_t upToDate = 0;
    for (const Guid guid : freshAtLatest) {
        upToDate += hasOutcome(guid) ? 0U : 1U;
    }
    note(ScriptLogLevel::Info, std::format("{} transpiled, {} failed, {} up to date", transpiled, failed, upToDate));
    outcomes.clear();
    freshAtLatest.clear();
    failedAtLatest.clear();
    passOpen = false;
}

void ScriptTranspiler::noteScan(const AssetDatabase& database, const ScriptProjectView& project) {
    if (project.projectRoot.empty() || project.language != ProjectLanguage::Ts) {
        return;
    }
    view = project;
    assetsPrefix = assetsPrefixOf(project.assetsPath);
    const std::vector<ScriptJob> scripts = collectScripts(database);
    if (scripts.empty() && !passOpen) {
        return;
    }
    passOpen = true;

    resolveBinary();
    learnIdentity();
    const ScriptFreshnessContext context = freshnessContext(database);

    freshAtLatest.clear();
    failedAtLatest.clear();
    for (const ScriptJob& job : scripts) {
        if (running.has_value() && running->guid == job.guid) {
            continue;  // finish() decides whether a scan that completed mid-run means another run
        }
        std::optional<ScriptRecord> fresh = freshRecord(job, context);
        if (!fresh.has_value()) {
            enqueue(job);
        } else if (fresh->status == ScriptRecordStatus::Failed) {
            failedAtLatest.push_back(FreshFailure{job.guid, job.projectRelative, std::move(fresh->diagnostics)});
        } else {
            const auto at = std::lower_bound(freshAtLatest.begin(), freshAtLatest.end(), job.guid);
            if (at == freshAtLatest.end() || !(*at == job.guid)) {
                freshAtLatest.insert(at, job.guid);
            }
        }
    }
    if (queue.empty() && !running.has_value()) {
        endPass();
    }
}

void ScriptTranspiler::service(const AssetDatabase& database, float deltaSeconds) {
    if (running.has_value()) {
        elapsed += std::max(deltaSeconds, 0.0F);
        if (!timedOut && elapsed > config.runLimitSeconds) {
            process.kill(true);
            timedOut = true;
        }
        int exitCode = 0;
        const ProcessState state = process.poll(exitCode);
        if (state == ProcessState::Running) {
            return;
        }
        if (state == ProcessState::Exited) {
            finish(database, exitCode);
        } else {
            failTool("esbuild at '" + binaryPath + "' stopped unexpectedly");
        }
    }

    while (!running.has_value() && !queue.empty()) {
        const ScriptJob job = queue.front();
        queue.erase(queue.begin());
        const AssetRecord* const record = database.findByGuid(job.guid);
        if (record == nullptr || record->relativePath != job.assetsRelative) {
            continue;  // gone or moved since it was queued: a later scan sees the new spelling
        }
        start(database, job);
        break;  // at most one start per call
    }

    if (!running.has_value() && queue.empty() && passOpen) {
        endPass();
    }
}

void ScriptTranspiler::failTool(const std::string& reason) {
    if (running.has_value()) {
        setOutcome(running->guid, ScriptOutcome::Failed);
        running.reset();
    }
    for (const ScriptJob& job : queue) {
        setOutcome(job.guid, ScriptOutcome::Failed);
    }
    queue.clear();
    process = ToolProcess{};
    timedOut = false;
    elapsed = 0.0F;
    note(ScriptLogLevel::Error, reason);
    endPass();
}

void ScriptTranspiler::start(const AssetDatabase& database, const ScriptJob& job) {
    running = job;  // failTool() counts it among the failed
    const bool requireExecBit = currentHostOs() != HostOs::Windows;
    if (binaryPath.empty() || !isExecutableFile(binaryPath, requireExecBit)) {
        failTool("esbuild was not found at '" + binaryPath + "'");
        return;
    }
    learnIdentity();
    if (binaryIdentity.empty()) {
        failTool("esbuild at '" + binaryPath + "' could not be read");
        return;
    }
    const std::string staging = stagingDirectory();
    if (const std::string reason = ensureDirectory(staging); !reason.empty()) {
        failTool("could not prepare '" + staging + "' -- " + reason);
        return;
    }
    const std::vector<std::string> args =
        buildEsbuildArgs(binaryPath, esbuildRunPaths(assetsPrefix, job.assetsRelative, job.guid));
    const std::string logPath = joinPath(staging, formatGuid(job.guid) + ".log");
    ++runs;
    if (const std::string reason = process.start(args, view.projectRoot, logPath); !reason.empty()) {
        failTool("esbuild at '" + binaryPath + "' could not be started -- " + reason);
        return;
    }
    startGeneration = database.generation();
    elapsed = 0.0F;
    timedOut = false;
}

void ScriptTranspiler::requeue(const AssetDatabase& database, ScriptJob job) {
    const AssetRecord* const record = database.findByGuid(job.guid);
    if (record == nullptr || !isScriptAssetName(record->relativePath)) {
        return;
    }
    job.assetsRelative = record->relativePath;
    job.projectRelative = projectRelativeOf(assetsPrefix, record->relativePath);
    queue.insert(queue.begin(), std::move(job));
}

void ScriptTranspiler::finish(const AssetDatabase& database, int exitCode) {
    if (!running.has_value()) {
        return;
    }
    const ScriptJob job = *running;
    if (timedOut) {
        failTool(
            std::format("esbuild at '{}' did not exit within {:.0f} seconds while transpiling '{}' and was stopped",
                        binaryPath, config.runLimitSeconds, job.projectRelative));
        return;
    }
    running.reset();
    if (database.generation() != startGeneration) {
        requeue(database, job);  // a scan completed mid-run: its hashes may be newer than what esbuild read
        return;
    }

    const std::string stem = joinPath(stagingDirectory(), formatGuid(job.guid));
    const std::string logPath = stem + ".log";
    const FileBytesResult logRead = readFileBytes(logPath, MAX_SCRIPT_LOG_BYTES);
    // A log the cap refused is not an empty log: it is never parsed, and its own lines say so. Any other read failure
    // (the log deleted from outside) reads as empty.
    const bool logTooLarge = logRead.refusedByCap;
    const EsbuildLogParse log = parseEsbuildLog(logRead.bytes.has_value() ? std::string_view(*logRead.bytes) : "");
    if (exitCode != 0) {
        failScript(database, job, exitCode, logPath, log, logTooLarge);
        return;
    }

    const std::string metafilePath = stem + ".meta.json";
    const FileBytesResult metaRead = readFileBytes(metafilePath, MAX_SCRIPT_METAFILE_BYTES);
    std::optional<EsbuildMetafile> metafile;
    if (metaRead.bytes.has_value()) {
        metafile = parseEsbuildMetafile(*metaRead.bytes);
    }
    if (!metafile.has_value() || metafile->inputs.empty()) {
        failPublish(job, job.projectRelative + ": error: esbuild's metafile '" + metafilePath + "' could not be read");
        return;
    }

    std::vector<RefusedInput> refused;
    for (const std::string& input : metafile->inputs) {
        const ScriptInputVerdict verdict = classifyScriptInput(input, assetsPrefix);
        if (verdict != ScriptInputVerdict::Accepted) {
            refused.push_back(RefusedInput{input, verdict});
        }
    }
    if (!refused.empty()) {
        failRefused(database, job, refused, *metafile);
        return;
    }
    publish(database, job, *metafile, log, logPath, logTooLarge);
}

void ScriptTranspiler::logDiagnostics(const ScriptJob& job, const EsbuildLogParse& log, const std::string& logPath) {
    show(job.projectRelative, log.diagnostics);
    if (log.total > log.diagnostics.size()) {
        note(ScriptLogLevel::Error, std::format("{}: error: {} more diagnostics are in '{}'", job.projectRelative,
                                                log.total - log.diagnostics.size(), logPath));
    }
}

ScriptRecord ScriptTranspiler::baseRecord(const ScriptJob& job, ScriptRecordStatus status) const {
    ScriptRecord record;
    record.status = status;
    record.script = job.projectRelative;
    record.esbuildVersion = std::string(esbuildPinnedVersion());
    record.esbuildBinary = binaryIdentity;
    record.flags = flagsFingerprint;
    return record;
}

std::string ScriptTranspiler::writeRecord(Guid guid, const ScriptRecord& record) const {
    const std::string directory = artifactDirectory();
    if (const std::string reason = ensureDirectory(directory); !reason.empty()) {
        return "could not prepare '" + directory + "' -- " + reason;
    }
    const std::string path = joinPath(directory, formatGuid(guid) + ".json");
    if (const std::string reason = writeTextFileAtomic(path, writeScriptRecordText(record)); !reason.empty()) {
        return "could not write '" + path + "' -- " + reason;
    }
    return {};
}

void ScriptTranspiler::failScript(const AssetDatabase& database, const ScriptJob& job, int exitCode,
                                  const std::string& logPath, const EsbuildLogParse& log, bool logTooLarge) {
    if (logTooLarge) {
        note(ScriptLogLevel::Error,
             std::format(
                 "{}: error: esbuild exited with code {} and its log '{}' is larger than {} KiB, so it was not read",
                 job.projectRelative, exitCode, logPath, MAX_SCRIPT_LOG_BYTES / 1024));
        setOutcome(job.guid, ScriptOutcome::Failed);
        return;
    }
    logDiagnostics(job, log, logPath);
    if (log.total == 0) {
        if (log.firstLine.empty()) {
            note(ScriptLogLevel::Error, std::format("{}: error: esbuild exited with code {} and wrote nothing to '{}'",
                                                    job.projectRelative, exitCode, logPath));
        } else {
            note(ScriptLogLevel::Error,
                 std::format("{}: error: esbuild exited with code {}; its log could not be read: '{}' (log: '{}')",
                             job.projectRelative, exitCode, log.firstLine, logPath));
        }
    }
    if (log.total == 0 || log.total > log.diagnostics.size()) {
        // The record would be a promise that nothing it depends on has changed, and a failure that names none of its
        // files, or only some, cannot keep it. The script stays stale and runs again at the next scan.
        setOutcome(job.guid, ScriptOutcome::Failed);
        return;
    }

    std::vector<std::string> paths{job.projectRelative};
    for (const ScriptDiagnostic& diagnostic : log.diagnostics) {
        if (!diagnostic.file.empty() &&
            classifyScriptInput(diagnostic.file, assetsPrefix) == ScriptInputVerdict::Accepted) {
            paths.push_back(diagnostic.file);
        }
    }
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    ScriptRecord record = baseRecord(job, ScriptRecordStatus::Failed);
    for (std::string& path : paths) {
        const std::optional<ContentHash> hash = hashOfInput(database, path);
        record.inputs.push_back(ScriptRecordInput{std::move(path), hash});
    }
    record.sourceSet = scriptSourceSetFingerprint(database.records());
    record.diagnostics = log.diagnostics;
    if (const std::string reason = writeRecord(job.guid, record); !reason.empty()) {
        note(ScriptLogLevel::Error, job.projectRelative + ": error: " + reason);
    }
    setOutcome(job.guid, ScriptOutcome::Failed);
}

void ScriptTranspiler::failPublish(const ScriptJob& job, const std::string& reason) {
    note(ScriptLogLevel::Error, reason);
    setOutcome(job.guid, ScriptOutcome::Failed);
}

void ScriptTranspiler::failRefused(const AssetDatabase& database, const ScriptJob& job,
                                   const std::vector<RefusedInput>& refused, const EsbuildMetafile& metafile) {
    ScriptRecord record = baseRecord(job, ScriptRecordStatus::Failed);
    for (const RefusedInput& input : refused) {
        // Location-less, as docs/09 15.2 says: the line a re-shown refusal prints is the one printed here.
        record.diagnostics.push_back(ScriptDiagnostic{ScriptDiagnosticSeverity::Error, std::string(), 0U, 0U,
                                                      refusalText(input.path, input.verdict)});
    }
    show(job.projectRelative, record.diagnostics);
    for (const std::string& input : metafile.inputs) {
        if (classifyScriptInput(input, assetsPrefix) == ScriptInputVerdict::Accepted) {
            record.inputs.push_back(ScriptRecordInput{input, hashOfInput(database, input)});
        }
    }
    record.sourceSet = scriptSourceSetFingerprint(database.records());
    if (const std::string reason = writeRecord(job.guid, record); !reason.empty()) {
        note(ScriptLogLevel::Error, job.projectRelative + ": error: " + reason);
    }
    setOutcome(job.guid, ScriptOutcome::Failed);
}

void ScriptTranspiler::publish(const AssetDatabase& database, const ScriptJob& job, const EsbuildMetafile& metafile,
                               const EsbuildLogParse& log, const std::string& logPath, bool logTooLarge) {
    const std::string staged = joinPath(stagingDirectory(), formatGuid(job.guid));
    const FileBytesResult map = readFileBytes(staged + ".js.map", MAX_SCRIPT_OUTPUT_BYTES);
    if (!map.bytes.has_value()) {
        failPublish(job, job.projectRelative + ": error: could not read '" + staged + ".js.map' -- " + map.error);
        return;
    }
    const FileBytesResult js = readFileBytes(staged + ".js", MAX_SCRIPT_OUTPUT_BYTES);
    if (!js.bytes.has_value()) {
        failPublish(job, job.projectRelative + ": error: could not read '" + staged + ".js' -- " + js.error);
        return;
    }

    const std::string directory = artifactDirectory();
    if (const std::string reason = ensureDirectory(directory); !reason.empty()) {
        failPublish(job, job.projectRelative + ": error: could not prepare '" + directory + "' -- " + reason);
        return;
    }
    const std::string published = joinPath(directory, formatGuid(job.guid));
    // Map before JS before record: a reader that finds a new JS finds its map, and the record -- the only thing that
    // makes the trio fresh -- lands last, so an interrupted publish runs again at the next scan.
    if (const std::string reason = writeTextFileAtomic(published + ".js.map", *map.bytes); !reason.empty()) {
        failPublish(job, job.projectRelative + ": error: could not write '" + published + ".js.map' -- " + reason);
        return;
    }
    if (const std::string reason = writeTextFileAtomic(published + ".js", *js.bytes); !reason.empty()) {
        failPublish(job, job.projectRelative + ": error: could not write '" + published + ".js' -- " + reason);
        return;
    }
    ScriptRecord record = baseRecord(job, ScriptRecordStatus::Ok);
    for (const std::string& input : metafile.inputs) {
        record.inputs.push_back(ScriptRecordInput{input, hashOfInput(database, input)});
    }
    if (const std::string reason = writeRecord(job.guid, record); !reason.empty()) {
        failPublish(job, job.projectRelative + ": error: " + reason);
        return;
    }

    if (logTooLarge) {
        note(ScriptLogLevel::Warn,
             std::format("{}: warning: esbuild's log '{}' is larger than {} KiB, so its warnings were not read",
                         job.projectRelative, logPath, MAX_SCRIPT_LOG_BYTES / 1024));
    } else {
        logDiagnostics(job, log, logPath);
        if (log.total == 0 && !log.firstLine.empty()) {
            note(ScriptLogLevel::Error, std::format("{}: error: esbuild's log could not be read: '{}' (log: '{}')",
                                                    job.projectRelative, log.firstLine, logPath));
        }
    }
    setOutcome(job.guid, ScriptOutcome::Transpiled);
}

}  // namespace engine::editor

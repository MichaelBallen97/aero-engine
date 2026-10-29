#pragma once
// Aero Engine -- the IMPURE half of the esbuild integration (task 4.3.1). SRC-PRIVATE: it holds a ToolProcess by
// value, and tool_process.hpp is src-private too. EditorApp keeps it behind a unique_ptr over this (incomplete) type.
//
// What it does: after every completed asset scan it finds the TypeScript scripts whose stored result is stale, runs
// esbuild over them ONE AT A TIME (the OS runs the child; a non-blocking poll per tick is the whole wait), and
// publishes <guid>.js, <guid>.js.map and <guid>.json under <projectRoot>/Library/Scripts/. Every line it wants shown
// comes out of takeLog() as a value: NOTHING HERE LOGS.
#include <aero/core/guid.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/esbuild_tool.hpp>
#include <aero/editor/project.hpp>  // ProjectLanguage

#include "tool_process.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

struct ScriptTranspilerConfig {
    std::string esbuildPath;  // "" => resolved at the first scan that finds a script
    float runLimitSeconds = SCRIPT_RUN_LIMIT_SECONDS;
};

struct ScriptProjectView {
    std::string projectRoot;  // ProjectSession::root(), copied; "" == no project
    std::string assetsPath;   // ProjectManifest::assetsPath, verbatim
    ProjectLanguage language = ProjectLanguage::Ts;
};

enum class ScriptLogLevel : std::uint8_t { Info = 0, Warn, Error };

struct ScriptLogLine {
    ScriptLogLevel level = ScriptLogLevel::Info;
    std::string text;  // complete, starting "scripts: "
};

class ScriptTranspiler {
public:
    explicit ScriptTranspiler(ScriptTranspilerConfig settings);
    ~ScriptTranspiler();  // the ToolProcess member kills a live child, then destroys it
    ScriptTranspiler(const ScriptTranspiler&) = delete;
    ScriptTranspiler& operator=(const ScriptTranspiler&) = delete;
    ScriptTranspiler(ScriptTranspiler&&) = delete;
    ScriptTranspiler& operator=(ScriptTranspiler&&) = delete;

    // Evaluates every script's record against the scan just completed and queues the stale ones. Spawns nothing.
    void noteScan(const AssetDatabase& database, const ScriptProjectView& project);
    // THE ONLY SPAWN SITE. At most one finish and one start per call; the wait is a non-blocking poll.
    void service(const AssetDatabase& database, float deltaSeconds);
    // Kills the child, drops the queue and the open pass. Files a killed run staged are never published.
    void resetForProjectSwap() noexcept;
    [[nodiscard]] std::vector<ScriptLogLine> takeLog() noexcept;    // moves the pending lines out
    [[nodiscard]] bool idle() const noexcept;                       // no queue, no child, no open pass
    [[nodiscard]] std::size_t runCount() const noexcept;            // esbuild starts ATTEMPTED, lifetime
    [[nodiscard]] const std::string& esbuildPath() const noexcept;  // "" until resolved

private:
    struct ScriptJob {
        Guid guid;
        std::string assetsRelative;
        std::string projectRelative;
    };
    enum class ScriptOutcome : std::uint8_t { Transpiled = 0, Failed };
    // A fresh failed record found by the latest evaluation: shown again, and counted, when the pass ends.
    struct FreshFailure {
        Guid guid;
        std::string script;  // the job's project-relative path
        std::vector<ScriptDiagnostic> diagnostics;
    };
    struct RefusedInput {
        std::string path;
        ScriptInputVerdict verdict = ScriptInputVerdict::Accepted;
    };

    void resolveBinary();
    void learnIdentity();
    [[nodiscard]] std::vector<ScriptJob> collectScripts(const AssetDatabase& database) const;
    [[nodiscard]] ScriptFreshnessContext freshnessContext(const AssetDatabase& database) const;
    [[nodiscard]] std::optional<ContentHash> hashOfInput(const AssetDatabase& database,
                                                         std::string_view projectRelative) const;
    // The job's record when it is current (for an ok one, both artifacts exist too), nullopt when the script is stale.
    [[nodiscard]] std::optional<ScriptRecord> freshRecord(const ScriptJob& job,
                                                          const ScriptFreshnessContext& context) const;
    void note(ScriptLogLevel level, const std::string& text);
    // One note per diagnostic, at its level: the live paths and the replay of a stored failure share this.
    void show(const std::string& script, const std::vector<ScriptDiagnostic>& lines);
    [[nodiscard]] bool hasOutcome(Guid guid) const noexcept;
    void setOutcome(Guid guid, ScriptOutcome outcome);
    void endPass();
    void enqueue(ScriptJob job);

    void start(const AssetDatabase& database, const ScriptJob& job);
    void finish(const AssetDatabase& database, int exitCode);
    void requeue(const AssetDatabase& database, ScriptJob job);
    void failTool(const std::string& reason);
    void failScript(const AssetDatabase& database, const ScriptJob& job, int exitCode, const std::string& logPath,
                    const EsbuildLogParse& log, bool logTooLarge);
    void failPublish(const ScriptJob& job, const std::string& reason);
    void failRefused(const AssetDatabase& database, const ScriptJob& job, const std::vector<RefusedInput>& refused,
                     const EsbuildMetafile& metafile);
    void publish(const AssetDatabase& database, const ScriptJob& job, const EsbuildMetafile& metafile,
                 const EsbuildLogParse& log, const std::string& logPath, bool logTooLarge);
    void logDiagnostics(const ScriptJob& job, const EsbuildLogParse& log, const std::string& logPath);
    [[nodiscard]] ScriptRecord baseRecord(const ScriptJob& job, ScriptRecordStatus status) const;
    // "" on success, else the reason. Creates Library/Scripts/ first.
    [[nodiscard]] std::string writeRecord(Guid guid, const ScriptRecord& record) const;

    [[nodiscard]] std::string stagingDirectory() const;
    [[nodiscard]] std::string artifactDirectory() const;

    ScriptTranspilerConfig config;
    std::string binaryPath;
    bool binaryResolved = false;
    std::string binaryIdentity;  // formatContentHash of the binary, "" while unknown
    std::string flagsFingerprint;
    ScriptProjectView view;
    std::string assetsPrefix;

    std::vector<ScriptJob> queue;
    std::optional<ScriptJob> running;
    std::uint64_t startGeneration = 0;
    float elapsed = 0.0F;
    bool timedOut = false;
    ToolProcess process;

    bool passOpen = false;
    std::vector<std::pair<Guid, ScriptOutcome>> outcomes;  // sorted by Guid, last outcome wins
    std::vector<Guid> freshAtLatest;                       // the latest evaluation's fresh ok records, sorted
    std::vector<FreshFailure> failedAtLatest;              // its fresh failed records, in evaluation order

    std::vector<ScriptLogLine> pending;
    std::size_t runs = 0;
};

}  // namespace engine::editor

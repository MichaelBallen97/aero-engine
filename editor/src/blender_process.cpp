// Aero Engine — blender_process.cpp: the tree's FIRST AND ONLY process spawn (task 3.2.4). Before
// this file there was no process-creation call of any kind -- no SDL_CreateProcess, no shell
// wrapper, no fork -- anywhere in engine/, editor/, tools/ or tests/, measured rather than assumed,
// and this stays the only one.
//
// NOTE TO THE AUTHOR OF THIS COMMENT: INV-B2's gate grep does not strip comments (the AC-5 rule
// again). Do NOT name the shell-launcher functions it searches for in prose here; naming them turns a
// hard, empty-output gate into one that has to be read and judged.
//
// NOTHING HERE LOGS (INV-B10): a status is RETURNED, never printed, exactly as text_file.cpp and
// asset_database.cpp already do. Nothing here throws. Nothing here removes, renames or copies a file.
//
// On POSIX, SDL's own process implementation reaps its children: wait() and waitpid(-1, ...) must not
// be called by the application, and SIGCHLD must not be ignored or handled. The editor does neither
// today, and this comment exists so a future addition does not start.
//
// Two guards are confirmed OUT OF SCOPE for the SDL_process.h include below, both read directly:
// check-platform-boundary.sh scans HEADER_GLOB='engine/*/include/*' -- public ENGINE headers only, so
// an editor/src/*.cpp is structurally invisible to it; check-rhi-boundary.sh scans engine/ and
// runtime/ and matches only SDL_*GPU tokens, and its own comment says editor/ is deliberately out of
// scope. file_dialog.cpp's <SDL3/SDL_dialog.h> has been the precedent since task 2.5.1.
#include "blender_process.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

[[nodiscard]] std::string sdlErrorOr(std::string_view fallback) {
    const char* const reason = SDL_GetError();
    if (reason != nullptr && reason[0] != '\0') {
        return {reason};
    }
    return std::string(fallback);  // AC-21 asserts a NON-EMPTY reason; SDL is not required to set one
}

}  // namespace

BlenderProcess::~BlenderProcess() { reset(); }

BlenderProcess::BlenderProcess(BlenderProcess&& other) noexcept
    : handle(other.handle), lastExitCode(other.lastExitCode), alive(other.alive) {
    other.handle = nullptr;
    other.alive = false;
}

BlenderProcess& BlenderProcess::operator=(BlenderProcess&& other) noexcept {
    if (this != &other) {
        reset();  // whatever WE held is killed and destroyed first -- never leaked, never orphaned
        handle = other.handle;
        lastExitCode = other.lastExitCode;
        alive = other.alive;
        other.handle = nullptr;
        other.alive = false;
    }
    return *this;
}

void BlenderProcess::reset() noexcept {
    if (handle == nullptr) {
        return;
    }
    auto* const process = static_cast<SDL_Process*>(handle);
    if (alive) {
        // FORCEFUL here, and deliberately so: nothing polls after this point, so there is nobody left
        // to escalate a graceful kill that the child ignores. The graceful-then-forceful pair lives in
        // BlenderService::poll(), which has ticks in which to wait. SDL_DestroyProcess does NOT stop a
        // process (F1), so killing first is what makes "quit mid-conversion leaves no orphan" true.
        SDL_KillProcess(process, /*force=*/true);
    }
    SDL_DestroyProcess(process);
    handle = nullptr;
    alive = false;
}

std::string BlenderProcess::start(const std::vector<std::string>& args, std::string_view workingDirUtf8,
                                  std::string_view logAbsolutePath) {
    reset();  // INV-B5: at most ONE live child per object, always
    if (args.empty()) {
        return "no command was supplied";
    }

    // The argv array BORROWS from `args`, which the caller owns for the whole of this call. A dangling
    // c_str() here is undefined behaviour ASan may or may not catch depending on allocation reuse, so
    // the backing strings must never be a temporary -- they are the caller's named vector.
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);  // SDL reads the array until this terminator

    const std::string workingDir(workingDirUtf8);
    const std::string logPath(logAbsolutePath);

    const SDL_PropertiesID props = SDL_CreateProperties();
    if (props == 0) {
        return sdlErrorOr("could not create the process properties");
    }

    SDL_IOStream* logStream = nullptr;
    std::string failure;
    // reinterpret_cast, not static_cast: `const char**` -> `void*` is a MULTILEVEL pointer conversion,
    // which bugprone-multi-level-implicit-pointer-conversion requires to be spelled explicitly. SDL
    // reads the array as `const char * const *` and never writes through it.
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, reinterpret_cast<void*>(argv.data()));
    if (!workingDir.empty()) {
        // AC-20: the child's working directory, so a tool that writes a RELATIVE path writes it here
        // and never into the project root or the assets root.
        SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, workingDir.c_str());
    }
    if (!logPath.empty()) {
        logStream = SDL_IOFromFile(logPath.c_str(), "wb");
        if (logStream == nullptr) {
            failure = sdlErrorOr("could not open the tool log for writing");
        } else {
            SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_REDIRECT);
            SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_POINTER, logStream);
            // STDERR_TO_STDOUT folds the error stream into the SAME file, so one log carries the whole
            // run. SDL's header states this has NO EFFECT if STDERR_NUMBER is also set, which is
            // exactly why STDERR_NUMBER is deliberately never set here. Seed S34 removes this line.
            SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
        }
    }

    if (failure.empty()) {
        SDL_Process* const process = SDL_CreateProcessWithProperties(props);
        if (process == nullptr) {
            failure = sdlErrorOr("the process could not be started");
        } else {
            handle = process;
            alive = true;
            lastExitCode = 0;
        }
    }

    // Closed IMMEDIATELY, on EVERY path including both failure paths. SDL's own header requires the
    // application to close a redirect stream once the process is created; leaving it open is a handle
    // leak on every platform and, on Windows, blocks the next run's overwrite of the same log.
    if (logStream != nullptr) {
        SDL_CloseIO(logStream);
    }
    SDL_DestroyProperties(props);  // destroyed on EVERY path too, including the failure path
    return failure;
}

ProcessState BlenderProcess::poll(int& exitCodeOut) noexcept {
    if (handle == nullptr) {
        return ProcessState::SpawnFailed;
    }
    if (!alive) {
        exitCodeOut = lastExitCode;  // already reaped; report the same code rather than re-waiting
        return ProcessState::Exited;
    }
    int code = 0;
    // block == false, ALWAYS and everywhere (INV-B4). A blocking wait here would freeze the editor for
    // the whole of a multi-minute export, which is the one thing this entire design exists to avoid.
    if (SDL_WaitProcess(static_cast<SDL_Process*>(handle), false, &code)) {
        alive = false;
        lastExitCode = code;
        exitCodeOut = code;
        return ProcessState::Exited;
    }
    // exitCodeOut is deliberately NOT written here: SDL leaves it untouched while the process runs.
    return ProcessState::Running;
}

void BlenderProcess::kill(bool force) noexcept {
    if (handle != nullptr && alive) {
        SDL_KillProcess(static_cast<SDL_Process*>(handle), force);
    }
}

bool BlenderProcess::running() const noexcept { return alive; }

}  // namespace engine::editor

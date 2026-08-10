#pragma once
// Aero Engine — the ONLY SDL_Process translation unit in this tree (task 3.2.4). SRC-PRIVATE, the
// gltf_import.hpp / fbx_import.hpp / obj_import.hpp precedent a FOURTH time: a third-party API is
// confined to one .hpp/.cpp pair, and nothing above it names its types.
//
// This header names NO SDL type at all -- the SDL_Process* lives in the .cpp behind an opaque void*,
// so blender_service.hpp (which is PUBLIC) can hold this class through a unique_ptr over an
// INCOMPLETE type without SDL reaching a single public editor header.
//
// NOTHING HERE LOGS (INV-B10). Nothing here deletes, renames or copies a file (INV-B9): this TU is in
// NEITHER of check-project-no-delete.sh's two lists, which is exactly what makes a future
// std::filesystem::remove written here a hard CI failure rather than a review comment.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

enum class ProcessState : std::uint8_t { Running = 0, Exited, SpawnFailed };

class BlenderProcess {
public:
    BlenderProcess() noexcept = default;
    ~BlenderProcess();                                     // kills if still running, THEN destroys
    BlenderProcess(BlenderProcess&&) noexcept;             // out-of-line
    BlenderProcess& operator=(BlenderProcess&&) noexcept;  // out-of-line
    BlenderProcess(const BlenderProcess&) = delete;
    BlenderProcess& operator=(const BlenderProcess&) = delete;

    // "" on success, the OS/SDL reason otherwise. AT MOST ONE live child per object: an existing one
    // is killed and destroyed first.
    //
    // Opens logAbsolutePath "wb" as the REDIRECT stream for BOTH stdout and stderr, and CLOSES IT
    // BEFORE RETURNING (D11) -- SDL's own header requires the application to close a redirect stream
    // once the process is created, and on Windows an open handle would additionally block the next
    // run's overwrite of the same path. An EMPTY log path means "do not redirect", which leaves
    // stdout at SDL's own default; stdin is left at its default, SDL_PROCESS_STDIO_NULL.
    //
    // NO PIPE IS EVER CREATED. SDL's header warns that a process whose output is piped to the
    // application can block indefinitely waiting to be read, and that the wait call will then never
    // report completion -- which for a per-tick poll loop against a chatty tool is a deadlock, not a
    // slowdown. A file has neither problem and needs no per-frame drain.
    [[nodiscard]] std::string start(const std::vector<std::string>& args, std::string_view workingDirUtf8,
                                    std::string_view logAbsolutePath);

    // NON-BLOCKING, always (INV-B4). `exitCodeOut` is written ONLY when this returns Exited -- SDL
    // documents that it leaves the value untouched while the process is still running, so reading it
    // in any other state reads whatever the caller last put there.
    //
    // The exit code is the process's own code when it terminated normally, a NEGATIVE SIGNAL NUMBER
    // when a signal killed it, or -255 otherwise. Nothing in this task compares it to anything but
    // zero, which is what makes the cancel and timeout paths behave identically on all three
    // platforms even though their kill semantics differ (R5).
    //
    // SpawnFailed is returned whenever there is no live handle -- start() failed, or was never
    // called. The service only ever polls a process it successfully started, so the two cases never
    // need to be told apart above this layer.
    [[nodiscard]] ProcessState poll(int& exitCodeOut) noexcept;

    // force == false is SIGTERM on POSIX and TerminateProcess on Windows, which has no graceful
    // signal -- so the escalation to force == true is POSIX-only behaviour (R5). Every caller asserts
    // "no longer running", never "received a signal", so all three platforms agree.
    void kill(bool force) noexcept;

    [[nodiscard]] bool running() const noexcept;

private:
    void reset() noexcept;   // kill (forcefully) if alive, then destroy; leaves the object empty
    void* handle = nullptr;  // an SDL_Process*, deliberately type-erased so this HEADER names no SDL type
    int lastExitCode = 0;    // the code observed at the transition to Exited; re-reported on later polls
    bool alive = false;
};

}  // namespace engine::editor

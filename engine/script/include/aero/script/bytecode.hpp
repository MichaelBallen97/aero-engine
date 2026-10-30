#pragma once
// Aero Engine -- the bytecode cook's public face (task 4.3.2): one ES module's source text in, the backend's bytecode
// out. The backend never crosses this header: the payload is plain bytes, the release and the bytecode version are
// plain numbers. The container those bytes travel in (`.aeroscript`) belongs to engine/assets; the cooker assembles it.
//
// The bytes are what the pinned backend writes with its function source text stripped: frames keep file, line and
// column; a script function's toString() reads "function <name>() {\n    [native code]\n}"; name and length are
// unchanged; import.meta still works.

#include <aero/script/vm.hpp>  // ScriptError

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace engine::script {

// The most source text one cook takes, and the heap the cook's VMs get. The heap is large on purpose: a cook compiles
// under a limit no real script nears, so a heap refusal there means a script too large for the cook, never a tight
// budget a game VM would have.
inline constexpr std::size_t MAX_COOK_SOURCE_BYTES = std::size_t{16} * 1024U * 1024U;    // 16 MiB
inline constexpr std::size_t COOK_HEAP_LIMIT_BYTES = std::size_t{1024} * 1024U * 1024U;  // 1 GiB

enum class ScriptCookStatus : std::uint8_t {
    Ok = 0,
    TooLarge,           // over MAX_COOK_SOURCE_BYTES; refused before a VM exists
    CompileError,       // not a module the backend compiles
    LoadError,          // a static import or re-export: a cooked script imports nothing
    OutOfMemory,        // the cook's heap refused an allocation, or its VM could not be created
    NotRenameable,      // a name the cook reserves occurs in the script itself (the rename proof failed)
    ReadBackMismatch,   // the payload did not read back and write again to the same bytes
    UnsupportedOutput,  // the backend wrote another bytecode version, or the checksum bypass value
};
// Never named toString: doctest's DOCTEST_STRINGIFY finds an unqualified toString by ADL.
[[nodiscard]] std::string_view scriptCookStatusLabel(ScriptCookStatus status) noexcept;

struct ScriptCookResult {
    ScriptCookStatus status = ScriptCookStatus::CompileError;
    ScriptError error;                  // empty iff Ok; names sourceName, never a name the cook reserves
    std::vector<std::byte> payload;     // non-empty iff Ok
    std::uint32_t backendRelease = 0;   // scriptBackendRelease() iff Ok
    std::uint32_t bytecodeVersion = 0;  // scriptBytecodeVersion() iff Ok
    std::uint32_t stripFlags = 0;       // 1 (function source text stripped) iff Ok
};

// One ES module source in, the backend's bytecode out. `sourceName` is what an error names (the cooker passes the
// input's path). Never logs, never reads a file, never throws. Uses the calling thread's stack like a Vm call
// (DEFAULT_STACK_LIMIT_BYTES + 256 KiB free). The payload does not depend on `sourceName`, the process, or what any
// VM ran before.
[[nodiscard]] ScriptCookResult cookScriptBytecode(std::string_view source, std::string_view sourceName);

// The backend's release, (major << 16) | (minor << 8) | patch: 0x00000F01 for quickjs-ng 0.15.1.
[[nodiscard]] std::uint32_t scriptBackendRelease() noexcept;
// The bytecode version the pinned backend writes first: 26.
[[nodiscard]] std::uint32_t scriptBytecodeVersion() noexcept;

}  // namespace engine::script

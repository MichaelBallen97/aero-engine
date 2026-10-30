#pragma once
// Aero Engine -- the bytecode cook's test seam (task 4.3.2; internal to engine/script), the VmAccess precedent: the
// public cook refuses every static import and has no module source; a test that needs bytecode that imports (R22's
// two shapes through bytecode) cooks through here.

#include <aero/script/bytecode.hpp>

#include <string_view>

namespace engine {
class VirtualFileSystem;
}

namespace engine::script::internal {

struct CookOptions {
    bool refusesImports = true;  // false only in tests: R22's shapes need bytecode that imports
    // BORROWED; where an allowed import is read, at cook and at read-back. Absolute res:// specifiers only: the cook's
    // own module name holds no path a relative specifier could resolve against.
    const VirtualFileSystem* modules = nullptr;
};

[[nodiscard]] ScriptCookResult cookScriptBytecodeWith(std::string_view source, std::string_view sourceName,
                                                      const CookOptions& options);

}  // namespace engine::script::internal

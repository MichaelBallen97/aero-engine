#include "fbx_import.hpp"

#include <string>
#include <ufbx.h>

namespace engine::editor {

ImportResult importFbx(std::string_view, std::span<const std::byte>, const ImportSettings&, ImportDepth,
                       std::span<const ExternalBuffer>) {
    // A STUB, replaced in Steps 4-9. It deliberately calls an OUT-OF-LINE ufbx symbol so this step
    // proves the LINK across three toolchains, not merely the include: 33 204 lines of C compiled into
    // a C++ static library is exactly the kind of thing that compiles on one lane and fails to link on
    // another. ufbx_is_thread_safe() is `ufbx_abi bool ufbx_is_thread_safe(void)` -- a real function in
    // ufbx.c, never constant-folded away.
    ImportResult result;
    result.status = ImportStatus::ParseFailed;
    result.message = ufbx_is_thread_safe() ? "the FBX backend is not implemented yet"
                                           : "the FBX backend is not implemented yet (single-threaded build)";
    return result;
}

}  // namespace engine::editor

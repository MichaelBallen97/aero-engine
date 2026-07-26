#include "editor_reflection.hpp"

#include <aero/core/log.hpp>

#if defined(AERO_EDITOR_REFLECTION)
// Forward-declared here; DEFINED by the GENERATED aero_editor_core.aggregator.gen.cpp (cmake/reflect.cmake's
// aero_reflect_generate(), task 1.1.4) that calls every per-header register function for the four
// built-in component headers in HEADERS-list order. The snake_case name is the frozen cross-boundary
// contract between hand-written and generated code (spec D3/D7) -- not a C++-style identifier subject
// to docs/04's camelCase law, exactly like meta_test.cpp's five existing NOLINTs of this shape.
// NOLINTNEXTLINE(readability-identifier-naming)
void aero_reflect_register_all_aero_editor_core();
#endif

namespace engine::editor {

void registerEditorReflection() {
    static const bool registered = [] {
#if defined(AERO_EDITOR_REFLECTION)
        aero_reflect_register_all_aero_editor_core();
#else
        AERO_LOG_WARN("editor: built without AERO_REFLECT_TOOLS — inspector field editing disabled");
#endif
        return true;
    }();
    (void)registered;
}

}  // namespace engine::editor

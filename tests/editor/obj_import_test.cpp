// tests/editor/obj_import_test.cpp -- task 3.2.3: the Wavefront OBJ/MTL backend (tinyobjloader). A TU
// of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, tier-0 (the fbx_import_test.cpp / model_import_test.cpp precedent): model_import.hpp
// depends only on aero/core/{guid,math}.hpp, aero/editor/import_settings.hpp and
// aero/editor/scene_bounds.hpp -- the last of those reaches aero::scene, a PUBLIC, UNGATED dependency
// of aero_editor_core (only engine/scene_serialize is gated on AERO_REFLECT_TOOLS) -- so every case in
// this file must be PRESENT and PASSING in both reduced configurations, not merely the default build.
// No GPU, no window, no ImGui context, no sleeps: nearly every case is a raw string literal, so the
// suite runs with zero disk (the two exceptions are AERO_ASSET_FIXTURES_DIR's committed cube.obj/
// cube.mtl, reached through readFileBytes, once tinyobjloader's real-file path needs proving).
//
// THIS TU NAMES NO tinyobjloader TYPE (AC-11's sibling check, §V4/§V6) -- it drives the OBJ backend
// only indirectly, through the PUBLIC importModel() dispatch. obj_import.hpp is src-private and stays
// that way; nothing here #includes it.
#include <aero/editor/model_import.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace {

// The fastgltf/ufbx-free byte-loading pattern model_import_test.cpp's and fbx_import_test.cpp's own
// asBytes use, restated here so this TU stays independent of both.
[[nodiscard]] std::span<const std::byte> asBytes(const std::string& text) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

}  // namespace

using engine::editor::ImportDepth;
using engine::editor::importModel;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;

// task 3.2.3, Step 2: a PLACEHOLDER. It becomes AC-25's real one-triangle Full case at Step 5, once
// geometry conversion exists. Exists from this step so AC-69's "OI1 present in both reduced
// configurations" has something to find, and so this file is tracked before the guards next run.
//
// A BUILD-TIME FINDING, recorded here rather than smoothed over: isImportableModelName already claims
// ".obj" as of THIS commit (§D-4(a)), but the dispatch (§D-4(d)) does not grow its OBJ/MTL arm until
// Step 3. In between, a ".obj" name falls through to the glTF arm's "everything else importable"
// catch-all (`if (isImportableModelName(fileName)) return importGltf(...)`) and fails THERE --
// ParseFailed, not Unsupported -- because Wavefront text is not valid JSON. This is exactly the
// mis-route MI105c exists to catch, arising here with no sabotage seed needed at all, purely from
// widening the predicate one step before the dispatch. Step 3 corrects it; this assertion documents the
// transient, real state of the tree between the two commits rather than a hypothetical one.
TEST_CASE("obj_import: placeholder -- .obj is not yet routed to the OBJ backend (OI1)") {
    const std::string doc = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const ImportResult result = importModel("t.obj", "", asBytes(doc), ImportSettings{}, ImportDepth::Full, {});
    CHECK(result.status == ImportStatus::ParseFailed);  // misrouted through the glTF catch-all -- Step 3 fixes it
}

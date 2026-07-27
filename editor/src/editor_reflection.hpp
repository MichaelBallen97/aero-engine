#pragma once
// Aero Engine -- the D16 reflection bootstrap (task 2.2.2). SRC-PRIVATE, entt-free (the aggregator
// call it forwards to is the only entt-adjacent thing here, and even that is just a function call).
//
// Idempotent, process-lifetime, no teardown (F24): registering entt::meta types is not state anyone
// OWNS the way EditorApp owns its engine objects (2.1.3 D1) -- it is a one-shot side effect safe to
// call from create(), which is what lets aero_editor_imgui_test drive a fully-registered editor
// through tick() with no extra plumbing.

namespace engine::editor {

// Registers every built-in component's generated entt::meta artifacts, exactly once per process.
// Safe to call from multiple call sites (idempotent via a function-local static). Under
// -DAERO_REFLECT_TOOLS=OFF, this degrades to one AERO_LOG_WARN and no registration (D12) -- the
// inspector then shows every present component with hasFields == false ("fields unavailable").
void registerEditorReflection();

}  // namespace engine::editor

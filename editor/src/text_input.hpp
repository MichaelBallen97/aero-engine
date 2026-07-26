#pragma once
// Aero Engine -- a src-private std::string InputText wrapper, replacing vcpkg's prebuilt
// imgui_stdlib (task 2.2.1, promoted to a shared TU at task 2.2.2). vcpkg's imguid.lib ships
// imgui_stdlib.cpp.obj built WITHOUT ASan; on the Windows Debug lane MSVC's ASan turns on
// std::string/std::vector container annotations in every TU we compile (annotate_string/
// annotate_vector = 1), so linking against that prebuilt object is an ODR-class mismatch
// (LNK2038). Reimplementing the ~20-line resize-callback trick here (behaviour identical to
// misc/cpp/imgui_stdlib.cpp: unbounded length, no truncation) means the prebuilt object is never
// referenced, so the mismatch cannot occur.
//
// Deliberately NOT `#include <imgui_stdlib.h>` and NOT in `namespace ImGui` -- both would reopen
// the exact duplicate-symbol risk this fix removes (F18/§DN-6). Shared by hierarchy_panel.cpp
// (task 2.2.1) and inspector_panel.cpp (task 2.2.2) -- promoted here so neither re-derives it.
#include <imgui.h>
#include <string>

namespace engine::editor {

// Mirrors ImGui::InputText(const char*, std::string*, ...) from misc/cpp/imgui_stdlib.cpp.
bool inputTextString(const char* label, std::string& str, ImGuiInputTextFlags flags);

}  // namespace engine::editor

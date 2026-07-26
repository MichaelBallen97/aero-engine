#include "text_input.hpp"

#include <cstddef>

namespace engine::editor {

namespace {

// Deliberately NOT `namespace ImGui` -- see text_input.hpp's header comment.
struct InputTextResizeUserData {
    std::string* str = nullptr;
    ImGuiInputTextCallback chainCallback = nullptr;
    void* chainUserData = nullptr;
};

int inputTextResizeCallback(ImGuiInputTextCallbackData* data) {
    auto* userData = static_cast<InputTextResizeUserData*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* str = userData->str;
        IM_ASSERT(data->Buf == str->data());
        str->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = str->data();  // non-const std::string::data() (C++17) -- no cast needed
    } else if (userData->chainCallback != nullptr) {
        data->UserData = userData->chainUserData;
        return userData->chainCallback(data);
    }
    return 0;
}

}  // namespace

bool inputTextString(const char* label, std::string& str, ImGuiInputTextFlags flags) {
    IM_ASSERT((flags & ImGuiInputTextFlags_CallbackResize) == 0);
    flags |= ImGuiInputTextFlags_CallbackResize;

    InputTextResizeUserData cbUserData{.str = &str};
    return ImGui::InputText(label, str.data(), str.capacity() + 1, flags, inputTextResizeCallback, &cbUserData);
}

}  // namespace engine::editor

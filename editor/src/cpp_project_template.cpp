// Aero Engine -- the C++ project template's pure render (task 4.5.2). Includes its own header, its data header and
// std only, so tests/sdk/ can compile this TU with aero::core alone.
#include <aero/editor/cpp_project_template.hpp>

#include "cpp_project_template_data.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace engine::editor {

namespace {

constexpr std::string_view TOKEN_ID = "@AERO_PROJECT_ID@";
constexpr std::string_view TOKEN_TITLE = "@AERO_PROJECT_TITLE@";
constexpr std::string_view TOKEN_NAME = "@AERO_PROJECT_NAME@";

[[nodiscard]] bool isIdentifierByte(unsigned char byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_';
}

struct Replacements {
    std::string_view name;
    std::string id;
    std::string title;
};

// One walk over the template. At each '@' the three tokens are tried; replaced text is appended and never rescanned,
// so a project whose name spells a token keeps it literal.
[[nodiscard]] std::string renderText(std::string_view templateText, const Replacements& replacements) {
    std::string out;
    out.reserve(templateText.size() + replacements.id.size() + replacements.title.size());
    std::size_t at = 0;
    while (at < templateText.size()) {
        const std::string_view rest = templateText.substr(at);
        if (rest.front() == '@') {
            if (rest.starts_with(TOKEN_ID)) {
                out += replacements.id;
                at += TOKEN_ID.size();
                continue;
            }
            if (rest.starts_with(TOKEN_TITLE)) {
                out += replacements.title;
                at += TOKEN_TITLE.size();
                continue;
            }
            if (rest.starts_with(TOKEN_NAME)) {
                out += replacements.name;
                at += TOKEN_NAME.size();
                continue;
            }
        }
        out += rest.front();
        ++at;
    }
    return out;
}

}  // namespace

std::string cppProjectIdentifier(std::string_view projectName) {
    std::string id = "game_";
    id.reserve(id.size() + projectName.size());
    for (const char c : projectName) {
        id += isIdentifierByte(static_cast<unsigned char>(c)) ? c : '_';
    }
    return id;
}

std::string cppStringLiteralBody(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (byte < 0x20U || byte >= 0x7FU) {
            out += '\\';
            out += static_cast<char>('0' + ((byte >> 6U) & 7U));
            out += static_cast<char>('0' + ((byte >> 3U) & 7U));
            out += static_cast<char>('0' + (byte & 7U));
        } else {
            out += c;
        }
    }
    return out;
}

std::array<CppProjectFile, CPP_PROJECT_FILE_COUNT> renderCppProject(std::string_view projectName) {
    const Replacements replacements{
        .name = projectName, .id = cppProjectIdentifier(projectName), .title = cppStringLiteralBody(projectName)};
    return {{
        {.relativePath = "CMakeLists.txt", .text = renderText(embedded::cppProjectCmakeTemplate(), replacements)},
        {.relativePath = "src/main.cpp", .text = renderText(embedded::cppProjectMainTemplate(), replacements)},
        {.relativePath = "README.md", .text = renderText(embedded::cppProjectReadmeTemplate(), replacements)},
        {.relativePath = ".gitignore", .text = renderText(embedded::cppProjectGitignoreTemplate(), replacements)},
    }};
}

}  // namespace engine::editor

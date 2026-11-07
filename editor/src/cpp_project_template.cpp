// Aero Engine -- the C++ project template's render and writer (task 4.5.2). Includes its own header, text_file.hpp,
// its data header and std only, so tests/sdk/ can compile this TU with aero::core alone.
//
// The writer only ever CREATES: its mutating calls are ensureDirectory and writeTextFileAtomic, the writeTsconfig
// precedent. It names no delete, rename or copy call (check A's ninth file); the rename inside the atomic write lives
// in text_file.cpp.
#include <aero/editor/cpp_project_template.hpp>
#include <aero/editor/text_file.hpp>

#include "cpp_project_template_data.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

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

// The parent folder of a relative path with a '/', "" when it has none.
[[nodiscard]] std::string_view parentOf(std::string_view relativePath) {
    const std::size_t slash = relativePath.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : relativePath.substr(0, slash);
}

[[nodiscard]] std::string joinUnder(std::string_view root, std::string_view leaf) {
    std::string path(root);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    path += leaf;
    return path;
}

// One file: present (a directory wearing the name counts) -> Unchanged; else its parent, then the atomic write.
[[nodiscard]] CppProjectFileOutcome writeOne(std::string_view projectRoot, const CppProjectFile& file) {
    const std::string path = joinUnder(projectRoot, file.relativePath);
    if (fileExists(path)) {
        return {.relativePath = file.relativePath, .state = CppProjectWrite::Unchanged, .error = {}};
    }
    if (const std::string_view parent = parentOf(file.relativePath); !parent.empty()) {
        if (std::string error = ensureDirectory(joinUnder(projectRoot, parent)); !error.empty()) {
            return {.relativePath = file.relativePath, .state = CppProjectWrite::Failed, .error = std::move(error)};
        }
    }
    if (std::string error = writeTextFileAtomic(path, file.text); !error.empty()) {
        return {.relativePath = file.relativePath, .state = CppProjectWrite::Failed, .error = std::move(error)};
    }
    return {.relativePath = file.relativePath, .state = CppProjectWrite::Written, .error = {}};
}

}  // namespace

bool CppProjectReport::ok() const noexcept {
    for (const CppProjectFileOutcome& outcome : files) {
        if (outcome.state == CppProjectWrite::Failed) {
            return false;
        }
    }
    return true;
}

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

CppProjectReport writeCppProject(std::string_view projectRootUtf8, std::string_view projectName) {
    const std::array<CppProjectFile, CPP_PROJECT_FILE_COUNT> rendered = renderCppProject(projectName);
    CppProjectReport report;
    for (std::size_t i = 0; i < CPP_PROJECT_FILE_COUNT; ++i) {
        if (projectRootUtf8.empty()) {
            report.files[i] = {
                .relativePath = rendered[i].relativePath, .state = CppProjectWrite::Failed, .error = "no project root"};
        } else {
            report.files[i] = writeOne(projectRootUtf8, rendered[i]);
        }
    }
    return report;
}

}  // namespace engine::editor

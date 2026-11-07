// Aero Engine -- task 4.5.2: writes the C++ project template into the folder named by argv[1], so
// tests/sdk/template.cmake can build and run the result against the installed prefix. Linked with aero::core alone: the
// SDK build has no editor.
#include <aero/editor/cpp_project_template.hpp>
#include <aero/editor/text_file.hpp>

#include <cstdio>
#include <string>
#include <string_view>

namespace {

// In the source, never in argv: Windows re-encodes a command line through the code page. The escapes spell the name
// "Cube Demo n-tilde" as UTF-8 and keep this file 7-bit.
constexpr std::string_view DEMO_NAME = "Cube Demo \xC3\xB1";

[[nodiscard]] const char* stateName(engine::editor::CppProjectWrite state) noexcept {
    switch (state) {
        case engine::editor::CppProjectWrite::Written:
            return "written";
        case engine::editor::CppProjectWrite::Unchanged:
            return "unchanged";
        case engine::editor::CppProjectWrite::Failed:
            break;
    }
    return "failed";
}

int run(const std::string& root) {
    if (const std::string error = engine::editor::ensureDirectory(root); !error.empty()) {
        std::fprintf(stderr, "cannot create %s: %s\n", root.c_str(), error.c_str());
        return 1;
    }
    const engine::editor::CppProjectReport report = engine::editor::writeCppProject(root, DEMO_NAME);
    for (const engine::editor::CppProjectFileOutcome& file : report.files) {
        const std::string path(file.relativePath);
        std::printf("%s %s %s\n", stateName(file.state), path.c_str(), file.error.c_str());
    }
    return report.ok() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <project root>\n", argv[0]);
        return 2;
    }
    try {
        return run(argv[1]);
    } catch (...) {
        std::fprintf(stderr, "the writer failed on an exception\n");
        return 1;
    }
}

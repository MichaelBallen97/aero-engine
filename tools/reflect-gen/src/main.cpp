// tools/reflect-gen/src/main.cpp — task 1.1.1, step S1: the discovery + parse SPINE only.
//
// This is deliberately minimal (plan S1): --help/--version plus enough of the createIndex ->
// parseTranslationUnit -> visitChildren flow to prove libclang 18 discovery, the keg-only rpath, and
// the F4 sysroot path in isolation, BEFORE the full D4 argv grammar / exit codes / RAII discipline are
// fleshed out in S2. Do not judge this file's argv handling or naming polish yet — S2 replaces it
// wholesale.

#include <clang-c/Index.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr const char* TOOL_VERSION = "0.1.0";
}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("aero_reflect_gen %s -- libclang parse+walk harness (task 1.1.1, S1 spine)\n", TOOL_VERSION);
            return 0;
        }
        if (std::strcmp(argv[i], "--version") == 0) {
            CXString clangVersion = clang_getClangVersion();
            std::printf("aero_reflect_gen %s (%s)\n", TOOL_VERSION, clang_getCString(clangVersion));
            clang_disposeString(clangVersion);
            return 0;
        }
    }

    if (argc < 2) {
        std::fprintf(stderr, "aero_reflect_gen: error: missing <input> (S1 spine takes exactly one path)\n");
        return 1;
    }

    // Everything from argv[2] onward is forwarded verbatim to libclang (the eventual `-- <clang args>`
    // convention; S1 does not yet parse a `--` separator).
    std::vector<const char*> clangArgs;
    for (int i = 2; i < argc; ++i) {
        clangArgs.push_back(argv[i]);
    }

    CXIndex index = clang_createIndex(0, 0);
    CXTranslationUnit tu =
        clang_parseTranslationUnit(index, argv[1], clangArgs.data(), static_cast<int>(clangArgs.size()), nullptr, 0,
                                   CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_KeepGoing);
    if (tu == nullptr) {
        std::fprintf(stderr, "aero_reflect_gen: error: failed to parse '%s'\n", argv[1]);
        clang_disposeIndex(index);
        return 2;
    }

    const unsigned numDiagnostics = clang_getNumDiagnostics(tu);
    std::fprintf(stderr, "aero_reflect_gen: %u diagnostic(s)\n", numDiagnostics);
    for (unsigned i = 0; i < numDiagnostics; ++i) {
        CXDiagnostic diagnostic = clang_getDiagnostic(tu, i);
        CXString formatted = clang_formatDiagnostic(diagnostic, clang_defaultDiagnosticDisplayOptions());
        std::fprintf(stderr, "  %s\n", clang_getCString(formatted));
        clang_disposeString(formatted);
        clang_disposeDiagnostic(diagnostic);
    }

    clang_visitChildren(
        clang_getTranslationUnitCursor(tu),
        [](CXCursor cursor, CXCursor, CXClientData) {
            CXSourceLocation location = clang_getCursorLocation(cursor);
            if (clang_Location_isFromMainFile(location) == 0) {
                return CXChildVisit_Continue;
            }
            unsigned line = 0;
            unsigned column = 0;
            clang_getSpellingLocation(location, nullptr, &line, &column, nullptr);
            CXString kindSpelling = clang_getCursorKindSpelling(clang_getCursorKind(cursor));
            CXString cursorSpelling = clang_getCursorSpelling(cursor);
            std::printf("%s '%s' @%u:%u\n", clang_getCString(kindSpelling), clang_getCString(cursorSpelling), line,
                        column);
            clang_disposeString(cursorSpelling);
            clang_disposeString(kindSpelling);
            return CXChildVisit_Recurse;
        },
        nullptr);

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return 0;
}

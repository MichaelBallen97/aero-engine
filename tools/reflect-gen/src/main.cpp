// tools/reflect-gen/src/main.cpp — task 1.1.1: aero_reflect_gen, the first-party libclang parse+walk
// harness. This is the FROZEN CLI contract (plan/spec D4) that 1.1.2-1.1.4 extend but never break —
// see README.md for the full rationale.
//
//   aero_reflect_gen [--all] [--main-file-only] [--version] [--help] <input> [-- <clang args>...]
//
// <input> is the translation unit to parse. Everything after `--` is forwarded verbatim to libclang
// (-std=, -I, -isysroot, -D, ...). --main-file-only (the default) limits the AST walk to cursors
// physically in <input>; --all includes cursors from every included header (wins if both are given).
// --version/--help short-circuit before any parse. Output: an indented AST walk to stdout, one line
// per cursor; diagnostics and errors go to stderr only.
//
// Exit codes: 0 parsed with zero error/fatal diagnostics (warnings allowed), 1 usage error, 2 parse
// failure (null TU, or >=1 error/fatal diagnostic), 3 I/O error (<input> unreadable).
//
// Freestanding tool: no aero:: engine headers, no aero::core link (spec D5) -- only libclang's stable
// C API (<clang-c/Index.h>, never a C++ Clang/LLVM header, spec F3) and the standard library, so
// tools/ never drags spdlog/GLM/vcpkg usage requirements (or R12's shared include root) into a
// build-time-only binary. That C-API-only boundary is also what keeps a future std::meta migration
// (ADR-004) local to this one file.

#include <clang-c/Index.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::string_view TOOL_VERSION = "0.1.0";

enum class ExitCode : std::uint8_t {
    Success = 0,
    UsageError = 1,
    ParseError = 2,
    IoError = 3,
};

// --- argv grammar (frozen, D4) -------------------------------------------------------------------

void printUsage(std::ostream& out) {
    out << "aero_reflect_gen " << TOOL_VERSION << " -- libclang parse+walk harness (task 1.1.1)\n"
        << "\n"
        << "Usage:\n"
        << "  aero_reflect_gen [--all] [--main-file-only] <input> [-- <clang args>...]\n"
        << "  aero_reflect_gen --version\n"
        << "  aero_reflect_gen --help\n"
        << "\n"
        << "  <input>              Translation unit to parse.\n"
        << "  -- <clang args>...   Forwarded verbatim to libclang (-std=, -I, -isysroot, -D, ...).\n"
        << "  --main-file-only     (default) Limit the AST walk to cursors physically in <input>.\n"
        << "  --all                Include cursors from every included header (wins if both given).\n"
        << "  --version            Print the tool version and clang_getClangVersion(); exit 0.\n"
        << "  --help               Print this usage; exit 0.\n"
        << "\n"
        << "Output: an indented AST walk to stdout, one line per cursor. Diagnostics/errors: stderr only.\n"
        << "\n"
        << "Exit codes: 0 parsed clean (warnings allowed), 1 usage error, 2 parse failure, "
        << "3 I/O error (<input> unreadable).\n";
}

// Converts a CXString to a std::string and disposes it immediately -- every CXString this tool
// touches is clang_disposeString'd right after clang_getCString, with no path that skips it.
std::string toStdString(CXString clangString) {
    const char* cstr = clang_getCString(clangString);
    std::string result = (cstr != nullptr) ? std::string(cstr) : std::string();
    clang_disposeString(clangString);
    return result;
}

void printVersion(std::ostream& out) {
    out << "aero_reflect_gen " << TOOL_VERSION << " (" << toStdString(clang_getClangVersion()) << ")\n";
}

struct Args {
    std::string input;
    bool wantAll = false;
    bool wantMainFileOnly = false;
    bool wantHelp = false;
    bool wantVersion = false;
    std::vector<const char*> clangArgs;  // everything after `--`, forwarded verbatim (points into argv)
};

// Hand-rolled argv parse (D4 flow step 1; no getopt -- Windows has none). Any usage violation prints
// nothing itself (the caller prints usage to stderr) and returns nullopt.
std::optional<Args> parseArgs(int argc, char** argv) {
    Args args;

    // --help/--version short-circuit: found anywhere BEFORE `--`, they win immediately (common CLI
    // ergonomics, matching aero_shaderc's precedent). Never inspect tokens after `--` -- those are
    // opaque, verbatim clang arguments that must not influence tool-flag parsing.
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--") {
            break;
        }
        if (a == "--help") {
            args.wantHelp = true;
            return args;
        }
        if (a == "--version") {
            args.wantVersion = true;
            return args;
        }
    }

    bool haveInput = false;
    int i = 1;
    for (; i < argc; ++i) {
        const std::string_view token = argv[i];
        if (token == "--") {
            ++i;
            break;
        }
        if (token == "--all") {
            args.wantAll = true;
        } else if (token == "--main-file-only") {
            args.wantMainFileOnly = true;
        } else if (!token.empty() && token.front() == '-') {
            std::cerr << "aero_reflect_gen: error: unknown flag '" << token << "'\n";
            return std::nullopt;
        } else if (haveInput) {
            std::cerr << "aero_reflect_gen: error: <input> given twice ('" << args.input << "' and '" << token
                      << "')\n";
            return std::nullopt;
        } else {
            args.input = std::string(token);
            haveInput = true;
        }
    }
    for (; i < argc; ++i) {
        args.clangArgs.push_back(argv[i]);
    }

    if (!haveInput) {
        std::cerr << "aero_reflect_gen: error: missing <input>\n";
        return std::nullopt;
    }
    return args;
}

// Must exist, be a regular file, and be openable for read -- checked BEFORE libclang is invoked at
// all, so a missing/unreadable <input> gets the honest I/O exit code (3) rather than being folded
// into the null-TU parse-failure exit code (2, E9's distinction).
bool isReadableFile(const std::string& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) {
        return false;
    }
    const std::ifstream probe(path, std::ios::binary);
    return probe.good();
}

// --- RAII for libclang handles --------------------------------------------------------------------
//
// Non-copyable, non-movable: runMain() constructs exactly one of each and lets ordinary C++ scope
// rules release them -- no ownership transfer is ever needed for a single straight-line CLI flow, so
// move/copy semantics would be needless complexity. Every early `return` in runMain() after
// construction still runs these destructors (that IS the RAII guarantee), keeping ASan clean for our
// own code (libclang's own exit-time leaks are a separate, documented ASAN_OPTIONS concern in the
// test driver, not something these guards need to solve).

// Disposes via clang_disposeIndex. Declared BEFORE TranslationUnitGuard in runMain() on purpose: C++
// destroys locals in REVERSE declaration order, so that ordering alone guarantees
// clang_disposeTranslationUnit runs before clang_disposeIndex -- the required dispose order -- with
// no extra bookkeeping.
class IndexGuard {
public:
    explicit IndexGuard(CXIndex handle) noexcept : index(handle) {}
    IndexGuard(const IndexGuard&) = delete;
    IndexGuard& operator=(const IndexGuard&) = delete;
    IndexGuard(IndexGuard&&) = delete;
    IndexGuard& operator=(IndexGuard&&) = delete;
    ~IndexGuard() {
        if (index != nullptr) {
            clang_disposeIndex(index);
        }
    }

    [[nodiscard]] CXIndex get() const noexcept { return index; }

private:
    CXIndex index;
};

// Disposes via clang_disposeTranslationUnit. Only ever constructed once clang_parseTranslationUnit has
// returned a non-null TU (runMain checks first) -- see IndexGuard's comment for the dispose-order
// guarantee this relies on.
class TranslationUnitGuard {
public:
    explicit TranslationUnitGuard(CXTranslationUnit handle) noexcept : tu(handle) {}
    TranslationUnitGuard(const TranslationUnitGuard&) = delete;
    TranslationUnitGuard& operator=(const TranslationUnitGuard&) = delete;
    TranslationUnitGuard(TranslationUnitGuard&&) = delete;
    TranslationUnitGuard& operator=(TranslationUnitGuard&&) = delete;
    ~TranslationUnitGuard() {
        if (tu != nullptr) {
            clang_disposeTranslationUnit(tu);
        }
    }

    [[nodiscard]] CXTranslationUnit get() const noexcept { return tu; }

private:
    CXTranslationUnit tu;
};

// --- the AST walk (D4/C.4) -------------------------------------------------------------------------

struct WalkState {
    int depth = 0;
    bool allFiles = false;
};

// Manual recursion (rather than returning CXChildVisit_Recurse off one shared depth counter) is what
// lets each level print at its OWN depth: a fresh WalkState is built per call with depth+1 and handed
// to a nested clang_visitChildren for this cursor's children, then this call always returns
// ...Continue (both to move on to siblings after a printed cursor, and to skip a filtered-out
// cursor's whole subtree without descending into it).
CXChildVisitResult visitCursor(CXCursor cursor, CXCursor /*parent*/, CXClientData clientData) {
    auto* state = static_cast<WalkState*>(clientData);
    const CXSourceLocation location = clang_getCursorLocation(cursor);
    if (!state->allFiles && clang_Location_isFromMainFile(location) == 0) {
        return CXChildVisit_Continue;  // skip this cursor's WHOLE subtree; keep visiting siblings
    }

    unsigned line = 0;
    unsigned column = 0;
    clang_getSpellingLocation(location, nullptr, &line, &column, nullptr);

    const std::string kindSpelling = toStdString(clang_getCursorKindSpelling(clang_getCursorKind(cursor)));
    const std::string cursorSpelling = toStdString(clang_getCursorSpelling(cursor));

    for (int i = 0; i < state->depth; ++i) {
        std::cout << "  ";
    }
    std::cout << kindSpelling << " '" << cursorSpelling << "' @" << line << ':' << column << '\n';

    WalkState child{.depth = state->depth + 1, .allFiles = state->allFiles};
    clang_visitChildren(cursor, visitCursor, &child);
    return CXChildVisit_Continue;
}

}  // namespace

ExitCode runMain(int argc, char** argv) {
    const std::optional<Args> parsed = parseArgs(argc, argv);
    if (!parsed.has_value()) {
        printUsage(std::cerr);
        return ExitCode::UsageError;
    }
    const Args& args = *parsed;

    if (args.wantHelp) {
        printUsage(std::cout);
        return ExitCode::Success;
    }
    if (args.wantVersion) {
        printVersion(std::cout);
        return ExitCode::Success;
    }

    // --- step 2: stat <input> BEFORE libclang is invoked at all (E9's exit-3-vs-2 distinction) -----
    if (!isReadableFile(args.input)) {
        std::cerr << "aero_reflect_gen: error: cannot read input file '" << args.input << "'\n";
        return ExitCode::IoError;
    }

    // --- step 3/8: CXIndex, RAII-disposed on every path (declared before the TU guard on purpose) --
    const IndexGuard indexGuard(clang_createIndex(0, 0));

    // --- step 4: parse ------------------------------------------------------------------------------
    CXTranslationUnit tu = clang_parseTranslationUnit(
        indexGuard.get(), args.input.c_str(), args.clangArgs.data(), static_cast<int>(args.clangArgs.size()), nullptr,
        0, CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_KeepGoing);
    if (tu == nullptr) {
        std::cerr << "aero_reflect_gen: error: failed to parse '" << args.input << "'\n";
        return ExitCode::ParseError;
    }
    const TranslationUnitGuard tuGuard(tu);

    // --- step 5: diagnostics pass -- stderr only; warnings tolerated (F5) ---------------------------
    auto maxSeverity = static_cast<int>(CXDiagnostic_Ignored);
    const unsigned numDiagnostics = clang_getNumDiagnostics(tu);
    for (unsigned i = 0; i < numDiagnostics; ++i) {
        CXDiagnostic diagnostic = clang_getDiagnostic(tu, i);
        std::cerr << toStdString(clang_formatDiagnostic(diagnostic, clang_defaultDiagnosticDisplayOptions())) << '\n';
        maxSeverity = std::max(maxSeverity, static_cast<int>(clang_getDiagnosticSeverity(diagnostic)));
        clang_disposeDiagnostic(diagnostic);
    }

    // --- step 6: AST walk -- stdout only -------------------------------------------------------------
    WalkState state{.depth = 0, .allFiles = args.wantAll};
    clang_visitChildren(clang_getTranslationUnitCursor(tu), visitCursor, &state);

    // --- step 7: verdict ------------------------------------------------------------------------------
    return (maxSeverity >= static_cast<int>(CXDiagnostic_Error)) ? ExitCode::ParseError : ExitCode::Success;
}

// The real entry point is a thin, non-throwing wrapper (docs/04: no exceptions across a public API
// boundary -- main() is this freestanding tool's outermost one). std::filesystem/std::string/etc. can
// theoretically throw (bad_alloc, filesystem_error) in extreme conditions runMain() does not
// individually guard against; catching here turns any such escape into a clean diagnostic + exit code
// instead of an uncaught-exception abort.
int main(int argc, char** argv) {
    try {
        return static_cast<int>(runMain(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "aero_reflect_gen: error: unexpected exception: " << e.what() << '\n';
        return static_cast<int>(ExitCode::IoError);
    } catch (...) {
        std::cerr << "aero_reflect_gen: error: unexpected exception\n";
        return static_cast<int>(ExitCode::IoError);
    }
}

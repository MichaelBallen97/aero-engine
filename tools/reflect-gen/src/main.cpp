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
#include <utility>
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
        << "  aero_reflect_gen [--all] [--main-file-only] [--components] [--emit-meta] [-o <file>] <input> "
        << "[-- <clang args>...]\n"
        << "  aero_reflect_gen --version\n"
        << "  aero_reflect_gen --help\n"
        << "\n"
        << "  <input>              Translation unit to parse.\n"
        << "  -- <clang args>...   Forwarded verbatim to libclang (-std=, -I, -isysroot, -D, ...).\n"
        << "  --main-file-only     (default) Limit the AST walk to cursors physically in <input>.\n"
        << "  --all                Include cursors from every included header (wins if both given).\n"
        << "  --components         List detected engine::component structs/classes and fields, not the raw AST walk.\n"
        << "  --emit-meta          Emit entt::meta registration C++ for detected components, instead of the AST walk.\n"
        << "  -o <file>            Write --emit-meta output to <file> (default: stdout).\n"
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
    bool wantComponents = false;
    bool wantEmitMeta = false;  // task 1.1.3: emit entt::meta registration instead of walking
    bool wantHelp = false;
    bool wantVersion = false;
    std::optional<std::string> outputPath;  // task 1.1.3: -o <file>; nullopt => stdout
    std::vector<const char*> clangArgs;     // everything after `--`, forwarded verbatim (points into argv)
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
        } else if (token == "--components") {
            args.wantComponents = true;
        } else if (token == "--emit-meta") {
            args.wantEmitMeta = true;
        } else if (token == "-o") {
            if (i + 1 >= argc) {  // bounds check: missing operand => usage error
                std::cerr << "aero_reflect_gen: error: '-o' requires a file path\n";
                return std::nullopt;
            }
            args.outputPath = std::string(argv[++i]);  // the for-loop's ++i then skips past the operand
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

// ---- task 1.1.2: reflection model (spec D9) -------------------------------------------------------
enum class FieldCategory : std::uint8_t { Primitive, Vec3, Quat, Unsupported };

struct Field {
    std::string name;
    std::string typeName;
    FieldCategory category = FieldCategory::Unsupported;
};

struct Component {
    std::string qualifiedName;
    unsigned line = 0;
    unsigned column = 0;
    std::vector<Field> fields;
};

// Strip a leading elaborated-type keyword ("struct "/"class ") a record spelling may carry under
// MSVC-compat libclang, so the reported type name is host-invariant (no-op on macOS/Linux). Applied to
// BOTH the canonical spelling (classification) and the as-written spelling (display) so they never
// disagree, and to pre-empt the Windows verify-at-implementation point (spec Sec 3.9).
std::string stripElaboratedKeyword(std::string spelling) {
    constexpr std::string_view STRUCT_KW = "struct ";
    constexpr std::string_view CLASS_KW = "class ";
    if (spelling.starts_with(STRUCT_KW)) {
        spelling.erase(0, STRUCT_KW.size());
    } else if (spelling.starts_with(CLASS_KW)) {
        spelling.erase(0, CLASS_KW.size());
    }
    return spelling;
}

// Classify a field's type against ADR-004's minimal subset. Match on the CANONICAL type so a
// using/typedef alias still resolves (D6). The builtin range [CXType_Bool, CXType_LongDouble] is the
// whole fundamental-arithmetic set (bool, char family, short/int/long/long long signed+unsigned,
// float, double, long double) -- verified F4.
FieldCategory classifyField(CXType fieldType) {
    const CXType canonical = clang_getCanonicalType(fieldType);
    if (canonical.kind >= CXType_Bool && canonical.kind <= CXType_LongDouble) {
        return FieldCategory::Primitive;
    }
    const std::string spelling = stripElaboratedKeyword(toStdString(clang_getTypeSpelling(canonical)));
    if (spelling == "engine::Vec3") {
        return FieldCategory::Vec3;
    }
    if (spelling == "engine::Quat") {
        return FieldCategory::Quat;
    }
    return FieldCategory::Unsupported;
}

std::string_view categoryTag(FieldCategory category) {
    switch (category) {
        case FieldCategory::Primitive:
            return "primitive";
        case FieldCategory::Vec3:
            return "vec3";
        case FieldCategory::Quat:
            return "quat";
        case FieldCategory::Unsupported:
            return "unsupported";
    }
    return "unsupported";  // unreachable; satisfies -Wreturn-type
}

// One-level child visit: does this record carry the component annotate marker? (F2)
CXChildVisitResult annotateMarkerVisitor(CXCursor cursor, CXCursor /*parent*/, CXClientData clientData) {
    if (clang_getCursorKind(cursor) == CXCursor_AnnotateAttr &&
        toStdString(clang_getCursorSpelling(cursor)) == "engine::component") {
        *static_cast<bool*>(clientData) = true;
        return CXChildVisit_Break;
    }
    return CXChildVisit_Continue;  // stay at direct-child level (presence, not recursion)
}

bool hasComponentAnnotation(CXCursor record) {
    bool found = false;
    clang_visitChildren(record, annotateMarkerVisitor, &found);
    return found;
}

// Qualified name via the semantic-parent walk (namespaces + enclosing records), joined with "::".
std::string buildQualifiedName(CXCursor cursor) {
    std::vector<std::string> parts;
    parts.push_back(toStdString(clang_getCursorSpelling(cursor)));
    CXCursor parent = clang_getCursorSemanticParent(cursor);
    while (clang_Cursor_isNull(parent) == 0 && clang_getCursorKind(parent) != CXCursor_TranslationUnit) {
        std::string name = toStdString(clang_getCursorSpelling(parent));
        if (!name.empty()) {  // skip anonymous namespaces
            parts.push_back(std::move(name));
        }
        parent = clang_getCursorSemanticParent(parent);
    }
    std::reverse(parts.begin(), parts.end());  // was built innermost-first; join outermost-first
    std::string result;
    for (const auto& part : parts) {
        if (!result.empty()) {
            result += "::";
        }
        result += part;
    }
    return result;
}

// Collect a component's non-static data members (FieldDecl only -> excludes statics/methods/nested,
// E11) in declaration/source order (AC-9).
CXChildVisitResult fieldVisitor(CXCursor cursor, CXCursor /*parent*/, CXClientData clientData) {
    if (clang_getCursorKind(cursor) == CXCursor_FieldDecl) {
        auto* fields = static_cast<std::vector<Field>*>(clientData);
        const CXType type = clang_getCursorType(cursor);
        fields->push_back(Field{
            .name = toStdString(clang_getCursorSpelling(cursor)),
            .typeName = stripElaboratedKeyword(toStdString(clang_getTypeSpelling(type))),  // as-written
            .category = classifyField(type),                                               // canonical classify
        });
    }
    return CXChildVisit_Continue;
}

struct DetectState {
    bool allFiles = false;
    std::vector<Component>* components = nullptr;
};

CXChildVisitResult detectVisitor(CXCursor cursor, CXCursor /*parent*/, CXClientData clientData) {
    auto* state = static_cast<DetectState*>(clientData);
    const CXSourceLocation location = clang_getCursorLocation(cursor);
    if (!state->allFiles && clang_Location_isFromMainFile(location) == 0) {
        return CXChildVisit_Continue;  // F5: skip non-main-file subtrees (the ~400 stdlib records)
    }
    const CXCursorKind kind = clang_getCursorKind(cursor);
    const bool isRecord = (kind == CXCursor_StructDecl || kind == CXCursor_ClassDecl);
    if (isRecord && clang_isCursorDefinition(cursor) != 0 && hasComponentAnnotation(cursor)) {
        Component component;
        component.qualifiedName = buildQualifiedName(cursor);
        clang_getSpellingLocation(location, nullptr, &component.line, &component.column, nullptr);
        clang_visitChildren(cursor, fieldVisitor, &component.fields);
        state->components->push_back(std::move(component));
        return CXChildVisit_Continue;  // flat model: don't descend into a detected component
    }
    return CXChildVisit_Recurse;  // descend into namespaces / non-component records to find nested (E4)
}

// Stdout = listing only; warnings to stderr (1.1.1 stream discipline). Source-order traversal =>
// deterministic (AC-7).
void emitComponents(const std::vector<Component>& components) {
    for (const Component& component : components) {
        std::cout << "component " << component.qualifiedName << " @" << component.line << ':' << component.column
                  << '\n';
        for (const Field& field : component.fields) {
            std::cout << "  field " << field.name << " : " << field.typeName << " [" << categoryTag(field.category)
                      << "]\n";
            if (field.category == FieldCategory::Unsupported) {  // lenient: warn, never fail (D7)
                std::cerr << "aero_reflect_gen: warning: " << component.qualifiedName << '.' << field.name << " : "
                          << field.typeName << " is not in the reflectable subset (primitives + Vec3/Quat)\n";
            }
        }
    }
}

// ---- task 1.1.3: entt::meta codegen (spec D3/D4/D5/D7) --------------------------------------------

// Map every char outside [A-Za-z0-9_] to '_', prefixing '_' if the result would start with a digit, so
// the generated register-fn name is a valid, deterministic C++ identifier derived from the input file
// stem (D7).
std::string sanitizeIdentifier(std::string_view stem) {
    std::string result;
    result.reserve(stem.size());
    for (const char c : stem) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        result.push_back(ok ? c : '_');
    }
    if (result.empty() || (result.front() >= '0' && result.front() <= '9')) {
        result.insert(result.begin(), '_');
    }
    return result;
}

// Serialize the detected components as a compilable entt::meta registration TU (D7). Writes generated
// C++ to `out`; unsupported fields become a `// skipped:` line + the SAME stderr warning 1.1.2 emits
// (D4). No addresses, timestamps, or absolute paths (source #include is a basename) => byte-identical
// across runs (AC-7).
void emitMeta(const std::vector<Component>& components, const std::string& inputPath, std::ostream& out) {
    const std::string basename = fs::path(inputPath).filename().string();
    const std::string stem = fs::path(inputPath).stem().string();
    const std::string registerFn = "aero_reflect_register_" + sanitizeIdentifier(stem);

    out << "// GENERATED by aero_reflect_gen --emit-meta — DO NOT EDIT.\n"
        << "// source: " << basename << "\n"
        << "#include <entt/meta/factory.hpp>\n"
        << "#include <entt/core/hashed_string.hpp>\n"
        << "\n"
        << "#include \"" << basename << "\"\n"
        << "\n"
        << "void " << registerFn << "() {\n"
        << "    using namespace entt::literals;\n";

    if (components.empty()) {
        out << "    // no engine::component annotations detected\n";
    }

    for (const Component& component : components) {
        const std::string& qn = component.qualifiedName;
        out << "    entt::meta_factory<" << qn << ">{}\n"
            << "        .type(\"" << qn << "\"_hs, \"" << qn << "\")";
        for (const Field& field : component.fields) {  // pass 1: supported members -> the .data chain
            if (field.category != FieldCategory::Unsupported) {
                out << "\n        .data<&" << qn << "::" << field.name << ">(\"" << field.name << "\"_hs, \""
                    << field.name << "\")";
            }
        }
        out << ";\n";
        for (const Field& field : component.fields) {  // pass 2: unsupported -> comment + stderr warning
            if (field.category == FieldCategory::Unsupported) {
                out << "    // skipped: " << field.name << " (" << field.typeName << " — unsupported)\n";
                std::cerr << "aero_reflect_gen: warning: " << qn << '.' << field.name << " : " << field.typeName
                          << " is not in the reflectable subset (primitives + Vec3/Quat)\n";
            }
        }
    }
    out << "}\n";
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

    // --- step 4: parse (inject the reflection marker at the FRONT, D3) -----------------------------
    // AERO_COMPONENT expands to clang::annotate iff AERO_REFLECT_PARSE is defined; define it for THIS
    // tool's parse so no caller manages it. Verified inert for the literal-attr fixtures (AC-8).
    std::vector<const char*> effectiveArgs;
    effectiveArgs.reserve(args.clangArgs.size() + 1);
    effectiveArgs.push_back("-DAERO_REFLECT_PARSE=1");
    effectiveArgs.insert(effectiveArgs.end(), args.clangArgs.begin(), args.clangArgs.end());

    CXTranslationUnit tu = clang_parseTranslationUnit(
        indexGuard.get(), args.input.c_str(), effectiveArgs.data(), static_cast<int>(effectiveArgs.size()), nullptr, 0,
        CXTranslationUnit_SkipFunctionBodies | CXTranslationUnit_KeepGoing);
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

    // --- step 6: emit (--emit-meta) / detect (--components) / walk -- stdout|-o for emit, stdout else ------
    if (args.wantEmitMeta) {
        // Generate ONLY for a clean parse: on error/fatal diagnostics fall through to the exit-2 verdict
        // (step 7) WITHOUT running detection or opening -o, so a parse failure leaves NO output file (AC-8).
        if (maxSeverity < static_cast<int>(CXDiagnostic_Error)) {
            std::vector<Component> components;
            DetectState detect{.allFiles = args.wantAll, .components = &components};
            clang_visitChildren(clang_getTranslationUnitCursor(tu), detectVisitor, &detect);
            if (args.outputPath.has_value()) {
                std::ofstream out(*args.outputPath, std::ios::trunc);  // TEXT mode (see note) — truncate + write
                if (!out) {
                    std::cerr << "aero_reflect_gen: error: cannot open output file '" << *args.outputPath << "'\n";
                    return ExitCode::IoError;  // E-output-io: exit 3
                }
                emitMeta(components, args.input, out);
                out.flush();
                if (!out) {
                    std::cerr << "aero_reflect_gen: error: failed writing output file '" << *args.outputPath << "'\n";
                    return ExitCode::IoError;
                }
            } else {
                emitMeta(components, args.input, std::cout);
            }
        }
    } else if (args.wantComponents) {
        std::vector<Component> components;
        DetectState detect{.allFiles = args.wantAll, .components = &components};
        clang_visitChildren(clang_getTranslationUnitCursor(tu), detectVisitor, &detect);
        emitComponents(components);
    } else {
        WalkState state{.depth = 0, .allFiles = args.wantAll};
        clang_visitChildren(clang_getTranslationUnitCursor(tu), visitCursor, &state);
    }

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

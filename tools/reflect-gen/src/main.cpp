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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
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
        << "  aero_reflect_gen [--all] [--main-file-only] [--components] [--emit-meta] [--emit-json] [-o <file>] "
        << "[--depfile <file>] <input> [-- <clang args>...]\n"
        << "  aero_reflect_gen --version\n"
        << "  aero_reflect_gen --help\n"
        << "\n"
        << "  <input>              Translation unit to parse.\n"
        << "  -- <clang args>...   Forwarded verbatim to libclang (-std=, -I, -isysroot, -D, ...).\n"
        << "  --main-file-only     (default) Limit the AST walk to cursors physically in <input>.\n"
        << "  --all                Include cursors from every included header (wins if both given).\n"
        << "  --components         List detected engine::component structs/classes and fields, not the raw AST walk.\n"
        << "  --emit-meta          Emit entt::meta registration C++ for detected components, instead of the AST walk.\n"
        << "  --emit-json          Emit JSON serializers (aeroWriteJson + aeroReadJson per component) to stdout or "
        << "-o. Mutually exclusive with --emit-meta.\n"
        << "  -o <file>            Write --emit-meta/--emit-json output to <file> (default: stdout).\n"
        << "  --depfile <file>     With -o and one of --emit-meta/--emit-json: write a Makefile-format depfile of "
        << "the parse's #include closure.\n"
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
    bool wantEmitJson = false;  // task 1.2.1: emit JSON serializers (mutually exclusive with --emit-meta, D8)
    bool wantHelp = false;
    bool wantVersion = false;
    std::optional<std::string> outputPath;   // task 1.1.3: -o <file>; nullopt => stdout
    std::optional<std::string> depfilePath;  // task 1.1.4: --depfile <file>; requires --emit-meta + -o (D6)
    std::vector<const char*> clangArgs;      // everything after `--`, forwarded verbatim (points into argv)
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
        } else if (token == "--emit-json") {  // NEW (task 1.2.1)
            args.wantEmitJson = true;
        } else if (token == "-o") {
            if (i + 1 >= argc) {  // bounds check: missing operand => usage error
                std::cerr << "aero_reflect_gen: error: '-o' requires a file path\n";
                return std::nullopt;
            }
            args.outputPath = std::string(argv[++i]);  // the for-loop's ++i then skips past the operand
        } else if (token == "--depfile") {             // NEW — consumes the NEXT token (mirrors -o)
            if (i + 1 >= argc) {
                std::cerr << "aero_reflect_gen: error: '--depfile' requires a file path\n";
                return std::nullopt;  // missing operand => usage error (exit 1)
            }
            args.depfilePath = std::string(argv[++i]);  // last-wins on repeat (F4 parity, free)
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

    // D8: --emit-json and --emit-meta each emit a DIFFERENT artifact to one -o; requesting both is meaningless.
    if (args.wantEmitMeta && args.wantEmitJson) {
        std::cerr << "aero_reflect_gen: error: --emit-json and --emit-meta are mutually exclusive\n";
        return std::nullopt;  // usage error (exit 1)
    }

    // D6: a depfile's make-rule target IS the -o path, so --depfile is meaningful ONLY with -o AND
    // exactly one emit mode (mutual-exclusion above already rejects both). Fail fast (exit 1) BEFORE
    // any filesystem work rather than write a rule for a phantom target. (E6 / depfile_requires_* cases.)
    if (args.depfilePath && (!args.outputPath || !(args.wantEmitMeta || args.wantEmitJson))) {
        std::cerr << "aero_reflect_gen: error: --depfile requires -o and --emit-meta or --emit-json\n";
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
enum class FieldCategory : std::uint8_t { Primitive, Vec3, Quat, String, Unsupported };

struct Field {
    std::string name;
    std::string typeName;
    FieldCategory category = FieldCategory::Unsupported;
    bool isBool = false;  // Primitive refinement: engine::range never applies to bool (D7)
    bool hasRange = false;
    std::string rangeMin;  // validated numeric token, suffix-stripped ("0.0175", "0", "-1")
    std::string rangeMax;  // stored as TEXT, never re-formatted -- that is what keeps AC-4 byte-stable
    bool color = false;
};

struct Component {
    std::string qualifiedName;
    unsigned line = 0;
    unsigned column = 0;
    bool atNamespaceScope = true;  // false => nested in a record/function; --emit-json skips it
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
// using/typedef alias still resolves (D6). Task 1.2.2 (D11) replaces the old whole-builtin-range test
// with an EXPLICIT whitelist of the 18 CXTypeKinds serialize.hpp can actually widen/narrow: the old
// range [CXType_Bool, CXType_LongDouble] (21 kinds) also admitted `long double`, `__int128`, and
// `unsigned __int128`, for which the writer/reader have no viable overload (ambiguous float/double
// resolution for long double; std::is_integral_v<__int128> is false under strict -std=c++20) -- a
// component carrying one of those three used to generate NON-COMPILING code (F10). Those three now
// fall through to Unsupported like any other out-of-subset type, uniformly across all four consumers.
FieldCategory classifyField(CXType fieldType) {
    const CXType canonical = clang_getCanonicalType(fieldType);
    switch (canonical.kind) {
        case CXType_Bool:
        case CXType_Char_U:
        case CXType_UChar:
        case CXType_Char16:
        case CXType_Char32:
        case CXType_UShort:
        case CXType_UInt:
        case CXType_ULong:
        case CXType_ULongLong:
        case CXType_Char_S:
        case CXType_SChar:
        case CXType_WChar:
        case CXType_Short:
        case CXType_Int:
        case CXType_Long:
        case CXType_LongLong:
        case CXType_Float:
        case CXType_Double:
            return FieldCategory::Primitive;
        default:
            break;
    }
    const std::string spelling = stripElaboratedKeyword(toStdString(clang_getTypeSpelling(canonical)));
    if (spelling == "engine::Vec3") {
        return FieldCategory::Vec3;
    }
    if (spelling == "engine::Quat") {
        return FieldCategory::Quat;
    }
    // Task 2.2.2 (D3; plan decision O3, 2026-07-26). std::string, however THIS host's libclang prints
    // it. Verified against clang 18's default PrintingPolicy (UsePreferredNames=1,
    // SuppressInlineNamespace=1, SuppressDefaultTemplateArgs=1) plus a live libclang probe:
    //   macOS/libc++    -> "std::string"             (libc++'s _LIBCPP_PREFERRED_NAME(string))
    //   Linux/libstdc++ -> "std::basic_string<char>" (inline ns __cxx11 AND default args elided)
    //   Windows/MS STL  -> "std::basic_string<char>"
    // The third spelling is belt-and-braces if a host ever stops eliding default args.
    //
    // EXACT, NEVER A PREFIX. A prefix would also match
    // std::basic_string<char, MyTraits, std::pmr::polymorphic_allocator<char>> -- whose non-default
    // arguments are NOT elided, so it prints with them -- and serialize.hpp has no overload for it:
    // the generated TU would not compile. That is the long-double/__int128 class of bug the 1.2.2
    // whitelist rewrite exists to prevent. u8/u16/u32/wstring are excluded for free.
    //
    // If a lane ever prints a FOURTH spelling the field falls through to Unsupported -- tagged
    // [unsupported], one warning, exit 0, never a miscompile -- and reflect-gen.string_components reds
    // with the observed line dumped. The structural-match escalation (and its trigger condition) is
    // written out in the task plan's risk R1; do NOT apply it pre-emptively.
    if (spelling == "std::string" || spelling == "std::basic_string<char>" ||
        spelling == "std::basic_string<char, std::char_traits<char>, std::allocator<char>>") {
        return FieldCategory::String;
    }
    return FieldCategory::Unsupported;
}

// Task 2.2.2 (D7): whether a field's CANONICAL type is exactly `bool` -- engine::range never applies
// to a bool field (a checkbox has no numeric range), and this is the oracle fieldVisitor asks.
bool isBoolField(CXType fieldType) { return clang_getCanonicalType(fieldType).kind == CXType_Bool; }

std::string_view categoryTag(FieldCategory category) {
    switch (category) {
        case FieldCategory::Primitive:
            return "primitive";
        case FieldCategory::Vec3:
            return "vec3";
        case FieldCategory::Quat:
            return "quat";
        case FieldCategory::String:
            return "string";
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

// True when every semantic ancestor up to the translation unit is a namespace. A component nested
// inside a record (or a function) cannot be emitted by --emit-json: the "namespace" prefix its
// qualified name would produce names a struct/class, so the generated TU cannot compile (1.2 audit,
// finding 1). Detection still reports such components (1.1.2 E4) and --emit-meta still registers them
// (a fully-qualified template argument needs no namespace wrapping); only the JSON emitter skips.
bool isNamespaceScoped(CXCursor cursor) {
    CXCursor parent = clang_getCursorSemanticParent(cursor);
    while (clang_Cursor_isNull(parent) == 0 && clang_getCursorKind(parent) != CXCursor_TranslationUnit) {
        if (clang_getCursorKind(parent) != CXCursor_Namespace) {
            return false;
        }
        parent = clang_getCursorSemanticParent(parent);
    }
    return true;
}

// ---- task 2.2.2: field annotation collection (D7) -------------------------------------------------

struct FieldAnnotations {
    bool color = false;
    bool hasRange = false;
    std::string rangeMin;
    std::string rangeMax;
    std::vector<std::string> diagnostics;  // deferred: judged after classification (D7)
};

// One numeric literal token as AERO_RANGE stringized it: optional sign, optional ONE trailing f/F/l/L.
// strtod is the oracle -- it accepts 0, -1, 1e-3, 0x10, 3.1241 and rejects `lo`, `1 + 2`, `'a'`, "".
std::optional<std::string> parseRangeToken(std::string token) {
    if (!token.empty()) {
        const char last = token.back();
        if (last == 'f' || last == 'F' || last == 'l' || last == 'L') {
            token.pop_back();
        }
    }
    if (token.empty()) {
        return std::nullopt;
    }
    const char* begin = token.c_str();
    char* end = nullptr;
    (void)std::strtod(begin, &end);
    if (end != begin + token.size()) {
        return std::nullopt;  // must consume the WHOLE token
    }
    return token;
}

// "engine::range:<min>:<max>". Exactly one ':' may remain after the prefix -- a numeric literal cannot
// contain one, so any other arity is malformed by construction.
bool parseRangePayload(std::string_view payload, FieldAnnotations& out, std::string& reason) {
    const auto sep = payload.find(':');
    if (sep == std::string_view::npos || payload.find(':', sep + 1) != std::string_view::npos) {
        reason = "expected exactly two ':'-separated numeric literals";
        return false;
    }
    const std::optional<std::string> lo = parseRangeToken(std::string{payload.substr(0, sep)});
    const std::optional<std::string> hi = parseRangeToken(std::string{payload.substr(sep + 1)});
    if (!lo || !hi) {
        reason = "range bounds must be numeric literals";
        return false;
    }
    if (std::strtod(lo->c_str(), nullptr) > std::strtod(hi->c_str(), nullptr)) {
        reason = "range min is greater than max";
        return false;
    }
    out.hasRange = true;
    out.rangeMin = *lo;
    out.rangeMax = *hi;
    return true;
}

// One-level child visit over a FieldDecl -- the SAME presence pattern as annotateMarkerVisitor
// (Continue, never Recurse). The AnnotateAttr is the FIRST child, so this is cheap.
CXChildVisitResult fieldAnnotationVisitor(CXCursor cursor, CXCursor /*parent*/, CXClientData clientData) {
    if (clang_getCursorKind(cursor) != CXCursor_AnnotateAttr) {
        return CXChildVisit_Continue;
    }
    auto* out = static_cast<FieldAnnotations*>(clientData);
    const std::string spelling = toStdString(clang_getCursorSpelling(cursor));
    constexpr std::string_view ENGINE_PREFIX = "engine::";
    constexpr std::string_view RANGE_PREFIX = "engine::range:";
    if (spelling == "engine::color") {
        out->color = true;
    } else if (spelling.starts_with(RANGE_PREFIX)) {
        std::string reason;
        if (!parseRangePayload(std::string_view{spelling}.substr(RANGE_PREFIX.size()), *out, reason)) {
            out->diagnostics.push_back("malformed engine::range annotation (" + reason + ") -- ignored");
        }
    } else if (spelling.starts_with(ENGINE_PREFIX)) {
        out->diagnostics.push_back("unknown engine:: field annotation '" + spelling + "' -- ignored");
    }
    // A non-engine:: annotate belongs to some other tool: ignore SILENTLY (E7).
    return CXChildVisit_Continue;
}

// The 2-field state fieldVisitor needs: the field vector to append to, PLUS the owning component's
// qualified name for its warning messages (E5) -- detectVisitor computes qualifiedName BEFORE calling
// clang_visitChildren(cursor, fieldVisitor, ...), so a pointer into it outlives the whole walk.
struct FieldVisitState {
    std::vector<Field>* fields;
    const std::string* qualifiedName;
};

// Collect a component's non-static data members (FieldDecl only -> excludes statics/methods/nested,
// E11) in declaration/source order (AC-9), plus any field annotation (task 2.2.2). Applicability is
// judged AFTER classification (D7): engine::range only on a non-bool Primitive, engine::color only on
// a Vec3 -- anything else is dropped with a warning naming the field.
CXChildVisitResult fieldVisitor(CXCursor cursor, CXCursor /*parent*/, CXClientData clientData) {
    if (clang_getCursorKind(cursor) == CXCursor_FieldDecl) {
        auto* state = static_cast<FieldVisitState*>(clientData);
        const CXType type = clang_getCursorType(cursor);
        const FieldCategory category = classifyField(type);

        FieldAnnotations annotations;
        clang_visitChildren(cursor, fieldAnnotationVisitor, &annotations);

        Field field{
            .name = toStdString(clang_getCursorSpelling(cursor)),
            .typeName = stripElaboratedKeyword(toStdString(clang_getTypeSpelling(type))),  // as-written
            .category = category,                                                          // canonical classify
            .isBool = isBoolField(type),
        };

        for (const std::string& diagnostic : annotations.diagnostics) {
            std::cerr << "aero_reflect_gen: warning: " << *state->qualifiedName << '.' << field.name << ": "
                      << diagnostic << '\n';
        }
        if (annotations.hasRange) {
            if (category == FieldCategory::Primitive && !field.isBool) {
                field.hasRange = true;
                field.rangeMin = annotations.rangeMin;
                field.rangeMax = annotations.rangeMax;
            } else {
                std::cerr << "aero_reflect_gen: warning: " << *state->qualifiedName << '.' << field.name
                          << ": engine::range applies only to numeric scalar fields\n";
            }
        }
        if (annotations.color) {
            if (category == FieldCategory::Vec3) {
                field.color = true;
            } else {
                std::cerr << "aero_reflect_gen: warning: " << *state->qualifiedName << '.' << field.name
                          << ": engine::color applies only to Vec3 fields\n";
            }
        }

        state->fields->push_back(std::move(field));
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
        component.atNamespaceScope = isNamespaceScoped(cursor);
        clang_getSpellingLocation(location, nullptr, &component.line, &component.column, nullptr);
        FieldVisitState fieldState{.fields = &component.fields, .qualifiedName = &component.qualifiedName};
        clang_visitChildren(cursor, fieldVisitor, &fieldState);
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
                      << "]";
            if (field.hasRange) {
                std::cout << " [range " << field.rangeMin << ':' << field.rangeMax << "]";
            }
            if (field.color) {
                std::cout << " [color]";
            }
            std::cout << '\n';
            if (field.category == FieldCategory::Unsupported) {  // lenient: warn, never fail (D7)
                std::cerr << "aero_reflect_gen: warning: " << component.qualifiedName << '.' << field.name << " : "
                          << field.typeName
                          << " is not in the reflectable subset (primitives + Vec3/Quat/std::string)\n";
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

// Split a qualified name at its LAST "::" into {namespace, unqualifiedType}. "engine::demo::Light" ->
// {"engine::demo", "Light"}; "Player" -> {"", "Player"}. Used by --emit-json to wrap each serializer in its
// component's namespace so ADL resolves (D7). (--emit-meta did not need this — it used the fully-qualified name
// as a template argument.)
std::pair<std::string, std::string> splitQualifiedName(const std::string& qualifiedName) {
    const std::string::size_type pos = qualifiedName.rfind("::");
    if (pos == std::string::npos) {
        return {std::string{}, qualifiedName};
    }
    return {qualifiedName.substr(0, pos), qualifiedName.substr(pos + 2)};
}

// Serialize the detected components as a compilable entt::meta registration TU (D7). Writes generated
// C++ to `out`; unsupported fields become a `// skipped:` line + the SAME stderr warning 1.1.2 emits
// (D4). No addresses, timestamps, or absolute paths (source #include is a basename) => byte-identical
// across runs (AC-7).
void emitMeta(const std::vector<Component>& components, const std::string& inputPath, std::ostream& out) {
    const std::string basename = fs::path(inputPath).filename().string();
    const std::string stem = fs::path(inputPath).stem().string();
    const std::string registerFn = "aero_reflect_register_" + sanitizeIdentifier(stem);

    // Task 2.2.2 (D6): the annotations header is included ONLY when at least one custom will be
    // emitted, so every annotation-free consumer's generated bytes stay IDENTICAL -- including
    // reflect-gen.incremental_e2e's nested probe, which compiles generated meta with ONLY the EnTT
    // include root and would fail to find the header.
    const bool anyCustom = std::any_of(components.begin(), components.end(), [](const Component& c) {
        return std::any_of(c.fields.begin(), c.fields.end(), [](const Field& f) {
            return f.category != FieldCategory::Unsupported && (f.hasRange || f.color);
        });
    });

    out << "// GENERATED by aero_reflect_gen --emit-meta — DO NOT EDIT.\n"
        << "// source: " << basename << "\n"
        << "#include <entt/meta/factory.hpp>\n"
        << "#include <entt/core/hashed_string.hpp>\n";
    if (anyCustom) {
        out << "\n#include <aero/reflect/annotations.hpp>\n";
    }
    out << "\n"
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
            if (field.category == FieldCategory::Unsupported) {
                continue;
            }
            out << "\n        .data<&" << qn << "::" << field.name << ">(\"" << field.name << "\"_hs, \"" << field.name
                << "\")";
            if (field.hasRange || field.color) {  // task 2.2.2 (D6): sparse, per member
                const std::string rangeMin = field.hasRange ? field.rangeMin : "0.0";
                const std::string rangeMax = field.hasRange ? field.rangeMax : "0.0";
                out << "\n        .custom<engine::reflect::FieldUiMeta>(engine::reflect::FieldUiMeta{"
                    << ".hasRange = " << (field.hasRange ? "true" : "false") << ", .rangeMin = " << rangeMin
                    << ", .rangeMax = " << rangeMax << ", .color = " << (field.color ? "true" : "false") << "})";
            }
        }
        out << ";\n";
        for (const Field& field : component.fields) {  // pass 2: unsupported -> comment + stderr warning
            if (field.category == FieldCategory::Unsupported) {
                out << "    // skipped: " << field.name << " (" << field.typeName << " — unsupported)\n";
                std::cerr << "aero_reflect_gen: warning: " << qn << '.' << field.name << " : " << field.typeName
                          << " is not in the reflectable subset (primitives + Vec3/Quat/std::string)\n";
            }
        }
    }
    out << "}\n";
}

// ---- task 1.2.1/1.2.2: --emit-json (per-component JSON serializer PAIR, D2/D4/D9) ---------------------
// Emits, per detected component T, BOTH a writer `void aeroWriteJson(engine::JsonWriter&, const T&)`
// (task 1.2.1, unchanged) and a reader `bool aeroReadJson(const engine::JsonValue&, T&)` (task 1.2.2,
// new) into the SAME generated TU, wrapped in the component's namespace so ADL resolves for namespaced
// components (D7). Serialization is one consumer (ADR-004) whose halves must never version-skew —
// putting both in one artifact makes a write-only or stale-read build unrepresentable (D2). Unsupported
// fields emit a `// skipped:` comment in BOTH functions; the writer pass still owns the one stderr
// warning per field (D9 -- the reader pass emits no second warning, AC-2). A component that is not at
// namespace scope is skipped whole, with a `// skipped component:` comment + one stderr warning (1.2
// audit, finding 1): its namespace wrapper would name a record and the TU would not compile.
void emitJson(const std::vector<Component>& components, const std::string& inputPath, std::ostream& out) {
    const std::string basename = fs::path(inputPath).filename().string();

    out << "// GENERATED by aero_reflect_gen --emit-json — DO NOT EDIT.\n";
    out << "// source: " << basename << "\n";
    out << "#include <aero/reflect/serialize.hpp>\n";
    out << "\n";
    out << "#include \"" << basename << "\"\n";

    if (components.empty()) {
        out << "\n// no engine::component annotations detected\n";
        return;
    }

    for (const Component& component : components) {
        if (!component.atNamespaceScope) {  // see isNamespaceScoped (1.2 audit, finding 1)
            out << "\n// skipped component: " << component.qualifiedName
                << " (not at namespace scope — unsupported by --emit-json)\n";
            std::cerr << "aero_reflect_gen: warning: " << component.qualifiedName
                      << " is not at namespace scope; --emit-json emits only namespace-scoped components\n";
            continue;
        }
        const auto [ns, typeName] = splitQualifiedName(component.qualifiedName);
        const std::string& qualifiedName = component.qualifiedName;
        out << "\n";
        if (!ns.empty()) {
            out << "namespace " << ns << " {\n";
        }

        // --- the writer (task 1.2.1, byte-identical) -------------------------------------------------
        out << "void aeroWriteJson(engine::JsonWriter& writer, const " << typeName << "& value) {\n";
        out << "    writer.beginObject();\n";
        // Pass 1: supported fields (uniform line; overload resolution routes primitive/Vec3/Quat).
        for (const Field& field : component.fields) {
            if (field.category != FieldCategory::Unsupported) {
                out << "    writer.key(\"" << field.name << "\");  ";
                out << "engine::reflect::writeJson(writer, value." << field.name << ");\n";
            }
        }
        // Pass 2: unsupported fields — skip comment + the SAME stderr warning emitMeta/emitComponents emit.
        for (const Field& field : component.fields) {
            if (field.category == FieldCategory::Unsupported) {
                out << "    // skipped: " << field.name << " (" << field.typeName << " — unsupported)\n";
                std::cerr << "aero_reflect_gen: warning: " << qualifiedName << '.' << field.name << " : "
                          << field.typeName
                          << " is not in the reflectable subset (primitives + Vec3/Quat/std::string)\n";
            }
        }
        out << "    writer.endObject();\n";
        out << "}\n";

        // --- the reader (task 1.2.2, new; D9) --------------------------------------------------------
        out << "\n";
        out << "bool aeroReadJson(const engine::JsonValue& json, " << typeName << "& value) {\n";
        out << "    if (!engine::reflect::expectObject(json, \"" << qualifiedName << "\")) {\n";
        out << "        return false;\n";
        out << "    }\n";
        out << "    bool ok = true;\n";
        // Pass 1: supported fields, source order. `&& ok` on the RIGHT so every field is attempted even
        // after an earlier one fails (best-effort, AC-11) -- `ok && readField(...)` would short-circuit.
        for (const Field& field : component.fields) {
            if (field.category != FieldCategory::Unsupported) {
                out << "    ok = engine::reflect::readField(json, \"" << qualifiedName << "\", \"" << field.name
                    << "\", value." << field.name << ") && ok;\n";
            }
        }
        // Pass 2: the SAME `// skipped:` comment as the writer -- no second stderr warning (AC-2, the
        // writer pass above already warned once per field).
        for (const Field& field : component.fields) {
            if (field.category == FieldCategory::Unsupported) {
                out << "    // skipped: " << field.name << " (" << field.typeName << " — unsupported)\n";
            }
        }
        // warnUnknownKeys: supported field names only, source order; {} for a tag (AC-4).
        out << "    engine::reflect::warnUnknownKeys(json, \"" << qualifiedName << "\", {";
        bool firstKnownKey = true;
        for (const Field& field : component.fields) {
            if (field.category != FieldCategory::Unsupported) {
                if (!firstKnownKey) {
                    out << ", ";
                }
                out << "\"" << field.name << "\"";
                firstKnownKey = false;
            }
        }
        out << "});\n";
        out << "    return ok;\n";
        out << "}\n";

        if (!ns.empty()) {
            out << "}  // namespace " << ns << "\n";
        }
    }
}

// ---- task 1.1.4: --depfile (Makefile-format include-closure depfile, D7) -------------------------

// CXInclusionVisitor (clang-c/Index.h): invoked once per file in the TU's #include closure. Appends
// each included file's name to the caller's set, absolute + lexically-normal + forward-slash so the
// paths ninja will stat are the paths the parse saw (D7/E12). Empty names (rare) are skipped.
void inclusionVisitor(CXFile includedFile, CXSourceLocation* /*stack*/, unsigned /*len*/, CXClientData clientData) {
    auto* deps = static_cast<std::set<std::string>*>(clientData);
    const std::string name = toStdString(clang_getFileName(includedFile));
    if (!name.empty()) {
        deps->insert(fs::absolute(name).lexically_normal().generic_string());
    }
}

// Escape a path for a Makefile dep list: space -> "\ ", '#' -> "\#", '$' -> "$$" (D7). Backslashes
// cannot appear post-generic_string(); UNC/drive-colon paths pass through verbatim (E12).
std::string escapeDepfilePath(const std::string& path) {
    std::string result;
    result.reserve(path.size());
    for (const char c : path) {
        if (c == ' ') {
            result += "\\ ";
        } else if (c == '#') {
            result += "\\#";
        } else if (c == '$') {
            result += "$$";
        } else {
            result.push_back(c);
        }
    }
    return result;
}

// Emit one Makefile rule "<abs -o path>: <sorted deduped escaped abs deps>". Deps = the input file
// plus every clang_getInclusions() file (system/SDK/resource headers INCLUDED — the -MD convention,
// D7). std::set gives dedupe+sort in one step => deterministic per machine (AC-3). Text mode, trunc.
// Returns false on any open/write failure (caller emits the diagnostic + exit 3, V5). Called ONLY
// after the -o write fully succeeded and ONLY on a clean parse (D8) -- both guaranteed upstream.
bool writeDepfile(CXTranslationUnit tu, const std::string& inputPath, const std::string& outputPath,
                  const std::string& depfilePath) {
    std::set<std::string> deps;
    clang_getInclusions(tu, inclusionVisitor, &deps);
    deps.insert(fs::absolute(inputPath).lexically_normal().generic_string());  // belt-and-suspenders

    const std::string target = escapeDepfilePath(fs::absolute(outputPath).lexically_normal().generic_string());

    std::ofstream out(depfilePath, std::ios::trunc);  // TEXT mode, like -o (byte-consistent)
    if (!out) {
        return false;
    }
    out << target << ':';
    for (const std::string& dep : deps) {
        out << ' ' << escapeDepfilePath(dep);
    }
    out << '\n';
    out.flush();
    return static_cast<bool>(out);
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

    // --- step 6: emit (--emit-json/--emit-meta) / detect (--components) / walk -- stdout|-o for emit, stdout else --
    if (args.wantEmitJson) {                                       // NEW leading branch (task 1.2.1)
        if (maxSeverity < static_cast<int>(CXDiagnostic_Error)) {  // same clean-parse gate
            std::vector<Component> components;
            DetectState detect{.allFiles = args.wantAll, .components = &components};
            clang_visitChildren(clang_getTranslationUnitCursor(tu), detectVisitor, &detect);
            if (args.outputPath.has_value()) {
                std::ofstream out(*args.outputPath, std::ios::trunc);
                if (!out) {
                    std::cerr << "aero_reflect_gen: error: cannot open output file '" << *args.outputPath << "'\n";
                    return ExitCode::IoError;
                }
                emitJson(components, args.input, out);
                out.flush();
                if (!out) {
                    std::cerr << "aero_reflect_gen: error: failed writing output file '" << *args.outputPath << "'\n";
                    return ExitCode::IoError;
                }
                if (args.depfilePath.has_value()) {  // reuses writeDepfile verbatim (F1)
                    if (!writeDepfile(tu, args.input, *args.outputPath, *args.depfilePath)) {
                        std::cerr << "aero_reflect_gen: error: failed writing depfile '" << *args.depfilePath << "'\n";
                        return ExitCode::IoError;
                    }
                }
            } else {
                emitJson(components, args.input, std::cout);
            }
        }
    } else if (args.wantEmitMeta) {  // was `if` — body UNCHANGED
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
                // NEW (D6/D7/D8): the depfile's target IS the -o path, so it is written here, only
                // after -o fully succeeded. parseArgs rejected --depfile without --emit-meta+-o (D6);
                // the enclosing `maxSeverity < Error` gate ensured a clean parse (D8) => no stale
                // depfile can appear on a parse failure. `tu` (line 528) is still in scope.
                if (args.depfilePath.has_value()) {
                    if (!writeDepfile(tu, args.input, *args.outputPath, *args.depfilePath)) {
                        std::cerr << "aero_reflect_gen: error: failed writing depfile '" << *args.depfilePath << "'\n";
                        return ExitCode::IoError;
                    }
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

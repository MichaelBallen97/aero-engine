// tools/cooker/src/main.cpp — task 3.3.1: aero_cooker, the first-party asset cooker CLI. The frozen
// contract lives in README.md; this file's argv grammar and exit codes are that contract's only
// implementation. Diagnostics go to stderr only; stdout is reserved for --help/--version.
//
// UNLIKE aero_shaderc AND aero_reflect_gen, this tool links engine and editor targets -- aero::assets
// for the container and aero::editor_core for importModel(), because re-implementing a glTF parser
// here would be a second parser for the format ADR-003 designates canonical. tools/ is outside the
// golden rule on both halves; see tools/cooker/CMakeLists.txt for the citation.
//
// IT SPAWNS NO PROCESS, EVER. A .blend is refused with a message naming the editor's conversion path
// (task 3.2.4's D15); SDL_Process appears nowhere under tools/.
#include <aero/core/guid.hpp>
#include <aero/editor/import_settings.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using engine::editor::ImportSettings;

constexpr std::string_view TOOL_VERSION = "0.1.0";

enum class ExitCode : std::uint8_t { Success = 0, UsageError = 1, CookError = 2, IoError = 3 };

// --- argv grammar (frozen; README.md is the contract) --------------------------------------------
//
//   aero_cooker mesh --input <file> --output <file.aeromesh>
//                    [--guid <32 hex>] [--scale <float>]
//                    [--no-materials] [--no-animations] [--no-skins]
//   aero_cooker --version
//   aero_cooker --help
//
// SUBCOMMAND-SHAPED FROM DAY ONE so 3.3.2 adds `aero_cooker texture ...` with no reshuffle. `mesh` is
// the only subcommand in v1; any other token is a usage error naming it.
//
// EVERY FLAG IS AT-MOST-ONCE. Unlike aero_shaderc --define, this grammar has no repeatable flag at
// all, so the check is uniform: one `have<Flag>` bool each.
void printUsage(std::ostream& out) {
    out << "aero_cooker " << TOOL_VERSION << " -- source model -> cooked .aeromesh (task 3.3.1)\n"
        << "\n"
        << "Usage:\n"
        << "  aero_cooker mesh --input <file> --output <file.aeromesh>\n"
        << "                   [--guid <32 hex>] [--scale <float>]\n"
        << "                   [--no-materials] [--no-animations] [--no-skins]\n"
        << "  aero_cooker --version\n"
        << "  aero_cooker --help\n"
        << "\n"
        << "Subcommands:\n"
        << "  mesh                   Cook one source model into one .aeromesh container.\n"
        << "\n"
        << "Required:\n"
        << "  --input <file>         The source model. .gltf .glb .fbx .obj .mtl .dae .ply .stl.\n"
        << "  --output <file>        The artifact path. The directory must already exist.\n"
        << "\n"
        << "Optional:\n"
        << "  --guid <32 hex>        The source asset's GUID, exactly 32 hex digits, any case. It is\n"
        << "                         written into the container's header. Default: the nil GUID.\n"
        << "  --scale <float>        The importer's uniform scale. Zero and negative are accepted;\n"
        << "                         only a non-finite value is refused. Default: 1.\n"
        << "  --no-materials         Import no materials; every submesh records no material.\n"
        << "  --no-animations        Import no animations (v1 cooks geometry only).\n"
        << "  --no-skins             Import no skin tables (v1 cooks geometry only).\n"
        << "\n"
        << "Every flag may be given at most once. Nothing is written unless the whole cook succeeded,\n"
        << "so a failing input leaves zero artifacts.\n"
        << "\n"
        << "A .blend is refused: convert it in the editor first (Import Details, task 3.2.4). This\n"
        << "tool never spawns a process.\n"
        << "\n"
        << "Exit codes: 0 success, 1 usage error, 2 import or cook error, 3 I/O error.\n";
}

void printVersion(std::ostream& out) { out << "aero_cooker " << TOOL_VERSION << '\n'; }

// PURE, LOCALE-FREE decimal -> float, fully consumed or refused.
//
// MEASURED DEVIATION FROM THE PLAN's own §D-8, and the reason is a toolchain fact rather than a
// preference: std::from_chars's FLOATING-POINT overload does not exist on every toolchain this
// project builds with. Apple's libc++ in MacOSX15.4.sdk -- the SDK this repo's own clang-tidy
// invocation pins -- and Homebrew LLVM 18's libc++ both ship __charconv/from_chars_integral.h ONLY,
// so `std::from_chars(first, last, float&)` is a hard "call to deleted function" there.
// editor/src/model_import.cpp:145-160 already measured exactly this and hand-rolled a parser rather
// than take engine/reflect/src/json_value.cpp's __cpp_lib_to_chars + strtof_l route, whose fallback
// needs per-OS includes.
//
// A CLI can afford the third answer neither of those could: one istringstream imbued with
// std::locale::classic(), which is decimal-point-stable on every lane with no #if and no per-OS
// include. std::stof/std::atof stay refused for the reason they always were -- stof reads the C
// locale's decimal point, so a German-locale shell would read "0.01" as 0, and it throws.
//
// FULL CONSUMPTION is required ("1.0x" is refused whole), and a non-finite value is refused: num_get
// accepts "nan" and "inf", and a NaN scale would make every cooked position and every cooked bound
// NaN with no diagnostic anywhere. ZERO AND NEGATIVE ARE ACCEPTED -- the editor's own widget honours
// a hand-edited zero or negative scale and never clamps, and the CLI must not be stricter than the
// editor it mirrors.
[[nodiscard]] std::optional<float> parseScale(std::string_view text) {
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    float value = 0.0F;
    in >> value;
    if (in.fail() || !in.eof()) {
        return std::nullopt;
    }
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

struct Args {
    std::string inputPath;
    std::string outputPath;
    engine::Guid guid;  // nil unless --guid was given; nil is legal and deterministic
    ImportSettings settings;
    bool wantHelp = false;
    bool wantVersion = false;
};

// Hand-rolled argv parse (no getopt -- Windows has none), the aero_shaderc shape. Any violation
// prints its own reason to stderr and the caller exits 1. Returns nullopt on any usage violation.
std::optional<Args> parseArgs(int argc, char** argv) {
    Args args;

    // --help/--version short-circuit: found anywhere, they win immediately (common CLI ergonomics,
    // and aero_shaderc's own behaviour).
    for (int i = 1; i < argc; ++i) {
        const std::string_view token = argv[i];
        if (token == "--help") {
            args.wantHelp = true;
            return args;
        }
        if (token == "--version") {
            args.wantVersion = true;
            return args;
        }
    }

    if (argc < 2) {
        std::cerr << "aero_cooker: error: no subcommand given (expected: mesh)\n";
        return std::nullopt;
    }
    const std::string_view subcommand = argv[1];
    if (subcommand != "mesh") {
        std::cerr << "aero_cooker: error: unknown subcommand '" << subcommand << "' (expected: mesh)\n";
        return std::nullopt;
    }

    bool haveInput = false;
    bool haveOutput = false;
    bool haveGuid = false;
    bool haveScale = false;
    bool haveNoMaterials = false;
    bool haveNoAnimations = false;
    bool haveNoSkins = false;

    for (int i = 2; i < argc; ++i) {
        const std::string_view flag = argv[i];
        // A value-taking flag at the end of argv is a usage error naming the flag.
        const auto needValue = [&](std::string_view name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "aero_cooker: error: " << name << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };
        const auto refuseRepeat = [&](bool& have, std::string_view name) {
            if (have) {
                std::cerr << "aero_cooker: error: " << name << " may be given at most once\n";
                return false;
            }
            have = true;
            return true;
        };

        if (flag == "--input") {
            if (!refuseRepeat(haveInput, flag)) {
                return std::nullopt;
            }
            const char* value = needValue(flag);
            if (value == nullptr) {
                return std::nullopt;
            }
            args.inputPath = value;
        } else if (flag == "--output") {
            if (!refuseRepeat(haveOutput, flag)) {
                return std::nullopt;
            }
            const char* value = needValue(flag);
            if (value == nullptr) {
                return std::nullopt;
            }
            args.outputPath = value;
        } else if (flag == "--guid") {
            if (!refuseRepeat(haveGuid, flag)) {
                return std::nullopt;
            }
            const char* value = needValue(flag);
            if (value == nullptr) {
                return std::nullopt;
            }
            // parseGuid accepts EXACTLY 32 hex digits, any case, and nothing else. A dashed or braced
            // value is a usage error, never a silently normalized success. Absent -> nil, which is
            // legal and deterministic; the tool never probes for a sibling .meta, because that would
            // make its output depend on a file it was not given.
            const std::optional<engine::Guid> parsed = engine::parseGuid(value);
            if (!parsed.has_value()) {
                std::cerr << "aero_cooker: error: invalid --guid value '" << value
                          << "' (expected exactly 32 hex digits, no dashes and no braces)\n";
                return std::nullopt;
            }
            args.guid = *parsed;
        } else if (flag == "--scale") {
            if (!refuseRepeat(haveScale, flag)) {
                return std::nullopt;
            }
            const char* value = needValue(flag);
            if (value == nullptr) {
                return std::nullopt;
            }
            const std::optional<float> parsed = parseScale(value);
            if (!parsed.has_value()) {
                std::cerr << "aero_cooker: error: invalid --scale value '" << value
                          << "' (expected one finite decimal number)\n";
                return std::nullopt;
            }
            args.settings.scale = *parsed;
        } else if (flag == "--no-materials") {
            if (!refuseRepeat(haveNoMaterials, flag)) {
                return std::nullopt;
            }
            args.settings.importMaterials = false;
        } else if (flag == "--no-animations") {
            if (!refuseRepeat(haveNoAnimations, flag)) {
                return std::nullopt;
            }
            args.settings.importAnimations = false;
        } else if (flag == "--no-skins") {
            if (!refuseRepeat(haveNoSkins, flag)) {
                return std::nullopt;
            }
            args.settings.importSkins = false;
        } else {
            std::cerr << "aero_cooker: error: unknown flag '" << flag << "'\n";
            return std::nullopt;
        }
    }

    if (!haveInput) {
        std::cerr << "aero_cooker: error: --input is required\n";
        return std::nullopt;
    }
    if (!haveOutput) {
        std::cerr << "aero_cooker: error: --output is required\n";
        return std::nullopt;
    }
    return args;
}

ExitCode runMain(int argc, char** argv) {
    const std::optional<Args> parsed = parseArgs(argc, argv);
    if (!parsed.has_value()) {
        printUsage(std::cerr);
        return ExitCode::UsageError;
    }
    if (parsed->wantHelp) {
        printUsage(std::cout);
        return ExitCode::Success;
    }
    if (parsed->wantVersion) {
        printVersion(std::cout);
        return ExitCode::Success;
    }

    // Task 3.3.1 step 8 ships the grammar and the exit codes only; step 9 replaces this block with
    // the two-pass import driver, the cook and the artifact write. No registered case reaches it.
    std::cerr << "aero_cooker: error: the mesh cook is not wired up yet\n";
    return ExitCode::CookError;
}

}  // namespace

// The real entry point is a thin, non-throwing wrapper (docs/04: no exceptions across a public API
// boundary -- main() is this tool's outermost one). Anything that escapes runMain becomes a clean
// diagnostic plus an exit code instead of an uncaught-exception abort. aero_shaderc's own main() is
// this same three-line shape.
int main(int argc, char** argv) {
    try {
        return static_cast<int>(runMain(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "aero_cooker: error: unexpected exception: " << e.what() << '\n';
        return static_cast<int>(ExitCode::IoError);
    } catch (...) {
        std::cerr << "aero_cooker: error: unexpected exception\n";
        return static_cast<int>(ExitCode::IoError);
    }
}

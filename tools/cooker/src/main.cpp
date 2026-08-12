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
// (task 3.2.4's D15). The gate grep for that invariant scans tools/ for SDL's process API and the
// three C spawn primitives by name and does NOT strip comments, so none of those tokens may be
// written in prose anywhere under this directory -- the 3.2.4 and 3.2.5 rule, a third application.
#include <aero/assets/mesh_cook.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/import_settings.hpp>
#include <aero/editor/mesh_cook_source.hpp>
#include <aero/editor/model_import.hpp>
#include <aero/editor/text_file.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <locale>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using engine::editor::ExternalBuffer;
using engine::editor::ImportDepth;
using engine::editor::ImportResult;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;

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

// stderr only, one line each, so a build log can be grepped for them.
void reportWarnings(std::string_view origin, const std::vector<std::string>& warnings, std::size_t total) {
    for (const std::string& warning : warnings) {
        std::cerr << "aero_cooker: warning: " << origin << ": " << warning << '\n';
    }
    if (total > warnings.size()) {
        std::cerr << "aero_cooker: warning: " << origin << ": " << (total - warnings.size())
                  << " further warnings were not listed\n";
    }
}

[[nodiscard]] bool endsWithFolded(std::string_view text, std::string_view suffix) noexcept {
    if (text.size() < suffix.size()) {
        return false;
    }
    const std::size_t offset = text.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        char a = text[offset + i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (a != suffix[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::span<const std::byte> asBytes(const std::string& text) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

ExitCode runMain(int argc, char** argv) {
    // ---- 1. argv ---------------------------------------------------------------------------
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
    const Args& args = *parsed;

    // ---- 2. the file NAME decides what happens, before a byte is read -----------------------
    // DEVIATION from the plan's own §D-8, which numbered the read first and this test second. Its
    // §A-12 says the .blend arm sits "before anything is read", and that is the reading that
    // survives contact: readFileBytes refuses an over-cap file WITHOUT OPENING IT, so reading first
    // would answer a 300 MB .blend with "the file is too large" instead of "convert it in the
    // editor". Nothing else moves -- a missing .gltf still reaches the read below and still exits 3,
    // because its NAME is importable.
    const std::string leaf = fs::path(args.inputPath).filename().string();
    if (!engine::editor::isImportableModelName(leaf)) {
        if (endsWithFolded(leaf, ".blend")) {
            std::cerr << "aero_cooker: error: '" << leaf
                      << "' must be converted before it can be cooked -- open it in the editor and use the "
                         "Import Details panel's Convert with Blender action (task 3.2.4). This tool never "
                         "spawns a process.\n";
        } else {
            std::cerr << "aero_cooker: error: no importer claims '" << leaf
                      << "' (expected .gltf .glb .fbx .obj .mtl .dae .ply .stl)\n";
        }
        return ExitCode::CookError;
    }

    // ---- 3. the source bytes ----------------------------------------------------------------
    engine::editor::FileBytesResult source =
        engine::editor::readFileBytes(args.inputPath, engine::editor::MAX_MODEL_FILE_BYTES);
    if (!source.bytes.has_value()) {
        if (source.refusedByCap) {
            std::cerr << "aero_cooker: error: '" << args.inputPath << "' is " << source.size << " bytes, above the "
                      << engine::editor::MAX_MODEL_FILE_BYTES << "-byte import limit\n";
        } else {
            std::cerr << "aero_cooker: error: cannot read '" << args.inputPath << "': " << source.error << '\n';
        }
        return ExitCode::IoError;
    }
    const std::span<const std::byte> sourceBytes = asBytes(*source.bytes);

    // ---- 4. the Structure pass, ONLY for a file kind whose Full pass needs external bytes ----
    // ModelImportSession::service() gates its own first pass on exactly this predicate, and for the
    // same reason: for .fbx, .dae, .ply, .stl and .mtl the Structure pass has nothing to tell us and
    // the reads it would drive are pure waste.
    //
    // assetRelativeDir is "" everywhere (AC-38): the input file's OWN directory is the resolution
    // root, so relative URIs resolve inside it and every escape above it is refused by the existing
    // classifyUri with no new code here.
    std::vector<ExternalBuffer> externals;
    if (engine::editor::modelImporterNeedsExternalBuffers(leaf)) {
        const ImportResult structure =
            engine::editor::importModel(leaf, "", sourceBytes, args.settings, ImportDepth::Structure, {});
        if (structure.status != ImportStatus::Ok && structure.status != ImportStatus::Truncated) {
            std::cerr << "aero_cooker: error: " << engine::editor::importStatusLabel(structure.status) << ": "
                      << structure.message << '\n';
            return ExitCode::CookError;
        }
        const fs::path inputDir = fs::path(args.inputPath).parent_path();
        externals.reserve(structure.externalUris.size());
        std::uint64_t total = 0;
        std::size_t taken = 0;
        for (const std::string& uri : structure.externalUris) {
            if (taken >= engine::editor::MAX_EXTERNAL_URIS) {
                std::cerr << "aero_cooker: warning: more than " << engine::editor::MAX_EXTERNAL_URIS
                          << " external buffers were named; the rest were skipped\n";
                break;
            }
            const std::string path = (inputDir / fs::path(uri)).string();
            engine::editor::FileBytesResult buffer =
                engine::editor::readFileBytes(path, engine::editor::MAX_EXTERNAL_BYTES_PER_MODEL);
            if (!buffer.bytes.has_value()) {
                // A missing or unreadable buffer is a WARNING and is skipped -- the .mtl precedent.
                // Whether it is fatal is the IMPORTER's call: a glTF whose geometry lived in that
                // buffer reports MissingBuffer below, a .obj whose .mtl is absent does not.
                std::cerr << "aero_cooker: warning: cannot read external buffer '" << uri << "': " << buffer.error
                          << '\n';
                continue;
            }
            // Charged with the OBSERVED size and broken on overflow, mirroring the session's own
            // loop. Unlike the session there is no Truncated-instead-of-partial fallback: a
            // build-time tool that silently emitted structure-only geometry would be worse than one
            // that says a buffer was skipped.
            if (buffer.bytes->size() > engine::editor::MAX_EXTERNAL_BYTES_PER_MODEL - total) {
                std::cerr << "aero_cooker: warning: the external buffers exceed this importer's per-model limit of "
                          << engine::editor::MAX_EXTERNAL_BYTES_PER_MODEL << " bytes; '" << uri
                          << "' and everything after it were skipped\n";
                break;
            }
            total += buffer.bytes->size();
            ++taken;
            externals.push_back(ExternalBuffer{uri, std::move(*buffer.bytes)});
        }
    }

    // ---- 5. the Full pass --------------------------------------------------------------------
    const ImportResult imported =
        engine::editor::importModel(leaf, "", sourceBytes, args.settings, ImportDepth::Full, externals);
    if (imported.status != ImportStatus::Ok && imported.status != ImportStatus::Truncated) {
        reportWarnings("import", imported.warnings, imported.warningTotal);
        std::cerr << "aero_cooker: error: " << engine::editor::importStatusLabel(imported.status) << ": "
                  << imported.message << '\n';
        return ExitCode::CookError;
    }
    reportWarnings("import", imported.warnings, imported.warningTotal);
    if (imported.status == ImportStatus::Truncated) {
        std::cerr << "aero_cooker: warning: import truncated: " << imported.message << '\n';
    }

    // ---- 6. the cook -------------------------------------------------------------------------
    const engine::assets::MeshCookResult cooked = engine::editor::cookImportedModel(imported.model, args.guid);
    reportWarnings("cook", cooked.warnings, cooked.warningTotal);
    if (cooked.bytes.empty()) {
        // The ONLY way this happens is the allocation-failure arm, which reports Truncated with the
        // buffer cleared. There is nothing coherent to write, so it is an error rather than an
        // artifact -- INV-C3's other half.
        std::cerr << "aero_cooker: error: the cook produced no container: " << cooked.message << '\n';
        return ExitCode::CookError;
    }
    if (cooked.status == engine::assets::MeshCookStatus::Truncated) {
        // Truncated still exits 0: the artifact is COHERENT, merely smaller, which is the same
        // reading the importer gives its own Truncated.
        std::cerr << "aero_cooker: warning: cook truncated: " << cooked.message << '\n';
    }

    // ---- 7. the write. ONLY NOW is the output path touched (AC-36) ---------------------------
    // writeTextFileAtomic is binary on BOTH sides (std::ios::binary | std::ios::trunc), which is
    // what stops a text-mode write turning every 0x0A in the container into 0x0D 0x0A on exactly one
    // lane. THE TOOL CREATES NO DIRECTORY: --output inside a directory that does not exist is an I/O
    // error, because a build-time tool that invents directories is how a typo becomes a mystery
    // tree. aero_shaderc does create its --output-dir, and the difference is deliberate: that flag
    // names a DIRECTORY, this one names a FILE.
    const std::string_view artifact(reinterpret_cast<const char*>(cooked.bytes.data()), cooked.bytes.size());
    const std::string writeError = engine::editor::writeTextFileAtomic(args.outputPath, artifact);
    if (!writeError.empty()) {
        std::cerr << "aero_cooker: error: cannot write '" << args.outputPath << "': " << writeError << '\n';
        return ExitCode::IoError;
    }

    // ---- 8. done -----------------------------------------------------------------------------
    return ExitCode::Success;
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

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
#include <aero/assets/cooked_texture.hpp>
#include <aero/assets/mesh_cook.hpp>
#include <aero/assets/texture_cook.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/animation_cook_source.hpp>
#include <aero/editor/import_settings.hpp>
#include <aero/editor/mesh_cook_source.hpp>
#include <aero/editor/model_import.hpp>
#include <aero/editor/skeleton_cook_source.hpp>
#include <aero/editor/text_file.hpp>
#include <aero/editor/texture_cook_source.hpp>

#include <array>
#include <charconv>
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
#include <system_error>
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
//   aero_cooker texture --input <file> --output <file.ktx2>
//                       (--srgb | --linear)
//                       [--guid <32 hex>] [--format bc1|bc3|bc4|bc5|rgba8|auto] [--no-mips]
//   aero_cooker skeleton --input <file> --output <file.aeroskel>
//                        [--guid <32 hex>] [--skin <index>] [--scale <float>]
//   aero_cooker animation --input <file> --output <file.aeroanim>
//                         [--guid <32 hex>] [--clip <index>]
//   aero_cooker --version
//   aero_cooker --help
//
// SUBCOMMAND-SHAPED FROM DAY ONE, and tasks 3.3.2, 3.5.1 and 3.5.2 each filled one in with no
// reshuffle of the mesh path: --input, --output and --guid stay in the shared prefix and the flag loop
// splits only on the subcommand-specific arms. Any token that is none of `mesh`, `texture`,
// `skeleton` and `animation` is a usage error naming it.
//
// EVERY FLAG IS AT-MOST-ONCE. Unlike aero_shaderc --define, this grammar has no repeatable flag at
// all, so the check is uniform: one `have<Flag>` bool each.
void printUsage(std::ostream& out) {
    out << "aero_cooker " << TOOL_VERSION << " -- source assets -> cooked engine artifacts\n"
        << "\n"
        << "Usage:\n"
        << "  aero_cooker mesh --input <file> --output <file.aeromesh>\n"
        << "                   [--guid <32 hex>] [--scale <float>]\n"
        << "                   [--no-materials] [--no-animations] [--no-skins]\n"
        << "  aero_cooker texture --input <file> --output <file.ktx2>\n"
        << "                      (--srgb | --linear)\n"
        << "                      [--guid <32 hex>] [--format bc1|bc3|bc4|bc5|rgba8|auto] [--no-mips]\n"
        << "  aero_cooker skeleton --input <file> --output <file.aeroskel>\n"
        << "                       [--guid <32 hex>] [--skin <index>] [--scale <float>]\n"
        << "  aero_cooker animation --input <file> --output <file.aeroanim>\n"
        << "                        [--guid <32 hex>] [--clip <index>]\n"
        << "  aero_cooker --version\n"
        << "  aero_cooker --help\n"
        << "\n"
        << "Subcommands:\n"
        << "  mesh                   Cook one source model into one .aeromesh container.\n"
        << "  texture                Cook one source image into one KTX2 container.\n"
        << "  skeleton               Cook one skin of one source model into one .aeroskel container.\n"
        << "  animation              Cook one clip of one source model into one .aeroanim container.\n"
        << "\n"
        << "Required (all subcommands):\n"
        << "  --input <file>         The source asset.\n"
        << "                         mesh, skeleton, animation:\n"
        << "                                         .gltf .glb .fbx .obj .mtl .dae .ply .stl\n"
        << "                         texture:        .png .jpg .jpeg .tga .bmp .gif .psd\n"
        << "  --output <file>        The artifact path. The directory must already exist.\n"
        << "\n"
        << "Required (texture only):\n"
        << "  --srgb | --linear      The source's colour space. EXACTLY ONE, and there is no default:\n"
        << "                         sRGB is wrong for every normal, roughness, metallic and mask map,\n"
        << "                         and linear is wrong for every base-colour and emissive map. Unlike\n"
        << "                         most wrong defaults this one produces an image that still looks\n"
        << "                         like a texture, just too dark or too washed out, so it survives\n"
        << "                         review and ships.\n"
        << "\n"
        << "Optional (all subcommands):\n"
        << "  --guid <32 hex>        The source asset's GUID, exactly 32 hex digits, any case. It is\n"
        << "                         written into the artifact. Default: the nil GUID.\n"
        << "\n"
        << "Optional (mesh and skeleton):\n"
        << "  --scale <float>        The importer's uniform scale. Zero and negative are accepted;\n"
        << "                         only a non-finite value is refused. Default: 1.\n"
        << "\n"
        << "Optional (mesh only):\n"
        << "  --no-materials         Import no materials; every submesh records no material.\n"
        << "  --no-animations        Import no animations (v1 cooks geometry only).\n"
        << "  --no-skins             Import no vertex joint/weight streams and no skin tables.\n"
        << "\n"
        << "Optional (skeleton only):\n"
        << "  --skin <index>         Which skin to cook, as a position in the model's skin list.\n"
        << "                         Default: 0. A model with more skins than the one cooked warns\n"
        << "                         naming the total.\n"
        << "\n"
        << "Optional (animation only):\n"
        << "  --clip <index>         Which clip to cook, as a position in the model's animation list.\n"
        << "                         Default: 0. A model with more clips than the one cooked warns\n"
        << "                         naming the total. There is no --scale here: no importer applies\n"
        << "                         the uniform scale to an animation channel, so the flag would\n"
        << "                         change no byte of the output.\n"
        << "\n"
        << "Optional (texture only):\n"
        << "  --format <token>       bc1, bc3, bc4, bc5, rgba8 or auto. Default: auto, which answers\n"
        << "                         bc3 when any texel's alpha is below 255 and bc1 otherwise, in the\n"
        << "                         requested colour space. It never answers bc4, bc5 or rgba8: those\n"
        << "                         encode intent, which pixels cannot reveal. Vulkan defines no sRGB\n"
        << "                         variant of bc4 or bc5, so --srgb with either is a usage error.\n"
        << "  --no-mips              Emit level 0 only, instead of the full mip chain.\n"
        << "\n"
        << "Every flag may be given at most once. Nothing is written unless the whole cook succeeded,\n"
        << "so a failing input leaves zero artifacts.\n"
        << "\n"
        << "A .blend is refused: convert it in the editor first (Import Details, task 3.2.4). This\n"
        << "tool never spawns a process. A .hdr is refused too: stb_image does not fail on a Radiance\n"
        << "file, it silently tone-maps it to 8-bit, and HDR belongs to BC6H, which v1 does not cook.\n"
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

// PURE, LOCALE-FREE decimal -> uint32, fully consumed or refused.
//
// std::from_chars, unlike parseScale's istringstream neighbour above, because the INTEGER overload is
// the one every toolchain this project builds with does ship (the float overload is the missing one --
// see parseScale's own note). It is locale-independent BY DEFINITION rather than by imbuing, it never
// throws, and it reports overflow as a distinct error rather than saturating the way strtoul does.
//
// FULL CONSUMPTION is required, so "1x" is refused whole rather than read as 1; and a LEADING SIGN is
// refused because from_chars's unsigned overload does not accept one at all -- "-1" fails at the first
// character, which is the answer we want and is stated here so a future reader does not "fix" it into
// a signed parse followed by a range check.
//
// TWO CONSUMERS since task 3.5.2: --skin and --clip. Both are a POSITION in one of the model's own
// lists and both want exactly these three refusals, so the second one reuses this parser rather than
// growing a twin. The NAME is deliberately not generalized: renaming it across a file three tasks
// have hardened would be a diff with no behaviour in it.
[[nodiscard]] std::optional<std::uint32_t> parseSkinIndex(std::string_view text) {
    std::uint32_t value = 0;
    const char* const first = text.data();
    const char* const last = first + text.size();
    const std::from_chars_result parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return std::nullopt;
    }
    return value;
}

enum class Subcommand : std::uint8_t { Mesh, Texture, Skeleton, Animation };

// The six accepted --format tokens, in the order the usage text and every diagnostic list them. `auto`
// is deliberately IN the table rather than a special case at the parse site: it is a legal value of
// the flag, and a user who spells it out must get the same answer as one who omits the flag.
constexpr std::array<std::string_view, 6> TEXTURE_FORMAT_TOKENS{"bc1", "bc3", "bc4", "bc5", "rgba8", "auto"};

struct Args {
    Subcommand subcommand = Subcommand::Mesh;
    std::string inputPath;
    std::string outputPath;
    engine::Guid guid;  // nil unless --guid was given; nil is legal and deterministic
    ImportSettings settings;
    // --- texture only ---
    bool srgb = false;                 // meaningful only after the exactly-one check below passed
    std::string formatToken = "auto";  // one of TEXTURE_FORMAT_TOKENS
    bool generateMips = true;          // --no-mips clears it
    // --- skeleton only ---
    std::uint32_t skinIndex = 0;  // --skin; a POSITION in the model's skin list, never a localId
    // --- animation only ---
    std::uint32_t clipIndex = 0;  // --clip; a POSITION in the model's animation list, never a localId
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
        std::cerr << "aero_cooker: error: no subcommand given (expected: mesh, texture, skeleton or "
                     "animation)\n";
        return std::nullopt;
    }
    const std::string_view subcommand = argv[1];
    if (subcommand == "mesh") {
        args.subcommand = Subcommand::Mesh;
    } else if (subcommand == "texture") {
        args.subcommand = Subcommand::Texture;
    } else if (subcommand == "skeleton") {
        args.subcommand = Subcommand::Skeleton;
    } else if (subcommand == "animation") {
        args.subcommand = Subcommand::Animation;
    } else {
        std::cerr << "aero_cooker: error: unknown subcommand '" << subcommand
                  << "' (expected: mesh, texture, skeleton or animation)\n";
        return std::nullopt;
    }
    // EXPLICIT PER-SUBCOMMAND PREDICATES, replacing task 3.3.2's single `isTexture` discriminator: with
    // three subcommands "not texture" and "mesh" stopped being the same set, and --scale belongs to two
    // of them. Named booleans rather than a comparison at each arm, so the scoping of a flag is
    // readable in one place instead of inferred from a chain of negations.
    const bool isMesh = args.subcommand == Subcommand::Mesh;
    const bool isTexture = args.subcommand == Subcommand::Texture;
    const bool isSkeleton = args.subcommand == Subcommand::Skeleton;
    const bool isAnimation = args.subcommand == Subcommand::Animation;
    // NOT extended to animation at task 3.5.2, and that is a FINDING rather than a preference: all
    // four importers apply ImportSettings::scale to root node translations, to mesh positions and to
    // inverse-bind translation columns, and to no animation channel anywhere -- so --scale here would
    // change no byte of the output. A flag that lies is worse than a flag that is absent.
    const bool takesScale = isMesh || isSkeleton;  // the importer's scale reaches node TRS as well as
                                                   // positions, so a skeleton cook honours it too

    bool haveInput = false;
    bool haveOutput = false;
    bool haveGuid = false;
    bool haveScale = false;
    bool haveNoMaterials = false;
    bool haveNoAnimations = false;
    bool haveNoSkins = false;
    bool haveSrgb = false;
    bool haveLinear = false;
    bool haveFormat = false;
    bool haveNoMips = false;
    bool haveSkin = false;
    bool haveClip = false;

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
            // --- from here the arms are SUBCOMMAND-SPECIFIC. --input, --output and --guid above are
            // the shared prefix. A flag given to a subcommand that does not take it falls through to
            // the unknown-flag arm at the bottom, which names it -- deliberately, rather than accepting
            // it silently for the wrong subcommand.
        } else if (takesScale && flag == "--scale") {
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
        } else if (isMesh && flag == "--no-materials") {
            if (!refuseRepeat(haveNoMaterials, flag)) {
                return std::nullopt;
            }
            args.settings.importMaterials = false;
        } else if (isMesh && flag == "--no-animations") {
            if (!refuseRepeat(haveNoAnimations, flag)) {
                return std::nullopt;
            }
            args.settings.importAnimations = false;
        } else if (isMesh && flag == "--no-skins") {
            if (!refuseRepeat(haveNoSkins, flag)) {
                return std::nullopt;
            }
            args.settings.importSkins = false;
        } else if (isSkeleton && flag == "--skin") {
            if (!refuseRepeat(haveSkin, flag)) {
                return std::nullopt;
            }
            const char* value = needValue(flag);
            if (value == nullptr) {
                return std::nullopt;
            }
            const std::optional<std::uint32_t> parsed = parseSkinIndex(value);
            if (!parsed.has_value()) {
                std::cerr << "aero_cooker: error: invalid --skin value '" << value
                          << "' (expected one non-negative whole number)\n";
                return std::nullopt;
            }
            args.skinIndex = *parsed;
        } else if (isAnimation && flag == "--clip") {
            if (!refuseRepeat(haveClip, flag)) {
                return std::nullopt;
            }
            const char* value = needValue(flag);
            if (value == nullptr) {
                return std::nullopt;
            }
            // --skin's twin, through --skin's own parser: a POSITION in one of the model's lists,
            // locale-independent, fully consumed, and a leading sign refused at the first character.
            const std::optional<std::uint32_t> parsed = parseSkinIndex(value);
            if (!parsed.has_value()) {
                std::cerr << "aero_cooker: error: invalid --clip value '" << value
                          << "' (expected one non-negative whole number)\n";
                return std::nullopt;
            }
            args.clipIndex = *parsed;
        } else if (isTexture && flag == "--srgb") {
            if (!refuseRepeat(haveSrgb, flag)) {
                return std::nullopt;
            }
        } else if (isTexture && flag == "--linear") {
            if (!refuseRepeat(haveLinear, flag)) {
                return std::nullopt;
            }
        } else if (isTexture && flag == "--format") {
            if (!refuseRepeat(haveFormat, flag)) {
                return std::nullopt;
            }
            const char* value = needValue(flag);
            if (value == nullptr) {
                return std::nullopt;
            }
            args.formatToken = value;
        } else if (isTexture && flag == "--no-mips") {
            if (!refuseRepeat(haveNoMips, flag)) {
                return std::nullopt;
            }
            args.generateMips = false;
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
    if (!isTexture) {
        return args;  // mesh, skeleton and animation have no post-parse validation: every flag they
                      // take is already fully decided at its own arm above
    }

    // ---- texture post-parse validation, in a FIXED order ------------------------------------------
    // 1. EXACTLY ONE colour space, and there is no default. Both messages name BOTH flags, because a
    //    user who gave neither and a user who gave both are asking the same question.
    if (!haveSrgb && !haveLinear) {
        std::cerr << "aero_cooker: error: texture requires exactly one of --srgb or --linear (there is no "
                     "default: sRGB is wrong for every normal, roughness, metallic and mask map, and linear is "
                     "wrong for every base-colour and emissive map)\n";
        return std::nullopt;
    }
    if (haveSrgb && haveLinear) {
        std::cerr << "aero_cooker: error: --srgb and --linear are mutually exclusive; give exactly one\n";
        return std::nullopt;
    }
    args.srgb = haveSrgb;

    // 2. The token must be one of the six, checked BEFORE the bc4/bc5 conflict so `--format bc7
    //    --srgb` names the unknown token rather than a conflict it does not have.
    bool known = false;
    for (const std::string_view token : TEXTURE_FORMAT_TOKENS) {
        if (args.formatToken == token) {
            known = true;
        }
    }
    if (!known) {
        std::cerr << "aero_cooker: error: unknown --format value '" << args.formatToken
                  << "' (expected: bc1, bc3, bc4, bc5, rgba8 or auto)\n";
        return std::nullopt;
    }

    // 3. sRGB with a single- or two-channel format. Vulkan enumerates 139 BC4_UNORM, 140 BC4_SNORM,
    //    141 BC5_UNORM, 142 BC5_SNORM with no sRGB value among them, so the combination is not
    //    unsupported by us -- it does not exist.
    if (args.srgb && (args.formatToken == "bc4" || args.formatToken == "bc5")) {
        std::cerr << "aero_cooker: error: --srgb cannot be combined with --format " << args.formatToken
                  << ": Vulkan defines no sRGB variant of either (BC4_UNORM is 139 and BC5_UNORM is 141, with "
                     "no sRGB value between them)\n";
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

// ---- the model-import prelude, shared by `mesh` and `skeleton` (task 3.5.1) ----------------------
//
// Steps 2-5 of the model-cook path -- the file NAME check, the capped read, the Structure pass, the
// external-buffer budget and the Full import -- EXTRACTED rather than duplicated. Those ninety lines
// of budgeted I/O took three tasks to harden (3.3.1's budget, 3.2.3's .mtl warning posture, 3.2.4's
// .blend refusal), and a second copy would only ever diverge on inputs no case cooks. The nineteen
// mesh CLI cases that existed before this function are the regression harness proving the extraction
// moved nothing -- measured, not rounded: eighteen arms that pass `mesh` as argv[1], plus
// golden_manifest, which cooks five tuples through it.
//
// nullopt => the failure was ALREADY PRINTED and `exitCode` holds what runMain must return. On
// success `exitCode` is untouched and the status is Ok or Truncated, never anything else.
//
// The RESULT IS FULLY OWNED: ImportedModel holds vectors and strings only, no span into the source
// bytes, so the file buffer read here may die with this frame. Verified against model_import.hpp
// rather than assumed -- the mesh path used to keep both alive together by accident of scope.
[[nodiscard]] std::optional<ImportResult> importModelForCook(const Args& args, ExitCode& exitCode) {
    // ---- 2. the file NAME decides what happens, before a byte is read -----------------------
    // DEVIATION from task 3.3.1's plan §D-8, which numbered the read first and this test second. Its
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
        exitCode = ExitCode::CookError;
        return std::nullopt;
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
        exitCode = ExitCode::IoError;
        return std::nullopt;
    }
    const std::span<const std::byte> sourceBytes = asBytes(*source.bytes);

    // ---- 4. the Structure pass, ONLY for a file kind whose Full pass needs external bytes ----
    // ModelImportSession::service() gates its own first pass on exactly this predicate, and for the
    // same reason: for .fbx, .dae, .ply, .stl and .mtl the Structure pass has nothing to tell us and
    // the reads it would drive are pure waste.
    //
    // assetRelativeDir is "" everywhere: the input file's OWN directory is the resolution root, so
    // relative URIs resolve inside it and every escape above it is refused by the existing
    // classifyUri with no new code here.
    std::vector<ExternalBuffer> externals;
    if (engine::editor::modelImporterNeedsExternalBuffers(leaf)) {
        const ImportResult structure =
            engine::editor::importModel(leaf, "", sourceBytes, args.settings, ImportDepth::Structure, {});
        if (structure.status != ImportStatus::Ok && structure.status != ImportStatus::Truncated) {
            std::cerr << "aero_cooker: error: " << engine::editor::importStatusLabel(structure.status) << ": "
                      << structure.message << '\n';
            exitCode = ExitCode::CookError;
            return std::nullopt;
        }
        const fs::path inputDir = fs::path(args.inputPath).parent_path();
        externals.reserve(structure.externalUris.size());
        std::uint64_t total = 0;
        std::size_t taken = 0;
        for (const std::string& uri : structure.externalUris) {
            // DEFENCE IN DEPTH, and unreachable today. The real cap is enforced inside every
            // importer before an ImportResult is returned -- gltf_import.cpp, fbx_import.cpp,
            // obj_import.cpp and assimp_import.cpp each bound externalUris at MAX_EXTERNAL_URIS --
            // so structure.externalUris can never exceed it and `taken`, which only ever increments
            // on a successful read, can never reach it. Kept because this loop is the CALLER's own
            // budget and a future importer that forgot the bound would find it here rather than in
            // an unbounded allocation. Deliberately NOT deleted: a guard removed because it cannot
            // fire today is a guard nobody restores when the reason it could not fire changes.
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
    ImportResult imported =
        engine::editor::importModel(leaf, "", sourceBytes, args.settings, ImportDepth::Full, externals);
    if (imported.status != ImportStatus::Ok && imported.status != ImportStatus::Truncated) {
        reportWarnings("import", imported.warnings, imported.warningTotal);
        std::cerr << "aero_cooker: error: " << engine::editor::importStatusLabel(imported.status) << ": "
                  << imported.message << '\n';
        exitCode = ExitCode::CookError;
        return std::nullopt;
    }
    reportWarnings("import", imported.warnings, imported.warningTotal);
    if (imported.status == ImportStatus::Truncated) {
        std::cerr << "aero_cooker: warning: import truncated: " << imported.message << '\n';
    }
    return imported;
}

// ---- the animation subcommand (task 3.5.2) ------------------------------------------------------
//
// PLACED ABOVE runSkeleton, WHICH IS ITSELF ABOVE runTexture, and for the same reason runSkeleton's
// own do-not-move note below gives: cooker.texture_nothing_written_on_failure pins a source-text
// ordering inside the region between `ExitCode runTexture(...)` and `ExitCode runMain(...)`, and
// that check requires `writeTextFileAtomic(args.outputPath` to occur EXACTLY ONCE in it. A fourth
// subcommand written there would put a second, unrelated occurrence in that region and make
// "before" ambiguous. This is the furthest point in the file from it. Do not move this function
// below runTexture.
//
// ONE ARTIFACT PER INVOCATION, chosen by --clip, on the skeleton path's terms: a .aeroanim is one
// clip, and cooking every clip of a multi-clip model in one run would need a naming rule for the
// outputs that nothing in this pipeline has.
ExitCode runAnimation(const Args& args) {
    ExitCode failure = ExitCode::CookError;
    const std::optional<ImportResult> imported = importModelForCook(args, failure);
    if (!imported.has_value()) {
        return failure;
    }

    // ---- 6. the cook -------------------------------------------------------------------------
    // The adapter owns every refusal that is about the MODEL (an out-of-range clip index, a
    // Structure-depth model) and every advisory (the multi-clip notice, a dropped channel); the cook
    // owns the ones about the BYTES (a duplicate (node, path) pair, a non-monotonic time list, a
    // count over its cap). Both arrive on one result, so the CLI needs no second implementation of
    // either.
    const engine::assets::AnimationCookResult cooked =
        engine::editor::cookImportedAnimation(imported->model, args.clipIndex, args.guid);
    // FIRST, so a refusal's warnings are not swallowed by the error path below.
    reportWarnings("cook", cooked.warnings, cooked.warningTotal);
    if (cooked.status == engine::assets::AnimationCookStatus::Invalid) {
        std::cerr << "aero_cooker: error: " << cooked.message << '\n';
        return ExitCode::CookError;
    }
    if (cooked.status == engine::assets::AnimationCookStatus::Truncated) {
        // Truncated still exits 0: the artifact is COHERENT, merely smaller -- the mesh path's own
        // reading of Truncated, and the importer's before it.
        std::cerr << "aero_cooker: warning: cook truncated: " << cooked.message << '\n';
    }

    // ---- 7. the write. ONLY NOW is the output path touched -----------------------------------
    // The same primitive, the same reason and the same refusal to create a directory as all three
    // older subcommands: writeTextFileAtomic is binary on BOTH sides, so a text-mode write cannot
    // turn a 0x0A inside a float into 0x0D 0x0A on exactly one lane.
    const std::string_view artifact(reinterpret_cast<const char*>(cooked.bytes.data()), cooked.bytes.size());
    const std::string writeError = engine::editor::writeTextFileAtomic(args.outputPath, artifact);
    if (!writeError.empty()) {
        std::cerr << "aero_cooker: error: cannot write '" << args.outputPath << "': " << writeError << '\n';
        return ExitCode::IoError;
    }
    return ExitCode::Success;
}

// ---- the skeleton subcommand (task 3.5.1) -------------------------------------------------------
//
// PLACED ABOVE runTexture DELIBERATELY, and the reason is a test rather than taste:
// cooker.texture_nothing_written_on_failure pins a source-text ordering inside the region between
// `ExitCode runTexture(...)` and `ExitCode runMain(...)`, and that check requires
// `writeTextFileAtomic(args.outputPath` to occur EXACTLY ONCE in it. A third subcommand written
// between those two would put a second, unrelated occurrence there and make "before" ambiguous. Do
// not move this function below runTexture.
//
// ONE ARTIFACT PER INVOCATION, chosen by --skin. Cooking every skin of a multi-skin model in one run
// would need a naming rule for the outputs and a way to say which cooked mesh each skeleton belongs
// to, and that PAIRING is instancing metadata -- docs/09 section 9.0's named gap, not this tool's.
ExitCode runSkeleton(const Args& args) {
    ExitCode failure = ExitCode::CookError;
    const std::optional<ImportResult> imported = importModelForCook(args, failure);
    if (!imported.has_value()) {
        return failure;
    }

    // ---- 6. the cook -------------------------------------------------------------------------
    // The adapter owns every refusal that is about the MODEL (an out-of-range skin index, a
    // Structure-depth model, a dangling joint) and every advisory (the multi-skin notice, the
    // weight-range scan); the cook owns the ones about the BYTES. Both arrive on one result, so the
    // CLI needs no second implementation of either.
    const engine::assets::SkeletonCookResult cooked =
        engine::editor::cookImportedSkeleton(imported->model, args.skinIndex, args.guid);
    // FIRST, so a refusal's warnings are not swallowed by the error path below.
    reportWarnings("cook", cooked.warnings, cooked.warnings.size());
    if (cooked.status == engine::assets::SkeletonCookStatus::Invalid) {
        std::cerr << "aero_cooker: error: " << cooked.message << '\n';
        return ExitCode::CookError;
    }

    // ---- 7. the write. ONLY NOW is the output path touched -----------------------------------
    // The same primitive, the same reason and the same refusal to create a directory as both older
    // subcommands: writeTextFileAtomic is binary on BOTH sides, so a text-mode write cannot turn a
    // 0x0A inside a float or a matrix cell into 0x0D 0x0A on exactly one lane.
    const std::string_view artifact(reinterpret_cast<const char*>(cooked.bytes.data()), cooked.bytes.size());
    const std::string writeError = engine::editor::writeTextFileAtomic(args.outputPath, artifact);
    if (!writeError.empty()) {
        std::cerr << "aero_cooker: error: cannot write '" << args.outputPath << "': " << writeError << '\n';
        return ExitCode::IoError;
    }
    return ExitCode::Success;
}

// `token` is already known to be one of the six (parseArgs checked), and --srgb has already been
// refused for bc4/bc5, so this switch-shaped chain is TOTAL over what can reach it. `auto` is the one
// token that consults the pixels, through the editor's own policy function -- which is why the policy
// lives one layer up from the cook and is called before it.
[[nodiscard]] engine::assets::CookedTextureFormat resolveTextureFormat(const std::string& token, bool srgb,
                                                                       std::span<const std::byte> rgba8) {
    using engine::assets::CookedTextureFormat;
    if (token == "bc1") {
        return srgb ? CookedTextureFormat::Bc1RgbSrgb : CookedTextureFormat::Bc1RgbUnorm;
    }
    if (token == "bc3") {
        return srgb ? CookedTextureFormat::Bc3Srgb : CookedTextureFormat::Bc3Unorm;
    }
    if (token == "bc4") {
        return CookedTextureFormat::Bc4Unorm;  // no sRGB variant exists; parseArgs refused the pair
    }
    if (token == "bc5") {
        return CookedTextureFormat::Bc5Unorm;  // likewise
    }
    if (token == "rgba8") {
        return srgb ? CookedTextureFormat::Rgba8Srgb : CookedTextureFormat::Rgba8Unorm;
    }
    return engine::editor::chooseTextureFormat(rgba8, srgb);
}

ExitCode runTexture(const Args& args) {
    // ---- 1. the file NAME decides what happens, before a byte is read ------------------------
    // The same rule and the same reason as the mesh path's .blend arm: readFileBytes refuses an
    // over-cap file WITHOUT OPENING IT, so reading first would answer a 300 MB .hdr with "the file is
    // too large" instead of "HDR is not supported". A .hdr gets its own message because stb_image does
    // NOT fail on a Radiance file -- it silently applies a fixed gamma-2.2 tone map and hands back
    // 8-bit LDR bytes, so cooking one produces a plausible artifact that is quietly wrong.
    const std::string leaf = fs::path(args.inputPath).filename().string();
    if (!engine::editor::isCookableTextureName(leaf)) {
        if (engine::editor::isHdrTextureName(leaf)) {
            std::cerr << "aero_cooker: error: '" << leaf
                      << "' is a high-dynamic-range image and is not cooked by v1 -- stb_image would silently "
                         "tone-map it to 8 bits through a fixed gamma-2.2 curve, producing a plausible artifact "
                         "that is quietly wrong. HDR belongs to BC6H, which arrives with a later task.\n";
        } else {
            std::cerr << "aero_cooker: error: no decoder claims '" << leaf
                      << "' (expected .png .jpg .jpeg .tga .bmp .gif .psd)\n";
        }
        return ExitCode::CookError;
    }

    // ---- 2. the source bytes ------------------------------------------------------------------
    // MAX_TEXTURE_FILE_BYTES, not MAX_MODEL_FILE_BYTES: the cap bounds the COMPRESSED source file,
    // while the decoded pixel count is bounded separately and per-axis by decodeImageRgba8 below.
    engine::editor::FileBytesResult source =
        engine::editor::readFileBytes(args.inputPath, engine::editor::MAX_TEXTURE_FILE_BYTES);
    if (!source.bytes.has_value()) {
        if (source.refusedByCap) {
            std::cerr << "aero_cooker: error: '" << args.inputPath << "' is " << source.size << " bytes, above the "
                      << engine::editor::MAX_TEXTURE_FILE_BYTES << "-byte texture read limit\n";
        } else {
            std::cerr << "aero_cooker: error: cannot read '" << args.inputPath << "': " << source.error << '\n';
        }
        return ExitCode::IoError;
    }

    // ---- 3. the decode ------------------------------------------------------------------------
    const engine::editor::DecodedImage image =
        engine::editor::decodeImageRgba8(asBytes(*source.bytes), engine::assets::MAX_TEXTURE_DIMENSION);
    if (!image.error.empty()) {
        std::cerr << "aero_cooker: error: cannot decode '" << leaf << "': " << image.error << '\n';
        return ExitCode::CookError;
    }

    // ---- 4. the format, then 5. the cook ------------------------------------------------------
    engine::assets::TextureCookInput input;
    input.sourceGuid = args.guid;
    input.width = image.width;
    input.height = image.height;
    input.rgba8 = image.rgba8;
    input.format = resolveTextureFormat(args.formatToken, args.srgb, image.rgba8);
    input.generateMips = args.generateMips;

    const engine::assets::TextureCookResult cooked = engine::assets::cookTexture(input);
    // FIRST, so a refusal's warnings are not swallowed by the error path below.
    reportWarnings("cook", cooked.warnings, cooked.warningTotal);
    if (cooked.status == engine::assets::TextureCookStatus::Refused) {
        std::cerr << "aero_cooker: error: " << cooked.message << '\n';
        return ExitCode::CookError;
    }

    // ---- 6. the write. ONLY NOW is the output path touched -------------------------------------
    // writeTextFileAtomic, the same call the mesh path makes: it is BINARY ON BOTH SIDES
    // (std::ios::binary | std::ios::trunc), so its name is about its ATOMICITY and not about text
    // mode. That is what stops a text-mode write turning every 0x0A in the container into 0x0D 0x0A on
    // exactly one lane -- and a KTX2 identifier ends in 0x0D 0x0A 0x1A 0x0A, so this file would be
    // corrupt at byte 8. Do not "fix" the name by reaching for a different primitive.
    const std::string_view artifact(reinterpret_cast<const char*>(cooked.bytes.data()), cooked.bytes.size());
    const std::string writeError = engine::editor::writeTextFileAtomic(args.outputPath, artifact);
    if (!writeError.empty()) {
        std::cerr << "aero_cooker: error: cannot write '" << args.outputPath << "': " << writeError << '\n';
        return ExitCode::IoError;
    }
    return ExitCode::Success;
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
    // The subcommand split. Task 3.3.2 added the first branch, task 3.5.1 the second and task 3.5.2
    // the third; the mesh path below is still the same sequence it was at 3.3.1, with steps 2-5 now
    // living in importModelForCook so the skeleton and animation paths can share them rather than
    // copy them.
    if (args.subcommand == Subcommand::Texture) {
        return runTexture(args);
    }
    if (args.subcommand == Subcommand::Skeleton) {
        return runSkeleton(args);
    }
    if (args.subcommand == Subcommand::Animation) {
        return runAnimation(args);
    }

    // ---- 2-5. the name check, the capped read, Structure, the external budget, Full ----------
    ExitCode failure = ExitCode::CookError;
    const std::optional<ImportResult> imported = importModelForCook(args, failure);
    if (!imported.has_value()) {
        return failure;
    }

    // ---- 6. the cook -------------------------------------------------------------------------
    const engine::assets::MeshCookResult cooked = engine::editor::cookImportedModel(imported->model, args.guid);
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

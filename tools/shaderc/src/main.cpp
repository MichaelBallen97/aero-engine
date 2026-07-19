// tools/shaderc/src/main.cpp — task 0.4.3: aero_shaderc, the first-party HLSL -> SPIR-V/MSL/DXIL
// compiler CLI. This is the ONLY frozen contract surface consumers (CMake, 0.4.4's loader, the
// future cooker) depend on (spec D1/D4/D5) — the SDL_shadercross calls below are an implementation
// detail that can be swapped for raw DXC + SPIRV-Cross (see README.md's fallback recipe) without
// this file's argv grammar or JSON schema changing.
//
// Freestanding tool: no aero:: engine headers, no aero::core link (spec 3.3) — only the toolchain
// library and the standard library, so tools/ never drags spdlog/vcpkg usage requirements into a
// build-time-only binary. Diagnostics go to stderr only; stdout is reserved for --help/--version.

#include <SDL3_shadercross/SDL_shadercross.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

// The exact SDL_shadercross commit this tool is built against (tools/shaderc/bootstrap.cmake pins
// the same SHA) — spec D5's frozen JSON "toolchain" constant. Bumping the pin (spec H4) means
// bumping this string in the same commit.
constexpr std::string_view TOOLCHAIN_ID = "sdl-shadercross@1ca46e0e";
constexpr std::string_view TOOL_VERSION = "0.1.0";
// The fixed entry-point convention (spec D4): every engine shader's HLSL entry function is `main`,
// and Metal's cleansed rename of that is always `main0` (spec F7/D5/E11) — a fixed rule, not
// introspected from the transpiled MSL text.
constexpr std::string_view ENTRY_POINT = "main";
constexpr std::string_view MSL_ENTRY_POINT = "main0";

enum class ExitCode : std::uint8_t {
    Success = 0,
    UsageError = 1,
    CompileError = 2,
    IoError = 3,
};

// RAII for every SDL_malloc'd buffer this tool touches (spirv/dxil bytes, the msl string, the
// reflect metadata) — calling SDL_free keeps our own leak story clean independent of D8's
// ASAN_OPTIONS=detect_leaks=0 (that env var covers the toolchain's OWN by-design exit leaks, not
// ours). A single non-template deleter works for every pointee type: unique_ptr invokes
// `deleter(ptr)`, and any object pointer converts implicitly to `void*`.
struct SdlFreeDeleter {
    void operator()(void* ptr) const noexcept {
        if (ptr != nullptr) {
            SDL_free(ptr);
        }
    }
};
template <typename T>
using SdlUniquePtr = std::unique_ptr<T, SdlFreeDeleter>;

// --- argv grammar (spec D4, frozen) --------------------------------------------------------------
//
//   aero_shaderc --input <file.hlsl> --stage vertex|fragment --output-dir <dir>
//                [--name <base>] [--formats spirv,msl,dxil] [--include <dir>]
//                [--define NAME[=VALUE]]... [--version] [--help]
//
// No --entry, --debug, --msl-version, no compute, no DXBC in v1 (spec D4) — any such flag is simply
// unrecognized and hits the unknown-flag usage error below; lifting the limit is an append-later
// change to this grammar, never a silent extension.

void printUsage(std::ostream& out) {
    out << "aero_shaderc " << TOOL_VERSION << " -- HLSL -> SPIR-V/MSL/DXIL (task 0.4.3)\n"
        << "\n"
        << "Usage:\n"
        << "  aero_shaderc --input <file.hlsl> --stage vertex|fragment --output-dir <dir>\n"
        << "               [--name <base>] [--formats spirv,msl,dxil] [--include <dir>]\n"
        << "               [--define NAME[=VALUE]]... \n"
        << "  aero_shaderc --version\n"
        << "  aero_shaderc --help\n"
        << "\n"
        << "Required:\n"
        << "  --input <file.hlsl>    Path to the HLSL source. Entry point is always `main`.\n"
        << "  --stage <stage>        `vertex` or `fragment` (no compute in v1).\n"
        << "  --output-dir <dir>     Directory the artifacts are written into (created if absent).\n"
        << "\n"
        << "Optional:\n"
        << "  --name <base>          Output basename. Default: <input filename minus \".hlsl\">.\n"
        << "  --formats <list>       Comma-separated subset of spirv,msl,dxil. Default: all three.\n"
        << "  --include <dir>        Include directory for #include-d HLSL. May be given at most\n"
        << "                         once -- SDL_shadercross's HLSL_Info::include_dir is a single\n"
        << "                         path, not a list.\n"
        << "  --define NAME[=VALUE]  Preprocessor define; repeatable. If NAME repeats, the LAST\n"
        << "                         occurrence wins (matches compiler -D convention).\n"
        << "\n"
        << "Every requested step must succeed before ANY output file is written -- a failing shader\n"
        << "leaves zero artifacts in <dir> (no partial/stale outputs).\n"
        << "\n"
        << "Exit codes: 0 success, 1 usage error, 2 compile/transpile/reflect error, 3 I/O error.\n";
}

void printVersion(std::ostream& out) {
    out << "aero_shaderc " << TOOL_VERSION << " (toolchain: " << TOOLCHAIN_ID << ")\n";
}

struct ParsedDefine {
    std::string name;
    std::optional<std::string> value;
};

struct FormatSet {
    bool spirv = false;
    bool msl = false;
    bool dxil = false;
    [[nodiscard]] bool any() const noexcept { return spirv || msl || dxil; }
};

struct Args {
    std::string inputPath;
    std::string stage;  // "vertex" | "fragment", validated
    std::string outputDir;
    std::optional<std::string> name;
    std::optional<std::string> includeDir;
    std::vector<ParsedDefine> defines;  // argv order; last-name-wins collapse happens later
    FormatSet formats;                  // resolved: explicit subset, or all three if --formats absent
    bool wantHelp = false;
    bool wantVersion = false;
};

// Splits "NAME=VALUE" or "NAME" into a ParsedDefine. Returns nullopt if NAME would be empty.
std::optional<ParsedDefine> parseDefine(std::string_view token) {
    const auto eq = token.find('=');
    if (eq == std::string_view::npos) {
        if (token.empty()) {
            return std::nullopt;
        }
        return ParsedDefine{std::string(token), std::nullopt};
    }
    const std::string_view name = token.substr(0, eq);
    if (name.empty()) {
        return std::nullopt;
    }
    return ParsedDefine{std::string(name), std::string(token.substr(eq + 1))};
}

// Collapses argv-order defines into last-name-wins order (spec E6). Stable on first-seen position;
// only the value can be overwritten by a later occurrence of the same name.
std::vector<ParsedDefine> collapseDefines(const std::vector<ParsedDefine>& in) {
    std::vector<ParsedDefine> out;
    for (const auto& d : in) {
        auto it = std::find_if(out.begin(), out.end(), [&](const ParsedDefine& e) { return e.name == d.name; });
        if (it != out.end()) {
            it->value = d.value;
        } else {
            out.push_back(d);
        }
    }
    return out;
}

// Parses "--formats" comma list into a FormatSet. Returns nullopt on an empty token, an unknown
// token, or a duplicate token (spec E5 -- all three are usage errors).
std::optional<FormatSet> parseFormats(std::string_view csv) {
    FormatSet result;
    std::size_t start = 0;
    while (start <= csv.size()) {
        const auto comma = csv.find(',', start);
        const std::string_view token =
            (comma == std::string_view::npos) ? csv.substr(start) : csv.substr(start, comma - start);
        if (token.empty()) {
            return std::nullopt;
        }
        if (token == "spirv") {
            if (result.spirv) {
                return std::nullopt;
            }
            result.spirv = true;
        } else if (token == "msl") {
            if (result.msl) {
                return std::nullopt;
            }
            result.msl = true;
        } else if (token == "dxil") {
            if (result.dxil) {
                return std::nullopt;
            }
            result.dxil = true;
        } else {
            return std::nullopt;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return result;
}

// Hand-rolled argv parse (spec D4 flow step 1; no getopt -- Windows has none). Any violation prints
// usage to stderr and the caller exits 1. Returns nullopt on any usage violation.
std::optional<Args> parseArgs(int argc, char** argv) {
    Args args;
    bool haveInput = false;
    bool haveStage = false;
    bool haveOutputDir = false;
    bool haveInclude = false;
    bool haveFormats = false;

    // --help/--version short-circuit: found anywhere, they win immediately (common CLI ergonomics).
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--help") {
            args.wantHelp = true;
            return args;
        }
        if (a == "--version") {
            args.wantVersion = true;
            return args;
        }
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        const auto needValue = [&](std::string_view name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "aero_shaderc: error: " << name << " requires a value\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (flag == "--input") {
            const char* v = needValue(flag);
            if (v == nullptr) {
                return std::nullopt;
            }
            args.inputPath = v;
            haveInput = true;
        } else if (flag == "--stage") {
            const char* v = needValue(flag);
            if (v == nullptr) {
                return std::nullopt;
            }
            args.stage = v;
            haveStage = true;
        } else if (flag == "--output-dir") {
            const char* v = needValue(flag);
            if (v == nullptr) {
                return std::nullopt;
            }
            args.outputDir = v;
            haveOutputDir = true;
        } else if (flag == "--name") {
            const char* v = needValue(flag);
            if (v == nullptr) {
                return std::nullopt;
            }
            args.name = std::string(v);
        } else if (flag == "--formats") {
            const char* v = needValue(flag);
            if (v == nullptr) {
                return std::nullopt;
            }
            haveFormats = true;
            auto parsed = parseFormats(v);
            if (!parsed.has_value()) {
                std::cerr << "aero_shaderc: error: invalid --formats value '" << v
                          << "' (expected a comma list from spirv,msl,dxil, no duplicates)\n";
                return std::nullopt;
            }
            args.formats = *parsed;
        } else if (flag == "--include") {
            if (haveInclude) {
                std::cerr << "aero_shaderc: error: --include may be given at most once "
                             "(SDL_shadercross's include_dir is a single path)\n";
                return std::nullopt;
            }
            const char* v = needValue(flag);
            if (v == nullptr) {
                return std::nullopt;
            }
            args.includeDir = std::string(v);
            haveInclude = true;
        } else if (flag == "--define") {
            const char* v = needValue(flag);
            if (v == nullptr) {
                return std::nullopt;
            }
            auto parsed = parseDefine(v);
            if (!parsed.has_value()) {
                std::cerr << "aero_shaderc: error: invalid --define value '" << v << "' (expected NAME[=VALUE])\n";
                return std::nullopt;
            }
            args.defines.push_back(std::move(*parsed));
        } else {
            std::cerr << "aero_shaderc: error: unknown flag '" << flag << "'\n";
            return std::nullopt;
        }
    }

    if (!haveInput || !haveStage || !haveOutputDir) {
        std::cerr << "aero_shaderc: error: --input, --stage, and --output-dir are required\n";
        return std::nullopt;
    }
    if (args.stage != "vertex" && args.stage != "fragment") {
        std::cerr << "aero_shaderc: error: --stage must be 'vertex' or 'fragment' (got '" << args.stage << "')\n";
        return std::nullopt;
    }
    if (!haveFormats) {
        args.formats = FormatSet{.spirv = true, .msl = true, .dxil = true};
    }
    return args;
}

// --- JSON writer (spec D5 exact schema; hand-rolled -- no library dependency for a fixed shape) --

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            default:
                out += c;
        }
    }
    return out;
}

std::string buildJson(std::string_view name, std::string_view stage, std::uint32_t samplerCount,
                      std::uint32_t storageTextureCount, std::uint32_t storageBufferCount,
                      std::uint32_t uniformBufferCount, const FormatSet& emitted) {
    std::ostringstream out;
    out << "{\n"
        << "  \"schema\": 1,\n"
        << R"(  "toolchain": ")" << TOOLCHAIN_ID << "\",\n"
        << R"(  "name": ")" << jsonEscape(name) << "\",\n"
        << R"(  "stage": ")" << stage << "\",\n"
        << R"(  "entryPoint": ")" << ENTRY_POINT << "\",\n"
        << R"(  "mslEntryPoint": ")" << MSL_ENTRY_POINT << "\",\n"
        << "  \"samplerCount\": " << samplerCount << ",\n"
        << "  \"storageTextureCount\": " << storageTextureCount << ",\n"
        << "  \"storageBufferCount\": " << storageBufferCount << ",\n"
        << "  \"uniformBufferCount\": " << uniformBufferCount << ",\n"
        << "  \"formats\": [";
    bool first = true;
    for (const auto& [want, token] :
         {std::pair{emitted.spirv, "spirv"}, std::pair{emitted.msl, "msl"}, std::pair{emitted.dxil, "dxil"}}) {
        if (!want) {
            continue;
        }
        if (!first) {
            out << ", ";
        }
        out << "\"" << token << "\"";
        first = false;
    }
    out << "]\n}\n";
    return out.str();
}

// Reads the whole file as raw bytes (no text-mode translation -- DXC handles BOM/encoding itself,
// spec 3.3 step 2). Returns nullopt on any failure (missing file, permissions, ...).
std::optional<std::string> readFileBinary(const fs::path& path) {
    const std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (!in.good() && !in.eof()) {
        return std::nullopt;
    }
    return buffer.str();
}

// Writes `content` to `path` in binary mode; returns false on any failure.
bool writeFileBinary(const fs::path& path, const void* data, std::size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    if (size > 0) {
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
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

    // --- step 2: read the input (spec 3.3) --------------------------------------------------
    const std::optional<std::string> source = readFileBinary(args.inputPath);
    if (!source.has_value()) {
        std::cerr << "aero_shaderc: error: cannot read input file '" << args.inputPath << "'\n";
        return ExitCode::IoError;
    }

    const std::string base = args.name.value_or([&] {
        std::string stem = fs::path(args.inputPath).filename().string();
        constexpr std::string_view SUFFIX = ".hlsl";
        if (stem.size() > SUFFIX.size() && stem.compare(stem.size() - SUFFIX.size(), SUFFIX.size(), SUFFIX) == 0) {
            stem.erase(stem.size() - SUFFIX.size());
        }
        return stem;
    }());

    const SDL_ShaderCross_ShaderStage stage =
        (args.stage == "vertex") ? SDL_SHADERCROSS_SHADERSTAGE_VERTEX : SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;

    // --- step 3: init (paranoia-checked; F7: always returns true today) --------------------
    if (!SDL_ShaderCross_Init()) {
        std::cerr << "aero_shaderc: error: SDL_ShaderCross_Init failed: " << SDL_GetError() << '\n';
        return ExitCode::CompileError;
    }

    // --- step 4: build HLSL_Info (spec 3.3) -------------------------------------------------
    const std::vector<ParsedDefine> defines = collapseDefines(args.defines);
    std::vector<SDL_ShaderCross_HLSL_Define> sdlDefines;
    sdlDefines.reserve(defines.size() + 1);
    for (const auto& d : defines) {
        sdlDefines.push_back(SDL_ShaderCross_HLSL_Define{
            .name = const_cast<char*>(d.name.c_str()),
            .value = d.value.has_value() ? const_cast<char*>(d.value->c_str()) : nullptr,
        });
    }
    sdlDefines.push_back(SDL_ShaderCross_HLSL_Define{.name = nullptr, .value = nullptr});

    SDL_ShaderCross_HLSL_Info hlslInfo{};
    hlslInfo.source = source->c_str();
    hlslInfo.entrypoint = ENTRY_POINT.data();
    hlslInfo.include_dir = args.includeDir.has_value() ? args.includeDir->c_str() : nullptr;
    hlslInfo.defines = defines.empty() ? nullptr : sdlDefines.data();
    hlslInfo.shader_stage = stage;
    hlslInfo.props = 0;

    // --- step 5: HLSL -> SPIR-V, ALWAYS (reflection source + MSL input, spec D5) ------------
    std::size_t spirvSize = 0;
    const SdlUniquePtr<void> spirv(SDL_ShaderCross_CompileSPIRVFromHLSL(&hlslInfo, &spirvSize));
    if (!spirv) {
        std::cerr << "aero_shaderc: error: HLSL -> SPIR-V compile failed: " << SDL_GetError() << '\n';
        SDL_ShaderCross_Quit();
        return ExitCode::CompileError;
    }

    // --- step 6: reflect (spec D5; E10 -- a distinct message from a compile failure) --------
    SdlUniquePtr<SDL_ShaderCross_GraphicsShaderMetadata> reflected(
        SDL_ShaderCross_ReflectGraphicsSPIRV(static_cast<const Uint8*>(spirv.get()), spirvSize, 0));
    if (!reflected) {
        std::cerr << "aero_shaderc: error: SPIR-V reflection failed: " << SDL_GetError() << '\n';
        SDL_ShaderCross_Quit();
        return ExitCode::CompileError;
    }
    const SDL_ShaderCross_GraphicsShaderResourceInfo counts = reflected->resource_info;

    // --- step 7: MSL transpile, if requested -------------------------------------------------
    SdlUniquePtr<void> msl;
    std::size_t mslLength = 0;
    if (args.formats.msl) {
        SDL_ShaderCross_SPIRV_Info spirvInfo{};
        spirvInfo.bytecode = static_cast<const Uint8*>(spirv.get());
        spirvInfo.bytecode_size = spirvSize;
        spirvInfo.entrypoint = ENTRY_POINT.data();
        spirvInfo.shader_stage = stage;
        spirvInfo.props = 0;
        msl.reset(SDL_ShaderCross_TranspileMSLFromSPIRV(&spirvInfo));
        if (!msl) {
            std::cerr << "aero_shaderc: error: SPIR-V -> MSL transpile failed: " << SDL_GetError() << '\n';
            SDL_ShaderCross_Quit();
            return ExitCode::CompileError;
        }
        mslLength = std::strlen(static_cast<const char*>(msl.get()));
    }

    // --- step 8: DXIL compile, if requested (native HLSL path) -----------------------------
    SdlUniquePtr<void> dxil;
    std::size_t dxilSize = 0;
    if (args.formats.dxil) {
        dxil.reset(SDL_ShaderCross_CompileDXILFromHLSL(&hlslInfo, &dxilSize));
        if (!dxil) {
            std::cerr << "aero_shaderc: error: HLSL -> DXIL compile failed: " << SDL_GetError() << '\n';
            SDL_ShaderCross_Quit();
            return ExitCode::CompileError;
        }
    }

    // --- step 9: write, ONLY now that every requested step has succeeded (E1) --------------
    std::error_code ec;
    fs::create_directories(args.outputDir, ec);
    if (ec) {
        std::cerr << "aero_shaderc: error: cannot create output directory '" << args.outputDir << "': " << ec.message()
                  << '\n';
        SDL_ShaderCross_Quit();
        return ExitCode::IoError;
    }

    const fs::path outDir(args.outputDir);
    std::vector<fs::path> written;
    const auto tryWrite = [&](const fs::path& path, const void* data, std::size_t size) -> bool {
        if (!writeFileBinary(path, data, size)) {
            return false;
        }
        written.push_back(path);
        return true;
    };

    bool ok = true;
    if (args.formats.spirv) {
        ok = ok && tryWrite(outDir / (base + ".spv"), spirv.get(), spirvSize);
    }
    if (args.formats.msl) {
        ok = ok && tryWrite(outDir / (base + ".msl"), msl.get(), mslLength);
    }
    if (args.formats.dxil) {
        ok = ok && tryWrite(outDir / (base + ".dxil"), dxil.get(), dxilSize);
    }
    if (ok) {
        const std::string json = buildJson(base, args.stage, counts.num_samplers, counts.num_storage_textures,
                                           counts.num_storage_buffers, counts.num_uniform_buffers, args.formats);
        ok = tryWrite(outDir / (base + ".json"), json.data(), json.size());
    }

    if (!ok) {
        std::cerr << "aero_shaderc: error: failed to write artifacts under '" << args.outputDir << "'\n";
        for (const auto& p : written) {
            std::error_code removeEc;
            fs::remove(p, removeEc);  // best-effort (E1 tail); a failed write is already fatal
        }
        SDL_ShaderCross_Quit();
        return ExitCode::IoError;
    }

    SDL_ShaderCross_Quit();
    return ExitCode::Success;
}

// The real entry point is a thin, non-throwing wrapper (docs/04: no exceptions across a public API
// boundary -- main() is this freestanding tool's outermost one). std::filesystem/std::string/etc. can
// theoretically throw (bad_alloc, filesystem_error) in extreme conditions runMain() does not
// individually guard against; catching here turns any such escape into a clean diagnostic + exit
// code instead of an uncaught-exception abort.
int main(int argc, char** argv) {
    try {
        return static_cast<int>(runMain(argc, argv));
    } catch (const std::exception& e) {
        std::cerr << "aero_shaderc: error: unexpected exception: " << e.what() << '\n';
        return static_cast<int>(ExitCode::IoError);
    } catch (...) {
        std::cerr << "aero_shaderc: error: unexpected exception\n";
        return static_cast<int>(ExitCode::IoError);
    }
}

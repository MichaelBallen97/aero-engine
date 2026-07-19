// Aero Engine — the shader loader (task 0.4.4): the runtime CONSUMER of the tools/shaderc artifact
// contract (0.4.3 D4/D5). Sits ABOVE the SDL_GPU backend and speaks only the public Device/VFS
// vocabulary — this TU includes NO SDL header and no third-party JSON library (boundary rule); the
// JSON reader below is a small, hand-rolled, schema-1-only scanner (spec D2), not a general parser.

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/vfs.hpp>
#include <aero/rhi/descriptors.hpp>
#include <aero/rhi/device.hpp>
#include <aero/rhi/shader_loader.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::rhi {
namespace {

// --- the hand-rolled schema-1 scanner (spec 3.3) ------------------------------------------------
// Not a general JSON parser: a flat object of known keys. It never recurses (the only container is
// the flat "formats" string array). Every helper advances a std::size_t cursor over the whole input.

void skipWs(std::string_view sv, std::size_t& pos) {
    while (pos < sv.size() && (sv[pos] == ' ' || sv[pos] == '\t' || sv[pos] == '\n' || sv[pos] == '\r')) {
        ++pos;
    }
}

// Requires a leading '"'; copies bytes up to the matching closing '"'. Accepts ONLY the two escapes
// the writer ever emits (F1): \" -> " and \\ -> \. Any other \x, or EOF before the closing quote, is
// a hard parse error (E15) -- the producer never emits \uXXXX or any other escape.
std::optional<std::string> parseString(std::string_view sv, std::size_t& pos) {
    if (pos >= sv.size() || sv[pos] != '"') {
        return std::nullopt;
    }
    ++pos;
    std::string out;
    while (true) {
        if (pos >= sv.size()) {
            return std::nullopt;  // EOF mid-string
        }
        const char c = sv[pos];
        if (c == '"') {
            ++pos;
            return out;
        }
        if (c == '\\') {
            ++pos;
            if (pos >= sv.size()) {
                return std::nullopt;  // EOF right after a backslash
            }
            const char escaped = sv[pos];
            if (escaped == '"') {
                out.push_back('"');
            } else if (escaped == '\\') {
                out.push_back('\\');
            } else {
                return std::nullopt;  // any escape besides \" and \\ is unsupported (E15)
            }
            ++pos;
            continue;
        }
        out.push_back(c);
        ++pos;
    }
}

// Requires >= 1 ASCII digit; accumulates in uint64_t and rejects a value > UINT32_MAX. A leading
// '-', '+', '.', or a trailing '.'/'e'/'E' all fail the "digit" checks below (schema counts and the
// schema number itself are always plain non-negative integers, never signed/fractional/exponential).
std::optional<std::uint32_t> parseUint(std::string_view sv, std::size_t& pos) {
    if (pos >= sv.size() || sv[pos] < '0' || sv[pos] > '9') {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    while (pos < sv.size() && sv[pos] >= '0' && sv[pos] <= '9') {
        value = (value * 10) + static_cast<std::uint64_t>(sv[pos] - '0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;  // overflow -- reject rather than silently truncate
        }
        ++pos;
    }
    // A digit run immediately followed by '.', 'e', or 'E' is a float/exponent form: not a plain uint.
    if (pos < sv.size() && (sv[pos] == '.' || sv[pos] == 'e' || sv[pos] == 'E')) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

// '[' ... ']' of parseString results, comma-separated; '[]' is allowed (the caller decides whether an
// empty "formats" is an error).
std::optional<std::vector<std::string>> parseStringArray(std::string_view sv, std::size_t& pos) {
    if (pos >= sv.size() || sv[pos] != '[') {
        return std::nullopt;
    }
    ++pos;
    std::vector<std::string> out;
    skipWs(sv, pos);
    if (pos < sv.size() && sv[pos] == ']') {
        ++pos;
        return out;
    }
    while (true) {
        skipWs(sv, pos);
        std::optional<std::string> element = parseString(sv, pos);
        if (!element) {
            return std::nullopt;
        }
        out.push_back(std::move(*element));
        skipWs(sv, pos);
        if (pos >= sv.size()) {
            return std::nullopt;
        }
        if (sv[pos] == ',') {
            ++pos;
            continue;
        }
        if (sv[pos] == ']') {
            ++pos;
            return out;
        }
        return std::nullopt;
    }
}

// There is no public ShaderFormat toString (F2) -- this is a TU-local convenience for log messages.
constexpr std::string_view formatName(ShaderFormat format) noexcept {
    switch (format) {
        case ShaderFormat::SpirV:
            return "spirv";
        case ShaderFormat::Dxil:
            return "dxil";
        case ShaderFormat::Msl:
            return "msl";
    }
    return "unknown";  // unreachable on the 3-value enum; keeps the compiler happy
}

// Bits tracking which of the 10 REQUIRED keys have been seen (D2 strictness: a duplicate or a
// missing required key is a hard error, not silently ignored). "toolchain" is intentionally absent
// here -- it is parsed when present but is informational-only and never required (D6).
struct SeenKeys {
    bool schema = false;
    bool name = false;
    bool stage = false;
    bool entryPoint = false;
    bool mslEntryPoint = false;
    bool samplerCount = false;
    bool storageTextureCount = false;
    bool storageBufferCount = false;
    bool uniformBufferCount = false;
    bool formats = false;
};

}  // namespace

std::optional<ShaderMetadata> parseShaderMetadata(std::string_view json) {
    std::size_t pos = 0;
    skipWs(json, pos);
    if (pos >= json.size() || json[pos] != '{') {
        AERO_LOG_ERROR("shader metadata: root value is not an object");
        return std::nullopt;
    }
    ++pos;

    ShaderMetadata meta{};
    SeenKeys seen{};

    while (true) {
        skipWs(json, pos);
        if (pos < json.size() && json[pos] == '}') {
            ++pos;
            break;
        }

        const std::optional<std::string> key = parseString(json, pos);
        if (!key) {
            AERO_LOG_ERROR("shader metadata: expected a quoted key");
            return std::nullopt;
        }
        skipWs(json, pos);
        if (pos >= json.size() || json[pos] != ':') {
            AERO_LOG_ERROR("shader metadata: expected ':' after key '{}'", *key);
            return std::nullopt;
        }
        ++pos;
        skipWs(json, pos);

        if (*key == "schema") {
            if (seen.schema) {
                AERO_LOG_ERROR("shader metadata: duplicate key 'schema'");
                return std::nullopt;
            }
            const std::optional<std::uint32_t> schema = parseUint(json, pos);
            if (!schema) {
                AERO_LOG_ERROR("shader metadata: 'schema' must be a non-negative integer");
                return std::nullopt;
            }
            if (*schema != 1) {
                AERO_LOG_ERROR("shader metadata: unsupported schema version {} (loader supports 1)", *schema);
                return std::nullopt;
            }
            seen.schema = true;
        } else if (*key == "stage") {
            if (seen.stage) {
                AERO_LOG_ERROR("shader metadata: duplicate key 'stage'");
                return std::nullopt;
            }
            const std::optional<std::string> stage = parseString(json, pos);
            if (!stage) {
                AERO_LOG_ERROR("shader metadata: 'stage' must be a string");
                return std::nullopt;
            }
            if (*stage == "vertex") {
                meta.stage = ShaderStage::Vertex;
            } else if (*stage == "fragment") {
                meta.stage = ShaderStage::Fragment;
            } else {
                AERO_LOG_ERROR("shader metadata: unknown stage '{}'", *stage);
                return std::nullopt;
            }
            seen.stage = true;
        } else if (*key == "name") {
            if (seen.name) {
                AERO_LOG_ERROR("shader metadata: duplicate key 'name'");
                return std::nullopt;
            }
            std::optional<std::string> value = parseString(json, pos);
            if (!value) {
                AERO_LOG_ERROR("shader metadata: 'name' must be a string");
                return std::nullopt;
            }
            meta.name = std::move(*value);
            seen.name = true;
        } else if (*key == "entryPoint") {
            if (seen.entryPoint) {
                AERO_LOG_ERROR("shader metadata: duplicate key 'entryPoint'");
                return std::nullopt;
            }
            std::optional<std::string> value = parseString(json, pos);
            if (!value) {
                AERO_LOG_ERROR("shader metadata: 'entryPoint' must be a string");
                return std::nullopt;
            }
            meta.entryPoint = std::move(*value);
            seen.entryPoint = true;
        } else if (*key == "mslEntryPoint") {
            if (seen.mslEntryPoint) {
                AERO_LOG_ERROR("shader metadata: duplicate key 'mslEntryPoint'");
                return std::nullopt;
            }
            std::optional<std::string> value = parseString(json, pos);
            if (!value) {
                AERO_LOG_ERROR("shader metadata: 'mslEntryPoint' must be a string");
                return std::nullopt;
            }
            meta.mslEntryPoint = std::move(*value);
            seen.mslEntryPoint = true;
        } else if (*key == "toolchain") {
            // D6: informational only -- not a required key, never validated. A repeat simply overwrites.
            std::optional<std::string> value = parseString(json, pos);
            if (!value) {
                AERO_LOG_ERROR("shader metadata: 'toolchain' must be a string");
                return std::nullopt;
            }
            meta.toolchain = std::move(*value);
        } else if (*key == "samplerCount") {
            if (seen.samplerCount) {
                AERO_LOG_ERROR("shader metadata: duplicate key 'samplerCount'");
                return std::nullopt;
            }
            const std::optional<std::uint32_t> value = parseUint(json, pos);
            if (!value) {
                AERO_LOG_ERROR("shader metadata: 'samplerCount' must be a non-negative integer");
                return std::nullopt;
            }
            meta.samplerCount = *value;
            seen.samplerCount = true;
        } else if (*key == "storageTextureCount") {
            if (seen.storageTextureCount) {
                AERO_LOG_ERROR("shader metadata: duplicate key 'storageTextureCount'");
                return std::nullopt;
            }
            const std::optional<std::uint32_t> value = parseUint(json, pos);
            if (!value) {
                AERO_LOG_ERROR("shader metadata: 'storageTextureCount' must be a non-negative integer");
                return std::nullopt;
            }
            meta.storageTextureCount = *value;
            seen.storageTextureCount = true;
        } else if (*key == "storageBufferCount") {
            if (seen.storageBufferCount) {
                AERO_LOG_ERROR("shader metadata: duplicate key 'storageBufferCount'");
                return std::nullopt;
            }
            const std::optional<std::uint32_t> value = parseUint(json, pos);
            if (!value) {
                AERO_LOG_ERROR("shader metadata: 'storageBufferCount' must be a non-negative integer");
                return std::nullopt;
            }
            meta.storageBufferCount = *value;
            seen.storageBufferCount = true;
        } else if (*key == "uniformBufferCount") {
            if (seen.uniformBufferCount) {
                AERO_LOG_ERROR("shader metadata: duplicate key 'uniformBufferCount'");
                return std::nullopt;
            }
            const std::optional<std::uint32_t> value = parseUint(json, pos);
            if (!value) {
                AERO_LOG_ERROR("shader metadata: 'uniformBufferCount' must be a non-negative integer");
                return std::nullopt;
            }
            meta.uniformBufferCount = *value;
            seen.uniformBufferCount = true;
        } else if (*key == "formats") {
            if (seen.formats) {
                AERO_LOG_ERROR("shader metadata: duplicate key 'formats'");
                return std::nullopt;
            }
            const std::optional<std::vector<std::string>> tokens = parseStringArray(json, pos);
            if (!tokens) {
                AERO_LOG_ERROR("shader metadata: 'formats' must be an array of strings");
                return std::nullopt;
            }
            for (const std::string& token : *tokens) {
                if (token == "spirv") {
                    meta.hasSpirv = true;
                } else if (token == "msl") {
                    meta.hasMsl = true;
                } else if (token == "dxil") {
                    meta.hasDxil = true;
                } else {
                    AERO_LOG_ERROR("shader metadata: unknown formats token '{}'", token);
                    return std::nullopt;
                }
            }
            seen.formats = true;
        } else {
            // Unknown ADDITIVE key: parse-and-discard its value (forward compatibility within schema
            // 1, spec 3.3 step 2) -- peek the first non-ws char to pick the right shape.
            if (pos < json.size() && json[pos] == '"') {
                if (!parseString(json, pos)) {
                    AERO_LOG_ERROR("shader metadata: malformed string value for unknown key '{}'", *key);
                    return std::nullopt;
                }
            } else if (pos < json.size() && json[pos] == '[') {
                if (!parseStringArray(json, pos)) {
                    AERO_LOG_ERROR("shader metadata: malformed array value for unknown key '{}'", *key);
                    return std::nullopt;
                }
            } else if (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
                if (!parseUint(json, pos)) {
                    AERO_LOG_ERROR("shader metadata: malformed numeric value for unknown key '{}'", *key);
                    return std::nullopt;
                }
            } else {
                AERO_LOG_ERROR("shader metadata: unrecognized value for unknown key '{}'", *key);
                return std::nullopt;
            }
        }

        skipWs(json, pos);
        if (pos >= json.size()) {
            AERO_LOG_ERROR("shader metadata: truncated object (missing closing brace)");
            return std::nullopt;
        }
        if (json[pos] == ',') {
            ++pos;
            continue;
        }
        if (json[pos] == '}') {
            ++pos;
            break;
        }
        AERO_LOG_ERROR("shader metadata: expected ',' or a closing brace after a value");
        return std::nullopt;
    }

    skipWs(json, pos);
    if (pos != json.size()) {
        AERO_LOG_ERROR("shader metadata: trailing content after the closing brace");
        return std::nullopt;
    }

    if (!seen.schema) {
        AERO_LOG_ERROR("shader metadata: missing required key 'schema'");
        return std::nullopt;
    }
    if (!seen.name) {
        AERO_LOG_ERROR("shader metadata: missing required key 'name'");
        return std::nullopt;
    }
    if (!seen.stage) {
        AERO_LOG_ERROR("shader metadata: missing required key 'stage'");
        return std::nullopt;
    }
    if (!seen.entryPoint) {
        AERO_LOG_ERROR("shader metadata: missing required key 'entryPoint'");
        return std::nullopt;
    }
    if (!seen.mslEntryPoint) {
        AERO_LOG_ERROR("shader metadata: missing required key 'mslEntryPoint'");
        return std::nullopt;
    }
    if (!seen.samplerCount) {
        AERO_LOG_ERROR("shader metadata: missing required key 'samplerCount'");
        return std::nullopt;
    }
    if (!seen.storageTextureCount) {
        AERO_LOG_ERROR("shader metadata: missing required key 'storageTextureCount'");
        return std::nullopt;
    }
    if (!seen.storageBufferCount) {
        AERO_LOG_ERROR("shader metadata: missing required key 'storageBufferCount'");
        return std::nullopt;
    }
    if (!seen.uniformBufferCount) {
        AERO_LOG_ERROR("shader metadata: missing required key 'uniformBufferCount'");
        return std::nullopt;
    }
    if (!seen.formats) {
        AERO_LOG_ERROR("shader metadata: missing required key 'formats'");
        return std::nullopt;
    }
    if (!meta.hasSpirv && !meta.hasMsl && !meta.hasDxil) {
        AERO_LOG_ERROR("shader metadata: 'formats' must not be empty");
        return std::nullopt;
    }

    return meta;
}

std::optional<ShaderArtifact> selectArtifact(const ShaderMetadata& meta, ShaderFormat format) {
    switch (format) {
        case ShaderFormat::SpirV:
            return meta.hasSpirv ? std::optional{ShaderArtifact{.extension = ".spv", .entryPoint = meta.entryPoint}}
                                 : std::nullopt;
        case ShaderFormat::Dxil:
            return meta.hasDxil ? std::optional{ShaderArtifact{.extension = ".dxil", .entryPoint = meta.entryPoint}}
                                : std::nullopt;
        case ShaderFormat::Msl:
            return meta.hasMsl ? std::optional{ShaderArtifact{.extension = ".msl", .entryPoint = meta.mslEntryPoint}}
                               : std::nullopt;
    }
    return std::nullopt;  // unreachable on the 3-value enum; keeps the compiler happy
}

ShaderHandle loadShader(Device& device, const VirtualFileSystem& vfs, std::string_view basePath) {
    AERO_PROFILE_ZONE_NAMED("rhi::loadShader");
    const ShaderFormat format = device.shaderFormat();  // self-guards a moved-from device (logs + SpirV)

    const std::string jsonPath = std::string{basePath} + ".json";
    const std::optional<std::string> jsonText = vfs.readText(jsonPath);
    if (!jsonText) {
        AERO_LOG_ERROR("loadShader: cannot read '{}'", jsonPath);
        return {};
    }

    const std::optional<ShaderMetadata> meta = parseShaderMetadata(*jsonText);
    if (!meta) {
        AERO_LOG_ERROR("loadShader: '{}' failed to parse", jsonPath);  // the parser already logged the field
        return {};
    }

    const std::optional<ShaderArtifact> artifact = selectArtifact(*meta, format);
    if (!artifact) {
        AERO_LOG_ERROR("loadShader: '{}' has no {} artifact (spirv={} msl={} dxil={})", basePath, formatName(format),
                       meta->hasSpirv, meta->hasMsl, meta->hasDxil);
        return {};
    }

    const std::string codePath = std::string{basePath} + std::string{artifact->extension};
    const std::optional<ByteBuffer> code = vfs.readFile(codePath);
    if (!code) {
        AERO_LOG_ERROR("loadShader: cannot read '{}'", codePath);
        return {};
    }

    const ShaderDesc desc{
        .stage = meta->stage,
        .format = format,
        .bytecode = std::span<const std::byte>{*code},  // borrowed; createShader copies (F3)
        .entryPoint = artifact->entryPoint,             // borrowed view into `meta`; createShader NT-copies (F3)
        .samplerCount = meta->samplerCount,
        .storageTextureCount = meta->storageTextureCount,
        .storageBufferCount = meta->storageBufferCount,
        .uniformBufferCount = meta->uniformBufferCount,
    };
    return device.createShader(desc);  // empty-bytecode / E20 mismatch re-validated here
}

}  // namespace engine::rhi

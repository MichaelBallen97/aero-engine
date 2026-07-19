#pragma once
// Aero Engine — rhi shader loading (task 0.4.4): the runtime CONSUMER of the tools/shaderc artifact
// contract (0.4.3 D4/D5). It reads a cooked shader's `.json` sidecar + the backend-correct bytecode
// (`.spv`/`.dxil`/`.msl`) through the VFS and turns them into an rhi ShaderHandle via Device::createShader.
//
// This header is a CONVENIENCE BRIDGE beside the sacred wrapper (ADR-002), NOT part of it: the umbrella
// <aero/rhi/rhi.hpp> deliberately does NOT include it, so the pure SDL_GPU-free GPU vocabulary stays free
// of VFS/file-I/O coupling. Include this header explicitly when you need to load cooked shaders. It exposes
// only engine + standard-library types — no SDL, no DXC, no JSON library (boundary rule).
//
// ERROR MODEL (matches device.hpp): nothing throws; loadShader returns an INVALID handle on failure, the
// parse/select helpers return std::nullopt, and every failure emits one AERO_LOG_ERROR naming the cause.

#include <aero/rhi/handles.hpp>  // ShaderHandle
#include <aero/rhi/types.hpp>    // ShaderStage, ShaderFormat

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace engine {
class VirtualFileSystem;  // forward-declared: the loader's one core type; .cpp includes <aero/core/vfs.hpp>
}  // namespace engine

namespace engine::rhi {

class Device;  // forward-declared: loadShader takes Device& only; .cpp includes <aero/rhi/device.hpp>

// The engine-owned view of one `<base>.json` sidecar (schema 1). Owns its strings so it outlives the JSON
// buffer it was parsed from. No third-party type appears here.
struct ShaderMetadata {
    ShaderStage stage = ShaderStage::Vertex;  // from "stage": "vertex"|"fragment"
    std::string name;                         // "triangle.vert" — informational (logs)
    std::string entryPoint = "main";          // used for SpirV/Dxil
    std::string mslEntryPoint = "main0";      // used for Msl (Metal's cleansed name, F5)
    std::uint32_t samplerCount = 0;
    std::uint32_t storageTextureCount = 0;
    std::uint32_t storageBufferCount = 0;
    std::uint32_t uniformBufferCount = 0;
    bool hasSpirv = false;  // derived from the "formats" array
    bool hasMsl = false;
    bool hasDxil = false;
    std::string toolchain;  // "sdl-shadercross@…" — informational only, NEVER validated (D6)
};

// Which sidecar file extension + entry-point name a given device format needs from `meta`. The returned
// views are valid WHILE `meta` LIVES: `extension` is a static literal; `entryPoint` aliases meta's strings.
struct ShaderArtifact {
    std::string_view extension;   // ".spv" | ".dxil" | ".msl"
    std::string_view entryPoint;  // "main" | "main0"
};

// Parse one schema-1 `.json` sidecar. Pure — no GPU, no VFS, no I/O. Returns std::nullopt (+ one
// AERO_LOG_ERROR naming the offending field) on: not an object, missing/duplicate/mistyped required key,
// unknown "stage", empty/unknown-token "formats", a count that is negative/fractional/overflows u32, a
// malformed string, trailing non-whitespace, or schema != 1. Unknown ADDITIVE keys are ignored (forward
// compatibility within schema 1). This is the tier-0, every-lane building block.
[[nodiscard]] std::optional<ShaderMetadata> parseShaderMetadata(std::string_view json);

// Pick the sidecar + entry point for `format` from `meta`. Returns std::nullopt (no log — the caller
// decides whether absence is an error) when `meta` carries no artifact in `format`. Pure — no GPU.
[[nodiscard]] std::optional<ShaderArtifact> selectArtifact(const ShaderMetadata& meta, ShaderFormat format);

// Load one compiled shader stage and create its rhi shader. `basePath` is an EXTENSION-LESS res:// path,
// e.g. "res://shaders/triangle.vert"; the loader appends ".json" then the format's sidecar extension. It
// reads <base>.json (VFS), parses it, selects the artifact for Device::shaderFormat(), reads the sidecar
// bytes (VFS), fills a ShaderDesc (counts ONLY from the JSON, D5), and calls Device::createShader. Returns
// an INVALID ShaderHandle (+ AERO_LOG_ERROR) on any failure. The stage baked into the shader is the JSON's,
// not the caller's — a wrong-stage load surfaces later at createGraphicsPipeline.
[[nodiscard]] ShaderHandle loadShader(Device& device, const VirtualFileSystem& vfs, std::string_view basePath);

}  // namespace engine::rhi

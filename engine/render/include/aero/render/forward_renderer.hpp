#pragma once
// Aero Engine — render::ForwardRenderer (task 1.4.1): the scene-free draw engine. Encapsulates exactly
// the hand-rolled sequence samples/phase-0-cube/main.cpp records by hand (load shaders, build a
// pipeline, upload the primitive catalog, push uniforms + drawIndexed per instance) behind a small,
// data-driven API: draw(Frame&, const RenderView&). No scene type, no SDL type, no vcpkg type appears
// in this header (rule #3) — RenderView/MeshInstance/etc. are all rhi/core/math aggregates (D2).
//
// LIFETIME CONTRACTS: the rhi::Device passed to create() MUST outlive the ForwardRenderer. draw() must
// be called with an OPEN Frame (between Renderer::beginFrame and endFrame), matching the pipeline's
// colorFormat/depthFormat exactly (D11 — a non-depth Renderer's depthFormat() == Invalid, which
// create() rejects).
//
// ERROR MODEL (docs/04, mirrors rhi/render): nothing throws. create() returns nullopt (+ ERROR) on
// failure, destroying any GPU resource it already created first (no ~Device leak WARN). draw() is a
// void, best-effort recording call — it no-ops when !view.hasCamera (D5's 0-camera case).

#include <aero/render/lighting.hpp>
#include <aero/render/mesh.hpp>
#include <aero/render/renderer.hpp>  // Frame
#include <aero/rhi/format.hpp>       // rhi::TextureFormat

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace engine::rhi {
class Device;  // forward-declared: render's one heavy rhi type; forward_renderer.cpp includes device.hpp
}  // namespace engine::rhi

namespace engine {
class VirtualFileSystem;  // forward-declared: create() takes it by ref; .cpp includes vfs.hpp
}  // namespace engine

namespace engine::render {

// How to create a ForwardRenderer. colorFormat/depthFormat must match the Renderer this draws into
// (renderer.colorFormat()/renderer.depthFormat()); depthFormat == Invalid FAILS create() (D11 — the
// forward pipeline always depth-tests). Shader paths are res:// (extension-less) VFS paths, resolved
// through the caller-supplied VirtualFileSystem (the samples/phase-0-cube pattern).
struct ForwardRendererConfig {
    rhi::TextureFormat colorFormat = rhi::TextureFormat::Invalid;  // required (renderer.colorFormat())
    rhi::TextureFormat depthFormat = rhi::TextureFormat::Invalid;  // required, != Invalid (renderer.depthFormat())
    std::string_view vertexShaderPath = "res://scene.vert";
    std::string_view fragmentShaderPath = "res://scene.frag";
};

// Move-only RAII; owns the lit pipeline and a procedural primitive-mesh catalog (cube/sphere/plane).
// No third-party type appears here — every member is an rhi handle or a plain engine aggregate, so
// there is no pimpl (matching render::Renderer's own precedent).
class ForwardRenderer {
public:
    [[nodiscard]] static std::optional<ForwardRenderer> create(rhi::Device& device, const VirtualFileSystem& shaderVfs,
                                                               const ForwardRendererConfig& config);

    ~ForwardRenderer();  // destroys the pipeline + every primitive's buffers (no-op if moved-from)
    ForwardRenderer(ForwardRenderer&&) noexcept;             // USER-DEFINED: transfers + nulls the source
    ForwardRenderer& operator=(ForwardRenderer&&) noexcept;  // (a defaulted move would double-free the GPU handles)
    ForwardRenderer(const ForwardRenderer&) = delete;
    ForwardRenderer& operator=(const ForwardRenderer&) = delete;

    // Records the whole RenderView into `frame`'s open pass: binds the pipeline, pushes the fragment
    // light block once, then per instance pushes the vertex per-object block and issues one
    // drawIndexed against that instance's primitive mesh. No-ops (records nothing) when
    // !view.hasCamera (D5) — the frame's own clear still shows.
    void draw(Frame& frame, const RenderView& view);

private:
    struct PrimitiveMesh {
        rhi::BufferHandle vbuf;
        rhi::BufferHandle ibuf;
        std::uint32_t indexCount = 0;
    };

    ForwardRenderer(rhi::Device* device, rhi::GraphicsPipelineHandle pipeline,
                    std::array<PrimitiveMesh, static_cast<std::size_t>(PrimitiveId::Count)> primitives) noexcept;
    void destroyAll() noexcept;  // dtor + move-assign share this; no-op when device == nullptr

    rhi::Device* device = nullptr;  // non-owning; outlives the ForwardRenderer (contract)
    rhi::GraphicsPipelineHandle pipeline{};
    std::array<PrimitiveMesh, static_cast<std::size_t>(PrimitiveId::Count)> primitives{};
};

}  // namespace engine::render

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
//
// MATERIALS (task 3.4.1). The renderer owns the material registry: createMaterial/updateMaterial/
// destroyMaterial over a core SlotMap, so a MaterialHandle is generational and a stale one is a
// logged no-op rather than a dangling read (ADR-001). Beside it live the two pipelines (identical
// but for cullMode, for doubleSided), the three built-in 1x1 default textures (material.hpp's
// defaultTextureTexel is their single definition) and the premade default material every invalid
// MeshInstance::material resolves to. Ownership splits three ways and material.hpp states it in
// full: textures are BORROWED from the caller, samplers and the defaults are renderer-owned.

#include <aero/core/slot_map.hpp>
#include <aero/render/lighting.hpp>
#include <aero/render/material.hpp>
#include <aero/render/mesh.hpp>
#include <aero/render/renderer.hpp>  // Frame
#include <aero/rhi/descriptors.hpp>  // rhi::SamplerDesc
#include <aero/rhi/format.hpp>       // rhi::TextureFormat

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

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

    // --- materials (task 3.4.1) -----------------------------------------------------------------
    // Registry semantics: generational handles, so a stale one is a logged no-op (update returns
    // false; destroy warns) and a stale MeshInstance::material resolves to the default at draw time.
    // create NEVER fails for valid params — a material owns no GPU resource of its own beyond the
    // sampler handles the dedup cache may mint, and those are renderer-lifetime.
    [[nodiscard]] MaterialHandle createMaterial(const MaterialParams& params, const MaterialTextureSlots& slots);
    // 3.4.2's live-preview seam, built now because it is ten lines and retrofitting a mutation path
    // later means touching the draw loop a second time. The next draw reads the new values.
    bool updateMaterial(MaterialHandle material, const MaterialParams& params, const MaterialTextureSlots& slots);
    // Never touches a slot's rhi::TextureHandle — those are the caller's (material.hpp). Destroying
    // the built-in default material is a LOGGED NO-OP: it is what every invalid handle falls back to.
    void destroyMaterial(MaterialHandle material);
    // The premade fallback: white dielectric, metallic 0, roughness 1, emissive black — deliberately
    // NOT glTF's metallic-1 default (material.hpp's DEFAULT_MATERIAL_PARAMS carries the reason).
    [[nodiscard]] MaterialHandle defaultMaterial() const noexcept;

    // --- diagnostics (task 3.4.1) ---------------------------------------------------------------
    // Two small observability accessors, documented as such rather than smuggled in: the sampler
    // cache's size and the Blend-drawn-opaque latch are otherwise unobservable without a log sink,
    // which this project deliberately does not have (the 0.2.4 deferral). They report; they never
    // change behaviour.
    [[nodiscard]] std::size_t samplerCacheSize() const noexcept;
    [[nodiscard]] bool hasWarnedBlendOpaque() const noexcept;

private:
    struct PrimitiveMesh {
        rhi::BufferHandle vbuf;
        rhi::BufferHandle ibuf;
        std::uint32_t indexCount = 0;
    };

    // One registered material: the caller's params and slots VERBATIM — invalid texture handles and
    // all, because a slot resolves to its built-in default at BIND time and not at create time, so an
    // updateMaterial that adds or removes a texture needs no default bookkeeping — plus the five
    // dedup'd, renderer-owned sampler handles resolved from those slots' descs.
    struct MaterialSlot {
        MaterialParams params;
        MaterialTextureSlots slots;
        std::array<rhi::SamplerHandle, MATERIAL_TEXTURE_SLOT_COUNT> samplers{};
    };

    ForwardRenderer(rhi::Device* device, rhi::GraphicsPipelineHandle pipeline,
                    rhi::GraphicsPipelineHandle pipelineCullNone) noexcept;
    void destroyAll() noexcept;  // dtor + move-assign share this; no-op when device == nullptr
    void reset() noexcept;       // null every member WITHOUT releasing anything (the moved-from state)
    // Linear scan over samplerCache, creating and appending on a miss. The cache is
    // renderer-lifetime and GROWS ONLY: it is bounded by the number of DISTINCT sampler descs a
    // program actually uses, which is tiny by construction (the format's token vocabulary is
    // 3x3x2x2x3), so an updateMaterial churning descs cannot make it unbounded in any real sense.
    [[nodiscard]] rhi::SamplerHandle resolveSampler(const rhi::SamplerDesc& desc);
    // The three 1x1 identity textures plus the default SamplerDesc's cache entry. False (+ ERROR) if
    // any create or upload fails; the caller then abandons the whole renderer, whose destructor
    // releases everything already made.
    [[nodiscard]] bool createDefaults();
    // One bindFragmentSamplers call for all five slots (task 3.4.1), resolving every invalid texture
    // handle to its built-in default at BIND time. Called on material change only, from draw().
    void bindMaterialTextures(rhi::RenderPassHandle pass, const MaterialSlot& slot);

    rhi::Device* device = nullptr;                   // non-owning; outlives the ForwardRenderer (contract)
    rhi::GraphicsPipelineHandle pipeline{};          // CullMode::Back — the engine convention
    rhi::GraphicsPipelineHandle pipelineCullNone{};  // the doubleSided twin, same two shaders
    std::array<PrimitiveMesh, static_cast<std::size_t>(PrimitiveId::Count)> primitives{};
    // The three built-in 1x1 defaults, INDEXED BY MaterialDefaultTextureKind — never by slot, and
    // never as three separately named members. Five slots map onto these three through
    // material.hpp's defaultTextureKindForSlot, which is the single place that mapping is decided;
    // the alternative (three names plus a five-entry table of them inside the .cpp) put the same
    // decision in two places, where a swapped pair renders a loud visual defect that no automated
    // case in this tree can see.
    std::array<rhi::TextureHandle, MATERIAL_DEFAULT_TEXTURE_KIND_COUNT> defaultTextures{};
    SlotMap<MaterialSlot, Material> materials;
    std::vector<std::pair<rhi::SamplerDesc, rhi::SamplerHandle>> samplerCache;  // linear scan; tiny
    MaterialHandle defaultMaterialHandle{};
    bool warnedBlendOnce = false;  // D9's latch: once per renderer lifetime, never per frame
};

}  // namespace engine::render

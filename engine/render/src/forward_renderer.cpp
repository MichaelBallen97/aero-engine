// engine/render/src/forward_renderer.cpp — task 1.4.1: the scene-free draw engine. create() mirrors
// samples/phase-0-cube/main.cpp's hand-rolled sequence (load shaders -> build a pipeline -> upload a
// mesh catalog) for the lit-primitive layout; draw() records one bound pipeline, one fragment light
// push, and per-instance vertex pushes + drawIndexed calls. Nothing here logs on the happy path;
// every failure path logs exactly one AERO_LOG_ERROR and returns nullopt/no-ops (docs/04).

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/vfs.hpp>
#include <aero/render/forward_renderer.hpp>
#include <aero/rhi/device.hpp>
#include <aero/rhi/shader_loader.hpp>

#include "material_pack.hpp"
#include "primitives.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>

namespace engine::render {

namespace {

// CPU mirrors of the HLSL cbuffers (shaders/scene.{vert,frag}.hlsl) — field order MUST match the
// HLSL exactly (D8); the sizeof static_asserts are the tripwire against a silent packing drift.
struct GpuPerObject {
    Mat4 mvp;
    Mat4 model;
    Mat4 normalMatrix;
    Vec3 baseColor;
    float pad0 = 0.0F;
};
static_assert(sizeof(GpuPerObject) == 208);
static_assert(std::is_trivially_copyable_v<GpuPerObject>);

struct GpuDirLight {
    Vec3 direction;
    float intensity = 0.0F;
    Vec3 color;
    float pad0 = 0.0F;
};
static_assert(sizeof(GpuDirLight) == 32);

struct GpuPointLight {
    Vec3 position;
    float range = 0.0F;
    Vec3 color;
    float intensity = 0.0F;
};
static_assert(sizeof(GpuPointLight) == 32);

struct GpuLightBlock {
    Vec3 ambient;
    std::uint32_t pointCount = 0;
    GpuDirLight dir;
    std::array<GpuPointLight, MAX_POINT_LIGHTS> points{};
    // task 3.4.1 — the one field the GGX BRDF needs that Lambert did not. CameraView has carried
    // eyePosition unused since 1.4.1, whose own comment says it exists "for future specular/fresnel
    // terms" (lighting.hpp). Appended AFTER the existing members, so every pre-3.4.1 field keeps its
    // offset and the growth is invisible to anything that does not read the tail.
    Vec3 eyePosition;
    float pad0 = 0.0F;
};
static_assert(sizeof(GpuLightBlock) == 16 + 32 + (32 * 8) + 16);  // 320 (was 304)
static_assert(std::is_trivially_copyable_v<GpuLightBlock>);

// >= PrimitiveId::Count (an out-of-range MeshInstance::primitive, defensive — the bridge already
// clamps) -> Cube.
[[nodiscard]] std::size_t clampPrimitiveIndex(PrimitiveId primitive) {
    const auto index = static_cast<std::size_t>(primitive);
    return index < static_cast<std::size_t>(PrimitiveId::Count) ? index : static_cast<std::size_t>(PrimitiveId::Cube);
}

// Full-field SamplerDesc equality — rhi::SamplerDesc has no operator== (it is a plain descriptor
// aggregate), and adding one there would be an rhi public-header change this task does not need.
// AC-25 only requires wrap U/V, min/mag, mip mode and the maxLod-0 idiom in the dedup key; comparing
// EVERY field is strictly safer, costs nothing at these sizes, and cannot silently miss a field a
// future desc gains — a new member is a compile error here rather than a wrongly-shared sampler.
[[nodiscard]] bool samplerDescEquals(const rhi::SamplerDesc& a, const rhi::SamplerDesc& b) noexcept {
    return a.minFilter == b.minFilter && a.magFilter == b.magFilter && a.mipmapMode == b.mipmapMode &&
           a.addressU == b.addressU && a.addressV == b.addressV && a.addressW == b.addressW &&
           a.mipLodBias == b.mipLodBias && a.minLod == b.minLod && a.maxLod == b.maxLod &&
           a.enableAnisotropy == b.enableAnisotropy && a.maxAnisotropy == b.maxAnisotropy &&
           a.enableCompare == b.enableCompare && a.compareOp == b.compareOp;
}

[[nodiscard]] GpuLightBlock pack(const RenderView& view) {
    GpuLightBlock block{};
    block.ambient = view.ambient;
    block.dir = {view.directional.direction, view.directional.intensity, view.directional.color, 0.0F};
    const std::size_t count = std::min<std::size_t>(view.points.size(), MAX_POINT_LIGHTS);
    for (std::size_t i = 0; i < count; ++i) {
        const PointLightData& src = view.points[i];
        block.points[i] = {src.position, src.range, src.color, src.intensity};
    }
    block.pointCount = static_cast<std::uint32_t>(count);
    block.eyePosition = view.camera.eyePosition;  // task 3.4.1 — the BRDF's view vector origin
    return block;
}

}  // namespace

ForwardRenderer::ForwardRenderer(rhi::Device* deviceIn, rhi::GraphicsPipelineHandle pipelineIn,
                                 rhi::GraphicsPipelineHandle pipelineCullNoneIn) noexcept
    : device(deviceIn), pipeline(pipelineIn), pipelineCullNone(pipelineCullNoneIn) {}

ForwardRenderer::ForwardRenderer(ForwardRenderer&& other) noexcept
    : device(other.device),
      pipeline(other.pipeline),
      pipelineCullNone(other.pipelineCullNone),
      primitives(other.primitives),
      defaultWhiteSrgb(other.defaultWhiteSrgb),
      defaultWhiteLinear(other.defaultWhiteLinear),
      defaultFlatNormal(other.defaultFlatNormal),
      materials(std::move(other.materials)),
      samplerCache(std::move(other.samplerCache)),
      defaultMaterialHandle(other.defaultMaterialHandle),
      warnedBlendOnce(other.warnedBlendOnce) {
    other.reset();
}

ForwardRenderer& ForwardRenderer::operator=(ForwardRenderer&& other) noexcept {
    if (this != &other) {
        destroyAll();
        device = other.device;
        pipeline = other.pipeline;
        pipelineCullNone = other.pipelineCullNone;
        primitives = other.primitives;
        defaultWhiteSrgb = other.defaultWhiteSrgb;
        defaultWhiteLinear = other.defaultWhiteLinear;
        defaultFlatNormal = other.defaultFlatNormal;
        materials = std::move(other.materials);
        samplerCache = std::move(other.samplerCache);
        defaultMaterialHandle = other.defaultMaterialHandle;
        warnedBlendOnce = other.warnedBlendOnce;
        other.reset();
    }
    return *this;
}

ForwardRenderer::~ForwardRenderer() { destroyAll(); }

void ForwardRenderer::reset() noexcept {
    device = nullptr;
    pipeline = {};
    pipelineCullNone = {};
    primitives = {};
    defaultWhiteSrgb = {};
    defaultWhiteLinear = {};
    defaultFlatNormal = {};
    materials.clear();  // a moved-from SlotMap keeps its scalar bookkeeping; clear() zeroes it
    samplerCache.clear();
    defaultMaterialHandle = {};
    warnedBlendOnce = false;
}

void ForwardRenderer::destroyAll() noexcept {
    if (device == nullptr) {
        return;
    }
    if (pipeline.valid()) {
        device->destroyGraphicsPipeline(pipeline);
    }
    if (pipelineCullNone.valid()) {
        device->destroyGraphicsPipeline(pipelineCullNone);
    }
    for (const PrimitiveMesh& mesh : primitives) {
        if (mesh.vbuf.valid()) {
            device->destroyBuffer(mesh.vbuf);
        }
        if (mesh.ibuf.valid()) {
            device->destroyBuffer(mesh.ibuf);
        }
    }
    // Renderer-owned, unlike the materials' textures: a material BORROWS its textures from the
    // caller, so nothing here walks the registry looking for rhi::TextureHandles to release.
    for (const rhi::TextureHandle texture : {defaultWhiteSrgb, defaultWhiteLinear, defaultFlatNormal}) {
        if (texture.valid()) {
            device->destroyTexture(texture);
        }
    }
    for (const auto& entry : samplerCache) {
        if (entry.second.valid()) {
            device->destroySampler(entry.second);
        }
    }
    reset();
}

std::optional<ForwardRenderer> ForwardRenderer::create(rhi::Device& device, const VirtualFileSystem& shaderVfs,
                                                       const ForwardRendererConfig& config) {
    AERO_PROFILE_ZONE;
    if (config.colorFormat == rhi::TextureFormat::Invalid) {
        AERO_LOG_ERROR("ForwardRenderer::create: colorFormat is Invalid");
        return std::nullopt;
    }
    if (config.depthFormat == rhi::TextureFormat::Invalid) {
        // D11: a non-depth Renderer's depthFormat() is Invalid — the forward pipeline always
        // depth-tests, so this is where that mismatch is caught.
        AERO_LOG_ERROR("ForwardRenderer::create: depthFormat is Invalid (Renderer must be created with .depth=true)");
        return std::nullopt;
    }

    const rhi::ShaderHandle vs = rhi::loadShader(device, shaderVfs, config.vertexShaderPath);
    const rhi::ShaderHandle fs = rhi::loadShader(device, shaderVfs, config.fragmentShaderPath);
    if (!vs.valid() || !fs.valid()) {
        AERO_LOG_ERROR("ForwardRenderer::create: scene shader load failed");
        if (vs.valid()) {
            device.destroyShader(vs);
        }
        if (fs.valid()) {
            device.destroyShader(fs);
        }
        return std::nullopt;
    }

    // Four attributes over the 48-byte MeshVertex (task 3.4.1). The layout may describe attributes
    // the CURRENT shader does not consume — legal on all three backends — which is why the vertex
    // layout grows one commit BEFORE the PBR shader rewrite that reads locations 2 and 3. The reverse
    // order is invalid: a shader consuming an undescribed attribute is a pipeline-creation failure.
    const rhi::VertexBufferLayout vbLayout{.slot = 0, .pitch = sizeof(MeshVertex)};
    const std::array<rhi::VertexAttribute, 4> attrs{{
        {.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offset = 0},
        {.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offset = 12},
        {.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offset = 24},
        {.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offset = 40},
    }};
    const rhi::ColorTargetDesc colorTarget{.format = config.colorFormat};
    const rhi::GraphicsPipelineDesc pipelineDesc{
        .vertexShader = vs,
        .fragmentShader = fs,
        .vertexBuffers = std::span{&vbLayout, 1},
        .vertexAttributes = attrs,
        .depthStencil = {.enableDepthTest = true, .enableDepthWrite = true, .compareOp = rhi::CompareOp::Less},
        .colorTargets = std::span{&colorTarget, 1},
        .depthStencilFormat = config.depthFormat,
    };
    const rhi::GraphicsPipelineHandle pipeline = device.createGraphicsPipeline(pipelineDesc);
    // The doubleSided twin (task 3.4.1): identical in every respect but cullMode, built from the SAME
    // two shader handles so there is no second shader load and no way for the pair to drift.
    rhi::GraphicsPipelineDesc cullNoneDesc = pipelineDesc;
    cullNoneDesc.rasterizer.cullMode = rhi::CullMode::None;
    const rhi::GraphicsPipelineHandle pipelineCullNone = device.createGraphicsPipeline(cullNoneDesc);
    device.destroyShader(vs);  // safe after pipeline creation (device.hpp)
    device.destroyShader(fs);
    if (!pipeline.valid() || !pipelineCullNone.valid()) {
        AERO_LOG_ERROR("ForwardRenderer::create: pipeline creation failed");
        if (pipeline.valid()) {
            device.destroyGraphicsPipeline(pipeline);
        }
        if (pipelineCullNone.valid()) {
            device.destroyGraphicsPipeline(pipelineCullNone);
        }
        return std::nullopt;
    }

    // From here on the renderer OWNS everything created, and its destructor IS the failure path —
    // which is why the old destroyPartial bookkeeping is gone: every early return below releases the
    // pipelines, the buffers uploaded so far, the default textures and the samplers, with no list to
    // keep in sync as that set grows.
    ForwardRenderer renderer{&device, pipeline, pipelineCullNone};

    for (std::size_t i = 0; i < renderer.primitives.size(); ++i) {
        detail::PrimitiveGeometry geometry;
        switch (static_cast<PrimitiveId>(i)) {
            case PrimitiveId::Cube:
                geometry = detail::makeCube();
                break;
            case PrimitiveId::Sphere:
                geometry = detail::makeSphere();
                break;
            case PrimitiveId::Plane:
                geometry = detail::makePlane();
                break;
            case PrimitiveId::Count:
                break;  // unreachable — i < primitives.size() == Count
        }

        const auto vbytes = static_cast<std::uint32_t>(geometry.vertices.size() * sizeof(MeshVertex));
        const auto ibytes = static_cast<std::uint32_t>(geometry.indices.size() * sizeof(std::uint16_t));
        const rhi::BufferHandle vbuf = device.createBuffer({.usage = rhi::BufferUsage::Vertex, .size = vbytes});
        const rhi::BufferHandle ibuf = device.createBuffer({.usage = rhi::BufferUsage::Index, .size = ibytes});
        // Recorded BEFORE the success check, so a failed upload still leaves both buffers owned by
        // `renderer` and released by its destructor on the early return below.
        renderer.primitives[i] = {vbuf, ibuf, static_cast<std::uint32_t>(geometry.indices.size())};
        const bool uploadedVertices =
            vbuf.valid() && device.uploadBuffer(vbuf, 0, std::as_bytes(std::span{geometry.vertices}));
        const bool uploadedIndices =
            ibuf.valid() && device.uploadBuffer(ibuf, 0, std::as_bytes(std::span{geometry.indices}));
        if (!uploadedVertices || !uploadedIndices) {
            AERO_LOG_ERROR("ForwardRenderer::create: primitive {} upload failed", i);
            return std::nullopt;
        }
    }

    if (!renderer.createDefaults()) {
        return std::nullopt;
    }
    // Never fails: a material owns no GPU resource of its own, and createDefaults has already proven
    // the default SamplerDesc resolves, which is the only device call this can make.
    renderer.defaultMaterialHandle = renderer.createMaterial(DEFAULT_MATERIAL_PARAMS, {});

    return renderer;
}

bool ForwardRenderer::createDefaults() {
    // The three 1x1 identity textures (D7). material.hpp's defaultTextureTexel is the SINGLE
    // definition of their bytes and formats — restating them here would be the second place for a
    // texel typo to hide, and the tier-0 case that pins them would then prove nothing about what the
    // GPU actually receives.
    const std::array<rhi::TextureHandle*, 3> targets{&defaultWhiteSrgb, &defaultWhiteLinear, &defaultFlatNormal};
    const std::array<std::size_t, 3> slotForTarget{0, 1, 2};  // baseColor (sRGB), metallicRoughness, normal
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const MaterialDefaultTexture entry = defaultTextureTexel(slotForTarget[i]);
        const rhi::TextureHandle texture = device->createTexture(
            {.format = entry.format, .usage = rhi::TextureUsage::Sampler, .width = 1, .height = 1});
        *targets[i] = texture;
        if (!texture.valid() || !device->uploadTexture(texture, 0, std::as_bytes(std::span{entry.texel}))) {
            AERO_LOG_ERROR("ForwardRenderer::create: built-in default texture {} could not be created", i);
            return false;
        }
    }
    // Pre-resolve the default SamplerDesc so the default material (and every slot a caller leaves
    // unset) shares one handle, and so a device that cannot create a sampler at all fails HERE,
    // where there is still something to say about it.
    if (!resolveSampler(rhi::SamplerDesc{}).valid()) {
        AERO_LOG_ERROR("ForwardRenderer::create: the default sampler could not be created");
        return false;
    }
    return true;
}

rhi::SamplerHandle ForwardRenderer::resolveSampler(const rhi::SamplerDesc& desc) {
    for (const auto& entry : samplerCache) {
        if (samplerDescEquals(entry.first, desc)) {
            return entry.second;
        }
    }
    const rhi::SamplerHandle sampler = device->createSampler(desc);
    if (sampler.valid()) {
        samplerCache.emplace_back(desc, sampler);
    }
    return sampler;
}

MaterialHandle ForwardRenderer::createMaterial(const MaterialParams& params, const MaterialTextureSlots& slots) {
    AERO_PROFILE_ZONE;
    MaterialSlot slot;
    slot.params = params;
    slot.slots = slots;
    for (std::size_t i = 0; i < MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        slot.samplers[i] = resolveSampler(materialSlotAt(slots, i).sampler);
    }
    // No std::move: MaterialSlot is trivially copyable (handles, floats and enums throughout), so a
    // move would be a copy with extra ceremony — and clang-tidy says so.
    return materials.insert(slot);
}

bool ForwardRenderer::updateMaterial(MaterialHandle material, const MaterialParams& params,
                                     const MaterialTextureSlots& slots) {
    AERO_PROFILE_ZONE;
    if (!materials.contains(material)) {
        AERO_LOG_WARN("ForwardRenderer::updateMaterial: stale or invalid MaterialHandle — no-op");
        return false;
    }
    // Resolved BEFORE the registry is touched: resolveSampler may append to samplerCache, and doing
    // the work first keeps the material's five handles from being half-written if it ever cannot.
    std::array<rhi::SamplerHandle, MATERIAL_TEXTURE_SLOT_COUNT> samplers{};
    for (std::size_t i = 0; i < MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        samplers[i] = resolveSampler(materialSlotAt(slots, i).sampler);
    }
    MaterialSlot* const slot = materials.get(material);
    slot->params = params;
    slot->slots = slots;
    slot->samplers = samplers;
    return true;
}

void ForwardRenderer::destroyMaterial(MaterialHandle material) {
    if (material == defaultMaterialHandle && material.valid()) {
        // Destroying the fallback would leave every invalid MeshInstance::material with nothing to
        // resolve to, so this is a logged no-op rather than a refusal the caller must handle.
        AERO_LOG_WARN("ForwardRenderer::destroyMaterial: the built-in default material is not destroyable — no-op");
        return;
    }
    if (!materials.remove(material)) {
        AERO_LOG_WARN("ForwardRenderer::destroyMaterial: stale or invalid MaterialHandle — no-op");
    }
}

MaterialHandle ForwardRenderer::defaultMaterial() const noexcept { return defaultMaterialHandle; }

std::size_t ForwardRenderer::samplerCacheSize() const noexcept { return samplerCache.size(); }

bool ForwardRenderer::hasWarnedBlendOpaque() const noexcept { return warnedBlendOnce; }

void ForwardRenderer::bindMaterialTextures(rhi::RenderPassHandle pass, const MaterialSlot& slot) {
    // A-4: resolution to the built-in defaults happens at BIND time, and "which default belongs to
    // slot k" is spelled HERE and nowhere else. The three physical textures cover five slots because
    // occlusion shares metallicRoughness' white-linear texel and emissive shares baseColor's
    // white-sRGB one — exactly the aliasing material.hpp's defaultTextureTexel already encodes.
    const std::array<rhi::TextureHandle, MATERIAL_TEXTURE_SLOT_COUNT> defaults{
        defaultWhiteSrgb, defaultWhiteLinear, defaultFlatNormal, defaultWhiteLinear, defaultWhiteSrgb};
    std::array<rhi::TextureSamplerBinding, MATERIAL_TEXTURE_SLOT_COUNT> bindings{};
    for (std::size_t i = 0; i < MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        const MaterialTextureSlot& source = materialSlotAt(slot.slots, i);
        bindings[i] = {source.texture.valid() ? source.texture : defaults[i], slot.samplers[i]};
    }
    // ONE call for all five (AC-26): slot order is declaration order, the shaderc contract, so t0..t4
    // and s0..s4 land in D7's binding order by construction.
    device->bindFragmentSamplers(pass, 0, bindings);
}

void ForwardRenderer::draw(Frame& frame, const RenderView& view) {
    AERO_PROFILE_ZONE;
    if (!view.hasCamera) {
        return;  // D5 0-camera: the frame's own clear still shows
    }
    const rhi::RenderPassHandle pass = frame.pass();
    const rhi::CommandBufferHandle cmd = frame.commandBuffer();

    device->bindGraphicsPipeline(pass, pipeline);  // cull-back is the reset state
    bool boundCullNone = false;

    const GpuLightBlock lights = pack(view);
    device->pushFragmentUniforms(cmd, 0, std::as_bytes(std::span{&lights, 1}));

    // The state cache is three variables, and the loop is correct under ANY instance order because
    // every draw's state is a pure function of its RESOLVED material — cheap under the common order
    // (materials arrive grouped) and never wrong under an adversarial one. No sorting happens here;
    // 3.6.1/Phase 8 own draw ordering.
    MaterialHandle lastMaterial{};
    bool firstMaterial = true;

    for (const MeshInstance& instance : view.instances) {
        const MaterialHandle resolved =
            materials.contains(instance.material) ? instance.material : defaultMaterialHandle;
        if (firstMaterial || resolved != lastMaterial) {
            // Never null: `resolved` is either a handle contains() just proved live, or the built-in
            // default, which destroyMaterial refuses to remove. The comment is the argument; a dead
            // runtime arm here would be untestable by construction (A-6's posture).
            const MaterialSlot& slot = *materials.get(resolved);
            if (slot.params.alpha == MaterialAlpha::Blend && !warnedBlendOnce) {
                AERO_LOG_WARN(
                    "ForwardRenderer: Blend material drawn OPAQUE (transparency has no owner yet); "
                    "this warning latches once per renderer");
                warnedBlendOnce = true;
            }
            const detail::GpuMaterialParams gpuParams = detail::packMaterial(slot.params);
            device->pushFragmentUniforms(cmd, 1, std::as_bytes(std::span{&gpuParams, 1}));
            bindMaterialTextures(pass, slot);
            // The pipeline rebinds ONLY when doubleSided flips. Pushed uniforms are per-COMMAND
            // BUFFER, not per-pipeline (device.hpp's push-uniform contract), so slots 0 and 1 survive
            // the rebind and do not have to be re-pushed after it.
            const bool wantCullNone = slot.params.doubleSided;
            if (wantCullNone != boundCullNone) {
                device->bindGraphicsPipeline(pass, wantCullNone ? pipelineCullNone : pipeline);
                boundCullNone = wantCullNone;
            }
            lastMaterial = resolved;
            firstMaterial = false;
        }

        const GpuPerObject perObject{instance.mvp, instance.model, instance.normalMatrix, instance.color, 0.0F};
        device->pushVertexUniforms(cmd, 0, std::as_bytes(std::span{&perObject, 1}));

        const PrimitiveMesh& mesh = primitives[clampPrimitiveIndex(instance.primitive)];
        device->bindVertexBuffer(pass, 0, mesh.vbuf);
        device->bindIndexBuffer(pass, mesh.ibuf, rhi::IndexType::Uint16);
        device->drawIndexed(pass, mesh.indexCount);
    }
}

}  // namespace engine::render

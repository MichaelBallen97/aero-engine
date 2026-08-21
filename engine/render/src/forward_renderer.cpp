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
#include "mesh_pack.hpp"
#include "primitives.hpp"
#include "skinning_pack.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace engine::render {

namespace {

// The VERTEX stage's CPU mirror (shaders/scene.vert.hlsl) — field order MUST match the HLSL exactly
// (D8); the sizeof static_assert is the tripwire against a silent packing drift. The two FRAGMENT
// blocks (Lights b0, MaterialParams b1) live in material_pack.hpp instead, beside the functions that
// fill them, because a packer nobody outside this file can call is a packer no test can falsify.
struct GpuPerObject {
    Mat4 mvp;
    Mat4 model;
    Mat4 normalMatrix;
    Vec3 baseColor;
    float pad0 = 0.0F;
};
static_assert(sizeof(GpuPerObject) == 208);
static_assert(std::is_trivially_copyable_v<GpuPerObject>);

// >= PrimitiveId::Count (an out-of-range MeshInstance::primitive, defensive — the bridge already
// clamps) -> Cube.
[[nodiscard]] std::size_t clampPrimitiveIndex(PrimitiveId primitive) {
    const auto index = static_cast<std::size_t>(primitive);
    return index < static_cast<std::size_t>(PrimitiveId::Count) ? index : static_cast<std::size_t>(PrimitiveId::Cube);
}

// task 3.6.1 -- the local box of a built-in primitive, FOLDED over the vertices
// make{Cube,Sphere,Plane}() actually returned. There is deliberately no constant table to compare
// this against: primitives.cpp IS the single source for what each shape is, so a second copy of 0.5
// anywhere in this file would be a second truth that can drift out of step with it silently.
//
// std::min/std::max take the ACCUMULATOR FIRST (the expandBox rule in engine/assets/src/
// mesh_cook.cpp): the two argument orders return different zeros for the pair (-0.0f, +0.0f), and
// this box is compared for equality. A geometry with no vertices would leave the inverted sentinel,
// which Aabb::valid() rejects and draw() then culls -- unreachable here, since all three primitives
// are non-empty by construction, and the safe answer if that ever changes.
[[nodiscard]] Aabb foldPrimitiveBounds(std::span<const MeshVertex> vertices) {
    constexpr float INFINITY_F = std::numeric_limits<float>::infinity();
    Aabb box{Vec3{INFINITY_F, INFINITY_F, INFINITY_F}, Vec3{-INFINITY_F, -INFINITY_F, -INFINITY_F}};
    for (const MeshVertex& vertex : vertices) {
        box.min.x = std::min(box.min.x, vertex.position.x);
        box.min.y = std::min(box.min.y, vertex.position.y);
        box.min.z = std::min(box.min.z, vertex.position.z);
        box.max.x = std::max(box.max.x, vertex.position.x);
        box.max.y = std::max(box.max.y, vertex.position.y);
        box.max.z = std::max(box.max.z, vertex.position.z);
    }
    return box;
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

// --- task 3.5.1: the cooked-section repack's byte readers ---------------------------------------
// Every read goes through cooked_mesh.hpp's bounds-checked get* primitives, so a hostile or
// hand-edited offset yields zeroes rather than a read past the section's slice.
[[nodiscard]] Vec2 readVec2(std::span<const std::byte> bytes, std::size_t at) noexcept {
    return Vec2{assets::getF32(bytes, at), assets::getF32(bytes, at + 4)};
}
[[nodiscard]] Vec3 readVec3(std::span<const std::byte> bytes, std::size_t at) noexcept {
    return Vec3{assets::getF32(bytes, at), assets::getF32(bytes, at + 4), assets::getF32(bytes, at + 8)};
}
[[nodiscard]] Vec4 readVec4(std::span<const std::byte> bytes, std::size_t at) noexcept {
    return Vec4{assets::getF32(bytes, at), assets::getF32(bytes, at + 4), assets::getF32(bytes, at + 8),
                assets::getF32(bytes, at + 12)};
}

// The ABSENT-ATTRIBUTE IDENTITY VALUES (D7), stated once. A vertex with no normal faces +Z, a vertex
// with no tangent runs along +X with glTF's +1 handedness, and a vertex with no UV samples texel
// (0,0). A zero normal is the seed this constant exists to make impossible: it renders a black,
// unlit surface that no automated case in this tree could otherwise see.
constexpr MeshVertex ABSENT_ATTRIBUTE_DEFAULTS{
    .position = Vec3{}, .normal = Vec3{0.0F, 0.0F, 1.0F}, .tangent = Vec4{1.0F, 0.0F, 0.0F, 1.0F}, .uv = Vec2{}};

}  // namespace

namespace detail {

PackedMeshSection packMeshSection(const assets::CookedMesh& mesh, std::uint32_t sectionIndex) {
    PackedMeshSection packed;
    if (sectionIndex >= mesh.sections.size()) {
        return packed;
    }
    const assets::CookedSection& section = mesh.sections[sectionIndex];
    if (section.vertexCount == 0 || section.vertexStride == 0) {
        return packed;
    }
    // The section's slice of the FLAT attribute table, bounds-checked against the table rather than
    // trusted: a parsed mesh always agrees, and a hand-built one must not become a read.
    if (section.firstAttribute > mesh.attributes.size() ||
        mesh.attributes.size() - section.firstAttribute < section.attributeCount) {
        return packed;
    }
    const std::span<const assets::CookedVertexAttribute> attributes{mesh.attributes.data() + section.firstAttribute,
                                                                    section.attributeCount};
    const std::span<const std::byte> bytes = assets::sectionVertexBytes(mesh, sectionIndex);
    if (bytes.size() < static_cast<std::uint64_t>(section.vertexCount) * section.vertexStride) {
        return packed;
    }

    // One pass over the TABLE before one pass over the vertices: which streams exist, and whether
    // every attribute lies wholly inside the stride. Widened to u64 so an absurd offset cannot wrap.
    bool hasJoints = false;
    bool hasWeights = false;
    for (const assets::CookedVertexAttribute& attribute : attributes) {
        const std::uint64_t end =
            static_cast<std::uint64_t>(attribute.offset) + assets::cookedVertexFormatBytes(attribute.format);
        if (end > section.vertexStride) {
            return packed;
        }
        switch (attribute.semantic) {
            case assets::CookedVertexSemantic::Joints0:
                hasJoints = true;
                break;
            case assets::CookedVertexSemantic::Weights0:
                hasWeights = true;
                break;
            // Decoded and DROPPED: the 48-byte MeshVertex has no seat for a second UV set or a
            // vertex colour, and inventing one would change the pipeline's layout for every mesh.
            case assets::CookedVertexSemantic::TexCoord1:
            case assets::CookedVertexSemantic::Color0:
                packed.droppedAttributes = true;
                break;
            case assets::CookedVertexSemantic::Position:
            case assets::CookedVertexSemantic::Normal:
            case assets::CookedVertexSemantic::Tangent:
            case assets::CookedVertexSemantic::TexCoord0:
                break;
        }
    }

    packed.stream0.assign(section.vertexCount, ABSENT_ATTRIBUTE_DEFAULTS);
    // Both or neither, mirroring the format's own pairing rule (docs/09 section 9.8): the cook emits
    // Joints0 and Weights0 together or not at all, and the repack asserts nothing beyond that.
    if (hasJoints && hasWeights) {
        packed.stream1.assign(section.vertexCount, SkinVertex{});
    }

    for (std::uint32_t v = 0; v < section.vertexCount; ++v) {
        const std::size_t base = static_cast<std::size_t>(v) * section.vertexStride;
        MeshVertex& vertex = packed.stream0[v];
        for (const assets::CookedVertexAttribute& attribute : attributes) {
            const std::size_t at = base + attribute.offset;
            switch (attribute.semantic) {
                case assets::CookedVertexSemantic::Position:
                    vertex.position = readVec3(bytes, at);
                    break;
                case assets::CookedVertexSemantic::Normal:
                    vertex.normal = readVec3(bytes, at);
                    break;
                case assets::CookedVertexSemantic::Tangent:
                    vertex.tangent = readVec4(bytes, at);
                    break;
                case assets::CookedVertexSemantic::TexCoord0:
                    vertex.uv = readVec2(bytes, at);
                    break;
                case assets::CookedVertexSemantic::TexCoord1:
                case assets::CookedVertexSemantic::Color0:
                    break;  // already latched above
                case assets::CookedVertexSemantic::Joints0:
                    if (!packed.stream1.empty()) {
                        // u32 VERBATIM — the wire format's own width, never narrowed on the way to
                        // the GPU (rhi::VertexFormat::Uint4 is what the pipeline describes).
                        packed.stream1[v].joints = {assets::getU32(bytes, at), assets::getU32(bytes, at + 4),
                                                    assets::getU32(bytes, at + 8), assets::getU32(bytes, at + 12)};
                    }
                    break;
                case assets::CookedVertexSemantic::Weights0:
                    if (!packed.stream1.empty()) {
                        packed.stream1[v].weights = readVec4(bytes, at);
                    }
                    break;
            }
        }
    }
    return packed;
}

}  // namespace detail

ForwardRenderer::ForwardRenderer(rhi::Device* deviceIn, rhi::GraphicsPipelineHandle pipelineIn,
                                 rhi::GraphicsPipelineHandle pipelineCullNoneIn,
                                 rhi::GraphicsPipelineHandle pipelineSkinnedIn,
                                 rhi::GraphicsPipelineHandle pipelineSkinnedCullNoneIn) noexcept
    : device(deviceIn),
      pipeline(pipelineIn),
      pipelineCullNone(pipelineCullNoneIn),
      pipelineSkinned(pipelineSkinnedIn),
      pipelineSkinnedCullNone(pipelineSkinnedCullNoneIn) {}

ForwardRenderer::ForwardRenderer(ForwardRenderer&& other) noexcept
    : device(other.device),
      pipeline(other.pipeline),
      pipelineCullNone(other.pipelineCullNone),
      pipelineSkinned(other.pipelineSkinned),
      pipelineSkinnedCullNone(other.pipelineSkinnedCullNone),
      primitives(other.primitives),
      defaultTextures(other.defaultTextures),
      materials(std::move(other.materials)),
      meshes(std::move(other.meshes)),
      liveMeshes(std::move(other.liveMeshes)),
      samplerCache(std::move(other.samplerCache)),
      defaultMaterialHandle(other.defaultMaterialHandle),
      paletteScratch(other.paletteScratch),
      skinnedDraws(other.skinnedDraws),
      pipelineBinds(other.pipelineBinds),
      warnedBlendOnce(other.warnedBlendOnce),
      warnedDroppedAttributes(other.warnedDroppedAttributes),
      warnedStaleMesh(other.warnedStaleMesh),
      warnedSubmeshRange(other.warnedSubmeshRange),
      warnedSkinningCap(other.warnedSkinningCap),
      warnedStrayPalette(other.warnedStrayPalette) {
    other.reset();
}

ForwardRenderer& ForwardRenderer::operator=(ForwardRenderer&& other) noexcept {
    if (this != &other) {
        destroyAll();
        device = other.device;
        pipeline = other.pipeline;
        pipelineCullNone = other.pipelineCullNone;
        pipelineSkinned = other.pipelineSkinned;
        pipelineSkinnedCullNone = other.pipelineSkinnedCullNone;
        primitives = other.primitives;
        defaultTextures = other.defaultTextures;
        materials = std::move(other.materials);
        meshes = std::move(other.meshes);
        liveMeshes = std::move(other.liveMeshes);
        samplerCache = std::move(other.samplerCache);
        defaultMaterialHandle = other.defaultMaterialHandle;
        paletteScratch = other.paletteScratch;
        skinnedDraws = other.skinnedDraws;
        pipelineBinds = other.pipelineBinds;
        warnedBlendOnce = other.warnedBlendOnce;
        warnedDroppedAttributes = other.warnedDroppedAttributes;
        warnedStaleMesh = other.warnedStaleMesh;
        warnedSubmeshRange = other.warnedSubmeshRange;
        warnedSkinningCap = other.warnedSkinningCap;
        warnedStrayPalette = other.warnedStrayPalette;
        other.reset();
    }
    return *this;
}

ForwardRenderer::~ForwardRenderer() { destroyAll(); }

void ForwardRenderer::reset() noexcept {
    device = nullptr;
    pipeline = {};
    pipelineCullNone = {};
    pipelineSkinned = {};
    pipelineSkinnedCullNone = {};
    primitives = {};
    defaultTextures = {};
    materials.clear();  // a moved-from SlotMap keeps its scalar bookkeeping; clear() zeroes it
    // Releases nothing, deliberately: MeshEntry holds plain rhi handles, so clearing the registry and
    // its live-handle list abandons them without a device call — which is exactly what the moved-from
    // state needs, and what destroyAll() has already done the releasing for.
    meshes.clear();
    liveMeshes.clear();
    samplerCache.clear();
    defaultMaterialHandle = {};
    paletteScratch = {};
    skinnedDraws = 0;
    pipelineBinds = 0;
    warnedBlendOnce = false;
    warnedDroppedAttributes = false;
    warnedStaleMesh = false;
    warnedSubmeshRange = false;
    warnedSkinningCap = false;
    warnedStrayPalette = false;
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
    if (pipelineSkinned.valid()) {
        device->destroyGraphicsPipeline(pipelineSkinned);
    }
    if (pipelineSkinnedCullNone.valid()) {
        device->destroyGraphicsPipeline(pipelineSkinnedCullNone);
    }
    for (const PrimitiveMesh& mesh : primitives) {
        if (mesh.vbuf.valid()) {
            device->destroyBuffer(mesh.vbuf);
        }
        if (mesh.ibuf.valid()) {
            device->destroyBuffer(mesh.ibuf);
        }
    }
    // Registered meshes DO own their buffers (task 3.5.1), unlike materials — walked through the
    // live-handle list beside the registry, because SlotMap exposes no iteration.
    for (const MeshHandle handle : liveMeshes) {
        if (const MeshEntry* const entry = meshes.get(handle); entry != nullptr) {
            destroyMeshBuffers(*entry);
        }
    }
    // Renderer-owned, unlike the materials' textures: a material BORROWS its textures from the
    // caller, so nothing here walks the registry looking for rhi::TextureHandles to release.
    for (const rhi::TextureHandle texture : defaultTextures) {
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

    // THREE shaders, FOUR pipelines (task 3.5.1). The skinned vertex stage shares the fragment stage
    // byte for byte — its VsOutput is field-for-field identical, so the fragment stage cannot tell
    // which one fed it — which is why scene.frag.hlsl is untouched by this task.
    const rhi::ShaderHandle vs = rhi::loadShader(device, shaderVfs, config.vertexShaderPath);
    const rhi::ShaderHandle vsSkinned = rhi::loadShader(device, shaderVfs, config.skinnedVertexShaderPath);
    const rhi::ShaderHandle fs = rhi::loadShader(device, shaderVfs, config.fragmentShaderPath);
    if (!vs.valid() || !vsSkinned.valid() || !fs.valid()) {
        AERO_LOG_ERROR("ForwardRenderer::create: scene shader load failed");
        if (vs.valid()) {
            device.destroyShader(vs);
        }
        if (vsSkinned.valid()) {
            device.destroyShader(vsSkinned);
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

    // The SKINNED pair (task 3.5.1): two vertex buffer layouts and six attributes. Locations 0-3 are
    // the static layout verbatim from slot 0; locations 4-5 come from slot 1's 32-byte SkinVertex —
    // a Uint4 of joint indices at offset 0 and a Float4 of weights at offset 16, which is the tree's
    // first INTEGER vertex attribute. The same two-field copy produces its cull-none twin, so the
    // doubleSided idiom is applied twice from the same descriptors rather than restated.
    const std::array<rhi::VertexBufferLayout, 2> skinnedLayouts{{
        {.slot = 0, .pitch = sizeof(MeshVertex)},
        {.slot = 1, .pitch = sizeof(detail::SkinVertex)},
    }};
    const std::array<rhi::VertexAttribute, 6> skinnedAttrs{{
        {.location = 0, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offset = 0},
        {.location = 1, .bufferSlot = 0, .format = rhi::VertexFormat::Float3, .offset = 12},
        {.location = 2, .bufferSlot = 0, .format = rhi::VertexFormat::Float4, .offset = 24},
        {.location = 3, .bufferSlot = 0, .format = rhi::VertexFormat::Float2, .offset = 40},
        {.location = 4, .bufferSlot = 1, .format = rhi::VertexFormat::Uint4, .offset = 0},
        {.location = 5, .bufferSlot = 1, .format = rhi::VertexFormat::Float4, .offset = 16},
    }};
    rhi::GraphicsPipelineDesc skinnedDesc = pipelineDesc;
    skinnedDesc.vertexShader = vsSkinned;
    skinnedDesc.vertexBuffers = skinnedLayouts;
    skinnedDesc.vertexAttributes = skinnedAttrs;
    const rhi::GraphicsPipelineHandle pipelineSkinned = device.createGraphicsPipeline(skinnedDesc);
    rhi::GraphicsPipelineDesc skinnedCullNoneDesc = skinnedDesc;
    skinnedCullNoneDesc.rasterizer.cullMode = rhi::CullMode::None;
    const rhi::GraphicsPipelineHandle pipelineSkinnedCullNone = device.createGraphicsPipeline(skinnedCullNoneDesc);

    device.destroyShader(vs);  // safe after pipeline creation (device.hpp)
    device.destroyShader(vsSkinned);
    device.destroyShader(fs);
    if (!pipeline.valid() || !pipelineCullNone.valid() || !pipelineSkinned.valid() ||
        !pipelineSkinnedCullNone.valid()) {
        AERO_LOG_ERROR("ForwardRenderer::create: pipeline creation failed");
        for (const rhi::GraphicsPipelineHandle handle :
             {pipeline, pipelineCullNone, pipelineSkinned, pipelineSkinnedCullNone}) {
            if (handle.valid()) {
                device.destroyGraphicsPipeline(handle);
            }
        }
        return std::nullopt;
    }

    // From here on the renderer OWNS everything created, and its destructor IS the failure path —
    // which is why the old destroyPartial bookkeeping is gone: every early return below releases the
    // pipelines, the buffers uploaded so far, the default textures and the samplers, with no list to
    // keep in sync as that set grows.
    ForwardRenderer renderer{&device, pipeline, pipelineCullNone, pipelineSkinned, pipelineSkinnedCullNone};

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
        renderer.primitives[i] = {vbuf, ibuf, static_cast<std::uint32_t>(geometry.indices.size()),
                                  foldPrimitiveBounds(geometry.vertices)};
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
    // The three 1x1 identity textures (D7), created IN KIND ORDER into the array bindMaterialTextures
    // indexes by kind — one loop index used on both sides of the assignment, so "which texture is at
    // index k" is not a fact stated anywhere that could disagree with material.hpp. That header's
    // defaultTextureTexelForKind is the SINGLE definition of their bytes and formats; restating them
    // here would be the second place for a texel typo to hide, and the tier-0 case that pins them
    // would then prove nothing about what the GPU actually receives.
    for (std::size_t kind = 0; kind < defaultTextures.size(); ++kind) {
        const MaterialDefaultTexture entry = defaultTextureTexelForKind(static_cast<MaterialDefaultTextureKind>(kind));
        const rhi::TextureHandle texture = device->createTexture(
            {.format = entry.format, .usage = rhi::TextureUsage::Sampler, .width = 1, .height = 1});
        defaultTextures[kind] = texture;
        if (!texture.valid() || !device->uploadTexture(texture, 0, std::as_bytes(std::span{entry.texel}))) {
            AERO_LOG_ERROR("ForwardRenderer::create: built-in default texture {} could not be created", kind);
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

void ForwardRenderer::destroyMeshBuffers(const MeshEntry& entry) noexcept {
    if (entry.vertexBuffer.valid()) {
        device->destroyBuffer(entry.vertexBuffer);
    }
    if (entry.skinBuffer.valid()) {
        device->destroyBuffer(entry.skinBuffer);
    }
    if (entry.indexBuffer.valid()) {
        device->destroyBuffer(entry.indexBuffer);
    }
}

MeshHandle ForwardRenderer::createMesh(const assets::CookedMesh& mesh) {
    AERO_PROFILE_ZONE;
    MeshEntry entry;
    // A Uint16 file draws 16-bit. The index region is uploaded VERBATIM — no widening, no re-emit —
    // so this is the only place the file's width becomes a bind-time decision.
    entry.indexType =
        mesh.indexType == assets::CookedIndexType::Uint32 ? rhi::IndexType::Uint32 : rhi::IndexType::Uint16;

    // Repack EVERY section on the CPU first, so both buffer sizes are known before a single GPU
    // object exists and a repack refusal costs nothing to unwind.
    std::vector<MeshVertex> stream0;
    std::vector<detail::SkinVertex> stream1;
    entry.sections.reserve(mesh.sections.size());
    for (std::size_t i = 0; i < mesh.sections.size(); ++i) {
        const detail::PackedMeshSection packed = detail::packMeshSection(mesh, static_cast<std::uint32_t>(i));
        // AN EMPTY REPACK IS A REFUSAL SIGNAL, NOT AN EMPTY SECTION (mesh_pack.hpp:44-47), and the
        // section's own DECLARED vertexCount is what tells the two apart — parseCookedMesh refuses
        // attributeCount == 0 and vertexStride == 0 but NOT vertexCount == 0, so a genuinely empty
        // section is a legal thing to be handed and packs empty for an ordinary reason.
        //
        // Checked BEFORE the offset is recorded, because the offset is `stream0.size()` taken before
        // the append: a section that packs to nothing would inherit whatever is appended NEXT — the
        // following section's vertices, or the end of the buffer when it is the last one — and a
        // submesh naming it would then bind there and drawIndexed past the end of the buffer
        // (robustBufferAccess is an OPTIONAL Vulkan feature in SDL, so that is UB on a device that
        // does not report it) or silently draw another section's geometry. Refusing the WHOLE mesh
        // matches the zero-geometry posture below and costs nothing to unwind: no GPU object exists
        // yet, which is exactly why the repack runs first.
        if (mesh.sections[i].vertexCount > 0 && packed.stream0.empty()) {
            AERO_LOG_ERROR(
                "ForwardRenderer::createMesh: section {} declares {} vertices but repacked to nothing — the "
                "cooked layout is inconsistent (a stride, an attribute offset or a slice does not fit); mesh "
                "refused",
                i, mesh.sections[i].vertexCount);
            return {};
        }
        if (packed.droppedAttributes && !warnedDroppedAttributes) {
            AERO_LOG_WARN(
                "ForwardRenderer::createMesh: a cooked section carries TexCoord1 and/or Color0, which the "
                "48-byte vertex layout has no seat for — decoded and dropped; this warning latches once "
                "per renderer");
            warnedDroppedAttributes = true;
        }
        MeshSectionDraw draw;
        draw.stream0ByteOffset = static_cast<std::uint32_t>(stream0.size() * sizeof(MeshVertex));
        draw.stream1ByteOffset = static_cast<std::uint32_t>(stream1.size() * sizeof(detail::SkinVertex));
        draw.hasSkin = !packed.stream1.empty();
        entry.sections.push_back(draw);
        stream0.insert(stream0.end(), packed.stream0.begin(), packed.stream0.end());
        stream1.insert(stream1.end(), packed.stream1.begin(), packed.stream1.end());
    }

    entry.submeshes.reserve(mesh.submeshes.size());
    for (const assets::CookedSubmesh& submesh : mesh.submeshes) {
        // firstIndex is already ABSOLUTE in index units into the file's single index region
        // (docs/09 section 9.5), which is what lets one bindIndexBuffer at offset 0 serve every
        // submesh. materialIndex travels verbatim; resolving it stays the caller's job.
        entry.submeshes.push_back({.sectionIndex = submesh.sectionIndex,
                                   .firstIndex = submesh.firstIndex,
                                   .indexCount = submesh.indexCount,
                                   .materialIndex = submesh.materialIndex,
                                   // task 3.6.1 -- the file's own per-submesh box, verbatim. Never
                                   // the model box: a submesh at the far end of a model would then
                                   // claim the whole model's extent and never cull.
                                   .bounds = toAabb(submesh.bounds)});
    }

    const std::span<const std::byte> indices = assets::indexBytes(mesh);
    if (stream0.empty() || indices.empty()) {
        // A zero-primitive .aeromesh is a VALID 96-byte file (docs/09 section 9), so this is a
        // refusal rather than a parse concern: there is nothing to create a buffer for, and a
        // zero-size createBuffer is itself an rhi validation failure.
        AERO_LOG_ERROR("ForwardRenderer::createMesh: the cooked mesh carries no drawable geometry");
        return {};
    }

    const auto vertexBytes = static_cast<std::uint32_t>(stream0.size() * sizeof(MeshVertex));
    const auto skinBytes = static_cast<std::uint32_t>(stream1.size() * sizeof(detail::SkinVertex));
    entry.vertexBuffer = device->createBuffer({.usage = rhi::BufferUsage::Vertex, .size = vertexBytes});
    entry.indexBuffer =
        device->createBuffer({.usage = rhi::BufferUsage::Index, .size = static_cast<std::uint32_t>(indices.size())});
    if (!stream1.empty()) {
        entry.skinBuffer = device->createBuffer({.usage = rhi::BufferUsage::Vertex, .size = skinBytes});
    }
    // Uploads go through the BLOCKING init-time path on purpose (device.hpp: "NOT a per-frame path").
    // Short-circuiting is deliberate: nothing is uploaded into a buffer that failed to create.
    const bool ok = entry.vertexBuffer.valid() && entry.indexBuffer.valid() &&
                    (stream1.empty() || entry.skinBuffer.valid()) &&
                    device->uploadBuffer(entry.vertexBuffer, 0, std::as_bytes(std::span{stream0})) &&
                    device->uploadBuffer(entry.indexBuffer, 0, indices) &&
                    (stream1.empty() || device->uploadBuffer(entry.skinBuffer, 0, std::as_bytes(std::span{stream1})));
    if (!ok) {
        // The entry never reached the registry, so its destructor is not the failure path here the
        // way the renderer's own is — release explicitly, then report one handle's worth of failure.
        AERO_LOG_ERROR("ForwardRenderer::createMesh: GPU buffer creation or upload failed");
        destroyMeshBuffers(entry);
        return {};
    }

    const MeshHandle handle = meshes.insert(std::move(entry));
    liveMeshes.push_back(handle);
    return handle;
}

void ForwardRenderer::destroyMesh(MeshHandle mesh) {
    const MeshEntry* const entry = meshes.get(mesh);
    if (entry == nullptr) {
        AERO_LOG_WARN("ForwardRenderer::destroyMesh: stale or invalid MeshHandle — no-op");
        return;
    }
    destroyMeshBuffers(*entry);
    meshes.remove(mesh);
    std::erase(liveMeshes, mesh);
}

std::uint32_t ForwardRenderer::meshSubmeshCount(MeshHandle mesh) const noexcept {
    const MeshEntry* const entry = meshes.get(mesh);
    return entry == nullptr ? 0U : static_cast<std::uint32_t>(entry->submeshes.size());
}

std::size_t ForwardRenderer::samplerCacheSize() const noexcept { return samplerCache.size(); }

bool ForwardRenderer::hasWarnedBlendOpaque() const noexcept { return warnedBlendOnce; }

std::size_t ForwardRenderer::skinnedDrawCount() const noexcept { return skinnedDraws; }

std::size_t ForwardRenderer::pipelineBindCount() const noexcept { return pipelineBinds; }

bool ForwardRenderer::hasWarnedSkinningCap() const noexcept { return warnedSkinningCap; }

void ForwardRenderer::bindMaterialTextures(rhi::RenderPassHandle pass, const MaterialSlot& slot) {
    // A-4: resolution to the built-in defaults happens at BIND time, and "which default belongs to
    // slot k" is answered by material.hpp's defaultTextureKindForSlot — the same function whose answer
    // defaultTextureTexel returns the bytes of, so what a tier-0 case pins IS what this loop binds.
    // There is deliberately no per-slot table here: the three physical textures cover five slots
    // (occlusion shares metallicRoughness' white-linear texel, emissive shares baseColor's white-sRGB
    // one), and a hand-written five-name table restating that aliasing is a swap waiting to happen
    // with no automated witness — binding the flat normal as a base colour still draws a lit surface.
    std::array<rhi::TextureSamplerBinding, MATERIAL_TEXTURE_SLOT_COUNT> bindings{};
    for (std::size_t i = 0; i < MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        const MaterialTextureSlot& source = materialSlotAt(slot.slots, i);
        const rhi::TextureHandle fallback = defaultTextures[static_cast<std::size_t>(defaultTextureKindForSlot(i))];
        bindings[i] = {source.texture.valid() ? source.texture : fallback, slot.samplers[i]};
    }
    // ONE call for all five (AC-26): slot order is declaration order, the shaderc contract, so t0..t4
    // and s0..s4 land in D7's binding order by construction.
    device->bindFragmentSamplers(pass, 0, bindings);
}

// task 3.6.1 -- SILENT by contract, on every path. A nullopt means "cannot prove anything about
// this instance", never an error report: the arms in draw() below own the latched WARNs for the
// cases that produce one, and a line here would either duplicate a warning that is about to fire or
// invent one for a case that is not a problem. The resolution order MIRRORS the draw loop's, so the
// two can never disagree about which instance is which.
std::optional<Aabb> ForwardRenderer::instanceBounds(const MeshInstance& instance) const {
    if (!instance.mesh.valid()) {
        return primitives[clampPrimitiveIndex(instance.primitive)].bounds;  // ARM 1's mesh
    }
    const MeshEntry* const entry = meshes.get(instance.mesh);
    if (entry == nullptr) {
        return std::nullopt;  // stale -- ARM 2's WARN is still owed and must not be consumed here
    }
    if (instance.submesh >= entry->submeshes.size()) {
        return std::nullopt;  // out of range -- likewise ARM 2's
    }
    const MeshSubmeshDraw& submesh = entry->submeshes[instance.submesh];
    if (submesh.sectionIndex >= entry->sections.size()) {
        return std::nullopt;  // the silent section guard's twin
    }
    return submesh.bounds;
}

void ForwardRenderer::draw(Frame& frame, const RenderView& view) {
    AERO_PROFILE_ZONE;
    if (!view.hasCamera) {
        return;  // D5 0-camera: the frame's own clear still shows
    }
    const rhi::RenderPassHandle pass = frame.pass();
    const rhi::CommandBufferHandle cmd = frame.commandBuffer();

    device->bindGraphicsPipeline(pass, pipeline);  // cull-back, static: the reset state
    bool boundCullNone = false;
    bool boundSkinned = false;

    const detail::GpuLightBlock lights = detail::packLights(view);
    device->pushFragmentUniforms(cmd, 0, std::as_bytes(std::span{&lights, 1}));

    // Which of the FOUR pipelines is bound is now a function of TWO booleans, so it moved out of the
    // material-change block and into the per-instance path — the (skinned, cullNone) pair is compared
    // against the bound pair. For a view with no skinned instance the rebind still fires at exactly
    // the same points it always did (only when doubleSided flips), so the static command stream is
    // unchanged. Pushed uniforms are per-COMMAND BUFFER, not per-pipeline (device.hpp), so slots
    // survive every rebind and are never re-pushed for one.
    const auto bindPipelineFor = [&](bool skinned, bool cullNone) {
        if (skinned == boundSkinned && cullNone == boundCullNone) {
            return;
        }
        rhi::GraphicsPipelineHandle wanted = pipeline;
        if (skinned) {
            wanted = cullNone ? pipelineSkinnedCullNone : pipelineSkinned;
        } else if (cullNone) {
            wanted = pipelineCullNone;
        }
        device->bindGraphicsPipeline(pass, wanted);
        ++pipelineBinds;
        boundSkinned = skinned;
        boundCullNone = cullNone;
    };

    // The state cache is three variables, and the loop is correct under ANY instance order because
    // every draw's state is a pure function of its RESOLVED material — cheap under the common order
    // (materials arrive grouped) and never wrong under an adversarial one. No sorting happens here;
    // 3.6.1/Phase 8 own draw ordering.
    MaterialHandle lastMaterial{};
    bool firstMaterial = true;
    bool doubleSided = false;  // the resolved material's, carried across instances with the cache

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
            doubleSided = slot.params.doubleSided;
            lastMaterial = resolved;
            firstMaterial = false;
        }

        const GpuPerObject perObject{instance.mvp, instance.model, instance.normalMatrix, instance.color, 0.0F};

        // --- D8's draw resolution, in order ---------------------------------------------------
        // ARM 1: no registered mesh -> the built-in primitive path, byte-identical to 1.4.1's.
        if (!instance.mesh.valid()) {
            bindPipelineFor(false, doubleSided);
            device->pushVertexUniforms(cmd, 0, std::as_bytes(std::span{&perObject, 1}));
            const PrimitiveMesh& mesh = primitives[clampPrimitiveIndex(instance.primitive)];
            device->bindVertexBuffer(pass, 0, mesh.vbuf);
            device->bindIndexBuffer(pass, mesh.ibuf, rhi::IndexType::Uint16);
            device->drawIndexed(pass, mesh.indexCount);
            continue;
        }

        // ARM 2: a stale handle or an out-of-range submesh SKIPS the instance. Neither is a reason to
        // abandon the frame, and neither may become a read of a freed buffer.
        const MeshEntry* const entry = meshes.get(instance.mesh);
        if (entry == nullptr) {
            // LATCHED like every sibling arm below, and for the sharper reason: a stale MeshHandle in
            // a RenderView is PERSISTENT by nature — a deleted asset with live entity references
            // produces one every frame, forever, for every instance naming it — so an unlatched line
            // here is a 60 Hz log flood rather than the one-off a bad submesh index is.
            if (!warnedStaleMesh) {
                AERO_LOG_WARN(
                    "ForwardRenderer::draw: stale or invalid MeshHandle — instance skipped; this warning "
                    "latches once per renderer");
                warnedStaleMesh = true;
            }
            continue;
        }
        if (instance.submesh >= entry->submeshes.size()) {
            if (!warnedSubmeshRange) {
                AERO_LOG_WARN(
                    "ForwardRenderer::draw: MeshInstance::submesh is past the mesh's submesh table — "
                    "instance skipped; this warning latches once per renderer");
                warnedSubmeshRange = true;
            }
            continue;
        }
        const MeshSubmeshDraw& submesh = entry->submeshes[instance.submesh];
        if (submesh.sectionIndex >= entry->sections.size()) {
            continue;  // unreachable through createMesh (the parse validated it); never a read
        }
        const MeshSectionDraw& section = entry->sections[submesh.sectionIndex];

        // ARM 3: no skin stream OR an empty palette -> the STATIC pipeline. An empty palette on a
        // skinned section IS the bind pose the vertices are authored in, so this degrades to exactly
        // the right picture at zero cost — no identity palette is substituted, and none is needed.
        const bool skinned = section.hasSkin && !instance.palette.empty();
        if (!section.hasSkin && !instance.palette.empty() && !warnedStrayPalette) {
            AERO_LOG_WARN(
                "ForwardRenderer::draw: a palette was supplied for a mesh section with no skin stream — "
                "ignored; this warning latches once per renderer");
            warnedStrayPalette = true;
        }

        // ARM 4: a palette past the measured cap SKIPS the instance rather than truncating it — a
        // truncated palette binds the tail's vertices to joint 0 and looks like a modelling defect.
        if (skinned && instance.palette.size() > MAX_SKINNING_JOINTS) {
            if (!warnedSkinningCap) {
                AERO_LOG_WARN(
                    "ForwardRenderer::draw: skinning palette of {} joints exceeds MAX_SKINNING_JOINTS ({}), "
                    "the measured portable push-uniform ceiling — instance skipped. Raising it means a "
                    "storage buffer instead of a push-uniform slot, which is an rhi surface change. This "
                    "warning latches once per renderer",
                    instance.palette.size(), MAX_SKINNING_JOINTS);
                warnedSkinningCap = true;
            }
            continue;
        }

        bindPipelineFor(skinned, doubleSided);
        device->pushVertexUniforms(cmd, 0, std::as_bytes(std::span{&perObject, 1}));

        if (skinned) {
            // ALWAYS the full 4080-byte block from a ZEROED scratch (INV-S5), so no backend's
            // partial-cbuffer semantics are ever exercised and the unused tail is deterministic.
            paletteScratch.fill(Vec4{});
            detail::packJointPaletteRows(instance.palette,
                                         std::span{paletteScratch}.first(3 * instance.palette.size()));
            device->pushVertexUniforms(cmd, 1, std::as_bytes(std::span{paletteScratch}));
            ++skinnedDraws;
        }

        // BIND-TIME BYTE OFFSETS, and drawIndexed's vertexOffset stays 0 (the plan's section 0.3): a
        // base-vertex draw applies the base to EVERY bound stream uniformly, so a mesh whose static
        // section precedes a skinned one would need stream 1 zero-filled for every vertex or bound at
        // a negative offset. firstIndex is already absolute into the file's single index region, so
        // the index buffer binds once at offset 0.
        device->bindVertexBuffer(pass, 0, entry->vertexBuffer, section.stream0ByteOffset);
        if (skinned) {
            device->bindVertexBuffer(pass, 1, entry->skinBuffer, section.stream1ByteOffset);
        }
        device->bindIndexBuffer(pass, entry->indexBuffer, entry->indexType);
        device->drawIndexed(pass, submesh.indexCount, 1, submesh.firstIndex, 0, 0);
    }
}

}  // namespace engine::render

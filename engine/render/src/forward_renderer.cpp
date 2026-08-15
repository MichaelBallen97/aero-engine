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
};
static_assert(sizeof(GpuLightBlock) == 16 + 32 + (32 * 8));  // 304
static_assert(std::is_trivially_copyable_v<GpuLightBlock>);

// >= PrimitiveId::Count (an out-of-range MeshInstance::primitive, defensive — the bridge already
// clamps) -> Cube.
[[nodiscard]] std::size_t clampPrimitiveIndex(PrimitiveId primitive) {
    const auto index = static_cast<std::size_t>(primitive);
    return index < static_cast<std::size_t>(PrimitiveId::Count) ? index : static_cast<std::size_t>(PrimitiveId::Cube);
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
    return block;
}

}  // namespace

ForwardRenderer::ForwardRenderer(
    rhi::Device* deviceIn, rhi::GraphicsPipelineHandle pipelineIn,
    std::array<PrimitiveMesh, static_cast<std::size_t>(PrimitiveId::Count)> primitivesIn) noexcept
    : device(deviceIn), pipeline(pipelineIn), primitives(primitivesIn) {}

ForwardRenderer::ForwardRenderer(ForwardRenderer&& other) noexcept
    : device(other.device), pipeline(other.pipeline), primitives(other.primitives) {
    other.device = nullptr;
    other.pipeline = {};
    other.primitives = {};
}

ForwardRenderer& ForwardRenderer::operator=(ForwardRenderer&& other) noexcept {
    if (this != &other) {
        destroyAll();
        device = other.device;
        pipeline = other.pipeline;
        primitives = other.primitives;
        other.device = nullptr;
        other.pipeline = {};
        other.primitives = {};
    }
    return *this;
}

ForwardRenderer::~ForwardRenderer() { destroyAll(); }

void ForwardRenderer::destroyAll() noexcept {
    if (device == nullptr) {
        return;
    }
    if (pipeline.valid()) {
        device->destroyGraphicsPipeline(pipeline);
    }
    for (const PrimitiveMesh& mesh : primitives) {
        if (mesh.vbuf.valid()) {
            device->destroyBuffer(mesh.vbuf);
        }
        if (mesh.ibuf.valid()) {
            device->destroyBuffer(mesh.ibuf);
        }
    }
    device = nullptr;
    pipeline = {};
    primitives = {};
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
    device.destroyShader(vs);  // safe after pipeline creation (device.hpp)
    device.destroyShader(fs);
    if (!pipeline.valid()) {
        AERO_LOG_ERROR("ForwardRenderer::create: pipeline creation failed");
        return std::nullopt;
    }

    std::array<ForwardRenderer::PrimitiveMesh, static_cast<std::size_t>(PrimitiveId::Count)> primitives{};
    const auto destroyPartial = [&](std::size_t uploadedCount) {
        device.destroyGraphicsPipeline(pipeline);
        for (std::size_t i = 0; i < uploadedCount; ++i) {
            device.destroyBuffer(primitives[i].vbuf);
            device.destroyBuffer(primitives[i].ibuf);
        }
    };

    for (std::size_t i = 0; i < primitives.size(); ++i) {
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
        const bool uploadedVertices =
            vbuf.valid() && device.uploadBuffer(vbuf, 0, std::as_bytes(std::span{geometry.vertices}));
        const bool uploadedIndices =
            ibuf.valid() && device.uploadBuffer(ibuf, 0, std::as_bytes(std::span{geometry.indices}));
        if (!uploadedVertices || !uploadedIndices) {
            AERO_LOG_ERROR("ForwardRenderer::create: primitive {} upload failed", i);
            if (vbuf.valid()) {
                device.destroyBuffer(vbuf);
            }
            if (ibuf.valid()) {
                device.destroyBuffer(ibuf);
            }
            destroyPartial(i);
            return std::nullopt;
        }
        primitives[i] = {vbuf, ibuf, static_cast<std::uint32_t>(geometry.indices.size())};
    }

    return ForwardRenderer{&device, pipeline, primitives};
}

void ForwardRenderer::draw(Frame& frame, const RenderView& view) {
    AERO_PROFILE_ZONE;
    if (!view.hasCamera) {
        return;  // D5 0-camera: the frame's own clear still shows
    }
    const rhi::RenderPassHandle pass = frame.pass();
    const rhi::CommandBufferHandle cmd = frame.commandBuffer();

    device->bindGraphicsPipeline(pass, pipeline);

    const GpuLightBlock lights = pack(view);
    device->pushFragmentUniforms(cmd, 0, std::as_bytes(std::span{&lights, 1}));

    for (const MeshInstance& instance : view.instances) {
        const GpuPerObject perObject{instance.mvp, instance.model, instance.normalMatrix, instance.color, 0.0F};
        device->pushVertexUniforms(cmd, 0, std::as_bytes(std::span{&perObject, 1}));

        const PrimitiveMesh& mesh = primitives[clampPrimitiveIndex(instance.primitive)];
        device->bindVertexBuffer(pass, 0, mesh.vbuf);
        device->bindIndexBuffer(pass, mesh.ibuf, rhi::IndexType::Uint16);
        device->drawIndexed(pass, mesh.indexCount);
    }
}

}  // namespace engine::render

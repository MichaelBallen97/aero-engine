#pragma once
// Aero Engine — render::SkyPass (task E.2.1): the background gradient, one fullscreen triangle,
// recorded into a pass the CALLER opened and has NOT ended. Its slot is SceneRenderer::render,
// between the shadow pass and the forward pass: the sky writes no depth, so the geometry that
// follows overdraws it wherever it covers, and E.1.1's Tested debug lines still depth-test against
// the cleared 1.0 everywhere else.
//
// THERE IS NO MODE IN THE SHADERS. The two background modes live entirely on the CPU
// (environment.hpp's resolveSkyGradient), and the GPU receives a horizon plus two DIFFERENCES -- a
// Solid sky is two EXACTLY ZERO deltas, and `x + 0 * w` is exact on every IEEE backend, which is what
// makes Solid reproduce a clear bit for bit. There is no second pipeline, no variant key and nothing
// to keep in sync between two HLSL arms.
//
// LIFETIME CONTRACT (post_process.hpp's, verbatim): the rhi::Device passed to create() MUST outlive
// the SkyPass. Move-only with USER-DEFINED noexcept moves -- a defaulted move would double-free the
// pipeline.
//
// ERROR MODEL (docs/04): nothing throws. create() -> nullopt (+ one ERROR naming the cause), having
// destroyed anything it already made; draw() is void and best-effort with latched WARNs, matching
// ForwardRenderer::draw and PostProcess::resolve.

#include <aero/render/lighting.hpp>       // RenderView, CameraView
#include <aero/render/render_target.hpp>  // Frame
#include <aero/rhi/format.hpp>
#include <aero/rhi/handles.hpp>
#include <aero/rhi/types.hpp>

#include <cstddef>
#include <optional>
#include <string_view>

namespace engine::rhi {
class Device;  // forward-declared, exactly as forward_renderer.hpp / post_process.hpp do
}  // namespace engine::rhi

namespace engine {
class VirtualFileSystem;  // forward-declared: create() takes it by ref; the .cpp includes vfs.hpp
}  // namespace engine

namespace engine::render {

struct SkyPassConfig {
    // REQUIRED: the format of the COLOUR target of the pass draw() records into. Invalid or a depth
    // format FAILS create(). Named as ForwardRendererConfig's twin, because this pass records into
    // the SAME pass the forward renderer does -- not as PostProcessConfig's `outputColorFormat`,
    // which names a DIFFERENT target.
    rhi::TextureFormat colorFormat = rhi::TextureFormat::Invalid;
    // The DEPTH format of that same pass. It MUST match even though depth is neither tested nor
    // written (GraphicsPipelineDesc's sentinel; post_process.cpp's outputDepthFormat note). Invalid
    // means "a depth-free frame" and is LEGAL.
    rhi::TextureFormat depthFormat = rhi::TextureFormat::Invalid;
    // Extension-less res:// VFS paths, READ ONLY INSIDE create() and never after -- the stored copy's
    // views are never dereferenced again (PostProcessConfig's own note, same reason).
    std::string_view vertexShaderPath = "res://sky.vert";
    std::string_view fragmentShaderPath = "res://sky.frag";
};

class SkyPass {
public:
    // nullopt + ONE ERROR naming the cause on: colorFormat Invalid or a depth format; either shader
    // failing to load; pipeline creation failing. DESTROYS ANYTHING IT ALREADY CREATED before
    // returning -- no ~Device leaked-shader WARN on any failure path (PP4's rule).
    [[nodiscard]] static std::optional<SkyPass> create(rhi::Device& device, const VirtualFileSystem& shaderVfs,
                                                       const SkyPassConfig& config);

    ~SkyPass();                   // no-op if moved-from
    SkyPass(SkyPass&&) noexcept;  // USER-DEFINED -- a defaulted move double-frees the pipeline
    SkyPass& operator=(SkyPass&&) noexcept;
    SkyPass(const SkyPass&) = delete;
    SkyPass& operator=(const SkyPass&) = delete;

    // Records ONE three-vertex draw into `frame`'s OPEN pass: bind, push the 64-byte camera block
    // (vertex slot 0) and the 48-byte gradient block (fragment slot 0), draw(3).
    //
    // SETS NO VIEWPORT AND NO SCISSOR. It inherits the frame's, exactly as ForwardRenderer::draw
    // does, so the sky and the geometry cover the same rect BY CONSTRUCTION rather than by two calls
    // that agree. renderSelectionMask sets its own because it OPENS its own pass on a margined
    // target; PostProcess::resolve sets its own because it draws into someone else's frame. Neither
    // applies here, and a viewport set here would be a second source for one fact.
    //
    // Records NOTHING when !view.hasCamera -- silently, the forward pass's own 0-camera rule -- and
    // nothing when packSkyCamera refuses, which WARNs ONCE per pass lifetime. Best-effort and void.
    // A moved-from pass is a logged no-op with drawCount() UNMOVED (PostProcess::resolve's asymmetry).
    void draw(Frame& frame, const RenderView& view);

    // Draws that actually ISSUED, pass lifetime. A refused draw does NOT move it.
    [[nodiscard]] std::size_t drawCount() const noexcept;
    // The degenerate-camera latch's observable: inverse(proj * view) had a non-finite element on some
    // draw. Latched once, never per frame.
    [[nodiscard]] bool hasWarnedDegenerateCamera() const noexcept;

private:
    SkyPass(rhi::Device* devicePtr, rhi::GraphicsPipelineHandle pipelineHandle) noexcept;
    void destroyAll() noexcept;  // dtor + move-assign share this; no-op when device == nullptr
    void reset() noexcept;       // null every member WITHOUT releasing anything (the moved-from state)

    rhi::Device* device = nullptr;  // non-owning; outlives the pass (contract)
    rhi::GraphicsPipelineHandle pipeline{};
    std::size_t draws = 0;
    bool warnedDegenerateCamera = false;
    // A SECOND latch, private and accessor-less. A not-renderable pass stays not renderable, so an
    // unlatched WARN here would fire every frame forever. drawCount() unmoved is already the
    // observable, so this needs no accessor -- PostProcess's third latch, same reason.
    bool warnedNotRenderable = false;
};

}  // namespace engine::render

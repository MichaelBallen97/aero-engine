#pragma once
// Aero Engine — render::PostProcess (task 3.6.3): the fullscreen tonemap/gamma scaffold. It OWNS an
// HDR scene RenderTarget (colour + depth), and its whole job is: give the forward pass somewhere
// linear and unbounded to draw, then resolve that into the 8-bit surface the screen or ImGui reads.
//
// WHY IT OWNS THE TARGET (D4) rather than being a stateless blit taking a texture handle.
// RenderTarget's ALLOCATION extent is >= its DRAWN extent (quantum = 64 in both editor consumers),
// so a resolve that samples [0,1]^2 reads unrendered margin. Making the caller compute
// uvMax = drawExtent / textureExtent puts a correctness-critical two-line division at every call
// site; owning the target puts it in one function that cannot be called wrongly. Ownership is also
// what makes the two invariants OBSERVABLE at all -- see the two hasWarned* accessors.
//
// THE CYCLE, and it is not optional (INV-7):
//     beginScene(clear) -> [forward/scene draws] -> endScene(frame)      // submits command buffer A
//     output.beginFrame(clear) -> resolve(outFrame, params) -> output.endFrame(outFrame)   // buffer B
// A is submitted BEFORE B is acquired, so render_target.hpp's queue-ordering note applies with no
// interleaving at all: SDL_GPU transitions the colour attachment back to "ready for a graphics read"
// when A's pass ends, and commands in an earlier submit begin before any command in a later one.
// NO EXPLICIT BARRIER IS NEEDED AND NONE IS AVAILABLE.
//
// resolve() NEVER REFUSES. A mismatched extent still draws (stretched), a resolve before endScene
// still draws (undefined contents), and each latches its own WARN once per pass lifetime. A stretched
// picture beats a black one, which is exactly what makes the LATCH -- not the absence of a picture --
// the observable. The one asymmetry: a moved-from or not-renderable pass is a logged no-op with
// resolveCount() UNMOVED (PP10).
//
// LIFETIME CONTRACTS (render_target.hpp's, one level up): the rhi::Device passed to create() MUST
// outlive the PostProcess. Move-only with USER-DEFINED noexcept moves -- a defaulted move would
// double-free the pipeline and the sampler.
//
// ERROR MODEL (docs/04): nothing throws. create() -> nullopt (+ one ERROR naming the cause);
// resize() -> false leaving the pass not renderable; beginScene() -> nullopt; resolve() is void and
// best-effort, matching ForwardRenderer::draw.

#include <aero/render/render_target.hpp>  // RenderTarget, Frame, RENDER_TARGET_MAX_EXTENT
#include <aero/render/tonemap.hpp>        // TonemapParams
#include <aero/rhi/format.hpp>
#include <aero/rhi/handles.hpp>
#include <aero/rhi/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace engine::rhi {
class Device;  // forward-declared, exactly as forward_renderer.hpp / render_target.hpp do
}  // namespace engine::rhi

namespace engine {
class VirtualFileSystem;  // forward-declared: create() takes it by ref; the .cpp includes vfs.hpp
}  // namespace engine

namespace engine::render {

// PURE, GPU-free, total (tier-0). The source sub-rect's UV maximum, per axis, independently:
//     texture == 0            -> 1.0        (a not-renderable target; never a division by zero)
//     otherwise               -> min(draw / texture, 1.0)
// The min() is INV-1's violation handled rather than asserted: draw > texture cannot happen under the
// adjacent-resize rule, and if it ever does, sampling past 1.0 would read the margin.
// It lives here rather than in tonemap.hpp because it names an rhi type and because it belongs beside
// its only consumer.
[[nodiscard]] Vec2 tonemapSourceUvMax(rhi::Extent2D drawExtent, rhi::Extent2D textureExtent) noexcept;

struct PostProcessConfig {
    // REQUIRED. The format of the surface resolve() writes into -- the swapchain's colorFormat(), or
    // the editor's RGBA8Unorm output target. Invalid or a depth format FAILS create().
    rhi::TextureFormat outputColorFormat = rhi::TextureFormat::Invalid;
    // The pipeline's depth format MUST match the pass it records into (the GraphicsPipelineDesc
    // sentinel). Invalid means "resolve into a depth-free frame", which is what both editor consumers
    // use; a caller resolving into a depth-carrying frame passes that frame's depth format here.
    // PP12 covers both arms.
    rhi::TextureFormat outputDepthFormat = rhi::TextureFormat::Invalid;
    // The HDR intermediate. RGBA16Float is on SDL's universally-supported list for BOTH the SAMPLER
    // and COLOR_TARGET usages this needs (SDL_gpu.h:696, :712, pinned 3.4.12), and
    // RenderTarget::create queries supportsTextureFormat anyway, so an exotic device produces a clean
    // nullopt + ERROR rather than a wrong picture. THERE IS DELIBERATELY NO FALLBACK CHAIN: a silent
    // downgrade to a format with different precision is a picture that differs between machines with
    // nothing reporting it. This is a config field so Phase 8.2's HDR work has a seam.
    rhi::TextureFormat sceneColorFormat = rhi::TextureFormat::RGBA16Float;
    // The SCENE target's depth, which the forward pass needs (ForwardRendererConfig rejects an
    // Invalid depthFormat outright). Not to be confused with outputDepthFormat above -- both editor
    // consumers keep this TRUE and make their OUTPUT target depth-free.
    bool sceneDepth = true;
    std::uint32_t quantum = 1;
    std::uint32_t maxExtent = RENDER_TARGET_MAX_EXTENT;
    // Extension-less res:// VFS paths, resolved through the caller-supplied VirtualFileSystem.
    // READ ONLY INSIDE create() and never after -- the stored copy's views are never dereferenced
    // again, which is stated rather than worked around, because a string_view member that outlives
    // its backing string is a recorded editor-side trap.
    std::string_view vertexShaderPath = "res://fullscreen.vert";
    std::string_view fragmentShaderPath = "res://tonemap.frag";
};

class PostProcess {
public:
    // nullopt + ONE ERROR naming the cause on: outputColorFormat Invalid or a depth format; either
    // shader failing to load; pipeline creation failing; sampler creation failing; the scene
    // RenderTarget failing. DESTROYS ANYTHING IT ALREADY CREATED before returning -- no ~Device leak
    // WARN on any failure path (AC-7, PP4).
    [[nodiscard]] static std::optional<PostProcess> create(rhi::Device& device, const VirtualFileSystem& shaderVfs,
                                                           rhi::Extent2D requested, const PostProcessConfig& config);

    ~PostProcess();                                  // no-op if moved-from
    PostProcess(PostProcess&&) noexcept;             // USER-DEFINED: transfers + nulls the source
    PostProcess& operator=(PostProcess&&) noexcept;  // (a defaulted move double-frees the GPU handles)
    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    // Forwards to the scene target. false (+ ERROR) only on a real allocation failure, which leaves
    // the pass NOT renderable but destructible.
    bool resize(rhi::Extent2D requested);

    // Clears the sceneEnded flag, then forwards to the scene target.
    [[nodiscard]] std::optional<Frame> beginScene(const rhi::Color& clearColor);
    // Sets the sceneEnded flag, then forwards. Consumes `frame` and SUBMITS command buffer A.
    bool endScene(Frame frame);

    // Records the resolve into `output`'s OPEN pass. See the class comment for the two latches and
    // the one asymmetry. Best-effort and void, matching ForwardRenderer::draw.
    void resolve(Frame& output, const TonemapParams& params);

    [[nodiscard]] rhi::TextureFormat sceneColorFormat() const noexcept;
    [[nodiscard]] rhi::TextureFormat sceneDepthFormat() const noexcept;
    [[nodiscard]] rhi::Extent2D sceneDrawExtent() const noexcept;
    [[nodiscard]] rhi::Extent2D sceneTextureExtent() const noexcept;

    // --- diagnostics (the 3.4.1 / 3.5.1 / 3.6.1 posture: they REPORT, they never change behaviour)
    // Resolves that actually issued a draw, pass lifetime. A refused resolve does NOT move it, which
    // is the whole of PP10's assertion.
    [[nodiscard]] std::size_t resolveCount() const noexcept;
    // INV-1's observable: output.extent() != sceneDrawExtent() on some resolve. Latched once.
    [[nodiscard]] bool hasWarnedExtentMismatch() const noexcept;
    // INV-7's observable: a resolve recorded before endScene() closed the scene pass. Without this,
    // that is a silent read of undefined texture contents that NO AUTOMATED TIER IN THIS TREE CAN
    // SEE, because nothing here reads a pixel back. Latched once; beginScene clears the flag again.
    [[nodiscard]] bool hasWarnedResolveBeforeEndScene() const noexcept;

private:
    PostProcess(rhi::Device* device, const PostProcessConfig& config, RenderTarget&& sceneTarget,
                rhi::GraphicsPipelineHandle pipelineHandle, rhi::SamplerHandle samplerHandle) noexcept;
    void destroyAll() noexcept;  // dtor + move-assign share this; no-op when device == nullptr
    void reset() noexcept;       // null every member WITHOUT releasing anything (the moved-from state)

    rhi::Device* device = nullptr;  // non-owning; outlives the pass (contract)
    PostProcessConfig cfg{};
    std::optional<RenderTarget> scene;  // OWNED (D4)
    rhi::GraphicsPipelineHandle pipeline{};
    rhi::SamplerHandle sampler{};
    std::size_t resolves = 0;
    bool sceneEnded = false;
    bool warnedExtentMismatch = false;
    bool warnedResolveBeforeEndScene = false;
    // A THIRD latch, private and accessor-less. A not-renderable pass stays not renderable, so an
    // unlatched WARN here would fire every frame forever -- the defect 3.5.1's code-review round
    // closed on the stale-handle WARN. resolveCount() unmoved is already the observable PP10 asserts,
    // so this needs no accessor.
    bool warnedNotRenderable = false;
};

}  // namespace engine::render

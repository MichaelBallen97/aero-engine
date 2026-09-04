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

#include <aero/assets/cooked_mesh.hpp>  // task 3.5.1 — createMesh's parameter (the 3.4.1 assets edge)
#include <aero/core/slot_map.hpp>
#include <aero/render/culling.hpp>  // task 3.6.1 -- Aabb (the registry's per-mesh bounds)
#include <aero/render/lighting.hpp>
#include <aero/render/material.hpp>
#include <aero/render/mesh.hpp>
#include <aero/render/renderer.hpp>           // Frame
#include <aero/render/selection_outline.hpp>  // task E.1.4 -- SelectionMaskView, SELECTION_MASK_*
#include <aero/render/skinning.hpp>           // task 3.5.1 — MAX_SKINNING_JOINTS (the palette scratch's size)
#include <aero/rhi/descriptors.hpp>           // rhi::SamplerDesc
#include <aero/rhi/format.hpp>                // rhi::TextureFormat
#include <aero/rhi/types.hpp>                 // rhi::IndexType

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>  // task E.1.4 -- renderSelectionMask's two instance spans
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
    // task 3.5.1 — the skinned twin, sharing fragmentShaderPath byte for byte. create() loads all
    // three and builds FOUR pipelines (static/skinned x cull-back/cull-none); a missing skinned
    // shader fails create() loudly, exactly as a missing static one always has.
    std::string_view skinnedVertexShaderPath = "res://scene_skinned.vert";
    std::string_view fragmentShaderPath = "res://scene.frag";
    // task 3.6.2 — the depth pass. APPENDED and DEFAULTED, so every existing caller (both editor
    // owners, four samples, every test) compiles and behaves identically, exactly as 3.5.1's
    // skinnedVertexShaderPath did one epic ago.
    //
    // shadowMapResolution: 0 means SHADOWS OFF and is exact -- it is never rounded and never
    // clamped. Any other value is rounded UP to the next power of two and then clamped to
    // [256, 8192], with ONE warning naming the requested and the allocated value, because a bad
    // number is a configuration typo and refusing to start a renderer over one is a worse answer
    // than starting with something sane. shadowMapResolution() reports what was ALLOCATED.
    //
    // A missing shadow shader FAILS create() loudly, exactly as a missing skinned vertex shader has
    // since 3.5.1: all three ship in shaders/CMakeLists.txt, so a missing one means a broken build,
    // not a configuration the engine should paper over.
    std::uint32_t shadowMapResolution = 2048;
    std::string_view shadowVertexShaderPath = "res://shadow.vert";
    std::string_view shadowSkinnedVertexShaderPath = "res://shadow_skinned.vert";
    std::string_view shadowFragmentShaderPath = "res://shadow.frag";
    // task E.1.4 -- the SELECTION MASK pass. false skips the four pipelines entirely and makes
    // renderSelectionMask a silent no-op returning an invalid view: the shadowMapResolution == 0
    // escape hatch, in bool form.
    //
    // DEFAULT TRUE. The mask pipelines cost four pipeline objects at create() and NO TEXTURE until
    // the first renderSelectionMask, so a sample that never selects anything pays only the creation.
    // A MISSING selection_mask.frag FAILS create() loudly while this is true, exactly as a missing
    // shadow shader has since 3.6.2: all four stages ship in shaders/CMakeLists.txt, so a missing one
    // means a broken build rather than a configuration the engine should paper over.
    bool selectionMask = true;
    std::string_view selectionMaskFragmentShaderPath = "res://selection_mask.frag";
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

    // task 3.6.2 — the depth pass. Records every caster into the renderer-owned shadow map on its
    // OWN command buffer and submits it, then returns the light-space transform draw() needs.
    //
    //     view.shadow = forward.renderShadowMap(view);
    //     forward.draw(frame, view);
    //
    // IT TAKES NO Frame AND TOUCHES NONE. `Frame` carries an already-open render pass and SDL
    // refuses a second pass on a command buffer that has one open, so the depth pass cannot be
    // recorded into the frame the caller hands draw(). This is RenderTarget::endFrame's own pattern:
    // command buffers submitted earlier on the queue order before later ones, so the map is written
    // by this submission and sampled by the frame's, with no explicit barrier (SDL's per-container
    // state tracking performs the depth-write -> shader-read transition on bind).
    //
    // CALLING IT INSIDE AN OPEN FRAME IS LEGAL and is what SceneRenderer::render does -- the two
    // passes are on different command buffers and SDL's pass-in-progress guard is per command
    // buffer. The cost is that the depth pass is recorded AFTER Renderer::beginFrame's vsync block
    // rather than before it. A caller that wants it off the critical path calls this BEFORE
    // beginFrame; nothing in the returned value depends on the frame.
    //
    // Returns ShadowView{} (valid == false) -- submitting nothing and acquiring NO command buffer --
    // when the view has no camera, when shadowsEnabled is false, when the light does not cast, when
    // its intensity is 0, when shadowMapResolution() is 0, or when the fit is invalid. Only the last
    // of those warns, and it latches.
    [[nodiscard]] ShadowView renderShadowMap(const RenderView& view);

    // task E.1.4 -- the SELECTION MASK pass. Records every instance in `secondary`, then every
    // instance in `primary`, into a RENDERER-OWNED R8Unorm texture, DEPTH-TESTED against `sceneDepth`
    // with CompareOp::LessOrEqual and depth write OFF.
    //
    //     const SelectionMaskView mask = forward.renderSelectionMask(
    //         post.sceneDepthTexture(), post.sceneTextureExtent(), post.sceneDrawExtent(),
    //         set.secondary, set.primary);
    //     outline.composite(outFrame, mask, params);
    //
    // IT TAKES NO Frame AND TOUCHES NONE, for renderShadowMap's reason: a Frame carries an
    // already-open pass and SDL refuses a second pass on a command buffer that has one open. It
    // acquires its OWN command buffer, records, and SUBMITS -- so it MUST be called AFTER
    // PostProcess::endScene has submitted the pass that wrote `sceneDepth`, and BEFORE the frame that
    // composites. Commands in an earlier submit begin before any command in a later one, so no
    // explicit barrier exists and none is needed.
    //
    // `sceneDepth` MUST have been written by a pass that STORED it (RenderTargetConfig::depthStore /
    // PostProcessConfig::sceneDepthStore). Reading a DontCare'd depth attachment is undefined, and is
    // GARBAGE on a tile-based deferred renderer -- every Apple Silicon Mac. NOTHING HERE CAN DETECT
    // THAT, which is why the two flags exist and why this sentence is here.
    //
    // `textureExtent` is the ALLOCATION of the depth texture, and the mask is (re)allocated to match
    // it EXACTLY -- never sized independently (D4: SDL requires every attachment in a pass to agree on
    // dimensions, and a second sizing policy would diverge the first time the two were created or
    // resized in a different order). `drawExtent` is the rendered sub-rect and is what viewport and
    // scissor are set to -- NOT OPTIONAL, and invisible in every test whose drawExtent equals its
    // textureExtent (D10).
    //
    // Returns an INVALID view -- acquiring nothing and submitting nothing -- when the pipelines were
    // not built, when both spans are empty, when either extent is degenerate, when drawExtent exceeds
    // textureExtent on either axis, or on any acquire / pass / allocation failure.
    [[nodiscard]] SelectionMaskView renderSelectionMask(rhi::TextureHandle sceneDepth, rhi::Extent2D textureExtent,
                                                        rhi::Extent2D drawExtent,
                                                        std::span<const MeshInstance> secondary,
                                                        std::span<const MeshInstance> primary);

    [[nodiscard]] std::size_t selectionMaskPassCount() const noexcept;       // command buffers ACQUIRED, lifetime
    [[nodiscard]] std::size_t lastFrameSelectionMaskDrawn() const noexcept;  // instances that issued a draw
    [[nodiscard]] bool hasWarnedSelectionMaskCaster() const noexcept;        // D6's Mask/Blend latch
    [[nodiscard]] bool hasWarnedSelectionMaskUnavailable() const noexcept;   // pipelines unbuilt / alloc failed

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

    // --- meshes (task 3.5.1) --------------------------------------------------------------------
    // The tree's first mesh registry, with the material registry's semantics member for member:
    // generational handles, so a stale one is a logged no-op and a stale MeshInstance::mesh draws
    // nothing rather than reading a freed buffer.
    //
    // createMesh repacks every cooked section on the CPU and uploads it into at most two GPU streams
    // through the existing BLOCKING init-time path — uploads happen at LOAD, never per frame (the
    // 3.4.1 D17 posture, and rhi::Device::uploadBuffer's own "NOT a per-frame path" contract).
    //
    // LIFETIME: the caller's parse BUFFER must stay alive ACROSS this call. A CookedMesh's bulk data
    // is a RETAINED SPAN into the bytes handed to parseCookedMesh (the docs/09 section 9 contract),
    // and createMesh reads it through sectionVertexBytes/indexBytes. Nothing is retained after
    // createMesh returns. Returns an invalid handle (+ ERROR) if any buffer create or upload fails.
    [[nodiscard]] MeshHandle createMesh(const assets::CookedMesh& mesh);
    // Releases the entry's GPU buffers. A stale or invalid handle is a logged no-op, so a
    // double-destroy is safe.
    void destroyMesh(MeshHandle mesh);
    // How many submeshes a registered mesh has — the range MeshInstance::submesh must index into.
    // 0 for an invalid or stale handle, which is what makes "no submesh is in range" the answer a
    // caller gets without having to ask twice.
    [[nodiscard]] std::uint32_t meshSubmeshCount(MeshHandle mesh) const noexcept;

    // --- diagnostics (task 3.4.1) ---------------------------------------------------------------
    // Two small observability accessors, documented as such rather than smuggled in: the sampler
    // cache's size and the Blend-drawn-opaque latch are otherwise unobservable without a log sink,
    // which this project deliberately does not have (the 0.2.4 deferral). They report; they never
    // change behaviour.
    [[nodiscard]] std::size_t samplerCacheSize() const noexcept;
    [[nodiscard]] bool hasWarnedBlendOpaque() const noexcept;

    // --- diagnostics (task 3.5.1) ---------------------------------------------------------------
    // The same posture, two more: which arm of the draw resolution an instance took is otherwise
    // unobservable, so "this drew skinned" and "the joint cap fired" would each be a behaviour no
    // automated case could witness. They report; they never change behaviour.
    [[nodiscard]] std::size_t skinnedDrawCount() const noexcept;  // through the skinned pipelines, renderer lifetime
    [[nodiscard]] bool hasWarnedSkinningCap() const noexcept;     // the over-cap latch fired at least once
    // How many pipeline TRANSITIONS draw() has issued, renderer lifetime — the per-frame reset bind
    // is deliberately NOT counted, so this reads 0 for any view whose instances all want the same
    // pipeline and N for a view that alternates N times. Added by the
    // code-review round for a reason worth stating: a WRONG pipeline for an instance is invisible to
    // every other observable here. skinnedDrawCount() counts the instances that took the skinned ARM,
    // not the pipeline they were drawn with, and a static instance that wrongly inherits the skinned
    // pipeline reads stale stream-1 data — valid memory, no validation error, wrong picture. Counting
    // binds makes both transitions of the four-pipeline state machine observable: N alternating
    // instances must produce N binds, and a skipped rebind shows up as a smaller number.
    [[nodiscard]] std::size_t pipelineBindCount() const noexcept;

    // --- diagnostics (task 3.6.1) ---------------------------------------------------------------
    // Culling counters, PER-FRAME: both reset at the top of every draw() -- including a draw() that
    // early-returns on !view.hasCamera, so a no-camera frame reads 0/0. THEY NEED NOT SUM TO
    // view.instances.size(): an instance skipped by the stale-handle arm, the submesh-range arm, the
    // section guard or the over-cap arm was not culled and did not draw, so it lands in NEITHER
    // bucket. lastFrameDrawn counts issued drawIndexed calls and nothing else -- it is incremented
    // at the two drawIndexed sites and nowhere else, which is what makes that gap true by
    // construction rather than by bookkeeping that could drift.
    [[nodiscard]] std::size_t lastFrameDrawn() const noexcept;
    [[nodiscard]] std::size_t lastFrameCulled() const noexcept;
    // How many times the material-change block ran (fragment uniform push + five-texture bind),
    // renderer lifetime. Added for the same reason pipelineBindCount was in 3.5.1: that block
    // executing for a CULLED instance is otherwise unobservable, and it is exactly what culling
    // ahead of material resolution saves. Pipeline binds cannot see the difference -- they happen
    // INSIDE the draw arms, downstream of both candidate cull placements -- so counting them proves
    // culling reduces rebinds without proving the cull sits where it was designed to sit.
    [[nodiscard]] std::size_t materialBindCount() const noexcept;
    // The degenerate-projection latch fired at least once: a viewProj that yields no usable frustum
    // disables culling FOR THAT DRAW and warns, rather than culling to black.
    [[nodiscard]] bool hasWarnedDegenerateFrustum() const noexcept;

    // --- diagnostics (task 3.6.2) ---------------------------------------------------------------
    // The same posture as 3.6.1's pair, and the same two rules. PER-FRAME: both reset at the TOP of
    // every renderShadowMap, INCLUDING one that early-returns, so a shadowless frame reads 0/0
    // rather than the previous frame's numbers. AND THEY NEED NOT SUM to view.instances.size(): an
    // instance dropped by the shared resolver was neither drawn nor culled, so it lands in NEITHER
    // bucket. ++shadowDrawn lives at the TWO drawIndexed sites -- the primitive arm and the mesh
    // arm -- and nowhere else, which makes that gap true by construction rather than by bookkeeping
    // that could drift. The count is two, not one: a maintainer who greps, finds two and "corrects"
    // the code to match a "single site" claim would silently stop counting every primitive caster.
    [[nodiscard]] std::size_t lastFrameShadowDrawn() const noexcept;
    [[nodiscard]] std::size_t lastFrameShadowCulled() const noexcept;
    // The fit failed at least once. Latched: an invalid fit is a real problem (a degenerate camera,
    // a zero-length sun) and must be loud, but it is also persistent, so an unlatched line would be
    // a 60 Hz flood. A DISABLED light is not a failed fit and does not warn.
    [[nodiscard]] bool hasWarnedShadowFit() const noexcept;
    // What was ALLOCATED, not what was requested: a caller who asked for 3000 and got 4096 must be
    // able to see that without reading the log, and the PCF step in the fragment block is 1 / this.
    // 0 means shadows are off (and a 1x1 depth placeholder is bound, so slot 5 is never unbound).
    [[nodiscard]] std::uint32_t shadowMapResolution() const noexcept;
    // How many command buffers this renderer has ACQUIRED for a depth pass, renderer lifetime.
    // Counted at the ACQUISITION rather than at the submit, deliberately: the contract
    // renderShadowMap makes is that it acquires NOTHING on an early return, and a counter that moved
    // only at submit could not tell "never acquired" from "acquired and leaked".
    [[nodiscard]] std::size_t shadowPassCount() const noexcept;

private:
    struct PrimitiveMesh {
        rhi::BufferHandle vbuf;
        rhi::BufferHandle ibuf;
        std::uint32_t indexCount = 0;
        // task 3.6.1 -- FOLDED in create() over the vertices make{Cube,Sphere,Plane}() actually
        // returned, never a table: there is no second copy of 0.5 to drift out of step with
        // primitives.cpp, which IS the single source for what each primitive's shape is.
        Aabb bounds{};
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

    // One registered cooked mesh (task 3.5.1). Every section is concatenated into ONE stream-0
    // buffer and (for skinned sections only) one stream-1 buffer, so a draw binds by BYTE OFFSET
    // rather than by base vertex — see the draw loop's own note for why vertexOffset stays 0.
    struct MeshSectionDraw {
        std::uint32_t stream0ByteOffset = 0;
        std::uint32_t stream1ByteOffset = 0;  // meaningful IFF hasSkin
        bool hasSkin = false;
    };
    struct MeshSubmeshDraw {
        std::uint32_t sectionIndex = 0;
        std::uint32_t firstIndex = 0;  // ABSOLUTE, in index units, into the file's single index region
        std::uint32_t indexCount = 0;
        // Copied VERBATIM from the cooked submesh. Resolving it to a MaterialHandle stays the
        // CALLER's job (3.4.1's posture: a material is registered by whoever loaded the .aeromat),
        // so the registry stores the number and never interprets it.
        std::uint32_t materialIndex = 0;
        // task 3.6.1 -- CookedSubmesh::bounds via toAabb, VERBATIM (the materialIndex posture): the
        // registry stores the file's numbers and never re-folds them. May be the inverted empty
        // sentinel from a hand-built or corrupt file -- instanceBounds returns it as-is and draw()
        // culls it, which is the right answer for a submesh with no geometry.
        Aabb bounds{};
    };
    struct MeshEntry {
        rhi::BufferHandle vertexBuffer;  // stream 0 — 48-byte MeshVertex, every section concatenated
        rhi::BufferHandle skinBuffer;    // stream 1 — 32-byte SkinVertex, skinned sections only; invalid when none
        rhi::BufferHandle indexBuffer;   // the file's index region, uploaded VERBATIM
        rhi::IndexType indexType = rhi::IndexType::Uint16;  // a Uint16 file draws 16-bit
        std::vector<MeshSectionDraw> sections;
        std::vector<MeshSubmeshDraw> submeshes;
    };

    // The four pipelines are built before the renderer exists (create() owns that sequence), so they
    // arrive here rather than being assigned afterwards — which is what makes the destructor the
    // failure path for everything created past this point.
    ForwardRenderer(rhi::Device* device, rhi::GraphicsPipelineHandle pipeline,
                    rhi::GraphicsPipelineHandle pipelineCullNone, rhi::GraphicsPipelineHandle pipelineSkinned,
                    rhi::GraphicsPipelineHandle pipelineSkinnedCullNone) noexcept;
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
    // Releases an entry's up-to-three buffers (task 3.5.1). Shared by createMesh's failure path,
    // destroyMesh and destroyAll, so "which buffers a mesh owns" is stated exactly once.
    void destroyMeshBuffers(const MeshEntry& entry) noexcept;
    // One bindFragmentSamplers call for all five slots (task 3.4.1), resolving every invalid texture
    // handle to its built-in default at BIND time. Called on material change only, from draw().
    void bindMaterialTextures(rhi::RenderPassHandle pass, const MaterialSlot& slot);
    // task 3.6.1 -- the instance's LOCAL-space box, or nullopt for "cannot prove anything about
    // this instance". SILENT on every path: a nullopt is not an error report, and the arms below
    // own the latched WARNs for the cases that produce one. Mirrors the draw loop's own resolution
    // order, so the two can never disagree about which instance is which.
    [[nodiscard]] std::optional<Aabb> instanceBounds(const MeshInstance& instance) const;

    // task 3.6.2 (D9) -- draw()'s four resolution arms, factored so the depth pass reaches the same
    // decisions rather than carrying a copy that could drift.
    enum class InstanceDrawStatus : std::uint8_t {
        Primitive,     // ARM 1 -- no registered mesh; draw the built-in `primitive`
        Mesh,          // the registered path resolved; entry/submesh/section are non-null
        StaleMesh,     // ARM 2a -- draw() owns warnedStaleMesh
        SubmeshRange,  // ARM 2b -- draw() owns warnedSubmeshRange
        SectionRange,  // ARM 2c -- SILENT in draw() too; the guard is unreachable through createMesh
        SkinningCap,   // ARM 4  -- draw() owns warnedSkinningCap
    };

    struct ResolvedInstanceDraw {
        InstanceDrawStatus status = InstanceDrawStatus::StaleMesh;
        const MeshEntry* entry = nullptr;          // non-null iff status == Mesh
        const MeshSubmeshDraw* submesh = nullptr;  // non-null iff status == Mesh
        const MeshSectionDraw* section = nullptr;  // non-null iff status == Mesh
        bool skinned = false;                      // section->hasSkin && !instance.palette.empty()
        bool strayPalette = false;                 // a palette on a section with NO skin stream -- a WARN that does
                                                   // NOT skip; draw() owns warnedStrayPalette and the shadow pass
                                                   // deliberately ignores it (one owner per diagnostic)
    };

    // SILENT on every path -- "silent" means DOES NOT LOG. Returning a reason is not logging, and it
    // is what lets draw() keep every latched WARN it has always owned while the shadow loop reaches
    // the same four decisions without firing anything twice. instanceBounds' own posture, one layer
    // up.
    [[nodiscard]] ResolvedInstanceDraw resolveInstanceDraw(const MeshInstance& instance) const;

    // task 3.6.2 -- the depth pass's texture, sampler and two pipelines. Called by create() after the
    // renderer exists, so a half-built set unwinds through the destructor with no bookkeeping.
    [[nodiscard]] bool createShadowResources(const VirtualFileSystem& shaderVfs, const ForwardRendererConfig& config);

    // task E.1.4 -- the mask pass's four pipelines. Called by create() beside createShadowResources,
    // AFTER the renderer exists, so a half-built set unwinds through the destructor with no
    // bookkeeping. IT LOADS ITS OWN THREE SHADERS: create() destroys vs/vsSkinned/fs BEFORE the
    // ForwardRenderer object is constructed, so the handles it built the forward pipelines from are
    // already dead by the time this runs. createShadowResources does exactly the same for its three.
    [[nodiscard]] bool createSelectionMaskResources(const VirtualFileSystem& shaderVfs,
                                                    const ForwardRendererConfig& config);

    rhi::Device* device = nullptr;                   // non-owning; outlives the ForwardRenderer (contract)
    rhi::GraphicsPipelineHandle pipeline{};          // CullMode::Back — the engine convention
    rhi::GraphicsPipelineHandle pipelineCullNone{};  // the doubleSided twin, same two shaders
    // task 3.5.1 — the same two, built from the SKINNED vertex shader and the same fragment stage:
    // two vertex buffer layouts and six attributes instead of one and four. Four pipelines total,
    // from three shader handles, all destroyed after creation exactly as the pair always was.
    rhi::GraphicsPipelineHandle pipelineSkinned{};
    rhi::GraphicsPipelineHandle pipelineSkinnedCullNone{};
    std::array<PrimitiveMesh, static_cast<std::size_t>(PrimitiveId::Count)> primitives{};
    // The three built-in 1x1 defaults, INDEXED BY MaterialDefaultTextureKind — never by slot, and
    // never as three separately named members. Five slots map onto these three through
    // material.hpp's defaultTextureKindForSlot, which is the single place that mapping is decided;
    // the alternative (three names plus a five-entry table of them inside the .cpp) put the same
    // decision in two places, where a swapped pair renders a loud visual defect that no automated
    // case in this tree can see.
    std::array<rhi::TextureHandle, MATERIAL_DEFAULT_TEXTURE_KIND_COUNT> defaultTextures{};
    SlotMap<MaterialSlot, Material> materials;
    SlotMap<MeshEntry, MeshTag> meshes;  // task 3.5.1 — the same generational shape as `materials`
    // ...and, beside it, the live handles. SlotMap deliberately exposes no iteration and engine/core
    // is byte-identical this task, so this is what lets destroyAll() release every registered mesh's
    // buffers. It is written in exactly two places (createMesh appends, destroyMesh erases) and read
    // in one (destroyAll), and it is a linear vector for the same reason samplerCache is: the bound
    // is a program's live mesh count. Materials need no equivalent — a material owns no GPU resource
    // of its own, which is why the registry beside it has never had to be walked.
    std::vector<MeshHandle> liveMeshes;
    std::vector<std::pair<rhi::SamplerDesc, rhi::SamplerHandle>> samplerCache;  // linear scan; tiny
    MaterialHandle defaultMaterialHandle{};
    // The palette staging buffer: 255 rows x 16 bytes = 4080. Renderer-owned and ZEROED before every
    // skinned draw, so the FULL block is pushed every time (INV-S5) and no backend's partial-cbuffer
    // semantics are ever exercised — a shorter push would be legal on one backend and read stale ring
    // bytes on another, which is exactly the class of divergence this task refuses to introduce.
    std::array<Vec4, 3ULL * MAX_SKINNING_JOINTS> paletteScratch{};
    std::size_t skinnedDraws = 0;
    std::size_t pipelineBinds = 0;         // every bindGraphicsPipeline draw() issues, renderer lifetime
    std::size_t lastDrawn = 0;             // task 3.6.1 -- PER-FRAME; reset at the top of draw()
    std::size_t lastCulled = 0;            // task 3.6.1 -- PER-FRAME; reset at the top of draw()
    std::size_t materialBinds = 0;         // task 3.6.1 -- material-change blocks run, renderer lifetime
    bool warnedBlendOnce = false;          // D9's latch: once per renderer lifetime, never per frame
    bool warnedDroppedAttributes = false;  // task 3.5.1 — TexCoord1/Color0 dropped at repack, latched once
    bool warnedStaleMesh = false;          // an instance named a MeshHandle the registry no longer holds
    bool warnedSubmeshRange = false;       // an instance's submesh index is past the mesh's table
    bool warnedSkinningCap = false;        // a palette longer than MAX_SKINNING_JOINTS was refused
    bool warnedStrayPalette = false;       // a palette on a mesh section that carries no skin stream
    bool warnedDegenerateFrustum = false;  // task 3.6.1 -- a viewProj with no usable frustum, latched once
    // task E.1.4 -- draw()'s OWN resolved cull pair, carried to renderSelectionMask so the mask
    // mirrors the forward pass's frustum decision with the SAME frustum and the SAME gate rather than
    // a second extraction that would be free to disagree with it. PER-FRAME, reset at the top of
    // draw() and written where draw() finishes resolving them (after the degenerate-projection
    // disable), so a view with no camera -- which draws nothing and culls nothing -- leaves culling
    // OFF here. That is the SAFE direction: the mask can never drop an instance the forward pass drew.
    Frustum lastViewFrustum{};
    bool lastViewCulling = false;
    // task 3.6.2 — the depth pass's own resources. The texture ALWAYS exists (a 1x1 depth
    // placeholder when shadowResolution == 0), because SPIRV-Cross emits depth2d<float> for the
    // comparison slot and binding an RGBA8 default there is a Metal type mismatch.
    rhi::TextureHandle shadowTexture{};
    rhi::SamplerHandle shadowSampler{};  // renderer-owned; NOT in samplerCache (enableCompare)
    rhi::GraphicsPipelineHandle shadowPipeline{};
    rhi::GraphicsPipelineHandle shadowPipelineSkinned{};
    rhi::TextureFormat shadowFormat = rhi::TextureFormat::Invalid;
    std::uint32_t shadowResolution = 0;   // the CLAMPED value; 0 == off
    std::size_t lastShadowDrawn = 0;      // PER-FRAME; reset at the top of renderShadowMap
    std::size_t lastShadowCulled = 0;     // PER-FRAME; reset at the top of renderShadowMap
    std::size_t shadowPasses = 0;         // command buffers acquired for a depth pass, lifetime
    bool warnedShadowFit = false;         // the fit failed at least once, latched
    bool warnedShadowMaskCaster = false;  // a Mask/Blend material cast an opaque silhouette (D13)
    // task E.1.4 -- the SELECTION MASK pass's own resources. The texture is created LAZILY on the
    // first pass, because its extent is not known until the caller names one.
    rhi::GraphicsPipelineHandle selectionMaskPipeline{};                 // static,  CullMode::Back
    rhi::GraphicsPipelineHandle selectionMaskPipelineCullNone{};         // static,  CullMode::None
    rhi::GraphicsPipelineHandle selectionMaskPipelineSkinned{};          // skinned, CullMode::Back
    rhi::GraphicsPipelineHandle selectionMaskPipelineSkinnedCullNone{};  // skinned, CullMode::None
    rhi::TextureHandle selectionMaskTexture{};                           // R8Unorm; created LAZILY on the first pass
    rhi::Extent2D selectionMaskAllocExtent{};                            // == the extent it was last told about (INV-2)
    std::size_t selectionMaskPasses = 0;                                 // command buffers acquired, renderer lifetime
    std::size_t lastSelectionMaskDrawn = 0;       // PER-FRAME; reset at the top of renderSelectionMask
    bool warnedSelectionMaskCaster = false;       // a Mask/Blend material masked as a solid quad (D6)
    bool warnedSelectionMaskUnavailable = false;  // pipelines unbuilt or the texture could not be made
};

}  // namespace engine::render

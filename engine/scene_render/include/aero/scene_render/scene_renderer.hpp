#pragma once
// Aero Engine — engine::scene_render (task 1.4.1): the World -> render bridge. This is the ONLY code
// in the tree that sees BOTH engine::scene and engine::render — it sits ABOVE both (docs/03), which is
// what keeps `render` scene-free and `scene` GPU-free (D1). Two pieces:
//   * buildRenderView — a PURE, GPU-free free function (D7) that walks a World and resolves it into a
//     render::RenderView: instances from each<Transform, MeshRenderer>, one camera (lowest entity
//     index, D5), one directional + up to MAX_POINT_LIGHTS point lights + up to MAX_SPOT_LIGHTS
//     spot lights (D6; task E.2.2 for the spots), and the scene's
//     Environment (task E.2.1: lowest index, defaults when absent) -- which replaced the small fixed
//     ambient this line used to describe.
//     Tier-0 unit-testable with no GPU.
//   * SceneRenderer — the facade: owns a render::ForwardRenderer, a render::SkyPass (task E.2.1) and
//     reused scratch storage; one call per frame (render(World&, Frame&)) builds the view and draws
//     it -- sky first, then the geometry over it -- turning buildRenderView's diagnostic counts into
//     latched WARNs (once per SceneRenderer lifetime, never per frame).
//
// The component headers (Transform/Camera/Light/MeshRenderer) are .cpp includes only — this public
// header names just World + render types (spec 3.5). rhi::Device is forward-declared, exactly as
// render's own headers do; create()'s .cpp includes device.hpp.

#include <aero/render/render.hpp>  // Frame, ForwardRenderer, RenderView, MeshInstance, PointLightData, rhi::Extent2D
#include <aero/scene/world.hpp>    // World, Entity
#include <aero/scene_render/asset_bindings.hpp>  // task 3.1.5 — AssetBindingTable, MeshBinding

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace engine::rhi {
class Device;  // forward-declared: create() takes it by ref; scene_renderer.cpp includes device.hpp
}  // namespace engine::rhi

namespace engine {
class VirtualFileSystem;  // forward-declared: create() takes it by ref; scene_renderer.cpp includes vfs.hpp
}  // namespace engine

namespace engine::scene_render {

// Reusable scratch storage so buildRenderView allocates nothing after warm-up (the vectors keep their
// capacity across calls). Owned by the caller — SceneRenderer keeps one internally.
struct RenderViewScratch {
    std::vector<render::MeshInstance> instances;
    std::vector<render::PointLightData> points;
    std::vector<render::SpotLightData> spots;  // task E.2.2 -- its own vector, its own budget
};

// PURE, GPU-free (D7): walks `world`, resolves the camera + lights + renderable instances into
// `scratch`, and returns a RenderView VIEWING it. The returned view's instances/points spans point
// INTO `scratch` — valid only while `scratch` outlives the view and is not re-used for another call
// (a second buildRenderView invalidates a prior view's spans). Non-const World& because World::each<>
// is non-const (it only reads the bound components in practice). `viewport` (typically
// Frame::extent()) supplies the projection aspect ratio. Camera/light policy: D5/D6 — 0 cameras
// yields !hasCamera (nothing drawn, the clear still shows); >1 camera/directional picks the lowest
// entity index; point lights beyond MAX_POINT_LIGHTS are dropped, in iteration order, and spot lights
// beyond MAX_SPOT_LIGHTS are dropped in iteration order with their OWN flag (spotsTruncated, task
// E.2.2) -- the two budgets are independent. The diagnostic counts
// (cameraCount/directionalCount/pointsTruncated/spotsTruncated) are always filled —
// SceneRenderer::render turns them into latched WARNs; tier-0 tests assert them directly.
//
// `cameraOverride` (task 2.3.1): when non-null, it REPLACES the World's camera entirely — view, proj
// AND eyePosition, all three fields, by whole-struct assignment — and every MeshInstance::mvp is
// built from ITS view-projection; `viewport` is then not consulted for aspect at all (the caller has
// already resolved it, e.g. from its own render target's drawn sub-rect). The scene walk (instances,
// cameraCount, the light walk) is UNCHANGED and UNAFFECTED by the override — `view.cameraCount` stays
// informational either way, which is what a future Game view (Phase 4) will want. When null (the
// default), behaviour is unchanged in EVERY observable respect, including the 0-camera early return
// that skips the light walk.
//
// `bindings` (task 3.1.5): the resolution table for MeshRenderer::mesh / ::material. DEFAULTED AND
// LAST, so every caller written before 3.1.5 -- both samples and every existing test -- compiles and
// behaves IDENTICALLY (INV-D3). With `bindings == nullptr` the walk is byte-equivalent to the
// pre-3.1.5 walk for every input: an entity whose `mesh` is NIL takes the primitive path statement for
// statement, and an entity whose `mesh` is VALID emits nothing and adds one to
// RenderView::unresolvedMeshes -- a state no pre-3.1.5 input can reach, since MeshRenderer had no
// `mesh` field to fill. A null table and a missing entry are NOT errors: they are the ordinary
// in-flight state between a drop and the editor ledger's upload, which is why they are COUNTED and
// never warned.
[[nodiscard]] render::RenderView buildRenderView(World& world, RenderViewScratch& scratch, rhi::Extent2D viewport,
                                                 const render::CameraView* cameraOverride = nullptr,
                                                 const AssetBindingTable* bindings = nullptr);

// task E.1.4 -- reusable scratch so the walk allocates nothing after warm-up (RenderViewScratch's own
// contract). Owned by the CALLER; cleared on entry to every buildSelectionMaskSet call.
struct SelectionMaskScratch {
    std::vector<render::MeshInstance> secondary;
    std::vector<render::MeshInstance> primary;
    std::vector<Entity> withoutGeometry;
};

// The three spans VIEW `scratch` and are valid only while it outlives them and is not re-used for
// another call -- RenderView's own span rule, verbatim.
struct SelectionMaskSet {
    std::span<const render::MeshInstance> secondary;
    std::span<const render::MeshInstance> primary;
    // Selected entities that produced NO instance at all: no Transform, no MeshRenderer, an
    // unresolvable mesh GUID, or a meshIndex matching zero submeshes. THE MARKER LIST, and the ONLY
    // source for it (D11) -- deriving it a second way lets an entity be neither outlined nor markered
    // on any frame the two disagree, which is visually "unselected" with every automated observable
    // green.
    std::span<const Entity> withoutGeometry;
    std::uint32_t skippedOverCap = 0;    // selected entities past `entityCap`; neither outlined nor markered
    std::uint32_t unresolvedMeshes = 0;  // informational; the editor's ledger owns the user-facing message
    std::uint32_t unresolvedMaterials = 0;
};

inline constexpr std::size_t DEFAULT_SELECTION_MASK_ENTITY_CAP = 256;

// PURE, GPU-free (buildRenderView's D7 posture). Walks `selected` in SELECTION ORDER, resolving each
// entity through the SAME three arms buildRenderView uses -- which is why this lives in
// scene_renderer.cpp beside it rather than in a file of its own.
//
// Per entity, capped at `entityCap`:
//   * dead / null handle               -> skipped SILENTLY, and the cap counter does NOT advance
//                                         (2.3.2's A7 rule, verbatim: a stale handle must not consume
//                                         another entity's budget)
//   * no Transform, or no MeshRenderer -> withoutGeometry. THE Transform CLAUSE IS NOT DEFENSIVE:
//                                         buildRenderView walks each<Transform, MeshRenderer>, so such
//                                         an entity is NOT DRAWN by the forward pass and must get a
//                                         marker rather than an outline that can never appear
//   * MeshRenderer with an INVALID mesh -> ONE primitive instance
//   * a resolved binding               -> one instance per submesh matching `meshIndex`
//   * a reference nothing resolves, or zero matching submeshes -> withoutGeometry, ++unresolvedMeshes
//
// `palette` is left EMPTY on every instance, and that is a statement about the tree rather than an
// omission: nothing in scene_render or editor/ fills one (git grep computeJointPalette reaches only
// the two samples), so a skinned mesh draws in BIND POSE in the viewport and a bind-pose mask is
// exactly the right mask. The day a skinning path reaches this bridge, filling it here is the ONLY
// change the outline needs.
//
// `camera` is the view the caller is about to render, by value semantics: every instance's mvp is
// camera.proj * camera.view * model. MUTATES NEITHER the World NOR the Selection, and specifically
// does NOT prune -- that is 2.2.1's job, done by the Hierarchy at the top of ITS onDraw.
//
// `entityCap` is the EDITOR's cap, passed in rather than duplicated (D15): the editor passes
// MAX_HIGHLIGHTED_ENTITIES, so the marker path and the mask path cannot cap at different numbers.
// INSTANCES are not capped -- one entity with seven submeshes emits seven, because the forward pass
// has no instance cap either and inventing one here would make the mask disagree with the picture for
// exactly the models most likely to be selected.
[[nodiscard]] SelectionMaskSet buildSelectionMaskSet(World& world, std::span<const Entity> selected, Entity primary,
                                                     const render::CameraView& camera, SelectionMaskScratch& scratch,
                                                     const AssetBindingTable* bindings = nullptr,
                                                     std::size_t entityCap = DEFAULT_SELECTION_MASK_ENTITY_CAP);

// The scene's ACTIVE directional light: the lowest entity index carrying a DirectionalLight, which is
// the one buildRenderView lights the scene with (D6's rule, unchanged since 1.4.1). An INVALID Entity
// means the scene has none.
//
// IT EXISTS BECAUSE THE EDITOR CANNOT ASK RenderView. render::RenderView is scene-free by the golden
// rule and can never carry an engine::Entity, so the viewport's "which directional light did the
// bridge ignore?" question had no answer that was not a SECOND resolution -- and a second walk with a
// different iteration mechanism (each<T> here, eachEntity in the editor) is exactly how
// buildSelectionMaskSet's D11 says a tie-break drifts. buildRenderView CALLS THIS, so the two cannot
// disagree BY CONSTRUCTION rather than by review.
//
// Non-const World& for World::each<>'s sake, like buildRenderView. Emits NO log record on any path,
// and does NOT touch RenderView::directionalCount -- that diagnostic is per-WALK, not per-winner.
//
// task E.2.4: resolveDirectionalLight below calls this, so there are now three readers of one
// tie-break and still exactly one tie-break.
[[nodiscard]] Entity activeDirectionalLight(World& world);

// task E.2.4: the scene's ACTIVE directional light, RESOLVED -- the walk buildRenderView runs,
// extracted so a SECOND reader (the editor's material preview) gets the bridge's OWN answer rather
// than a second resolution that drifts. That is buildSelectionMaskSet's D11 recorded verbatim:
// a second walk with a different iteration mechanism is exactly how a tie-break stops agreeing.
// activeDirectionalLight's tie-break is the ONLY tie-break -- this CALLS it.
//
// `data.intensity == 0` AND an INVALID `entity` both mean "the scene has none" -- DirectionalLightData's
// own encoding, unchanged since 1.4.1 -- and a default-constructed ResolvedDirectionalLight IS that
// state, which is what makes the extraction behaviour-free for a world with no sun.
// `count` is EVERY DirectionalLight in the world, the per-WALK diagnostic RenderView::directionalCount
// carries; it describes the scene, not the winner.
//
// EMITS NO LOG RECORD ON ANY PATH. The latched "multiple DirectionalLights" WARN stays
// SceneRenderer::render's, so the editor's SECOND call per frame produces no second WARN.
// Non-const World& for World::each<>'s sake, like buildRenderView and activeDirectionalLight.
struct ResolvedDirectionalLight {
    render::DirectionalLightData data{};
    Entity entity{};
    std::uint32_t count = 0;
};
[[nodiscard]] ResolvedDirectionalLight resolveDirectionalLight(World& world);

// task E.2.4: the scene's Environment, RESOLVED -- lowest entity index wins (D5's rule, as for the
// camera, the directional light and the listener), both SELECTORS clamped through
// render::clampBackgroundMode / clampAmbientMode (the clampPrimitive rule, so a hand-edited 7 renders
// mode 0 rather than reinterpreting a byte). With none: `entity` invalid, `count` 0, and
// `data == EnvironmentData{}` -- which is the value RenderView::environment already defaults to, and
// is why the extraction is behaviour-free there too. ZERO IS NOT A DIAGNOSTIC (E.2.1): a world without
// one is the ordinary state of every scene authored before that task. Same log-free posture as above.
struct ResolvedEnvironment {
    render::EnvironmentData data{};
    Entity entity{};
    std::uint32_t count = 0;
};
[[nodiscard]] ResolvedEnvironment resolveEnvironment(World& world);

// Room for future knobs (ambient override, max lights, ...); v0 uses defaults.
struct SceneRendererConfig {};

// The facade: owns a render::ForwardRenderer + a render::SkyPass + reused scratch. One call per frame
// draws the whole World. Move-only (its two non-trivial members, ForwardRenderer and SkyPass, both
// have user-defined noexcept moves); copies deleted transitively.
class SceneRenderer {
public:
    // colorFormat/depthFormat must equal the target Renderer's renderer.colorFormat()/depthFormat() —
    // depthFormat == Invalid (a non-depth Renderer, D11) fails inside ForwardRenderer::create and
    // this returns nullopt (+ ERROR), the same as ForwardRenderer::create's own contract.
    [[nodiscard]] static std::optional<SceneRenderer> create(rhi::Device& device, const VirtualFileSystem& shaderVfs,
                                                             rhi::TextureFormat colorFormat,
                                                             rhi::TextureFormat depthFormat,
                                                             const SceneRendererConfig& config = {});

    ~SceneRenderer() = default;
    // task E.2.1: still DEFAULTED -- SkyPass, like ForwardRenderer, has USER-DEFINED noexcept moves,
    // so the two members' own moves do the transfer.
    SceneRenderer(SceneRenderer&&) noexcept = default;
    SceneRenderer& operator=(SceneRenderer&&) noexcept = default;
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // buildRenderView(world, internal scratch, frame.extent(), cameraOverride) + the latched D5/D6
    // WARNs + forward.draw (a no-op when the resolved view has no camera). `world` is non-const for
    // the same reason buildRenderView's is. Task 2.3.1: when `cameraOverride` is non-null, the two
    // CAMERA WARNs ("no Camera in world" / "multiple Cameras") are SUPPRESSED — the override makes
    // both moot — while the directional, point-light and spot-light WARNs are UNAFFECTED.
    void render(World& world, render::Frame& frame, const render::CameraView* cameraOverride = nullptr);

    // ---- task 3.1.5 -----------------------------------------------------------------------------
    // The owned ForwardRenderer. A MeshHandle and a MaterialHandle are PER-ForwardRenderer, so
    // whoever fills `bindings()` MUST mint its handles on THIS renderer -- that is the whole reason
    // this accessor exists, and it is what lets the editor's ledger own the upload without owning the
    // SceneRenderer.
    [[nodiscard]] render::ForwardRenderer& renderer() noexcept;
    [[nodiscard]] const render::ForwardRenderer& renderer() const noexcept;
    // ---- task E.2.1 -----------------------------------------------------------------------------
    // The owned SkyPass. It exists for the same reason renderer() does -- the pass's drawCount() and
    // its degenerate-camera latch are the only observables a test has for a pass that records into
    // someone else's frame, and SB16 reads both through here.
    [[nodiscard]] render::SkyPass& skyPass() noexcept;
    [[nodiscard]] const render::SkyPass& skyPass() const noexcept;
    // The resolution table render() threads into buildRenderView. Filled from outside; never cleared
    // here.
    [[nodiscard]] AssetBindingTable& bindings() noexcept;
    [[nodiscard]] const AssetBindingTable& bindings() const noexcept;
    // The two diagnostics of the LAST render(), LATCHED -- buildRenderView's RenderView does not
    // outlive that call. Zero until the first render(). Deliberately NOT WARNed: see render()'s own
    // comment and RenderView's.
    [[nodiscard]] std::uint32_t lastUnresolvedMeshes() const noexcept;
    [[nodiscard]] std::uint32_t lastUnresolvedMaterials() const noexcept;

private:
    // task E.2.1: BOTH by rvalue -- SkyPass has no default constructor, only create(), which is what
    // makes "a SceneRenderer without a sky pass" unrepresentable rather than merely untested. Private:
    // create() move-constructs.
    SceneRenderer(render::ForwardRenderer&& fwd, render::SkyPass&& skyPassValue) noexcept;

    render::ForwardRenderer forward;
    // task E.2.1 -- drawn between the shadow pass and the forward pass. The member name differs from
    // the accessor's for the same reason bindingTable's does (the note below).
    render::SkyPass sky;
    RenderViewScratch scratch;
    // The member names differ from the accessor names on purpose -- the house rule for an
    // accessor/member collision (AssetDatabase::records()/recordList, RenderTarget::depthFormat()/
    // depthFormatValue), never a trailing underscore.
    AssetBindingTable bindingTable;
    std::uint32_t lastUnresolvedMeshesValue = 0;
    std::uint32_t lastUnresolvedMaterialsValue = 0;
    bool noCameraWarned = false;
    bool multiCameraWarned = false;
    bool multiDirWarned = false;
    bool pointTruncWarned = false;
    bool spotTruncWarned = false;         // task E.2.2 -- >MAX_SPOT_LIGHTS SpotLights; a separate latch
    bool multiEnvironmentWarned = false;  // task E.2.1 -- >1 Environment; ZERO is deliberately silent
};

}  // namespace engine::scene_render

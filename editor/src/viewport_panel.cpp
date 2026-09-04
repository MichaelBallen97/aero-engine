// Aero Engine — the Viewport panel implementation (task 2.2.3), the only new ImGui TU this task
// adds. See viewport_panel.hpp for the two-phase contract (D3).
//
// Destruction order (AC-12): ~ViewportPanel runs inside ~PanelRegistry inside ~EditorApp, which
// precedes ~Device both in main.cpp's RAII-order comment and in the GPU test — so `target` and
// `sceneRenderer` (and the GPU objects they own) release before the Device does.
#include "viewport_panel.hpp"

#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/vfs.hpp>
#include <aero/editor/asset_drag.hpp>    // task 3.1.5: the payload, the decode and the routing matrix
#include <aero/editor/axis_palette.hpp>  // task E.1.2: AXIS_X_LINEAR / AXIS_Z_LINEAR
#include <aero/editor/command_stack.hpp>
#include <aero/editor/gizmo.hpp>
#include <aero/editor/picking.hpp>
#include <aero/editor/scene_bounds.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/transform_command.hpp>
#include <aero/editor/transform_ops.hpp>
#include <aero/rhi/internal/native_device.hpp>
#include <aero/scene/mesh_renderer.hpp>  // task 3.1.5: the Material arm asks the LIVE World

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <imgui_internal.h>  // task 3.1.5: ImRect + BeginDragDropTargetCustom (shell_ui.cpp's precedent)
#include <memory>
#include <optional>
#include <utility>

// <ImGuizmo.h> deliberately follows <imgui.h> and lives in its own trailing include block: it does
// NOT include imgui.h (it forward-declares ImGuiWindow, then names ImDrawList / ImVec2 / ImU32 /
// ImGuiContext), and SortIncludes: CaseSensitive would hoist 'I' above 'i'. The ordering is held
// STRUCTURALLY by .clang-format's ImGuizmo category (Priority 5), not by this comment.
#include <ImGuizmo.h>

namespace engine::editor {

namespace {

constexpr std::uint32_t VIEWPORT_EXTENT_QUANTUM = 64;
// ALPHA 1.0 IS LOAD-BEARING (E4): ImGui's pipeline alpha-blends (SrcAlpha/OneMinusSrcAlpha), so a
// 0-alpha clear would let the editor's chrome show THROUGH the viewport wherever no geometry drew.
// scene.frag.hlsl already writes alpha 1 (F19) -- the clear is the only alpha we can get wrong.
constexpr rhi::Color VIEWPORT_CLEAR_COLOR{0.06F, 0.06F, 0.07F, 1.0F};
constexpr ImVec4 OVERLAY_COLOR{0.7F, 0.7F, 0.75F, 0.8F};
constexpr float OVERLAY_INSET = 4.0F;
// task 3.6.3: the two view-option widget widths, chosen so the whole row (T R S Local + combo +
// slider) fits a 640-point viewport. NOT load-bearing -- a wrap at a narrow dock width is a
// cosmetic finding for the validation pass, not a correctness one.
constexpr float OPTIONS_COMBO_WIDTH = 92.0F;
constexpr float OPTIONS_SLIDER_WIDTH = 130.0F;

// task E.1.2: the grid's look. The GREYS are file-local (they are chosen against
// VIEWPORT_CLEAR_COLOR, which is also file-local); the AXIS colours are NOT -- E.3.1's Inspector
// rows and E.1.5's gizmo read the same two constants, which is what "sharing E.3.1's axis palette"
// means. E.6.1's EditorTheme is where all of them eventually live.
//
// NO DEPTH BIAS ACCOMPANIES THIS, and that is a measured result rather than an omission: a
// rasterizer depth bias does not apply to line primitives at all (D3D12 excludes points and lines
// outright, Metal says it influences triangles only, Vulkan permits it for lines but never
// guarantees it), and a sweep of 13 line depths x 5 magnitudes against a depth-writing quad moved
// nothing on any of them. So the grid cannot be pulled toward the camera by that knob, and a
// coplanar surface at y = 0 will z-fight it -- handed to E.5.2, which creates the first such
// surface. ensureInitialized's debug-draw creation call below is unchanged for the same reason.
[[nodiscard]] render::DebugGridStyle viewportGridStyle() noexcept {
    render::DebugGridStyle style{};
    style.axisXColor = AXIS_X_LINEAR;
    style.axisZColor = AXIS_Z_LINEAR;
    return style;
}

// NUL-terminated, because ImGui needs NUL-terminated text and render::tonemapOperatorLabel
// returns a std::string_view whose data() carries no such guarantee -- passing that straight to
// ImGui is the classic read-past-the-end. A switch over string LITERALS is the whole fix; the
// vocabulary is closed and tiny, so material_panel.cpp's copy-into-a-std::string scratch would be
// more machinery than the problem. TM2 pins the three literals on the engine side and I106 pins
// that the two agree.
[[nodiscard]] const char* tonemapOperatorLabelCStr(render::TonemapOperator op) {
    switch (op) {  // NO default: -- a fourth enumerator must be a -Wswitch error, never silent
        case render::TonemapOperator::None:
            return "None";
        case render::TonemapOperator::Reinhard:
            return "Reinhard";
        case render::TonemapOperator::AcesApprox:
            return "ACES";
        case render::TonemapOperator::Count:
            break;
    }
    return "Unknown";
}

// D6: colour lives HERE, never in the public header -- an ImU32 there would break the ImGui-free
// rule, which is exactly why buildSelectionOverlay tags a ROLE and the panel maps role -> style.
// The two THICKNESSES are tuning values, settled on a manual validation pass (editor/VALIDATION.md).
//
// task E.1.4: the two COLOURS are no longer stated here at all. They live in engine/render as
// SELECTION_OUTLINE_PRIMARY_DEFAULT / _SECONDARY_DEFAULT, because the GPU silhouette and the point
// marker must be the same amber and a second statement of it is a drift surface no test can see
// (D18). The multiply below is by 255.0F and the round is std::lround -- NEVER a reciprocal, because
// k * fl(1/255) is bit-unequal to fl(k/255) for 126 of the 256 byte values (E.1.1, measured) and this
// is a round trip. std::lround is not constexpr, so these cannot be namespace-scope constexpr ImU32
// the way the two literals they replace were; the conversion happens at the ONE call site below.
[[nodiscard]] ImU32 selectionImU32(Vec4 srgb) noexcept {
    const auto ch = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F)); };
    return IM_COL32(ch(srgb.x), ch(srgb.y), ch(srgb.z), ch(srgb.w));
}

// task E.1.4: the band's authored thickness, in POINTS. Resolved to an integer pixel radius so the
// band width is an exact integer and the shader's tap offsets are exact texel multiples. A 1x display
// gets a 2-pixel band and a 2x display a 4-pixel one, rather than a band that halves in apparent
// size. This is the FIRST overlay in the tree that can control its own apparent thickness: E.1.1's
// handoff table lists thick lines as unowned precisely because a LineList primitive has no width
// control on any backend. An edge-detect band does.
constexpr float SELECTION_OUTLINE_RADIUS_POINTS = 1.0F;
constexpr float SELECTION_THICKNESS = 1.0F;
constexpr float SELECTION_PRIMARY_THICKNESS = 2.0F;

// D7/E9: GetContentRegionAvail() is in LOGICAL units; allocation must be sized in PIXELS. A
// non-finite or non-positive scale falls back to 1.0 (the framePaceSleepMs NaN-safe idiom,
// editor_app.cpp:29) -- spelled with the negated `>` so NaN takes the fallback branch.
[[nodiscard]] std::uint32_t toPixels(float logical, float scale) noexcept {
    const float safeScale = (scale > 0.0F) ? scale : 1.0F;
    const long rounded = std::lround(static_cast<double>(logical) * static_cast<double>(safeScale));
    return rounded < 1 ? 1U : static_cast<std::uint32_t>(rounded);
}

// The shared "nothing to show" path (D11/D12): a centred one-line message, no image, no request.
void drawUnavailableMessage(const char* reason) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 textSize = ImGui::CalcTextSize(reason);
    const ImVec2 cursor = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(cursor.x + (avail.x - textSize.x) * 0.5F, cursor.y + (avail.y - textSize.y) * 0.5F));
    ImGui::TextUnformatted(reason);
}

// Task 2.3.3 -- the ONLY place our enums touch ImGuizmo's. Both are total over the enum with NO
// default: (adding a GizmoOperation enumerator is caught by clang-tidy's
// bugprone-switch-missing-default-case, the DOCK_SLOT_COUNT discipline in its switch form).
// ImGuizmo::OPERATION and ImGuizmo::MODE are UNSCOPED enums (ImGuizmo.h:188,217), so the
// enumerators are spelled bare (ImGuizmo::TRANSLATE, ImGuizmo::LOCAL, ...).
[[nodiscard]] ImGuizmo::OPERATION toImGuizmoOperation(GizmoOperation op) noexcept {
    ImGuizmo::OPERATION result = ImGuizmo::TRANSLATE;
    switch (op) {
        case GizmoOperation::Translate:
            result = ImGuizmo::TRANSLATE;
            break;
        case GizmoOperation::Rotate:
            result = ImGuizmo::ROTATE;
            break;
        case GizmoOperation::Scale:
            result = ImGuizmo::SCALE;
            break;
    }
    return result;
}

[[nodiscard]] ImGuizmo::MODE toImGuizmoMode(GizmoSpace space) noexcept {
    ImGuizmo::MODE result = ImGuizmo::WORLD;
    switch (space) {
        case GizmoSpace::Local:
            result = ImGuizmo::LOCAL;
            break;
        case GizmoSpace::World:
            result = ImGuizmo::WORLD;
            break;
    }
    return result;
}

}  // namespace

ViewportPanel::ViewportPanel(rhi::Device& deviceIn) noexcept : device(&deviceIn) {}

const char* ViewportPanel::id() const noexcept { return "Viewport"; }
DockSlot ViewportPanel::defaultDockSlot() const noexcept { return DockSlot::Center; }
PanelOptions ViewportPanel::options() const noexcept {
    return {.noScrollbar = true, .noPadding = true, .noScrollWithMouse = true};
}

EditorCamera& ViewportPanel::camera() noexcept { return editorCamera; }
const EditorCamera& ViewportPanel::camera() const noexcept { return editorCamera; }

// ---- task 3.1.5 ----------------------------------------------------------------------------------
render::ForwardRenderer* ViewportPanel::sceneForwardRenderer() noexcept {
    return sceneRenderer ? &sceneRenderer->renderer() : nullptr;
}

scene_render::AssetBindingTable* ViewportPanel::sceneAssetBindings() noexcept {
    return sceneRenderer ? &sceneRenderer->bindings() : nullptr;
}

// ---- task 3.6.3 ----------------------------------------------------------------------------------
const render::PostProcess* ViewportPanel::postProcess() const noexcept { return post ? &*post : nullptr; }

// ---- task E.1.1 ----------------------------------------------------------------------------------
render::DebugDraw* ViewportPanel::debugDraw() noexcept { return debugDrawer ? &*debugDrawer : nullptr; }
const render::DebugDraw* ViewportPanel::debugDraw() const noexcept { return debugDrawer ? &*debugDrawer : nullptr; }

// THE ARM-TIME DECISION, and it is a GEOMETRIC test on purpose -- see the header for the regression
// this replaces. Reads the rect the LAST DRAWN FRAME recorded (step 9b), which is what makes it
// answerable at step 8b, before this frame's strip has been submitted.
//
// An EMPTY rect owns nothing: that is the first frame, and any frame that returned before step 9b.
// The negated `>` is the file's own NaN-safe idiom (a non-finite extent takes the "owns nothing"
// branch), and the half-open `< max` matches the FIRE step's own image-rect test one screen below.
bool ViewportPanel::overlayOwnsPress(Vec2 pressPoints) const noexcept {
    if (!(overlayRowBottomRight.x > overlayRowTopLeft.x) || !(overlayRowBottomRight.y > overlayRowTopLeft.y)) {
        return false;
    }
    return pressPoints.x >= overlayRowTopLeft.x && pressPoints.x < overlayRowBottomRight.x &&
           pressPoints.y >= overlayRowTopLeft.y && pressPoints.y < overlayRowBottomRight.y;
}

const render::RenderTarget* ViewportPanel::outputTarget() const noexcept { return target ? &*target : nullptr; }

std::uint32_t ViewportPanel::lastUnresolvedMeshes() const noexcept {
    return sceneRenderer ? sceneRenderer->lastUnresolvedMeshes() : 0U;
}

std::uint32_t ViewportPanel::lastUnresolvedMaterials() const noexcept {
    return sceneRenderer ? sceneRenderer->lastUnresolvedMaterials() : 0U;
}

Entity ViewportPanel::pickAt(const World& world, Vec2 ndc) const {
    // ONE pick, THIS panel's state, no ImGui: the accept-time peek and tick()'s drain both come here,
    // so a drop that was offered and a drop that is applied can never disagree about which entity was
    // under the cursor. `lastImageSizePoints` is the last DRAWN image rect; a panel that has never
    // drawn has a zero size, which pickEntity handles as any degenerate viewport (no point hit).
    const PickRequest request{
        .ndc = ndc, .aspect = lastAspect, .viewportSizePoints = lastImageSizePoints, .meshBounds = meshBounds};
    return pickEntity(world, editorCamera, request).entity;
}

void ViewportPanel::acceptViewportAssetDrop(PanelContext& context, Vec2 imageOrigin, Vec2 avail) {
    const ImGuiPayload* const payload = ImGui::GetDragDropPayload();  // PEEK -- no accept yet
    if (payload == nullptr || !payload->IsDataType(ASSET_PAYLOAD_TYPE)) {
        return;
    }
    const std::optional<AssetDragPayload> asset = decodeAssetDragPayload(payload->Data, payload->DataSize);
    if (!asset.has_value()) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const Vec2 ndc = viewportNdc(Vec2{io.MousePos.x, io.MousePos.y}, imageOrigin, avail);
    const auto kind = static_cast<AssetKind>(asset->kind);
    // A Material payload needs to know whether the entity UNDER THE CURSOR has a MeshRenderer, so the
    // peek runs a real pick, at the hover position, this frame. One pick per frame during a drag; this
    // panel already pays one per click.
    bool targetHasMesh = false;
    if (kind == AssetKind::Material) {
        const Entity hit = pickAt(context.world, ndc);
        targetHasMesh = hit.valid() && context.world.has<MeshRenderer>(hit);
    }
    if (classifyAssetDrop(kind, DropSurface::Viewport, targetHasMesh) == DropAction::None) {
        return;  // REFUSED AT PEEK: AcceptDragDropPayload is never called, so ImGui draws no highlight
    }
    if (ImGui::AcceptDragDropPayload(ASSET_PAYLOAD_TYPE) != nullptr) {
        // RECORD ONLY. Placement is resolved in tick(), from the same camera state the command sees.
        pendingAssetDrop = ViewportAssetDrop{.payload = *asset, .ndc = ndc};
    }
}

void ViewportPanel::ensureInitialized([[maybe_unused]] rhi::Extent2D firstExtent) {
    if (status != Status::Uninitialized) {
        return;
    }
    // `#if defined(...)` -- the exact spelling editor_reflection.cpp:5 already uses for
    // AERO_EDITOR_REFLECTION. Never `#if !AERO_EDITOR_SHADERS`: an undefined macro there is a
    // -Wundef trap, and matching the existing precedent keeps both degradation paths grep-able.
#if defined(AERO_EDITOR_SHADERS)
    shaderVfs.mount(std::make_unique<DirectoryBackend>(AERO_SHADERS_DIR));
    // task 3.6.3: `post` FIRST -- it owns the HDR target the scene renderer is then built against, so
    // the two are engaged together or neither is.
    post = render::PostProcess::create(*device, shaderVfs, firstExtent,
                                       {.outputColorFormat = rhi::TextureFormat::RGBA8Unorm,
                                        .outputDepthFormat = rhi::TextureFormat::Invalid,
                                        // task E.1.4: the mask pass reads this depth with
                                        // LoadOp::Load, and an unstored depth is GARBAGE on a tiler
                                        // rather than stale (D2).
                                        .sceneDepthStore = true,
                                        .quantum = VIEWPORT_EXTENT_QUANTUM});
    if (!post) {
        status = Status::Unavailable;
        unavailableReason = "post-process creation failed (are res://fullscreen.vert / res://tonemap.frag cooked?)";
        return;
    }
    // task 3.6.3: `.depth = false` -- nothing depth-tests into this target any more. The scene's depth
    // lives on the HDR target inside `post`; the only thing drawn here is a depth-off fullscreen
    // triangle.
    target = render::RenderTarget::create(
        *device, firstExtent,
        {.colorFormat = rhi::TextureFormat::RGBA8Unorm, .depth = false, .quantum = VIEWPORT_EXTENT_QUANTUM});
    if (!target) {
        post.reset();
        status = Status::Unavailable;
        unavailableReason = "render target creation failed";
        return;
    }
    // task 3.6.3: built against the HDR target's formats, NOT this panel's output target's -- that is
    // the seam, and PP5 asserts it is real rather than asserted.
    sceneRenderer =
        scene_render::SceneRenderer::create(*device, shaderVfs, post->sceneColorFormat(), post->sceneDepthFormat());
    if (!sceneRenderer) {
        target.reset();
        post.reset();
        status = Status::Unavailable;
        unavailableReason = "scene renderer creation failed (are res://scene.vert/.frag cooked?)";
        return;
    }
    // task E.1.1: the debug batch and its four pipelines, built against the SAME HDR pair the
    // SceneRenderer was, so a line records into the scene pass with matching formats and is
    // depth-tested against the geometry that pass just drew. ALL-OR-NOTHING with the three above:
    // one attempt, latched, and a failure resets everything already made.
    debugDrawer = render::DebugDraw::create(
        *device, shaderVfs, {.colorFormat = post->sceneColorFormat(), .depthFormat = post->sceneDepthFormat()});
    if (!debugDrawer) {
        sceneRenderer.reset();
        target.reset();
        post.reset();
        status = Status::Unavailable;
        unavailableReason = "debug draw creation failed (are res://debug_line.* / res://debug_billboard.* cooked?)";
        return;
    }
    // task E.1.4: built against the OUTPUT target's formats, NOT the HDR pair -- this is the one GPU
    // object in the panel built against `target`, and the asymmetry is the point of this comment: the
    // outline composites into the already-tonemapped image, so it is editor chrome rather than scene
    // content. ALL-OR-NOTHING with the four above it, and the reset order is the reverse of creation.
    selectionOutline = render::SelectionOutline::create(
        *device, shaderVfs,
        {.outputColorFormat = target->colorFormat(), .outputDepthFormat = rhi::TextureFormat::Invalid});
    if (!selectionOutline) {
        debugDrawer.reset();
        sceneRenderer.reset();
        target.reset();
        post.reset();
        status = Status::Unavailable;
        unavailableReason = "selection outline creation failed (is res://selection_outline.frag cooked?)";
        return;
    }
    status = Status::Ready;
#else  // -DAERO_SHADER_TOOLS=OFF (D12)
    status = Status::Unavailable;
    unavailableReason = "Viewport unavailable — built without AERO_SHADER_TOOLS";
    AERO_LOG_WARN("editor: viewport disabled — built with -DAERO_SHADER_TOOLS=OFF (no cooked shaders)");
#endif
}

void ViewportPanel::onDraw(PanelContext& context) {
    // Step 1 (E1): a NaN-safe negated `>` so a degenerate/zero-area panel returns before ANYTHING —
    // no init, no resize, no Image, no request. Covers minimize, collapse and a zero-size dock node.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (!(avail.x > 0.0F) || !(avail.y > 0.0F)) {
        return;
    }

    // Step 2 (D7/E9): size the allocation in PIXELS.
    const ImGuiIO& io = ImGui::GetIO();
    const rhi::Extent2D pixels{toPixels(avail.x, io.DisplayFramebufferScale.x),
                               toPixels(avail.y, io.DisplayFramebufferScale.y)};
    // task E.1.4: captured HERE and read in renderScene, because renderScene must not call ImGui
    // (2.2.3 INV-3). The same value toPixels already reads, stored rather than recomputed.
    lastFramebufferScale = io.DisplayFramebufferScale.x;

    // Step 3 (D11): lazy, latched, one-attempt initialisation.
    ensureInitialized(pixels);

    // Step 4. The `!target` half is defensive (status == Ready is set only alongside a live target
    // in ensureInitialized) and is what lets every access below be a CHECKED optional access.
    if (status != Status::Ready || !target || !post || !debugDrawer || !selectionOutline) {
        drawUnavailableMessage(unavailableReason != nullptr ? unavailableReason : "Viewport unavailable");
        return;
    }

    // Step 5.
    // task 3.6.3: THE TWO RESIZES ARE ADJACENT and driven by the SAME `pixels`, which is what makes
    // the resolve's 1:1 blit true BY CONSTRUCTION rather than by two call sites that could drift.
    // Reallocating the HDR target inside the draw walk is safe and is NOT the I96 violation it
    // resembles: I96 is about ordering against ImGui's CONSUMPTION, and ImGui never sees this
    // texture -- only `target`'s, whose resize is the line below and must stay there.
    //
    // BOTH RESULTS ARE CHECKED AND EITHER FAILURE LATCHES. An earlier draft discarded the HDR
    // target's, on the theory that resolve() would then log once and the picture would fall back to
    // the clear colour. THAT IS FALSE, and it is false in three ways at once. renderScene returns at
    // its own `if (!sceneFrame)` -- ABOVE target->beginFrame, post->resolve and target->endFrame --
    // so the one latched diagnostic designed for a not-renderable pass is unreachable on this path.
    // RenderTarget::resize zeroes its allocation extent before allocate() can fail, so every
    // subsequent frame re-runs allocate() and re-emits its ERROR: once per frame, forever, with the
    // editor's err counter climbing. And target->resize (4 B/texel) can still SUCCEED where the
    // RGBA16Float pair (8 B/texel) failed, reallocating the very texture ImGui is about to sample --
    // which nothing then renders into, so ImGui::Image below would sample UNDEFINED CONTENT rather
    // than a clear colour. Latching is what the output target has always done here; the HDR one gets
    // the same treatment, and the panel says which of the two failed.
    const bool sceneResized = post->resize(pixels);
    const bool outputResized = target->resize(pixels);
    if (!sceneResized || !outputResized) {
        status = Status::Unavailable;
        unavailableReason = sceneResized ? "render target allocation failed" : "HDR scene target allocation failed";
        drawUnavailableMessage(unavailableReason);
        return;
    }

    // Step 6: the native handle, as void*. Never pass 0 to ImGui (F3) -- a null accessor result takes
    // the message path instead.
    void* const native = rhi::internal::NativeDeviceAccessor::texture(*device, target->colorTexture());
    if (native == nullptr) {
        drawUnavailableMessage("texture unavailable");
        return;
    }

    // Step 7: the UV sub-rect (D5/D6) -- textureExtent() >= drawExtent() always (INV-1), so uvMax is
    // in (0,1].
    const rhi::Extent2D drawExtent = target->drawExtent();
    const rhi::Extent2D textureExtent = target->textureExtent();
    const ImVec2 uvMax{static_cast<float>(drawExtent.width) / static_cast<float>(textureExtent.width),
                       static_cast<float>(drawExtent.height) / static_cast<float>(textureExtent.height)};

    // Step 8: ImTextureID is an ImU64 holding the raw native texture pointer (F1/F3, imgui.h:345 +
    // imgui_impl_sdlgpu3.cpp:33). A pointer-to-integer conversion is the only way to spell that.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto texId = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(native));
    const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
    ImGui::Image(texId, avail, ImVec2(0, 0), uvMax);

    // --- 8b. navigation (task 2.3.1). `hovered` is captured HERE, immediately after the Image and
    // BEFORE the overlay text, so it refers to the image item and not to a label drawn over it.
    // ImGui::Image submits its item with id 0, so IsItemHovered() works (ItemAdd sets HoveredRect
    // regardless of id) but SetItemKeyOwner does NOT (it returns false for id 0) -- the wheel is
    // claimed by ImGuiWindowFlags_NoScrollWithMouse in options() instead. See plan C1.
    const bool hovered = ImGui::IsItemHovered();
    lastImageSizePoints = Vec2{avail.x, avail.y};  // task 3.1.5: what pickAt needs, latched here

    // --- 8b-drop (task 3.1.5). BEFORE the gesture block, so a live payload suppresses the very
    // inputs computed just below it. While ANY drag payload is live the viewport is a DROP TARGET,
    // not an input surface -- the Hierarchy's own signal, applied here. Suppressing `hovered` for
    // the gesture, the pick and the F-focus guard is enough AND is the minimum: the drop target
    // itself tests the mouse against a RECT, not IsItemHovered, so it is unaffected.
    const bool dragActive = ImGui::GetDragDropPayload() != nullptr;
    const bool inputHovered = hovered && !dragActive;
    if (dragActive) {
        // A press that began on the image, dragged out to the browser and released would otherwise
        // leave the arm latched into the next frame.
        pickArmed = false;
        // ImGui::Image submits its item with id 0, so BeginDragDropTarget() cannot attach to it (the
        // SetItemKeyOwner note one floor up says why id 0 is special). BeginDragDropTargetCustom
        // takes the rect and an EXPLICIT id and consults no item state at all -- it needs
        // g.DragDropActive, this window to be the hovered dock-tree root, the mouse inside `bb`, and
        // a NON-ZERO id, which it IM_ASSERTs. The != 0 guard costs one comparison and turns a
        // would-be Debug-lane abort into "the target does not exist this frame".
        const ImRect imageRect(imageOrigin, ImVec2(imageOrigin.x + avail.x, imageOrigin.y + avail.y));
        const ImGuiID dropId = ImGui::GetID("##aero_asset_drop");
        if (dropId != 0 && ImGui::BeginDragDropTargetCustom(imageRect, dropId)) {
            acceptViewportAssetDrop(context, Vec2{imageOrigin.x, imageOrigin.y}, Vec2{avail.x, avail.y});
            ImGui::EndDragDropTarget();  // ONLY because BeginDragDropTargetCustom returned true
        }
    }

    const CameraGestureInput gestureInput{
        .hovered = inputHovered,
        .leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left),
        .rightDown = ImGui::IsMouseDown(ImGuiMouseButton_Right),
        .middleDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle),
        .leftPressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left),
        .rightPressed = ImGui::IsMouseClicked(ImGuiMouseButton_Right),
        .middlePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Middle),
        // F22b: io.KeyCtrl IS ALREADY "Ctrl on Windows/Linux, Cmd on macOS" (imgui.h). Do NOT write
        // `|| io.KeySuper` -- that would ALSO fire on physical Ctrl on macOS and on the Windows/Super
        // key elsewhere.
        .alt = io.KeyAlt,
        .ctrlOrCmd = io.KeyCtrl};
    gesture = nextGesture(gesture, gestureInput);

    const bool flying = gesture.gesture == CameraGesture::Fly;
    // C6: the KEY reads are gated on !io.WantTextInput, exactly like F below -- ImGui::IsKeyDown polls
    // with ImGuiKeyOwner_Any and is blind to a focused InputText, so without this a rename in the
    // Hierarchy would both type `wasd` and fly the camera. The MOUSE half is deliberately NOT gated: a
    // drag must keep working while a field has focus.
    const bool keysLive = flying && !io.WantTextInput;
    const CameraInput cameraInput{
        .dragDelta = (gesture.gesture != CameraGesture::None) ? Vec2{io.MouseDelta.x, io.MouseDelta.y} : Vec2::zero(),
        // ours when the image is hovered, and STILL ours mid-fly even if the cursor wandered out of
        // the panel (the drag stays alive under mouse capture, so the speed control must follow it)
        .wheelNotches = (hovered || flying) ? io.MouseWheel : 0.0F,
        .viewportHeightPoints = avail.y,  // POINTS (D15) -- NEVER drawExtent().height
        .gesture = gesture.gesture,
        .moveForward = keysLive && ImGui::IsKeyDown(ImGuiKey_W),
        .moveBack = keysLive && ImGui::IsKeyDown(ImGuiKey_S),
        .moveLeft = keysLive && ImGui::IsKeyDown(ImGuiKey_A),
        .moveRight = keysLive && ImGui::IsKeyDown(ImGuiKey_D),
        .moveUp = keysLive && ImGui::IsKeyDown(ImGuiKey_E),    // WORLD +Y
        .moveDown = keysLive && ImGui::IsKeyDown(ImGuiKey_Q),  // WORLD -Y
        .fast = keysLive && io.KeyShift};
    editorCamera.update(cameraInput, context.deltaSeconds);

    // drawExtent is step 7's local, in PIXELS -- the ONE place pixels are correct (D15).
    lastAspect =
        drawExtent.height != 0 ? static_cast<float>(drawExtent.width) / static_cast<float>(drawExtent.height) : 1.0F;

    // --- 8b'. transform gizmos (task 2.3.3). BETWEEN the camera and picking, and that position is
    // load-bearing in BOTH directions (D4):
    //   AFTER the camera  -- Manipulate must see THIS frame's view/projection, or the handles land
    //                        one frame behind the pixels they belong to (2.3.2's INV-2 argument).
    //   BEFORE picking    -- ImGuizmo::IsOver()/IsUsing() describe the LAST Manipulate call
    //                        (gContext.mOperation is assigned at ImGuizmo.cpp:2719, near the END of
    //                        Manipulate), so consulting them before it answers about LAST frame.
    updateGizmo(context, Vec2{imageOrigin.x, imageOrigin.y}, Vec2{avail.x, avail.y}, hovered);

    // --- 8c. picking (task 2.3.2). Runs BEFORE F -- so a click-then-F in the same frame frames what
    // was just clicked -- and BEFORE the overlay, so the highlight shows the new selection with no
    // frame of lag (D11). Writing context.selection HERE is the HierarchyPanel::applyPending shape
    // (F32): the Image item is submitted and closed, no ImGui tree is open, and no eachChild walk is
    // in flight, which is what .claude/rules/editor.md's "not during a draw walk" actually protects.
    updatePick(context, Vec2{imageOrigin.x, imageOrigin.y}, Vec2{avail.x, avail.y}, inputHovered);

    // D7: F is gated on HOVER, not on focus. This is a REASONED DEVIATION from the chord rule in
    // .claude/rules/editor.md, not an oversight: that rule exists so a focused InputText cannot
    // swallow a menu accelerator like Ctrl+S. F is not a chord, and its correct routing is the
    // opposite -- it must act on the panel the mouse is OVER, not the one last CLICKED, or focusing
    // the camera would require clicking the viewport first, which no 3D application demands. ImGui
    // has NO "route to hovered" flag, so Shortcut() cannot express it. io.WantTextInput preserves the
    // rule's real intent (AC-15); repeat=false frames once per press, not every frame.
    if (inputHovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        focusSelection(context);
    }

    // --- 8e. the selection highlight (task 2.3.2). LAST of the new phases so it projects through the
    // POST-F camera: F moves the eye in this same frame and renderScene will submit that moved
    // camera, so building the overlay any earlier would put the box one frame behind the pixels
    // (INV-2). Drawing here also puts it UNDER the size readout below and OVER the Image above.
    drawSelectionOverlay(context, Vec2{imageOrigin.x, imageOrigin.y}, Vec2{avail.x, avail.y});

    // Step 9 (D15/C3): the overlay offset is written COMPONENT-WISE -- ImVec2 + ImVec2 does not
    // compile in this codebase (IMGUI_DEFINE_MATH_OPERATORS is defined nowhere).
    ImGui::SetCursorScreenPos(ImVec2(imageOrigin.x + OVERLAY_INSET, imageOrigin.y + OVERLAY_INSET));
    ImGui::TextColored(OVERLAY_COLOR, "%ux%u", drawExtent.width, drawExtent.height);
    if (gesture.gesture == CameraGesture::Fly) {
        ImGui::TextColored(OVERLAY_COLOR, "fly %.1f u/s", static_cast<double>(editorCamera.flySpeed()));
    }
    // Step 9b: RECORD THE INTERACTIVE STRIP'S RECT, in the same screen-space POINTS io.MousePos uses,
    // so updatePick's ARM step can refuse a press the strip owns. `rowStart` is captured BEFORE the
    // gizmo bar and `GetItemRectMax()` AFTER the view options, whose exposure slider is the row's
    // last and rightmost item -- so the pair spans the whole interactive row.
    //
    // ONLY THE INTERACTIVE ROW. The size readout and the fly line above it are TextColored, which
    // submits nothing clickable, so a press there has always fallen through to the scene pick and
    // still does -- this records what it is about, not the whole overlay.
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    drawGizmoBar();  // task 2.3.3: T / R / S | Local/World, submitted AFTER Manipulate (A8)
    // drawGizmoBar() leaves the cursor at the START of the next line (its last submitted item, the
    // Local/World button, is not followed by a SameLine()), so the SameLine goes HERE, before the
    // call, to keep the two on one row.
    ImGui::SameLine();
    drawViewOptions();  // task 3.6.3 -- OUTSIDE drawGizmoBar's BeginDisabled(!gizmoHasTarget) scope
    const ImVec2 rowEnd = ImGui::GetItemRectMax();
    overlayRowTopLeft = Vec2{rowStart.x, rowStart.y};
    overlayRowBottomRight = Vec2{rowEnd.x, rowEnd.y};

    // Step 10: record the request, LAST, after everything succeeded.
    renderRequested = true;
}

void ViewportPanel::focusSelection(PanelContext& context) {
    // task 3.1.5, corrected by E.1.4: `meshBounds` reaches BOTH remaining consumers -- this one and
    // updatePick's PickRequest -- or neither of them (INV-D6). The pick box and the frame box are the
    // same box, structurally rather than by review. THE HIGHLIGHT NO LONGER RESOLVES A BOX AT ALL: it
    // draws a GPU silhouette for everything with geometry and a point marker for everything without,
    // and which of the two an entity gets is decided once by scene_render::buildSelectionMaskSet.
    const Aabb bounds = context.selection.empty()
                            ? sceneBounds(context.world, meshBounds)
                            : selectionBounds(context.world, context.selection.entities(), meshBounds);
    if (bounds.valid()) {
        editorCamera.focusOn(bounds, lastAspect);
    } else {
        editorCamera.reset();  // nothing framable anywhere -- the DELIBERATE recovery path for a user
                               // who has flown into the void (E14)
    }
}

void ViewportPanel::updatePick(PanelContext& context, Vec2 imageOrigin, Vec2 avail, bool hovered) {
    const ImGuiIO& io = ImGui::GetIO();

    // ARM. Only a plain FRESH LMB press on the image that nextGesture did NOT claim and that the
    // GIZMO did not claim. `gizmoActive` is the panel's OWN flag, assigned on every updateGizmo
    // entry -- NOT a direct ImGuizmo::IsOver() call, which reads stale gContext state on any frame
    // where no Manipulate ran (F8/D10). This corrects the spelling 2.3.2's D20 seam comment
    // proposed; the intent is unchanged.
    //
    // ...AND NOT A PRESS THE OVERLAY STRIP OWNS. That last term is a RECT TEST rather than a question
    // to ImGui, and the difference is a shipped regression: the first attempt disarmed on
    // ImGui::IsAnyItemActive(), which CANNOT tell "a widget took this click" from "the user clicked
    // the window background". Verified in the pinned tree rather than reasoned about --
    // imgui.cpp:5522 "Click on empty space to focus window and start moving" reaches
    // StartMouseMovingWindow at :5534, which does SetActiveID(window->MoveId, window) at :5389, and
    // IsAnyItemActive() is `g.ActiveId != 0` at :6617. ImGui::Image submits with id 0, so a click on
    // the viewport image IS window empty space, ActiveId becomes the window's MoveId, and the guard
    // fired on exactly the frames a pick was being attempted -- disabling scene picking outright.
    const Vec2 pressPos{io.MousePos.x, io.MousePos.y};
    if (hovered && !gizmoActive && gesture.gesture == CameraGesture::None &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !overlayOwnsPress(pressPos)) {
        pickArmed = true;
        pickPressPos = pressPos;
    }
    if (!pickArmed) {
        return;
    }
    // A gizmo grab that began AFTER the arm DISARMS, exactly like a camera gesture does (E10/E11's
    // "nothing latches across frames" rule). So does a button that vanished without a release we
    // saw: the not-down check below runs on EVERY armed frame rather than only on the release edge,
    // because onDraw does not run at all for a hidden panel and the arm could otherwise survive a
    // hide/show.
    if (gizmoActive || gesture.gesture != CameraGesture::None) {
        pickArmed = false;
        return;
    }
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            pickArmed = false;
        }
        return;
    }
    pickArmed = false;

    // FIRE. Two gates, both on POINTS (D18). The RECT test rather than IsItemHovered(): the SDL3
    // backend captures the mouse while a button is held (F29), so a release far outside is an
    // ORDINARY sequence, and a containment test on io.MousePos is deterministic where ImGui's hover
    // heuristics are not (F28: the image's item id is 0 and it never becomes Active).
    const Vec2 pos{io.MousePos.x, io.MousePos.y};
    const bool insideRect = pos.x >= imageOrigin.x && pos.x < imageOrigin.x + avail.x && pos.y >= imageOrigin.y &&
                            pos.y < imageOrigin.y + avail.y;
    const Vec2 travel = pos - pickPressPos;
    // The negated `>` NaN-safe idiom (viewport_panel.cpp:104): a non-finite delta takes the reject
    // branch. PICK_CLICK_SLOP_POINTS is OURS, deliberately not io.MouseDragThreshold (F24/D10).
    const bool withinSlop = lengthSquared(travel) <= PICK_CLICK_SLOP_POINTS * PICK_CLICK_SLOP_POINTS;
    if (!insideRect || !withinSlop) {
        return;
    }

    const PickRequest request{.ndc = viewportNdc(pos, imageOrigin, avail),
                              .aspect = lastAspect,
                              .viewportSizePoints = avail,
                              .meshBounds = meshBounds};  // task 3.1.5 -- one of INV-D6's three
    const PickResult result = pickEntity(context.world, editorCamera, request);
    // F30: io.KeyCtrl ALONE is ALREADY "Ctrl on Windows/Linux, Cmd on macOS". Writing
    // `io.KeyCtrl || io.KeySuper` would ALSO fire on physical Ctrl on macOS -- identical to :171.
    const PickAction action =
        pickSelectionAction(result.hit(), context.selection.contains(result.entity), io.KeyCtrl, io.KeyShift);
    applyPickAction(context.selection, action, result.entity);
}

void ViewportPanel::updateGizmo(PanelContext& context, Vec2 imageOrigin, Vec2 avail, bool hovered) {
    const ImGuiIO& io = ImGui::GetIO();

    // 1. Pessimistic defaults (INV-4) -- before anything below can return.
    gizmoActive = false;
    gizmoHasTarget = false;

    // 2. Mode keys. The third gate removes every overlap with fly's W/A/S/D/Q/E (D7): fly's
    // `keysLive` is `flying && !io.WantTextInput` and fly requires RMB held => gesture == Fly, so
    // the two gates are mutually exclusive by construction. X is unbound anywhere else in the tree.
    const bool keysLive = hovered && !io.WantTextInput && gesture.gesture == CameraGesture::None;
    gizmoMode = nextGizmoMode(
        gizmoMode, GizmoModeInput{.translatePressed = keysLive && ImGui::IsKeyPressed(ImGuiKey_W, /*repeat=*/false),
                                  .rotatePressed = keysLive && ImGui::IsKeyPressed(ImGuiKey_E, /*repeat=*/false),
                                  .scalePressed = keysLive && ImGui::IsKeyPressed(ImGuiKey_R, /*repeat=*/false),
                                  .spaceTogglePressed = keysLive && ImGui::IsKeyPressed(ImGuiKey_X, /*repeat=*/false)});

    // 3. Target resolution (D11: PRIMARY only).
    const Entity target = context.selection.primary();
    const std::optional<Mat4> model = gizmoModelMatrix(context.world, target);
    const std::optional<Transform> before = readTransform(context.world, target);
    if (!model.has_value() || !before.has_value()) {
        // A4/E6-corrected (approved at plan review): this return is the one path that zeroes
        // gizmoWasUsing while ImGuizmo's OWN mbUsing latch can still be set (the target vanished,
        // e.g. destroyed, mid-drag) -- so the same moment must clear ImGuizmo's latch too, exactly
        // like item 5's guard below. gizmoHasTarget stays false, so the bar draws disabled (E5/D19).
        gizmoWasUsing = false;
        gizmoWarnLatched = false;
        ImGuizmo::Enable(false);
        context.commands.breakMergeChain();  // INV-3: every site that clears gizmoWasUsing also
                                             // breaks the chain -- this return delivers no End edge
        return;                              // AC-14: no ImGuizmo call at all
    }
    gizmoHasTarget = true;

    // 4. The behind-camera skip (D9/F5).
    const Mat4 viewProj = editorCamera.projectionMatrix(lastAspect) * editorCamera.viewMatrix();
    if (gizmoOriginBehindCamera(viewProj, *model, avail) && !ImGuizmo::IsUsing()) {
        // E19 -- every early return clears BOTH latches, not just the one above. Reaching this
        // return means IsUsing() is false, so if the previous frame was mid-drag the End edge
        // happens HERE. Reading IsUsing() here is deliberate and is NOT the F8 mistake: F8 forbids
        // consulting IsOver()/IsUsing() about THIS frame's cursor; here we want exactly "was a drag
        // in flight as of the last Manipulate", which mirrors ImGuizmo's own `&& !gContext.mbUsing`
        // (ImGuizmo.cpp:2697) so an in-flight drag is never cut off mid-gesture (A10).
        gizmoWasUsing = false;
        gizmoWarnLatched = false;
        context.commands.breakMergeChain();  // INV-3: this return also delivers no End edge
        return;
    }

    // 5. Per-frame setup. SetDrawlist is FIRST and EVERY frame, because BeginFrame() reassigns
    // gContext.mDrawList to the "gizmo" window's list (ImGuizmo.cpp:1017).
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetOrthographic(false);                                   // EditorCamera is perspective-only
    ImGuizmo::SetRect(imageOrigin.x, imageOrigin.y, avail.x, avail.y);  // POINTS (D18), never drawExtent

    // A4/E6-corrected (approved at plan review): a drag ImGuizmo never saw RELEASED (panel hidden,
    // window minimized or the target destroyed while LMB was down) leaves mbUsing latched -- and the
    // next Manipulate's move branch (ImGuizmo.cpp:2184) then writes the OLD entity's mModelSource
    // plus a fresh ray delta into OUR matrix for one frame. Enable(false) force-clears it
    // (ImGuizmo.cpp:1070-1077).
    // The `!IsMouseReleased` term is LOAD-BEARING: on the genuine release frame mbUsing is STILL true
    // here (Manipulate clears it at :2247), and the naive `!IsMouseDown()` form alone would drop that
    // frame's final delta -- a visible snap-back on EVERY release.
    if (gizmoWasUsing && !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        ImGuizmo::Enable(false);
    }
    ImGuizmo::Enable(gesture.gesture == CameraGesture::None);  // D20: an RMB-fly begun mid-drag ends
                                                               // the drag cleanly at its current value

    // 6. Arguments.
    const GizmoSpace space = effectiveSpace(gizmoMode.operation, gizmoMode.space);
    // F30/D8: io.KeyCtrl IS ALREADY "Ctrl on Windows/Linux, Cmd on macOS". Writing
    // `io.KeyCtrl || io.KeySuper` would ALSO fire on physical Ctrl on macOS -- identical to :181-182.
    const std::optional<Vec3> snap = gizmoSnapStep(gizmoMode.operation, io.KeyCtrl);
    Mat4 matrix = *model;  // Manipulate mutates in place; never pass model->
    const Mat4 view = editorCamera.viewMatrix();
    const Mat4 proj = editorCamera.projectionMatrix(lastAspect);
    const float* const snapPtr = snap ? &snap->x : nullptr;

    // 7. The call.
    // AC-16: Mat4 is COLUMN-MAJOR with the translation in columns[3], is_standard_layout
    // (mat4.hpp:42) and sizeof == 16*sizeof(float) (mat4.hpp:44) -- exactly ImGuizmo's matrix_t
    // union (ImGuizmo.cpp:319-332, m16[12..14] == position). data() passes straight through: no
    // transpose, no repack, no scratch buffer.
    // A7: snapPtr reads three contiguous floats from &Vec3::x, which rests on the SAME pair of
    // asserts for Vec3 -- vec3.hpp:34 (is_standard_layout_v) and :36 (sizeof == 3*sizeof(float)).
    // ImGuizmo's Translate path reads snap[0..2] (ImGuizmo.cpp:1259-1264); Rotate and Scale read
    // only snap[0] (:2482 DEGREES, :2372 replicated) -- gizmoSnapStep fills all three regardless.
    const bool changed = ImGuizmo::Manipulate(view.data(), proj.data(), toImGuizmoOperation(gizmoMode.operation),
                                              toImGuizmoMode(space), matrix.data(), /*deltaMatrix=*/nullptr, snapPtr,
                                              /*localBounds=*/nullptr, /*boundsSnap=*/nullptr);  // D14

    // 8. Arbitration flag (D10), computed only on a frame that actually submitted.
    const bool isUsing = ImGuizmo::IsUsing();
    // A9: IsOver() already ORs IsUsing() in (ImGuizmo.cpp:1042-1047); the second term is
    // belt-and-braces against an upstream change. isUsing is separately needed below.
    gizmoActive = ImGuizmo::IsOver() || isUsing;

    // 9. Drag edges (D22) + the D12 WARN latch.
    const GizmoDragEdge edge = gizmoDragEdge(gizmoWasUsing, isUsing);
    gizmoWasUsing = isUsing;
    if (edge == GizmoDragEdge::Begin) {
        gizmoWarnLatched = false;
    }
    // Code-review round (Gap 1): a new drag never merges into an old one, and this has to run BEFORE
    // this frame's own write-back below. ImGuizmo's Scale/Rotate handlers (unlike Translate) run their
    // "just grabbed" and "apply this frame's delta" blocks back to back on the SAME frame -- the onset
    // block sets mbUsing true and the very next `if (mbUsing)` block runs immediately after, so a
    // stale mScaleLast/mRotationAngleOrigin comparison left over from an EARLIER, unrelated drag can
    // report `changed` on this very Begin frame (ImGuizmo.cpp HandleScale ~:2305-2384, HandleRotation
    // ~:2429-2495). Breaking here, first, is what makes that push start a fresh entry instead of
    // folding into whatever the previous drag left open.
    if (edge == GizmoDragEdge::Begin) {
        context.commands.breakMergeChain();
    }

    // 10. Write back. Code-review round (Gap 1): the RELEASE frame carries the drag's FINAL delta --
    // ImGuizmo clears its own mbUsing latch only after writing that delta into `matrix` and reporting
    // `modified` (translate: ImGuizmo.cpp ~:2244-2249; scale/rotate are the same shape), so `edge ==
    // End` and a genuine write-back land on the SAME frame. The chain must therefore stay open through
    // THIS push and close only after it, on every exit path below -- closing first (the previous
    // shape) records the release frame as a SECOND, un-merged entry, failing AC-16. The old
    // `if (!changed) { return; }` early return is now this `if (changed)` block for exactly that
    // reason: whatever happens inside it, the chain-close after it still runs.
    if (changed) {
        const GizmoWrite write =
            gizmoWriteFromWorld(gizmoParentMatrix(context.world, target), matrix, *before, gizmoMode.operation);
        switch (write.status) {
            case GizmoWriteStatus::Applied: {
                // Task 2.4.1 D5: push() APPLIES the command. The direct transform write this replaces
                // is GONE -- there is exactly one write path now and it lives inside
                // TransformCommand::redo (AC-18). 2.3.3 D13's "mutating the World here is permitted"
                // reasoning is unchanged: the Image item is submitted and closed, no ImGui tree is
                // open, and no eachChild walk is in flight. The offscreen scene pass gains nothing from
                // this change (INV-5).
                CommandContext cmd = toCommandContext(context);
                context.commands.push(cmd, std::make_unique<TransformCommand>(target, *before, write.transform));
                break;
            }
            case GizmoWriteStatus::NoChange:
                break;  // write nothing (AC-11)
            case GizmoWriteStatus::NotFinite:
            case GizmoWriteStatus::NotDecomposable:
                // D12: an unlatched WARN emits ~60 lines/second straight into the Console panel and
                // drowns it -- the same discipline SceneRenderer's camera WARNs use.
                if (!gizmoWarnLatched) {
                    gizmoWarnLatched = true;
                    AERO_LOG_WARN(
                        "editor: gizmo drag rejected -- the result is degenerate or non-finite "
                        "(shear from a non-uniformly scaled ancestor, or a singular parent)");
                }
                break;
        }
    }
    // Code-review round (Gap 1): nothing merges into a finished drag -- closed AFTER this frame's
    // write-back above, not before it, so the release frame's own push (if any) still merges into the
    // drag it completes. Keeping this as its own site (rather than folding it back into item 9's
    // pre-write check) is what fixes AC-16 without giving up INV-3's belt-and-braces defence: a future,
    // non-gizmo push arriving while this frame's chain is still nominally open must not fold into a
    // drag that has already finished.
    if (edge == GizmoDragEdge::End) {
        context.commands.breakMergeChain();
    }
}

void ViewportPanel::drawGizmoBar() {
    // A6: IsItemHovered(AllowWhenDisabled) + SetTooltip, NEVER SetItemTooltip -- its ForTooltip flags
    // exclude disabled items and the tooltip would silently never appear (shell_ui.cpp's
    // menuItemStub idiom). Checked per-button (IsItemHovered only ever answers about the LAST
    // submitted item), so hovering ANY button in a no-target row explains why (E5).
    ImGui::PushID("gizmobar");  // 1:1 with PopID below -- INV-6
    ImGui::BeginDisabled(!gizmoHasTarget);

    const auto opButton = [this](const char* label, GizmoOperation op) {
        const bool active = gizmoMode.operation == op;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        const bool clicked = ImGui::SmallButton(label);
        if (active) {
            ImGui::PopStyleColor();  // 1:1, unconditional -- never inside a branch that can skip it
        }
        if (!gizmoHasTarget && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("No gizmo target -- select an entity with a Transform");
        }
        if (clicked) {
            gizmoMode.operation = op;
        }
        ImGui::SameLine();
    };
    opButton("T", GizmoOperation::Translate);
    opButton("R", GizmoOperation::Rotate);
    opButton("S", GizmoOperation::Scale);

    // AC-4/D3: the label always shows effectiveSpace, so it reads "Local" for Scale and cannot claim
    // otherwise, even though this one button is only conditionally disabled (nested inside the row's
    // own BeginDisabled).
    const GizmoSpace effective = effectiveSpace(gizmoMode.operation, gizmoMode.space);
    const bool scaleForcesLocal = gizmoMode.operation == GizmoOperation::Scale;
    if (scaleForcesLocal) {
        ImGui::BeginDisabled();
    }
    const bool spaceClicked = ImGui::SmallButton(effective == GizmoSpace::Local ? "Local" : "World");
    if (scaleForcesLocal) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!gizmoHasTarget) {
            ImGui::SetTooltip("No gizmo target -- select an entity with a Transform");
        } else if (scaleForcesLocal) {
            ImGui::SetTooltip("Scale is always Local");
        }
    }
    if (spaceClicked) {
        gizmoMode.space = (gizmoMode.space == GizmoSpace::Local) ? GizmoSpace::World : GizmoSpace::Local;
    }

    ImGui::EndDisabled();
    ImGui::PopID();
}

void ViewportPanel::drawViewOptions() {
    // NOT inside drawGizmoBar(): that function wraps its whole row in
    // ImGui::BeginDisabled(!gizmoHasTarget), and tonemap controls that grey out because nothing is
    // selected would be a defect. Called on the same LINE, in a different scope.
    ImGui::PushID("viewoptions");  // 1:1 with PopID below -- INV-6

    // FIRST IN THE ROW, DELIBERATELY. onDraw's step 9b records
    // `rowEnd = ImGui::GetItemRectMax()` right after this function returns, and its comment names
    // the exposure slider as the row's last and rightmost item. A checkbox appended at the END would
    // make that comment false and silently move the rect overlayOwnsPress() reads -- which decides
    // whether a click on the strip deselects the scene entity behind it. At the front, 2.3.2's
    // contract, its comment and its rect are all untouched. E.2.4 moves this whole row into a
    // popover and takes the checkbox with it.
    //
    // ONE toggle covers the grid AND the axes: they are one piece of chrome, and a second checkbox
    // for two lines is not worth a row of the strip. `drawAxes` stays a parameter of the emitter so
    // a test and the sample can drive both arms.
    bool gridChecked = gridEnabledValue;
    if (ImGui::Checkbox("Grid", &gridChecked)) {
        gridEnabledValue = gridChecked;
    }
    ImGui::SameLine();

    render::TonemapParams edited = tonemapParamsValue;
    bool changed = false;

    ImGui::SetNextItemWidth(OPTIONS_COMBO_WIDTH);
    // ASYMMETRIC pair (like BeginMenu): EndCombo runs ONLY when BeginCombo returned true. Getting
    // that backwards is an IM_ASSERT abort in the Debug build, not a visual glitch.
    if (ImGui::BeginCombo("##tonemap", tonemapOperatorLabelCStr(edited.curve))) {
        for (std::size_t i = 0; i < render::TONEMAP_OPERATOR_COUNT; ++i) {
            const auto candidate = static_cast<render::TonemapOperator>(i);
            const bool selected = candidate == edited.curve;
            if (ImGui::Selectable(tonemapOperatorLabelCStr(candidate), selected)) {
                edited.curve = candidate;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(OPTIONS_SLIDER_WIDTH);
    // THE ORDER OF THE `||` IS DELIBERATE: SliderFloat(...) || changed, never changed || Slider(...).
    // The second form short-circuits and would SKIP SUBMITTING THE SLIDER ENTIRELY on any frame the
    // combo changed -- and an ImGui item that is not submitted is an item that vanishes for a frame.
    changed = ImGui::SliderFloat("Exposure", &edited.exposure, render::MIN_EXPOSURE, render::MAX_EXPOSURE, "%.3f",
                                 ImGuiSliderFlags_Logarithmic) ||
              changed;

    if (changed) {
        // 3.4.2's rule: clamping is done in C++, never by trusting the widget's own range. A slider
        // with a v_min still lets a Ctrl+Click type anything at all, and no tier in this tree can
        // perform that click -- so this call is the only thing standing between a typed value and a
        // uniform.
        tonemapParamsValue = render::sanitizeTonemapParams(edited);
    }
    ImGui::PopID();
}

void ViewportPanel::drawSelectionOverlay(PanelContext& context, Vec2 imageOrigin, Vec2 avail) {
    // The SAME view-projection renderScene will submit this tick (INV-2/F7: lastAspect comes from
    // drawExtent, and renderScene derives the identical number from frame->extent() with no resize in
    // between) -- so the markers land on the pixels they belong to, never one frame behind them.
    const Mat4 view = editorCamera.viewMatrix();
    const Mat4 proj = editorCamera.projectionMatrix(lastAspect);
    const Mat4 viewProj = proj * view;
    // task E.1.4: built ONCE per tick and consumed TWICE -- here for the markers, and in renderScene
    // for the mask (D12). Building it twice would be cheap and would be TWO SOURCES OF TRUTH, which
    // is the defect D11 exists to remove, one layer up. `sceneRenderer` is a CHECKED optional at this
    // point: onDraw's step-4 guard dominates this call.
    selectionMaskSet = scene_render::buildSelectionMaskSet(
        context.world, context.selection.entities(), context.selection.primary(),
        {.view = view, .proj = proj, .eyePosition = editorCamera.position()}, selectionMaskScratch,
        sceneRenderer ? &sceneRenderer->bindings() : nullptr, MAX_HIGHLIGHTED_ENTITIES);

    // task E.1.4: the marker list comes from the SAME resolution the mask uses, so an entity can
    // never be both un-outlined and un-markered (D11/INV-6). The MeshBoundsLookup argument is
    // deliberately OMITTED -- this call no longer resolves a box.
    buildSelectionOverlay(context.world, selectionMaskSet.withoutGeometry, context.selection.primary(), viewProj, avail,
                          overlayScratch);
    if (overlayScratch.empty()) {
        return;  // E1: no PushClipRect at all, so there is no pair left unbalanced
    }

    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    // 1:1 with PopClipRect, like every ImGui pair (.claude/rules/editor.md) -- an unbalanced pair is
    // an IM_ASSERT abort in Debug, not a visual glitch. `true` INTERSECTS with the window's own clip
    // rect rather than replacing it, which is what stops a box on an off-screen entity from drawing
    // OVER the Hierarchy panel (E8). The draw-list form, not ImGui::PushClipRect: this task draws
    // only and hit-tests nothing, so render-level scissoring is the correct and cheaper one (F22).
    drawList->PushClipRect(ImVec2(imageOrigin.x, imageOrigin.y),
                           ImVec2(imageOrigin.x + avail.x, imageOrigin.y + avail.y), true);
    for (const OverlaySegment& segment : overlayScratch) {
        const bool isPrimary = segment.role == OverlayRole::Primary;
        // F27: ImVec2 + ImVec2 does not compile in this codebase (IMGUI_DEFINE_MATH_OPERATORS is
        // defined nowhere) -- component-wise, always.
        drawList->AddLine(ImVec2(imageOrigin.x + segment.a.x, imageOrigin.y + segment.a.y),
                          ImVec2(imageOrigin.x + segment.b.x, imageOrigin.y + segment.b.y),
                          selectionImU32(isPrimary ? render::SELECTION_OUTLINE_PRIMARY_DEFAULT
                                                   : render::SELECTION_OUTLINE_SECONDARY_DEFAULT),
                          isPrimary ? SELECTION_PRIMARY_THICKNESS : SELECTION_THICKNESS);
    }
    drawList->PopClipRect();
}

void ViewportPanel::renderScene(World& world) {
    // E2/S6: consumed UNCONDITIONALLY, before the status check -- dropping this makes a panel hidden
    // between two ticks render one stale frame.
    const bool requested = std::exchange(renderRequested, false);
    // The `!target`/`!sceneRenderer` half is defensive, same reasoning as onDraw's step 4 — it is
    // what lets both accesses below be CHECKED optional accesses.
    if (!requested || status != Status::Ready || !target || !sceneRenderer || !post || !debugDrawer ||
        !selectionOutline) {
        return;
    }
    // INV-3: no ImGui call, no World mutation -- and, from task 2.3.1, no CAMERA mutation either. The
    // camera is READ here and WRITTEN only in onDraw; moving the update here would work today and
    // break the moment 2.3.3 needs the view-projection during the draw walk.
    AERO_PROFILE_ZONE;
    // task 3.6.3: the scene now draws into the HDR target `post` owns, and a fullscreen resolve turns
    // that into the 8-bit texture ImGui samples. Command buffer A (the scene) is SUBMITTED before B
    // (the resolve) is acquired, so the queue-ordering guarantee applies with no interleaving at all.
    std::optional<render::Frame> sceneFrame = post->beginScene(VIEWPORT_CLEAR_COLOR);
    if (!sceneFrame) {
        return;
    }
    // Still the DRAWN sub-rect, and still the correct aspect source with quantum = 64 -- unchanged
    // semantics, one level down (2.2.3's own S3 rule, now reading the HDR target's frame).
    const rhi::Extent2D extent = sceneFrame->extent();
    const float aspect =
        extent.height != 0 ? static_cast<float>(extent.width) / static_cast<float>(extent.height) : 1.0F;
    const render::CameraView cameraView{.view = editorCamera.viewMatrix(),
                                        .proj = editorCamera.projectionMatrix(aspect),
                                        .eyePosition = editorCamera.position()};
    // buildRenderView + latched WARNs (suppressed for the camera pair, D3) + ForwardRenderer::draw.
    // This is the ONE place in the tree that names render::CameraView, and it is src-private (D2).
    sceneRenderer->render(world, *sceneFrame, &cameraView);
    // task E.1.1: THE SLOT. AFTER the forward pass, so a Tested line is depth-tested against this
    // frame's geometry; BEFORE endScene, so it records into the still-open HDR pass and goes through
    // the resolve -- and therefore the tonemap -- with everything else. The flush submits its OWN
    // upload command buffer here, which is strictly before endScene submits A, which is what orders
    // the copy ahead of the draws that read it. Empty most frames until E.1.2 and E.2.3 fill it, and
    // an empty flush acquires nothing, uploads nothing and draws nothing.

    // task E.1.2: THE GRID -- the first content in the batch E.1.1 left empty. HERE rather than in
    // onDraw for three reasons: this is BELOW renderScene's own guard chain, so a push can never
    // survive a frame that does not flush and draw two grids next frame; the camera here is the one
    // being RENDERED, not last frame's lastAspect; and it is one place. Pushed FIRST in the frame,
    // into the Tested bucket, which is the right end of both orders -- E.1.1 records Tested before
    // Overlay, so E.2.3's gizmos win over the grid with no coordination between the two tasks.
    //
    // farPlane() is read, not trusted: EditorCamera::clampState() guarantees far > near > 0 but
    // NOT finiteness (its own comment says so -- a directly-poisoned NaN survives until the next
    // update() sweeps it). emitDebugGrid's totality gate is what makes that safe: a non-finite
    // camera emits nothing, rejects nothing, and leaves the batch untouched.
    if (gridEnabledValue) {
        (void)render::emitDebugGrid(debugDrawer->batch(), {.eye = editorCamera.position(),
                                                           .focus = editorCamera.pivot(),
                                                           .farPlane = editorCamera.farPlane(),
                                                           .style = viewportGridStyle()});
    }
    debugDrawer->flush(*sceneFrame, cameraView);
    post->endScene(std::move(*sceneFrame));  // submits command buffer A -- AFTER the upload's submit

    // task E.1.4: the mask, on its OWN command buffer, STRICTLY BETWEEN A and B. It reads the depth A
    // just wrote, which is why PostProcessConfig::sceneDepthStore is true in ensureInitialized -- a
    // DontCare'd depth attachment is GARBAGE on a tiler, not stale. Earlier than this and it attaches
    // a depth texture whose pass is still open; later and it writes a mask nothing reads.
    const render::SelectionMaskView maskView = sceneRenderer->renderer().renderSelectionMask(
        post->sceneDepthTexture(), post->sceneTextureExtent(), post->sceneDrawExtent(), selectionMaskSet.secondary,
        selectionMaskSet.primary);

    std::optional<render::Frame> outFrame = target->beginFrame(VIEWPORT_CLEAR_COLOR);
    if (!outFrame) {
        selectionMaskSet = {};  // D12: cleared on the early-return path too, never left stale
        return;
    }
    post->resolve(*outFrame, tonemapParamsValue);
    // AFTER the resolve, into the SAME open pass: the outline is editor chrome and must not go
    // through the tone curve (D7). An invalid maskView -- which is what an empty selection returns --
    // records nothing at all and does not move compositeCount().
    selectionOutline->composite(*outFrame, maskView, selectionOutlineParams());
    target->endFrame(std::move(*outFrame));  // submits B, strictly after A and the mask
    selectionMaskSet = {};                   // D12: never drawn twice, never drawn stale
}

// task E.1.4: the two colours are the ENGINE defaults, so the outline and the point marker cannot
// drift apart (D18). The radius is derived from the framebuffer scale captured in onDraw --
// renderScene must not call ImGui (2.2.3 INV-3). The 1L/8L literals mirror
// SELECTION_OUTLINE_MIN_RADIUS / _MAX_RADIUS; the sanitize call is what makes the pair authoritative
// even if they ever drift, which is why it is not redundant.
render::SelectionOutlineParams ViewportPanel::selectionOutlineParams() const noexcept {
    const auto radius = static_cast<std::uint32_t>(
        std::clamp(std::lround(SELECTION_OUTLINE_RADIUS_POINTS * lastFramebufferScale), 1L, 8L));
    return render::sanitizeSelectionOutlineParams({.radiusPixels = radius});
}

const render::SelectionOutline* ViewportPanel::selectionOutlinePass() const noexcept {
    return selectionOutline ? &*selectionOutline : nullptr;
}

}  // namespace engine::editor

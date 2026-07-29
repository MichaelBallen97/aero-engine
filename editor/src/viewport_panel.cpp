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
#include <aero/editor/gizmo.hpp>
#include <aero/editor/picking.hpp>
#include <aero/editor/scene_bounds.hpp>
#include <aero/editor/selection.hpp>
#include <aero/editor/transform_ops.hpp>
#include <aero/rhi/internal/native_device.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <memory>
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

// D6: colour lives HERE, never in the public header -- an ImU32 there would break the ImGui-free
// rule, which is exactly why buildSelectionOverlay tags a ROLE and the panel maps role -> style.
// These four are TUNING values, judged by the human pass (editor/VALIDATION.md).
// IM_COL32 is a pure shift/or over its four arguments (imgui.h:3096), so constexpr is valid here --
// verified against the pinned 1.92.8 header, not assumed.
constexpr ImU32 SELECTION_COLOR = IM_COL32(255, 148, 32, 190);          // amber, dimmed
constexpr ImU32 SELECTION_PRIMARY_COLOR = IM_COL32(255, 176, 64, 255);  // brighter + opaque = primary
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

void ViewportPanel::ensureInitialized([[maybe_unused]] rhi::Extent2D firstExtent) {
    if (status != Status::Uninitialized) {
        return;
    }
    // `#if defined(...)` -- the exact spelling editor_reflection.cpp:5 already uses for
    // AERO_EDITOR_REFLECTION. Never `#if !AERO_EDITOR_SHADERS`: an undefined macro there is a
    // -Wundef trap, and matching the existing precedent keeps both degradation paths grep-able.
#if defined(AERO_EDITOR_SHADERS)
    shaderVfs.mount(std::make_unique<DirectoryBackend>(AERO_SHADERS_DIR));
    target = render::RenderTarget::create(
        *device, firstExtent,
        {.colorFormat = rhi::TextureFormat::RGBA8Unorm, .depth = true, .quantum = VIEWPORT_EXTENT_QUANTUM});
    if (!target) {
        status = Status::Unavailable;
        unavailableReason = "render target creation failed";
        return;
    }
    sceneRenderer =
        scene_render::SceneRenderer::create(*device, shaderVfs, target->colorFormat(), target->depthFormat());
    if (!sceneRenderer) {
        target.reset();
        status = Status::Unavailable;
        unavailableReason = "scene renderer creation failed (are res://scene.vert/.frag cooked?)";
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

    // Step 3 (D11): lazy, latched, one-attempt initialisation.
    ensureInitialized(pixels);

    // Step 4. The `!target` half is defensive (status == Ready is set only alongside a live target
    // in ensureInitialized) and is what lets every access below be a CHECKED optional access.
    if (status != Status::Ready || !target) {
        drawUnavailableMessage(unavailableReason != nullptr ? unavailableReason : "Viewport unavailable");
        return;
    }

    // Step 5.
    if (!target->resize(pixels)) {
        status = Status::Unavailable;
        unavailableReason = "render target allocation failed";
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
    const CameraGestureInput gestureInput{
        .hovered = hovered,
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
    updatePick(context, Vec2{imageOrigin.x, imageOrigin.y}, Vec2{avail.x, avail.y}, hovered);

    // D7: F is gated on HOVER, not on focus. This is a REASONED DEVIATION from the chord rule in
    // .claude/rules/editor.md, not an oversight: that rule exists so a focused InputText cannot
    // swallow a menu accelerator like Ctrl+S. F is not a chord, and its correct routing is the
    // opposite -- it must act on the panel the mouse is OVER, not the one last CLICKED, or focusing
    // the camera would require clicking the viewport first, which no 3D application demands. ImGui
    // has NO "route to hovered" flag, so Shortcut() cannot express it. io.WantTextInput preserves the
    // rule's real intent (AC-15); repeat=false frames once per press, not every frame.
    if (hovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
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
    drawGizmoBar();  // task 2.3.3: T / R / S | Local/World, submitted AFTER Manipulate (A8)

    // Step 10: record the request, LAST, after everything succeeded.
    renderRequested = true;
}

void ViewportPanel::focusSelection(PanelContext& context) {
    const Aabb bounds = context.selection.empty() ? sceneBounds(context.world)
                                                  : selectionBounds(context.world, context.selection.entities());
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
    if (hovered && !gizmoActive && gesture.gesture == CameraGesture::None &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        pickArmed = true;
        pickPressPos = Vec2{io.MousePos.x, io.MousePos.y};
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

    const PickRequest request{
        .ndc = viewportNdc(pos, imageOrigin, avail), .aspect = lastAspect, .viewportSizePoints = avail};
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
        return;  // AC-14: no ImGuizmo call at all
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

    // 10. Write back.
    if (!changed) {
        return;
    }
    const GizmoWrite write =
        gizmoWriteFromWorld(gizmoParentMatrix(context.world, target), matrix, *before, gizmoMode.operation);
    switch (write.status) {
        case GizmoWriteStatus::Applied:
            // D13: mutating the World here is permitted. The Image item is submitted and closed, no
            // ImGui tree is open, and no eachChild walk is in flight -- which is what
            // .claude/rules/editor.md's "never mutate the World during a draw walk" actually
            // protects (the same reasoning :212-216 already records for the selection write).
            // INV-3 is untouched: renderScene gains nothing.
            writeTransform(context.world, target, write.transform);
            break;
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

void ViewportPanel::drawSelectionOverlay(PanelContext& context, Vec2 imageOrigin, Vec2 avail) {
    // The SAME view-projection renderScene will submit this tick (INV-2/F7: lastAspect comes from
    // drawExtent, and renderScene derives the identical number from frame->extent() with no resize in
    // between) -- so the box lands on the pixels it belongs to, never one frame behind them.
    const Mat4 viewProj = editorCamera.projectionMatrix(lastAspect) * editorCamera.viewMatrix();
    buildSelectionOverlay(context.world, context.selection.entities(), context.selection.primary(), viewProj, avail,
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
                          isPrimary ? SELECTION_PRIMARY_COLOR : SELECTION_COLOR,
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
    if (!requested || status != Status::Ready || !target || !sceneRenderer) {
        return;
    }
    // INV-3: no ImGui call, no World mutation -- and, from task 2.3.1, no CAMERA mutation either. The
    // camera is READ here and WRITTEN only in onDraw; moving the update here would work today and
    // break the moment 2.3.3 needs the view-projection during the draw walk.
    AERO_PROFILE_ZONE;
    std::optional<render::Frame> frame = target->beginFrame(VIEWPORT_CLEAR_COLOR);
    if (!frame) {
        return;
    }
    // frame->extent() is the DRAWN sub-rect (2.2.3's own S3 pins this), so it stays the correct
    // aspect source even with quantum = 64.
    const rhi::Extent2D extent = frame->extent();
    const float aspect =
        extent.height != 0 ? static_cast<float>(extent.width) / static_cast<float>(extent.height) : 1.0F;
    const render::CameraView cameraView{.view = editorCamera.viewMatrix(),
                                        .proj = editorCamera.projectionMatrix(aspect),
                                        .eyePosition = editorCamera.position()};
    // buildRenderView + latched WARNs (suppressed for the camera pair, D3) + ForwardRenderer::draw.
    // This is the ONE place in the tree that names render::CameraView, and it is src-private (D2).
    sceneRenderer->render(world, *frame, &cameraView);
    target->endFrame(std::move(*frame));
}

}  // namespace engine::editor

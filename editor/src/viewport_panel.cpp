// Aero Engine — the Viewport panel implementation (task 2.2.3), the only new ImGui TU this task
// adds. See viewport_panel.hpp for the two-phase contract (D3).
//
// Destruction order (AC-12): ~ViewportPanel runs inside ~PanelRegistry inside ~EditorApp, which
// precedes ~Device both in main.cpp's RAII-order comment and in the GPU test — so `target` and
// `sceneRenderer` (and the GPU objects they own) release before the Device does.
#include "viewport_panel.hpp"

#include <aero/core/log.hpp>
#include <aero/core/profiler.hpp>
#include <aero/core/vfs.hpp>
#include <aero/rhi/internal/native_device.hpp>
#include <aero/scene/camera.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <memory>
#include <utility>

namespace engine::editor {

namespace {

constexpr std::uint32_t VIEWPORT_EXTENT_QUANTUM = 64;
// ALPHA 1.0 IS LOAD-BEARING (E4): ImGui's pipeline alpha-blends (SrcAlpha/OneMinusSrcAlpha), so a
// 0-alpha clear would let the editor's chrome show THROUGH the viewport wherever no geometry drew.
// scene.frag.hlsl already writes alpha 1 (F19) -- the clear is the only alpha we can get wrong.
constexpr rhi::Color VIEWPORT_CLEAR_COLOR{0.06F, 0.06F, 0.07F, 1.0F};
constexpr ImVec4 OVERLAY_COLOR{0.7F, 0.7F, 0.75F, 0.8F};
constexpr float OVERLAY_INSET = 4.0F;

// D7/E9: GetContentRegionAvail() is in LOGICAL units; allocation must be sized in PIXELS. A
// non-finite or non-positive scale falls back to 1.0 (the framePaceSleepMs NaN-safe idiom,
// editor_app.cpp:29) -- spelled with the negated `>` so NaN takes the fallback branch.
[[nodiscard]] std::uint32_t toPixels(float logical, float scale) noexcept {
    const float safeScale = (scale > 0.0F) ? scale : 1.0F;
    const long rounded = std::lround(static_cast<double>(logical) * static_cast<double>(safeScale));
    return rounded < 1 ? 1U : static_cast<std::uint32_t>(rounded);
}

[[nodiscard]] bool worldHasCamera(World& world) {
    bool found = false;
    world.each<Camera>([&](Entity /*e*/, Camera& /*camera*/) { found = true; });
    return found;
}

// The shared "nothing to show" path (D11/D12): a centred one-line message, no image, no request.
void drawUnavailableMessage(const char* reason) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 textSize = ImGui::CalcTextSize(reason);
    const ImVec2 cursor = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(cursor.x + (avail.x - textSize.x) * 0.5F, cursor.y + (avail.y - textSize.y) * 0.5F));
    ImGui::TextUnformatted(reason);
}

}  // namespace

ViewportPanel::ViewportPanel(rhi::Device& deviceIn) noexcept : device(&deviceIn) {}

const char* ViewportPanel::id() const noexcept { return "Viewport"; }
DockSlot ViewportPanel::defaultDockSlot() const noexcept { return DockSlot::Center; }
PanelOptions ViewportPanel::options() const noexcept { return {.noScrollbar = true, .noPadding = true}; }

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

    // Step 9 (D15/C3): the overlay offset is written COMPONENT-WISE -- ImVec2 + ImVec2 does not
    // compile in this codebase (IMGUI_DEFINE_MATH_OPERATORS is defined nowhere).
    ImGui::SetCursorScreenPos(ImVec2(imageOrigin.x + OVERLAY_INSET, imageOrigin.y + OVERLAY_INSET));
    ImGui::TextColored(OVERLAY_COLOR, "%ux%u", drawExtent.width, drawExtent.height);
    if (!worldHasCamera(context.world)) {
        ImGui::TextColored(OVERLAY_COLOR, "No camera in scene");
    }

    // Step 10: record the request, LAST, after everything succeeded.
    renderRequested = true;
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
    // INV-3: no ImGui call and no World mutation anywhere in this function.
    AERO_PROFILE_ZONE;
    std::optional<render::Frame> frame = target->beginFrame(VIEWPORT_CLEAR_COLOR);
    if (!frame) {
        return;
    }
    sceneRenderer->render(world, *frame);  // buildRenderView + latched WARNs + ForwardRenderer::draw
    target->endFrame(std::move(*frame));
}

}  // namespace engine::editor

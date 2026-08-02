// Aero Engine — the ONLY place ImGui + SDL_GPU appear in the editor (task 2.1.1). Everything here
// runs on the far side of both boundary guards (docs/03 F2): editor/ may include SDL and SDL_GPU
// directly, and the two engine-side seams (native_window.hpp / native_device.hpp / native_event.hpp)
// hand it the raw handles it cannot synthesize any other way.
#include <aero/core/log.hpp>
#include <aero/editor/imgui_layer.hpp>
#include <aero/platform/context.hpp>
#include <aero/platform/internal/native_event.hpp>
#include <aero/platform/internal/native_window.hpp>
#include <aero/platform/window.hpp>
#include <aero/rhi/device.hpp>
#include <aero/rhi/internal/native_device.hpp>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <filesystem>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <memory>
#include <utility>

namespace engine::editor {

namespace {

// D5 sink: tee every raw SDL event ImGui's own backend needs to see (including the text-input /
// gamepad events engine::platform::Context::pollEvent would otherwise translate-and-discard).
void onRawEvent(void* /*user*/, const void* e) { ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(e)); }

// D7/G4: a stable, writable, exe-relative path for imgui.ini. SDL_GetBasePath() is SDL-cached (do
// NOT free); SDL_GetPrefPath() must be SDL_free()'d by the caller. Falls back to a CWD-relative name
// with a WARN if neither resolves (E3).
std::string deriveIniPath() {
    if (const char* const base = SDL_GetBasePath(); base != nullptr) {
        return std::string(base) + "aero_editor.ini";
    }
    if (char* const pref = SDL_GetPrefPath("AeroEngine", "AeroEditor"); pref != nullptr) {
        const std::string path(pref);
        SDL_free(pref);
        return path + "aero_editor.ini";
    }
    AERO_LOG_WARN("editor: could not resolve a base/pref path for imgui.ini; falling back to CWD");
    return "aero_editor.ini";
}

}  // namespace

std::optional<ImGuiLayer> ImGuiLayer::create(rhi::Device& device, platform::Window& window, platform::Context& ctx,
                                             bool persistLayout, std::string_view iniPathOverride) {
    if (ImGui::GetCurrentContext() != nullptr) {
        AERO_LOG_ERROR("editor: ImGuiLayer::create: an ImGui context already exists (one per process)");
        return std::nullopt;
    }

    const rhi::SwapchainHandle swapchain = device.createSwapchain(window);
    if (!swapchain.valid()) {
        AERO_LOG_ERROR("editor: ImGuiLayer::create: createSwapchain failed");
        return std::nullopt;
    }

    SDL_Window* const win = platform::internal::NativeWindowAccessor::get(window);
    auto* const dev = static_cast<SDL_GPUDevice*>(rhi::internal::NativeDeviceAccessor::device(device));
    if (win == nullptr || dev == nullptr) {
        AERO_LOG_ERROR("editor: ImGuiLayer::create: native window/device handle unavailable");
        device.destroySwapchain(swapchain);
        return std::nullopt;
    }

    // EMPTY override => the shipped exe/pref-relative derivation. A non-empty one is used verbatim,
    // which is what lets a test drive the layout-RESTORE path without touching the real editor ini.
    std::string resolvedIni = iniPathOverride.empty() ? deriveIniPath() : std::string(iniPathOverride);
    auto iniPath = std::make_unique<std::string>(std::move(resolvedIni));
    const bool wantsDefault = !(persistLayout && std::filesystem::exists(*iniPath));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;  // docking ON, viewports OFF always (D6/AC-10)
    io.ConfigDpiScaleFonts = true;                     // G3: 1.92 docking DPI font auto-scale
    io.IniFilename = persistLayout ? iniPath->c_str() : nullptr;

    ImGui::StyleColorsDark();
    if (const float scale = SDL_GetWindowDisplayScale(win); scale > 0.0F) {
        ImGui::GetStyle().ScaleAllSizes(scale);
    }

    if (!ImGui_ImplSDL3_InitForSDLGPU(win)) {
        AERO_LOG_ERROR("editor: ImGuiLayer::create: ImGui_ImplSDL3_InitForSDLGPU failed");
        ImGui::DestroyContext();
        device.destroySwapchain(swapchain);
        return std::nullopt;
    }

    ImGui_ImplSDLGPU3_InitInfo initInfo{};
    initInfo.Device = dev;
    initInfo.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, win);
    initInfo.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    initInfo.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    initInfo.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
    if (!ImGui_ImplSDLGPU3_Init(&initInfo)) {
        AERO_LOG_ERROR("editor: ImGuiLayer::create: ImGui_ImplSDLGPU3_Init failed");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        device.destroySwapchain(swapchain);
        return std::nullopt;
    }

    platform::internal::RawEventAccessor::setRawEventSink(ctx, &onRawEvent, nullptr);

    return ImGuiLayer(&device, &ctx, swapchain, std::move(iniPath), wantsDefault);
}

ImGuiLayer::ImGuiLayer(rhi::Device* device, platform::Context* ctx, rhi::SwapchainHandle swapchain,
                       std::unique_ptr<std::string> ownedIniPath, bool wantsDefaultLayout) noexcept
    : device(device),
      ctx(ctx),
      swapchain(swapchain),
      ownedIniPath(std::move(ownedIniPath)),
      defaultLayoutWanted(wantsDefaultLayout),
      live(true) {}

ImGuiLayer::~ImGuiLayer() {
    if (!live) {
        return;
    }
    // Teardown order is load-bearing (E7): stop feeding ImGui first, wait for the GPU to be idle (no
    // in-flight frame still touches ImGui's GPU objects), then tear the backends/context/swapchain
    // down in reverse-of-init order.
    platform::internal::RawEventAccessor::setRawEventSink(*ctx, nullptr, nullptr);
    device->waitIdle();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    device->destroySwapchain(swapchain);
}

ImGuiLayer::ImGuiLayer(ImGuiLayer&& other) noexcept
    : device(other.device),
      ctx(other.ctx),
      swapchain(other.swapchain),
      ownedIniPath(std::move(other.ownedIniPath)),
      defaultLayoutWanted(other.defaultLayoutWanted),
      live(other.live) {
    other.live = false;
}

ImGuiLayer& ImGuiLayer::operator=(ImGuiLayer&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (live) {
        platform::internal::RawEventAccessor::setRawEventSink(*ctx, nullptr, nullptr);
        device->waitIdle();
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        device->destroySwapchain(swapchain);
    }
    device = other.device;
    ctx = other.ctx;
    swapchain = other.swapchain;
    ownedIniPath = std::move(other.ownedIniPath);
    defaultLayoutWanted = other.defaultLayoutWanted;
    live = other.live;
    other.live = false;
    return *this;
}

void ImGuiLayer::beginFrame() {
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

bool ImGuiLayer::endFrame(const rhi::Color& clearColor) {
    // ImGui::Render() runs first, unconditionally — this keeps NewFrame/Render balanced even when
    // the frame below is skipped for a minimized window (E1).
    ImGui::Render();
    ImDrawData* const drawData = ImGui::GetDrawData();

    const rhi::CommandBufferHandle cmd = device->acquireCommandBuffer();
    const std::optional<rhi::SwapchainTexture> acquired = device->acquireSwapchainTexture(cmd, swapchain);
    if (!acquired) {
        device->cancel(cmd);
        return false;  // minimized — not an error
    }

    auto* const nativeCmd =
        static_cast<SDL_GPUCommandBuffer*>(rhi::internal::NativeDeviceAccessor::commandBuffer(*device, cmd));
    ImGui_ImplSDLGPU3_PrepareDrawData(drawData, nativeCmd);  // MUST run before the render pass (F7a)

    const rhi::ColorAttachment color{.texture = acquired->texture, .clearColor = clearColor};
    const rhi::RenderPassHandle pass = device->beginRenderPass(cmd, {.colorAttachments = {&color, 1}});
    auto* const nativePass =
        static_cast<SDL_GPURenderPass*>(rhi::internal::NativeDeviceAccessor::renderPass(*device, pass));
    ImGui_ImplSDLGPU3_RenderDrawData(drawData, nativeCmd, nativePass);
    device->endRenderPass(pass);

    return device->submit(cmd);  // presents
}

bool ImGuiLayer::wantsDefaultLayout() const noexcept { return defaultLayoutWanted; }

}  // namespace engine::editor

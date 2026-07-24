#pragma once
// Aero Engine — editor ImGui-on-engine host (task 2.1.1; Epic 2.1 opens the /editor layer). Engine
// and std types ONLY: no ImGui or SDL type appears here, so aero_editor and the GPU smoke test can
// include this header without becoming ImGui-aware — all ImGui/SDL_GPU code lives in
// editor/src/imgui_layer.cpp.
//
// Owns the process-global ImGui context + the SDL3/SDL_GPU3 backends + the master window's
// swapchain, and drives one ImGui frame over rhi::Device directly (not render::Renderer — ImGui's
// PrepareDrawData must run before the render pass opens, which Renderer::beginFrame does not allow
// for). Move-only; at most one live instance per process (ImGui's context is a process singleton).
// The Device, Window and Context passed to create() MUST outlive this layer.

#include <aero/rhi/handles.hpp>  // rhi::SwapchainHandle
#include <aero/rhi/types.hpp>    // rhi::Color

#include <memory>
#include <optional>
#include <string>

namespace engine::rhi {
class Device;
}  // namespace engine::rhi

namespace engine::platform {
class Window;
class Context;
}  // namespace engine::platform

namespace engine::editor {

class ImGuiLayer {
public:
    // nullopt (+ ERROR) if an ImGui context already exists, backend init fails, or the swapchain
    // cannot be created. persistLayout=false disables imgui.ini (the test path — no file written).
    // When true, the layer derives + owns the exe-relative ini path internally (it has SDL — D7),
    // so callers stay SDL-free. Installs the raw-event sink on ctx (D5) so ImGui receives every
    // event, including ones Context::pollEvent would otherwise translate-and-discard.
    [[nodiscard]] static std::optional<ImGuiLayer> create(rhi::Device& device, platform::Window& window,
                                                          platform::Context& ctx, bool persistLayout = true);

    // Teardown order (load-bearing): clear the raw-event sink -> device->waitIdle() -> SDLGPU3
    // shutdown -> SDL3 shutdown -> ImGui::DestroyContext() -> destroySwapchain(). A moved-from
    // layer is inert (no double DestroyContext).
    ~ImGuiLayer();
    ImGuiLayer(ImGuiLayer&&) noexcept;
    ImGuiLayer& operator=(ImGuiLayer&&) noexcept;
    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // ImGui_ImplSDLGPU3_NewFrame + ImGui_ImplSDL3_NewFrame + ImGui::NewFrame, in that order.
    void beginFrame();

    // ImGui::Render (called first, unconditionally, to keep NewFrame/Render balanced even when the
    // frame is skipped); acquire a command buffer + swapchain texture; PrepareDrawData (before the
    // render pass — mandatory, it records copy passes); begin a clear pass; RenderDrawData; end the
    // pass; submit (presents). Returns false when the window is not presentable (minimized) — NOT
    // an error, just skip the sleep-friendly frame.
    [[nodiscard]] bool endFrame(const rhi::Color& clearColor);

    // True when no imgui.ini was found/loaded at create() — drives the first-run DockBuilder default
    // layout. Derived from a filesystem check at create() time, not from live node introspection.
    [[nodiscard]] bool wantsDefaultLayout() const noexcept;

private:
    ImGuiLayer(rhi::Device* device, platform::Context* ctx, rhi::SwapchainHandle swapchain,
               std::unique_ptr<std::string> ownedIniPath, bool wantsDefaultLayout) noexcept;

    rhi::Device* device = nullptr;
    platform::Context* ctx = nullptr;
    rhi::SwapchainHandle swapchain{};
    // Heap-stable backing for io.IniFilename (D7): ImGui stores the ini path by pointer, so it must
    // keep a fixed address for the context's life even as ImGuiLayer moves. A unique_ptr keeps the
    // std::string put (only the pointer moves), so io.IniFilename never dangles — SSO paths included.
    std::unique_ptr<std::string> ownedIniPath;
    bool defaultLayoutWanted = false;
    bool live = false;  // moved-from inert (Window/Renderer precedent)
};

}  // namespace engine::editor

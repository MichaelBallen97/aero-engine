#pragma once
// Aero Engine — EditorApp: the editor's application shell (task 2.1.3, D1/D4/D11). ImGui-FREE BY
// RULE (D9/AC-3) — this header exposes engine + std types only. Owns the ImGui host, the panel
// registry, and the frame clock, and turns the loop into a callable tick() (run() is
// `while (tick()) {}`), which is what makes it drivable N-frames-at-a-time from a test.

#include <aero/core/time.hpp>
#include <aero/editor/imgui_layer.hpp>
#include <aero/editor/panel_registry.hpp>
#include <aero/editor/selection.hpp>
#include <aero/rhi/types.hpp>
#include <aero/scene/world.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace engine::editor {

class ViewportPanel;  // task 2.2.3: src-private (editor/src/viewport_panel.hpp). Only the NAME is
                      // needed here, so this PUBLIC header stays free of viewport_panel.hpp's
                      // engine includes and stays ImGui/entt-free.
class ConsolePanel;   // task 2.2.5: src-private (editor/src/console_panel.hpp). Only the NAME is
                      // needed here, so this PUBLIC header stays free of console_panel.hpp and
                      // therefore of <aero/editor/console_model.hpp> as well.

struct EditorAppConfig {
    rhi::Color clearColor{0.10F, 0.10F, 0.12F, 1.0F};  // unchanged from 2.1.1
    bool persistLayout = true;                         // -> ImGuiLayer (imgui.ini); false in tests
    bool registerDefaultPanels = true;                 // the five 2.2.x placeholders (D8)
    // The default new-scene contents (D9): Main Camera + Directional Light + Cube, so a freshly
    // launched editor is not an empty box and 2.2.3's viewport has something to render. Tests that
    // want a clean World set it false.
    bool seedDefaultScene = true;
    // Frame-rate ceiling applied ONLY while the window has no keyboard focus (D4). <= 0 disables the
    // throttle entirely — what the GPU smoke test uses so its frames stay unpaced + deterministic.
    float unfocusedFrameCapHz = 20.0F;
    // Task 2.2.4: the asset browser's root, as UTF-8. EMPTY -> the process working directory,
    // resolved ONCE at create() by resolveProjectRoot(). aero_editor's optional argv[1] writes this
    // field. A bad path is NOT validated here (D17): it flows to the panel, which reports it in-panel
    // and still docks and quits cleanly. Task 2.6.1 replaces this with the opened project's path and
    // calls AssetBrowserPanel::setRoot().
    std::string projectRoot;
};

// Sleep applied when the window is not presentable (minimized) — inherited verbatim from 2.1.1.
inline constexpr std::uint32_t MINIMIZED_SLEEP_MS = 4;

// PURE pacing policy (D4), exposed for tier-0 testing. Order of precedence is load-bearing:
// minimized beats unfocused (E12).
//   !presented                         -> MINIMIZED_SLEEP_MS
//   focused, or capHz <= 0             -> 0        (vsync paces us)
//   otherwise                          -> max(0, round(1000 / capHz) - frameElapsedMs)
[[nodiscard]] std::uint32_t framePaceSleepMs(bool presented, bool focused, float unfocusedCapHz,
                                             float frameElapsedMs) noexcept;

class EditorApp {
public:
    // nullopt (+ ERROR, already logged by ImGuiLayer) when the ImGui host cannot be created. The
    // device, window and ctx MUST outlive the app (the ImGuiLayer contract, D1).
    [[nodiscard]] static std::optional<EditorApp> create(rhi::Device& device, platform::Window& window,
                                                         platform::Context& ctx, const EditorAppConfig& config = {});

    ~EditorApp() = default;
    EditorApp(EditorApp&&) noexcept = default;
    EditorApp& operator=(EditorApp&&) noexcept = default;
    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    // One full editor frame: input newFrame -> event drain -> clock tick -> ImGui frame (menu bar,
    // dockspace, panels) -> present -> pace. Returns false once a quit has been requested, and is
    // idempotent after that (further calls return false without drawing — E10).
    bool tick();

    // tick() until it returns false. Returns the process exit code (0).
    int run();

    [[nodiscard]] PanelRegistry& panels() noexcept;
    [[nodiscard]] const PanelRegistry& panels() const noexcept;
    // The edited scene. EditorApp OWNS it (D6): a World is DOCUMENT state, unlike the
    // Context/Window/Device that main.cpp injects because they are process-global (2.1.3 D1).
    [[nodiscard]] World& world() noexcept;
    [[nodiscard]] const World& world() const noexcept;
    // The shared entity selection: the hierarchy writes it, 2.2.2's inspector and 2.2.3's viewport
    // read it, 2.3.2's picking writes it. ONE object, never two.
    [[nodiscard]] Selection& selection() noexcept;
    [[nodiscard]] const Selection& selection() const noexcept;
    [[nodiscard]] const FrameClock& clock() const noexcept;
    [[nodiscard]] bool focused() const noexcept;
    // True when the LAST tick() actually presented (false when the window was minimized). This is
    // what the GPU smoke test asserts — tick()'s own bool means "still running", not "presented".
    [[nodiscard]] bool presentedLastFrame() const noexcept;
    // How many log records the Console panel currently holds. 0 when no console panel is registered
    // (registerDefaultPanels == false, or registration was rejected). Exists for the same reason
    // presentedLastFrame() does (D16): without it, "records are captured while the panel is HIDDEN"
    // (AC-6) is mechanically unprovable and would fall entirely to the human pass.
    [[nodiscard]] std::size_t logRecordCount() const noexcept;

    void requestQuit() noexcept;
    void requestLayoutReset() noexcept;  // same effect as View > Reset Layout, applied next frame

private:
    // BY VALUE + move (task 2.2.4): EditorAppConfig gained a std::string field, so it is no longer
    // trivially copyable and modernize-pass-by-value (--warnings-as-errors in CI) requires this shape.
    EditorApp(ImGuiLayer layer, platform::Context& ctx, EditorAppConfig config);

    ImGuiLayer layer;
    platform::Context* ctx;
    EditorAppConfig config;
    PanelRegistry registry;
    FrameClock frameClock;
    bool running = true;
    bool windowFocused = true;        // a freshly created+shown window normally has focus; a stale false
                                      // would only cost one throttled frame before the first focus event
    bool applyDefaultLayout = false;  // seeded from ImGuiLayer::wantsDefaultLayout(); also set by
                                      // View > Reset Layout / requestLayoutReset()
    bool presented = false;
    World sceneWorld;
    Selection sceneSelection;
    // Non-owning; owned by `registry`, which holds panels through unique_ptr -- so the Panel object
    // is address-stable and this pointer survives an EditorApp move (F21). Null when
    // registerDefaultPanels == false (E13) or if registration was rejected (E14) -- ALWAYS null-check.
    ViewportPanel* viewportPanel = nullptr;
    // Non-owning; owned by `registry` (unique_ptr -> address-stable, survives an EditorApp move --
    // F17, the same reason viewportPanel above is legal). Null when registerDefaultPanels == false or
    // if registration was rejected -- ALWAYS null-check.
    ConsolePanel* consolePanel = nullptr;
};

}  // namespace engine::editor

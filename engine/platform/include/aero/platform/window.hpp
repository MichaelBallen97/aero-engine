#pragma once
// Aero Engine — window (task 0.3.1). A move-only RAII wrapper owning one OS window. The SDL_Window
// is hidden behind a pimpl (Impl is defined in src/platform.cpp), so no SDL type reaches this
// header. Create one through Context::createWindow — a Window cannot exist without a live,
// initialized Context, which is what guarantees SDL_Init ran first.
//
// LIFETIME CONTRACT: a Window must NOT outlive the Context that made it. ~Window calls the backend
// to destroy the OS window; after ~Context has shut SDL down that is undefined. Declare the Context
// first (outermost scope) so RAII destroys windows before it. (See §5 E15.)

#include <aero/platform/event.hpp>  // WindowId

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace engine::platform {

class Context;

// How to create a window. Sizes are LOGICAL (point) units; on a HiDPI display the pixel framebuffer
// is larger — query Window::pixelSize() for it.
struct WindowConfig {
    std::string title = "Aero";
    std::int32_t width = 1280;
    std::int32_t height = 720;
    bool resizable = true;
    bool highDpi = true;  // request a high-pixel-density framebuffer (SDL_WINDOW_HIGH_PIXEL_DENSITY)
    bool hidden = false;  // create without showing it (tests); call show() to reveal it later
};

// WindowSize (the logical/pixel size value type) moved to <aero/platform/event.hpp> in task 0.3.2 so
// it can double as the resize event payload; it arrives transitively via this header's event.hpp include.

class Window {
public:
    // Move-only. All special members are declared here and DEFINED in src/platform.cpp, where Impl
    // is complete (the pimpl requirement: unique_ptr<Impl> needs a complete Impl to move-assign or
    // destroy).
    Window(Window&&) noexcept;
    Window& operator=(Window&&) noexcept;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] WindowId id() const noexcept;

    // LOGICAL (point) size — what WindowConfig asked for and what WindowResized reports.
    [[nodiscard]] WindowSize size() const;
    // FRAMEBUFFER (pixel) size — what a renderer's swapchain must match; equals size() except on a
    // HiDPI display, where it is larger. What WindowPixelSizeChanged reports.
    [[nodiscard]] WindowSize pixelSize() const;

    void setTitle(std::string_view title);
    void setSize(std::int32_t width, std::int32_t height);  // logical units
    void show();
    void hide();

private:
    friend class Context;  // the only maker of Windows
    struct Impl;           // holds the SDL_Window*; defined in src/platform.cpp
    explicit Window(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl;
};

}  // namespace engine::platform

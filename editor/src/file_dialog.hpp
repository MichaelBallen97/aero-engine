#pragma once
// Aero Engine — the async boundary between SDL3's native file dialogs and the editor (task 2.5.1,
// D0/D6/D7/D8). src-private, SDL-only at source, ImGui-FREE. This is the whole of F2/F3/D6/D7.
//
// INV-3, in capitals: the dialog callback touches ONLY the channel below -- no World, no Selection,
// no ImGui, no panel, no log. It may run on an ARBITRARY thread (F2), so anything it touched that
// belonged to a frame would be a data race.
#include <aero/editor/scene_session.hpp>  // DialogResult

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace engine::editor {

// The one object that crosses threads. Heap-allocated and SHARED (std::shared_ptr), so it is
// address-stable across an EditorApp move AND outlives an EditorApp destroyed with a dialog still in
// flight (D6).
//
// `enable_shared_from_this`: `FileDialogHost` (scene_session.hpp) deliberately carries only a raw
// `DialogChannel*` -- a plain, trivially-nullable test seam (A17) -- while the two launchers below
// need a `shared_ptr` to construct the cross-thread Ticket. `shared_from_this()` is what bridges the
// two without adding a second owner: it is only ever called on the channel `EditorApp::create()`
// already holds by `shared_ptr`, so the precondition (an existing shared_ptr owner) always holds.
class DialogChannel : public std::enable_shared_from_this<DialogChannel> {
public:
    // Called on an ARBITRARY thread, exactly once per launch (D7).
    void deliver(const char* const* filelist);
    // MAIN thread. Returns the result once, and only once; resets to empty (INV-2/E3).
    [[nodiscard]] DialogResult take();

private:
    std::mutex mutex;
    DialogResult slot;
};

// `parent` may be null (SDL_dialog.h:140). `startDirectory` / `suggestion` are UTF-8; empty means
// "let the OS decide". Both COPY their arguments before returning -- SDL keeps no reference to them.
void launchOpenSceneDialog(const std::shared_ptr<DialogChannel>& channel, void* parentSdlWindow,
                           std::string_view startDirectory);
void launchSaveSceneDialog(const std::shared_ptr<DialogChannel>& channel, void* parentSdlWindow,
                           std::string_view suggestion);

// task 2.6.1: A FOLDER dialog -- no filters at all (SDL_ShowOpenFolderDialog takes none). Same
// callback, same Ticket, same arbitrary-thread contract (F1/INV-3) -- DialogChannel::deliver handles
// its result UNCHANGED, because the callback signature is identical to the two file dialogs' above.
void launchOpenProjectFolderDialog(const std::shared_ptr<DialogChannel>& channel, void* parentSdlWindow,
                                   std::string_view startDirectory);

// task 3.2.4: an ARBITRARY FILE, with NO FILTERS AT ALL. A Blender binary is `blender.exe` on Windows,
// `Blender` (NO EXTENSION) inside an .app bundle on macOS, and `blender` on Linux -- and SDL's filter
// patterns are documented as "alphanumerics, '-', '_', '.', or a lone '*'" (the SCENE_FILTERS comment
// in this file's .cpp cites the header line), which cannot express "a file with no extension".
// Passing nullptr/0 is the CORRECT spelling for that, not a shortcut. Same callback, same Ticket, same
// arbitrary-thread contract (F1/INV-3) as the three launchers above.
void launchLocateBlenderDialog(const std::shared_ptr<DialogChannel>& channel, void* parentSdlWindow,
                               std::string_view startDirectory);

// fix 2.5.1-focus: THE KEYBOARD AFTER A NATIVE DIALOG CLOSES. On macOS a sheet closed FROM THE KEYBOARD
// (Return or Escape) leaves NO key window: the editor stays main and the app stays active, but AppKit sends
// no windowDidBecomeKey:, so SDL's keyboard focus stays NULL and SDL_cocoakeyboard.m:553-559 drops every
// key until the window is clicked -- measured on hardware in 8 of 8 keyboard closes and 0 of 6 mouse
// closes. SDL's own ReactivateAfterDialog (SDL_cocoadialog.m:56-63) activates the APP, which restores
// whichever window was key before; after a keyboard close, none was.
//
// The decision, PURE so a test can walk every arm (I275): raise only when there IS a window and it does not
// already hold SDL's keyboard focus. Both pointers are opaque SDL_Window*s, compared and never dereferenced.
// The focus comparison makes it a no-op wherever the keyboard is ALREADY back when the result is taken --
// every mouse close in the active editor, measured on macOS -- with no new per-OS branch in editor/. Whether
// Windows' and Linux's dialogs hand it back before the result is taken is unmeasured; where they do not,
// this asks for a raise there too (on Wayland that request can surface as an attention hint).
[[nodiscard]] constexpr bool dialogCloseNeedsRaise(const void* window, const void* keyboardFocus) noexcept {
    return window != nullptr && keyboardFocus != window;
}

// MAIN THREAD ONLY -- SDL_GetKeyboardFocus and SDL_RaiseWindow both say so -- which is why its one caller is
// EditorApp::tick()'s take arm and never onDialogResult, which may run on ANOTHER thread
// (SDL_dialog.h:125-126). Reads SDL's keyboard focus and, when dialogCloseNeedsRaise says so, raises
// `parentSdlWindow` (on macOS: activate the app, then makeKeyAndOrderFront, SDL_cocoawindow.m's
// Cocoa_RaiseWindow; nothing for a hidden or minimised window). Returns true when it ASKED SDL to raise --
// SDL_RaiseWindow answers true whatever the window manager then does (SDL_video.c:3477-3488). Logs nothing
// -- this file's posture; the caller logs.
[[nodiscard]] bool restoreKeyboardFocusAfterDialog(void* parentSdlWindow);

}  // namespace engine::editor

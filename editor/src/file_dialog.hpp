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

}  // namespace engine::editor

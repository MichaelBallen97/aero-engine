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

}  // namespace engine::editor

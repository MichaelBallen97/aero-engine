// Aero Engine — SDL3 native dialogs + the cross-thread result channel (task 2.5.1). THE ONLY new SDL
// TU this task adds (besides the pre-existing imgui_layer.cpp). Logs NOTHING (D7/INV-3): the callback
// runs on an ARBITRARY thread (F2) and touches only the channel below.
#include "file_dialog.hpp"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_video.h>

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace engine::editor {

namespace {

// F3: STATIC storage duration. SDL requires filters to stay valid until the callback fires; a LOCAL
// array inside the launch function would dangle on the dialog thread the moment the launcher
// returns -- an ASan use-after-scope, reachable only by a human opening a dialog under the Debug
// preset (plan S15, R3). Pattern rules (SDL_dialog.h:57-60): alphanumerics, '-', '_', '.', or a lone
// '*'. "json", not "scene.json" -- SDL matches the FINAL extension only. std::array, not a C array
// (modernize-avoid-c-arrays, --warnings-as-errors on the Linux lane).
constexpr std::array<SDL_DialogFileFilter, 2> SCENE_FILTERS{{
    {"Aero scene (*.scene.json)", "json"},
    {"All files", "*"},
}};

// The ticket: ONE extra shared_ptr reference, heap-allocated, handed to SDL as `userdata` and ADOPTED
// by the callback. This is what makes both hazards impossible (D6):
//   * EditorApp MOVES while a dialog is open -> the channel does not move (it is on the heap) and the
//     ticket points at the control block, not at the app.
//   * EditorApp is DESTROYED while a dialog is open -> the channel's refcount is still >= 1, so
//     deliver() writes into a live object nobody will ever read, and the last release frees it.
// The raw `new` below carries no lint suppression (plan A23): cppcoreguidelines-* is not in
// .clang-tidy's Checks list, so one would be dead text. If a lane's clang-tidy fires here, fix with
// real code rather than adding a suppression back.
using Ticket = std::shared_ptr<DialogChannel>;

void SDLCALL onDialogResult(void* userdata, const char* const* filelist, int /*filter*/) {
    const std::unique_ptr<Ticket> ticket(static_cast<Ticket*>(userdata));  // adopt + free on return
    if (*ticket) {
        (*ticket)->deliver(filelist);
    }
}

}  // namespace

void DialogChannel::deliver(const char* const* filelist) {
    const std::lock_guard<std::mutex> lock(mutex);
    slot = {};
    slot.ready = true;
    // F4's three cases, in this exact order -- collapsing the first into the second (plan S14) would
    // report an SDL driver error as a silent cancel, which is exactly wrong.
    if (filelist == nullptr) {
        slot.failed = true;  // SDL error (F4)
        return;
    }
    if (*filelist == nullptr) {
        slot.cancelled = true;  // the user cancelled
        return;
    }
    slot.path = *filelist;  // first entry; allow_many is always false
}

DialogResult DialogChannel::take() {
    const std::lock_guard<std::mutex> lock(mutex);
    DialogResult result = slot;
    slot = {};  // resets to empty (INV-2/E3): a hypothetical second delivery overwrites, never replays
    return result;
}

void launchOpenSceneDialog(const std::shared_ptr<DialogChannel>& channel, void* parent,
                           std::string_view startDirectory) {
    auto* ticket = new Ticket(channel);
    const std::string dir(startDirectory);  // SDL copies it; OUR copy must survive the async call
    SDL_ShowOpenFileDialog(&onDialogResult, ticket, static_cast<SDL_Window*>(parent), SCENE_FILTERS.data(),
                           static_cast<int>(SCENE_FILTERS.size()), dir.empty() ? nullptr : dir.c_str(),
                           /*allow_many=*/false);
    // A1's macOS note: this launch happens with an ImGui frame half-built (the F12 slot), so a
    // BLOCKING dialog would stall a partially submitted frame. Measured (plan A21): Windows spawns a
    // thread (SDL_windowsdialog.c:1222), Linux goes through a portal/subprocess, and macOS uses
    // beginSheetModalForWindow (SDL_cocoadialog.m:186) -- all non-blocking -- BUT macOS falls back to
    // [dialog runModal] (SDL_cocoadialog.m:209) when `window == NULL`, which DOES block. We always
    // pass a parent (editor_app.cpp), so we always get the sheet; no defensive refusal is added.
    //
    // SDL_PollEvent already implicitly pumps (engine/platform/src/platform.cpp:426's own comment), so
    // no SDL_PumpEvents call is needed for the Linux XDG-portal DBus loop (plan A20) -- do not add one.
    //
    // SDL always invokes the callback, even on the "no dialog driver" path (SDL_unixdialog.c:70-77 ->
    // callback(userdata, NULL, -1)) -- which is what makes the Ticket leak-free by construction (A22).
}

void launchSaveSceneDialog(const std::shared_ptr<DialogChannel>& channel, void* parent, std::string_view suggestion) {
    auto* ticket = new Ticket(channel);
    const std::string dir(suggestion);  // SDL copies it; OUR copy must survive the async call
    SDL_ShowSaveFileDialog(&onDialogResult, ticket, static_cast<SDL_Window*>(parent), SCENE_FILTERS.data(),
                           static_cast<int>(SCENE_FILTERS.size()), dir.empty() ? nullptr : dir.c_str());
}

void launchOpenProjectFolderDialog(const std::shared_ptr<DialogChannel>& channel, void* parent,
                                   std::string_view startDirectory) {
    auto* ticket = new Ticket(channel);
    const std::string dir(startDirectory);  // SDL copies it; OUR copy must survive the async call
    SDL_ShowOpenFolderDialog(&onDialogResult, ticket, static_cast<SDL_Window*>(parent),
                             dir.empty() ? nullptr : dir.c_str(), /*allow_many=*/false);
}

void launchLocateBlenderDialog(const std::shared_ptr<DialogChannel>& channel, void* parent,
                               std::string_view startDirectory) {
    auto* ticket = new Ticket(channel);
    const std::string dir(startDirectory);  // SDL copies it; OUR copy must survive the async call
    // filters == nullptr and nfilters == 0: NO FILTER ARRAY AT ALL. See the header for why a Blender
    // binary cannot be expressed as an SDL filter pattern on all three hosts. Everything else is
    // launchOpenSceneDialog's shape verbatim, including the always-invoked-callback leak-freedom (A22).
    SDL_ShowOpenFileDialog(&onDialogResult, ticket, static_cast<SDL_Window*>(parent), /*filters=*/nullptr,
                           /*nfilters=*/0, dir.empty() ? nullptr : dir.c_str(), /*allow_many=*/false);
}

}  // namespace engine::editor

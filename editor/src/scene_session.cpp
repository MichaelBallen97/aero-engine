// Aero Engine — the document model, the pure file-flow guard, and THE scene swap (task 2.5.1). This
// TU is ImGui-FREE and SDL-FREE at source (plan A13): the whole state machine lives here as free
// functions so it is testable with no window and no GPU. <filesystem> and scene_serialize live in
// scene_file.cpp / scene_io.cpp instead (D19/F17, F9's gate).
#include <aero/core/log.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/world.hpp>

#include "file_dialog.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace engine::editor {

// ---- the pure guard state machine ----------------------------------------------------------------

bool discardsWork(FileAction action) noexcept {
    switch (action) {
        case FileAction::NewScene:
        case FileAction::OpenScene:
        case FileAction::Quit:
            return true;
        case FileAction::SaveScene:
        case FileAction::SaveSceneAs:
        case FileAction::None:
            return false;
    }
    return false;  // unreachable; every enumerator handled above
}

FileStep guardFor(FileAction action, bool dirty) noexcept {
    return (discardsWork(action) && dirty) ? FileStep::Confirm : FileStep::Perform;
}

FileStep saveStep(bool untitled) noexcept { return untitled ? FileStep::AskWhereToSave : FileStep::WriteNow; }

FileStep resolveConfirm(ConfirmChoice choice, bool untitled) noexcept {
    switch (choice) {
        case ConfirmChoice::Cancel:
            return FileStep::Nothing;
        case ConfirmChoice::Discard:
            return FileStep::Perform;
        case ConfirmChoice::Save:
            return saveStep(untitled);
    }
    return FileStep::Nothing;  // unreachable; every enumerator handled above
}

// ---- the document ----------------------------------------------------------------------------

std::string_view SceneSession::path() const noexcept { return scenePath; }
bool SceneSession::untitled() const noexcept { return scenePath.empty(); }
void SceneSession::setPath(std::string_view absolutePathUtf8) { scenePath = std::string(absolutePathUtf8); }
void SceneSession::clearPath() noexcept { scenePath.clear(); }

std::string SceneSession::documentName() const {
    if (untitled()) {
        return "Untitled";
    }
    return std::string(fileNameOf(scenePath));
}

std::string SceneSession::windowTitle(bool dirty) const {
    std::string title;
    if (dirty) {
        title += '*';
    }
    title += documentName();
    title += " - Aero Editor";
    return title;
}

std::string SceneSession::dialogDirectory(std::string_view projectRootUtf8) const {
    if (!untitled()) {
        const std::string_view dir = directoryOf(scenePath);
        if (!dir.empty()) {
            return std::string(dir);
        }
    }
    return std::string(projectRootUtf8);
}

std::string SceneSession::saveSuggestion(std::string_view projectRootUtf8) const {
    if (!untitled()) {
        return scenePath;
    }
    const std::string dir = dialogDirectory(projectRootUtf8);
    if (dir.empty()) {
        return "Untitled.scene.json";
    }
    std::string suggestion = dir;
    suggestion += '/';
    suggestion += "Untitled.scene.json";
    return suggestion;
}

// ---- pure path helpers -------------------------------------------------------------------------

std::string_view fileNameOf(std::string_view pathUtf8) noexcept {
    const std::size_t slash = pathUtf8.find_last_of("/\\");
    if (slash == std::string_view::npos) {
        return pathUtf8;
    }
    // NOT substr(): it is specified to throw std::out_of_range, which would escape this noexcept
    // function (bugprone-exception-escape, --warnings-as-errors in CI) -- the project_files.cpp
    // leafOf precedent. The pointer+size constructor IS noexcept, and `slash + 1 <= pathUtf8.size()`
    // holds by construction since find_last_of found a character.
    return std::string_view(pathUtf8.data() + slash + 1U, pathUtf8.size() - slash - 1U);
}

std::string_view directoryOf(std::string_view pathUtf8) noexcept {
    const std::size_t slash = pathUtf8.find_last_of("/\\");
    if (slash == std::string_view::npos) {
        return {};
    }
    return std::string_view(pathUtf8.data(), slash);
}

bool hasExtension(std::string_view pathUtf8) noexcept {
    return fileNameOf(pathUtf8).find('.') != std::string_view::npos;
}

std::string withSceneExtension(std::string_view pathUtf8) {
    if (hasExtension(pathUtf8)) {
        return std::string(pathUtf8);
    }
    std::string result(pathUtf8);
    result += SCENE_EXTENSION;
    return result;
}

// ---- THE swap (D2/INV-6/INV-1) -------------------------------------------------------------------

void resetSceneState(CommandContext& context, CommandStack& commands) {
    context.world.clear();
    context.selection.clear();
    context.roots.clear();
    commands.clear();
}

void newScene(CommandContext& context, CommandStack& commands) {
    resetSceneState(context, commands);
    seedDefaultScene(context.world);
}

// ---- the two logging actions (A30: the ONLY two places this task logs) ---------------------------

bool openSceneFile(CommandContext& context, CommandStack& commands, SceneSession& session,
                   std::string_view absolutePathUtf8) {
    const std::string path(absolutePathUtf8);
    const FileReadResult read = readTextFile(path);
    if (!read.text.has_value()) {
        AERO_LOG_ERROR("editor: could not open scene '{}' -- {}", path, read.error);
        return false;
    }
    const SceneOpenOutcome outcome = openSceneText(context, commands, *read.text);
    if (!outcome.ok) {
        std::string reason = outcome.message;
        if (outcome.line > 0) {
            reason += " (line " + std::to_string(outcome.line) + ", column " + std::to_string(outcome.column) + ")";
        }
        AERO_LOG_ERROR("editor: could not open scene '{}' -- {}", path, reason);
        return false;
    }
    session.setPath(path);  // only now -- nothing changed the path while the parse could still fail
    AERO_LOG_INFO("editor: opened scene '{}' -- {} entities, {} components ({} skipped, {} failed)", path,
                  outcome.entities, outcome.components, outcome.skipped, outcome.failed);
    if (outcome.skipped + outcome.failed > 0) {  // D21
        AERO_LOG_WARN("editor: scene '{}' loaded with {} skipped and {} failed components", path, outcome.skipped,
                      outcome.failed);
    }
    return true;
}

bool saveSceneFile(CommandContext& context, CommandStack& commands, SceneSession& session,
                   std::string_view absolutePathUtf8, bool appendExtension) {
    const std::optional<std::string> text = sceneToText(context.world);
    if (!text.has_value()) {
        AERO_LOG_ERROR("editor: could not save scene '{}' -- {}", absolutePathUtf8, "built without AERO_REFLECT_TOOLS");
        return false;
    }
    const std::string target = appendExtension ? withSceneExtension(absolutePathUtf8) : std::string(absolutePathUtf8);
    // D13's existence check fires ONLY when the extension was actually appended -- if the user typed a
    // name that already has one, the native panel already asked about overwriting.
    if (appendExtension && target != absolutePathUtf8 && fileExists(target)) {
        AERO_LOG_ERROR("editor: could not save scene '{}' -- {}", target, "a file with that name already exists");
        return false;
    }
    const std::string reason = writeTextFileAtomic(target, *text);
    if (!reason.empty()) {
        AERO_LOG_ERROR("editor: could not save scene '{}' -- {}", target, reason);
        return false;  // NO setClean, NO setPath -- a save that lies is the worst outcome here (R4)
    }
    commands.setClean();
    session.setPath(target);
    return true;
}

// ---- the flow (A13): performAction, applyFileRequests, applyDialogResult -------------------------

namespace {

// The single sink every discarding path funnels through. File-local: nothing outside this TU calls it
// directly, which is what keeps the save/open paths existing exactly once (A13).
void performAction(FileAction action, CommandContext& context, CommandStack& commands, SceneSession& session,
                   FileFlow& flow, const FileDialogHost& host) {
    switch (action) {
        case FileAction::NewScene:
            newScene(context, commands);
            session.clearPath();
            return;
        case FileAction::OpenScene:
            if (!flow.requestedPath.empty()) {  // D15's test seam: skip the dialog, use this path
                const std::string path = flow.requestedPath;
                flow.requestedPath.clear();
                (void)openSceneFile(context, commands, session, path);
                return;
            }
            if (host.channel != nullptr) {
                flow.dialog = DialogKind::Open;
                launchOpenSceneDialog(host.channel->shared_from_this(), host.parentWindow,
                                      session.dialogDirectory(host.projectRoot));
            }
            // host.channel == nullptr and no requestedPath: A17's silent no-op. flow.dialog stays
            // None -- nothing is left "in flight" for a result that will never arrive.
            return;
        case FileAction::SaveSceneAs:
            flow.saveBeforePending = false;
            if (!flow.requestedPath.empty()) {
                const std::string path = flow.requestedPath;
                flow.requestedPath.clear();
                (void)saveSceneFile(context, commands, session, path, /*appendExtension=*/false);
                return;
            }
            if (host.channel != nullptr) {
                flow.dialog = DialogKind::Save;
                launchSaveSceneDialog(host.channel->shared_from_this(), host.parentWindow,
                                      session.saveSuggestion(host.projectRoot));
            }
            return;
        case FileAction::Quit:
            flow.quitConfirmed = true;
            return;
        case FileAction::SaveScene:  // never reaches here -- SaveScene is resolved by saveStep()
                                     // BEFORE performAction is ever called (applyFileRequests below)
        case FileAction::None:
            return;
    }
}

}  // namespace

void applyFileRequests(CommandContext& context, CommandStack& commands, SceneSession& session, FileFlow& flow,
                       const FileDialogHost& host) {
    // 1. Resolve the modal FIRST -- before the new request, so one call carrying both a modal answer
    //    and a fresh request (SS28) resolves the pending action, then the request, and neither is
    //    left set.
    if (flow.choice.has_value()) {
        const ConfirmChoice choice = *flow.choice;
        flow.choice.reset();
        flow.confirmOpen = false;
        switch (resolveConfirm(choice, session.untitled())) {
            case FileStep::Nothing:
                flow.pending = FileAction::None;  // Cancel/Esc: NOTHING ELSE changes (AC-24)
                break;
            case FileStep::Perform: {  // Don't Save
                const FileAction pending = flow.pending;
                flow.pending = FileAction::None;
                performAction(pending, context, commands, session, flow, host);
                break;
            }
            case FileStep::WriteNow: {  // Save, titled
                const bool ok = saveSceneFile(context, commands, session, session.path(), /*appendExtension=*/false);
                const FileAction pending = flow.pending;
                flow.pending = FileAction::None;
                if (ok) {
                    performAction(pending, context, commands, session, flow, host);
                }
                break;
            }
            case FileStep::AskWhereToSave: {  // Save, untitled -- chains through Save As
                flow.saveBeforePending = true;
                if (!flow.requestedPath.empty()) {
                    const std::string path = flow.requestedPath;
                    flow.requestedPath.clear();
                    const bool ok = saveSceneFile(context, commands, session, path, /*appendExtension=*/false);
                    const FileAction pending = flow.pending;
                    flow.pending = FileAction::None;
                    flow.saveBeforePending = false;
                    if (ok) {
                        performAction(pending, context, commands, session, flow, host);
                    }
                } else if (host.channel != nullptr) {
                    flow.dialog = DialogKind::Save;
                    launchSaveSceneDialog(host.channel->shared_from_this(), host.parentWindow,
                                          session.saveSuggestion(host.projectRoot));
                    // flow.pending stays SET -- applyDialogResult performs it once the write succeeds.
                } else {
                    flow.pending = FileAction::None;  // A17: nothing to launch, nothing to wait for
                    flow.saveBeforePending = false;
                }
                break;
            }
            case FileStep::Confirm:
                break;  // unreachable: resolveConfirm() never returns this
        }
    }

    // 2. Then the new request.
    if (flow.requested == FileAction::None) {
        return;
    }
    const FileAction action = flow.requested;
    flow.requested = FileAction::None;  // a request NEVER survives the frame that carried it

    if (flow.dialog != DialogKind::None) {
        // D8/AC-5: everything File is disabled while a dialog is in flight, so a MENU item drawn
        // disabled can never produce a request here -- only a chord can. Silent for every action
        // except Quit, which gets one INFO (E4): the dialog always calls back eventually, so the
        // editor is never wedged, but the user's quit was still ignored and that is worth a record.
        if (action == FileAction::Quit) {
            AERO_LOG_INFO("editor: quit request ignored -- a file dialog is already open (D8)");
        }
        return;
    }

    if (action == FileAction::SaveScene) {
        switch (saveStep(session.untitled())) {
            case FileStep::WriteNow:
                (void)saveSceneFile(context, commands, session, session.path(), /*appendExtension=*/false);
                return;
            case FileStep::AskWhereToSave:
                flow.saveBeforePending = false;
                if (!flow.requestedPath.empty()) {
                    const std::string path = flow.requestedPath;
                    flow.requestedPath.clear();
                    (void)saveSceneFile(context, commands, session, path, /*appendExtension=*/false);
                } else if (host.channel != nullptr) {
                    flow.dialog = DialogKind::Save;
                    launchSaveSceneDialog(host.channel->shared_from_this(), host.parentWindow,
                                          session.saveSuggestion(host.projectRoot));
                }
                return;
            case FileStep::Nothing:
            case FileStep::Confirm:
            case FileStep::Perform:
                return;  // unreachable: saveStep() never returns any of these
        }
        return;
    }
    if (action == FileAction::SaveSceneAs) {
        performAction(action, context, commands, session, flow, host);
        return;
    }

    // Everything else (NewScene, OpenScene, Quit): the guard decides.
    switch (guardFor(action, !commands.isClean())) {
        case FileStep::Perform:
            performAction(action, context, commands, session, flow, host);
            return;
        case FileStep::Confirm:
            flow.pending = action;
            flow.confirmOpen = true;
            return;
        case FileStep::Nothing:
        case FileStep::WriteNow:
        case FileStep::AskWhereToSave:
            return;  // unreachable: guardFor() never returns either
    }
}

void applyDialogResult(CommandContext& context, CommandStack& commands, SceneSession& session, FileFlow& flow,
                       const FileDialogHost& host, const DialogResult& result) {
    if (!result.ready) {
        return;
    }
    const DialogKind kind = flow.dialog;
    flow.dialog = DialogKind::None;
    if (result.failed) {  // F4/AC-13: exactly one ERROR
        AERO_LOG_ERROR("editor: the system file dialog failed -- {}", "the operation could not be completed");
        flow.pending = FileAction::None;
        flow.saveBeforePending = false;
        return;
    }
    if (result.cancelled) {  // D11: SILENT at every level -- the commonest interaction in this feature
        flow.pending = FileAction::None;
        flow.saveBeforePending = false;
        return;
    }
    if (kind == DialogKind::Open) {
        (void)openSceneFile(context, commands, session, result.path);
        flow.pending = FileAction::None;
        return;
    }
    // kind == DialogKind::Save. appendExtension is true ONLY here -- a native Save panel is the one
    // place a user can type a bare name (D13); requestSaveSceneAs(path) hands a path literally.
    const bool ok = saveSceneFile(context, commands, session, result.path, /*appendExtension=*/true);
    if (ok && flow.saveBeforePending) {
        performAction(flow.pending, context, commands, session, flow, host);
    }
    flow.pending = FileAction::None;
    flow.saveBeforePending = false;
}

}  // namespace engine::editor

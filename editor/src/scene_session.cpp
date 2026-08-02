// Aero Engine — the document model, the pure file-flow guard, and THE scene swap (task 2.5.1). This
// TU is ImGui-FREE and SDL-FREE at source (plan A13): the whole state machine lives here as free
// functions so it is testable with no window and no GPU. <filesystem> and the engine's
// serialization bridge live in text_file.cpp / scene_io.cpp instead (D19/F17, F9's gate).
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

// task 2.6.1: NewProject/OpenProject join NewScene/OpenScene/Quit. This is the ONLY change to any
// pure function's body this task makes -- and it is what buys AC-19 whole (D1).
bool modalInputActive(const FileFlow& flow, const ProjectFlow& projectFlow) noexcept {
    return flow.dialog != DialogKind::None || flow.confirmOpen || projectFlow.form.open;
}

bool discardsWork(FileAction action) noexcept {
    switch (action) {
        case FileAction::NewScene:
        case FileAction::OpenScene:
        case FileAction::Quit:
        case FileAction::NewProject:
        case FileAction::OpenProject:
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

std::string SceneSession::windowTitle(bool dirty, std::string_view projectName) const {
    std::string title;
    if (dirty) {
        title += '*';
    }
    title += documentName();
    if (!projectName.empty()) {
        title += " - ";
        title += projectName;
    }
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

// ---- the two project-opening logging actions (task 2.6.1; mirrors openSceneFile/saveSceneFile as
// the ONLY other places this task logs) -------------------------------------------------------------

namespace {

// THE only thing in this tree that changes the open project (INV-P1). It ALWAYS resets the scene
// FIRST, through the existing one-operation swap -- World, Selection, RootOrder and CommandStack
// cleared together, with no way to perform half of it (F8/INV-6). A project-load flow that swapped
// the World without clearing the stack is precisely what .claude/rules/editor.md names in advance as
// the thing never to add.
void adoptProject(CommandContext& ctx, CommandStack& commands, SceneSession& session, ProjectContext& project,
                  ProjectManifest manifest, std::string root) {
    newScene(ctx, commands);  // resetSceneState + seedDefaultScene, one operation
    session.clearPath();      // the previous scene's path belonged to the previous project
    project.session.set(std::move(manifest), root);
    promoteRecent(project.recents, std::move(root));  // AFTER the line above -- that one COPIES
                                                      // `root`, this one moves it. Never move twice.
    project.flow.recentsDirty = true;                 // without this, AC-22 never persists
}

}  // namespace

bool openProjectPath(CommandContext& context, CommandStack& commands, SceneSession& session, ProjectContext& project,
                     std::string_view pathUtf8) {
    const ProjectLoadOutcome outcome = loadProjectFrom(pathUtf8);
    if (!outcome.ok) {
        std::string reason = outcome.message;
        if (outcome.line > 0) {
            reason += " (line " + std::to_string(outcome.line) + ", column " + std::to_string(outcome.column) + ")";
        }
        AERO_LOG_ERROR("editor: could not open project '{}' -- {}", pathUtf8, reason);
        return false;  // NOTHING changed: no session, no scene, no recents (AC-8)
    }
    for (const std::string& key : outcome.unknownKeys) {
        AERO_LOG_WARN("editor: project '{}' -- ignoring unknown key \"{}\"", outcome.root, key);
    }
    if (!project.engineVersion.empty() && outcome.manifest.engineVersion != project.engineVersion) {
        // D14: informational only -- never compared for ordering, never a gate. The !empty() guard
        // keeps a test context (which passes "") silent.
        AERO_LOG_WARN("editor: project '{}' was created with engine version {} (this build is {})", outcome.root,
                      outcome.manifest.engineVersion, project.engineVersion);
    }
    adoptProject(context, commands, session, project, outcome.manifest, outcome.root);  // ONLY after
                                                                                        // every check passed
    AERO_LOG_INFO("editor: opened project '{}' at '{}' -- assets '{}', scenes '{}'", project.session.name(),
                  project.session.root(), project.session.assetsRoot(), project.session.scenesRoot());
    return true;
}

bool createAndOpenProject(CommandContext& context, CommandStack& commands, SceneSession& session,
                          ProjectContext& project, std::string_view location, std::string_view name) {
    const ProjectCreateOutcome outcome = createProject(location, name, project.engineVersion);
    if (outcome.problem != CreateProblem::Ok) {
        AERO_LOG_ERROR("editor: could not create project '{}' -- {}", name, outcome.message);
        return false;  // D7: nothing was removed, and nothing is switched to
    }
    // NEVER re-reads the file it just wrote (A24) -- that would be a second INFO; AC-15 says one.
    adoptProject(context, commands, session, project, outcome.manifest, outcome.root);
    AERO_LOG_INFO("editor: created project '{}' at '{}'", outcome.manifest.name, outcome.root);
    return true;
}

// ---- the flow (A13): performAction, applyFileRequests, applyDialogResult -------------------------

namespace {

// The single sink every discarding path funnels through. File-local: nothing outside this TU calls it
// directly, which is what keeps the save/open paths existing exactly once (A13).
void performAction(FileAction action, CommandContext& context, CommandStack& commands, SceneSession& session,
                   FileFlow& flow, const FileDialogHost& host, ProjectContext& project) {
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
        case FileAction::NewProject:
            // Opens the FORM. Creates nothing, launches no dialog, touches no disk. Cancelling it
            // leaves the (possibly dirty) scene exactly as it was -- which is why guard-FIRST is safe
            // here (D1).
            project.flow.form = {};
            project.flow.form.open = true;
            if (project.session.isOpen()) {
                project.flow.form.location = std::string(directoryOf(project.session.root()));
            }
            return;
        case FileAction::OpenProject:
            if (!project.flow.requestedPath.empty()) {  // the no-dialog seam, exactly OpenScene's shape
                const std::string path = project.flow.requestedPath;
                project.flow.requestedPath.clear();
                (void)openProjectPath(context, commands, session, project, path);
                return;
            }
            if (host.channel != nullptr) {
                flow.dialog = DialogKind::ProjectFolder;
                launchOpenProjectFolderDialog(host.channel->shared_from_this(), host.parentWindow,
                                              std::string(project.session.root()));
            }
            // No channel and no requestedPath: A17's silent no-op. flow.dialog stays None -- nothing
            // is left "in flight" for a result that will never arrive.
            return;
        case FileAction::SaveScene:  // never reaches here -- SaveScene is resolved by saveStep()
                                     // BEFORE performAction is ever called (applyFileRequests below)
        case FileAction::None:
            return;
    }
}

}  // namespace

void applyFileRequests(CommandContext& context, CommandStack& commands, SceneSession& session, FileFlow& flow,
                       const FileDialogHost& host, ProjectContext& project) {
    // 0. The New Project form's own four requests -- BEFORE the modal-answer block below. Ordering
    //    note, load-bearing: `createRequested` can only be set while `form.open` is true, and
    //    `form.open` is exactly what the refusal check below tests. Running these afterwards would
    //    let one frame's Create be swallowed by its own form.
    if (project.flow.form.cancelRequested) {
        project.flow.form = {};  // closes the modal, discards
    }
    if (project.flow.form.browseRequested) {
        project.flow.form.browseRequested = false;
        // NOT guarded by the unsaved-changes interlock: the guard was already answered before the
        // form opened. With no channel, A17's silent no-op applies exactly as every other launcher.
        //
        // SHOULD-FIX 5 (code review): `flow.dialog == DialogKind::None` is ALSO required -- this
        // consumer runs at step 0, BEFORE the refusal check further down (:468's
        // `flow.dialog != DialogKind::None || ...`), and unlike every other launcher it had no
        // in-flight guard of its own. A second Browse click before the first dialog answers would
        // otherwise overwrite `flow.dialog` and launch a SECOND native dialog; `DialogChannel::take()`
        // resets its slot on every read, so whichever result answers SECOND is consumed with
        // `flow.dialog` already reset to None by the first -- and applyDialogResult's kind chain used
        // to have no arm for that, falling through into the Save arm below and writing the scene to
        // "<picked folder>.scene.json" (2.5.1's BLOCKING-2 again). Dropping a second Browse while one
        // is already in flight is a silent no-op, exactly A17's shape for every other launcher.
        if (host.channel != nullptr && flow.dialog == DialogKind::None) {
            flow.dialog = DialogKind::ProjectLocation;
            const std::string startDir =
                project.flow.form.location.empty() ? std::string(project.session.root()) : project.flow.form.location;
            launchOpenProjectFolderDialog(host.channel->shared_from_this(), host.parentWindow, startDir);
        }
    }
    if (project.flow.form.createRequested) {
        project.flow.form.createRequested = false;
        const bool ok = createAndOpenProject(context, commands, session, project, project.flow.form.location,
                                             project.flow.form.name);
        if (ok) {
            project.flow.form = {};  // only SUCCESS closes the modal
        } else {
            // LEAVE THE MODAL OPEN with the typed name intact -- createAndOpenProject already logged
            // the real reason to the Console; this is the inline fallback for the failure modes the
            // live name/location validation cannot see in advance (a race, a write failure, ...).
            project.flow.form.error = "could not create the project -- see the Console for details";
        }
    }
    if (project.flow.clearRecentsRequested) {
        project.flow.clearRecentsRequested = false;
        project.recents.paths.clear();
        project.flow.recentsDirty = true;
    }

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
                // BLOCKING-1 (code review): `flow.requestedPath` may be the ABANDONED pending
                // action's OWN target (e.g. a deferred OpenScene's file) -- it must not survive to be
                // mistaken for a later, unrelated request's path. Cleared on every path that abandons
                // or defers below, never only where it happens to be read. task 2.6.1 widens this the
                // same way: the pending action could equally be a deferred NewProject/OpenProject,
                // whose own target lives in `project.flow.requestedPath`, a DIFFERENT flow object's
                // field -- clearing both is always safe (the one that was not in use is already empty).
                flow.requestedPath.clear();
                project.flow.requestedPath.clear();
                break;
            case FileStep::Perform: {  // Don't Save
                const FileAction pending = flow.pending;
                flow.pending = FileAction::None;
                performAction(pending, context, commands, session, flow, host, project);
                break;
            }
            case FileStep::WriteNow: {  // Save, titled
                const bool ok = saveSceneFile(context, commands, session, session.path(), /*appendExtension=*/false);
                const FileAction pending = flow.pending;
                flow.pending = FileAction::None;
                if (ok) {
                    performAction(pending, context, commands, session, flow, host, project);
                } else {
                    flow.requestedPath.clear();          // the pending action is ABANDONED, not performed --
                    project.flow.requestedPath.clear();  // its own target (if any) must not leak,
                                                         // whichever flow object it lives in (BLOCKING-1)
                }
                break;
            }
            case FileStep::AskWhereToSave: {  // Save, untitled -- chains through Save As
                // BLOCKING-1 (code review): this branch used to read `flow.requestedPath` as ITS OWN
                // save target, sharing the field with `flow.pending`'s (e.g. a deferred OpenScene's)
                // own target -- that is exactly what let "Open X (guarded, dirty) -> modal Save" write
                // the CURRENT scene over X. No hook in this tree ever supplies a direct save target for
                // an untitled Save reached THIS way (only requestSaveSceneAs(path), a different, never
                // guarded action, does that), so this branch no longer consults `requestedPath` at
                // all: it either launches the real Save dialog or -- with no channel to launch one --
                // abandons the pending action outright, exactly like every other no-channel arm (A17).
                flow.saveBeforePending = true;
                if (host.channel != nullptr) {
                    flow.dialog = DialogKind::Save;
                    launchSaveSceneDialog(host.channel->shared_from_this(), host.parentWindow,
                                          session.saveSuggestion(host.projectRoot));
                    // flow.pending AND flow.requestedPath stay SET -- applyDialogResult performs the
                    // pending action (consuming requestedPath itself, if any) once the write succeeds.
                } else {
                    flow.pending = FileAction::None;  // A17: nothing to launch, nothing to wait for
                    flow.saveBeforePending = false;
                    flow.requestedPath.clear();          // the pending action is ABANDONED (BLOCKING-1)
                    project.flow.requestedPath.clear();  // whichever flow object it lives in
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

    if (modalInputActive(flow, project.flow)) {
        // D8/AC-5, widened by BLOCKING-2 (code review) and again by task 2.6.1: everything File is
        // disabled while a NATIVE dialog is in flight, the unsaved-changes MODAL is up, OR the New
        // Project form is up -- `shell_ui.cpp`'s `fileEnabled` mirrors this exactly (defence in
        // depth), so a MENU item drawn disabled can never produce a request here, only a chord (or a
        // raw request*() call bypassing the UI entirely) can. Before BLOCKING-2 widened this, a
        // dirty+untitled Quit could raise the modal (confirmOpen=true, flow.dialog still None) and a
        // Ctrl+S fired WHILE it was showing would fall straight through to AskWhereToSave and launch a
        // second, native Save dialog on top of the still-open ImGui modal. Silent for every action
        // except Quit, which gets one INFO (E4): the flow always resolves eventually (the modal answers
        // it, or the dialog calls back), so the editor is never wedged, but the user's quit was still
        // ignored and that is worth a record.
        if (action == FileAction::Quit) {
            AERO_LOG_INFO(
                "editor: quit request ignored -- a file dialog or the unsaved-changes modal is already up (D8)");
        }
        project.flow.requestedPath.clear();  // a swallowed request's own target must not leak
        flow.requestedPath.clear();          // (BLOCKING-1)
        return;
    }

    if (action == FileAction::SaveScene) {
        switch (saveStep(session.untitled())) {
            case FileStep::WriteNow:
                (void)saveSceneFile(context, commands, session, session.path(), /*appendExtension=*/false);
                return;
            case FileStep::AskWhereToSave:
                // BLOCKING-1 (code review): no hook ever sets `flow.requestedPath` for a plain SaveScene
                // request (only `requestOpenScene(path)`/`requestSaveSceneAs(path)` do, for a DIFFERENT
                // action each), so reading it here could only ever pick up a STALE value left behind by
                // some other, unrelated request. Always launch the real dialog, or no-op with no channel
                // (A17) -- exactly the AskWhereToSave arm above, mirrored for the un-guarded SaveScene
                // path.
                flow.saveBeforePending = false;
                if (host.channel != nullptr) {
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
        performAction(action, context, commands, session, flow, host, project);
        return;
    }

    // Everything else (NewScene, OpenScene, Quit, NewProject, OpenProject): the guard decides.
    switch (guardFor(action, !commands.isClean())) {
        case FileStep::Perform:
            performAction(action, context, commands, session, flow, host, project);
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
                       const FileDialogHost& host, const DialogResult& result, ProjectContext& project) {
    if (!result.ready) {
        return;
    }
    const DialogKind kind = flow.dialog;
    flow.dialog = DialogKind::None;
    if (kind == DialogKind::None) {
        // SHOULD-FIX 5 (code review): an ORPHANED result -- from a dialog launch that got superseded
        // before it answered, so whichever result claims `flow.dialog` FIRST resets it to None for the
        // one that answers SECOND (DialogChannel::take() always resets its slot). Every arm below
        // assumes `kind` names a real, still-in-flight dialog; the final one does not even check its
        // own `kind == DialogKind::Save` explicitly (a comment states it "by elimination"), so an
        // orphan used to fall straight through into it and save the CURRENT scene to a random picked
        // path (2.5.1's BLOCKING-2 again, reached through 2.6.1's Browse). Dropped here, silently,
        // before touching flow.pending/saveBeforePending/requestedPath -- none of those belong to this
        // orphan, and clearing them would incorrectly abandon a genuinely unrelated pending action.
        return;
    }
    if (result.failed) {  // F4/AC-13: exactly one ERROR
        AERO_LOG_ERROR("editor: the system file dialog failed -- {}", "the operation could not be completed");
        flow.pending = FileAction::None;
        flow.saveBeforePending = false;
        flow.requestedPath.clear();          // the pending action is ABANDONED here too (BLOCKING-1)
        project.flow.requestedPath.clear();  // whichever flow object its own target lives in
        return;
    }
    if (result.cancelled) {  // D11: SILENT at every level -- the commonest interaction in this feature
        flow.pending = FileAction::None;
        flow.saveBeforePending = false;
        flow.requestedPath.clear();          // the pending action is ABANDONED here too (BLOCKING-1)
        project.flow.requestedPath.clear();  // whichever flow object its own target lives in
        return;
    }
    if (kind == DialogKind::Open) {
        (void)openSceneFile(context, commands, session, result.path);
        flow.pending = FileAction::None;
        return;
    }
    // task 2.6.1, +2 arms, both after the failed/cancelled handling above -- a cancelled folder pick
    // stays silent (E18/E19).
    if (kind == DialogKind::ProjectFolder) {
        (void)openProjectPath(context, commands, session, project, result.path);
        flow.pending = FileAction::None;
        return;
    }
    if (kind == DialogKind::ProjectLocation) {
        project.flow.form.location = result.path;  // and NOTHING else -- the modal stays up and the
        flow.pending = FileAction::None;           // user still has to press Create (E19)
        return;
    }
    // kind == DialogKind::Save. appendExtension is true ONLY here -- a native Save panel is the one
    // place a user can type a bare name (D13); requestSaveSceneAs(path) hands a path literally.
    const bool ok = saveSceneFile(context, commands, session, result.path, /*appendExtension=*/true);
    if (ok && flow.saveBeforePending) {
        performAction(flow.pending, context, commands, session, flow, host, project);
    }
    flow.pending = FileAction::None;
    flow.saveBeforePending = false;
}

}  // namespace engine::editor

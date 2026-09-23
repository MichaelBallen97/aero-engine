// Aero Engine — the document model, the pure file-flow guard, and THE scene swap (task 2.5.1). This
// TU is ImGui-FREE and SDL-FREE at source (plan A13): the whole state machine lives here as free
// functions so it is testable with no window and no GPU. <filesystem> and the engine's
// serialization bridge live in text_file.cpp / scene_io.cpp instead (D19/F17, F9's gate).
#include <aero/core/log.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/project_state.hpp>      // task E.4.1: the per-project state, its resolver and its
                                              // path arithmetic. ProjectSession itself arrives through
                                              // scene_session.hpp -> project.hpp.
#include <aero/editor/scene_containment.hpp>  // task E.4.2: the containment verdict and its one sentence
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
    return flow.dialog != DialogKind::None || flow.confirmOpen || projectFlow.form.open ||
           flow.containmentOffer.open;  // task E.4.2 -- the THIRD modal. Omitting it is 2.5.1's
                                        // BLOCKING-2 in a new costume: a Ctrl+S behind the modal
                                        // launching a second native dialog on top of it (D10).
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

namespace {

// task E.4.2: fill the offer a refusal raises, or do nothing at all when the caller supplied none.
// File-local: the two choke points directly below are its ONLY callers, and a refusal is the ONLY
// thing that may write these fields -- a second writer anywhere would be a second offer policy with
// no way to order it against this one (E.3.2's one-focus-slot rule, applied to a modal).
//
// The serial is bumped on EVERY raise, including one that overwrites an offer still on screen: it is
// what EditorApp's monotonic counter mirrors, and a raise that did not bump would be a refusal the
// GPU tier cannot see. The two IN one-shots are deliberately NOT touched -- a raise cannot answer
// itself, and clearing them here would silently swallow an accept the user pressed on the previous
// offer in the same tick.
void raiseContainmentOffer(ContainmentOffer* offer, std::string_view pathUtf8, const std::string& reason,
                           const ContainmentVerdict& verdict, bool forSave) {
    if (offer == nullptr) {
        return;  // D12: refuse to the log and raise nothing -- every test and every non-UI caller
    }
    const std::size_t serial = offer->refusalSerial;
    offer->open = true;
    offer->forSave = forSave;
    offer->scenePath = std::string(pathUtf8);
    offer->reason = reason;  // containmentReason's OWN bytes, never a second wording (D6/AC-20)
    // D9: a refused SAVE offers nothing, and BOTH terms are spelled. The resolver does not even
    // perform the walk for a save, so `verdict.owningProjectRoot` is empty there today -- but a
    // future resolver change must not silently grow a button, so the guard lives here too.
    offer->projectRoot = forSave ? std::string() : verdict.owningProjectRoot;
    offer->projectName = forSave ? std::string() : verdict.owningProjectName;
    offer->refusalSerial = serial + 1U;
}

}  // namespace

bool openSceneFile(CommandContext& context, CommandStack& commands, SceneSession& session,
                   std::string_view absolutePathUtf8, const SceneFileContext& fileContext) {
    const std::string path(absolutePathUtf8);
    // task E.4.2 (D6): FIRST -- before readTextFile, so a refused open performs NO I/O AT ALL and
    // leaves the World, the Selection, the RootOrder, the clean flag and session.path() byte-identical
    // (AC-4). findOwningProject = TRUE: an open is the one refusal that can offer a project (D9).
    const ContainmentVerdict verdict =
        resolveSceneContainment(path, fileContext.projectRoot, /*findOwningProject=*/true);
    if (!containmentPermits(verdict.state)) {
        const std::string reason = containmentReason(verdict, fileContext.projectRoot, /*forSave=*/false);
        AERO_LOG_ERROR("editor: could not open scene '{}' -- {}", path, reason);  // exactly ONE
        raiseContainmentOffer(fileContext.offer, path, reason, verdict, /*forSave=*/false);
        return false;
    }
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
                   std::string_view absolutePathUtf8, bool appendExtension, const SceneFileContext& fileContext) {
    // task E.4.2 (D7): the extension rule MOVES ABOVE the serialization, so containment is checked on
    // the file that will actually be WRITTEN rather than on the argument. Both live in the same
    // directory, so today the verdict is the same either way -- but checking the written thing is the
    // only version of this that stays true if withSceneExtension ever changes. IO20 pins it; S18 seeds
    // the raw argument back in.
    const std::string target = appendExtension ? withSceneExtension(absolutePathUtf8) : std::string(absolutePathUtf8);
    // findOwningProject = FALSE (D9): a refused save NEVER offers a project, because accepting one
    // routes through adoptProject (:259-267) -> newScene (:261) -> World::clear() +
    // CommandStack::clear() and would discard the very work being saved. There is nothing to offer, so
    // there is nothing to look up, and a refused save costs no extra filesystem call at all.
    const ContainmentVerdict verdict =
        resolveSceneContainment(target, fileContext.projectRoot, /*findOwningProject=*/false);
    if (!containmentPermits(verdict.state)) {
        const std::string reason = containmentReason(verdict, fileContext.projectRoot, /*forSave=*/true);
        AERO_LOG_ERROR("editor: could not save scene '{}' -- {}", target, reason);
        // `target`, NEVER absolutePathUtf8 -- the modal shows the file that would have been written,
        // which is the same string the ERROR one line above named (D7/IO20).
        raiseContainmentOffer(fileContext.offer, target, reason, verdict, /*forSave=*/true);
        return false;  // NO setClean, NO setPath -- a save that lies is the worst outcome here (R4)
    }
    const std::optional<std::string> text = sceneToText(context.world);
    if (!text.has_value()) {
        // task E.4.2: names `target`, not the argument -- `target` now exists above and the D13 refusal
        // two lines below already names it, so one function would otherwise spell "the file" two ways.
        AERO_LOG_ERROR("editor: could not save scene '{}' -- {}", target, "built without AERO_REFLECT_TOOLS");
        return false;
    }
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

// task E.4.1. Runs ONLY from openProjectPath, ONLY after adoptProject, and exactly ONCE per open.
// createAndOpenProject does not call it: createProject refuses a non-empty target
// (project_file.cpp:213-217), so a fresh project has no Library/, no state file and no scenes BY
// CONSTRUCTION, and a call there could only ever be a no-op (F2).
//
// IT CANNOT CHANGE THE PROJECT, and that is enforced rather than intended: its parameter is a
// `const ProjectSession&`, so INV-P1's setter is not reachable from here at all -- 2.6.2's
// PanelContext::project rule ("the const is the enforcement"), one layer down. It only chooses a scene
// and opens it through the ONE existing path, so INV-P1 and INV-6 are both untouched.
//
// The parameter is `projectSession`, NEVER `session`: `session` is the SceneSession above it.
void restoreLastScene(CommandContext& ctx, CommandStack& commands, SceneSession& session,
                      const ProjectSession& projectSession) {
    // A NAMED LOCAL, FIRST: root() returns a std::string_view into the live session (project.hpp:100-102,
    // and 2.6.1's FileDialogHost::projectRoot lesson), and everything below outlives the full-expression.
    const std::string root(projectSession.root());
    bool corrupt = false;
    const ProjectState state = readProjectState(root, corrupt);
    if (corrupt) {
        // A MISSING file is silent; only one that EXISTS and cannot be read or parsed gets this line,
        // or every first open of every project warns. UNCONDITIONAL -- ABOVE the sceneIoAvailable gate
        // on purpose (D10), so the diagnostic is identical in all three build configurations and is
        // assertable from the UNGATED project_test.cpp (PJ41). Putting the gate first would silence it
        // in the tools-OFF build, a behaviour difference no test could see without an #if, which test
        // files in this tree may not contain.
        AERO_LOG_WARN("editor: project state '{}' is unreadable or unsupported -- starting a new scene",
                      projectStatePath(root));
    }
    if (!sceneIoAvailable()) {
        return;  // .claude/rules/editor.md: every native scene I/O call site checks this FIRST.
    }
    const std::string recordedAbsolute = absoluteScenePath(root, state.lastScene);
    // UNCONDITIONAL, even when the recorded scene will win (plan 4.1): chooseStartupScene must receive
    // a TRUE firstSceneFound or PJ25's "the decider ignores a fact it should ignore" rows stop being an
    // assertion. One opendir per project open, against a tick-1 rescan of the whole assets tree.
    const std::string firstRelative = firstSceneUnder(root, projectSession.manifest().scenesPath);
    const StartupSceneFacts facts{.recorded = state.lastSceneRecorded,
                                  .recordedEmpty = state.lastScene.empty(),
                                  .recordedExists = !recordedAbsolute.empty() && fileExists(recordedAbsolute),
                                  .firstSceneFound = !firstRelative.empty()};
    switch (chooseStartupScene(facts)) {
        case StartupScene::NewScene:
            return;  // adoptProject already left exactly this: a seeded scene, no path, a clean history
        case StartupScene::Recorded:
            // ONE attempt, ever. A failure logs one ERROR inside openSceneFile and leaves the seeded
            // scene untouched (PARSE FIRST, THEN SWAP -- scene_io.cpp's rule; openSceneFile calls
            // setPath only after the load succeeded). That IS D6's "stop": it needs no code, only the
            // absence of a second try.
            (void)openSceneFile(ctx, commands, session, recordedAbsolute, SceneFileContext{root, nullptr});
            return;
        case StartupScene::FirstUnderScenes:
            (void)openSceneFile(ctx, commands, session, absoluteScenePath(root, firstRelative),
                                SceneFileContext{root, nullptr});
            return;
    }
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
    // task E.4.1 (code review): HAND OFF the OUTGOING project's (root, scene) pair before the adopt,
    // while both halves still exist. A guarded Save can chain into this open inside ONE tick --
    // applyDialogResult saves an untitled scene INTO the outgoing project and then performs the
    // pending OpenProject in the same call -- and that pair is invisible at both ends of the tick, so
    // the end-of-tick reconcile would see only a root change, take AdoptBaseline and write nothing.
    // EditorApp::persistProjectState drains this once per tick and is still the ONE write site; this
    // writes nothing, reads no file and logs nothing.
    //
    // ABOVE the adopt and OUTSIDE it, both deliberately: `set()` is what makes root() name the NEW
    // project, and adoptProject's five statements are byte-identical to what 2.6.1 shipped (AC-26) --
    // it stays the one operation that changes the project and does nothing else.
    if (project.session.isOpen()) {  // nothing to hand off when this is the first open of the session
        // A NAMED LOCAL FIRST: root() is a VIEW into the member `set()` is about to overwrite.
        std::string outgoingRoot(project.session.root());
        // The RECORDED form -- project-relative, exactly what persistProjectState compares against its
        // baseline and what parseProjectState can read back. "" when the scene is untitled or lives
        // outside the project, which is a legitimate value to record (D2's third state).
        project.flow.outgoingStateScene = projectRelativeScenePath(outgoingRoot, session.path());
        project.flow.outgoingStateRoot = std::move(outgoingRoot);  // LAST: non-empty IS the flag
    }
    adoptProject(context, commands, session, project, outcome.manifest, outcome.root);  // ONLY after
                                                                                        // every check passed
    AERO_LOG_INFO("editor: opened project '{}' at '{}' -- assets '{}', scenes '{}'", project.session.name(),
                  project.session.root(), project.session.assetsRoot(), project.session.scenesRoot());
    // task E.4.1: AFTER the adopt (F1 -- the state file's path is derived from project.session.root(),
    // which only set() makes correct; calling it before would read the OUTGOING project's state) and
    // AFTER the INFO (plan 4.4), so the Console reads "opened project" and THEN the scene's own lines.
    // With the INFO last, a restore whose candidate fails to open reads "could not open scene X"
    // followed by "opened project Y", which a reader parses as the PROJECT having failed.
    restoreLastScene(context, commands, session, project.session);
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
                (void)openSceneFile(context, commands, session, path,
                                    SceneFileContext{project.session.root(), &flow.containmentOffer});
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
                (void)saveSceneFile(context, commands, session, path, /*appendExtension=*/false,
                                    SceneFileContext{project.session.root(), &flow.containmentOffer});
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
    //
    // 0a. task E.4.2 (D10): the containment offer's two answers. HERE, at step 0, for the reason the
    //     block comment above already gives: step 2's modalInputActive refusal tests
    //     `containmentOffer.open`, so draining later would let the very modal the user just answered
    //     swallow the request it produced -- a modal that can never be answered. S11 is the seed.
    if (flow.containmentOffer.dismissRequested) {
        flow.containmentOffer = {};  // closes the modal, offers nothing, changes nothing else
    }
    if (flow.containmentOffer.acceptRequested) {
        // Re-tested HERE, not merely at the button that is drawn: the button only EXISTS when
        // !forSave && !projectRoot.empty(), but a raw request hook can set acceptRequested on a
        // save-shaped offer, and that must not reach adoptProject -> newScene -> World::clear()
        // (D9). SS48's second arm is what proves the re-test, by setting the flag directly.
        const bool offerable = !flow.containmentOffer.forSave && !flow.containmentOffer.projectRoot.empty();
        std::string root = std::move(flow.containmentOffer.projectRoot);
        flow.containmentOffer = {};  // BEFORE the request, so modalInputActive is ALREADY false when
                                     // step 2 tests it -- S25 is the seed for getting this backwards
        if (offerable) {
            project.flow.requestedPath = std::move(root);
            flow.requested = FileAction::OpenProject;  // step 2 applies guardFor, so a DIRTY scene
        }  // raises the unsaved-changes modal FIRST (D10)
    }
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
                const bool ok = saveSceneFile(context, commands, session, session.path(), /*appendExtension=*/false,
                                              SceneFileContext{project.session.root(), &flow.containmentOffer});
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
                (void)saveSceneFile(context, commands, session, session.path(), /*appendExtension=*/false,
                                    SceneFileContext{project.session.root(), &flow.containmentOffer});
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
        (void)openSceneFile(context, commands, session, result.path,
                            SceneFileContext{project.session.root(), &flow.containmentOffer});
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
    // task 3.2.4, +1 arm, and its PLACEMENT is the whole point: it sits with the other `kind ==` arms
    // and BEFORE the terminal Save fall-through, which is reached BY ELIMINATION and would otherwise
    // save the current scene to the picked binary's path -- 2.5.1's BLOCKING-2 in a new costume, and
    // exactly what SHOULD-FIX 5's orphan guard above exists for.
    if (kind == DialogKind::BlenderBinary) {
        flow.pickedBlenderPath = result.path;  // and NOTHING else -- applied by tick()'s reconcile (D13)
        flow.pending = FileAction::None;
        return;
    }
    // kind == DialogKind::Save. appendExtension is true ONLY here -- a native Save panel is the one
    // place a user can type a bare name (D13); requestSaveSceneAs(path) hands a path literally.
    const bool ok = saveSceneFile(context, commands, session, result.path, /*appendExtension=*/true,
                                  SceneFileContext{project.session.root(), &flow.containmentOffer});
    if (ok && flow.saveBeforePending) {
        performAction(flow.pending, context, commands, session, flow, host, project);
    }
    flow.pending = FileAction::None;
    flow.saveBeforePending = false;
}

}  // namespace engine::editor

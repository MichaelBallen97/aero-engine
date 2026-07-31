// Aero Engine — the document model, the pure file-flow guard, and THE scene swap (task 2.5.1). This
// TU is ImGui-FREE and SDL-FREE at source (plan A13): the whole state machine lives here as free
// functions so it is testable with no window and no GPU. <filesystem> and scene_serialize live in
// scene_file.cpp / scene_io.cpp instead (D19/F17, F9's gate).
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/world.hpp>

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

}  // namespace engine::editor

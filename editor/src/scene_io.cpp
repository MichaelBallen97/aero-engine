// Aero Engine — the editor's scene_serialize bridge (task 2.5.1, D18/F9/F10). THE ONE editor TU that
// gates on the reflection-tooling macro below -- four occurrences, matching editor_reflection.cpp's
// established shape. NO <aero/core/log.hpp> -- this TU returns status and never prints it:
// SceneOpenOutcome deliberately does not carry the path, and a load record that cannot name the file
// it loaded is useless in the Log panel. The CALLER (scene_session.cpp's openSceneFile/saveSceneFile)
// logs instead, because only it has both the path and the outcome (project_files.hpp:15-16's "status
// is RETURNED, never printed" convention, applied a second time).
#include <aero/editor/scene_session.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#if defined(AERO_EDITOR_REFLECTION)
    #include <aero/scene_serialize/scene_serialize.hpp>  // reachable ONLY in this configuration (F9)
#endif

namespace engine::editor {

bool sceneIoAvailable() noexcept {
#if defined(AERO_EDITOR_REFLECTION)
    return true;
#else
    return false;
#endif
}

SceneOpenOutcome openSceneText(CommandContext& context, CommandStack& commands, std::string_view text) {
#if defined(AERO_EDITOR_REFLECTION)
    // PARSE FIRST, into a document, WITHOUT touching the World (D5). scene_serialize::loadSceneText
    // would already refuse before mutating -- but doing the parse HERE makes the ordering VISIBLE and
    // lets the reset happen between the two halves rather than being trusted to a sibling module.
    const SceneParseResult parsed = parseScene(text);
    // NOT `!parsed.ok()`: bugprone-unchecked-optional-access cannot connect an opaque out-of-line ok()
    // to `document`, and it would flag the deref below on the Linux lint lane (scene_serialize.cpp's
    // loadSceneText carries the identical comment for the identical reason).
    if (!parsed.document.has_value()) {
        return {.ok = false, .message = parsed.error.message, .line = parsed.error.line, .column = parsed.error.column};
    }
    resetSceneState(context, commands);  // <-- THE ONE swap, and only now (D2)
    const scene_serialize::SceneLoadReport report = scene_serialize::loadScene(context.world, *parsed.document);
    commands.clear();  // belt-and-braces: leaves it CLEAN (F7) -- not redundant paranoia, see the header
    return {.ok = true,
            .entities = report.entitiesCreated,
            .components = report.componentsAttached,
            .skipped = report.componentsSkipped,
            .failed = report.componentsFailed};
#else
    (void)context;
    (void)commands;
    (void)text;
    return {.ok = false, .message = "built without AERO_REFLECT_TOOLS"};
#endif
}

std::optional<std::string> sceneToText(const World& world) {
#if defined(AERO_EDITOR_REFLECTION)
    return scene_serialize::saveWorldText(world);
#else
    (void)world;
    return std::nullopt;
#endif
}

}  // namespace engine::editor

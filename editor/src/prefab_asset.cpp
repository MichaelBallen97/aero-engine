// Aero Engine — Save as Prefab below EditorApp (task 4.4.4). See prefab_asset.hpp: the third assets-root
// writeTextFileAtomic call site, from the named local `prefabAbsolutePath`. This file touches no directory API, deletes
// nothing and logs nothing; the caller owns the INFO and the WARN.
#include <aero/editor/asset_actions.hpp>
#include <aero/editor/asset_view.hpp>
#include <aero/editor/prefab_asset.hpp>
#include <aero/editor/project_files.hpp>
#include <aero/editor/scene_session.hpp>
#include <aero/editor/text_file.hpp>
#include <aero/scene/prefab.hpp>

#include <optional>
#include <vector>

namespace engine::editor {

namespace {

[[nodiscard]] PrefabSaveResult refusal(std::string_view why) {
    PrefabSaveResult result;
    result.error = std::string(why);
    return result;
}

}  // namespace

std::string prefabFileStem(std::string_view rootName) {
    std::string withSuffix(rootName);
    withSuffix += PREFAB_FILE_SUFFIX;
    if (validateAssetName(rootName) != AssetNameRefusal::None ||
        validateAssetName(withSuffix) != AssetNameRefusal::None) {
        return std::string(FALLBACK_PREFAB_STEM);
    }
    return std::string(rootName);
}

PrefabSaveResult savePrefabFile(const World& world, std::span<const Entity> roots, std::string_view assetsRoot,
                                std::string_view directoryRel) {
    // An empty root is the blenderExportDir() lesson: concatenating onto it names "/Crate.aeroprefab", an absolute path
    // at the filesystem root.
    if (assetsRoot.empty()) {
        return refusal("no project is open");
    }
    if (!sceneIoAvailable()) {
        return refusal("scene I/O is not available in this build");
    }
    if (roots.empty()) {
        return refusal("nothing to save");
    }
    // listingIsComplete, never `status == Ok`: a listing that hit a cap or lost its iterator keeps Ok and hands back a
    // PREFIX, and a prefix cannot prove a name is free. Hidden entries are included, and nothing is filtered by the
    // ignore roster, because a file the scan ignores still owns its name.
    const DirectoryListing listing = listDirectory(assetsRoot, directoryRel, true);
    if (!listingIsComplete(listing)) {
        return refusal("the folder could not be read in full");
    }
    std::vector<std::string_view> taken;
    taken.reserve(listing.entries.size());
    for (const FileEntry& entry : listing.entries) {
        taken.emplace_back(entry.name);
    }
    const std::string fileName = uniqueAssetFileName(prefabFileStem(world.name(roots.front())), PREFAB_FILE_SUFFIX,
                                                     taken, MAX_NEW_PREFAB_ATTEMPTS);
    if (fileName.empty()) {
        return refusal("no unused name after " + std::to_string(MAX_NEW_PREFAB_ATTEMPTS) + " attempts");
    }
    // The wrapper's name is the chosen file's stem, so a Crate-2 file reads Crate-2.
    const std::string_view chosenStem =
        std::string_view(fileName).substr(0, fileName.size() - PREFAB_FILE_SUFFIX.size());
    const std::optional<Prefab> prefab = Prefab::capture(world, roots, chosenStem);
    if (!prefab.has_value()) {
        return refusal("nothing to save");
    }
    const std::optional<std::string> text = prefabToText(*prefab);
    if (!text.has_value()) {
        return refusal("scene I/O is not available in this build");
    }
    const std::string relativePath = joinRelative(directoryRel, fileName);
    // A NAMED LOCAL, exactly as the amended INV-A1 requires of every path handed to the one write path.
    const std::string prefabAbsolutePath = std::string(assetsRoot) + "/" + relativePath;
    if (std::string error = writeTextFileAtomic(prefabAbsolutePath, *text); !error.empty()) {
        return refusal(error);
    }
    PrefabSaveResult saved;
    saved.relativePath = relativePath;
    return saved;
}

}  // namespace engine::editor

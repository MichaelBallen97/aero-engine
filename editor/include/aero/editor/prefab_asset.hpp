#pragma once
// Aero Engine — Save as Prefab below EditorApp (task 4.4.4). PUBLIC, ImGui-free and tier-0: a function of a World, a
// root and a folder, reachable from aero_editor_shell_test with a TempDir and no window.
//
// THE THIRD assets-root writeTextFileAtomic call site, after the scene saves' and the sidecars': the write is in
// prefab_asset.cpp, from a NAMED LOCAL (`prefabAbsolutePath`), which is what keeps the amended INV-A1 a grep. It is NOT
// in I97's four files, material_panel.cpp, material_preview.cpp, material_edit.cpp and asset_browser_panel.cpp, whose
// pin forbids the call.
//
// It logs nothing, runs nothing in a draw walk, and never deletes: a refusal writes nothing and names why.
#include <aero/scene/scene.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace engine::editor {

inline constexpr std::size_t MAX_NEW_PREFAB_ATTEMPTS = 64;
inline constexpr std::string_view FALLBACK_PREFAB_STEM = "Prefab";

// `rootName`, or FALLBACK_PREFAB_STEM when it is empty or validateAssetName refuses it, alone or with the prefab suffix
// on. The name is checked both ways because each check can refuse what the other accepts: "Crate." fails alone and
// passes with the suffix, and 245 bytes passes alone and fails with it.
[[nodiscard]] std::string prefabFileStem(std::string_view rootName);

struct PrefabSaveResult {
    std::string relativePath;  // assets-relative, '/'-separated; empty on a refusal
    std::string error;         // empty on success
};

// Writes `roots` (already topMost-filtered) as `<directoryRel>/<stem>.aeroprefab` under `assetsRoot`. The stem is the
// first root's name, made unique against the folder's UNFILTERED listing (an ignored file still owns its name); a
// Prefab of several roots is wrapped in a root named for the CHOSEN stem, so a `Crate-2` file reads `Crate-2`. It never
// overwrites. Refusals, each writing nothing: no project, no scene I/O, no roots, a listing that is not complete, no
// unused name, a capture that refuses, or the write's own reason.
[[nodiscard]] PrefabSaveResult savePrefabFile(const World& world, std::span<const Entity> roots,
                                              std::string_view assetsRoot, std::string_view directoryRel);

}  // namespace engine::editor

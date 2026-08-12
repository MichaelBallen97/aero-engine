#pragma once
// Aero Engine — the model importer's user-intent settings (task 3.2.1). PUBLIC, and DELIBERATELY
// ALONE IN THIS HEADER.
//
// Why its own file (plan §A-11): this type is shared by TWO subsystems with very different dependency
// footprints -- the committed .meta format (asset_meta.hpp, whose whole include list is
// <aero/core/guid.hpp> + <aero/editor/asset_cache.hpp>, and which is reached from editor_app.hpp by
// most of the editor) and the importer (model_import.hpp, which pulls <aero/core/math.hpp> and
// <aero/editor/scene_bounds.hpp>, and through the latter <aero/scene/entity.hpp>). Declaring
// ImportSettings in model_import.hpp would force asset_meta.hpp to include it, putting aero::scene and
// the whole math umbrella on the compile line of every TU that touches an asset record.
// THIS HEADER INCLUDES <cstdint> AND NOTHING ELSE, FOREVER. Do not add an include here.
//
// D7's omit-when-default rule is literally `settings == ImportSettings{}`, so THIS STRUCT'S DEFAULTS
// ARE THE COMMITTED FORMAT'S DEFAULTS. Changing one changes what an existing sidecar means. Do not.
#include <cstdint>
#include <string_view>

namespace engine::editor {

// The importer's registered identity in the machine-local import cache (3.1.2's seam, D8). Declared
// HERE rather than in model_import.hpp so asset_meta.cpp can write the block without depending on the
// importer -- the dependency must run .meta -> settings, never .meta -> importer.
inline constexpr std::string_view GLTF_IMPORTER_NAME = "gltf";
inline constexpr std::uint32_t GLTF_IMPORTER_VERSION = 1;
// task 3.2.2 (D15). HERE, beside the glTF pair and NOT in model_import.hpp, for the identical reason:
// asset_meta.cpp must write the block without depending on the importer, and this header's own rule is
// that it includes <cstdint>/<string_view> and nothing else, forever. Two inline constexprs add no
// include. `.meta` STAYS AT VERSION 1 -- a v2 bump nils every GUID in the project for an older build.
inline constexpr std::string_view FBX_IMPORTER_NAME = "fbx";
inline constexpr std::uint32_t FBX_IMPORTER_VERSION = 1;
// task 3.2.3 (D17). HERE, beside the glTF and FBX pairs and NOT in model_import.hpp, for the identical
// reason: asset_meta.cpp must write the block without depending on the importer. ONE name serves BOTH
// .obj and .mtl -- one importer, two claimed extensions. `.meta` STAYS AT VERSION 1.
inline constexpr std::string_view OBJ_IMPORTER_NAME = "obj";
inline constexpr std::uint32_t OBJ_IMPORTER_VERSION = 1;
// task 3.2.5 (A-4). HERE, beside the glTF, FBX and OBJ pairs and NOT in model_import.hpp, for the
// identical reason: asset_meta.cpp must write the block without depending on the importer. ONE name
// serves ALL THREE of .dae, .ply and .stl -- one importer, three claimed extensions, the .obj/.mtl
// shape one step wider. `.meta` STAYS AT VERSION 1 and gains no key.
inline constexpr std::string_view ASSIMP_IMPORTER_NAME = "assimp";
inline constexpr std::uint32_t ASSIMP_IMPORTER_VERSION = 1;

struct ImportSettings {
    // A user-intent multiplier, NOT a unit conversion -- glTF is metres by specification (F7b).
    // Applied to positions, to ROOT node translations only, and to the inverse bind matrices'
    // translation column; never to normals, tangents, UVs, colours, weights or rotations (plan A22).
    float scale = 1.0F;
    bool importMaterials = true;
    bool importAnimations = true;
    bool importSkins = true;
    bool operator==(const ImportSettings&) const = default;
};

}  // namespace engine::editor

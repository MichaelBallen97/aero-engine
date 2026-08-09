// Aero Engine — the Wavefront OBJ/MTL backend (task 3.2.3). THE ONLY tinyobjloader TRANSLATION UNIT IN
// THE TREE (INV-O1). This file grows across several steps; see docs/plans/3.2.3-obj-import-tinyobjloader.md
// §S for the sequence and docs/10-engineering-log.md's 3.2.3 entry for the finished design.
//
// Step 1 (this commit): wire tinyobjloader into the build behind the D21 earcut guard, with an INERT
// body. NOT YET routed from model_import.cpp's dispatch -- importModel still refuses every ".obj"/".mtl"
// exactly as before this commit, so nothing about existing behaviour changes. The only goal of this step
// is to prove the port configures, builds and links on all six presets before a line of importer logic
// exists (R1).
#include "obj_import.hpp"

// task 3.2.3 D21: the mapbox earcut triangulation path reads vertex positions guarded only by
// assert(), which vanishes under NDEBUG -- a Debug abort on the sanitiser lanes and a heap over-read in
// Release, both reachable from an ordinary broken .obj. The vcpkg port neither defines this macro nor
// installs the mapbox/ headers. If you are turning it on deliberately, replace this guard with our own
// bounds-checked triangulation first.
#ifdef TINYOBJLOADER_USE_MAPBOX_EARCUT
    #error \
        "task 3.2.3 D21: the mapbox earcut triangulation path reads vertex positions guarded only by \
assert(), which vanishes under NDEBUG -- a Debug abort on the sanitiser lanes and a heap over-read in \
Release, both reachable from an ordinary broken .obj. The vcpkg port neither defines this macro nor \
installs the mapbox/ headers. If you are turning it on deliberately, replace this guard with our own \
bounds-checked triangulation first."
#endif
// TINYOBJLOADER_IMPLEMENTATION must NEVER be defined anywhere in this tree: vcpkg already compiles the
// library into its own archive, and defining this macro here too would compile a second copy into this
// TU and risk an ODR conflict with the linked archive.
#include <tiny_obj_loader.h>

namespace engine::editor {

ImportResult importObj(std::string_view /*fileName*/, std::string_view /*assetRelativeDir*/,
                       std::span<const std::byte> /*bytes*/, const ImportSettings& /*settings*/, ImportDepth /*depth*/,
                       std::span<const ExternalBuffer> /*external*/) {
    // task 3.2.3, Step 1: not implemented yet, and not yet called from model_import.cpp's dispatch --
    // this body exists only so the target below links. The Structure arm lands at Step 3, the Full
    // parse at Step 4, geometry at Step 5, material buckets and nodes at Step 6, and the .mtl arm at
    // Step 7.
    ImportResult result;
    result.status = ImportStatus::Unsupported;
    result.message = "the OBJ importer is not wired yet";
    return result;
}

}  // namespace engine::editor

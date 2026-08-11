// Aero Engine — the Assimp backend for .dae/.ply/.stl (task 3.2.5). THE ONLY ASSIMP TRANSLATION UNIT
// IN THE TREE (INV-A1). This file grows across several steps; see
// docs/plans/3.2.5-assimp-fallback-importers.md §S for the sequence and docs/10-engineering-log.md's
// 3.2.5 entry for the finished design.
//
// Step 1 (this commit): the link, and nothing else. importAssimp constructs a real Assimp::Importer --
// which is the WHOLE POINT of this commit, because an empty TU that names no Assimp symbol pulls no
// archive member and would therefore prove neither that the second stb_image implementation the port
// carries is harmless nor that the Windows DLL is copied beside the test binaries.
//
// FORBIDDEN, PERMANENTLY, and each named here so this file's own documentation states the rule (which
// is exactly why the gate that checks them STRIPS COMMENTS first): Importer::ReadFile, DefaultIOSystem,
// aiImportFile, aiExportScene, Assimp::Exporter, ZipArchiveIOSystem, SetIOHandler(nullptr),
// DefaultLogger, UnregisterLoader, and the post-process flags PreTransformVertices, MakeLeftHanded,
// ConvertToLeftHanded, FlipUVs, FlipWindingOrder, GlobalScale, GenNormals, GenSmoothNormals,
// CalcTangentSpace, JoinIdenticalVertices, RemoveRedundantMaterials, GenBoundingBoxes, FindInvalidData,
// FindDegenerates.
#include "assimp_import.hpp"

#include <assimp/Importer.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace engine::editor {

ImportResult importAssimp(std::string_view fileName, std::string_view assetRelativeDir,
                          std::span<const std::byte> bytes, const ImportSettings& settings, ImportDepth depth,
                          std::span<const ExternalBuffer> external) {
    (void)fileName;
    (void)assetRelativeDir;
    (void)bytes;
    (void)settings;
    (void)depth;
    (void)external;

    // Step 1 only: forces the linker to pull assimp's archive (or, on Windows, to load its DLL) so this
    // commit genuinely proves the link on all six presets. Replaced by runAssimp at step 3.
    const Assimp::Importer probe;
    (void)probe.GetImporterCount();

    ImportResult result;
    result.status = ImportStatus::Unsupported;
    result.message = "no importer claims this file type";  // BYTE-IDENTICAL to the fall-through's own
                                                           // message, so behaviour is unchanged
    return result;
}

}  // namespace engine::editor

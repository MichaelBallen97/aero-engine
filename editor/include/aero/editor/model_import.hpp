#pragma once
// Aero Engine — the canonical, format-agnostic, third-party-free imported model (task 3.2.1).
// PUBLIC, and the asset_cache.hpp / asset_meta.hpp shape verbatim: free of ImGui, SDL, entt,
// <filesystem>, <fstream> and every build gate. NOTHING HERE LOGS (INV-A3, an eighth application) --
// status is RETURNED, never printed.
//
// STEP 1 of this task's build: a MINIMAL shape holding only what editor/src/gltf_import.cpp's linking
// stub needs to compile -- ImportStatus, ImportResult, ImportDepth, ExternalBuffer, and an EMPTY
// ImportedModel placeholder. Step 2 replaces this file with the full canonical model (every
// node/mesh/material/image/skin/animation type, the D15 caps, and the pure URI-policy helpers).
//
// NO FASTGLTF ANYWHERE IN THIS HEADER OR ITS .cpp (INV-M1/AC-55). The glTF backend lives behind the
// src-private editor/src/gltf_import.hpp, exactly as the stb_image decoder lives behind
// editor/src/thumbnail_store.hpp (task 3.1.3, D18).
#include <aero/editor/import_settings.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::editor {

// Filled out in full at Step 2 (§D-2): nodes, meshes, materials, images, skins, animations, summary.
struct ImportedModel {};

enum class ImportStatus : std::uint8_t {
    Ok = 0,
    Unsupported,       // no importer claims this extension -- NOTHING was read
    ParseFailed,       // fastgltf refused the document
    Malformed,         // parsed, but violates a glTF invariant this importer requires
    MissingExtension,  // extensionsRequired names something this build does not implement (D20/A6)
    MissingBuffer,     // a Full pass whose caller did not supply a buffer the Structure pass named
    Truncated,         // a D15 cap was hit; `model` is COHERENT but SMALLER, never partial-claiming-whole
};

struct ImportResult {
    ImportStatus status = ImportStatus::Ok;
    // "" IFF status == Ok (plan A15). Two caps in one import produce ONE Truncated and two
    // "; "-joined messages.
    std::string message;
    ImportedModel model;  // EMPTY unless status is Ok or Truncated
    // Every ACCEPTED external URI, as classifyUri resolved it (project-relative, '/'-separated),
    // deduplicated, in first-seen source order.
    std::vector<std::string> externalUris;
    std::vector<std::string> warnings;  // capped at MAX_IMPORT_WARNINGS
    std::size_t warningTotal = 0;       // UNCAPPED
};

// ---- the entry point --------------------------------------------------------------------------------
enum class ImportDepth : std::uint8_t {
    // Nodes, meshes' SHAPES, materials, images, skins' joints, clips' shapes, and every external URI.
    // NO vertex data, NO index data, NO animation samples, NO inverse bind matrices. Needs the
    // .gltf/.glb bytes and NOTHING ELSE. What the SCAN runs, budgeted.
    Structure = 0,
    // Everything, including every accessor's decoded contents. Needs the bytes AND every external
    // buffer the Structure pass named. What the PANEL runs, for one asset, on demand.
    Full,
};

// One external buffer, supplied BY THE CALLER (D3/D4). `uri` matches an entry of a previous Structure
// pass's `externalUris` EXACTLY.
struct ExternalBuffer {
    std::string uri;
    std::string bytes;  // std::string is the BYTE container, as everywhere else in this tree
};

}  // namespace engine::editor

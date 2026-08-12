#pragma once
// Aero Engine — the ImportedModel -> mesh cook adapter (task 3.3.1). The editor's ENTIRE contribution
// to this task: two pure functions and no UI. No panel change, no menu item, no cook-on-import, no
// Library/ write, no EditorApp edit -- asset_database.cpp, asset_view.cpp, import_details_panel.cpp,
// editor_app.cpp and model_import_session.{hpp,cpp} are all BYTE-IDENTICAL to main afterwards (AC-40).
//
// It lives in /editor rather than inside tools/cooker so it is exercised by aero_editor_shell_test
// against real fixtures, and so the editor's own future cook path and the CLI share ONE mapping
// rather than two.
//
// PURE: no disk, no ImGui, no SDL, no <filesystem>, no logging.
#include <aero/assets/mesh_cook.hpp>
#include <aero/editor/model_import.hpp>

#include <vector>

namespace engine::editor {

// Every primitive of every mesh, in ascending (meshIndex, primitiveIndex) order.
//
// LIFETIME: the returned vector's spans point INTO `model`, which must outlive both the vector and any
// cook using it. That is the whole reason this returns spans rather than copies -- an ImportedModel of
// a real character is tens of megabytes and the cook reads each array exactly once.
//
// sourceMeshIndex is the POSITION in model.meshes and sourcePrimitiveIndex the position in that mesh's
// `primitives` -- NOT ImportedMesh::localId, which is the source file's own id and, for FBX, a raw
// ufbx typed_id rather than a dense index. The position is what a consumer resolves
// (ImportedNode::meshIndex holds the same value), and it is what the cooked submesh records.
//
// An EMPTY MESH contributes nothing and is not an error (OBJ and Assimp both produce them routinely),
// and it renumbers nothing: the index recorded is a mesh's position in model.meshes, which an empty
// neighbour cannot move.
[[nodiscard]] std::vector<assets::MeshCookPrimitive> meshCookPrimitives(const ImportedModel& model);

// meshCookPrimitives + cookMesh, for the common case. `model` need only outlive the call. It is the
// convenience, never a second policy: the bytes are identical to cooking the vector by hand.
[[nodiscard]] assets::MeshCookResult cookImportedModel(const ImportedModel& model, Guid sourceGuid);

}  // namespace engine::editor

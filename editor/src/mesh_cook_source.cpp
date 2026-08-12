// Aero Engine — the ImportedModel -> mesh cook adapter (task 3.3.1). See mesh_cook_source.hpp for the
// contract. PURE: no disk, no ImGui, no SDL, no <filesystem>, no logging.
#include <aero/editor/mesh_cook_source.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::editor {
namespace {

// The two "absent" sentinels are the same value BY CONSTRUCTION, which is the whole reason
// materialIndex copies verbatim below with no mapping. They live in different layers and neither
// header includes the other -- engine/assets may never include an editor header -- so this is the one
// place the equality can be pinned at compile time. MK2 re-asserts it at runtime, where a reader looks.
static_assert(INVALID_SUBASSET == assets::COOKED_INVALID_MATERIAL,
              "the editor's absent-sub-asset sentinel and the cooked format's absent-material sentinel "
              "are copied VERBATIM with no mapping; if they ever diverge this adapter must translate");

}  // namespace

std::vector<assets::MeshCookPrimitive> meshCookPrimitives(const ImportedModel& model) {
    std::vector<assets::MeshCookPrimitive> out;
    std::size_t total = 0;
    for (const ImportedMesh& mesh : model.meshes) {
        total += mesh.primitives.size();
    }
    out.reserve(total);
    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        const ImportedMesh& mesh = model.meshes[m];
        for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
            const ImportedPrimitive& src = mesh.primitives[p];
            assets::MeshCookPrimitive dst;
            // THE POSITION in model.meshes, never ImportedMesh::localId -- see the header.
            dst.sourceMeshIndex = static_cast<std::uint32_t>(m);
            dst.sourcePrimitiveIndex = static_cast<std::uint32_t>(p);
            dst.materialIndex = src.materialIndex;  // VERBATIM, sentinel included (the static_assert above)
            // Every one of these is an implicit std::vector -> std::span conversion that BORROWS.
            // ImportedPrimitive::attributes is deliberately NOT read: the cook derives the mask from
            // which arrays are actually non-empty and correctly sized, which is strictly more reliable
            // than a bitset a backend could have set inconsistently. MK11 asserts the two agree on
            // every fixture, which turns a redundancy into a cross-check.
            dst.positions = src.positions;
            dst.normals = src.normals;
            dst.tangents = src.tangents;
            dst.uv0 = src.uv0;
            dst.uv1 = src.uv1;
            dst.colors = src.colors;
            dst.joints = src.joints;
            dst.weights = src.weights;
            dst.indices = src.indices;
            out.push_back(dst);
        }
    }
    return out;
}

assets::MeshCookResult cookImportedModel(const ImportedModel& model, Guid sourceGuid) {
    const std::vector<assets::MeshCookPrimitive> primitives = meshCookPrimitives(model);
    assets::MeshCookInput input;
    input.sourceGuid = sourceGuid;
    input.primitives = primitives;
    return assets::cookMesh(input);
}

}  // namespace engine::editor

// Aero Engine — the ImportedModel -> skeleton cook adapter (task 3.5.1). See
// skeleton_cook_source.hpp for the contract. PURE: no disk, no ImGui, no SDL, no <filesystem>, no
// logging.
#include <aero/editor/skeleton_cook_source.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace engine::editor {
namespace {

// The editor's "absent sub-asset" and the cook's "no parent / no palette slot" are the same value by
// construction, which is what lets ImportedNode::parent become parentLocalId with no mapping. They
// live in different layers and neither header includes the other, so this is the one place the
// equality can be pinned at compile time (the mesh_cook_source.cpp precedent).
static_assert(INVALID_SUBASSET == assets::SKELETON_INVALID_INDEX,
              "ImportedNode::parent is copied VERBATIM into SkeletonCookJoint::parentLocalId; if the "
              "two sentinels ever diverge this adapter must translate");

using NodeEntry = std::pair<std::uint32_t, std::uint32_t>;  // localId -> position in model.nodes

[[nodiscard]] SkeletonSourceResult fail(std::string error) {
    SkeletonSourceResult out;
    out.ok = false;
    out.error = std::move(error);
    return out;
}

[[nodiscard]] bool byKey(const NodeEntry& entry, std::uint32_t value) { return entry.first < value; }

// The ONE resolution path. Never nodes[localId]: for FBX a localId is a raw ufbx typed_id, so the
// position it happens to equal for glTF is a coincidence this tree has already been bitten by twice.
[[nodiscard]] std::uint32_t positionOf(const std::vector<NodeEntry>& byLocalId, std::uint32_t localId) {
    const auto it = std::lower_bound(byLocalId.begin(), byLocalId.end(), localId, byKey);
    if (it == byLocalId.end() || it->first != localId) {
        return INVALID_SUBASSET;
    }
    return it->second;
}

// Membership of the emitted set, as a sorted vector of localIds -- a hash container would put an
// iteration order between this adapter and the cook's input.
[[nodiscard]] bool contains(const std::vector<std::uint32_t>& sorted, std::uint32_t value) {
    return std::binary_search(sorted.begin(), sorted.end(), value);
}
void insertSorted(std::vector<std::uint32_t>& sorted, std::uint32_t value) {
    sorted.insert(std::lower_bound(sorted.begin(), sorted.end(), value), value);
}

// One joint record built from a node, with the caller deciding the palette slot and the IBM. The TRS
// is copied verbatim: ImportedNode is always TRS (a matrix source was decomposed at import), and
// this adapter converts nothing.
[[nodiscard]] assets::SkeletonCookJoint jointFromNode(const ImportedNode& node, std::uint32_t paletteSlot,
                                                      const Mat4& inverseBind) {
    assets::SkeletonCookJoint joint;
    joint.localId = node.localId;
    joint.parentLocalId = node.parent;  // VERBATIM, sentinel included (the static_assert above)
    joint.paletteSlot = paletteSlot;
    joint.translation = node.translation;
    joint.rotation = node.rotation;
    joint.scale = node.scale;
    joint.inverseBind = inverseBind;
    return joint;
}

}  // namespace

SkeletonSourceResult skeletonCookJoints(const ImportedModel& model, std::uint32_t skinIndex) {
    // 1. the skin itself. The message names what EXISTS, so "0 skin(s)" is the honest answer for a
    //    model that carries none rather than a different error for the same question.
    if (skinIndex >= model.skins.size()) {
        return fail(
            std::format("skin index {} is out of range: the model has {} skin(s)", skinIndex, model.skins.size()));
    }
    const ImportedSkin& skin = model.skins[skinIndex];

    SkeletonSourceResult out;
    // 2. the multi-skin advisory. ONE home for it: the CLI relays what it finds here, so there is no
    //    second implementation to drift (a cooked skeleton is per-skin by construction).
    if (model.skins.size() > 1) {
        out.warnings.push_back(
            std::format("the model has {} skins; cooking skin {} only", model.skins.size(), skinIndex));
    }

    // 3. the Structure-depth refusal. inverseBindMatrices is EXACTLY joints.size() at Full depth and
    //    DELIBERATELY EMPTY at Structure depth (model_import.hpp), so an empty one beside a
    //    non-empty joint list is a half-imported model. Cooking it would write identity matrices
    //    that look like a rig and deform like a bug.
    const std::size_t jointCount = skin.joints.size();
    const std::size_t matrixCount = skin.inverseBindMatrices.size();
    if (jointCount == 0) {
        return fail(std::format("skin {} lists no joints", skinIndex));
    }
    if (matrixCount == 0) {
        return fail(
            std::format("skin {} carries {} joints and no inverse bind matrices: this model was "
                        "imported at Structure depth, and a skeleton needs a Full import",
                        skinIndex, jointCount));
    }
    if (matrixCount != jointCount) {
        return fail(
            std::format("skin {} carries {} joints and {} inverse bind matrices", skinIndex, jointCount, matrixCount));
    }

    // 4. THE map, built once: localId -> position in model.nodes.
    std::vector<NodeEntry> byLocalId;
    byLocalId.reserve(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        byLocalId.emplace_back(model.nodes[i].localId, static_cast<std::uint32_t>(i));
    }
    std::sort(byLocalId.begin(), byLocalId.end());

    // 5. the palette joints, in SOURCE ORDER: slot k is the skin's k-th joint, which is glTF's own
    //    binding rule and what every JOINTS_0 index means. A duplicate localId is NOT dropped here --
    //    the cook refuses it, and refusing in one place keeps the two layers' stories identical.
    std::vector<std::uint32_t> included;
    out.joints.reserve(skin.joints.size());
    included.reserve(skin.joints.size());
    for (std::size_t k = 0; k < skin.joints.size(); ++k) {
        const std::uint32_t position = positionOf(byLocalId, skin.joints[k]);
        if (position == INVALID_SUBASSET) {
            return fail(std::format("skin {} joint {} names node localId {}, which the model does not have", skinIndex,
                                    k, skin.joints[k]));
        }
        out.joints.push_back(
            jointFromNode(model.nodes[position], static_cast<std::uint32_t>(k), skin.inverseBindMatrices[k]));
        if (!contains(included, skin.joints[k])) {
            insertSorted(included, skin.joints[k]);
        }
    }

    // 6. the ancestor closure. glTF permits non-joint nodes between and above joints, and a joint's
    //    global transform is the product of ALL its ancestors, so every one of them is carried as a
    //    hierarchy-only record with an IDENTITY inverse bind matrix.
    //
    //    ImportedSkin::skeletonRoot is DELIBERATELY NOT CONSULTED: the closure derives from parent
    //    links alone, and that field is recorded as-is by the importers even when it names something
    //    that is not a joint (model_import.hpp's E24) -- unvalidated provenance, not a boundary.
    //
    //    Iterative and depth-capped, so a hostile parent cycle is an ERROR and never a hang.
    const std::size_t paletteCount = out.joints.size();
    for (std::size_t k = 0; k < paletteCount; ++k) {
        std::uint32_t cursor = out.joints[k].parentLocalId;
        std::uint32_t depth = 0;
        while (cursor != INVALID_SUBASSET) {
            if (depth >= MAX_NODE_DEPTH) {
                return fail(
                    std::format("the parent chain above skin {} joint {} is deeper than {} nodes, or "
                                "forms a cycle",
                                skinIndex, k, MAX_NODE_DEPTH));
            }
            ++depth;
            const std::uint32_t position = positionOf(byLocalId, cursor);
            if (position == INVALID_SUBASSET) {
                return fail(std::format("node localId {} names parent localId {}, which the model does not have",
                                        out.joints[k].localId, cursor));
            }
            const ImportedNode& ancestor = model.nodes[position];
            if (!contains(included, cursor)) {
                out.joints.push_back(jointFromNode(ancestor, assets::SKELETON_INVALID_INDEX, Mat4::identity()));
                insertSorted(included, cursor);
            }
            cursor = ancestor.parent;
        }
    }

    // 7. the weight-range advisory (AC-17). A per-vertex joint index at or past the skin's joint
    //    count binds that vertex to a palette slot the artifact does not carry: memory-safe, because
    //    every shader read lands inside the pushed block, and visually wrong. WARN, never demote --
    //    the cook copies vertex data verbatim and this adapter modifies nothing at all.
    const auto jointLimit = static_cast<std::uint32_t>(skin.joints.size());
    for (const ImportedNode& node : model.nodes) {
        if (node.skinIndex != skinIndex || node.meshIndex >= model.meshes.size()) {
            continue;
        }
        const ImportedMesh& mesh = model.meshes[node.meshIndex];
        for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
            const ImportedPrimitive& primitive = mesh.primitives[p];
            for (std::size_t v = 0; v < primitive.joints.size(); ++v) {
                if (out.warnings.size() >= MAX_IMPORT_WARNINGS) {
                    break;
                }
                const std::array<std::uint16_t, 4>& influences = primitive.joints[v];
                for (const std::uint16_t index : influences) {
                    if (index < jointLimit) {
                        continue;
                    }
                    out.warnings.push_back(
                        std::format("mesh {} primitive {} vertex {} binds joint index {}, "
                                    "past skin {}'s {} joints",
                                    node.meshIndex, p, v, index, skinIndex, jointLimit));
                    break;  // ONE warning per vertex; a vertex with four bad influences is one defect
                }
            }
        }
    }

    out.ok = true;
    return out;
}

assets::SkeletonCookResult cookImportedSkeleton(const ImportedModel& model, std::uint32_t skinIndex, Guid sourceGuid) {
    SkeletonSourceResult source = skeletonCookJoints(model, skinIndex);
    if (!source.ok) {
        assets::SkeletonCookResult refused;
        refused.status = assets::SkeletonCookStatus::Invalid;
        refused.message = std::move(source.error);
        return refused;
    }
    assets::SkeletonCookInput input;
    input.sourceGuid = sourceGuid;
    input.sourceSkinIndex = skinIndex;
    input.joints = source.joints;
    assets::SkeletonCookResult out = assets::cookSkeleton(input);
    // The adapter's advisories ride out FIRST: they are about the model, and the cook's (none today)
    // would be about the bytes. One list, one printer, no second implementation.
    out.warnings.insert(out.warnings.begin(), source.warnings.begin(), source.warnings.end());
    return out;
}

}  // namespace engine::editor

#pragma once
// Aero Engine — the ImportedModel -> skeleton cook adapter (task 3.5.1). The mesh_cook_source.hpp
// mold, one format over: two pure functions and no UI. No panel change, no menu item, no
// cook-on-import, no Library/ write.
//
// PURE: no disk, no ImGui, no SDL, no <filesystem>, no logging -- warnings are RETURNED, never
// printed, because the CLI and a future editor cook path want them in different places.
//
// THIS IS THE FOURTH CONSUMER OF THE localId RULE (.claude/rules/editor.md). ImportedSkin::joints,
// ImportedNode::parent and ImportedNode::children all hold localIds -- POSITIONS for glTF and raw
// ufbx typed_ids for FBX -- so every resolution below goes through ONE localId -> position map built
// once, and nothing here ever writes nodes[localId]. That exact confusion has shipped twice in this
// tree: once as an ASan heap-buffer-overflow (task 3.2.2) and once as a silently wrong deformation
// (task 3.2.5's blocking finding).
#include <aero/assets/skeleton_cook.hpp>
#include <aero/editor/model_import.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine::editor {

struct SkeletonSourceResult {
    bool ok = false;
    std::string error;  // "" IFF ok
    // Source-order palette slots (slot k IS the skin's k-th joint, glTF's own binding rule) followed
    // by the ancestor closure as hierarchy-only entries. The COOK owns the canonical order; this
    // vector is deliberately not sorted here.
    std::vector<assets::SkeletonCookJoint> joints;
    std::vector<std::string> warnings;  // capped at MAX_IMPORT_WARNINGS
};

// Walk `model` and produce the flat joint list `cookSkeleton` consumes.
//
// Refusals name what exists: an out-of-range skin index reports the model's real skin count (zero
// included), a Structure-depth model is refused BY NAME because half-imported inverse bind matrices
// must never become identity by accident, and a skin joint naming no node is an error rather than a
// dropped bone.
//
// The ancestor closure includes every non-joint ancestor up to the root as a hierarchy-only entry,
// because glTF permits non-joint nodes between and above joints and a joint's global transform is
// the product of ALL its ancestors. The walk is iterative and depth-capped by the importer's own
// MAX_NODE_DEPTH, so a hostile parent cycle is an error return and never a hang.
[[nodiscard]] SkeletonSourceResult skeletonCookJoints(const ImportedModel& model, std::uint32_t skinIndex);

// skeletonCookJoints + cookSkeleton, for the common case. `model` need only outlive the call, and
// the bytes are identical to composing the two by hand -- this is the convenience, never a second
// policy. An adapter error becomes {Invalid, error, {}, {}}; adapter warnings ride out on the
// result, which is how the CLI reports them without a second implementation.
[[nodiscard]] assets::SkeletonCookResult cookImportedSkeleton(const ImportedModel& model, std::uint32_t skinIndex,
                                                              Guid sourceGuid);

}  // namespace engine::editor

#pragma once
// Aero Engine -- the ImportedModel -> entity-subtree planner (task 3.1.5). Pair 18. PUBLIC and PURE:
// no ImGui, no entt, no disk, no GPU, no logging; warnings are RETURNED, never printed. It is the
// FIFTH named consumer of the localId rule, and the first outside a cook adapter or a panel.
//
// TWO KINDS OF NUMBER LIVE IN ImportedNode AND THEY ARE NOT INTERCHANGEABLE.
//   * `parent`, `children` and ImportedModel::roots hold localIds -- the SOURCE FILE's own ids. For
//     FBX they are raw ufbx typed_ids: sparse, unordered, and NOT indices into `nodes`. Indexing
//     nodes[localId] is a real ASan heap-overflow that shipped once (3.2.2) and a silent wrong
//     deformation that shipped once (3.2.5). EVERY link here crosses ONE sorted localId -> position
//     map, built once, in the .cpp.
//   * `meshIndex` is ALREADY a position -- into ImportedModel::meshes -- and is the same number
//     CookedSubmesh::sourceMeshIndex records. It is deliberately NOT resolved through that map, and
//     "fixing" it to go through the map is seed S6: it would work for glTF (where the two coincide)
//     and produce a wrong mesh for FBX, which is the exact shape of the bug the map exists to prevent.
//
// EVERY WALK HERE IS ITERATIVE AND DEPTH-BOUNDED. misc-no-recursion is --warnings-as-errors in CI,
// and a hostile parent cycle must EXHAUST the worklist and return ok = false naming it -- never hang,
// never recurse.
#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>
#include <aero/editor/model_import.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// InstantiatePlan::ok is false for EXACTLY three causes, each with its own message. A consumer
// switches on THIS rather than string-matching `error` -- string-matching a refusal reason is how a
// message reword becomes a behaviour change. Only NoNodes is worth a Full-import retry: a cycle or an
// over-deep chain will not be fixed by re-importing at a different depth.
enum class InstantiatePlanRefusal : std::uint8_t { None = 0, NoNodes, Cycle, TooDeep };

// NEVER named toString (.claude/rules/ci-portability.md): DOCTEST_STRINGIFY expands to an UNQUALIFIED
// toString(...), so one on a public header is found by ADL and hard-errors every lane inside doctest.h.
[[nodiscard]] std::string_view instantiatePlanRefusalLabel(InstantiatePlanRefusal refusal) noexcept;

struct InstantiatePlanNode {
    std::string name;              // ImportedNode::name VERBATIM; "" stays "" -- names are never invented
    std::uint32_t parentSlot = 0;  // index into `nodes`; slot 0 is the synthetic root and points at itself
    Vec3 translation{};
    Quat rotation = Quat::identity();
    Vec3 scale = Vec3::one();
    Guid mesh{};                  // assetGuid when the node carries a mesh, else NIL
    std::uint32_t meshIndex = 0;  // ImportedNode::meshIndex -- a POSITION (see the asymmetry note above).
                                  // Only ever consulted when `mesh` is valid: ImportedNode::meshIndex
                                  // defaults to INVALID_SUBASSET, never to 0, so this plan-side 0 is a
                                  // placeholder rather than "mesh 0".
};

struct InstantiatePlan {
    // PARENTS BEFORE CHILDREN, by construction: the walk is a BFS, so parentSlot < index for every
    // index > 0. That is what lets InstantiateAssetCommand create entities in ONE forward pass with no
    // fixup phase. nodes[0] is the synthetic root and is the only slot whose parentSlot is itself.
    std::vector<InstantiatePlanNode> nodes;
    std::vector<std::string> warnings;
    bool ok = false;
    InstantiatePlanRefusal refusal = InstantiatePlanRefusal::None;
    std::string error;  // "" iff ok
};

// PURE and TOTAL: every ImportedModel yields a plan, and a refused plan carries an empty `nodes`.
// `assetStem` names the synthetic root (the model's file stem, without its extension); `assetGuid` is
// the asset every mesh-carrying node references.
[[nodiscard]] InstantiatePlan buildInstantiatePlan(const ImportedModel& model, std::string_view assetStem,
                                                   Guid assetGuid);

}  // namespace engine::editor

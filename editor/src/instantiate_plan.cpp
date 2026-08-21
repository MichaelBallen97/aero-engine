// editor/src/instantiate_plan.cpp -- task 3.1.5: the ImportedModel -> entity-subtree planner.
// PURE: no ImGui, no entt, no disk, no GPU, no logging; warnings are RETURNED.
//
// THE ONE localId -> position MAP LIVES HERE, AND IT IS THE ONLY WAY A LINK IS RESOLVED. See the
// header's asymmetry note: `parent`, `children` and `roots` cross it; `meshIndex` deliberately does
// NOT, because it is already a position into ImportedModel::meshes. There is no `nodes[localId]`
// anywhere in this file and there must never be one.
#include <aero/editor/instantiate_plan.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// One (source id -> position) pair. A sorted vector plus std::lower_bound, never a hash container:
// the tree's established shape (recordList / byGuid / ThumbnailLedger::entries), and the reason a
// plan is byte-deterministic across two calls (PL20) rather than dependent on an iteration order.
struct LocalIdEntry {
    std::uint32_t localId = 0;
    std::uint32_t position = 0;
};

// A pending link, carrying the SOURCE id rather than a position: resolution happens at pop, which is
// what makes "this child names an id the model does not contain" one warn-and-skip arm instead of a
// second lookup site.
struct WorkItem {
    std::uint32_t localId = 0;
    std::uint32_t parentSlot = 0;
    std::uint32_t depth = 0;
};

[[nodiscard]] std::uint32_t positionOfLocalId(std::span<const LocalIdEntry> map, std::uint32_t localId) noexcept {
    const auto found =
        std::lower_bound(map.begin(), map.end(), localId,
                         [](const LocalIdEntry& entry, std::uint32_t value) noexcept { return entry.localId < value; });
    if (found == map.end() || found->localId != localId) {
        return INVALID_SUBASSET;
    }
    return found->position;
}

// Everything one drain of the worklist needs. A struct rather than a lambda capture because the drain
// runs TWICE -- once from the source roots, once per orphan the post-pass rescues -- and one body is
// the whole point.
struct PlanBuilder {
    const ImportedModel* model = nullptr;
    InstantiatePlan* plan = nullptr;
    Guid assetGuid;
    std::vector<LocalIdEntry> byLocalId;  // SORTED by localId; duplicates removed (first claimant wins)
    std::vector<std::uint8_t> emitted;    // per POSITION
    std::vector<std::uint8_t> skipped;    // per POSITION -- a duplicate localId, never emitted at all
    std::vector<WorkItem> queue;
    std::size_t head = 0;
};

// Drains the worklist. Returns the refusal that stopped it, or None when it emptied normally. The
// queue is a vector plus a head index rather than a std::deque: one allocation growth pattern, no
// node churn, and the whole thing is bounded by MAX_NODES_PER_MODEL.
[[nodiscard]] InstantiatePlanRefusal drainWorklist(PlanBuilder& builder) {
    const ImportedModel& model = *builder.model;
    InstantiatePlan& plan = *builder.plan;
    while (builder.head < builder.queue.size()) {
        const WorkItem item = builder.queue[builder.head];
        ++builder.head;

        if (item.depth > MAX_NODE_DEPTH) {
            return InstantiatePlanRefusal::TooDeep;
        }
        const std::uint32_t position = positionOfLocalId(builder.byLocalId, item.localId);
        if (position == INVALID_SUBASSET) {
            plan.warnings.push_back("a node references source id " + std::to_string(item.localId) +
                                    ", which this model does not contain");
            continue;  // and the whole subtree below it goes with it: nothing was pushed for it
        }
        if (builder.skipped[position] != 0U) {
            continue;  // a duplicate-id node was already reported once, at map-build time
        }
        if (builder.emitted[position] != 0U) {
            plan.warnings.push_back("the node hierarchy revisits source id " + std::to_string(item.localId));
            continue;
        }

        const ImportedNode& node = model.nodes[position];
        const auto slot = static_cast<std::uint32_t>(plan.nodes.size());
        InstantiatePlanNode planned;
        planned.name = node.name;  // VERBATIM, "" included -- names are never invented
        planned.parentSlot = item.parentSlot;
        planned.translation = node.translation;
        planned.rotation = node.rotation;
        planned.scale = node.scale;
        if (node.meshIndex != INVALID_SUBASSET) {
            planned.mesh = builder.assetGuid;
            // A POSITION, stored as-is. Resolving it through byLocalId is seed S6: it happens to work
            // for glTF, where the two coincide, and picks the wrong mesh for FBX.
            planned.meshIndex = node.meshIndex;
            if (node.meshIndex >= model.meshes.size()) {
                // STILL EMITTED: the reference degrades at draw time (nothing is drawn, and the
                // renderer counts it) rather than silently disappearing from the tree here.
                plan.warnings.push_back("node '" + node.name + "' references mesh " + std::to_string(node.meshIndex) +
                                        ", which this model does not have");
            }
        }
        plan.nodes.push_back(std::move(planned));
        builder.emitted[position] = 1U;

        for (const std::uint32_t child : node.children) {
            builder.queue.push_back(WorkItem{.localId = child, .parentSlot = slot, .depth = item.depth + 1});
        }
    }
    return InstantiatePlanRefusal::None;
}

// Built from the constant rather than written out, so a future MAX_NODE_DEPTH bump cannot leave the
// sentence claiming a bound the code no longer applies. It reads exactly 0.17's row today.
[[nodiscard]] std::string tooDeepMessage() {
    return "the node hierarchy is deeper than " + std::to_string(MAX_NODE_DEPTH) + " levels";
}

[[nodiscard]] InstantiatePlan refusedPlan(InstantiatePlanRefusal refusal, std::string error,
                                          std::vector<std::string> warnings) {
    InstantiatePlan plan;
    plan.ok = false;
    plan.refusal = refusal;
    plan.error = std::move(error);
    plan.warnings = std::move(warnings);
    return plan;
}

}  // namespace

std::string_view instantiatePlanRefusalLabel(InstantiatePlanRefusal refusal) noexcept {
    switch (refusal) {
        case InstantiatePlanRefusal::None:
            return "none";
        case InstantiatePlanRefusal::NoNodes:
            return "no nodes";
        case InstantiatePlanRefusal::Cycle:
            return "cycle";
        case InstantiatePlanRefusal::TooDeep:
            return "too deep";
    }
    return "none";  // unreachable; enumerated so a new refusal is a -Wswitch warning, not silent
}

InstantiatePlan buildInstantiatePlan(const ImportedModel& model, std::string_view assetStem, Guid assetGuid) {
    // 0.17, WHICH AMENDS THE SPEC'S D5. The predicate is `nodes.empty()` ALONE, never
    // `nodes.empty() && !meshes.empty()`: .obj, .ply and .stl produce zero nodes AND zero meshes at
    // ImportDepth::Structure, so the conjunction would never fire for the three formats it was written
    // for, and the drop would plan an empty subtree instead of falling back to a Full import.
    if (model.nodes.empty()) {
        return refusedPlan(InstantiatePlanRefusal::NoNodes, "this import produced no node hierarchy", {});
    }

    InstantiatePlan plan;
    PlanBuilder builder;
    builder.model = &model;
    builder.plan = &plan;
    builder.assetGuid = assetGuid;

    const std::size_t nodeCount = model.nodes.size();
    builder.emitted.assign(nodeCount, 0U);
    builder.skipped.assign(nodeCount, 0U);

    // ---- the ONE map: (localId -> position), sorted, duplicates resolved to the FIRST claimant ----
    builder.byLocalId.reserve(nodeCount);
    for (std::size_t i = 0; i < nodeCount; ++i) {
        builder.byLocalId.push_back(
            LocalIdEntry{.localId = model.nodes[i].localId, .position = static_cast<std::uint32_t>(i)});
    }
    // Stable, and keyed on localId alone, so equal ids stay in POSITION order and "the first claimant"
    // is the lowest position -- deterministic, and the same rule the duplicate-GUID repair uses.
    std::ranges::stable_sort(builder.byLocalId, {}, &LocalIdEntry::localId);
    std::vector<LocalIdEntry> unique;
    unique.reserve(builder.byLocalId.size());
    for (const LocalIdEntry& entry : builder.byLocalId) {
        if (!unique.empty() && unique.back().localId == entry.localId) {
            // A duplicate key makes EVERY reference to it ambiguous, so the later node is skipped
            // entirely rather than merged into the first.
            plan.warnings.push_back("two nodes share source id " + std::to_string(entry.localId) +
                                    "; the second was skipped");
            builder.skipped[entry.position] = 1U;
            continue;
        }
        unique.push_back(entry);
    }
    builder.byLocalId = std::move(unique);

    // ---- slot 0: the synthetic root, identity TRS, no mesh ----
    InstantiatePlanNode syntheticRoot;
    syntheticRoot.name = std::string(assetStem);
    syntheticRoot.parentSlot = 0;  // the ONE slot that points at itself
    plan.nodes.push_back(std::move(syntheticRoot));

    // ---- the BFS from the source roots ----
    builder.queue.reserve(nodeCount);
    for (const std::uint32_t rootLocalId : model.roots) {
        builder.queue.push_back(WorkItem{.localId = rootLocalId, .parentSlot = 0, .depth = 1});
    }
    if (const InstantiatePlanRefusal refusal = drainWorklist(builder); refusal != InstantiatePlanRefusal::None) {
        return refusedPlan(refusal, tooDeepMessage(), std::move(plan.warnings));
    }

    // ---- CYCLE DETECTION IS WORKLIST EXHAUSTION, not a second traversal ----
    // The BFS above visits every node reachable from a root exactly once. Anything left is either an
    // ORPHAN (its parent chain terminates, it is simply not under any declared root) or part of a
    // CYCLE (its parent chain revisits, or runs past the depth bound). Both walks below are ITERATIVE
    // and BOUNDED; a chain mark stamped with a per-walk token keeps the whole pass allocation-free.
    std::vector<std::uint32_t> chainMark(nodeCount, 0U);
    std::uint32_t token = 0;
    for (std::uint32_t position = 0; position < static_cast<std::uint32_t>(nodeCount); ++position) {
        if (builder.emitted[position] != 0U || builder.skipped[position] != 0U) {
            continue;
        }
        ++token;
        std::uint32_t current = position;
        std::uint32_t steps = 0;
        bool cyclic = false;
        for (;;) {
            if (chainMark[current] == token) {
                cyclic = true;  // this chain has come back to a node it already walked
                break;
            }
            chainMark[current] = token;
            ++steps;
            if (steps > MAX_NODE_DEPTH) {
                cyclic = true;  // longer than any legal chain: treat it as unresolvable, never walk on
                break;
            }
            const std::uint32_t parentLocalId = model.nodes[current].parent;
            if (parentLocalId == INVALID_SUBASSET) {
                break;  // a genuine chain terminus
            }
            const std::uint32_t parentPosition = positionOfLocalId(builder.byLocalId, parentLocalId);
            if (parentPosition == INVALID_SUBASSET) {
                break;  // names a parent this model does not contain -- an orphan, not a cycle
            }
            current = parentPosition;
        }
        if (cyclic) {
            return refusedPlan(InstantiatePlanRefusal::Cycle, "the node hierarchy contains a cycle",
                               std::move(plan.warnings));
        }
        // The chain terminated. Rescue it under slot 0, starting from the highest ancestor that is not
        // already in the plan -- so an orphan SUBTREE arrives whole rather than one node at a time.
        const bool terminusAlreadyPlaced = builder.emitted[current] != 0U || builder.skipped[current] != 0U;
        const std::uint32_t start = terminusAlreadyPlaced ? position : current;
        plan.warnings.push_back("source id " + std::to_string(model.nodes[start].localId) +
                                " is not reachable from any root; it was placed at the top level");
        builder.queue.push_back(WorkItem{.localId = model.nodes[start].localId, .parentSlot = 0, .depth = 1});
        if (const InstantiatePlanRefusal refusal = drainWorklist(builder); refusal != InstantiatePlanRefusal::None) {
            return refusedPlan(refusal, tooDeepMessage(), std::move(plan.warnings));
        }
    }

    plan.ok = true;
    plan.refusal = InstantiatePlanRefusal::None;
    return plan;
}

}  // namespace engine::editor

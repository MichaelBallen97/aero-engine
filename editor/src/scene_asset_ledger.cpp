// Aero Engine — the scene-asset ledger's body (task 3.1.5, §D-8). See scene_asset_ledger.hpp for the
// purity rules and for the one ordering that matters. NOTHING HERE LOGS, touches a device, or opens a
// file; the seven steps of service() below are the whole of the decision.
#include <aero/editor/scene_asset_ledger.hpp>

#include <algorithm>
#include <utility>

namespace engine::editor {

namespace {

[[nodiscard]] const LedgerAssetFacts* findFacts(std::span<const LedgerAssetFacts> referenced, Guid guid) noexcept {
    // A LINEAR scan, deliberately: `referenced` arrives in ANY order (§0.3) and one tick's referenced
    // set is the handful of distinct assets a scene names, so sorting it here to binary-search it would
    // cost an allocation per pass to save nothing.
    for (const LedgerAssetFacts& facts : referenced) {
        if (facts.guid == guid) {
            return &facts;
        }
    }
    return nullptr;
}

void pushRetire(std::vector<Guid>& retire, Guid guid) {
    // A nudge and a generation bump in one tick must produce ONE retire, not two (LG17). The list is
    // tiny and unordered, so a scan is the whole dedup.
    if (std::find(retire.begin(), retire.end(), guid) == retire.end()) {
        retire.push_back(guid);
    }
}

[[nodiscard]] bool textureRequestBefore(const TextureRequest& a, const TextureRequest& b) noexcept {
    return a.materialIndex == b.materialIndex ? a.slot < b.slot : a.materialIndex < b.materialIndex;
}

}  // namespace

std::string_view ledgerStateLabel(LedgerState state) noexcept {
    switch (state) {
        case LedgerState::Ready:
            return "ready";
        case LedgerState::Failed:
            return "failed";
        case LedgerState::Absent:
            return "absent";
    }
    return "absent";  // unreachable for a valid enumerator; no default:, so a new one is a -Wswitch error
}

SceneAssetLedger::Entry* SceneAssetLedger::findMutableEntry(Guid guid) noexcept {
    const auto it = std::lower_bound(entries.begin(), entries.end(), guid,
                                     [](const Entry& e, Guid g) noexcept { return e.guid < g; });
    return (it != entries.end() && it->guid == guid) ? &*it : nullptr;
}

const SceneAssetLedger::Entry* SceneAssetLedger::findEntry(Guid guid) const noexcept {
    const auto it = std::lower_bound(entries.begin(), entries.end(), guid,
                                     [](const Entry& e, Guid g) noexcept { return e.guid < g; });
    return (it != entries.end() && it->guid == guid) ? &*it : nullptr;
}

void SceneAssetLedger::retireEntryHandles(Entry& entry) {
    // ONE LedgerDestroy PER HANDLE, so the caller's destroy step is three ifs with no dispatch and the
    // struct's "at most one of the three is valid" contract holds by construction.
    if (entry.handles.mesh.valid()) {
        pendingDestroy.push_back(LedgerDestroy{.mesh = entry.handles.mesh});
    }
    for (const render::MaterialHandle material : entry.handles.materials) {
        if (material.valid()) {
            pendingDestroy.push_back(LedgerDestroy{.material = material});
        }
    }
    for (const rhi::TextureHandle texture : entry.handles.textures) {
        if (texture.valid()) {
            pendingDestroy.push_back(LedgerDestroy{.texture = texture});
        }
    }
    entry.handles = LedgerHandles{};
    entry.pendingTextures.clear();
    bounds.removeMesh(entry.guid);
}

LedgerServiceOutput SceneAssetLedger::service(const LedgerServiceInput& input) {
    LedgerServiceOutput out;

    // 1. RETURN THE PREVIOUS PASS'S DESTROYS. THIS HAPPENS FIRST, BEFORE ANYTHING BELOW CAN ADD TO THE
    // LIST, so an entry retired now cannot be destroyed now -- a whole service pass separates "the
    // binding table stopped naming this handle" from "the GPU object dies". That single ordering is the
    // whole of INV-D2's GPU half, and it is why `pendingDestroy` is a member rather than a local.
    // Moving this below the retire steps is seed S20 and LG9 is what reddens.
    out.destroy = std::move(pendingDestroy);
    pendingDestroy.clear();  // a moved-from vector is valid but unspecified; say what it holds

    // 2. APPLY NUDGES, BEFORE the staleness sweep, so a nudge plus a generation bump in the same tick
    // collapse to ONE reload rather than two.
    for (const Guid guid : input.nudged) {
        Entry* entry = findMutableEntry(guid);
        if (entry == nullptr || entry->state != LedgerState::Ready) {
            continue;  // a nudge on an Absent or Failed entry is a no-op (LG16)
        }
        retireEntryHandles(*entry);
        entry->state = LedgerState::Absent;
        entry->loadedHashUsable = false;
        entry->message.clear();
        pushRetire(out.retire, guid);
    }

    // 3. STALENESS SWEEP, only when the generation moved.
    if (input.generation != lastGeneration) {
        std::size_t kept = 0;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            Entry& entry = entries[i];
            const LedgerAssetFacts* facts = findFacts(input.referenced, entry.guid);
            bool erase = false;
            if (entry.state == LedgerState::Ready) {
                if (facts == nullptr || !facts->recordPresent) {
                    // A guid whose record vanished is not "failed", it is GONE (LG12).
                    retireEntryHandles(entry);
                    pushRetire(out.retire, entry.guid);
                    erase = true;
                } else if (!facts->hashUsable) {
                    // KEEP THE OLD UPLOAD UNTOUCHED. An unhashed record's all-zero digest is the empty
                    // file's real value and never a sentinel (the 3.4.2 lesson), so adopting it would
                    // either re-load forever or freeze on a lie (LG13).
                } else if (!(facts->hash == entry.loadedHash)) {
                    retireEntryHandles(entry);
                    entry.state = LedgerState::Absent;
                    entry.loadedHashUsable = false;
                    pushRetire(out.retire, entry.guid);
                }
            } else if (entry.state == LedgerState::Failed) {
                // A Failed entry is re-examined HERE AND ONLY HERE. New bytes are a new question, so a
                // changed hash clears the stickiness; an unchanged one leaves it exactly as it was
                // (LG7, LG8).
                if (facts != nullptr && facts->hashUsable && !(facts->hash == entry.loadedHash)) {
                    entry.state = LedgerState::Absent;
                    entry.loadedHashUsable = false;
                    entry.message.clear();
                }
            }
            if (!erase) {
                if (kept != i) {
                    entries[kept] = std::move(entries[i]);
                }
                ++kept;
            }
        }
        entries.resize(kept);
        lastGeneration = input.generation;
    }

    // 4. RETIRE UNREFERENCED ENTRIES. Failed entries are erased too -- stickiness is
    // per-session-WHILE-REFERENCED, so deleting the entity, fixing the file and re-adding it heals
    // (LG11). Keeping failures forever would make that sequence not heal, on the wrong side.
    {
        std::size_t kept = 0;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            Entry& entry = entries[i];
            if (findFacts(input.referenced, entry.guid) == nullptr) {
                retireEntryHandles(entry);
                pushRetire(out.retire, entry.guid);
                continue;
            }
            if (kept != i) {
                entries[kept] = std::move(entries[i]);
            }
            ++kept;
        }
        entries.resize(kept);
    }

    // 5. INSERT MISSING ENTRIES. Two facts naming the same guid collapse onto one entry, because the
    // second finds the first (LG23): input dedup is the caller's job, but the ledger must be total.
    //
    // A GUID WITH NO RECORD GETS NO ENTRY, which §D-8 step 5 does not say and step 3 requires: without
    // it, the retire step 3 just performed is undone here, in the same call, and the guid is re-issued
    // every pass forever. The executor cannot even build a path for a guid the database does not know
    // (loadModel takes an AssetRecord), so every one of those directives burns the whole
    // one-per-pass budget and starves every asset that CAN load. A vanished record is gone, and gone
    // means no entry (LG12).
    for (const LedgerAssetFacts& facts : input.referenced) {
        if (!facts.recordPresent || findMutableEntry(facts.guid) != nullptr) {
            continue;
        }
        const auto at = std::lower_bound(entries.begin(), entries.end(), facts.guid,
                                         [](const Entry& e, Guid g) noexcept { return e.guid < g; });
        entries.insert(at, Entry{.guid = facts.guid,
                                 .assetClass = facts.isMaterial ? LedgerAssetClass::Material : LedgerAssetClass::Model,
                                 .state = LedgerState::Absent});
    }

    // 6. CHOOSE AT MOST ONE DIRECTIVE. Models and materials share this one budget.
    //
    // 6a. A PENDING TEXTURE of any Ready entry outranks everything, in entry order (which is guid
    // order) then slot order -- textures first, so a model that is already drawing with default texels
    // finishes dressing before a second model starts loading. That is the preview's own shape, and it
    // is what makes "within a few seconds every slot resolves" true rather than hoped.
    for (Entry& entry : entries) {
        if (entry.state != LedgerState::Ready || entry.pendingTextures.empty()) {
            continue;
        }
        const TextureRequest& request = entry.pendingTextures.front();  // sorted on adoption
        out.directive = LedgerDirective{.guid = request.guid,
                                        .assetClass = LedgerAssetClass::Texture,
                                        .ownerMaterial = entry.guid,
                                        .materialIndex = request.materialIndex,
                                        .slot = request.slot,
                                        .srgb = request.srgb};
        ++entry.attempts;
        ++attemptTotal;
        return out;
    }
    // 6b. Otherwise the first Absent entry, in guid order. `state == Failed` ISSUES NOTHING, EVER --
    // that is the stickiness, and `attempts` not moving is how a case sees it (LG5, LG6, seed S18).
    for (Entry& entry : entries) {
        if (entry.state != LedgerState::Absent) {
            continue;
        }
        // The facts' hash is captured AT ISSUE TIME, so a report that fails still knows WHICH bytes
        // failed -- which is what lets step 3 clear the failure once the bytes change.
        const LedgerAssetFacts* const facts = findFacts(input.referenced, entry.guid);
        // ...and an entry whose record has VANISHED is skipped, for the reason step 5 states above:
        // the loader can do nothing with a guid the database cannot resolve, so issuing for it burns
        // the whole one-per-pass budget and starves every asset that CAN load. Step 5 refuses to
        // INSERT such an entry, but an entry inserted while the record existed and orphaned later is
        // only re-examined by step 3 when it is Ready or Failed -- an Absent one would otherwise be
        // picked forever, climbing loadAttempts() while readyCount() never moves. Found by the
        // code-review round.
        if (facts == nullptr || !facts->recordPresent) {
            continue;
        }
        entry.loadedHash = facts->hash;
        entry.loadedHashUsable = facts->hashUsable;
        out.directive = LedgerDirective{.guid = entry.guid, .assetClass = entry.assetClass};
        ++entry.attempts;
        ++attemptTotal;
        return out;
    }

    // 7. Nothing to issue.
    return out;
}

void SceneAssetLedger::reportLoaded(Guid guid, ContentHash hash, const LedgerHandles& handles,
                                    std::span<const TextureRequest> textureRequests, LedgerAssetClass assetClass) {
    Entry* entry = findMutableEntry(guid);
    if (entry == nullptr) {
        // ADOPT, never discard. This used to `return`, on the theory that a missing entry meant the
        // entry had retired between the directive and the report and "the caller destroys its own" --
        // which no caller ever did. The reachable case is the opposite one: the drop path reports from
        // tick()'s reconcile block, which runs BEFORE serviceSceneAssets inserts entries, so on the tick
        // a drop lands there is no entry yet. The live MeshHandle and every MaterialHandle went on the
        // floor -- never adopted, never deferred, never destroyed -- while the binding table already
        // named them, and the ledger then re-imported, re-cooked and re-uploaded the same bytes on the
        // next pass. Reachable wherever the drop performs a Full import: .blend always, plus the
        // NoNodes fallback for .obj/.ply/.stl. A .gltf takes the Structure path and never gets here,
        // which is exactly why the .gltf-based drop cases could not see it. Found by the code-review
        // round. The adopted entry is referenced by the entity the drop just created, so step 4 keeps
        // it and step 5 finds it already present.
        const auto at = std::lower_bound(entries.begin(), entries.end(), guid,
                                         [](const Entry& e, Guid g) noexcept { return e.guid < g; });
        entry = &*entries.insert(at, Entry{.guid = guid, .assetClass = assetClass, .state = LedgerState::Absent});
    }
    // A re-report over live handles would strand the old ones. They go onto the DEFERRED list, never
    // destroyed here -- this class destroys nothing, ever.
    retireEntryHandles(*entry);
    entry->state = LedgerState::Ready;
    entry->loadedHash = hash;
    entry->loadedHashUsable = true;
    entry->message.clear();
    entry->handles = handles;
    // The parallel vector's length is enforced HERE, on adoption, rather than trusted: rebindSlot
    // indexes both with one materialIndex.
    entry->handles.materialStates.resize(entry->handles.materials.size());
    entry->pendingTextures.assign(textureRequests.begin(), textureRequests.end());
    std::sort(entry->pendingTextures.begin(), entry->pendingTextures.end(), textureRequestBefore);
    for (const std::pair<std::uint32_t, Aabb>& box : entry->handles.bounds) {
        bounds.set(MeshBoundsKey{.mesh = guid, .meshIndex = box.first}, box.second);
    }
}

void SceneAssetLedger::reportFailed(Guid guid, std::string message) {
    Entry* entry = findMutableEntry(guid);
    if (entry == nullptr) {
        return;
    }
    // A half-loaded model must not keep a mesh alive that nothing will ever bind -- so whatever this
    // entry already held joins the deferred destroy list.
    retireEntryHandles(*entry);
    entry->state = LedgerState::Failed;
    entry->message = std::move(message);
}

LedgerSlotBinding SceneAssetLedger::reportSlotTexture(const LedgerDirective& directive, rhi::TextureHandle texture) {
    Entry* entry = findMutableEntry(directive.ownerMaterial);
    if (entry == nullptr) {
        return {};
    }
    // Cleared whether or not the upload succeeded: a broken image costs one attempt per session, not
    // one per pass.
    const auto it = std::find_if(entry->pendingTextures.begin(), entry->pendingTextures.end(),
                                 [&directive](const TextureRequest& request) noexcept {
                                     return request.materialIndex == directive.materialIndex &&
                                            request.slot == directive.slot && request.guid == directive.guid;
                                 });
    if (it != entry->pendingTextures.end()) {
        entry->pendingTextures.erase(it);
    }
    if (!texture.valid() || directive.materialIndex >= entry->handles.materials.size()) {
        return {};
    }
    // ADOPTED: the handle joins what this entry owns, so it dies with the entry, on the deferred list.
    entry->handles.textures.push_back(texture);
    return LedgerSlotBinding{.material = entry->handles.materials[directive.materialIndex],
                             .state = &entry->handles.materialStates[directive.materialIndex]};
}

std::vector<LedgerDestroy> SceneAssetLedger::resetForProjectSwap() {
    for (Entry& entry : entries) {
        retireEntryHandles(entry);
    }
    entries.clear();
    bounds.clear();
    lastGeneration = 0;
    // The RETURNED list carries anything already deferred from the previous pass as well, which is what
    // makes "nothing is returned twice" true: the member is left empty, so the next service() returns
    // nothing (LG24).
    std::vector<LedgerDestroy> released = std::move(pendingDestroy);
    pendingDestroy.clear();
    return released;
}

LedgerState SceneAssetLedger::stateOf(Guid guid) const noexcept {
    const Entry* entry = findEntry(guid);
    return entry != nullptr ? entry->state : LedgerState::Absent;
}

std::string_view SceneAssetLedger::messageOf(Guid guid) const noexcept {
    const Entry* entry = findEntry(guid);
    return entry != nullptr ? std::string_view(entry->message) : std::string_view{};
}

std::size_t SceneAssetLedger::entryCount() const noexcept { return entries.size(); }

std::size_t SceneAssetLedger::readyCount() const noexcept {
    std::size_t ready = 0;
    for (const Entry& entry : entries) {
        if (entry.state == LedgerState::Ready) {
            ++ready;
        }
    }
    return ready;
}

std::size_t SceneAssetLedger::failedCount() const noexcept {
    std::size_t failed = 0;
    for (const Entry& entry : entries) {
        if (entry.state == LedgerState::Failed) {
            ++failed;
        }
    }
    return failed;
}

std::size_t SceneAssetLedger::loadAttempts() const noexcept { return attemptTotal; }

const MeshBoundsLookup& SceneAssetLedger::boundsLookup() const noexcept { return bounds; }

const LedgerHandles* SceneAssetLedger::handlesOf(Guid guid) const noexcept {
    const Entry* entry = findEntry(guid);
    return entry != nullptr ? &entry->handles : nullptr;
}

}  // namespace engine::editor

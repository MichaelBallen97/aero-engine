#pragma once
// Aero Engine — the scene-asset ledger (task 3.1.5, §D-8). PUBLIC AND PURE: it decides WHAT should be
// loaded, retired and destroyed, and it executes none of it. No GPU call, no disk access, no
// <filesystem>, no <fstream>, no AERO_LOG, no Dear ImGui, no panel, no World and no AssetDatabase --
// the caller collects five plain values per referenced guid into a LedgerAssetFacts and hands them in,
// which is skeleton_cook_source's posture (values in, values out) applied to a state machine and is
// what makes every case below a hand-built vector with no fixture and no device.
//
// THE ONE ORDERING THAT MATTERS, stated before anything else, because it is the whole of INV-D2's GPU
// half: service() returns the PREVIOUS pass's destroy list FIRST, before any step of THIS pass can add
// to it. So a whole service pass separates "the binding table stopped naming this handle" from "the GPU
// object dies", and nothing recorded in frame N can still name a handle destroyed in pass N+1. That is
// why `pendingDestroy` is a MEMBER and not a local, and why the deferral cannot be reconstructed from
// the retire list -- rebuilding it there would destroy this pass's retirements this pass, which is the
// same defect wearing a different hat.
//
// It names three renderer/rhi handle types and nothing else from those layers: they are what the
// destroy list carries, and a ledger that could not say which object it is retiring could not defer
// anything. Sorted vectors throughout, never a hash container: this type is reachable from EditorApp,
// whose move is `noexcept = default`, and MSVC's node-based containers are not nothrow-movable (3.1.2's
// R9, measured in CI as C2607).
#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/scene_bounds.hpp>  // Aabb, MeshBoundsKey, MeshBoundsLookup
#include <aero/render/material.hpp>      // render::MaterialHandle, MaterialParams, MaterialTextureSlots
#include <aero/render/mesh.hpp>          // render::MeshHandle
#include <aero/rhi/handles.hpp>          // rhi::TextureHandle

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

// Absent is 0 and is what an unknown guid answers, so "never asked about" and "asked about, not loaded
// yet" are deliberately the same state: neither has a handle, and the difference is entryCount()'s job.
enum class LedgerState : std::uint8_t { Absent = 0, Ready, Failed };

// NEVER named toString. doctest's DOCTEST_STRINGIFY expands to an UNQUALIFIED toString(...), which ADL
// finds on an engine enum, beats doctest's own template, and turns every CHECK on this type into a hard
// compile error inside doctest.h (.claude/rules/ci-portability.md).
[[nodiscard]] std::string_view ledgerStateLabel(LedgerState state) noexcept;

enum class LedgerAssetClass : std::uint8_t { Model = 0, Material, Texture };

// ONE referenced guid, as the caller sees it THIS tick. Five plain values, so a case builds one by
// hand and the ledger needs no database to be testable.
struct LedgerAssetFacts {
    Guid guid;
    bool isMaterial = false;     // .aeromat vs model -- decided by the CALLER from the record's own path
    bool recordPresent = false;  // database.findByGuid(guid) != nullptr
    // assetContentHashUsable(record) -- the 3.4.2 lesson, and the reason `hash` below is never read
    // without it: an unhashed record's ALL-ZERO digest is the empty file's real value and never a
    // sentinel, so adopting it would either re-load forever or freeze on a lie.
    bool hashUsable = false;
    ContentHash hash{};  // meaningful iff hashUsable
};

// One slot of one material that wants a texture. The SAME type the loader returns per bound slot, so
// the request the loader reports and the directive the ledger issues can never disagree about their
// four fields.
struct TextureRequest {
    std::size_t materialIndex = 0;  // WHICH of the entry's materials -- a model has one per source material
    std::size_t slot = 0;           // 0..MATERIAL_TEXTURE_SLOT_COUNT-1, binding order (material.hpp)
    Guid guid;                      // the TEXTURE asset
    bool srgb = false;              // materialSlotIsSrgb(slot) -- the slot decides, never the file
};

struct LedgerDirective {
    Guid guid;  // for a Texture directive this is the TEXTURE's guid
    LedgerAssetClass assetClass = LedgerAssetClass::Model;
    // Texture directives only: which entry's material binding wants it, which of that entry's
    // materials, which slot, and in which colour space.
    Guid ownerMaterial;             // nil for Model/Material directives
    std::size_t materialIndex = 0;  // ditto
    std::size_t slot = 0;
    bool srgb = false;
};

struct LedgerServiceInput {
    std::span<const LedgerAssetFacts> referenced;  // this tick's referenced set, ANY order
    std::uint64_t generation = 0;                  // AssetDatabase::generation()
    std::span<const Guid> nudged;                  // guids to mark stale NOW (the Apply nudge)
};

// Handed back ONE SERVICE PASS AFTER the binding table stopped naming it. At most one of the three is
// valid per entry, so the caller's destroy step is three ifs and no dispatch.
struct LedgerDestroy {
    render::MeshHandle mesh{};
    render::MaterialHandle material{};
    rhi::TextureHandle texture{};
};

struct LedgerServiceOutput {
    std::optional<LedgerDirective> directive;  // AT MOST ONE per service -- the budget, models and
                                               // materials sharing it
    std::vector<Guid> retire;                  // bindings whose guid left the referenced set or lost its record; the
                               // caller removes them from the table THIS pass and their handles die NEXT
    std::vector<LedgerDestroy> destroy;  // handles retired ONE PASS AGO -- safe to destroy NOW
};

// What a live material costs to remember. ForwardRenderer offers no read-back, so an arriving slot
// texture cannot be merged into a material without somebody holding its current params and slots; the
// ledger already owns the entry's lifetime and is the only thing that knows when they die.
struct MaterialRuntimeState {
    render::MaterialParams params;
    render::MaterialTextureSlots slots;
};

// The loader -> ledger report. Everything the entry OWNS and must eventually release.
struct LedgerHandles {
    render::MeshHandle mesh{};                      // model directives
    std::vector<render::MaterialHandle> materials;  // model: one per source material; material: one
    // PARALLEL TO `materials`, always the same length (the ledger enforces it on adoption). A parallel
    // vector is acceptable here because both are written in exactly one place and read in exactly one.
    std::vector<MaterialRuntimeState> materialStates;
    std::vector<rhi::TextureHandle> textures;  // every slot texture this entry OWNS
    // (sourceMeshIndex, mesh-LOCAL box), model only. §9.5/§9.8 make CookedSubmesh::bounds
    // node-independent -- folded from `positions` alone -- which is exactly what an entity-local box
    // must be, and is why no node transform enters this computation.
    std::vector<std::pair<std::uint32_t, Aabb>> bounds;
};

// What reportSlotTexture hands back so the caller can finish the rebind. Both fields are engaged
// together or not at all; a null `state` means there is nothing to rebind (an invalid upload, or an
// entry that retired between the directive and the report).
struct LedgerSlotBinding {
    render::MaterialHandle material{};
    MaterialRuntimeState* state = nullptr;  // BORROWED from the ledger entry; valid until the next
                                            // service()/report call, exactly like messageOf()'s view
};

class SceneAssetLedger {
public:
    // The seven steps, in the order that IS the design -- see the .cpp, where each step is numbered and
    // each is a case. Step 1 is the destroy deferral and it runs before everything else.
    [[nodiscard]] LedgerServiceOutput service(const LedgerServiceInput& input);

    // Records what the loader DID with the directive service() handed out. Split from service() on
    // purpose: the ledger is pure and cannot execute, so the two halves of "issue, then learn the
    // outcome" are two calls, and a loader that forgets to report leaves the entry Absent and the
    // budget re-issues -- visibly, through loadAttempts, rather than silently.
    //
    // `textureRequests` is TRAILING AND DEFAULTED (the writeMetaText / entityBounds precedent), so a
    // caller with no slots to dress calls the three-argument form unchanged. Each request becomes one
    // pending texture directive, ordered by (materialIndex, slot) so "slot order" is a property of the
    // storage rather than of the loop that reads it.
    // ADOPTS the handles. If no entry exists for `guid` yet, one is INSERTED with `assetClass` rather
    // than the report being dropped -- the drop path reports from the reconcile block, before
    // serviceSceneAssets has inserted anything, and discarding there stranded live GPU handles and
    // forced a second import of the same bytes (the code-review round).
    void reportLoaded(Guid guid, ContentHash hash, const LedgerHandles& handles,
                      std::span<const TextureRequest> textureRequests = {},
                      LedgerAssetClass assetClass = LedgerAssetClass::Model);
    void reportFailed(Guid guid, std::string message);

    // The texture half of "issue, then learn the outcome". A slot texture never gets its own ENTRY --
    // only a referenced model or material does -- so its outcome is recorded against the entry that
    // asked for it. STICKY EITHER WAY: the pending slot is cleared whether or not the upload
    // succeeded, so a broken image costs one attempt per session rather than one per pass (3.1.3's
    // ThumbnailLedger rule, restated). A VALID handle is additionally ADOPTED into that entry's owned
    // textures, so it dies with the entry, on the deferred list like everything else.
    [[nodiscard]] LedgerSlotBinding reportSlotTexture(const LedgerDirective& directive, rhi::TextureHandle texture);

    // Moves EVERY live handle onto the destroy list and returns it -- INCLUDING anything already
    // deferred from the previous pass, which is what makes "nothing is returned twice" true. The ledger
    // ends empty. One call, so a caller cannot half-reset: project swap and shutdown both use it.
    [[nodiscard]] std::vector<LedgerDestroy> resetForProjectSwap();

    [[nodiscard]] LedgerState stateOf(Guid guid) const noexcept;
    // "" unless Failed. A view into ledger state, valid until the next service()/report call -- the
    // MaterialSession::lastMessage() lifetime, restated.
    [[nodiscard]] std::string_view messageOf(Guid guid) const noexcept;
    [[nodiscard]] std::size_t entryCount() const noexcept;
    [[nodiscard]] std::size_t readyCount() const noexcept;
    [[nodiscard]] std::size_t failedCount() const noexcept;
    // Directives ISSUED, counted when the directive leaves service() rather than when it succeeds, so a
    // retry loop reads as attempts climbing while readyCount() stays flat. Monotonic for this ledger's
    // lifetime; resetForProjectSwap does NOT reset it, for the same reason the preview's does not.
    [[nodiscard]] std::size_t loadAttempts() const noexcept;
    [[nodiscard]] const MeshBoundsLookup& boundsLookup() const noexcept;
    // The entry's own handles, for the caller that must install a binding or rebind a slot. nullptr for
    // an unknown guid. BORROWED, with messageOf()'s lifetime.
    [[nodiscard]] const LedgerHandles* handlesOf(Guid guid) const noexcept;

private:
    struct Entry {
        Guid guid;
        LedgerAssetClass assetClass = LedgerAssetClass::Model;
        LedgerState state = LedgerState::Absent;
        // The hash of the bytes this entry currently reflects. Written at ISSUE time from the facts (so
        // a FAILED entry knows which bytes failed, which is what lets new bytes clear it) and again by
        // reportLoaded from what the loader actually saw.
        ContentHash loadedHash{};
        bool loadedHashUsable = false;
        std::uint32_t attempts = 0;
        std::string message;    // "" unless Failed
        LedgerHandles handles;  // what reportLoaded recorded; moved to `pendingDestroy` on retire
        // Per-slot texture directives still to issue, sorted by (materialIndex, slot).
        std::vector<TextureRequest> pendingTextures;
    };

    [[nodiscard]] Entry* findMutableEntry(Guid guid) noexcept;
    [[nodiscard]] const Entry* findEntry(Guid guid) const noexcept;
    // Moves every handle this entry owns onto `pendingDestroy`, drops its pending slots and removes its
    // bounds. It NEVER destroys anything: that is the caller's job, one pass later.
    void retireEntryHandles(Entry& entry);

    std::vector<Entry> entries;                 // SORTED by guid
    std::vector<LedgerDestroy> pendingDestroy;  // filled THIS pass, RETURNED next pass -- the deferral
    std::uint64_t lastGeneration = 0;
    std::size_t attemptTotal = 0;
    MeshBoundsLookup bounds;
};

}  // namespace engine::editor

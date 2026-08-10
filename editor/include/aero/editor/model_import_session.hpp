#pragma once
// Aero Engine — the on-demand model import driver (task 3.2.1). PUBLIC, ImGui-free, entt-free,
// SDL-free, fastgltf-free and <filesystem>-free (AC-57). It DOES read files -- through text_file.hpp's
// readFileBytes and writeTextFileAtomic, which own every byte the editor touches -- and it holds the
// ONLY write this whole task adds anywhere (INV-M9). NOTHING HERE LOGS.
#include <aero/core/guid.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/blender_service.hpp>  // task 3.2.4 -- a VALUE member, see below
#include <aero/editor/model_import.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace engine::editor {

enum class SessionState : std::uint8_t {
    Idle = 0,       // nothing selected
    NotImportable,  // something selected, but no importer claims it (or it is a directory -- E17).
                    // After task 3.2.4 a .blend NEVER lands here: this enumerator keeps its exact
                    // meaning, which is what makes it honest, and telling a user "no importer claims
                    // this file type" about a .blend became factually false the moment the Blender
                    // path shipped.
    Imported,       // status is Ok or Truncated
    Failed,         // every other ImportStatus
    // ---- task 3.2.4, APPENDED (never inserted) ----
    NeedsConversion,   // a .blend with no valid cached artifact. ALSO the nil-GUID case (AC-27): the
                       // panel draws the Blender section and DISABLES the button, which
                       // NotImportable -- which renders one sentence and returns before any section
                       // -- structurally cannot do.
    Converting,        // a .blend whose Blender run is in flight
    ConversionFailed,  // the run finished badly; blender().message() carries the reason
};

class ModelImportSession {
public:
    // ---- the RECONCILE surface (2.6.1 D10's pattern, a third application). EditorApp COMPARES and
    // calls only on a mismatch; NOTHING EVER PUSHES INTO THIS CLASS. ------------------------------
    void setTarget(std::string relativePath, std::uint64_t databaseGeneration);
    [[nodiscard]] const std::string& target() const noexcept { return targetPath; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generationValue; }

    // THE ONE IMPURE ENTRY POINT. Called from EditorApp::tick() ONLY, never from onDraw() (AC-48;
    // 3.1.3's BLOCKING-1 rule shape, a third application). Does NOTHING when the target is already
    // imported at this generation -- so AC-45's "exactly one import" is STRUCTURAL, not a call-site
    // convention, and ten further ticks cost ten early returns.
    //
    // task 3.2.4: `deltaSeconds` is ONE NEW PARAMETER, TRAILING and DEFAULTED, so every existing call
    // site compiles UNEDITED (AC-45; the writeMetaText precedent, 3.2.2). The PROJECT root is NOT a
    // parameter: database.projectRoot() already carries it, authoritatively and never derived from
    // the assets root (3.1.2's own AC-38). It is forwarded to BlenderService::poll(), which is called
    // from HERE and from nowhere else in the tree (AC-38).
    void service(std::string_view assetsRootUtf8, const AssetDatabase& database, float deltaSeconds = 0.0F);

    [[nodiscard]] SessionState state() const noexcept { return stateValue; }
    [[nodiscard]] const ImportResult& result() const noexcept { return resultValue; }
    [[nodiscard]] std::size_t importCount() const noexcept { return imports; }           // the AC-45/AC-47 observable
    [[nodiscard]] std::uint64_t fileSizeBytes() const noexcept { return observedSize; }  // shown BEFORE import (R5)

    // ---- the settings FORM -----------------------------------------------------------------------
    [[nodiscard]] const ImportSettings& pendingSettings() const noexcept { return pending; }
    [[nodiscard]] const ImportSettings& diskSettings() const noexcept { return onDisk; }
    [[nodiscard]] bool settingsDirty() const noexcept { return !(pending == onDisk); }
    void setPendingSettings(ImportSettings s) noexcept { pending = s; }
    void revertSettings() noexcept { pending = onDisk; }
    // E16/AC-51: FALSE when the target's identity is invalid (a nil GUID). Writing a sidecar for an
    // invalid identity would repair one BY THE BACK DOOR, which D7 forbids permanently.
    [[nodiscard]] bool canApply() const noexcept { return targetGuid.valid() && settingsDirty(); }
    // Writes the sidecar ATOMICALLY; returns "" on success OR when there is nothing to apply (not
    // dirty), the OS reason otherwise. THE ONLY WRITE IN THIS ENTIRE TASK (INV-M9). Never called from
    // onDraw(). code-review SHOULD-FIX 5: enforces the FULL canApply() condition itself (valid GUID
    // AND dirty), not just the GUID half -- a hook-driven Apply that bypasses the panel's own
    // BeginDisabled(!canApply()) must not rewrite a byte-identical sidecar for nothing.
    [[nodiscard]] std::string applySettings(std::string_view assetsRootUtf8);
    [[nodiscard]] const std::string& applyError() const noexcept { return lastApplyError; }

    // ---- task 3.2.4: the .blend conversion surface ------------------------------------------------
    // One-shots, set by EditorApp::tick() from the panel's own request channels and consumed by the
    // NEXT service(). Each also RE-ARMS the (target, generation) consume guard, because a request
    // arrives when the session has long since been serviced for this pair and would otherwise take
    // the early return forever.
    void requestConversion() noexcept;
    void cancelConversion() noexcept;
    [[nodiscard]] const BlenderService& blender() const noexcept { return blenderService; }
    // The ONE mutable reach, for EditorApp::tick()'s lazy resolve() and its Locate.../Re-detect
    // handling. The PANEL cannot call it at all: it holds a `const ModelImportSession*`, so AC-39 is a
    // compile-time property here rather than a convention.
    [[nodiscard]] BlenderService& blenderMutable() noexcept { return blenderService; }
    // AC-27: canApply()'s GUID half, lifted. canApply() ALSO requires settingsDirty(), so it cannot be
    // reused for "may this .blend be converted at all?" -- a freshly selected .blend is never dirty.
    [[nodiscard]] bool targetHasIdentity() const noexcept { return targetGuid.valid(); }

private:
    // task 3.2.4: the .blend arm, run INSTEAD of the two-pass importer path. Split out only for
    // readability -- service()'s existing early-return structure above it is unchanged.
    void serviceBlend(std::string_view assetsRootUtf8, const AssetDatabase& database, float deltaSeconds,
                      bool resyncForm);

    std::string targetPath;             // "" == nothing selected
    std::uint64_t generationValue = 0;  // the AssetDatabase generation this result belongs to
    bool serviced = false;              // the (target, generation) pair has been consumed
    // code-review SHOULD-FIX 4: TRUE only across the window between a setTarget() call that changed
    // the TARGET PATH and the next service() call that resolves it -- consumed (read once, cleared)
    // by service(), which resyncs pending/onDisk from the database ONLY when this is true. A
    // generation-only bump (an unrelated file's rescan) leaves it false, so an in-progress, unapplied
    // edit on the SAME target survives a re-service instead of being silently overwritten from disk.
    bool formNeedsResync = false;
    SessionState stateValue = SessionState::Idle;
    ImportResult resultValue;
    ImportSettings pending;
    ImportSettings onDisk;
    Guid targetGuid;  // cached at service() time (plan §A-16) -- applySettings takes no database
    std::uint64_t observedSize = 0;
    std::size_t imports = 0;
    std::string lastApplyError;
    // task 3.2.4: a VALUE member, exactly as this class is itself a value member of EditorApp. That is
    // what makes the static_asserts below evaluate BlenderService's own move -- and what makes its
    // named-deleter-over-an-incomplete-type PIMPL load-bearing rather than stylistic.
    BlenderService blenderService;
    bool conversionRequested = false;  // one-shot, drained by service()
    bool cancelRequested = false;      // one-shot, drained by service()
    // INV-M13: SORTED VECTORS ONLY -- no std::unordered_map, no std::set. This is a VALUE member of
    // EditorApp, whose move is `noexcept = default`, and MSVC's node-based containers are not
    // nothrow-move-CONSTRUCTIBLE (3.1.2's R9, measured in CI as C2607). This class holds none today;
    // the rule exists so a future field cannot break EditorApp's move on a lane this machine cannot test.
};

// The aggregate-first assert pair (command_stack.hpp:189-194's shape, a fifth application), so a
// regression NAMES this type instead of failing an opaque member's assert.
static_assert(std::is_nothrow_move_constructible_v<ModelImportSession>);
static_assert(std::is_nothrow_move_assignable_v<ModelImportSession>);

}  // namespace engine::editor

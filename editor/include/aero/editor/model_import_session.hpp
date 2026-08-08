#pragma once
// Aero Engine — the on-demand model import driver (task 3.2.1). PUBLIC, ImGui-free, entt-free,
// SDL-free, fastgltf-free and <filesystem>-free (AC-57). It DOES read files -- through text_file.hpp's
// readFileBytes and writeTextFileAtomic, which own every byte the editor touches -- and it holds the
// ONLY write this whole task adds anywhere (INV-M9). NOTHING HERE LOGS.
#include <aero/core/guid.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/model_import.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace engine::editor {

enum class SessionState : std::uint8_t {
    Idle = 0,       // nothing selected
    NotImportable,  // something selected, but no importer claims it (or it is a directory -- E17)
    Imported,       // status is Ok or Truncated
    Failed,         // every other ImportStatus
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
    void service(std::string_view assetsRootUtf8, const AssetDatabase& database);

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

private:
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

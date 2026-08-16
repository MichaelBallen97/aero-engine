#pragma once
// Aero Engine — the material edit session (task 3.4.2, D3/D4/D5/D10/D12). PUBLIC, GPU-FREE and
// ImGui-FREE: it names MaterialDocument and MaterialError and no render, rhi or ImGui type at all.
// The state machine behind the Material panel -- what is targeted, whether it diverges from the file,
// and the two explicit operations (Apply, Revert) that can change bytes on disk.
//
// HOLDS VALUES, TAKES THE DATABASE AS A PARAMETER (A-2 / INV-4). No `const AssetDatabase*` member, no
// World&, no Selection&: EditorApp is movable and create() returns std::optional<EditorApp>, so a
// reference member binds to a pre-move address. That is 3.1.1's D13, whose sabotage seed S23 reddened
// NOTHING -- a recorded coverage gap rather than proof the reference form is safe.
//
// DRIVEN FROM EditorApp::tick() ONLY, never from a panel. The panel reads a const view and records a
// pending whole-document edit; tick() drains it into edit() and then calls service().
#include <aero/core/content_hash.hpp>
#include <aero/reflect/material_format.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace engine::editor {

class AssetDatabase;  // BY PARAMETER ONLY (A-2/INV-4) -- never a member, so only the name is needed

// The cap on a `.aeromat` READ, enforced by readFileBytes from std::filesystem::file_size ALONE -- the
// file is never opened when it trips. readTextFile, which this session used until the code-review
// round, is documented as having NO cap and materialises whatever it is pointed at through an
// istreambuf_iterator, so one browser click on a multi-gigabyte file named `*.aeromat` was an
// out-of-memory abort inside a draw-driven reconcile. The preview's own texture path already capped at
// 64 MiB for the identical reason; nothing else about a click should be able to do more damage than
// looking at a picture.
//
// 4 MiB, and the number is not arbitrary in either direction. A canonical material document is a few
// hundred bytes and the format is bounded by construction -- ten scalars and five slots, no arrays,
// docs/09 section 11 -- so the cap sits four orders of magnitude above anything writeMaterialText can
// produce, which means it can only ever fire on a file that is not a material. It is deliberately not
// tighter: an unknown-key-laden or heavily-commented hand-authored file is legal input this editor
// must keep loading, and a cap a user can trip by hand is a cap that gets raised in a hurry.
inline constexpr std::uint64_t MAX_MATERIAL_FILE_BYTES = 4ULL * 1024 * 1024;

// What the panel renders. One value, no pointers.
enum class MaterialSessionState : std::uint8_t {
    Untargeted,  // nothing selected yet, or the record vanished
    Error,       // the file failed to read, parse or validate -- no editor at all (AC-9)
    Ready,       // fileCopy and sessionCopy are both engaged
};

// THE ONLY function in this tree that writes .aeromat bytes (D12). Both logical writes -- Apply, and
// task 3.4.2's New Material -- route through it, and its own definition holds the single
// writeTextFileAtomic call site in material_session.cpp, with the absolute path arriving as a NAMED
// LOCAL at every caller so the amended INV-A1 stays grep-decidable rather than heuristic.
// Never called from a draw walk, never from a scan, rescan, watcher or selection path (INV-2).
// Returns "" on success, the OS reason otherwise. NO validation: callers wanting the round-trip
// guarantee run validateMaterial first, exactly as writeMaterial's own comment requires.
[[nodiscard]] std::string saveMaterialFile(std::string_view absolutePathUtf8, const MaterialDocument& document);

// True iff the path's LEAF ends in ".aeromat", ASCII-case-folded -- composed from
// classifyAssetKind so this predicate and the browser's kind table can never disagree.
[[nodiscard]] bool isMaterialFileName(std::string_view relativePath) noexcept;

class MaterialSession {
public:
    // ---- driven from EditorApp::tick() ONLY ------------------------------------------------------
    // D3's STICKY rule, all arms. `selection` is the browser's assets-relative path; `generation` is
    // AssetDatabase::generation(). RETARGETS ONLY on a DIFFERENT, EXISTING *.aeromat -- selecting a
    // texture to reference must not tear down an edit session, and the browser has one selection.
    // On a generation bump with the target unchanged: a vanished record CLEARS the session (AC-14), a
    // moved content hash reloads a CLEAN session silently and leaves a DIRTY one alone behind a
    // notice.
    void reconcile(std::string_view selection, std::uint64_t generation, const AssetDatabase& database,
                   std::string_view assetsRootAbs);
    // AC-15. Resets EVERY cross-frame field; ME44 asserts each one individually.
    void resetForProjectSwap() noexcept;
    // The panel's drained whole-document edit. A no-op when nothing is targeted, and a no-op when the
    // document equals the current session copy -- so a drag that produces the same value costs no
    // preview re-push.
    void edit(const MaterialDocument& document);
    void requestApply() noexcept;
    void requestRevert() noexcept;
    // Performs AT MOST ONE of the two one-shots, Apply first. Apply: refuse unless Ready and dirty ->
    // validateMaterial -> ONE saveMaterialFile -> adopt. Revert: re-read and re-parse. A validation
    // failure changes NOTHING anywhere (INV-7).
    void service(const AssetDatabase& database, std::string_view assetsRootAbs);
    // One-shot: "the document changed, the preview must re-push". Set by a load, an edit and a revert.
    [[nodiscard]] bool takeDocumentChanged() noexcept;

    // ---- reads -----------------------------------------------------------------------------------
    [[nodiscard]] MaterialSessionState state() const noexcept;
    [[nodiscard]] std::string_view targetPath() const noexcept;           // "" when Untargeted
    [[nodiscard]] const MaterialDocument* document() const noexcept;      // the SESSION copy; null unless Ready
    [[nodiscard]] const MaterialDocument* fileDocument() const noexcept;  // the last-loaded FILE copy
    // sessionCopy != fileCopy through MaterialDocument's DEFAULTED ==, never a byte comparison against
    // what writeMaterialText would produce (D5/A-10). A valid but non-canonical file therefore loads
    // CLEAN, and the editor never rewrites a file nobody edited (INV-2).
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] const MaterialError* error() const noexcept;  // engaged iff state() == Error
    // The parser's own per-key "ignoring unknown key" list for the LOADED file, in source order, shown
    // by the panel's status strip. ENGINE-PROVIDED as of this task: MaterialParseResult carries the
    // warnings it logs, so nothing here re-derives docs/09 section 11's key set and the panel and the
    // Console say the same sentence. Empty for a rejected file, by the parser's own contract.
    [[nodiscard]] std::span<const std::string> warnings() const noexcept;
    // True while the file changed on disk UNDER a dirty session (AC-14). A clean session reloads
    // silently and never sets this.
    [[nodiscard]] bool externalChangeNoticed() const noexcept;
    [[nodiscard]] std::string_view lastMessage() const noexcept;  // the apply/revert/clear notice
    // How many times this session has WRITTEN a file. ME39/ME40's non-vacuity: without it, "Apply on a
    // clean session writes nothing" is indistinguishable from "Apply did nothing observable". It is a
    // LIFETIME counter, deliberately NOT reset by a retarget or a project swap -- see clear().
    [[nodiscard]] std::size_t writeCount() const noexcept;

private:
    void clear() noexcept;
    void loadTarget(std::string_view assetsRootAbs);

    // EVERY FIELD BELOW IS CROSS-FRAME (3.2.4's recorded class: a per-tick output read as the next
    // tick's input with nothing resetting it was that task's three worst code-review findings). Every
    // reset path -- retarget, clear, resetForProjectSwap -- resets ALL of them, and ME44 asserts each
    // one individually so a forgotten field is a red test rather than a discovery.
    std::string targetPathValue;
    std::uint64_t targetGeneration = 0;
    std::optional<ContentHash> lastKnownContentHash;
    std::optional<MaterialDocument> fileCopy;
    std::optional<MaterialDocument> sessionCopy;
    std::optional<MaterialError> parseError;
    std::vector<std::string> parseWarnings;
    std::string message;
    bool externalChange = false;
    bool applyRequested = false;
    bool revertRequested = false;
    bool documentChanged = false;
    std::size_t writes = 0;
};

// EditorApp holds a MaterialSession BY VALUE and its own move is `noexcept = default`, so this type
// must be nothrow-movable or that defaulted move silently disappears (MSVC reports it as C2607;
// 3.1.2's R9 measured it). Sorted std::vectors and std::strings only -- no std::unordered_map and no
// std::set, whose MSVC node-based moves are not nothrow (INV-W9's rule, one type over).
static_assert(std::is_nothrow_move_constructible_v<MaterialSession>);
static_assert(std::is_nothrow_move_assignable_v<MaterialSession>);

}  // namespace engine::editor

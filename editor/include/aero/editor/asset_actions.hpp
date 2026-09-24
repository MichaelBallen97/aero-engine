#pragma once
// Aero Engine — the orphan-sidecar delete action (task 3.1.3). PUBLIC, and the asset_meta.hpp shape
// verbatim: free of ImGui, SDL, entt and every build gate. The pure half is a PLANNER (asset_cache.hpp
// D17's shape, 3.1.2's precedent): the policy is a function of values, and the ONE `.cpp` call site is
// glue plus exactly one std::filesystem::remove.
//
// This is the FIRST destructive path in the editor (R5): every check below is re-verified from disk,
// in a fixed order, and any doubt refuses rather than guesses.
#include <aero/editor/asset_meta.hpp>     // task E.4.3 (commit 1) -- isMetaFileName, used by validateOrphanPath
#include <aero/editor/project_files.hpp>  // task E.4.3 (commit 1) -- leafOf

#include <cstdint>
#include <string>
#include <string_view>

namespace engine::editor {

enum class OrphanDeleteRefusal : std::uint8_t {
    None = 0,
    NotAMetaName,  // the path does not end in .meta (isMetaFileName on the LEAF)
    EscapesRoot,   // an absolute path, a rooted drive letter, a ".." segment, or a backslash
    Missing,       // the file is gone -- someone else deleted it. Nothing to do; not an error
    NotAMeta,      // it exists but does not parse as a .meta v1 (D12 check 4)
    AssetPresent,  // the asset it names exists again -- it is NOT an orphan any more (E22)
    RemoveFailed,  // the OS refused
};

struct OrphanDeleteResult {
    bool deleted = false;
    OrphanDeleteRefusal refusal = OrphanDeleteRefusal::None;
    std::string message;  // the exact human text the CALLER logs; "" iff deleted
};

// ---- task E.4.3: the file-operation vocabulary ---------------------------------------------------

// Why an OPERATION is refused. Ordered as the ladder evaluates them, which is ALSO the order
// executeAssetOpPlan re-verifies against disk. The two orders are the same on purpose: a refusal the
// planner produces and a refusal the executor produces for the same cause carry the same enumerator.
//
// It lands WHOLE in the commit that introduces it, even though only two enumerators are reachable
// there. The alternative -- a two-enumerator enum widened later -- would be a behaviour-changing edit
// to an enum's underlying values, which is exactly what performance-enum-size and this tree's
// append-only discipline exist to avoid.
enum class AssetOpRefusal : std::uint8_t {
    None = 0,
    NoProject,                // an empty or non-absolute assets/project root (looksLikeAnAbsoluteRoot)
    BadSourcePath,            // validateRelativeAssetPath on the source
    BadDestination,           // validateRelativeAssetPath on the destination directory
    SourceIsRoot,             // "" -- the assets root is the project's, not the browser's
    BadName,                  // validateAssetName said no; `nameRefusal` carries WHICH rule
    SourceMissing,            // re-verified from disk immediately before acting
    DestinationMissing,       // the destination is absent, or exists and is not a directory
    DestinationInsideSource,  // moving a folder into itself or into one of its own descendants
    AlreadyThere,             // the destination IS the source's current parent -- a no-op, not an error
    ListingIncomplete,        // listingIsComplete was false -- a PREFIX cannot prove a name is free
    NameTaken,
    SidecarBlocked,       // the sidecar's destination name is occupied -- refused BEFORE any move
    TrashUnavailable,     // no free sequence directory, or it could not be created
    RenameFailed,         // the OS refused step 0. NOTHING HAPPENED.
    SidecarRenameFailed,  // the OS refused step 1. Step 0 WAS ROLLED BACK.
    RollbackFailed,       // step 1 failed AND the rollback failed. The tree is TORN.
};

// The one-word label, for a log line and for a test's failure text. NOT named toString: doctest's
// DOCTEST_STRINGIFY expands to an UNQUALIFIED toString(...), which ADL finds on a public engine
// header, beats doctest's own template with, and then fails to decompose -- a hard compile error on
// every lane, reported inside doctest.h. `audioClipLoadStatusLabel` is the precedent.
[[nodiscard]] std::string_view assetOpRefusalLabel(AssetOpRefusal refusal) noexcept;

// PROMOTED from validateOrphanPath's body, VERBATIM in behaviour: refuses an EMPTY path, ANY
// backslash, a leading '/', a "X:" drive prefix, and ANY ".." SEGMENT (segment-wise, so a file
// legitimately named "..config" is NOT refused). noexcept; pointer + length throughout, never
// substr -- substr may throw and bugprone-exception-escape fires on a noexcept function that can.
//
// Returns AssetOpRefusal::None when the path is safe, and BadSourcePath otherwise; the ONE caller
// that needs a different enumerator maps it (validateOrphanPath -> EscapesRoot, planAssetOp's rung 3
// -> BadDestination).
[[nodiscard]] AssetOpRefusal validateRelativeAssetPath(std::string_view relativePath) noexcept;

// PURE (no disk): the path-shape half of D12's checks, so every branch is a tier-0 case.
// Rejects: a leaf that is not a sidecar name; an EMPTY path; an absolute path ('/' or "X:" prefix);
// ANY path containing a ".." segment; ANY backslash -- a Windows separator must never reach a
// relative key, because 2.2.4's relative paths are '/'-separated on every OS.
[[nodiscard]] OrphanDeleteRefusal validateOrphanPath(std::string_view relativeMetaPath) noexcept;

// THE ACTION. Re-verifies EVERYTHING from disk, in a fixed order, then removes exactly ONE file.
// NEVER throws. NEVER logs (INV-A3's posture extended to this TU): it RETURNS a result. An empty or
// non-absolute `assetsRootUtf8` is refused explicitly, as `EscapesRoot` (code-review finding 5) --
// without this an empty root would resolve "" + "/" + relativeMetaPath to the filesystem ROOT.
[[nodiscard]] OrphanDeleteResult deleteOrphanMeta(std::string_view assetsRootUtf8, std::string_view relativeMetaPath);

}  // namespace engine::editor

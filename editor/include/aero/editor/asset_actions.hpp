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

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine::editor {

enum class OrphanDeleteRefusal : std::uint8_t {
    None = 0,
    NotAMetaName,  // the path does not end in .meta (isMetaFileName on the LEAF)
    EscapesRoot,   // an absolute path, a rooted drive letter, a ".." segment, or a backslash
    Missing,       // the file is gone -- someone else deleted it. Nothing to do; not an error
    NotAMeta,      // it exists but does not parse as a .meta v1 (D12 check 4)
    AssetPresent,  // the asset it names exists again -- it is NOT an orphan any more (E22). Only a
                   // SCANNABLE name counts (task E.4.4): a file the ignore roster covers is never paired
                   // with a sidecar, so its presence does not stop the delete
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

enum class AssetOpKind : std::uint8_t { CreateFolder = 0, Rename, Move, Delete };

// Why a NAME is refused. Every rule is enforced on EVERY OS, unconditionally and with no #if: this
// project is a git repository a teammate clones on Windows, so a name that is legal here and fatal
// there is a defect this editor created. The rules are DATA, never a platform branch.
enum class AssetNameRefusal : std::uint8_t {
    None = 0,
    Empty,               // nothing to name
    TooLong,             // > MAX_ASSET_NAME_BYTES
    HasSeparator,        // '/' or '\\' -- a leaf is a leaf; a separator turns a rename into a move
    DotOrDotDot,         // "." or ".." -- a rename onto the parent or onto self
    Hidden,              // a leading '.' -- isHiddenName would remove it from the scan entirely
    ControlCharacter,    // any byte < 0x20 -- unrepresentable in the UI, hostile in a shell
    ReservedCharacter,   // any of * ? " < > | : -- illegal on Windows
    TrailingDotOrSpace,  // Windows STRIPS these SILENTLY: the file's real name would differ from the
                         // recorded one, and neither would be addressable
    ReservedDeviceName,  // CON/PRN/AUX/NUL/COM1-9/LPT1-9, case-insensitive, WITH or WITHOUT an
                         // extension -- "CON.png" is reserved too
    MetaSuffix,          // isMetaFileName -- would manufacture an orphan sidecar the scan then reports
    IgnoredName,         // isIgnoredAssetName -- the roster in asset_meta.hpp (docs/09 §5.10), which the
                         // scan skips, so a FILE given this name would vanish from the browser. Renamed
                         // IN PLACE from TempSuffix at task E.4.4 (same position, so no value moves) when
                         // the one suffix it tested became a whole roster. A FOLDER name is refused too,
                         // for want of a file/folder context here -- the MetaSuffix posture, not a claim
                         // that such a folder would be hidden (directories never are)
};

// The one-word label. NOT named toString, for assetOpRefusalLabel's reason above.
[[nodiscard]] std::string_view assetNameRefusalLabel(AssetNameRefusal refusal) noexcept;

struct AssetOpResult {
    bool performed = false;
    AssetOpRefusal refusal = AssetOpRefusal::None;
    AssetNameRefusal nameRefusal = AssetNameRefusal::None;  // meaningful iff refusal == BadName
    std::string message;                                    // the exact human text the CALLER logs; "" iff performed
    std::string resultingPath;  // the asset's NEW assets-relative path; "" unless performed, and
                                // ALWAYS "" for Delete -- the file is no longer in the assets tree,
                                // so there is nothing for requestSelectEntry to select
    // TRUE only for RollbackFailed. The caller WARNs BOTH paths and says the next scan will attempt
    // re-attachment (3.1.2 phase 5, asset_database.cpp:493; the call is at :540). Nothing else in the
    // editor reads it.
    bool torn = false;
};

// Two bases exist because Delete's destination is under the PROJECT root while every other
// destination is under the ASSETS root. Conflating them is exactly how a path lands in the wrong
// tree, and a single "relative" string gives the executor no way to tell which root to prepend.
enum class AssetPathBase : std::uint8_t { AssetsRoot = 0, ProjectRoot };

struct AssetOpPath {
    AssetPathBase base = AssetPathBase::AssetsRoot;
    std::string relative;  // '/'-separated on every OS; never empty in a POPULATED step
};

struct AssetOpStep {
    AssetOpPath from;
    AssetOpPath to;
};

struct AssetOpPlan {
    AssetOpRefusal refusal = AssetOpRefusal::None;
    AssetNameRefusal nameRefusal = AssetNameRefusal::None;
    std::string message;
    // steps[0] == the ASSET or the FOLDER. steps[1] == the SIDECAR.
    // stepCount is 1 for a folder (it HAS no sidecar) and 2 for a file. A CreateFolder plan has
    // stepCount 0 and carries its target in `directoryToCreate` instead.
    std::array<AssetOpStep, 2> steps{};
    std::size_t stepCount = 0;
    // Non-empty ONLY for CreateFolder and Delete: the directory the executor must ensureDirectory()
    // BEFORE step 0. For Delete it is the trash sequence directory plus the PRESERVED parent path,
    // so Trash/0007/textures/ exists before wood.png is renamed into it.
    AssetOpPath directoryToCreate{};
    // Assets-relative. "" for Delete (the file left the assets tree) and for a refused plan;
    // CreateFolder sets it to the NEW FOLDER's own path, not its parent's.
    std::string resultingRelativePath;
};

// The common single-component limit on every filesystem this project targets (HFS+/APFS 255 UTF-8
// bytes, NTFS 255 UTF-16 units, ext4 255 bytes). Bytes, never characters: a UTF-8 name of 200
// codepoints can exceed it, and the byte count is what the OS enforces.
inline constexpr std::size_t MAX_ASSET_NAME_BYTES = 255;

// PURE, noexcept. The WHOLE of the name table above, in ONE place, so no call site can enforce half
// of it. Rules are checked in enumerator order, so the refusal a caller sees is the FIRST rule the
// name broke -- deterministic, and what the AA battery asserts.
[[nodiscard]] AssetNameRefusal validateAssetName(std::string_view leaf) noexcept;

struct AssetOpInputs {
    std::string_view sourceRelative;          // assets-relative. "" is SourceIsRoot for every kind but
                                              // CreateFolder, where it is the LEGAL parent directory.
    bool sourceIsDirectory = false;           // from the FileEntry / the tree row -- NEVER derived
                                              // from the path, which cannot tell a folder from an
                                              // extension-less file
    std::string_view destinationDirRelative;  // Move only. "" == the assets root, and is LEGAL.
    std::string_view newLeaf;                 // Rename and CreateFolder only.
    std::uint32_t trashSequence = 0;          // Delete only; probed by the CALLER
};

// The shared path ladder: rungs 1, 2, 3, 5 and 6 of the full ladder -- everything decidable from two
// paths and a kind, with NO listing and NO disk. Rung 4 (BadName) is excluded because a peek has no
// leaf to validate. PURE, noexcept.
//
// classifyAssetMove IS a call to this with kind == Move, and planAssetOp's FIRST statement IS a call
// to this. That is what makes the peek a PREFIX of the ladder rather than a second copy of it: the
// two cannot disagree about DestinationInsideSource, which is the refusal that matters most and the
// one a hand-written second copy would get subtly wrong.
[[nodiscard]] AssetOpRefusal assetOpPathLadder(AssetOpKind kind, std::string_view sourceRelative,
                                               std::string_view destinationDirRelative) noexcept;

// The PEEK answer for a drag target: may this source be dropped on this folder? It IS a call to
// assetOpPathLadder with kind == Move -- a PREFIX of the full ladder, never a second copy of it.
// Returns None when the drop is legal SO FAR; the listing and disk refusals still run at drop time.
[[nodiscard]] AssetOpRefusal classifyAssetMove(std::string_view sourceRelative,
                                               std::string_view destinationDirRelative) noexcept;

// PURE. `destinationListing` is the listing of the directory the new name will live in:
//   Rename       -> the SOURCE's parent
//   Move         -> destinationDirRelative
//   CreateFolder -> sourceRelative (the parent the folder is created in)
//   Delete       -> an EMPTY Ok listing: the trash sequence directory is fresh by construction, so
//                   there is nothing to collide with and nothing to read. Rung 8 is SKIPPED for
//                   Delete for exactly that reason, and the caller passes DirectoryListing{} rather
//                   than paying a listDirectory on a directory it is about to create.
[[nodiscard]] AssetOpPlan planAssetOp(AssetOpKind kind, const AssetOpInputs& inputs,
                                      const DirectoryListing& destinationListing);

// ---- the project trash --------------------------------------------------------------------------

inline constexpr std::string_view ASSET_TRASH_DIR_NAME = "Trash";  // under ASSET_CACHE_DIR_NAME
inline constexpr std::uint32_t MAX_TRASH_SEQUENCE = 9999;          // four digits; bounded, surfaced

// PURE: (7, "textures/wood.png") -> "Library/Trash/0007/textures/wood.png", PROJECT-relative.
// The preserved path is the ASSETS-relative one, deliberately: everything the browser shows is
// assets-relative, so the trash reads back in the vocabulary the user was looking at.
// The sequence is zero-padded to FOUR digits with no locale and no <iomanip>; a sequence above
// MAX_TRASH_SEQUENCE is never produced (allocateTrashSequence refuses first).
[[nodiscard]] std::string trashRelativePathFor(std::uint32_t sequence, std::string_view assetsRelativePath);

// The first sequence whose directory does not exist, starting at 1. Uses fileExists ONLY -- it
// CREATES nothing, REMOVES nothing, and matches neither FORBIDDEN_RE nor DELETE_RE. nullopt when
// MAX_TRASH_SEQUENCE is exhausted, which the caller reports as TrashUnavailable rather than reusing
// a directory: reuse would put two deletes of the same path in one folder, where the second collides
// with the first.
[[nodiscard]] std::optional<std::uint32_t> allocateTrashSequence(std::string_view projectRootUtf8);

// THE ONE PLACE D3 AND D4 ARE IMPLEMENTED. Two renames, a rollback, and a fixed re-verification
// order. NEVER THROWS -- every std::filesystem call uses the std::error_code overload, exactly as
// deleteOrphanMeta already does. NEVER LOGS (INV-A3's posture, extended): it RETURNS a result.
[[nodiscard]] AssetOpResult executeAssetOpPlan(const AssetOpPlan& plan, std::string_view projectRootUtf8,
                                               std::string_view assetsRootUtf8);

// ---- the blast-radius count and the modals' sentences --------------------------------------------

// PURE, noexcept. AssetDatabase::records() is already sorted byte-lexicographically by relativePath
// (asset_database.hpp:145 states it), so this is a std::lower_bound on `folderRelative + '/'` plus a
// walk while the prefix holds. ZERO I/O -- which is what lets the delete modal state it from inside
// phase 4b, where I/O is forbidden (asset_browser_panel.hpp:8-16).
//
// Counts INDEXED ASSETS ONLY: never sidecars (they are not records), never ignored or hidden files.
// The modal's sentence says "indexed assets" rather than "files" because that is exactly what this
// number counts, and a sentence implying a total would be wrong the first time a .txt sat in the
// folder. A prefix match is SEGMENT-WISE: "tex" must not count "textures/a.png".
[[nodiscard]] std::size_t countRecordsUnder(std::span<const AssetRecord> records,
                                            std::string_view folderRelative) noexcept;

// The delete modal's three sentences, as VALUES. E.3.4's material_inspector_model precedent: PURE --
// no ImGui, no GPU, no <filesystem>, no logging -- so tier 0 can assert the exact text, which the
// ImGui tier cannot (TextWrapped submits no item a test can read back).
struct AssetDeletePrompt {
    std::string title;   // Delete folder "textures"?  /  Delete "textures/wood.png"?
    std::string detail;  // the blast-radius clause    /  the sidecar clause
    std::string footer;  // the "not erased" clause
};

// `indexedAssetCount` is countRecordsUnder's answer and is IGNORED when `isDirectory` is false.
// Plural agreement is arithmetic and belongs where it can be tested: 1 -> "1 indexed asset",
// anything else -> "<n> indexed assets", including 0.
[[nodiscard]] AssetDeletePrompt assetDeletePromptFor(std::string_view relativePath, bool isDirectory,
                                                     std::size_t indexedAssetCount);

// The one-sentence reason a typed name was refused, for the rename modal's inline error line.
// None returns "" -- the modal draws no error line at all when the name is legal.
[[nodiscard]] std::string assetNameRefusalMessage(AssetNameRefusal refusal);

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

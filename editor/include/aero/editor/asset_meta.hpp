#pragma once
// Aero Engine — the .meta v1 format and the pure asset-identity lifecycle planner (task 3.1.1).
// PUBLIC, and the project.hpp shape verbatim: free of ImGui, SDL, entt and <filesystem>; free of
// every build gate (D4/AC-17/INV-P5, project.hpp's precedent, a second application). NOTHING HERE
// LOGS (INV-A3) -- status is RETURNED, never printed (project_files.hpp:15-16's convention, a
// fourth application).
//
// planAssetMetas is the single most important function in this task. It touches no disk, no clock
// and no global state, and every lifecycle rule (D5-D9: create on discovery, never rewrite a valid
// sidecar, never overwrite an invalid one, never delete an orphan, repair a duplicate
// deterministically) is provable from a std::vector literal and a fixed seed.
#include <aero/core/guid.hpp>
#include <aero/editor/asset_cache.hpp>      // task 3.1.2: ImportChange, ContentHash. ONE WAY ONLY -- asset_cache.hpp
                                            // must NEVER include this file (docs/09 §6.9 / plan A20).
#include <aero/editor/import_settings.hpp>  // task 3.2.1 -- ImportSettings + the two importer-identity
                                            // constants ONLY. This header deliberately does NOT
                                            // include model_import.hpp, which would drag aero::scene
                                            // and the math umbrella onto every TU that touches an
                                            // asset record (plan §A-11).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

inline constexpr int ASSET_META_FORMAT_VERSION = 1;
inline constexpr std::string_view ASSET_META_SUFFIX = ".meta";
inline constexpr std::string_view ATOMIC_TEMP_SUFFIX = ".aero-tmp";  // D16
inline constexpr std::size_t MAX_ASSETS = 50000;                     // D14
inline constexpr std::size_t MAX_REPORTED_PER_CATEGORY = 20;
inline constexpr std::size_t CREATE_NOTICE_THRESHOLD = 1000;

// ---- naming --------------------------------------------------------------------------------------

// "wood.png" -> "wood.png.meta". Appends to the FULL file name, never replaces an extension -- so
// "wood.png" and "wood.jpg" never collide on their sidecar name (AC-18).
[[nodiscard]] std::string metaFileNameFor(std::string_view assetFileName);

// True iff `fileName` is longer than the suffix and its last 5 bytes ASCII-fold to ".meta" (AC-19).
// Case-INSENSITIVE on the suffix only -- ".meta" alone (5 bytes, not > 5) is NOT a sidecar name; the
// base-name half of a pairing is compared byte for byte elsewhere (asset_database.cpp).
[[nodiscard]] bool isMetaFileName(std::string_view fileName) noexcept;

// "wood.png.meta" -> "wood.png"; "" for a name that is not a sidecar (AC-20). POINTS INTO `metaName`
// -- never call it on a temporary whose lifetime ends before the result is used.
[[nodiscard]] std::string_view assetNameForMeta(std::string_view metaName) noexcept;

// D5/AC-21: is `fileName` a file the scan should mint an identity for? Rejects a hidden name, a
// sidecar itself, anything ending ATOMIC_TEMP_SUFFIX (a suffix test, not equality -- E19), and the
// two literal OS-noise names `Thumbs.db` / `desktop.ini` (A15's "two literal names plus the
// .aero-tmp suffix"; `.DS_Store` needs no entry, it is dot-prefixed and isHiddenName already removes
// it). Directories are never scannable asset names either -- the caller filters by isDirectory first.
[[nodiscard]] bool isScannableAssetName(std::string_view fileName) noexcept;

// task 3.1.4 (D4): "a name the SCAN can see" -- EXACTLY isScannableAssetName(name) ||
// isMetaFileName(name), and defined HERE, beside the two predicates it composes, so a future change
// to either flows into the watcher automatically instead of being remembered. The watcher's visible
// set MUST equal the scan's, or a file the watcher sees and the scan ignores triggers a rescan that
// changes nothing, then triggers again next sweep, forever -- `.DS_Store` alone would do it on every
// macOS machine.
[[nodiscard]] bool isWatchableAssetName(std::string_view fileName) noexcept;

// ---- the format -----------------------------------------------------------------------------------

enum class MetaError : std::uint8_t {
    None = 0,
    BadJson,
    NotAnObject,
    BadVersion,
    UnsupportedVersion,
    MissingGuid,
    BadGuidKind,
    BadGuidText,
    NilGuid,
};

// task 3.2.1 (D6). docs/09 §5.9. The OPTIONAL `importer` block: user intent, committed to git,
// surviving a machine change. THE FORMAT VERSION STAYS 1, and that is load-bearing:
//
//   A v2 bump is a DATA-LOSS TRAP in this codebase. parseMeta rejects version != 1 with "unsupported
//   asset meta format version <N>", which makes the record AssetMetaState::Invalid with a NIL GUID --
//   and D7 then forbids overwriting an invalid sidecar, correctly and permanently. An older build (a
//   teammate who has not pulled; the user's own previous install; a bisect) opening a v2 project marks
//   EVERY asset invalid, with no identity, and cannot repair a single one. There is no recovery path
//   that does not involve hand-editing files. An ADDITIVE OPTIONAL KEY at version 1 degrades instead:
//   §5.1's unknown-root-key rule means an older build reads the GUID correctly, warns once per model
//   asset, and loses only the settings -- which it could not have honoured anyway.
struct MetaImporterBlock {
    std::string name;           // "gltf" or "fbx" (task 3.2.2) -- whichever importer wrote this sidecar
    std::uint32_t version = 0;  // that importer's OWN version constant, at write time
    ImportSettings settings;
};

struct MetaParseResult {
    std::optional<Guid> guid;  // engaged == success
    MetaError error = MetaError::None;
    std::string message;     // the exact docs/09 §5.4 text; "" iff `guid` is engaged
    std::uint32_t line = 0;  // > 0 ONLY for a JSON-stage failure
    std::uint32_t column = 0;
    std::vector<std::string> unknownKeys;  // AC-14: WARNed by the CALLER, never here
    // ---- task 3.2.1, APPENDED (3.1.2's A2 trap: APPENDED, NEVER INSERTED -- asset_meta_test.cpp holds
    // positional aggregate initializers, and `bool -> uint32_t` is a PROMOTION that nothing
    // diagnoses) ----
    //
    // ENGAGEMENT is the signal: engaged == a well-formed `importer` block was present.
    // Disengaged == the block was ABSENT **or** MALFORMED.
    std::optional<MetaImporterBlock> importer;
    // "" iff the block was absent OR parsed cleanly. NON-EMPTY + DISENGAGED == malformed, and THAT
    // pair is how a caller distinguishes "absent" from "broken" -- NEVER by inspecting `unknownKeys`.
    std::string importerMessage;
};

// AC-12/INV-M11, and it is THE load-bearing rule of this extension: a malformed importer block NEVER
// invalidates an identity. `error` stays MetaError::None, `guid` stays engaged, and the failure
// surfaces ONLY through the two fields above. This asymmetry with every other field in the format is
// DELIBERATE: `version` and `guid` are IDENTITY, and a failure there is fatal; the importer block is
// PREFERENCE, and a failure there costs a user their settings, not their asset. NO NEW MetaError
// ENUMERATOR IS ADDED, for exactly that reason.

[[nodiscard]] MetaParseResult parseMeta(std::string_view text);
// D7's omit-when-default rule lives HERE, in exactly ONE place.
//
// task 3.2.2: `importerName`/`importerVersion` are NEW, TRAILING, DEFAULTED parameters -- a THIRD
// hard-coded-identity site the plan did not name (beside asset_database.cpp's phase 7.5 probe, §A-1,
// and import_details_panel.cpp's Overview line, §A-2): this function used to write GLTF_IMPORTER_NAME/
// GLTF_IMPORTER_VERSION unconditionally, so ModelImportSession::applySettings() would have written
// "name": "gltf" into an .fbx's own sidecar. The default keeps every EXISTING two-argument call site
// (this file's own tests, chiefly) compiling and behaving byte-identically; model_import_session.cpp is
// the ONE call site that now passes the file's REAL identity, via modelImporterIdentity(). Declared as
// two primitives, NOT `ImporterIdentity`, because THIS header deliberately does not include
// model_import.hpp (the comment two lines below states why, and asset_cache.hpp/asset_meta.hpp must
// never gain that include -- plan §A-11).
[[nodiscard]] std::string writeMetaText(Guid guid, const ImportSettings& settings,
                                        std::string_view importerName = GLTF_IMPORTER_NAME,
                                        std::uint32_t importerVersion = GLTF_IMPORTER_VERSION);
// The existing one-argument overload becomes a ONE-LINE DELEGATE to the above with ImportSettings{},
// so AC-9's byte-identity is true BY CONSTRUCTION rather than by two writers agreeing (INV-M10). The
// identity parameters are UNREACHABLE here: D7's omit-when-default branch never runs for
// ImportSettings{}, so which pair the delegate forwards is never observed.
[[nodiscard]] std::string writeMetaText(Guid guid);  // canonical, exactly one trailing '\n'

// ---- the lifecycle, as a PURE function (D5-D9) ------------------------------------------------

// `Reattached` is APPENDED, never inserted (task 3.1.2, D13): AssetMetaState is serialized nowhere
// (in-memory scan state only), so appending is safe; inserting would silently renumber `Invalid`,
// which several tests compare by value.
enum class AssetMetaState : std::uint8_t { Ok = 0, Created, Repaired, Invalid, Reattached };

struct AssetRecord {
    std::string relativePath;  // '/'-separated, relative to the assets root
    Guid guid;                 // nil iff state == Invalid
    AssetMetaState state = AssetMetaState::Ok;
    // ---- task 3.1.2, additive ----
    ContentHash contentHash;  // MEANINGLESS unless `change` is neither Unhashable nor NotHashed --
                              // all-zero is the EMPTY FILE's real digest, not a sentinel (plan A4).
    ImportChange change = ImportChange::UpToDate;  // filled by AssetDatabase, not by planAssetMetas
    // Code-review finding 3: true iff phase 7's writeTextFileAtomic for THIS record's sidecar failed
    // this scan (state still Created/Repaired/Reattached -- the write failure does not change it, D7's
    // "kept its in-memory GUID" posture). Phase 8 never assigns such a record a `change` at all (its
    // bytes on disk are not what would have been hashed), so `change` stays its default UpToDate --
    // every READER of `change` (the accessor, the footer) must check this flag first, or a failed write
    // silently reads as "up to date" for a file with no sidecar and no cache entry.
    bool metaWriteFailed = false;
    // ---- task 3.2.1, APPENDED ----
    // The asset's own import settings, parsed from its .meta's optional `importer` block in phase 2.
    // DEFAULTS when the block is absent OR malformed (D7 rule 1: absent == defaults, forever, with no
    // "unset" state, no tri-state and no migration). Phase 7.5 reads it so a probe uses the SAME
    // settings the panel would, and it is what makes AC-52 work: changing a setting changes metaHash
    // -> MetaChanged -> the asset lands in jobIndices -> phase 7.5 re-probes it with the NEW settings.
    ImportSettings importSettings;
};

// TRUE iff this scan actually hashed this record's bytes, so `contentHash` means something. Stated
// here, beside the field whose own comment states the rule, because every consumer needs it and each
// one that re-derives it gets one more chance to forget: an unhashed record keeps an ALL-ZERO digest,
// which is the EMPTY FILE's real value and never a sentinel, so "is it zero?" is not the question the
// caller wants asked. A record is unhashed when the per-scan budget ran out before reaching it
// (NotHashed), when its bytes could not be read at all (Unhashable), or when its sidecar write failed
// this scan (metaWriteFailed, whose records phase 8 never assigns a `change` to at all -- so a failed
// write reads as the default UpToDate unless this flag is checked FIRST).
//
// 3.1.3's ThumbnailKey rule is the shape every consumer follows: a record with no usable hash has no
// cache key, so it is not keyed, not cached and not compared -- never keyed on zeros.
[[nodiscard]] bool assetContentHashUsable(const AssetRecord& record) noexcept;

struct AssetPlanEntry {
    std::string relativePath;
    std::optional<Guid> guid;  // nullopt == no sidecar, OR one that failed to parse
    bool metaPresent = false;
    std::optional<Guid> reattachedGuid;  // task 3.1.2 (D13). Set ONLY by asset_database.cpp's phase 5.
    // task 3.2.1, APPENDED: the SAME field as AssetRecord::importSettings above, set by
    // asset_database.cpp's phase 2 from the sidecar's parsed importer block (when engaged) and left at
    // its default otherwise. planAssetMetas copies it straight across into the record it builds --
    // reattachedGuid's shape, a second application.
    ImportSettings importSettings;
};

struct AssetPlanResult {
    std::vector<AssetRecord> records;       // sorted byte-lexicographically by relativePath
    std::vector<std::size_t> writeIndices;  // indices into `records` whose .meta must be WRITTEN
    std::size_t created = 0;
    std::size_t repaired = 0;
    std::size_t invalid = 0;
    std::size_t reattached = 0;  // task 3.1.2
};

// THE WHOLE OF D5-D9, as a pure function of (entries, seed). Sorts first (AC-22), so the result is
// independent of the walk order, of the OS, and of entryOrderLess's case folding.
[[nodiscard]] AssetPlanResult planAssetMetas(std::vector<AssetPlanEntry> entries, GuidGenerator& generator);

}  // namespace engine::editor

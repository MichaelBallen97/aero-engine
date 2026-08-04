// Aero Engine — the asset import cache index v1 format's pure half (task 3.1.2). asset_meta.cpp's
// structure, one level deeper: an array of objects rather than a flat document. Uses
// engine::parseJson / engine::JsonValue / engine::JsonWriter (F6) -- the scene_format.cpp /
// project.cpp / asset_meta.cpp precedent, applied a fourth time. No <filesystem>, no SDL, no ImGui,
// no logging (INV-A3): status is RETURNED, never printed. No recursion anywhere -- the document is
// two levels deep and both are walked with plain loops.
//
// Step 4 landed parseAssetCache / writeAssetCacheText / importChangeLabel / AssetCacheIndex::find.
// Step 6 added planImports and commitImports, the pure change-detection cascade and its commit.
// Step 7 (this revision) adds planReattachments, D13's orphan re-attachment.
#include <aero/editor/asset_cache.hpp>
#include <aero/reflect/json_reader.hpp>
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

constexpr std::string_view VERSION_KEY = "version";
constexpr std::string_view HASH_ALGORITHM_KEY = "hashAlgorithm";
constexpr std::string_view ENTRIES_KEY = "entries";
constexpr std::string_view GUID_KEY = "guid";
constexpr std::string_view PATH_KEY = "path";
constexpr std::string_view SIZE_KEY = "size";
constexpr std::string_view MTIME_KEY = "mtime";
constexpr std::string_view CONTENT_HASH_KEY = "contentHash";
constexpr std::string_view META_HASH_KEY = "metaHash";
constexpr std::string_view IMPORTER_KEY = "importer";
constexpr std::string_view IMPORTER_VERSION_KEY = "importerVersion";
constexpr std::string_view DEPENDENCIES_KEY = "dependencies";
constexpr std::string_view MISSING_KEY = "missing";

// "(found <kind>)" -- or, when a Number failed a FORM rule rather than a KIND rule, the quoted
// lexeme instead: `(found "1.5")`. Copied from asset_meta.cpp:48-53 / project.cpp:49-54 /
// scene_format.cpp:43-49 (docs/09's error-catalog shape; the fourth copy -- scaffolding is copied,
// the assertion is shared).
std::string foundDetail(const JsonValue& v) {
    if (v.isNumber()) {
        return std::format("\"{}\"", v.numberLexeme());
    }
    return std::string(jsonKindName(v.kind()));
}

AssetCacheParseResult discarded(std::string reason) {
    AssetCacheParseResult result;
    result.outcome = CacheLoadOutcome::Discarded;
    result.discardReason = std::move(reason);
    return result;
}

}  // namespace

const AssetCacheEntry* AssetCacheIndex::find(Guid guid) const noexcept {
    const auto it = std::lower_bound(entries.begin(), entries.end(), guid,
                                     [](const AssetCacheEntry& entry, Guid target) { return entry.guid < target; });
    if (it == entries.end() || it->guid != guid) {
        return nullptr;  // also the answer for a nil GUID: no entry is EVER nil (INV-C1)
    }
    return &*it;
}

// parseAssetCache's order is load-bearing: it is what makes AC-14's "version-first" property and
// every "exact first error" assertion true. Written in EXACTLY this order; do not reorder for style.
AssetCacheParseResult parseAssetCache(std::string_view text) {
    const JsonParseResult parsed = parseJson(text);
    // NOT `!parsed.ok()`: bugprone-unchecked-optional-access cannot connect an opaque out-of-line
    // ok() to `value` (project.cpp:151-155's precedent, asset_meta.cpp's second application).
    if (!parsed.value.has_value()) {
        return discarded(parsed.error.message);
    }
    const JsonValue& root = *parsed.value;
    if (!root.isObject()) {
        return discarded(std::format("asset cache root must be a JSON object (found {})", jsonKindName(root.kind())));
    }

    // "version" FIRST -- a document missing ALL THREE root keys must still report the version error.
    const JsonValue* version = root.find(VERSION_KEY);
    if (version == nullptr) {
        return discarded("missing required key \"version\"");
    }
    const std::optional<std::uint64_t> versionValue = version->asU64();
    if (!versionValue.has_value()) {
        return discarded(std::format("\"version\" must be an integer (found {})", foundDetail(*version)));
    }
    if (*versionValue != static_cast<std::uint64_t>(ASSET_CACHE_FORMAT_VERSION)) {
        return discarded(std::format("unsupported asset cache format version {} (this build reads version {})",
                                     *versionValue, ASSET_CACHE_FORMAT_VERSION));
    }

    // then "hashAlgorithm"
    const JsonValue* hashAlgorithm = root.find(HASH_ALGORITHM_KEY);
    if (hashAlgorithm == nullptr) {
        return discarded("missing required key \"hashAlgorithm\"");
    }
    const std::optional<std::string_view> hashAlgorithmValue = hashAlgorithm->asString();
    if (!hashAlgorithmValue.has_value()) {
        return discarded(
            std::format("\"hashAlgorithm\" must be a string (found {})", jsonKindName(hashAlgorithm->kind())));
    }
    if (*hashAlgorithmValue != CONTENT_HASH_ALGORITHM) {
        return discarded(std::format(R"(unsupported hash algorithm "{}" (this build writes "{}"))", *hashAlgorithmValue,
                                     CONTENT_HASH_ALGORITHM));
    }

    // then "entries"
    const JsonValue* entriesField = root.find(ENTRIES_KEY);
    if (entriesField == nullptr) {
        return discarded("missing required key \"entries\"");
    }
    if (!entriesField->isArray()) {
        return discarded(std::format("\"entries\" must be an array (found {})", jsonKindName(entriesField->kind())));
    }

    AssetCacheParseResult result;
    // Duplicate detection (AC-16): keys in only once an element has survived EVERY other check --
    // so an earlier occurrence that was itself malformed (a missing path, say) never "claims" a GUID
    // it was never actually going to keep.
    std::unordered_set<Guid> claimed;
    for (const JsonValue& element : entriesField->elements()) {
        if (result.index.entries.size() >= MAX_CACHE_ENTRIES) {
            result.truncated = true;
            break;
        }
        if (!element.isObject()) {
            ++result.droppedEntries;
            continue;
        }

        // Every optional below is unwrapped into a PLAIN local IMMEDIATELY after its own check, never
        // carried across the several unrelated checks that follow -- bugprone-unchecked-optional-
        // access cannot connect a check to a dereference several statements later, even when the
        // intervening code never touches the optional at all (measured: clang-tidy flagged exactly
        // the six fields that used to be dereferenced only once, at push time, at the very end).

        const JsonValue* guidField = element.find(GUID_KEY);
        const std::optional<std::string_view> guidText = guidField != nullptr ? guidField->asString() : std::nullopt;
        if (!guidText.has_value()) {
            ++result.droppedEntries;
            continue;
        }
        const std::optional<Guid> guidOpt = parseGuid(*guidText);
        if (!guidOpt.has_value()) {
            ++result.droppedEntries;
            continue;
        }
        // `guid` ALONE may not be nil (AC-16/plan A4) -- contentHash/metaHash below explicitly MAY be:
        // MurmurHash3 x64_128 of the empty input is all-zero, a legitimate value, never this format's.
        if (!guidOpt->valid()) {
            ++result.droppedEntries;
            continue;
        }
        const Guid guid = *guidOpt;

        const JsonValue* pathField = element.find(PATH_KEY);
        const std::optional<std::string_view> pathTextOpt = pathField != nullptr ? pathField->asString() : std::nullopt;
        if (!pathTextOpt.has_value()) {
            ++result.droppedEntries;
            continue;
        }
        const std::string path(*pathTextOpt);

        const JsonValue* sizeField = element.find(SIZE_KEY);
        const std::optional<std::uint64_t> sizeOpt = sizeField != nullptr ? sizeField->asU64() : std::nullopt;
        if (!sizeOpt.has_value()) {
            ++result.droppedEntries;
            continue;
        }
        const std::uint64_t size = *sizeOpt;

        const JsonValue* mtimeField = element.find(MTIME_KEY);
        const std::optional<std::int64_t> mtimeOpt = mtimeField != nullptr ? mtimeField->asI64() : std::nullopt;
        if (!mtimeOpt.has_value()) {
            ++result.droppedEntries;
            continue;
        }
        const std::int64_t mtime = *mtimeOpt;

        const JsonValue* contentHashField = element.find(CONTENT_HASH_KEY);
        const std::optional<std::string_view> contentHashText =
            contentHashField != nullptr ? contentHashField->asString() : std::nullopt;
        if (!contentHashText.has_value()) {
            ++result.droppedEntries;
            continue;
        }
        const std::optional<ContentHash> contentHashOpt = parseContentHash(*contentHashText);
        // NIL IS ACCEPTED here (plan A4): only a malformed or absent field drops the entry.
        if (!contentHashOpt.has_value()) {
            ++result.droppedEntries;
            continue;
        }
        const ContentHash contentHash = *contentHashOpt;

        const JsonValue* metaHashField = element.find(META_HASH_KEY);
        const std::optional<std::string_view> metaHashText =
            metaHashField != nullptr ? metaHashField->asString() : std::nullopt;
        if (!metaHashText.has_value()) {
            ++result.droppedEntries;
            continue;
        }
        const std::optional<ContentHash> metaHashOpt = parseContentHash(*metaHashText);
        if (!metaHashOpt.has_value()) {
            ++result.droppedEntries;
            continue;
        }
        const ContentHash metaHash = *metaHashOpt;

        // ---- optional keys, each defaulted rather than dropping the entry when ABSENT -------------
        std::string importer;
        if (const JsonValue* importerField = element.find(IMPORTER_KEY); importerField != nullptr) {
            const std::optional<std::string_view> importerText = importerField->asString();
            if (!importerText.has_value()) {
                ++result.droppedEntries;
                continue;
            }
            importer = std::string(*importerText);
        }

        std::uint32_t importerVersion = 0;
        if (const JsonValue* importerVersionField = element.find(IMPORTER_VERSION_KEY);
            importerVersionField != nullptr) {
            const std::optional<std::uint64_t> importerVersionValue = importerVersionField->asU64();
            if (!importerVersionValue.has_value()) {
                ++result.droppedEntries;
                continue;
            }
            importerVersion = static_cast<std::uint32_t>(*importerVersionValue);
        }

        std::vector<Guid> dependencies;
        bool dependenciesMalformed = false;
        if (const JsonValue* dependenciesField = element.find(DEPENDENCIES_KEY); dependenciesField != nullptr) {
            if (!dependenciesField->isArray()) {
                dependenciesMalformed = true;
            } else {
                for (const JsonValue& depElement : dependenciesField->elements()) {
                    const std::optional<std::string_view> depText = depElement.asString();
                    const std::optional<Guid> dep = depText.has_value() ? parseGuid(*depText) : std::nullopt;
                    if (!dep.has_value()) {
                        dependenciesMalformed = true;
                        break;
                    }
                    if (dependencies.size() >= MAX_DEPENDENCIES_PER_ENTRY) {
                        ++result.droppedDependencies;  // E25: excess is dropped, the ENTRY survives
                        continue;
                    }
                    dependencies.push_back(*dep);
                }
            }
        }
        if (dependenciesMalformed) {
            ++result.droppedEntries;
            continue;
        }

        std::uint32_t missing = 0;
        if (const JsonValue* missingField = element.find(MISSING_KEY); missingField != nullptr) {
            const std::optional<std::uint64_t> missingValue = missingField->asU64();
            if (!missingValue.has_value()) {
                ++result.droppedEntries;
                continue;
            }
            missing = static_cast<std::uint32_t>(*missingValue);
        }

        // Every field validated -- NOW decide the duplicate question (AC-16): the first entry to
        // reach this point for a given GUID keeps it; a later one is dropped.
        if (!claimed.insert(guid).second) {
            ++result.droppedEntries;
            continue;
        }

        AssetCacheEntry entry;
        entry.guid = guid;
        entry.path = path;
        entry.size = size;
        entry.mtime = mtime;
        entry.contentHash = contentHash;
        entry.metaHash = metaHash;
        entry.importer = std::move(importer);
        entry.importerVersion = importerVersion;
        entry.dependencies = std::move(dependencies);
        entry.missing = missing;
        result.index.entries.push_back(std::move(entry));
    }

    // Unknown keys at BOTH levels are ignored SILENTLY (D7) -- no WARN, no collection, no report
    // field touched. This is the ONE place the cache's policy visibly inverts .meta's: a newer build
    // wrote them and the user cannot act on them.

    std::sort(result.index.entries.begin(), result.index.entries.end(),
              [](const AssetCacheEntry& a, const AssetCacheEntry& b) { return a.guid < b.guid; });
    return result;
}

std::string writeAssetCacheText(const AssetCacheIndex& index) {
    std::vector<AssetCacheEntry> sorted = index.entries;  // AC-18: sorted regardless of input order
    std::sort(sorted.begin(), sorted.end(),
              [](const AssetCacheEntry& a, const AssetCacheEntry& b) { return a.guid < b.guid; });

    JsonWriter writer;  // the DEFAULT config: pretty, 2-space -- docs/09's canonical form. Do NOT
                        // spell the config out; a second spelling is a second truth.
    writer.beginObject();
    writer.key("version");
    writer.value(static_cast<long long>(ASSET_CACHE_FORMAT_VERSION));
    writer.key("hashAlgorithm");
    writer.value(CONTENT_HASH_ALGORITHM);
    writer.key("entries");
    writer.beginArray();
    for (const AssetCacheEntry& entry : sorted) {
        writer.beginObject();
        // Named local FIRST, always (the 2.6.1 FileDialogHost::projectRoot lesson, asset_meta.cpp's
        // precedent): formatGuid/formatContentHash return BY VALUE.
        writer.key("guid");
        const std::string guidText = formatGuid(entry.guid);
        writer.value(std::string_view(guidText));
        writer.key("path");
        writer.value(std::string_view(entry.path));
        writer.key("size");
        writer.value(static_cast<unsigned long long>(entry.size));
        writer.key("mtime");
        // std::to_chars, locale-independent and exact past 2^53 (F1) -- AC-19's whole point.
        writer.value(static_cast<long long>(entry.mtime));
        writer.key("contentHash");
        const std::string contentHashText = formatContentHash(entry.contentHash);
        writer.value(std::string_view(contentHashText));
        writer.key("metaHash");
        const std::string metaHashText = formatContentHash(entry.metaHash);
        writer.value(std::string_view(metaHashText));
        // Optional keys are ALWAYS written, never omitted when defaulted (D-6) -- so the fixpoint is
        // unconditional and a reader never has to distinguish absent from default.
        writer.key("importer");
        writer.value(std::string_view(entry.importer));
        writer.key("importerVersion");
        writer.value(static_cast<unsigned long long>(entry.importerVersion));
        writer.key("dependencies");
        writer.beginArray();
        for (const Guid& dependency : entry.dependencies) {
            const std::string dependencyText = formatGuid(dependency);
            writer.value(std::string_view(dependencyText));
        }
        writer.endArray();
        writer.key("missing");
        writer.value(static_cast<unsigned long long>(entry.missing));
        writer.endObject();
    }
    writer.endArray();
    writer.endObject();
    std::string text = writer.str();
    text += '\n';  // exactly ONE trailing newline (the writer itself has none; parseJson accepts it)
    return text;
}

std::string_view importChangeLabel(ImportChange change) noexcept {
    // Enumerated so a new arm cannot be silent (A13) -- the logAssetScan ScanStatus switch is the
    // precedent (editor_app.cpp:100-112): assign inside the switch, return after it, so a `switch`
    // with NO `default:` makes a future enumerator a -Wswitch warning rather than a wrong string.
    std::string_view label = "unknown";  // unreachable today: every ImportChange arm is handled below
    switch (change) {
        case ImportChange::UpToDate:
            label = "up to date";
            break;
        case ImportChange::New:
            label = "new";
            break;
        case ImportChange::SourceChanged:
            label = "changed";
            break;
        case ImportChange::MetaChanged:
            label = "settings changed";
            break;
        case ImportChange::ImporterChanged:
            label = "importer changed";
            break;
        case ImportChange::DependencyChanged:
            label = "dependency changed";
            break;
        case ImportChange::Unhashable:
            label = "unreadable";
            break;
        case ImportChange::NotHashed:
            label = "not hashed";
            break;
    }
    return label;
}

ImportPlanResult planImports(std::vector<ImportInput> inputs, const AssetCacheIndex& previous) {
    // 1. Sort byte-lexicographically FIRST (AC-21, seed S11): std::string's `<` is a byte comparison,
    // never case-folded -- so the result is independent of walk order and of the OS.
    std::sort(inputs.begin(), inputs.end(),
              [](const ImportInput& a, const ImportInput& b) { return a.relativePath < b.relativePath; });

    ImportPlanResult result;
    result.entries.reserve(inputs.size());

    // 2. Own-cause pass, in EXACTLY this order per input (AC-22) -- a switch-free if/else-if chain,
    // never independent flags: the source could not be hashed beats every other cause, then New, then
    // SourceChanged, then MetaChanged, then ImporterChanged, else UpToDate.
    for (const ImportInput& input : inputs) {
        ImportPlanEntry entry;
        entry.guid = input.guid;
        entry.relativePath = input.relativePath;

        if (!input.contentHash.has_value()) {
            entry.change = input.hashSkippedByBudget ? ImportChange::NotHashed : ImportChange::Unhashable;
        } else {
            const AssetCacheEntry* previousEntry = previous.find(input.guid);
            if (previousEntry == nullptr) {
                entry.change = ImportChange::New;
            } else if (previousEntry->contentHash != *input.contentHash) {
                entry.change = ImportChange::SourceChanged;
            } else if (previousEntry->metaHash != input.metaHash) {
                entry.change = ImportChange::MetaChanged;
            } else if (previousEntry->importer != input.importer ||
                       previousEntry->importerVersion != input.importerVersion) {
                entry.change = ImportChange::ImporterChanged;
            } else {
                entry.change = ImportChange::UpToDate;
            }
        }
        result.entries.push_back(std::move(entry));
    }

    // 3. Build the REVERSE edge map once, from the PREVIOUS index's own dependency lists -- for each
    // input index `i` with a previous entry, for each `dep` in that entry's dependencies,
    // `dependents[dep]` gains `i`. In the SAME pass: if `dep` names no input in this scan's GUID set,
    // its dependent's input vanished, so `i` is marked dirty-by-dangling directly (D12) -- it can never
    // be reached through the worklist below, because a GUID with no input never enters `result.entries`
    // and so is never popped.
    std::unordered_map<Guid, std::size_t> inputIndexByGuid;
    inputIndexByGuid.reserve(result.entries.size());
    for (std::size_t i = 0; i < result.entries.size(); ++i) {
        inputIndexByGuid.emplace(result.entries[i].guid, i);
    }

    std::unordered_map<Guid, std::vector<std::size_t>> dependents;
    for (std::size_t i = 0; i < result.entries.size(); ++i) {
        const AssetCacheEntry* previousEntry = previous.find(result.entries[i].guid);
        if (previousEntry == nullptr) {
            continue;
        }
        for (const Guid& dep : previousEntry->dependencies) {
            if (!inputIndexByGuid.contains(dep)) {
                // Dangling: `dep` has no input this scan (E24-adjacent -- the dependency itself is
                // gone). Marked directly, subject to the same clean -> dirty transition rule as the
                // worklist below, so a node already dirty for its own reason keeps that reason (AC-22).
                if (result.entries[i].change == ImportChange::UpToDate) {
                    result.entries[i].change = ImportChange::DependencyChanged;
                }
                continue;
            }
            dependents[dep].push_back(i);
        }
    }

    // 4. Worklist over reverse edges (AC-24) -- seeded with every index whose change is not UpToDate
    // (own-cause AND dangling, both already settled above). A dependent is marked and PUSHED only on
    // the clean -> dirty transition, so it enters the queue at most once and this loop is O(V+E) on ANY
    // graph, cycles included. Deliberately: no `visited` set, no cycle detection, no recursion. A cycle
    // containing a dirty node makes every member DependencyChanged (both cycle members are reachable
    // from each other); a cycle containing none is never seeded and stays untouched.
    std::vector<std::size_t> queue;
    for (std::size_t i = 0; i < result.entries.size(); ++i) {
        if (result.entries[i].change != ImportChange::UpToDate) {
            queue.push_back(i);
        }
    }
    for (std::size_t queueHead = 0; queueHead < queue.size(); ++queueHead) {
        const std::size_t poppedIndex = queue[queueHead];
        const auto dependentsIt = dependents.find(result.entries[poppedIndex].guid);
        if (dependentsIt == dependents.end()) {
            continue;
        }
        for (const std::size_t dependentIndex : dependentsIt->second) {
            if (result.entries[dependentIndex].change == ImportChange::UpToDate) {
                result.entries[dependentIndex].change = ImportChange::DependencyChanged;
                queue.push_back(dependentIndex);
            }
        }
    }

    // 5. Collect jobIndices (every non-UpToDate index, in sorted-path order -- entries is already in
    // that order from step 1) and the six counts. `changed` folds SourceChanged + MetaChanged +
    // ImporterChanged for the LOG LINE only; the per-asset reason stays exact on ImportPlanEntry::change.
    for (std::size_t i = 0; i < result.entries.size(); ++i) {
        switch (result.entries[i].change) {
            case ImportChange::UpToDate:
                ++result.upToDate;
                break;
            case ImportChange::New:
                ++result.newAssets;
                result.jobIndices.push_back(i);
                break;
            case ImportChange::SourceChanged:
            case ImportChange::MetaChanged:
            case ImportChange::ImporterChanged:
                ++result.changed;
                result.jobIndices.push_back(i);
                break;
            case ImportChange::DependencyChanged:
                ++result.dependencyChanged;
                result.jobIndices.push_back(i);
                break;
            case ImportChange::Unhashable:
                ++result.unhashable;
                result.jobIndices.push_back(i);
                break;
            case ImportChange::NotHashed:
                ++result.notHashed;
                result.jobIndices.push_back(i);
                break;
        }
    }

    return result;
}

AssetCacheIndex commitImports(const AssetCacheIndex& previous, const std::vector<ImportInput>& inputs,
                              const ImportPlanResult& plan) {
    // `plan` names each input's REASON, which this function does not need: the one decision it makes
    // per input -- "was a hash resolved this scan?" -- reads `ImportInput::contentHash`'s engagement
    // directly (A19), which is exactly equivalent (contentHash is nullopt IFF change is Unhashable or
    // NotHashed) and keeps this function correct even if a caller's `plan` was computed against a
    // DIFFERENT `previous` snapshot than the one given here. Declared for symmetry with planImports's
    // signature; unused by this algorithm.
    (void)plan;

    AssetCacheIndex result;
    result.entries.reserve(previous.entries.size() + inputs.size());

    std::unordered_set<Guid> seenGuids;
    seenGuids.reserve(inputs.size());

    // One pass over the inputs: a hash resolved this scan commits a FRESH entry; no hash preserves the
    // previous entry VERBATIM (AC-25/INV-C4), fabricating nothing.
    for (const ImportInput& input : inputs) {
        seenGuids.insert(input.guid);
        if (input.contentHash.has_value()) {
            const AssetCacheEntry* previousEntry = previous.find(input.guid);
            AssetCacheEntry entry;
            entry.guid = input.guid;
            entry.path = input.relativePath;  // D11/E8: a move updates the path without invalidating it
            entry.size = input.size;
            entry.mtime = input.mtime;
            entry.contentHash = *input.contentHash;
            entry.metaHash = input.metaHash;
            entry.importer = input.importer;
            entry.importerVersion = input.importerVersion;
            // Dependencies are CARRIED OVER from the previous entry, never emptied -- inventing an
            // empty list would silently erase a future importer's edges the first time an older build
            // ran (D-6). Empty when there was no previous entry for this GUID.
            entry.dependencies = previousEntry != nullptr ? previousEntry->dependencies : std::vector<Guid>{};
            entry.missing = 0;  // A19: seen this scan, however it was classified
            result.entries.push_back(std::move(entry));
        } else {
            // Unhashable / NotHashed: no hash was resolved this scan. Copy the previous entry forward
            // VERBATIM, `missing` included -- and commit NO entry at all when there was none (a
            // NotHashed asset must never appear up to date; R-C2, seed S15).
            const AssetCacheEntry* previousEntry = previous.find(input.guid);
            if (previousEntry != nullptr) {
                result.entries.push_back(*previousEntry);
            }
        }
    }

    // Then one pass over `previous`: a GUID this scan never saw at all gets `missing + 1`, retained
    // while that value is <= MISSING_SCAN_GRACE, dropped when it would exceed it (A19: survives at 3,
    // dropped on the scan that would make it 4).
    for (const AssetCacheEntry& previousEntry : previous.entries) {
        if (seenGuids.contains(previousEntry.guid)) {
            continue;  // already handled above, one way or the other
        }
        if (previousEntry.missing + 1 > MISSING_SCAN_GRACE) {
            continue;  // dropped
        }
        AssetCacheEntry carried = previousEntry;
        carried.missing += 1;
        result.entries.push_back(std::move(carried));
    }

    std::sort(result.entries.begin(), result.entries.end(),
              [](const AssetCacheEntry& a, const AssetCacheEntry& b) { return a.guid < b.guid; });
    return result;
}

std::vector<ReattachMatch> planReattachments(const std::vector<ReattachCandidate>& candidates,
                                             const std::vector<OrphanMeta>& orphans, const AssetCacheIndex& previous,
                                             const std::vector<Guid>& liveGuids,
                                             const std::vector<std::string>& livePaths) {
    // 1. byHash: contentHash -> indices of PREVIOUS entries whose path this scan did NOT see live.
    // An entry whose path IS still live is filtered out HERE, before the hash map even exists -- two
    // byte-identical LIVE files therefore never contend for either identity: content equality is
    // deliberately never used for identity dedup (spec correction A12; IP36's whole point).
    const std::unordered_set<std::string> livePathSet(livePaths.begin(), livePaths.end());
    std::unordered_map<ContentHash, std::vector<std::size_t>> byHash;
    for (std::size_t i = 0; i < previous.entries.size(); ++i) {
        if (livePathSet.contains(previous.entries[i].path)) {
            continue;  // still live this scan -- not a reattachment target
        }
        // Entries with `missing > 0` are FULLY eligible (D14): no filter on that field at all.
        byHash[previous.entries[i].contentHash].push_back(i);
    }

    // 2. byGuidOrphans: guid -> orphan indices.
    std::unordered_map<Guid, std::vector<std::size_t>> byGuidOrphans;
    for (std::size_t i = 0; i < orphans.size(); ++i) {
        byGuidOrphans[orphans[i].guid].push_back(i);
    }

    // 3. liveGuidSet.
    const std::unordered_set<Guid> liveGuidSet(liveGuids.begin(), liveGuids.end());

    // 4. Process candidates in SORTED relativePath order -- deterministic and independent of the
    // caller's own vector order (INV-C3): "the first in sorted path order takes it" (AC-27).
    std::vector<std::size_t> candidateOrder(candidates.size());
    for (std::size_t i = 0; i < candidateOrder.size(); ++i) {
        candidateOrder[i] = i;
    }
    std::sort(candidateOrder.begin(), candidateOrder.end(), [&candidates](std::size_t lhs, std::size_t rhs) {
        return candidates[lhs].relativePath < candidates[rhs].relativePath;
    });

    std::vector<ReattachMatch> matches;
    for (const std::size_t candidateIndex : candidateOrder) {
        const ReattachCandidate& candidate = candidates[candidateIndex];

        // Conditions 1 and 2 (candidate exists, its hash was computed) hold by the CALLER's contract
        // -- ReattachCandidate::contentHash is not optional, unlike ImportInput's. Condition 3 starts
        // the runtime work: byHash[H] must have EXACTLY ONE surviving element (two -> ambiguous ->
        // nothing at all, not a guess -- IP28).
        const auto hashIt = byHash.find(candidate.contentHash);
        if (hashIt == byHash.end() || hashIt->second.size() != 1) {
            continue;
        }
        const std::size_t previousEntryIndex = hashIt->second.front();
        const AssetCacheEntry& previousEntry = previous.entries[previousEntryIndex];

        // Condition 4: that entry's GUID is claimed by NO live sidecar.
        if (liveGuidSet.contains(previousEntry.guid)) {
            continue;
        }

        // Condition 5: byGuidOrphans[guid] has EXACTLY ONE element.
        const auto orphanIt = byGuidOrphans.find(previousEntry.guid);
        if (orphanIt == byGuidOrphans.end() || orphanIt->second.size() != 1) {
            continue;
        }
        const OrphanMeta& orphan = orphans[orphanIt->second.front()];

        ReattachMatch match;
        match.candidateIndex = candidateIndex;
        match.guid = previousEntry.guid;
        match.fromMetaPath = orphan.relativePath;
        match.fromAssetPath = previousEntry.path;
        matches.push_back(std::move(match));

        // Remove this GUID from further consideration in BOTH maps -- two candidates can never claim
        // one identity (AC-27's "two candidates, one GUID" case, IP34).
        byHash.erase(hashIt);
        byGuidOrphans.erase(orphanIt);
    }

    return matches;
}

}  // namespace engine::editor

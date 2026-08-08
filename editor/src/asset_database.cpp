// Aero Engine — AssetDatabase's scan (tasks 3.1.1 + 3.1.2). No <filesystem>, no <fstream>, no
// recursion, no logging (INV-A3): status is RETURNED, never printed (project_files.hpp:15-16's
// convention, a fifth application). Composes 2.2.4's listDirectory, 2.5.1/2.6.1's text_file and
// 3.1.2's asset_cache entirely; touches disk through none of them directly.
//
// task 3.1.2 folds 3.1.1's five phases into eight: (1) guard, (2) walk + pair (now with canonical-
// path directory dedup, D9), (3) load the import cache, (4) hash what the (size, mtime) fast path
// could not vouch for, (5) re-attach orphans, (6) plan identity (planAssetMetas, unchanged), (7)
// write + index (3.1.1's write loop, extended for Reattached and A10's metaHash-of-what-we-wrote),
// (8) plan imports and commit the cache. <filesystem>/<fstream> stay banished (INV-A6/AC-32): the
// walk still uses only listDirectory/canonicalDirectory (project_files.cpp), and the cache load/write
// still uses only readTextFile/writeTextFileAtomic/ensureDirectory/fileExists (text_file.cpp).
#include <aero/editor/asset_cache.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/model_import.hpp>  // task 3.2.1: isImportableModelName/importModel/ImportDepth/
                                         // ImportStatus/importStatusLabel -- phase 7.5's ONLY new
                                         // dependency. GLTF_IMPORTER_NAME/VERSION arrive transitively
                                         // through this header's own import_settings.hpp include.
#include <aero/editor/text_file.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// Append `entry` to `list` while it is under MAX_REPORTED_PER_CATEGORY; the TOTAL counter (a
// separate std::size_t owned by the caller) is always incremented, capped or not, so a report can
// always say "…and N more" (D14).
void appendCapped(std::vector<std::string>& list, std::string entry) {
    if (list.size() < MAX_REPORTED_PER_CATEGORY) {
        list.push_back(std::move(entry));
    }
}

// ASCII-only, locale-independent. NEVER std::tolower(char): UTF-8 continuation bytes are NEGATIVE as
// char, which is UB and trips bugprone-signed-char-misuse (project_files.cpp:44-46's precedent,
// copied TU-locally like every other file in this task that needs it -- asset_meta.cpp, project.cpp,
// console_model.cpp -- there is no shared header for a two-line function).
constexpr unsigned char foldAscii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

// Code-review finding 2: the write-conflict guard's own comparator. Full-string, ASCII-case-
// insensitive, no allocation -- deliberately unconditional on every platform (D7's "one rule, no
// exceptions" reasoning): on a case-SENSITIVE filesystem this is merely conservative (two distinct
// files never collide at the OS level, so refusing the write costs an identity this session but
// destroys nothing); on a case-INSENSITIVE one it is what stops the destructive write from
// happening at all.
bool equalsAsciiCaseInsensitive(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (foldAscii(static_cast<unsigned char>(a[i])) != foldAscii(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// task 3.1.2 (D9): a pending directory carries its OWN canonical path alongside its relative one, so
// a non-symlink child can derive ITS canonical path for free (parentCanonical + '/' + name -- zero
// extra syscalls) instead of paying for canonicalDirectory on every directory in the tree.
struct PendingDir {
    std::string rel;
    std::string canonical;
};

// task 3.1.2: what phase 4's fast path needs from the walk, per asset -- free, since the FileEntry
// already carries it (plan A2/A3). `known` is false whenever EITHER field could not be read (a broken
// symlink, an OS refusal) -- the fast path requires BOTH.
struct SizeMtimeInfo {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    bool known = false;
};

// A present-but-unparseable sidecar (metaPresent && !guid) -- computable BEFORE planAssetMetas runs,
// since it is a pure function of the same two fields planAssetMetas itself switches on (asset_meta.cpp
// step 2). Phase 4 must never hash such a record (D11/INV-C1): it has no identity to key a cache
// entry on.
constexpr bool isInvalidPlanEntry(const AssetPlanEntry& entry) noexcept {
    return entry.metaPresent && !entry.guid.has_value();
}

}  // namespace

const std::string& AssetDatabase::root() const noexcept { return rootUtf8; }
const std::string& AssetDatabase::projectRoot() const noexcept { return projectRootUtf8; }
std::size_t AssetDatabase::size() const noexcept { return recordList.size(); }
std::span<const AssetRecord> AssetDatabase::records() const noexcept { return recordList; }  // task 3.1.3
std::size_t AssetDatabase::cacheSize() const noexcept { return cache.entries.size(); }
const ImportPlanResult& AssetDatabase::importPlan() const noexcept { return plan; }
std::uint64_t AssetDatabase::generation() const noexcept { return generationValue; }  // task 3.1.4

void AssetDatabase::invalidateCache() noexcept {
    cache = AssetCacheIndex{};
    // Defensive, and NOT load-bearing -- stated plainly because an earlier comment here claimed these
    // two lines "must go together" and no test can hold that claim: phase 3 clears `cacheTextOnDisk`
    // unconditionally, ahead of the `cacheInvalidated` branch, so deleting this line reddens nothing.
    // Keep it anyway (it costs nothing and it keeps the object self-consistent between the call and
    // the next scan), but the invariant that ACTUALLY protects a Reimport All from silently skipping
    // its rewrite is the skip-the-reload branch in phase 3, which AD58 covers.
    cacheTextOnDisk.clear();
    // Logged deviation (asset_database.hpp's comment beside the member): phase 3 otherwise reloads
    // BOTH fields straight back from disk on the very next scan, before phase 4 ever runs, making this
    // whole call a no-op. Consumed by exactly one phase 3.
    cacheInvalidated = true;
}

const AssetRecord* AssetDatabase::findByPath(std::string_view relativePath) const noexcept {
    // `records` is sorted byte-lexicographically by relativePath (planAssetMetas' own contract, AC-22)
    // -- a std::lower_bound over the vector directly, never a std::string constructed from the
    // std::string_view argument (that would allocate inside a noexcept function).
    const auto it =
        std::lower_bound(recordList.begin(), recordList.end(), relativePath,
                         [](const AssetRecord& record, std::string_view path) { return record.relativePath < path; });
    if (it == recordList.end() || it->relativePath != relativePath) {
        return nullptr;
    }
    return &*it;
}

const AssetRecord* AssetDatabase::findByGuid(Guid guid) const noexcept {
    if (!guid.valid()) {
        return nullptr;
    }
    const auto it = std::lower_bound(byGuid.begin(), byGuid.end(), guid,
                                     [](const std::pair<Guid, std::size_t>& entry, Guid g) { return entry.first < g; });
    if (it == byGuid.end() || it->first != guid) {
        return nullptr;
    }
    return &recordList[it->second];
}

std::optional<Guid> AssetDatabase::guidForPath(std::string_view relativePath) const noexcept {
    const AssetRecord* const record = findByPath(relativePath);
    if (record == nullptr) {
        return std::nullopt;
    }
    return record->guid;
}

AssetScanReport AssetDatabase::rescan(std::string newProjectRootUtf8, std::string newAssetsRootUtf8,
                                      GuidGenerator& generator, std::uint64_t hashBudgetBytes,
                                      std::uint64_t probeBudgetBytes) {
    // task 3.1.4 (D8): FIRST, not last. rescan() has THREE return paths -- the empty-roots guard, the
    // non-Ok root listing, and the normal end -- so "bump at the end" would need three edits today and
    // a fourth for any future early return. Bumping here makes "every rescan() call bumps the
    // generation" structurally true instead of remembered. Nothing reads generation() during a scan:
    // rescan() calls out to no consumer.
    ++generationValue;
    projectRootUtf8 = std::move(newProjectRootUtf8);
    rootUtf8 = std::move(newAssetsRootUtf8);
    recordList.clear();
    byGuid.clear();
    plan = ImportPlanResult{};

    AssetScanReport report;

    // ---- phase 1: guard --------------------------------------------------------------------------
    if (projectRootUtf8.empty() || rootUtf8.empty()) {
        report.status = ScanStatus::Missing;  // no project is open, or no assets root is configured
        return report;                        // zero writes; the cache file is left UNTOUCHED (AC-36)
    }
    DirectoryListing rootListing = listDirectory(rootUtf8, "", /*includeHidden=*/false);
    if (rootListing.status != ScanStatus::Ok) {
        report.status = rootListing.status;
        return report;  // AC-36: phase 3 never runs, so an existing cache file is never even opened
    }
    report.status = ScanStatus::Ok;

    // task 3.1.2 (A7): the walk's own output directory is excluded from the walk by CANONICAL PATH,
    // reusing D9's machinery rather than hand-rolled string arithmetic (<filesystem> stays forbidden,
    // INV-A6). "" whenever Library/ does not exist yet (canonicalDirectory's own failure return) --
    // which is exactly right for a first scan: there is nothing to skip.
    const std::string rootCanonical = canonicalDirectory(rootUtf8);
    const std::string libraryDirPath = projectRootUtf8 + '/' + std::string(ASSET_CACHE_DIR_NAME);
    const std::string libraryCanonical = canonicalDirectory(libraryDirPath);

    // ---- phase 2: walk + pair, dedup by canonical directory path (D9) ------------------------------
    std::vector<AssetPlanEntry> planEntries;
    // Two side maps, keyed by relative ASSET path, carrying what planAssetMetas' pure records cannot:
    // it sorts and mutates in place, so the pre-repair on-disk GUID and a read/parse failure's reason
    // are both gone by the time it returns. Populated here, consulted in phase 5 and phase 7/8.
    std::unordered_map<std::string, Guid> onDiskGuidByPath;
    std::unordered_map<std::string, std::string> invalidReasonByPath;
    // task 3.1.2: the digest of the WHOLE sidecar's bytes (D4), hashed for free as it is read -- the
    // text is already in hand for parseMeta. Recorded only when the READ succeeded (A10's other half:
    // a FRESHLY WRITTEN sidecar's own entry is added in phase 7, from the text just written).
    std::unordered_map<std::string, ContentHash> metaHashByPath;
    // task 3.1.2: every scannable asset's (size, mtime), straight from the FileEntry the walk already
    // holds -- free, and exactly what phase 4's fast path needs.
    std::unordered_map<std::string, SizeMtimeInfo> sizeMtimeByPath;
    // Code-review finding 2: every orphan's relative path, UNCAPPED -- report.orphans itself is
    // capped at MAX_REPORTED_PER_CATEGORY and must never be the source of truth for a safety check.
    // Consulted in phase 5 (candidates for re-attachment) and phase 7 (before any Created/Repaired/
    // Reattached write). report.orphans/orphanTotal are built ONCE, at the end of phase 5, from this
    // list minus whatever phase 5 consumed -- never populated incrementally during the walk (§6.8).
    std::vector<std::string> allOrphanPaths;
    const auto recordOrphan = [&](std::string_view dirRelPath, std::string_view metaName) {
        allOrphanPaths.push_back(joinRelative(dirRelPath, metaName));
    };

    // task 3.1.2 (D9): canonical-path dedup state. `visitedCanonical` is seeded with the ROOT's own
    // canonical path BEFORE the loop, so a self-link (`assets/link -> assets`) is caught on the first
    // repeat. `relByCanonical` records each visited directory's WINNING relative path, so a later
    // alias's WARN can name both sides -- the two containers are always kept in lockstep, so indexing
    // relByCanonical for an already-visited canonical path can never miss.
    std::set<std::string> visitedCanonical{rootCanonical};
    std::map<std::string, std::string> relByCanonical{{rootCanonical, std::string()}};

    std::vector<PendingDir> stack;  // explicit stack of PENDING directories -- misc-no-recursion
    stack.push_back(PendingDir{std::string(), rootCanonical});
    std::optional<DirectoryListing> pendingRootListing = std::move(rootListing);

    while (!stack.empty()) {
        const PendingDir dir = std::move(stack.back());
        stack.pop_back();
        const std::string& dirRel = dir.rel;

        DirectoryListing listing;
        if (dirRel.empty() && pendingRootListing.has_value()) {
            listing = std::move(*pendingRootListing);
            pendingRootListing.reset();
        } else {
            listing = listDirectory(rootUtf8, dirRel, /*includeHidden=*/false);
        }

        report.skippedEntries += listing.skipped;
        if (listing.truncated) {
            report.truncated = true;
        }
        if (listing.status != ScanStatus::Ok) {
            ++report.unreadableDirs;  // a directory BELOW the root failed -- the root's own status
            continue;                 // already lives in report.status (set once, in phase 1)
        }

        // Bucket this directory's entries: subdirectories to push, scannable assets, and sidecars.
        std::vector<const FileEntry*> dirAssets;
        std::vector<const FileEntry*> dirMetas;
        for (const FileEntry& entry : listing.entries) {
            if (entry.isDirectory) {
                std::string childRel = joinRelative(dirRel, entry.name);
                // D9: derive the child's canonical path for FREE when no link is in play (the
                // parent's canonical path is known and this child is not itself a symlink) -- zero
                // extra syscalls. Otherwise pay for the one real canonicalDirectory call (A7).
                std::string childCanonical;
                if (!entry.isSymlink && !dir.canonical.empty()) {
                    childCanonical = dir.canonical + '/' + entry.name;  // EXACT: no link in this chain
                } else {
                    childCanonical = canonicalDirectory(rootUtf8 + '/' + childRel);
                }
                if (!libraryCanonical.empty() && childCanonical == libraryCanonical) {
                    continue;  // A7: our OWN output -- not an alias, reported nowhere
                }
                if (childCanonical.empty()) {
                    // A symlink (or, on an exotic filesystem, a plain directory) we could not resolve:
                    // refuse to descend rather than guess (E17).
                    appendCapped(report.aliasedDirs, childRel + ": could not be resolved");
                    ++report.aliasedDirTotal;
                    continue;
                }
                const bool inserted = visitedCanonical.insert(childCanonical).second;
                if (!inserted) {
                    appendCapped(report.aliasedDirs,
                                 childRel + ": already scanned as '" + relByCanonical[childCanonical] + "'");
                    ++report.aliasedDirTotal;
                    continue;  // AC-31: no descent, NO RECORDS
                }
                relByCanonical.emplace(childCanonical, childRel);
                if (depthOf(childRel) < MAX_TREE_DEPTH) {
                    stack.push_back(PendingDir{std::move(childRel), std::move(childCanonical)});
                } else {
                    report.depthLimited = true;
                }
            } else if (isMetaFileName(entry.name)) {
                dirMetas.push_back(&entry);
            } else if (isScannableAssetName(entry.name)) {
                dirAssets.push_back(&entry);
                ++report.filesSeen;
            }
        }

        // Pair each asset against its sidecar WITHIN this directory's own entry set (3.1.1's phase 3).
        // Two candidates are possible only on a case-sensitive filesystem holding e.g. both "x.meta"
        // and "x.META": the byte-lexicographically FIRST (by the SIDECAR's own name) wins,
        // deterministically -- no guessing -- and every other candidate is reported as an orphan (E29).
        std::vector<bool> metaConsumed(dirMetas.size(), false);
        for (const FileEntry* const asset : dirAssets) {
            std::vector<std::size_t> candidateMetas;
            for (std::size_t i = 0; i < dirMetas.size(); ++i) {
                if (assetNameForMeta(dirMetas[i]->name) == asset->name) {  // EXACT bytes (AC-19)
                    candidateMetas.push_back(i);
                }
            }

            const std::string assetRelPath = joinRelative(dirRel, asset->name);
            sizeMtimeByPath[assetRelPath] =
                SizeMtimeInfo{asset->size, asset->mtime, asset->sizeKnown && asset->mtimeKnown};

            AssetPlanEntry planEntry;
            planEntry.relativePath = assetRelPath;

            if (candidateMetas.empty()) {
                planEntry.metaPresent = false;  // no sidecar -- Created, or Reattached (phase 5)
                planEntries.push_back(std::move(planEntry));
                continue;
            }

            std::size_t winner = candidateMetas.front();
            for (const std::size_t candidate : candidateMetas) {
                if (dirMetas[candidate]->name < dirMetas[winner]->name) {
                    winner = candidate;
                }
            }
            for (const std::size_t candidate : candidateMetas) {
                metaConsumed[candidate] = true;
                if (candidate == winner) {
                    continue;
                }
                recordOrphan(dirRel, dirMetas[candidate]->name);
            }

            planEntry.metaPresent = true;
            const std::string metaRelPath = joinRelative(dirRel, dirMetas[winner]->name);
            const FileReadResult read = readTextFile(rootUtf8 + '/' + metaRelPath);
            if (!read.text.has_value()) {
                // A read failure and a parse failure are the same thing here -- no identity (A7).
                planEntry.guid = std::nullopt;
                invalidReasonByPath[assetRelPath] = read.error;
            } else {
                // task 3.1.2 (D4): hashed as it is read, for free -- the text is already in hand for
                // parseMeta below, whatever parseMeta then decides.
                metaHashByPath[assetRelPath] = hashBytes(std::as_bytes(std::span<const char>(*read.text)));
                const MetaParseResult parsed = parseMeta(*read.text);
                if (parsed.guid.has_value()) {
                    planEntry.guid = parsed.guid;
                    onDiskGuidByPath[assetRelPath] = *parsed.guid;
                    // task 3.2.1: engaged == a well-formed importer block; disengaged (absent OR
                    // malformed, D7 rule 1) leaves planEntry.importSettings at its default. Read here,
                    // beside the GUID, because parseMeta already produced both from the SAME text.
                    if (parsed.importer.has_value()) {
                        planEntry.importSettings = parsed.importer->settings;
                    }
                    for (const std::string& key : parsed.unknownKeys) {
                        std::string warning = "editor: asset meta '";
                        warning += assetRelPath;
                        warning += "' -- ignoring unknown key \"";
                        warning += key;
                        warning += '"';
                        appendCapped(report.unknownKeyWarnings, std::move(warning));
                        ++report.unknownKeyTotal;
                    }
                } else {
                    planEntry.guid = std::nullopt;
                    invalidReasonByPath[assetRelPath] = parsed.message;
                }
            }
            planEntries.push_back(std::move(planEntry));
        }

        // Every sidecar this directory's assets did NOT claim is an orphan too (D8): reported, left
        // on disk, untouched.
        for (std::size_t i = 0; i < dirMetas.size(); ++i) {
            if (!metaConsumed[i]) {
                recordOrphan(dirRel, dirMetas[i]->name);
            }
        }

        if (report.filesSeen >= MAX_ASSETS) {
            // D14: bounded, and surfaced rather than silent. The CURRENT directory has already
            // finished (every one of its files was just given identity or paired above) -- only
            // directories not yet visited are left unvisited.
            report.truncated = true;
            break;
        }
    }

    // ---- phase 3: load the cache -------------------------------------------------------------------
    cacheTextOnDisk.clear();
    cache = AssetCacheIndex{};
    if (cacheInvalidated) {
        // invalidateCache()'s one-shot: skip the reload entirely for THIS scan, so phase 4 finds no
        // fast-path candidates and phase 8 commits a genuinely fresh index -- exactly as if the file
        // were absent, without touching the file (AC-35). Consumed once, regardless of what follows.
        cacheInvalidated = false;
    } else {
        const std::string cacheIndexPath = libraryDirPath + '/' + std::string(ASSET_CACHE_FILE_NAME);
        const FileReadResult cacheRead = readTextFile(cacheIndexPath);
        if (cacheRead.text.has_value()) {
            cacheTextOnDisk = *cacheRead.text;  // D15's comparand, retained VERBATIM
            AssetCacheParseResult parsed = parseAssetCache(cacheTextOnDisk);
            cache = std::move(parsed.index);
            report.cacheEntriesLoaded = cache.entries.size();
            report.cacheEntriesDropped = parsed.droppedEntries;
            report.cacheDepsDropped = parsed.droppedDependencies;
            report.cacheTruncated = parsed.truncated;
            if (parsed.outcome == CacheLoadOutcome::Discarded) {
                report.cacheDiscardReason = std::move(parsed.discardReason);
            }
        }  // else: Absent. Empty index, empty comparand, NO discard reason (E19).
    }

    // Every subsequent phase walks `planEntries` in SORTED path order -- both for determinism
    // (independent of walk order) and because the hash budget's exhaustion point (phase 4) must be
    // reproducible.
    std::vector<std::size_t> sortedPlanIndices(planEntries.size());
    for (std::size_t i = 0; i < sortedPlanIndices.size(); ++i) {
        sortedPlanIndices[i] = i;
    }
    std::sort(sortedPlanIndices.begin(), sortedPlanIndices.end(), [&planEntries](std::size_t a, std::size_t b) {
        return planEntries[a].relativePath < planEntries[b].relativePath;
    });

    // Code-review finding 2: how many planEntries claim EACH on-disk GUID this scan -- counted BEFORE
    // the fast-path loop below, because phase 4 runs before phase 6's planAssetMetas repairs a
    // duplicate. Two entries sharing a pre-repair GUID must never both consult the SAME cache entry:
    // the later one (which phase 6 will give a fresh GUID) would otherwise take the fast path and
    // inherit the FIRST one's contentHash under its own new identity.
    std::unordered_map<Guid, std::size_t> onDiskGuidCounts;
    for (const AssetPlanEntry& entry : planEntries) {
        if (entry.guid.has_value()) {
            ++onDiskGuidCounts[*entry.guid];
        }
    }

    // ---- phase 4: hash -------------------------------------------------------------------------------
    std::unordered_map<std::string, ContentHash> hashByPath;
    std::unordered_set<std::string> skippedByBudget;
    for (const std::size_t index : sortedPlanIndices) {
        const AssetPlanEntry& entry = planEntries[index];
        if (isInvalidPlanEntry(entry)) {
            continue;  // D11/INV-C1: no identity, no hash, no import input
        }

        const auto sizeMtimeIt = sizeMtimeByPath.find(entry.relativePath);
        const bool sizeMtimeKnown = sizeMtimeIt != sizeMtimeByPath.end() && sizeMtimeIt->second.known;

        // Code-review finding 2: refuse the fast path for a GUID more than one on-disk sidecar claims
        // THIS scan -- see onDiskGuidCounts' own comment above.
        const bool guidUniqueThisScan = entry.guid.has_value() && onDiskGuidCounts.find(*entry.guid)->second == 1;

        bool reusedFromCache = false;
        if (sizeMtimeKnown && guidUniqueThisScan) {
            const AssetCacheEntry* const cacheEntry = cache.find(*entry.guid);
            if (cacheEntry != nullptr && cacheEntry->size == sizeMtimeIt->second.size &&
                cacheEntry->mtime == sizeMtimeIt->second.mtime) {
                hashByPath[entry.relativePath] = cacheEntry->contentHash;
                ++report.fastPathHits;
                reusedFromCache = true;
            }
        }
        if (reusedFromCache) {
            continue;  // Metaless assets never reach here: `entry.guid` is always nullopt for them.
        }

        // E30: the budget is checked BEFORE the file is opened, so a single file bigger than the
        // whole budget is still hashed to completion exactly once -- otherwise it could never be
        // hashed at all.
        if (report.hashedBytes < hashBudgetBytes) {
            const FileHashResult hashResult = hashFileContents(rootUtf8 + '/' + entry.relativePath);
            if (hashResult.hash.has_value()) {
                ++report.hashed;
                report.hashedBytes += hashResult.bytesRead;
                hashByPath[entry.relativePath] = *hashResult.hash;
            } else {
                appendCapped(report.hashFailures, entry.relativePath + ": " + hashResult.error);
                ++report.hashFailureTotal;
            }
        } else {
            skippedByBudget.insert(entry.relativePath);
            report.hashBudgetExhausted = true;
        }
    }
    report.largeHashNotice = report.hashedBytes > HASH_NOTICE_THRESHOLD_BYTES;

    // ---- phase 5: re-attach ---------------------------------------------------------------------
    // The MATCHING sub-step below runs only when at least one metaless asset produced a hash this
    // scan (D13's own cost control) -- in steady state (every asset already has a sidecar) this opens
    // ZERO orphan files. The report.orphans REBUILD always runs, unconditionally, so a scan with no
    // candidates ends up with exactly the walk's own orphan list -- the same answer 3.1.1 gave.
    std::vector<ReattachCandidate> candidates;
    std::vector<std::size_t> candidatePlanIndex;  // candidates[i] <-> planEntries[candidatePlanIndex[i]]
    for (const std::size_t index : sortedPlanIndices) {
        const AssetPlanEntry& entry = planEntries[index];
        if (entry.metaPresent) {
            continue;
        }
        const auto hashIt = hashByPath.find(entry.relativePath);
        if (hashIt == hashByPath.end()) {
            continue;  // not hashed this scan (budget or a read failure) -- not a candidate
        }
        candidatePlanIndex.push_back(index);
        candidates.push_back(ReattachCandidate{entry.relativePath, hashIt->second});
    }

    std::unordered_set<std::string> consumedOrphanPaths;
    if (!candidates.empty()) {
        std::vector<OrphanMeta> orphans;
        const std::size_t orphanReadCount = std::min(allOrphanPaths.size(), MAX_ORPHANS_READ);
        for (std::size_t i = 0; i < orphanReadCount; ++i) {
            const FileReadResult orphanRead = readTextFile(rootUtf8 + '/' + allOrphanPaths[i]);
            if (!orphanRead.text.has_value()) {
                continue;
            }
            const MetaParseResult parsed = parseMeta(*orphanRead.text);
            if (!parsed.guid.has_value()) {
                continue;
            }
            orphans.push_back(OrphanMeta{allOrphanPaths[i], *parsed.guid});
        }

        std::vector<Guid> liveGuids;
        liveGuids.reserve(onDiskGuidByPath.size());
        for (const auto& [path, guid] : onDiskGuidByPath) {
            liveGuids.push_back(guid);
        }
        std::vector<std::string> livePaths;
        livePaths.reserve(planEntries.size());
        for (const AssetPlanEntry& entry : planEntries) {
            livePaths.push_back(entry.relativePath);
        }

        const std::vector<ReattachMatch> matches = planReattachments(candidates, orphans, cache, liveGuids, livePaths);
        for (const ReattachMatch& match : matches) {
            planEntries[candidatePlanIndex[match.candidateIndex]].reattachedGuid = match.guid;
            consumedOrphanPaths.insert(match.fromMetaPath);
            appendCapped(report.reattachments, candidates[match.candidateIndex].relativePath + ": took " +
                                                   formatGuid(match.guid) + " from orphan '" + match.fromMetaPath +
                                                   "' (last seen at '" + match.fromAssetPath +
                                                   "'); the old sidecar is left on disk and is safe to delete");
            ++report.reattachmentTotal;
        }
    }

    // §6.8 phase 5: a consumed orphan is reported ONCE, as a re-attachment, never also in `orphans` --
    // rebuilt here, unconditionally, from the UNCAPPED source of truth minus whatever was consumed.
    for (const std::string& orphanPath : allOrphanPaths) {
        if (consumedOrphanPaths.contains(orphanPath)) {
            continue;
        }
        appendCapped(report.orphans, orphanPath);
        ++report.orphanTotal;
    }

    // ---- phase 6: plan identity -----------------------------------------------------------------
    AssetPlanResult planResult = planAssetMetas(std::move(planEntries), generator);
    report.created = planResult.created;
    report.repaired = planResult.repaired;
    report.invalid = planResult.invalid;
    report.largeCreateNotice = planResult.writeIndices.size() > CREATE_NOTICE_THRESHOLD;

    // ---- phase 7: write and index ------------------------------------------------------------------
    recordList = std::move(planResult.records);  // `planResult.writeIndices` stays valid -- separate member
    // Code-review finding 2: which indices got downgraded below -- consulted by the reporting loop so
    // a conflict is reported exactly once, in report.writeConflicts, never doubled into
    // report.invalidPaths too.
    std::vector<bool> writeConflict(recordList.size(), false);
    // task 3.1.2 (A10): a write FAILURE excludes that record from phase 8's inputs entirely -- the
    // bytes on disk are not the bytes we would have hashed, and committing a hash for a file we did
    // not manage to write is exactly the false-UpToDate R-C2 forbids.
    std::vector<bool> writeFailed(recordList.size(), false);
    for (const std::size_t index : planResult.writeIndices) {
        AssetRecord& record = recordList[index];  // mutable: a conflict downgrades it below
        const std::string metaRelPath = record.relativePath + std::string(ASSET_META_SUFFIX);

        // D7/D8's "never destroy, never guess", extended to the write itself: a Created/Repaired/
        // Reattached write must never land on a path phase 2 already classified as an ORPHAN, compared
        // ASCII-case-insensitively against the UNCAPPED list -- on a case-insensitive filesystem
        // (APFS, NTFS -- most users) that write would silently replace a valid, unrelated orphaned
        // sidecar with a freshly-minted GUID, destroying a committed identity with no log, no guard
        // and no test able to see it. Checked on every platform, unconditionally (D7's "one rule, no
        // exceptions"): on a case-SENSITIVE filesystem this is merely conservative.
        const bool conflicts = std::any_of(
            allOrphanPaths.begin(), allOrphanPaths.end(),
            [&](const std::string& orphanPath) { return equalsAsciiCaseInsensitive(orphanPath, metaRelPath); });
        if (conflicts) {
            writeConflict[index] = true;
            if (record.state == AssetMetaState::Created) {
                --report.created;
            } else if (record.state == AssetMetaState::Repaired) {
                --report.repaired;
            }
            // A Reattached record that conflicts is downgraded exactly the same way -- its
            // re-attachment was already reported in report.reattachments at phase 5 and is not
            // un-reported; AssetScanReport carries no separate "reattached" counter to decrement.
            ++report.invalid;
            record.state = AssetMetaState::Invalid;  // D7's posture: no identity this session
            record.guid = Guid{};
            appendCapped(report.writeConflicts, record.relativePath + ": collides with orphan '" + metaRelPath + "'");
            ++report.writeConflictTotal;
            continue;  // INV-A1 still holds: writeTextFileAtomic is not called for this record
        }

        // INV-A1 (amended, §D-8): the ONE call site whose path is built from the ASSETS root.
        const std::string metaText = writeMetaText(record.guid);
        const std::string metaAbsolutePath = rootUtf8 + '/' + metaRelPath;
        const std::string error = writeTextFileAtomic(metaAbsolutePath, metaText);
        if (!error.empty()) {
            // A write failure does NOT abort the scan and removes nothing: the record keeps its
            // in-memory GUID (the editor stays usable) and the next scan retries.
            appendCapped(report.writeFailures, record.relativePath + ": " + error);
            ++report.writeFailureTotal;
            writeFailed[index] = true;
            // Code-review finding 3: the record's own flag, so a READER of `records` directly (the
            // accessor, the footer) can refuse to report `change` for it without needing this local
            // vector, which does not survive past this function.
            record.metaWriteFailed = true;
        } else {
            // A10: metaHash is the digest of the TEXT WE JUST WROTE, not nil -- the obvious "nil for a
            // fresh sidecar" reading would make every fresh asset report MetaChanged forever, starting
            // on the very next scan.
            metaHashByPath[record.relativePath] = hashBytes(std::as_bytes(std::span<const char>(metaText)));
        }
    }

    byGuid.reserve(recordList.size());
    for (std::size_t index = 0; index < recordList.size(); ++index) {
        const AssetRecord& record = recordList[index];
        if (record.state != AssetMetaState::Invalid) {
            byGuid.emplace_back(record.guid, index);  // INV-A7: an Invalid record is never reachable here
        }
        if (record.state == AssetMetaState::Invalid && !writeConflict[index]) {
            const auto reasonIt = invalidReasonByPath.find(record.relativePath);
            const std::string reason = (reasonIt != invalidReasonByPath.end()) ? (": " + reasonIt->second) : "";
            appendCapped(report.invalidPaths, record.relativePath + reason);
        } else if (record.state == AssetMetaState::Repaired) {
            // Only a genuine on-disk duplicate (both assets HAD a real sidecar) carries an "old" GUID
            // to report -- the pathological freshly-minted-collision case (an AP-tier concern; no
            // AssetDatabase-level test constructs it) has none, and is reported with just its new one.
            const auto oldIt = onDiskGuidByPath.find(record.relativePath);
            const std::string oldText = (oldIt != onDiskGuidByPath.end()) ? formatGuid(oldIt->second) : "?";
            appendCapped(report.repairs, record.relativePath + ": " + oldText + " -> " + formatGuid(record.guid));
        }
    }
    // D9 guarantees every LIVE Guid is unique after planAssetMetas' repair pass, so this sort should
    // never need to break a tie -- but a duplicate is handled deliberately, not assumed away: a STABLE
    // sort preserves relativePath order among equal keys, so std::lower_bound's first match is the
    // FIRST claimant by path.
    std::stable_sort(
        byGuid.begin(), byGuid.end(),
        [](const std::pair<Guid, std::size_t>& a, const std::pair<Guid, std::size_t>& b) { return a.first < b.first; });

    // ---- phase 8: plan imports and commit ----------------------------------------------------------
    std::vector<ImportInput> inputs;
    inputs.reserve(recordList.size());
    for (std::size_t index = 0; index < recordList.size(); ++index) {
        const AssetRecord& record = recordList[index];
        if (record.state == AssetMetaState::Invalid || writeFailed[index]) {
            continue;  // no identity (D11/INV-C1), or the bytes on disk are not what we would hash (A10)
        }
        ImportInput input;
        input.guid = record.guid;
        input.relativePath = record.relativePath;
        const auto hashIt = hashByPath.find(record.relativePath);
        if (hashIt != hashByPath.end()) {
            input.contentHash = hashIt->second;
        } else {
            input.hashSkippedByBudget = skippedByBudget.contains(record.relativePath);
        }
        const auto metaHashIt = metaHashByPath.find(record.relativePath);
        if (metaHashIt != metaHashByPath.end()) {
            input.metaHash = metaHashIt->second;
        }
        const auto sizeMtimeIt = sizeMtimeByPath.find(record.relativePath);
        if (sizeMtimeIt != sizeMtimeByPath.end()) {
            input.size = sizeMtimeIt->second.size;
            input.mtime = sizeMtimeIt->second.mtime;
        }
        inputs.push_back(std::move(input));
    }

    plan = planImports(inputs, cache);  // a COPY of `inputs` -- commitImports needs them too

    // ---- phase 7.5: probe changed models (task 3.2.1, D8) ------------------------------------------
    // Runs AFTER planImports (which decided what changed, using the PREVIOUS scan's dependency graph)
    // and BEFORE commitImports (which records this scan's). So a brand-new model plus its brand-new
    // texture cascade correctly from the SECOND scan onward, not the first -- which is CORRECT, since
    // on the first scan both are New anyway and both are re-imported regardless (E11).
    //
    // NEVER LOGS (INV-A3, an eighth application). NEVER WRITES (D7/INV-M9): no .meta, no cache file,
    // nothing. Runs ImportDepth::Structure ONLY -- A SCAN MUST NEVER DECODE A VERTEX. A steady-state
    // scan visits NOTHING, because plan.jobIndices is empty (AC-8).
    //
    // WARNING: check-project-no-delete.sh's Check A denylist includes THIS FILE and its FORBIDDEN_RE
    // matches the bare `::copy`. NO std::copy ANYWHERE IN THIS FILE -- use a range-for or assign().
    //
    // Deviation from the plan's own §D-6 text, logged: `leafOf`/`parentOf` are NOT new TU-local
    // helpers here -- project_files.hpp (already included transitively through asset_database.hpp)
    // already declares BOTH publicly with exactly the semantics §D-6 asked for ("parentDirOf" is
    // `parentOf` verbatim: "" for a root-level file, no trailing slash). A second, TU-local `leafOf`
    // with the identical signature would make an unqualified call inside `namespace engine::editor`
    // AMBIGUOUS (an anonymous namespace's members are injected into the enclosing namespace for
    // lookup), so this reuses the public ones instead of shadowing them. Likewise, `addCapped` is not
    // a real symbol in this file -- the existing capped-category idiom is `appendCapped(list, entry)`
    // plus a separate `++counter` at the call site (used by every other category below); reused
    // verbatim rather than adding an eighth near-duplicate helper.
    {
        std::uint64_t probeBudgetRemaining = probeBudgetBytes;
        for (const std::size_t jobIndex : plan.jobIndices) {
            const ImportPlanEntry& job = plan.entries[jobIndex];
            // plan.entries is planImports' OWN SORTED COPY -- jobIndex is NOT an index into `inputs`
            // (plan §A-12). Both vectors are sorted byte-lexicographically by relativePath, so one
            // lower_bound finds the match; a miss is impossible today and is simply skipped.
            const auto it =
                std::lower_bound(inputs.begin(), inputs.end(), job.relativePath,
                                 [](const ImportInput& in, const std::string& p) { return in.relativePath < p; });
            if (it == inputs.end() || it->relativePath != job.relativePath) {
                continue;
            }
            const std::string_view leaf = leafOf(it->relativePath);
            if (!isImportableModelName(leaf)) {
                continue;  // AC-2: a non-model's importer stays "" and its version stays 0
            }
            const std::string absolutePath = rootUtf8 + '/' + it->relativePath;
            const FileBytesResult fileResult = readFileBytes(absolutePath, MAX_MODEL_FILE_BYTES);
            // The budget is charged with the OBSERVED size, which readFileBytes fills EVEN ON REFUSAL.
            // Checked before the read matters: a file that would blow the budget is not probed at all,
            // and D9's carry-forward handles it correctly (AC-6).
            if (fileResult.size > probeBudgetRemaining) {
                report.probeBudgetExhausted = true;
                continue;  // leave `probe` DISENGAGED
            }
            probeBudgetRemaining -= fileResult.size;
            if (!fileResult.bytes.has_value()) {
                appendCapped(report.importFailures,
                             it->relativePath + ": " +
                                 (fileResult.refusedByCap ? "file is too large to import" : fileResult.error));
                ++report.importFailureTotal;
                continue;  // AC-43/E20: refused WITHOUT being opened; probe stays disengaged
            }
            const AssetRecord* const record = findByPath(it->relativePath);
            const ImportSettings settings = record != nullptr ? record->importSettings : ImportSettings{};
            const std::span<const std::byte> byteSpan(reinterpret_cast<const std::byte*>(fileResult.bytes->data()),
                                                      fileResult.bytes->size());
            const ImportResult imported =
                importModel(leaf, parentOf(it->relativePath), byteSpan, settings, ImportDepth::Structure, {});
            if (imported.status != ImportStatus::Ok && imported.status != ImportStatus::Truncated) {
                appendCapped(report.importFailures, it->relativePath + ": " +
                                                        std::string(importStatusLabel(imported.status)) + " -- " +
                                                        imported.message);
                ++report.importFailureTotal;
                continue;  // a failed import records NO importer and NO dependencies; carry forward
            }
            ProbeOutcome outcome;
            outcome.importer = std::string(GLTF_IMPORTER_NAME);
            outcome.importerVersion = GLTF_IMPORTER_VERSION;
            for (const std::string& uri : imported.externalUris) {
                if (uri == it->relativePath) {
                    continue;  // E9: a SELF-EDGE is dropped -- the cascade's worklist would treat it as
                               // a permanently dirty node
                }
                const std::optional<Guid> dep = guidForPath(uri);
                if (!dep.has_value() || !dep->valid()) {
                    continue;  // AC-5/E7/INV-M8: NEVER a nil GUID in the list. A nil would make the
                               // cascade's find() return nullptr for a reason unrelated to the graph.
                               // The warning already sits in imported.warnings, which the panel shows.
                }
                outcome.dependencies.push_back(*dep);
            }
            std::sort(outcome.dependencies.begin(), outcome.dependencies.end());
            outcome.dependencies.erase(std::unique(outcome.dependencies.begin(), outcome.dependencies.end()),
                                       outcome.dependencies.end());  // E8: two URIs, one path, one edge
            ++report.modelsProbed;
            report.dependenciesRecorded += outcome.dependencies.size();
            it->probe = std::move(outcome);
        }
    }

    // Apply the plan's per-asset reason (`inputs`/`plan.entries` are BOTH sorted byte-lexicographically
    // by relativePath -- planImports' own step 1 -- so this is a single merge pass, no repeated search).
    {
        std::size_t recordCursor = 0;
        for (const ImportPlanEntry& entry : plan.entries) {
            while (recordCursor < recordList.size() && recordList[recordCursor].relativePath < entry.relativePath) {
                ++recordCursor;
            }
            if (recordCursor < recordList.size() && recordList[recordCursor].relativePath == entry.relativePath) {
                recordList[recordCursor].change = entry.change;
            }
        }
    }
    // EVERY record's contentHash, independent of whether it became an import input at all -- a record
    // this scan never hashed simply keeps the default, all-zero ContentHash (also the empty file's
    // real digest, plan A4 -- `change` is the only trustworthy "was this real?" signal).
    for (AssetRecord& record : recordList) {
        const auto hashIt = hashByPath.find(record.relativePath);
        record.contentHash = hashIt != hashByPath.end() ? hashIt->second : ContentHash{};
    }

    report.upToDate = plan.upToDate;
    report.newAssets = plan.newAssets;
    report.changed = plan.changed;
    report.dependencyChanged = plan.dependencyChanged;
    report.unhashable = plan.unhashable;
    report.notHashed = plan.notHashed;

    AssetCacheIndex next = commitImports(cache, inputs, plan);  // NOT const: moved from below
    const std::string nextText = writeAssetCacheText(next);
    if (nextText != cacheTextOnDisk) {  // D15 -- the whole of INV-C5's second half: TEXT, never bytes
        const std::string dirError = ensureDirectory(libraryDirPath);
        if (!dirError.empty()) {
            report.cacheWriteError = dirError;
        } else {
            const std::string ignorePath = libraryDirPath + '/' + std::string(ASSET_CACHE_GITIGNORE_NAME);
            if (!fileExists(ignorePath)) {  // D6/E35 -- written ONLY when absent, never overwritten
                const std::string ignoreError = writeTextFileAtomic(ignorePath, LIBRARY_GITIGNORE_TEXT);
                if (!ignoreError.empty()) {
                    report.cacheWriteError = ignoreError;
                }
            }
            const std::string indexPath = libraryDirPath + '/' + std::string(ASSET_CACHE_FILE_NAME);
            const std::string writeError = writeTextFileAtomic(indexPath, nextText);
            if (!writeError.empty()) {
                report.cacheWriteError = writeError;
            } else {
                report.cacheWritten = true;
                cacheTextOnDisk = nextText;
            }
        }
    }
    cache = std::move(next);  // in-memory regardless of whether the write succeeded (INV-C11)

    return report;
}

}  // namespace engine::editor

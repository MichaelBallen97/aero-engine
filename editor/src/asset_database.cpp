// Aero Engine — AssetDatabase's scan (task 3.1.1). No <filesystem>, no <fstream>, no recursion, no
// logging (INV-A3): status is RETURNED, never printed (project_files.hpp:15-16's convention, a fifth
// application). Composes 2.2.4's listDirectory and 2.5.1/2.6.1's text_file entirely; touches disk
// through neither directly.
#include <aero/editor/asset_database.hpp>
#include <aero/editor/text_file.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

}  // namespace

const std::string& AssetDatabase::root() const noexcept { return rootUtf8; }
std::size_t AssetDatabase::size() const noexcept { return records.size(); }

const AssetRecord* AssetDatabase::findByPath(std::string_view relativePath) const noexcept {
    // `records` is sorted byte-lexicographically by relativePath (planAssetMetas' own contract, AC-22)
    // -- a std::lower_bound over the vector directly, never a std::string constructed from the
    // std::string_view argument (that would allocate inside a noexcept function).
    const auto it =
        std::lower_bound(records.begin(), records.end(), relativePath,
                         [](const AssetRecord& record, std::string_view path) { return record.relativePath < path; });
    if (it == records.end() || it->relativePath != relativePath) {
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
    return &records[it->second];
}

std::optional<Guid> AssetDatabase::guidForPath(std::string_view relativePath) const noexcept {
    const AssetRecord* const record = findByPath(relativePath);
    if (record == nullptr) {
        return std::nullopt;
    }
    return record->guid;
}

AssetScanReport AssetDatabase::rescan(std::string newRootUtf8, GuidGenerator& generator) {
    rootUtf8 = std::move(newRootUtf8);
    records.clear();
    byGuid.clear();

    AssetScanReport report;

    // ---- phase 1: guard --------------------------------------------------------------------------
    if (rootUtf8.empty()) {
        report.status = ScanStatus::Missing;  // no project is open
        return report;
    }
    DirectoryListing rootListing = listDirectory(rootUtf8, "", /*includeHidden=*/false);
    if (rootListing.status != ScanStatus::Ok) {
        report.status = rootListing.status;
        return report;
    }
    report.status = ScanStatus::Ok;

    // ---- phase 2+3: the walk, pairing each directory's assets against its own sidecars as we go ---
    // (phase 3 is directory-scoped, so interleaving it with phase 2 needs no cross-directory state --
    // the alternative, two separate full passes, would be equivalent and strictly more code.)
    std::vector<AssetPlanEntry> planEntries;
    // Two side maps, keyed by relative ASSET path, carrying what planAssetMetas' pure records cannot:
    // it sorts and mutates in place, so the pre-repair on-disk GUID and a read/parse failure's reason
    // are both gone by the time it returns. Populated here, consulted in phase 5.
    std::unordered_map<std::string, Guid> onDiskGuidByPath;
    std::unordered_map<std::string, std::string> invalidReasonByPath;
    // Code-review finding 2: every orphan's relative path, UNCAPPED -- report.orphans itself is
    // capped at MAX_REPORTED_PER_CATEGORY and must never be the source of truth for a safety check.
    // Consulted in phase 5, before any Created/Repaired write.
    std::vector<std::string> allOrphanPaths;
    const auto recordOrphan = [&](std::string_view dirRelPath, std::string_view metaName) {
        std::string path = joinRelative(dirRelPath, metaName);
        appendCapped(report.orphans, path);
        ++report.orphanTotal;
        allOrphanPaths.push_back(std::move(path));
    };

    std::vector<std::string> stack;  // explicit stack of PENDING relative directory paths -- misc-no-recursion
    stack.emplace_back();            // "" == the root, already listed above (phase 1) -- never re-listed
    std::optional<DirectoryListing> pendingRootListing = std::move(rootListing);

    while (!stack.empty()) {
        const std::string dirRel = std::move(stack.back());
        stack.pop_back();

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
                if (depthOf(childRel) < MAX_TREE_DEPTH) {
                    stack.push_back(std::move(childRel));
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

        // Pair each asset against its sidecar WITHIN this directory's own entry set (phase 3). Two
        // candidates are possible only on a case-sensitive filesystem holding e.g. both "x.meta" and
        // "x.META": the byte-lexicographically FIRST (by the SIDECAR's own name) wins, deterministically
        // -- no guessing -- and every other candidate is reported as an orphan (E29).
        std::vector<bool> metaConsumed(dirMetas.size(), false);
        for (const FileEntry* const asset : dirAssets) {
            std::vector<std::size_t> candidates;
            for (std::size_t i = 0; i < dirMetas.size(); ++i) {
                if (assetNameForMeta(dirMetas[i]->name) == asset->name) {  // EXACT bytes (AC-19)
                    candidates.push_back(i);
                }
            }

            const std::string assetRelPath = joinRelative(dirRel, asset->name);
            AssetPlanEntry planEntry;
            planEntry.relativePath = assetRelPath;

            if (candidates.empty()) {
                planEntry.metaPresent = false;  // no sidecar -- will be Created
                planEntries.push_back(std::move(planEntry));
                continue;
            }

            std::size_t winner = candidates.front();
            for (const std::size_t candidate : candidates) {
                if (dirMetas[candidate]->name < dirMetas[winner]->name) {
                    winner = candidate;
                }
            }
            for (const std::size_t candidate : candidates) {
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
                const MetaParseResult parsed = parseMeta(*read.text);
                if (parsed.guid.has_value()) {
                    planEntry.guid = parsed.guid;
                    onDiskGuidByPath[assetRelPath] = *parsed.guid;
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

    // ---- phase 4: plan ----------------------------------------------------------------------------
    AssetPlanResult plan = planAssetMetas(std::move(planEntries), generator);
    report.created = plan.created;
    report.repaired = plan.repaired;
    report.invalid = plan.invalid;
    report.largeCreateNotice = plan.writeIndices.size() > CREATE_NOTICE_THRESHOLD;

    // ---- phase 5: write and index ------------------------------------------------------------------
    records = std::move(plan.records);  // `plan.writeIndices` is a SEPARATE member -- still valid below
    // Code-review finding 2: which indices got downgraded below -- consulted by the reporting loop so
    // a conflict is reported exactly once, in report.writeConflicts, never doubled into
    // report.invalidPaths too.
    std::vector<bool> writeConflict(records.size(), false);
    for (const std::size_t index : plan.writeIndices) {
        AssetRecord& record = records[index];  // mutable: a conflict downgrades it below
        const std::string metaRelPath = record.relativePath + std::string(ASSET_META_SUFFIX);

        // D7/D8's "never destroy, never guess", extended to the write itself: a Created/Repaired
        // write must never land on a path phase 3 already classified as an ORPHAN, compared
        // ASCII-case-insensitively against the UNCAPPED list -- on a case-insensitive filesystem
        // (APFS, NTFS -- most users) that write would silently replace a valid, unrelated orphaned
        // sidecar (e.g. a case-only rename leaving `wood.png.meta` behind beside a new `Wood.png`)
        // with a freshly-minted GUID, destroying a committed identity with no log, no guard and no
        // test able to see it (the widened check-project-no-delete.sh guard cannot: this is a WRITE,
        // not a `remove`). Checked on every platform, unconditionally (D7's "one rule, no
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
            ++report.invalid;
            record.state = AssetMetaState::Invalid;  // D7's posture: no identity this session
            record.guid = Guid{};
            appendCapped(report.writeConflicts, record.relativePath + ": collides with orphan '" + metaRelPath + "'");
            ++report.writeConflictTotal;
            continue;  // INV-A1 still holds: writeTextFileAtomic is not called for this record
        }

        // INV-A1/AC-24: the ONE call site in the whole asset flow.
        const std::string error = writeTextFileAtomic(rootUtf8 + '/' + metaRelPath, writeMetaText(record.guid));
        if (!error.empty()) {
            // A write failure does NOT abort the scan and removes nothing: the record keeps its
            // in-memory GUID (the editor stays usable) and the next scan retries.
            appendCapped(report.writeFailures, record.relativePath + ": " + error);
            ++report.writeFailureTotal;
        }
    }

    byGuid.reserve(records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
        const AssetRecord& record = records[index];
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
    // FIRST claimant by path, the same "first wins" rule std::unordered_map::emplace's silent-no-op-on
    // -duplicate-key behaviour gave this method before the MSVC nothrow-move fallback replaced it.
    std::stable_sort(
        byGuid.begin(), byGuid.end(),
        [](const std::pair<Guid, std::size_t>& a, const std::pair<Guid, std::size_t>& b) { return a.first < b.first; });

    return report;
}

}  // namespace engine::editor

// Aero Engine — the on-demand model import driver's impure half (task 3.2.1). ImGui-free, entt-free,
// SDL-free, fastgltf-free and <filesystem>-free at source (AC-57) -- every byte this file touches goes
// through text_file.hpp's readFileBytes/writeTextFileAtomic, which own the platform's actual file I/O.
// NOTHING HERE LOGS.
//
// Deviation from the plan's own §D-7 text, logged (the identical deviation phase 7.5 in
// asset_database.cpp already made, for the identical reason): `leafOf`/`parentDirOf` are NOT new
// TU-local helpers -- project_files.hpp (already included transitively through asset_database.hpp)
// already declares BOTH publicly with exactly the semantics needed ("parentDirOf" is `parentOf`
// verbatim: "" for a root-level file, no trailing slash). Reused directly rather than shadowed.
#include <aero/editor/model_import_session.hpp>
#include <aero/editor/text_file.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// NIT 11 (code review): ImportedImage::guid's own doc comment promises "nil unless relativePath names
// a known asset", but nothing assigned it -- importModel() itself MUST stay pure (bytes in, value out,
// no database access), so the assignment happens HERE, the one place that holds both a fresh
// ImportResult and a `const AssetDatabase&` at the same time. Applied to `resultValue.model.images`
// after EVERY import call that can leave it non-empty (never after a FAILED import, whose `model` is
// contractually empty already, so there is nothing to walk).
void assignImageGuids(std::vector<ImportedImage>& images, const AssetDatabase& database) {
    for (ImportedImage& image : images) {
        if (image.relativePath.empty()) {
            continue;  // unresolved or embedded (D14) -- guid stays nil, exactly as documented
        }
        if (const std::optional<Guid> guid = database.guidForPath(image.relativePath); guid.has_value()) {
            image.guid = *guid;
        }
    }
}

// task 3.2.4 (§A-20 j): the settings fingerprint recorded in, and compared against, the provenance
// record. A PURE function of ImportSettings ALONE -- a nil Guid and writeMetaText's DEFAULT identity
// parameters, so neither the asset's own GUID nor its importer name leaks into the value. Reusing
// writeMetaText's serializer is the whole point: a future ImportSettings field enters the fingerprint
// automatically, which is the only reason the fingerprint exists at all (3.2.1 applies `scale` during
// IMPORT, and what is cached here is the GLB, so strictly nothing today needs it -- it is here so a
// future Blender-SIDE option cannot be added without the invalidation already in place).
[[nodiscard]] std::string fingerprintOf(const ImportSettings& settings) {
    const std::string text = writeMetaText(Guid{}, settings);
    return formatContentHash(hashBytes(std::as_bytes(std::span<const char>(text))));
}

// task 3.2.4: the record's contentHash is MEANINGFUL only under 3.1.3's ThumbnailKey rule, applied
// verbatim one subsystem over. An unhashed record has no cache key, exactly as an unhashed record has
// no thumbnail key -- so the artifact is treated as stale and NO provenance is written after the run.
// The model is still shown; it is simply not cached.
[[nodiscard]] bool sourceHashUsable(const AssetRecord& record) noexcept {
    return !record.metaWriteFailed && record.change != ImportChange::Unhashable &&
           record.change != ImportChange::NotHashed;
}

// The importers' own addWarning discipline (gltf_import.cpp:41 / fbx_import.cpp:242), restated here
// because this is the ONE warning the SESSION appends rather than an importer: warningTotal is
// UNCAPPED and `warnings` stops at MAX_IMPORT_WARNINGS, so the panel's "... and N more" tail stays
// honest.
void addSessionWarning(ImportResult& result, std::string text) {
    ++result.warningTotal;
    if (result.warnings.size() < MAX_IMPORT_WARNINGS) {
        result.warnings.push_back(std::move(text));
    }
}

}  // namespace

void ModelImportSession::setTarget(std::string relativePath, std::uint64_t databaseGeneration) {
    // E18: the SAME target and the SAME generation is a no-op -- idempotent by construction, so a
    // double reconcile in one tick cannot re-import.
    if (relativePath == targetPath && databaseGeneration == generationValue) {
        return;
    }
    // code-review SHOULD-FIX 4/5: computed BEFORE targetPath is overwritten below -- TRUE only when
    // the actual PATH is changing, never merely because the generation moved.
    const bool targetChanged = relativePath != targetPath;
    targetPath = std::move(relativePath);
    generationValue = databaseGeneration;
    serviced = false;
    resultValue = ImportResult{};
    lastApplyError.clear();
    observedSize = 0;
    if (targetChanged) {
        // task 3.2.4 code-review B2: the STATE belongs to the target that produced it, and every other
        // field here is already reset for exactly that reason. A .blend conversion is the first thing in
        // this class that SPANS FRAMES, so a stale SessionState::Converting survived a selection change
        // and was then read as an input: the panel drew "Running Blender... 42.3 s" against the newly
        // selected asset, and the run's completion was consumed on the NEW target's behalf -- importing
        // its artifact and writing a provenance record for an export that never ran for it. service()
        // sets the real state immediately below; the one frame between (the panel draws BEFORE service()
        // in a tick) now reads Idle, which is true, rather than the previous target's status.
        stateValue = SessionState::Idle;
        // SHOULD-FIX 5: editor_app.cpp's Apply drain (:596-621) runs BETWEEN this call (:589) and the
        // service() call that would otherwise re-resolve targetGuid (:692, post-draw). Leaving
        // targetGuid holding the PREVIOUS target's identity across that window means an Apply landing
        // in the same tick as a selection change writes the previous asset's GUID into the NEW
        // target's sidecar -- invalidate it NOW, synchronously, so canApply()/applySettings() correctly
        // refuse until service() resolves the new target's real identity.
        targetGuid = Guid{};
        // SHOULD-FIX 4 (recordless half): reset the form to defaults immediately, so neither that same
        // window nor a service() call against a target with no AssetDatabase record ever shows -- or
        // saves -- the PREVIOUS asset's settings. service() below overwrites both fields with the new
        // target's real on-disk values, when a record exists.
        pending = ImportSettings{};
        onDisk = ImportSettings{};
    }
    // else: the SAME target, only the generation moved (e.g. an unrelated file's rescan elsewhere in
    // the project) -- SHOULD-FIX 4 (dirty-edit half): preserve any unapplied edit. service() below must
    // not resync the form in this case, or a drag-in-progress on THIS asset would be silently discarded.
    formNeedsResync = targetChanged;
}

void ModelImportSession::requestConversion() noexcept {
    conversionRequested = true;
    // RE-ARM the consume guard. The request necessarily arrives long after this (target, generation)
    // pair was serviced, so without this the early return below would swallow it forever. Narrow on
    // purpose: it re-arms exactly one tick's worth of work, and the arm itself skips the cache probe
    // while a request is pending, which is what makes `Re-import` re-convert rather than re-hit.
    serviced = false;
}

void ModelImportSession::cancelConversion() noexcept {
    cancelRequested = true;
    serviced = false;
}

void ModelImportSession::service(std::string_view assetsRootUtf8, const AssetDatabase& database, float deltaSeconds) {
    if (serviced) {
        // task 3.2.4: the ONE exception to the (target, generation) consume guard. A .blend
        // conversion -- and the version probe that precedes it -- spans frames, so the arm must be
        // re-entered while either is in flight or nothing would ever poll them. poll() has exactly one
        // call site in the whole tree (AC-38), and it is inside serviceBlend(), so "the guard let us
        // back in" and "the child gets waited on" are the same statement.
        //
        // NARROW ON PURPOSE, on BOTH axes: it applies only to a .blend TARGET, so a .gltf selected
        // while some other probe is alive is not re-imported every tick; and only while something is
        // genuinely running, so a .blend that is Imported, NeedsConversion or ConversionFailed takes
        // the existing early return unchanged. A cache hit still costs ONE import and ten further
        // ticks still cost ten early returns (AC-45's property, preserved).
        const BlenderState blenderState = blenderService.state();
        const bool blendWorkInFlight =
            isBlendFileName(leafOf(targetPath)) &&
            (stateValue == SessionState::Converting || blenderState == BlenderState::Probing ||
             blenderState == BlenderState::Converting);
        if (!blendWorkInFlight) {
            return;  // AC-45: already imported at this (target, generation). STRUCTURAL, not conventional.
        }
    }
    serviced = true;
    // code-review SHOULD-FIX 4: consumed ONCE, right here -- true only when setTarget() last saw the
    // TARGET PATH change, never merely the generation. Everything below that would otherwise
    // unconditionally overwrite pending/onDisk from disk is now gated on it, so a re-service of the
    // SAME target (an unrelated file's rescan) preserves an in-progress, unapplied edit.
    const bool resyncForm = formNeedsResync;
    formNeedsResync = false;
    resultValue = ImportResult{};
    lastApplyError.clear();
    observedSize = 0;
    targetGuid = Guid{};
    if (targetPath.empty()) {
        stateValue = SessionState::Idle;
        return;  // AC-46; setTarget() already reset pending/onDisk to defaults when the path changed
    }
    const std::string_view leaf = leafOf(targetPath);
    // task 3.2.4: the .blend arm sits BEFORE the isImportableModelName early return, and the order is
    // load-bearing -- placed after it, the whole arm would be dead code, because .blend is deliberately
    // NOT in that predicate's table and never will be (D15).
    if (isBlendFileName(leaf)) {
        serviceBlend(assetsRootUtf8, database, deltaSeconds, resyncForm);
        return;
    }
    if (!isImportableModelName(leaf)) {
        stateValue = SessionState::NotImportable;
        return;  // AC-46/E17 -- and NOTHING was read; ditto
    }

    // Identity and the on-disk settings come from the database's OWN PARSED RECORD -- never re-parsed
    // here. A record with an invalid .meta still IMPORTS (import needs BYTES, not identity, E16); it
    // simply cannot Apply.
    if (const AssetRecord* const record = database.findByPath(targetPath); record != nullptr) {
        targetGuid = record->guid;
        if (resyncForm) {
            onDisk = record->importSettings;
            pending = onDisk;
        }
    }
    // else: SHOULD-FIX 4 (recordless half) -- targetGuid stays nil (set above). pending/onDisk stay at
    // whatever setTarget() left them: ImportSettings{} for a genuinely NEW, recordless target
    // (targetChanged was true, so setTarget() already reset both), or the SAME values as before for a
    // re-service of the SAME target whose record just vanished (targetChanged was false) -- never a
    // DIFFERENT, previously selected asset's settings either way.

    const std::string modelPath = std::string(assetsRootUtf8) + '/' + targetPath;
    const FileBytesResult modelBytes = readFileBytes(modelPath, MAX_MODEL_FILE_BYTES);
    observedSize = modelBytes.size;  // filled EVEN ON REFUSAL (E20/AC-43)
    if (!modelBytes.bytes.has_value()) {
        stateValue = SessionState::Failed;
        resultValue.status = ImportStatus::ParseFailed;
        resultValue.message = modelBytes.refusedByCap
                                  ? std::format("the file is {} bytes, above the {}-byte import limit", modelBytes.size,
                                                MAX_MODEL_FILE_BYTES)
                                  : modelBytes.error;  // E15: deleted between the reconcile and here
        ++imports;
        return;
    }
    const std::string dir = parentOf(targetPath);
    const std::span<const std::byte> span(reinterpret_cast<const std::byte*>(modelBytes.bytes->data()),
                                          modelBytes.bytes->size());

    // D5: FBX has no external geometry buffers, so the Structure pass has nothing to tell us and the
    // texture reads it would drive are pure waste -- worse, E21 ("a partial Full is never shown")
    // would let a MISSING TEXTURE block an import whose geometry was in the file all along (AC-57).
    // ONE PURE PREDICATE, BOTH ARMS TESTED (MS25/MS27). glTF: unchanged, two passes, one Structure
    // parse plus every external buffer. FBX: exactly one importModel call at Full depth, exactly one
    // file read (the .fbx itself), and no texture-read failure path at all.
    //
    // Verified safe: pass 1's ONLY products in this function are `externals` and the E21 fallback
    // result -- nothing else reads `structure`.
    std::vector<ExternalBuffer> externals;
    if (modelImporterNeedsExternalBuffers(leaf)) {
        // PASS 1 -- Structure, to learn the URI set. Cheap: one simdjson parse, no accessor touched.
        const ImportResult structure = importModel(leaf, dir, span, pending, ImportDepth::Structure, {});
        if (structure.status != ImportStatus::Ok && structure.status != ImportStatus::Truncated) {
            resultValue = structure;
            stateValue = SessionState::Failed;
            ++imports;
            return;
        }
        // Load EXACTLY what pass 1 named, through the editor's own capped byte primitive. Never a path
        // the DOCUMENT chose: every entry of externalUris has already been through classifyUri (AC-39).
        externals.reserve(structure.externalUris.size());
        std::uint64_t total = 0;
        bool overBudget = false;
        for (const std::string& rel : structure.externalUris) {
            const std::string path = std::string(assetsRootUtf8) + '/' + rel;
            FileBytesResult buf = readFileBytes(path, MAX_EXTERNAL_BYTES_PER_MODEL);
            if (!buf.bytes.has_value()) {
                continue;  // an unreadable buffer becomes MissingBuffer in pass 2, via the adapter
            }
            if (buf.bytes->size() > MAX_EXTERNAL_BYTES_PER_MODEL - total) {
                overBudget = true;
                break;
            }
            total += buf.bytes->size();
            externals.push_back(ExternalBuffer{rel, std::move(*buf.bytes)});
        }
        if (overBudget) {
            // E21: A PARTIAL FULL IS NEVER SHOWN. Keep the Structure result and say why.
            resultValue = structure;
            resultValue.status = ImportStatus::Truncated;
            resultValue.message =
                "the external buffers exceed this importer's per-model limit; showing the "
                "document's structure only";
            stateValue = SessionState::Imported;
            assignImageGuids(resultValue.model.images, database);  // NIT 11
            ++imports;
            return;
        }
    }

    // PASS 2 -- Full. For FBX this is the ONLY pass and `externals` is empty. D16: SYNCHRONOUS; a large
    // model WILL visibly hitch, and that is accepted -- it is what every editor in this class does on a
    // deliberate click, and MAX_MODEL_FILE_BYTES refuses the pathological case outright rather than
    // freezing.
    resultValue = importModel(leaf, dir, span, pending, ImportDepth::Full, externals);
    stateValue = (resultValue.status == ImportStatus::Ok || resultValue.status == ImportStatus::Truncated)
                     ? SessionState::Imported
                     : SessionState::Failed;
    assignImageGuids(resultValue.model.images, database);  // NIT 11 -- database access stops HERE;
                                                           // importModel() itself never sees one
    ++imports;
    // `modelBytes` outlives every use above. GltfDataBuffer::FromBytes COPIES (§G-16 item 1), so there
    // is no lifetime contract on these bytes -- but keeping the named local alive across both passes
    // costs nothing and survives a future switch to a BORROWING FromSpan.
}

void ModelImportSession::serviceBlend(std::string_view assetsRootUtf8, const AssetDatabase& database,
                                      float deltaSeconds, bool resyncForm) {
    // ---- 1. identity, from the database's OWN PARSED RECORD -- never re-parsed here ---------------
    const AssetRecord* const record = database.findByPath(targetPath);
    bool hashUsable = false;
    if (record != nullptr) {
        targetGuid = record->guid;
        if (resyncForm) {
            onDisk = record->importSettings;
            pending = onDisk;
        }
        hashUsable = sourceHashUsable(*record);
    }

    // ---- 2. AC-27: a nil GUID is NeedsConversion with NO identity, and NOTHING ELSE HAPPENS -------
    // Not NotImportable: that enumerator's panel branch renders one sentence and returns before any
    // section, so it would draw no button to disable -- and it would tell the user "no importer claims
    // this file type", which is false for a .blend after this task.
    if (!targetGuid.valid()) {
        stateValue = SessionState::NeedsConversion;
        conversionRequested = false;  // a request against an identity-less asset is dropped, not queued
        cancelRequested = false;
        return;  // no read, no resolve, no spawn, no byte written
    }

    // ---- 3/4. the derived-data paths, and what a still-valid record must say ----------------------
    const std::string guidText = formatGuid(targetGuid);  // returns BY VALUE -- named local first
    const std::string libraryDir = std::string(database.projectRoot()) + '/' + std::string(ASSET_CACHE_DIR_NAME) + '/' +
                                   std::string(BLENDER_EXPORT_DIR_NAME);
    // Named locals FIRST, always (AC-25): the provenance write below is one of the task's writes built
    // from the LIBRARY directory, and the invariant stays grep-decidable because of it.
    const std::string provenancePath = libraryDir + '/' + guidText + ".json";
    const std::string artifactPath = libraryDir + '/' + guidText + ".glb";
    const std::string artifactLeaf = guidText + ".glb";

    ExportProvenance expected;
    expected.guid = targetGuid;
    expected.sourcePath = targetPath;
    expected.sourceHash = record != nullptr ? record->contentHash : ContentHash{};
    // "" until something in this session has probed -- and an EMPTY expected version is not compared at
    // all (§A-9). That is exactly what makes a pure cache hit spawn nothing: comparing a version you
    // have not probed would require probing, and a probe is a process.
    expected.blenderVersion = blenderService.versionString();
    expected.scriptVersion = BLENDER_SCRIPT_VERSION;
    expected.settingsFingerprint = fingerprintOf(pending);

    // Read <guid>.glb and import it. AC-44/§A-2b, and this is the single most important paragraph in
    // the whole arm.
    const auto importArtifact = [&]() -> bool {
        ++imports;  // an ATTEMPT, exactly as the model path counts one on every terminal branch
        const FileBytesResult artifactBytes = readFileBytes(artifactPath, MAX_ARTIFACT_BYTES);
        observedSize = artifactBytes.size;  // filled EVEN ON REFUSAL (E12/AC-35's shape)
        if (!artifactBytes.bytes.has_value()) {
            resultValue = ImportResult{};
            resultValue.status = ImportStatus::ParseFailed;
            resultValue.message = artifactBytes.refusedByCap
                                      ? std::format("the Blender export is {} bytes, above the {}-byte limit",
                                                    artifactBytes.size, MAX_ARTIFACT_BYTES)
                                      : artifactBytes.error;
            return false;
        }
        const std::span<const std::byte> span(reinterpret_cast<const std::byte*>(artifactBytes.bytes->data()),
                                              artifactBytes.bytes->size());
        // ONE call. Full depth. EMPTY external span. EMPTY assetRelativeDir.
        //
        // modelImporterNeedsExternalBuffers is DELIBERATELY NOT CONSULTED here, and that is not an
        // oversight. It would answer TRUE for a ".glb", and running the two-pass driver would be:
        //   (a) pointless -- D3 guarantees Blender's GLB embeds its images as buffer views, so there is
        //       nothing external to fetch;
        //   (b) WRONG -- this file lives in Library/ and has NO assets-relative directory at all, so
        //       any relative URI would be resolved against a completely unrelated tree, naming a file
        //       the artifact has nothing to do with; and
        //   (c) a budget hazard -- the session would read those bytes and hand them to a Full pass that
        //       never needed them, with MAX_EXTERNAL_BYTES_PER_MODEL and its Truncated escalation in
        //       the path, which is exactly the failure 3.2.3 documented one format over.
        // artifactLeaf is a real ".glb" name, so importModel's own dispatch routes it to the glTF
        // backend exactly as any other GLB.
        resultValue = importModel(artifactLeaf, /*assetRelativeDir=*/"", span, pending, ImportDepth::Full, {});
        if (resultValue.status != ImportStatus::Ok && resultValue.status != ImportStatus::Truncated) {
            return false;
        }
        // "A GLB is self-contained" is a property of Blender's EXPORTER, not of the glTF
        // specification: a GLB may legally carry external URIs. Without this, such an artifact would
        // import with unresolved images, status Ok and NO SIGNAL AT ALL. Make the assumption
        // OBSERVABLE, and clear the list so nothing downstream mistakes those names for
        // assets-relative paths.
        if (!resultValue.externalUris.empty()) {
            addSessionWarning(resultValue,
                              std::format("the Blender export references {} external file(s); a GLB is expected to "
                                          "be self-contained, so they were not loaded",
                                          resultValue.externalUris.size()));
            resultValue.externalUris.clear();
        }
        assignImageGuids(resultValue.model.images, database);  // 3.2.1's NIT-11, reused unchanged
        return true;
    };

    // ---- 5. drive the service, and hand it this tick's delta --------------------------------------
    if (cancelRequested) {
        cancelRequested = false;
        blenderService.cancel();
    }
    const BlenderState blenderBefore = blenderService.state();
    if (stateValue == SessionState::Converting || blenderBefore == BlenderState::Probing ||
        blenderBefore == BlenderState::Converting) {
        // THE ONE AND ONLY BlenderService::poll() CALL SITE IN THE TREE (AC-38/INV-B15). It runs from
        // EditorApp::tick()'s post-draw slot, through service(), and NEVER from a panel's onDraw().
        blenderService.poll(deltaSeconds);
    }

    // code-review B2, the second half: the service holds AT MOST ONE run (INV-B5), and that run belongs
    // to whichever asset requested it -- never necessarily to the one selected now. Comparing the
    // service's own target GUID against ours is what makes "this result is mine" a fact rather than an
    // assumption; setTarget()'s state reset above makes the mismatch unreachable, and this makes it
    // harmless if a future path ever reaches it again. A mismatch falls through to the ordinary cache
    // probe below, which is exactly what the newly selected asset needs.
    if (stateValue == SessionState::Converting && blenderService.conversionGuid() == targetGuid) {
        switch (blenderService.state()) {
            case BlenderState::Ready:       // the request has not been drained into a spawn yet
            case BlenderState::Converting:  // the child is alive
                return;
            case BlenderState::Converted:
                if (importArtifact()) {
                    // AC-24/INV-B11: the provenance record is written ONLY NOW -- after the artifact
                    // has been read back AND imported successfully. A killed, timed-out, failed or
                    // unusable run leaves any previous record untouched.
                    if (hashUsable) {
                        expected.blenderPath = blenderService.binaryPath();
                        expected.blenderVersion = blenderService.versionString();
                        expected.artifactBytes = observedSize;
                        if (const std::string writeError =
                                writeTextFileAtomic(provenancePath, writeExportProvenanceText(expected));
                            !writeError.empty()) {
                            // The MODEL is fine and is shown; only the CACHE is lost, so the next
                            // selection re-converts. Surfaced as a warning rather than a log line --
                            // this TU never logs.
                            addSessionWarning(resultValue,
                                              std::format("the conversion succeeded but its cache record could not "
                                                          "be written to '{}' -- {}",
                                                          provenancePath, writeError));
                        }
                    }
                    // else: an unhashed record has no cache key at all (3.1.3's ThumbnailKey rule), so
                    // the model is SHOWN and deliberately NOT cached.
                    stateValue = SessionState::Imported;
                } else {
                    // AC-34: the SESSION composes the message, because only the session imports, and
                    // hands it back so the service can reach Failed/ArtifactUnusable.
                    blenderService.noteArtifactUnusable(
                        std::format("{}: {}", importStatusLabel(resultValue.status), resultValue.message));
                    stateValue = SessionState::ConversionFailed;
                }
                return;
            case BlenderState::Failed:
                stateValue = SessionState::ConversionFailed;
                return;
            case BlenderState::Unknown:
            case BlenderState::ToolMissing:
            case BlenderState::ToolUnusable:
            case BlenderState::Probing:
                // The tool was re-detected or replaced mid-run (setOverridePath resets to Unknown and
                // kills the child), so the run is gone with it. Offer a conversion again rather than
                // reporting a failure the user already caused deliberately.
                stateValue = SessionState::NeedsConversion;
                return;
        }
        return;  // unreachable -- every BlenderState is handled above (no `default:`, so a new
                 // enumerator is a -Wswitch warning)
    }

    // ---- 6. the cache probe. THE COMMON CASE, and the one that must cost nothing (AC-22) ----------
    // Skipped entirely while a conversion is pending (`Re-import` must re-convert, not re-hit), and
    // skipped when the record's own content hash is not meaningful: an unhashed record has NO CACHE KEY
    // at all, so the artifact beside it is treated as STALE rather than compared against a hash that
    // means nothing (3.1.3's ThumbnailKey rule, applied one subsystem over).
    if (!conversionRequested && hashUsable) {
        if (const FileReadResult provenanceText = readTextFile(provenancePath); provenanceText.text.has_value()) {
            const std::optional<ExportProvenance> actual = parseExportProvenance(*provenanceText.text);
            if (actual.has_value() && provenanceMatches(*actual, expected) && importArtifact()) {
                stateValue = SessionState::Imported;
                return;  // ZERO processes, ZERO bytes written, no environment read, no path stat'ed
            }
            // A record that does not parse, does not match, or whose artifact is missing/unusable is a
            // MISS -- 3.1.2 D7's "derived data is disposable", never a repair. E10/E11.
            resultValue = ImportResult{};
        }
    }

    // ---- 7/8. a miss. Convert if asked to, otherwise wait to be asked -----------------------------
    if (conversionRequested) {
        conversionRequested = false;
        const BlenderState blenderState = blenderService.state();
        const bool convertible = blenderState == BlenderState::Ready || blenderState == BlenderState::Converted ||
                                 blenderState == BlenderState::Failed;
        if (convertible) {
            const std::string blendAbsolute = std::string(assetsRootUtf8) + '/' + targetPath;
            blenderService.requestConversion(targetGuid, blendAbsolute, expected.sourceHash, libraryDir);
            stateValue = SessionState::Converting;
            return;  // the next tick's poll() drains the request and performs the ONE spawn
        }
        // AC-30: with no usable Blender the request is DROPPED and nothing is spawned. The panel's own
        // section already says why (ToolMissing / ToolUnusable / Probing).
    }
    stateValue = SessionState::NeedsConversion;
}

std::string ModelImportSession::applySettings(std::string_view assetsRootUtf8) {
    lastApplyError.clear();
    if (!targetGuid.valid()) {  // E16
        lastApplyError = "this asset has no valid .meta identity; settings cannot be saved";
        return lastApplyError;
    }
    if (!settingsDirty()) {
        // code-review SHOULD-FIX 5: enforce the FULL canApply() condition here too, not just the GUID
        // half -- a hook-driven Apply (EditorApp::requestModelImportApply(), which bypasses the panel's
        // own BeginDisabled(!canApply())) must not rewrite a byte-identical sidecar: that would dirty
        // its mtime and cost a watcher trigger plus a rescan for nothing. Not writing is not a failure.
        return lastApplyError;  // ""
    }
    const std::string metaPath = std::string(assetsRootUtf8) + '/' + targetPath + std::string(ASSET_META_SUFFIX);
    // task 3.2.2: the THIRD hard-coded-identity site (asset_meta.cpp's own writeMetaText used to write
    // GLTF_IMPORTER_NAME/VERSION unconditionally) is closed HERE, the one call site that can ever write
    // a non-default settings block. modelImporterIdentity(leafOf(targetPath)) -- the SAME per-format
    // pure function planImports and phase 7.5 both use -- so an .fbx's sidecar records "fbx", never a
    // borrowed "gltf" (AC-17, MS29).
    const ImporterIdentity identity = modelImporterIdentity(leafOf(targetPath));
    const std::string text =
        writeMetaText(targetGuid, pending, identity.name, identity.version);  // D7's omit-when-default
                                                                              // lives there
    lastApplyError = writeTextFileAtomic(metaPath, text);                     // .aero-tmp + rename (2.5.1)
    if (lastApplyError.empty()) {
        onDisk = pending;  // the form matches disk again; Apply disables itself (AC-51)
    }
    return lastApplyError;  // "" == success
}

}  // namespace engine::editor

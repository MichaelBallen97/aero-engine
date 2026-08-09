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

void ModelImportSession::service(std::string_view assetsRootUtf8, const AssetDatabase& database) {
    if (serviced) {
        return;  // AC-45: already imported at this (target, generation). STRUCTURAL, not conventional.
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

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
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

void ModelImportSession::setTarget(std::string relativePath, std::uint64_t databaseGeneration) {
    // E18: the SAME target and the SAME generation is a no-op -- idempotent by construction, so a
    // double reconcile in one tick cannot re-import.
    if (relativePath == targetPath && databaseGeneration == generationValue) {
        return;
    }
    targetPath = std::move(relativePath);
    generationValue = databaseGeneration;
    serviced = false;
    resultValue = ImportResult{};
    lastApplyError.clear();
    observedSize = 0;
}

void ModelImportSession::service(std::string_view assetsRootUtf8, const AssetDatabase& database) {
    if (serviced) {
        return;  // AC-45: already imported at this (target, generation). STRUCTURAL, not conventional.
    }
    serviced = true;
    resultValue = ImportResult{};
    lastApplyError.clear();
    observedSize = 0;
    targetGuid = Guid{};
    if (targetPath.empty()) {
        stateValue = SessionState::Idle;
        return;  // AC-46
    }
    const std::string_view leaf = leafOf(targetPath);
    if (!isImportableModelName(leaf)) {
        stateValue = SessionState::NotImportable;
        return;  // AC-46/E17 -- and NOTHING was read
    }

    // Identity and the on-disk settings come from the database's OWN PARSED RECORD -- never re-parsed
    // here. A record with an invalid .meta still IMPORTS (import needs BYTES, not identity, E16); it
    // simply cannot Apply.
    if (const AssetRecord* const record = database.findByPath(targetPath); record != nullptr) {
        targetGuid = record->guid;
        onDisk = record->importSettings;
        pending = onDisk;
    }

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

    // PASS 1 -- Structure, to learn the URI set. Cheap: one simdjson parse, no accessor touched.
    const ImportResult structure = importModel(leaf, dir, span, pending, ImportDepth::Structure, {});
    if (structure.status != ImportStatus::Ok && structure.status != ImportStatus::Truncated) {
        resultValue = structure;
        stateValue = SessionState::Failed;
        ++imports;
        return;
    }
    // Load EXACTLY what pass 1 named, through the editor's own capped byte primitive. Never a path the
    // DOCUMENT chose: every entry of externalUris has already been through classifyUri (AC-39).
    std::vector<ExternalBuffer> externals;
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
        ++imports;
        return;
    }

    // PASS 2 -- Full. D16: SYNCHRONOUS; a large model WILL visibly hitch, and that is accepted -- it is
    // what every editor in this class does on a deliberate click, and MAX_MODEL_FILE_BYTES refuses the
    // pathological case outright rather than freezing.
    resultValue = importModel(leaf, dir, span, pending, ImportDepth::Full, externals);
    stateValue = (resultValue.status == ImportStatus::Ok || resultValue.status == ImportStatus::Truncated)
                     ? SessionState::Imported
                     : SessionState::Failed;
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
    const std::string metaPath = std::string(assetsRootUtf8) + '/' + targetPath + std::string(ASSET_META_SUFFIX);
    const std::string text = writeMetaText(targetGuid, pending);  // D7's omit-when-default lives there
    lastApplyError = writeTextFileAtomic(metaPath, text);         // .aero-tmp + rename (2.5.1)
    if (lastApplyError.empty()) {
        onDisk = pending;  // the form matches disk again; Apply disables itself (AC-51)
    }
    return lastApplyError;  // "" == success
}

}  // namespace engine::editor

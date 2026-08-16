// Aero Engine — the material edit session (task 3.4.2). GPU-free, ImGui-free, and the ONE place in
// this tree that writes .aeromat bytes (D12). Everything here is driven from EditorApp::tick(); no
// panel reaches any of it.
//
// THE WRITE-PATH INVARIANT, stated where it lives: this TU holds EXACTLY ONE writeTextFileAtomic call
// site, inside saveMaterialFile below, and both logical writes -- Apply here, New Material in
// editor_app.cpp -- route through that one function with the absolute path assembled into a named
// local first. That is what keeps the amended INV-A1 a grep rather than a heuristic. This file is in
// NEITHER of check-project-no-delete.sh's lists, and being outside both is exactly what makes a future
// std::filesystem::remove here a hard CI failure.
#include <aero/editor/asset_database.hpp>
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/asset_view.hpp>
#include <aero/editor/material_session.hpp>
#include <aero/editor/project_files.hpp>
#include <aero/editor/text_file.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// Assembled ONCE, here, so every caller of the two disk operations spells the same thing.
[[nodiscard]] std::string materialAbsolutePathFor(std::string_view assetsRootAbs, std::string_view relativePath) {
    std::string absolute(assetsRootAbs);
    if (!absolute.empty() && absolute.back() != '/') {
        absolute += '/';
    }
    absolute += relativePath;
    return absolute;
}

}  // namespace

std::string saveMaterialFile(std::string_view absolutePathUtf8, const MaterialDocument& document) {
    const std::string text = writeMaterialText(document);
    return writeTextFileAtomic(absolutePathUtf8, text);  // "" == success
}

bool isMaterialFileName(std::string_view relativePath) noexcept {
    return classifyAssetKind(leafOf(relativePath), false) == AssetKind::Material;
}

void MaterialSession::clear() noexcept {
    targetPathValue.clear();
    targetGeneration = 0;
    lastKnownContentHash.reset();
    fileCopy.reset();
    sessionCopy.reset();
    parseError.reset();
    parseWarnings.clear();
    message.clear();
    externalChange = false;
    applyRequested = false;
    revertRequested = false;
    documentChanged = false;
    // `writes` is DELIBERATELY not reset. It is not session state, it is a lifetime counter: its whole
    // job is to make "Apply on a clean session wrote nothing" distinguishable from "Apply did nothing
    // observable", and a counter that a retarget silently rewinds cannot do that across a case that
    // targets two materials.
}

void MaterialSession::loadTarget(std::string_view assetsRootAbs) {
    fileCopy.reset();
    sessionCopy.reset();
    parseError.reset();
    parseWarnings.clear();
    externalChange = false;

    const std::string materialPath = materialAbsolutePathFor(assetsRootAbs, targetPathValue);
    const FileReadResult read = readTextFile(materialPath);
    if (!read.text.has_value()) {
        // Line/column stay zero: nothing reached the JSON stage, so there is no position to report
        // (material_format.hpp's own `line > 0` contract).
        parseError = MaterialError{.message = read.error};
        documentChanged = true;
        return;
    }
    MaterialParseResult parsed = parseMaterial(*read.text);
    // The `|| !has_value()` half is NOT defensive programming: bugprone-unchecked-optional-access is
    // --warnings-as-errors on the Linux Debug lane and cannot correlate ok() with the engagement of a
    // separate member (editor_app.cpp's logScope carries the identical note for the identical reason).
    if (!parsed.ok() || !parsed.document.has_value()) {
        parseError = std::move(parsed.error);
        documentChanged = true;
        return;
    }
    MaterialDocument loaded = std::move(*parsed.document);
    // The AGGREGATE non-canonical notice (see the header's recorded deviation). Decided exactly, by
    // comparing bytes with the canonical writer's own output -- no key vocabulary is duplicated here,
    // and the parser's per-key WARNs are already in the Console.
    const bool isCanonical = writeMaterialText(loaded) == *read.text;
    fileCopy = loaded;
    sessionCopy = std::move(loaded);
    if (!isCanonical) {
        parseWarnings.emplace_back("this file is not in canonical form; Apply rewrites it and drops any unknown keys");
    }
    documentChanged = true;
}

void MaterialSession::reconcile(std::string_view selection, std::uint64_t generation, const AssetDatabase& database,
                                std::string_view assetsRootAbs) {
    // D3, arm 1: a DIFFERENT, EXISTING *.aeromat retargets, and nothing else does. The `!=` is what
    // makes re-selecting the same path a no-op rather than a reload that discards the edits.
    if (isMaterialFileName(selection) && selection != targetPathValue && database.findByPath(selection) != nullptr) {
        clear();
        targetPathValue = selection;
        targetGeneration = generation;
        loadTarget(assetsRootAbs);
        const AssetRecord* record = database.findByPath(targetPathValue);
        if (record != nullptr) {
            lastKnownContentHash = record->contentHash;
        }
        return;
    }
    // D3, arm 2: ANY other selection -- another kind, a folder, nothing at all -- leaves the target
    // exactly where it is. Import Details retargets on everything; the two panels are tabs, not
    // shared state, and inverting this makes every texture-picking click destroy an edit session.
    if (targetPathValue.empty() || generation == targetGeneration) {
        return;
    }
    targetGeneration = generation;

    // D10: a rescan happened. Re-validate what we are holding.
    const AssetRecord* record = database.findByPath(targetPathValue);
    if (record == nullptr) {
        // Deleted, or renamed -- a rename is a delete plus an add at the database layer. Unapplied
        // edits are LOST, recorded as accepted: re-attaching across a rename needs GUID-keyed
        // targeting, a named v1 non-goal.
        const std::string gone = "'" + targetPathValue + "' is no longer in the project.";
        clear();
        message = gone;
        documentChanged = true;
        return;
    }
    if (!lastKnownContentHash.has_value()) {
        lastKnownContentHash = record->contentHash;  // first sighting -- adopt, never a notice
        return;
    }
    if (record->contentHash == *lastKnownContentHash) {
        return;
    }
    lastKnownContentHash = record->contentHash;
    if (dirty()) {
        // Last writer wins, stated: the edits stay, the notice appears, and Apply still overwrites.
        externalChange = true;
        message = "the file changed on disk; Apply will overwrite it";
        return;
    }
    // A clean session TRACKS the file -- 3.1.4's hot-reload promise applied to materials, silently.
    loadTarget(assetsRootAbs);
}

void MaterialSession::resetForProjectSwap() noexcept { clear(); }

void MaterialSession::edit(const MaterialDocument& document) {
    if (!sessionCopy.has_value() || parseError.has_value()) {
        return;  // nothing targeted, or an unreadable file -- there is no document to edit (AC-9)
    }
    if (*sessionCopy == document) {
        return;  // a drag frame that produced the same value costs no preview re-push
    }
    sessionCopy = document;
    documentChanged = true;
}

void MaterialSession::requestApply() noexcept { applyRequested = true; }
void MaterialSession::requestRevert() noexcept { revertRequested = true; }

void MaterialSession::service(const AssetDatabase& database, std::string_view assetsRootAbs) {
    (void)database;  // the database is a PARAMETER by rule (A-2), and the two operations below happen
                     // to need only the path; keeping it in the signature is what stops a future arm
                     // reaching for a member instead.
    // F9's rule: EVERY one-shot is drained as its OWN statement, unconditionally, BEFORE it is
    // inspected. A `applyRequested || revertRequested` expression would strand one of them.
    const bool doApply = applyRequested;
    applyRequested = false;
    const bool doRevert = revertRequested;
    revertRequested = false;

    if (doApply) {
        // The `|| !has_value()` half is the loadTarget note's twin: Ready already implies an engaged
        // sessionCopy, but bugprone-unchecked-optional-access cannot correlate the two.
        if (state() != MaterialSessionState::Ready || !sessionCopy.has_value()) {
            return;
        }
        const MaterialDocument& pending = *sessionCopy;
        if (!dirty()) {
            // The button is disabled when clean; this refuses REDUNDANTLY, which is S6's witness. A
            // no-op write would dirty the file's mtime and cost a watcher trigger plus a rescan for
            // nothing -- the applySettings rule, restated one format over.
            message = "nothing to apply";
            return;
        }
        if (const std::optional<MaterialError> invalid = validateMaterial(pending); invalid.has_value()) {
            // A validation failure changes NOTHING anywhere (INV-7): no write, no adoption, the
            // session copy intact so the user can fix the value that caused it. validateMaterial
            // reaches the arms JSON cannot spell -- a NaN factor is unspellable in a file and
            // trivially assignable in C++.
            message = "cannot apply -- " + invalid->message;
            return;
        }
        const std::string materialAbsolutePath = materialAbsolutePathFor(assetsRootAbs, targetPathValue);
        const std::string error = saveMaterialFile(materialAbsolutePath, pending);
        if (!error.empty()) {
            message = "could not save '" + targetPathValue + "' -- " + error;
            return;
        }
        fileCopy = sessionCopy;
        ++writes;
        externalChange = false;
        // The bytes on disk are ours now, so the rescan that follows must not read as an external
        // edit. Disengaging is what makes the next generation bump ADOPT the new hash silently.
        lastKnownContentHash.reset();
        parseWarnings.clear();  // whatever was non-canonical about the file is no longer true
        message = "saved '" + targetPathValue + "'";
        return;
    }
    if (doRevert) {
        if (targetPathValue.empty()) {
            return;
        }
        // AC-13: the file is re-read and re-parsed. One that became unreadable or invalid since load
        // degrades to the AC-9 error state with the session copy discarded -- never a half-load.
        loadTarget(assetsRootAbs);
        lastKnownContentHash.reset();
        message = parseError.has_value() ? "could not revert '" + targetPathValue + "'"
                                         : "reverted '" + targetPathValue + "'";
    }
}

bool MaterialSession::takeDocumentChanged() noexcept {
    const bool changed = documentChanged;
    documentChanged = false;
    return changed;
}

MaterialSessionState MaterialSession::state() const noexcept {
    if (targetPathValue.empty()) {
        return MaterialSessionState::Untargeted;
    }
    if (parseError.has_value() || !sessionCopy.has_value()) {
        return MaterialSessionState::Error;
    }
    return MaterialSessionState::Ready;
}

std::string_view MaterialSession::targetPath() const noexcept { return targetPathValue; }

const MaterialDocument* MaterialSession::document() const noexcept {
    return sessionCopy.has_value() ? &*sessionCopy : nullptr;
}

const MaterialDocument* MaterialSession::fileDocument() const noexcept {
    return fileCopy.has_value() ? &*fileCopy : nullptr;
}

bool MaterialSession::dirty() const noexcept {
    return fileCopy.has_value() && sessionCopy.has_value() && !(*sessionCopy == *fileCopy);
}

const MaterialError* MaterialSession::error() const noexcept { return parseError.has_value() ? &*parseError : nullptr; }

std::span<const std::string> MaterialSession::warnings() const noexcept { return parseWarnings; }

bool MaterialSession::externalChangeNoticed() const noexcept { return externalChange; }

std::string_view MaterialSession::lastMessage() const noexcept { return message; }

std::size_t MaterialSession::writeCount() const noexcept { return writes; }

}  // namespace engine::editor

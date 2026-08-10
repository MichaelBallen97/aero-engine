// Aero Engine — the Wavefront OBJ/MTL backend (task 3.2.3). THE ONLY tinyobjloader TRANSLATION UNIT IN
// THE TREE (INV-O1). This file grows across several steps; see docs/plans/3.2.3-obj-import-tinyobjloader.md
// §S for the sequence and docs/10-engineering-log.md's 3.2.3 entry for the finished design.
//
// Step 3 (this commit): the dispatch now routes here (model_import.cpp), and the ".obj" arm's Structure
// half is complete and final -- a PURE TEXT SCAN, a stated deviation from INV-M4 (D5): tinyobjloader is
// not entered at all. The ".mtl" arm still returns Unsupported; it lands at Step 7.
#include "obj_import.hpp"

// task 3.2.3 D21: the mapbox earcut triangulation path reads vertex positions guarded only by
// assert(), which vanishes under NDEBUG -- a Debug abort on the sanitiser lanes and a heap over-read in
// Release, both reachable from an ordinary broken .obj. The vcpkg port neither defines this macro nor
// installs the mapbox/ headers. If you are turning it on deliberately, replace this guard with our own
// bounds-checked triangulation first.
#ifdef TINYOBJLOADER_USE_MAPBOX_EARCUT
    #error \
        "task 3.2.3 D21: the mapbox earcut triangulation path reads vertex positions guarded only by \
assert(), which vanishes under NDEBUG -- a Debug abort on the sanitiser lanes and a heap over-read in \
Release, both reachable from an ordinary broken .obj. The vcpkg port neither defines this macro nor \
installs the mapbox/ headers. If you are turning it on deliberately, replace this guard with our own \
bounds-checked triangulation first."
#endif
// TINYOBJLOADER_IMPLEMENTATION must NEVER be defined anywhere in this tree (it is not defined below, or
// anywhere else): vcpkg already compiles the library into its own archive, and defining this macro here
// too would compile a second copy into this TU and risk an ODR conflict with the linked archive.
// clang-format sorts <tiny_obj_loader.h> alphabetically among the plain standard headers below (SAME
// IncludeCategories priority, no '/' in its own path) -- it is still the ONLY tinyobjloader include in
// this file, and the ONLY one anywhere in this tree (INV-O1).
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <tiny_obj_loader.h>
#include <vector>

namespace engine::editor {

namespace {

// TU-local suffix test, mirroring model_import.cpp's own endsWithFolded -- ITS OWN COPY, following the
// foldAscii/addWarning precedent this tree already set (a shared helper would need a home in a header
// that has no other reason to exist). Only ever asked to distinguish ".obj" from ".mtl" here; the
// dispatch in model_import.cpp has already decided `fileName` ends in one of the two.
[[nodiscard]] bool endsWithFoldedLocal(std::string_view name, std::string_view ext) noexcept {
    if (name.size() <= ext.size()) {
        return false;
    }
    const std::size_t offset = name.size() - ext.size();
    for (std::size_t i = 0; i < ext.size(); ++i) {
        auto a = static_cast<unsigned char>(name[offset + i]);
        auto b = static_cast<unsigned char>(ext[i]);
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<unsigned char>(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<unsigned char>(b + ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

// A25-equivalent (gltf_import.cpp's own precedent): warningTotal is UNCAPPED; `warnings` stops at
// MAX_IMPORT_WARNINGS. No call site may push_back into `warnings` directly.
void addWarning(ImportResult& result, std::string text) {
    ++result.warningTotal;
    if (result.warnings.size() < MAX_IMPORT_WARNINGS) {
        result.warnings.push_back(std::move(text));
    }
}

// A26-equivalent (gltf_import.cpp's own precedent): MONOTONE escalation, Ok < Truncated < the hard
// failures. The hard failures are returned directly by the phase that detects them and never come
// through here -- OBJ never produces MissingBuffer (D7: a missing/refused .mtl is a WARNING).
void escalate(ImportResult& result, ImportStatus status, std::string_view why) {
    const auto rank = [](ImportStatus s) -> int {
        switch (s) {
            case ImportStatus::Ok:
                return 0;
            case ImportStatus::Truncated:
                return 1;
            case ImportStatus::Unsupported:
            case ImportStatus::ParseFailed:
            case ImportStatus::Malformed:
            case ImportStatus::MissingExtension:
            case ImportStatus::MissingBuffer:
                return 2;
        }
        return 2;
    };
    if (rank(status) > rank(result.status)) {
        result.status = status;
    }
    if (!why.empty()) {
        if (!result.message.empty()) {
            result.message += "; ";
        }
        result.message += why;
    }
}

// task 3.2.3: is `line` an `mtllib` directive, and what (trimmed) operand does it carry? A TU-local
// MIRROR of model_import.cpp's own anonymous-namespace helper of the identical shape -- duplicated
// rather than shared (the same precedent as above), because E4's detection below needs to know an
// EMPTY-operand mtllib line existed, which scanObjMtlLibs's own public contract (candidates only, and
// NONE for an empty operand -- D16) cannot report back to a caller.
[[nodiscard]] bool mtllibOperandLocal(std::string_view line, std::string_view& operandOut) {
    constexpr std::string_view KEYWORD = "mtllib";
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (line.size() <= i + KEYWORD.size() || line.substr(i, KEYWORD.size()) != KEYWORD) {
        return false;
    }
    if (line[i + KEYWORD.size()] != ' ' && line[i + KEYWORD.size()] != '\t') {
        return false;
    }
    const std::string_view operand = line.substr(i + KEYWORD.size());
    std::size_t start = 0;
    while (start < operand.size() && (operand[start] == ' ' || operand[start] == '\t')) {
        ++start;
    }
    std::size_t end = operand.size();
    while (end > start && (operand[end - 1] == ' ' || operand[end - 1] == '\t')) {
        --end;
    }
    operandOut = operand.substr(start, end - start);
    return true;
}

// E4: how many `mtllib` lines matched the keyword+separator rule but trimmed to an EMPTY operand.
// scanObjMtlLibs deliberately produces no candidate for one of these (D16's own pseudocode); this is
// the caller-side counterpart that lets importObjFile emit the warning scanObjMtlLibs itself does not.
[[nodiscard]] std::size_t countEmptyMtllibOperandLines(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return 0;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::size_t count = 0;
    std::size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const std::size_t newline = text.find('\n', lineStart);
        std::string_view line =
            newline == std::string_view::npos ? text.substr(lineStart) : text.substr(lineStart, newline - lineStart);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        std::string_view operand;
        if (mtllibOperandLocal(line, operand) && operand.empty()) {
            ++count;
        }
        if (newline == std::string_view::npos) {
            break;
        }
        lineStart = newline + 1;
    }
    return count;
}

// The ".mtl" arm. NOT YET IMPLEMENTED -- lands at Step 7 (D6). `depth` is accepted and, once
// implemented, will be deliberately UNUSED: a .mtl's whole content is local, so this arm is
// depth-independent by construction (AC-23).
[[nodiscard]] ImportResult importMtlOnly(std::string_view /*assetRelativeDir*/, std::span<const std::byte> /*bytes*/,
                                         const ImportSettings& /*settings*/, ImportDepth /*depth*/,
                                         std::span<const ExternalBuffer> /*external*/) {
    ImportResult result;
    result.status = ImportStatus::Unsupported;
    result.message = "the .mtl importer is not wired yet";
    return result;
}

// The ".obj" arm.
[[nodiscard]] ImportResult importObjFile(std::string_view assetRelativeDir, std::span<const std::byte> bytes,
                                         const ImportSettings& settings, ImportDepth depth,
                                         std::span<const ExternalBuffer> external) {
    ImportResult result;
    if (looksLikeBinaryContent(bytes)) {  // AC-54: a renamed PNG/JPEG/GLB never reaches the scan below
        result.status = ImportStatus::Malformed;
        result.message = "this file is not text";
        return result;
    }

    // D5: the WHOLE Structure pass is this text scan. tinyobjloader is never entered, no stream is
    // constructed, no vertex is allocated -- a STATED DEVIATION FROM INV-M4 (Structure and Full
    // disagree about counts/names/the URI set for .obj; the .mtl arm keeps INV-M4 perfectly instead).
    const std::vector<std::string> candidates = scanObjMtlLibs(bytes, MAX_EXTERNAL_URIS);
    // INV-O10: scanObjMtlLibs's OWN cap already bounds `candidates` at MAX_EXTERNAL_URIS, so a
    // per-push check below would be UNREACHABLE (candidates.size() can never exceed the cap, and
    // dedup/refusal only ever REMOVE entries on the way to externalUris). Reaching the cap here is the
    // only observable signal that more candidates may have existed and were dropped; escalate on it
    // directly rather than on a push that can never actually be denied.
    if (candidates.size() >= MAX_EXTERNAL_URIS) {
        escalate(result, ImportStatus::Truncated,
                 "the external reference count exceeds this importer's per-model limit");
    }
    for (const std::string& candidate : candidates) {
        const std::string folded = foldBackslashesToSlashes(candidate);  // D15: a Wavefront operand is
                                                                         // a filesystem path, not a URI
        const UriClassification classified = classifyUri(folded, assetRelativeDir);
        if (classified.kind != UriClass::RelativePath) {
            // AC-17/AC-18: classifyUri's OWN exact reason string, never a paraphrase.
            addWarning(result, "mtllib '" + candidate + "': " + classified.reason);
            continue;  // INV-O8: a refused path never reaches externalUris
        }
        bool alreadyPresent = false;
        for (const std::string& existing : result.externalUris) {
            if (existing == classified.relativePath) {
                alreadyPresent = true;
                break;
            }
        }
        if (alreadyPresent) {
            continue;  // deduplicated -- several mtllib lines/candidates can resolve to one path (E3)
        }
        result.externalUris.push_back(classified.relativePath);
    }
    // E4: an mtllib line whose operand trims to nothing produces no candidate above -- OURS to warn
    // about, since scanObjMtlLibs's own contract is silent about it (D16).
    const std::size_t emptyOperandLines = countEmptyMtllibOperandLines(bytes);
    for (std::size_t i = 0; i < emptyOperandLines; ++i) {
        addWarning(result, "a 'mtllib' directive has an empty operand and was ignored");
    }

    // task 3.2.3, Step 3: at THIS commit, this pure text scan IS the whole function regardless of
    // `depth` -- the Full-depth arm (SpanStreamBuf, the LoadObj call, geometry, materials) lands
    // incrementally from Step 4 onward. `settings`/`external` are accepted and, for now, unused.
    (void)settings;
    (void)depth;
    (void)external;
    return result;
}

}  // namespace

ImportResult importObj(std::string_view fileName, std::string_view assetRelativeDir, std::span<const std::byte> bytes,
                       const ImportSettings& settings, ImportDepth depth, std::span<const ExternalBuffer> external) {
    if (endsWithFoldedLocal(fileName, ".mtl")) {
        return importMtlOnly(assetRelativeDir, bytes, settings, depth, external);
    }
    return importObjFile(assetRelativeDir, bytes, settings, depth, external);
}

}  // namespace engine::editor

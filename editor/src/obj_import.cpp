// Aero Engine — the Wavefront OBJ/MTL backend (task 3.2.3). THE ONLY tinyobjloader TRANSLATION UNIT IN
// THE TREE (INV-O1). This file grows across several steps; see docs/plans/3.2.3-obj-import-tinyobjloader.md
// §S for the sequence and docs/10-engineering-log.md's 3.2.3 entry for the finished design.
//
// Step 4 (this commit): the Full-depth ".obj" arm now enters tinyobjloader through a borrowed
// SpanStreamBuf and maps LoadObj's own diagnostics (§A-9/§A-10). Geometry is NOT converted yet -- the
// result carries the URI set, the status and the warnings, and `meshes` stays empty; that lands at
// Step 5. The ".mtl" arm still returns Unsupported; it lands at Step 7.
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
#include <exception>
#include <istream>
#include <span>
#include <sstream>
#include <streambuf>
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

// D19 -- ~8 lines, read-only, BORROWS `bytes` for the caller's whole call (never copies). The
// alternative (ObjReader::ParseFromString) copies the model TWICE more, up to ~768 MB resident at the
// 256 MiB file cap. With an EMPTY span, `bytes.data()` may be nullptr; `setg(nullptr, nullptr,
// nullptr)` is well-defined and yields an immediately-EOF stream (OI28 drives exactly that).
class SpanStreamBuf final : public std::streambuf {
public:
    explicit SpanStreamBuf(std::span<const std::byte> bytes) {
        // setg's signature requires char*, and this buffer is NEVER written through: every member this
        // class exposes is a READ.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        char* const begin = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
        setg(begin, begin, begin + bytes.size());
    }
};

// task 3.2.3 (§A-9): a reasonable, capped, human-legible excerpt of a library diagnostic string for
// ImportResult::message -- trimmed of surrounding whitespace, first line only (the library's own
// messages are frequently multi-line), capped so one pathological line cannot dominate the message.
[[nodiscard]] std::string firstLineCapped(std::string_view text, std::size_t capBytes) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    const std::string_view trimmed = text.substr(begin, end - begin);
    const std::size_t newline = trimmed.find('\n');
    std::string_view line = newline == std::string_view::npos ? trimmed : trimmed.substr(0, newline);
    while (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    if (line.size() > capBytes) {
        return std::string(line.substr(0, capBytes)) + "...";
    }
    return std::string(line);
}

// task 3.2.3 (§A-10): the library's diagnostic strings, appended one line at a time. TWO sentences are
// dropped, and only two -- both describe OUR OWN single-stream MaterialStreamReader plumbing on the
// SECOND and later `mtllib` directive ("Material stream in error state." -- MaterialStreamReader's own
// guard when our shared istringstream is already exhausted; "Failed to load material file(s). Use
// default material." -- LoadObj's mtllib branch when every reader call for that line failed), never
// anything about the user's file. The library's "Both `d` and `Tr`" warning is the OPPOSITE -- a REAL
// statement about the file -- and is NEVER dropped (§A-11).
void appendLibraryDiagnostics(ImportResult& result, std::string_view text) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::string_view line =
            newline == std::string_view::npos ? text.substr(start) : text.substr(start, newline - start);
        if (!line.empty() && line.find("Material stream in error state") == std::string_view::npos &&
            line.find("Failed to load material file") == std::string_view::npos) {
            addWarning(result, std::string(line));
        }
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
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
    // D7: the RAW candidate that first produced each accepted externalUris entry, SAME index, kept so
    // the Full pass below can name it in a "no matching .mtl was supplied" warning -- never inferred
    // from a library string, always OURS, derived from this candidate list versus the supplied set.
    std::vector<std::string> rawOperandFor;
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
        rawOperandFor.push_back(candidate);
    }
    // E4: an mtllib line whose operand trims to nothing produces no candidate above -- OURS to warn
    // about, since scanObjMtlLibs's own contract is silent about it (D16).
    const std::size_t emptyOperandLines = countEmptyMtllibOperandLines(bytes);
    for (std::size_t i = 0; i < emptyOperandLines; ++i) {
        addWarning(result, "a 'mtllib' directive has an empty operand and was ignored");
    }

    if (depth == ImportDepth::Structure) {
        return result;  // D5: this pure text scan IS the whole Structure pass
    }

    // ---- FULL -----------------------------------------------------------------------------------
    // D7: concatenate the SUPPLIED .mtl texts, in externalUris order, joined by "\n" -- a .mtl with no
    // trailing newline must not glue its last line onto the next file's first. A candidate with no
    // matching supplied buffer is a WARNING, never MissingBuffer (Wavefront's .mtl holds only
    // appearance; without it there is still a mesh).
    std::string mtlText;
    for (std::size_t i = 0; i < result.externalUris.size(); ++i) {
        bool found = false;
        for (const ExternalBuffer& buf : external) {
            if (buf.uri == result.externalUris[i]) {
                if (!mtlText.empty()) {
                    mtlText += '\n';
                }
                mtlText += buf.bytes;
                found = true;
                break;
            }
        }
        if (!found) {
            addWarning(result, "mtllib '" + rawOperandFor[i] + "': no matching .mtl was supplied");
        }
    }

    SpanStreamBuf objBuf(bytes);
    std::istream objStream(&objBuf);
    std::istringstream mtlStream(mtlText);
    tinyobj::MaterialStreamReader mtlReader(mtlStream);

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn;
    std::string err;
    const bool ok =
        tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, &objStream, &mtlReader, /*triangulate=*/true,
                         /*default_vcols_fallback=*/false);

    // §A-9: the RETURN VALUE is the authority, never the `err` string -- the mtllib branch appends
    // err_mtl to *err and keeps parsing, so a non-empty err on a TRUE return is ordinary.
    appendLibraryDiagnostics(result, warn);
    if (!ok) {
        result.status = ImportStatus::ParseFailed;
        result.message = err.empty() ? "the Wavefront parser refused this file" : firstLineCapped(err, 300);
        return result;
    }
    appendLibraryDiagnostics(result, err);

    if (attrib.vertices.empty()) {  // AC-55: no `v` lines at all
        result.status = ImportStatus::Malformed;
        if (result.message.empty()) {
            result.message = "this file declares no vertices";
        }
        return result;
    }

    // task 3.2.3, Step 4: at THIS commit, the library has been entered and its diagnostics mapped, but
    // geometry conversion (positions/indices/materials/nodes) does not exist yet -- `meshes` stays
    // empty regardless of what `shapes`/`materials` hold. Lands incrementally from Step 5 onward.
    (void)settings;
    (void)shapes;
    (void)materials;
    return result;
}

}  // namespace

ImportResult importObj(std::string_view fileName, std::string_view assetRelativeDir, std::span<const std::byte> bytes,
                       const ImportSettings& settings, ImportDepth depth, std::span<const ExternalBuffer> external) {
    // D20: NO exception crosses the public API. tinyobjloader grows std::vector/std::string/std::map/
    // std::stringstream directly from user-controlled input, so std::bad_alloc and std::length_error
    // are reachable -- this ONE try/catch is the boundary for BOTH arms (importObj itself, not each
    // arm separately), which is what keeps it a single block rather than two near-identical copies.
    try {
        if (endsWithFoldedLocal(fileName, ".mtl")) {
            return importMtlOnly(assetRelativeDir, bytes, settings, depth, external);
        }
        return importObjFile(assetRelativeDir, bytes, settings, depth, external);
    } catch (const std::exception& e) {
        ImportResult result;
        result.status = ImportStatus::ParseFailed;
        result.message = e.what();
        return result;
    } catch (...) {
        ImportResult result;
        result.status = ImportStatus::ParseFailed;
        result.message = "an unknown error occurred while parsing this file";
        return result;
    }
}

}  // namespace engine::editor

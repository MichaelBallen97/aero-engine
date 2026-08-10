// Aero Engine — the Wavefront OBJ/MTL backend (task 3.2.3). THE ONLY tinyobjloader TRANSLATION UNIT IN
// THE TREE (INV-O1). This file grows across several steps; see docs/plans/3.2.3-obj-import-tinyobjloader.md
// §S for the sequence and docs/10-engineering-log.md's 3.2.3 entry for the finished design.
//
// Step 6 (this commit): §D-7 steps 4, 7, 8 -- first-appearance material bucketing (D10), the l/p count
// warning (AC-34) and the N-root node array (D11). A primitive's materialIndex may, for THIS commit
// only, name an index into `result.model.materials`, which stays EMPTY until Step 7's §D-6 conversion.
// The ".mtl" arm still returns Unsupported; it lands at Step 7.
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
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <istream>
#include <span>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <tiny_obj_loader.h>
#include <unordered_map>
#include <utility>
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

// task 3.2.3, Step 5 (§D-7 step 5): the exact-triplet vertex dedup key. A STRUCT with an explicit hash
// functor, never a bit-packed std::uint64_t -- vertex_index/normal_index/texcoord_index are each only
// bounded by the 256 MiB file cap, not by any per-field cap, so packing risks silent collisions on a
// pathological file. `normal_index`/`texcoord_index` are `-1` ("absent"), never packed into an unsigned
// range.
struct TripletKey {
    int v = -1;
    int vn = -1;
    int vt = -1;
    bool operator==(const TripletKey&) const = default;
};
struct TripletKeyHash {
    [[nodiscard]] std::size_t operator()(const TripletKey& k) const noexcept {
        std::size_t h = std::hash<int>{}(k.v);
        h ^= std::hash<int>{}(k.vn) + 0x9e3779b9U + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(k.vt) + 0x9e3779b9U + (h << 6) + (h >> 2);
        return h;
    }
};

// task 3.2.3, Step 6 (§D-7 step 4): one already-triangulated, INV-O4-validated face, carried alongside
// its RAW (possibly negative or out-of-range) material id so the bucketing pass below can resolve it
// without re-reading `shape.mesh` a second time.
struct SurvivingFace {
    std::array<tinyobj::index_t, 3> corners{};
    int materialId = -1;
};

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

    // ---- §D-7: geometry conversion -------------------------------------------------------------
    // task 3.2.3, Step 5: steps 1-3, 5, 6, 9. Material bucketing (step 4), the l/p count warning
    // (step 7) and node construction (step 8) land at Step 6 -- EVERY primitive built here carries
    // materialIndex == INVALID_SUBASSET, and every mesh has AT MOST ONE primitive (its survivors,
    // unsplit by material).
    (void)materials;
    bool capsExceeded = false;  // INV-O10: once ANY structural cap is hit, stop -- a COHERENT smaller
                                // model, never a partial-claiming-whole one
    std::size_t totalVertices = 0;
    std::size_t totalIndices = 0;
    std::size_t totalPrimitives = 0;
    const auto vertexCount = static_cast<long>(attrib.vertices.size() / 3);
    const auto normalCount = static_cast<long>(attrib.normals.size() / 3);
    const auto texcoordCount = static_cast<long>(attrib.texcoords.size() / 2);
    const bool fileHasColors = !attrib.colors.empty();  // GLOBAL: default_vcols_fallback=false makes
                                                        // this all-or-nothing across the WHOLE file (F8)

    for (std::size_t shapeIdx = 0; shapeIdx < shapes.size() && !capsExceeded; ++shapeIdx) {
        const tinyobj::shape_t& shape = shapes[shapeIdx];
        ImportedMesh outMesh;
        outMesh.name = shape.name;
        outMesh.localId = static_cast<std::uint32_t>(shapeIdx);

        // steps 2/3: walk num_face_vertices, VALIDATING every corner BEFORE any array access
        // (INV-O4). A face failing ANY check is dropped WHOLE, never partially, with one capped
        // warning -- the single most important loop in this task.
        std::vector<SurvivingFace> survivors;  // FACE order
        std::size_t cursor = 0;
        for (std::size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
            const unsigned int faceVertexCount = shape.mesh.num_face_vertices[f];
            if (faceVertexCount != 3) {
                // Unreachable in principle -- triangulate=true always emits 3 -- kept as defence.
                addWarning(result, "mesh '" + shape.name + "' face " + std::to_string(f) +
                                       ": not a triangle after triangulation, skipped");
                cursor += faceVertexCount;
                continue;
            }
            bool faceValid = true;
            for (unsigned int k = 0; k < 3; ++k) {
                const tinyobj::index_t& idx = shape.mesh.indices[cursor + k];
                if (idx.vertex_index < 0 || idx.vertex_index >= vertexCount) {
                    faceValid = false;
                    break;
                }
                if (idx.normal_index >= 0 && idx.normal_index >= normalCount) {
                    faceValid = false;
                    break;
                }
                if (idx.texcoord_index >= 0 && idx.texcoord_index >= texcoordCount) {
                    faceValid = false;
                    break;
                }
            }
            if (faceValid) {
                const int rawMaterialId = f < shape.mesh.material_ids.size() ? shape.mesh.material_ids[f] : -1;
                survivors.push_back(SurvivingFace{
                    {shape.mesh.indices[cursor + 0], shape.mesh.indices[cursor + 1], shape.mesh.indices[cursor + 2]},
                    rawMaterialId});
            } else {
                addWarning(result, "mesh '" + shape.name + "' face " + std::to_string(f) +
                                       ": an index is out of range, dropped");
            }
            cursor += 3;
        }

        // step 4 (D10): bucket SURVIVING faces by their RESOLVED material index, in FIRST-APPEARANCE
        // order -- never by id value, which would reorder primitives whenever a material is inserted
        // into the .mtl. Two `usemtl` naming the SAME material share the SAME resolved index and
        // therefore the SAME bucket (E11) -- this falls out for free from keying on the value.
        std::vector<std::uint32_t> bucketMaterialIndex;
        std::unordered_map<std::uint32_t, std::size_t> bucketOf;
        std::vector<std::vector<std::size_t>> facesInBucket;
        for (std::size_t s = 0; s < survivors.size(); ++s) {
            std::uint32_t resolved = INVALID_SUBASSET;
            const int rawId = survivors[s].materialId;
            if (rawId >= 0) {
                if (static_cast<std::size_t>(rawId) < materials.size()) {
                    resolved = static_cast<std::uint32_t>(rawId);
                } else {
                    // >= materials.size(): a BAD reference, ONE capped warning. A negative id (no
                    // `usemtl` at all) is the ordinary case and gets none.
                    addWarning(result, "mesh '" + shape.name + "': a face references material " +
                                           std::to_string(rawId) + ", which does not exist");
                }
            }
            const auto found = bucketOf.find(resolved);
            std::size_t bucketIdx = 0;
            if (found == bucketOf.end()) {
                bucketIdx = bucketMaterialIndex.size();
                bucketOf.emplace(resolved, bucketIdx);
                bucketMaterialIndex.push_back(resolved);
                facesInBucket.emplace_back();
            } else {
                bucketIdx = found->second;
            }
            facesInBucket[bucketIdx].push_back(s);
        }

        for (std::size_t b = 0; b < bucketMaterialIndex.size() && !capsExceeded; ++b) {
            const std::vector<std::size_t>& faceIndices = facesInBucket[b];
            if (faceIndices.empty()) {
                continue;
            }
            if (totalPrimitives >= MAX_PRIMITIVES_PER_MODEL) {
                capsExceeded = true;
                escalate(result, ImportStatus::Truncated,
                         "the primitive count exceeds this importer's per-model limit");
                break;
            }

            // step 5's all-or-nothing rule (D9): does EVERY surviving corner in THIS bucket carry a
            // normal/texcoord? Filling gaps with zeroes is not available -- a zero normal is
            // indistinguishable from an absent one, which is the whole reason VertexAttribute is a
            // bitset.
            bool hasNormal = true;
            bool hasTexcoord = true;
            for (const std::size_t faceIdx : faceIndices) {
                for (const tinyobj::index_t& idx : survivors[faceIdx].corners) {
                    hasNormal = hasNormal && idx.normal_index >= 0;
                    hasTexcoord = hasTexcoord && idx.texcoord_index >= 0;
                }
            }

            ImportedPrimitive outPrim;
            outPrim.materialIndex = bucketMaterialIndex[b];
            // step 5's dedup map: packed (v,vn,vt) -> output vertex index. Iteration order of an
            // unordered_map is NOT the emission order -- output order is FACE order (`faceIndices`),
            // which is what makes the result deterministic regardless of hashing.
            std::unordered_map<TripletKey, std::uint32_t, TripletKeyHash> vertexCache;

            for (std::size_t fi = 0; fi < faceIndices.size() && !capsExceeded; ++fi) {
                const std::array<tinyobj::index_t, 3>& corners = survivors[faceIndices[fi]].corners;
                std::array<std::uint32_t, 3> cornerIndices{};
                std::size_t newVertexCount = 0;
                for (unsigned int k = 0; k < 3; ++k) {
                    const tinyobj::index_t& idx = corners[k];
                    const TripletKey key{idx.vertex_index, idx.normal_index, idx.texcoord_index};
                    if (vertexCache.find(key) == vertexCache.end()) {
                        ++newVertexCount;
                    }
                }
                if (totalVertices + newVertexCount > MAX_VERTICES_PER_MODEL) {
                    capsExceeded = true;
                    escalate(result, ImportStatus::Truncated,
                             "the vertex count exceeds this importer's per-model limit");
                    break;
                }
                if (totalIndices + 3 > MAX_INDICES_PER_MODEL) {
                    capsExceeded = true;
                    escalate(result, ImportStatus::Truncated,
                             "the index count exceeds this importer's per-model limit");
                    break;
                }
                for (unsigned int k = 0; k < 3; ++k) {
                    const tinyobj::index_t& idx = corners[k];
                    const TripletKey key{idx.vertex_index, idx.normal_index, idx.texcoord_index};
                    const auto found = vertexCache.find(key);
                    if (found != vertexCache.end()) {
                        cornerIndices[k] = found->second;
                        continue;
                    }
                    const auto vi = static_cast<std::size_t>(idx.vertex_index);
                    const Vec3 position =
                        Vec3{attrib.vertices[vi * 3 + 0], attrib.vertices[vi * 3 + 1], attrib.vertices[vi * 3 + 2]} *
                        settings.scale;  // A22: positions ONLY
                    outPrim.positions.push_back(position);
                    if (hasNormal) {
                        const auto ni = static_cast<std::size_t>(idx.normal_index);
                        outPrim.normals.push_back(Vec3{attrib.normals[ni * 3 + 0], attrib.normals[ni * 3 + 1],
                                                       attrib.normals[ni * 3 + 2]});  // NEVER scaled
                    }
                    if (hasTexcoord) {
                        const auto ti = static_cast<std::size_t>(idx.texcoord_index);
                        outPrim.uv0.push_back(Vec2{attrib.texcoords[ti * 2 + 0], attrib.texcoords[ti * 2 + 1]});
                    }
                    if (fileHasColors) {
                        outPrim.colors.push_back(Vec4{attrib.colors[vi * 3 + 0], attrib.colors[vi * 3 + 1],
                                                      attrib.colors[vi * 3 + 2], 1.0F});  // widened, a = 1
                    }
                    const auto newIndex = static_cast<std::uint32_t>(outPrim.positions.size() - 1);
                    vertexCache.emplace(key, newIndex);
                    cornerIndices[k] = newIndex;
                    ++totalVertices;
                }
                for (unsigned int k = 0; k < 3; ++k) {
                    outPrim.indices.push_back(cornerIndices[k]);
                }
                totalIndices += 3;
            }

            if (!outPrim.indices.empty()) {
                outPrim.attributes = VertexAttribute::Position;  // INV-M5/F6: ALWAYS non-empty on survival
                if (hasNormal) {
                    outPrim.attributes |= VertexAttribute::Normal;
                }
                if (hasTexcoord) {
                    outPrim.attributes |= VertexAttribute::TexCoord0;
                }
                if (fileHasColors) {
                    outPrim.attributes |= VertexAttribute::Color0;
                }
                Aabb primBounds = Aabb::empty();
                for (const Vec3& p : outPrim.positions) {
                    primBounds.expand(p);
                }
                outPrim.bounds = primBounds;
                result.model.summary.bounds.expand(primBounds);  // FROM THE PRIMITIVE, never from the mesh
                result.model.summary.vertexCount += outPrim.positions.size();
                result.model.summary.triangleCount += outPrim.indices.size() / 3;
                ++totalPrimitives;
                outMesh.primitives.push_back(std::move(outPrim));
            }
        }

        // step 7 (AC-34): lines and points are COUNTED and DROPPED, never imported -- one warning per
        // shape naming BOTH counts. The mesh survives with whatever primitives it built above (zero,
        // if it carried only l/p content), so the panel can show the user WHY it looks empty (D11).
        if (!shape.lines.indices.empty() || !shape.points.indices.empty()) {
            addWarning(result, "mesh '" + shape.name + "': " + std::to_string(shape.lines.indices.size()) +
                                   " line indices and " + std::to_string(shape.points.indices.size()) +
                                   " point indices were not imported");
        }

        // §A-12(a): an empty mesh (no surviving primitive) gets a POINT box, never the invalid
        // Aabb::empty() sentinel -- matching both shipped backends byte-for-byte.
        Aabb meshBounds = Aabb::empty();
        for (const ImportedPrimitive& p : outMesh.primitives) {
            meshBounds.expand(p.bounds);
        }
        outMesh.bounds = meshBounds.valid() ? meshBounds : Aabb{};
        result.model.summary.primitiveCount += outMesh.primitives.size();
        result.model.meshes.push_back(std::move(outMesh));
    }
    result.model.summary.meshCount = result.model.meshes.size();

    // step 8 (D11): one ROOT node per mesh, in source order, identity TRS, no synthetic parent --
    // ImportedNode's own field defaults already match this exactly, so only name/localId/meshIndex are
    // set. localId == i == meshIndex for every OBJ node (§A-13, O-iv) -- by CONSTRUCTION, not a guard.
    for (std::size_t i = 0; i < result.model.meshes.size(); ++i) {
        if (result.model.nodes.size() >= MAX_NODES_PER_MODEL) {
            escalate(result, ImportStatus::Truncated, "the node count exceeds this importer's per-model limit");
            break;
        }
        ImportedNode node;
        node.name = result.model.meshes[i].name;
        node.localId = static_cast<std::uint32_t>(i);
        node.meshIndex = static_cast<std::uint32_t>(i);
        const auto nodeIndex = static_cast<std::uint32_t>(result.model.nodes.size());
        result.model.roots.push_back(nodeIndex);
        result.model.nodes.push_back(std::move(node));
    }
    result.model.summary.nodeCount = result.model.nodes.size();

    // task 3.2.3, Step 6: material CONVERSION (tinyobj::material_t -> ImportedMaterial/ImportedImage,
    // §D-6) does not exist yet -- `result.model.materials`/`images` stay empty, so a primitive's
    // materialIndex may, for THIS commit only, name an index into an EMPTY array. Step 7 populates it.
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

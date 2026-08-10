// Aero Engine — the Wavefront OBJ/MTL backend (task 3.2.3). THE ONLY tinyobjloader TRANSLATION UNIT IN
// THE TREE (INV-O1). This file grows across several steps; see docs/plans/3.2.3-obj-import-tinyobjloader.md
// §S for the sequence and docs/10-engineering-log.md's 3.2.3 entry for the finished design.
//
// Step 7 (this commit): §D-6 material conversion (tinyobj::material_t -> ImportedMaterial/
// ImportedImage), SHARED verbatim by both arms -- the ".mtl" arm is now real (AC-23's depth-independence
// falls out of the shared function). Two build-time findings, corrected here rather than left to
// surprise a caller and recorded in full in declaredWithEmptyName's own comment: LoadMtl always
// flushes a trailing "material" whether or not any `newmtl` was ever seen, and a second `mtllib`
// directive's readMatFn call still re-enters LoadMtl (tinyobjloader's own stream-exhaustion check tests
// failbit, not eofbit) rather than taking the "stream in error" branch §A-10 describes.
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
#include <aero/editor/project_files.hpp>  // parentOf -- D16's textureBaseDir derivation, reused verbatim

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <istream>
#include <map>
#include <optional>
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

// code-review round, gap 5: one mtllib LINE's own candidate group -- `wholeOperand` is the line's raw
// trimmed operand (what a "no matching .mtl was supplied" warning names), `candidates` is that SAME
// operand's own candidate set (the whole operand, then each whitespace-separated token, D16's own
// shape). D7/AC-22 say ONE warning per mtllib LINE whose candidates were ALL unsupplied -- never one per
// accepted candidate -- and D16 deliberately offers a line's operand two ways, so an ordinary
// `mtllib my file.mtl` (the spaced file supplied) or `mtllib a.mtl b.mtl` (both files supplied
// separately) must produce ZERO warnings: at least one reading of the LINE was served. Reuses the
// PUBLIC scanObjMtlLibs on just this one line's own bytes rather than re-implementing the tokenizer a
// third time in this file -- scanObjMtlLibs is already the tested, canonical shape (MI124-MI131), and a
// single line is a trivially small input for it.
struct MtllibLineGroup {
    std::string wholeOperand;
    std::vector<std::string> candidates;
};

[[nodiscard]] std::vector<MtllibLineGroup> scanObjMtllibLineGroups(std::span<const std::byte> bytes) {
    std::vector<MtllibLineGroup> out;
    if (bytes.empty()) {
        return out;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const std::size_t newline = text.find('\n', lineStart);
        std::string_view line =
            newline == std::string_view::npos ? text.substr(lineStart) : text.substr(lineStart, newline - lineStart);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        std::string_view operand;
        if (mtllibOperandLocal(line, operand) && !operand.empty()) {
            const std::span<const std::byte> lineBytes(reinterpret_cast<const std::byte*>(line.data()), line.size());
            MtllibLineGroup group;
            group.wholeOperand = std::string(operand);
            group.candidates = scanObjMtlLibs(lineBytes, MAX_EXTERNAL_URIS);
            out.push_back(std::move(group));
        }
        if (newline == std::string_view::npos) {
            break;
        }
        lineStart = newline + 1;
    }
    return out;
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

// task 3.2.3, Step 7 (§D-6): one texture slot's resolution. `texname` empty -> the slot stays
// DISENGAGED, no image, no warning. Images are FOUND-OR-APPENDED by their RESOLVED relativePath, in
// first-seen order, so the SAME texture named by two materials becomes ONE ImportedImage (E14).
[[nodiscard]] std::optional<ImportedTextureRef> convertTextureSlot(std::string_view texname,
                                                                   const tinyobj::texture_option_t& texopt,
                                                                   std::string_view textureBaseDir, ImportResult& out) {
    if (texname.empty()) {
        return std::nullopt;
    }
    const std::string folded = foldBackslashesToSlashes(texname);  // D15: a filesystem path, not a URI
    const UriClassification classified = classifyUri(folded, textureBaseDir);
    if (classified.kind != UriClass::RelativePath) {
        // refused -> the slot stays DISENGAGED; an ImportedImage carries the reason so the panel can
        // show WHY (AC-15's texture half). INV-O8: never externalUris, never a dependency.
        ImportedImage image;
        image.uri = std::string(texname);
        image.refusal = classified.reason;
        out.model.images.push_back(std::move(image));
        addWarning(out, "texture '" + std::string(texname) + "': " + classified.reason);
        return std::nullopt;
    }
    std::uint32_t imageIndex = INVALID_SUBASSET;
    for (std::size_t i = 0; i < out.model.images.size(); ++i) {
        if (out.model.images[i].relativePath == classified.relativePath) {
            imageIndex = static_cast<std::uint32_t>(i);
            break;
        }
    }
    if (imageIndex == INVALID_SUBASSET) {
        ImportedImage image;
        image.uri = std::string(texname);
        image.relativePath = classified.relativePath;
        imageIndex = static_cast<std::uint32_t>(out.model.images.size());
        out.model.images.push_back(std::move(image));
        bool alreadyDependency = false;
        for (const std::string& existing : out.externalUris) {
            if (existing == classified.relativePath) {
                alreadyDependency = true;
                break;
            }
        }
        if (!alreadyDependency && out.externalUris.size() < MAX_EXTERNAL_URIS) {
            out.externalUris.push_back(classified.relativePath);
        }
    }
    ImportedTextureRef ref;
    ref.imageIndex = imageIndex;
    ref.uvSet = 0;
    // -clamp maps to BOTH axes -- texture_option_t::clamp is a SINGLE bool (VERIFIED). Absent -> Repeat.
    // TextureWrap::MirroredRepeat is UNREACHABLE from this backend -- .mtl has only -clamp.
    ref.wrapU = texopt.clamp ? TextureWrap::ClampToEdge : TextureWrap::Repeat;
    ref.wrapV = ref.wrapU;
    return ref;
}

// -s/-o have NO field in ImportedTextureRef; a non-default value on ANY of the four slots' texopts
// produces ONE aggregate warning per material rather than silently wrong UVs.
[[nodiscard]] bool hasNonDefaultTransform(const tinyobj::texture_option_t& texopt) noexcept {
    return texopt.scale[0] != 1.0F || texopt.scale[1] != 1.0F || texopt.scale[2] != 1.0F ||
           texopt.origin_offset[0] != 0.0F || texopt.origin_offset[1] != 0.0F || texopt.origin_offset[2] != 0.0F;
}

// build-time finding, corrected here rather than left to surprise a caller: tinyobjloader's LoadMtl
// UNCONDITIONALLY flushes ONE trailing "material" at end-of-parse (VERIFIED against the real header --
// "// flush last material." -- with NO check on whether ANY `newmtl` was ever seen), so even a
// COMPLETELY EMPTY .mtl stream produces materials.size() == 1, an unnamed, all-default entry. A SECOND
// build-time finding compounds it for the .obj arm specifically: MaterialStreamReader's own
// `if (!m_inStream)` guard tests std::istream::fail(), which checks failbit/badbit -- NOT eofbit -- so
// after the FIRST mtllib line's LoadMtl call exhausts the shared stream (setting only eofbit via
// peek()), the stream is STILL "not failed". A SECOND mtllib line's readMatFn call therefore does NOT
// take the "Material stream in error state" branch at all -- it calls LoadMtl AGAIN, on an
// already-EOF stream, which parses zero lines but STILL unconditionally flushes ITS OWN empty-named
// phantom (VERIFIED directly: a two-mtllib-line, two-VALID-file fixture produces `warn`/`err` BOTH
// empty, and a THIRD, empty-named material in `materials`). §A-10's own premise -- that this scenario
// is what makes the two library sentences fire -- does not hold for THIS library version and stream
// combination; see docs/10-engineering-log.md's 3.2.3 entry for the full account. The two sentences
// stay filtered in appendLibraryDiagnostics below as harmless defence in depth, but no fixture in this
// suite can PROVE they fire, and none claims to.
//
// E13 (a .mtl with no newmtl at all imports as ZERO materials) and AC-22 (an unsupplied .mtl leaves
// result.model.materials EMPTY) are OUR OWN policy, not something the library hands us for free, and
// BOTH phantom-material shapes above share the SAME observable signature: an EMPTY name. `newmtl`
// followed by literally nothing is a real, if malformed, user declaration -- and the library's OWN
// "empty material name in `newmtl`" warning is how convertMaterials tells the two apart.
[[nodiscard]] bool declaredWithEmptyName(std::string_view combinedWarnings) noexcept {
    return combinedWarnings.find("empty material name in") != std::string_view::npos;
}

// §D-6, SHARED VERBATIM by both arms -- what keeps a .mtl imported standalone and the same .mtl
// imported through its .obj byte-identical in the fields both produce (AC-23). `combinedWarnings` is
// the library's OWN raw warn+err text (BEFORE filtering), consulted ONLY to distinguish a genuine
// empty-named `newmtl` from a phantom flush (declaredWithEmptyName's own comment).
//
// RETURNS the library-index -> converted-index map (code-review gap 1, BLOCKING). `src`'s own index
// space (the library's `materials` vector, which `material_ids[face]` references) and OUR index space
// (`out.model.materials`, the CONVERTED vector) diverge the instant an entry is dropped below -- a
// phantom flush (this file's own Findings 1/2, top-of-file comment) or an overflow past
// MAX_MATERIALS_PER_MODEL. The two spaces coincide for every well-formed file and diverge the moment
// either happens -- the epic's own "two things that coincide for glTF and diverge here" pattern, one
// instance the plan's own §A-13 catalogue missed. A raw `material_ids[face]` value must NEVER become
// ImportedPrimitive::materialIndex directly; every caller resolves through this map instead, sized to
// `src.size()` and defaulted to INVALID_SUBASSET, so an unmapped (dropped, or never reached because of
// the cap) raw index reads as "no material" rather than a stale, now-out-of-range one.
[[nodiscard]] std::vector<std::uint32_t> convertMaterials(const std::vector<tinyobj::material_t>& src,
                                                           std::string_view textureBaseDir,
                                                           std::string_view combinedWarnings, ImportResult& out) {
    const bool keepEmptyNamed = declaredWithEmptyName(combinedWarnings);
    std::vector<std::uint32_t> rawToConverted(src.size(), INVALID_SUBASSET);
    std::uint32_t nextLocalId = 0;
    for (std::size_t rawIndex = 0; rawIndex < src.size(); ++rawIndex) {
        const tinyobj::material_t& m = src[rawIndex];
        if (m.name.empty() && !keepEmptyNamed) {
            continue;  // a phantom flush, never a real declaration -- silently dropped, no warning, and
                       // NEVER mapped: rawToConverted[rawIndex] stays INVALID_SUBASSET
        }
        if (nextLocalId >= MAX_MATERIALS_PER_MODEL) {
            escalate(out, ImportStatus::Truncated, "the material count exceeds this importer's per-model limit");
            break;  // every remaining src[] entry stays UNMAPPED too -- INVALID_SUBASSET, never a guess
        }
        ImportedMaterial mat;
        mat.name = m.name;
        mat.localId = nextLocalId++;

        // D14's zero-factor rule: where a zero factor would ANNIHILATE a texture the same material
        // supplies, read it as the neutral 1. Kd 0 0 0 with NO texture stays black -- a legitimately
        // black material.
        const bool hasBaseColorTex = !m.diffuse_texname.empty();
        Vec3 diffuse{m.diffuse[0], m.diffuse[1], m.diffuse[2]};
        if (hasBaseColorTex && diffuse == Vec3::zero()) {
            diffuse = Vec3::one();
        }
        // §A-11: `d` ALWAYS wins over `Tr`, regardless of order -- the library's own dissolve field
        // already encodes this (VERIFIED against the upstream source); read verbatim.
        mat.baseColorFactor = Vec4{diffuse.x, diffuse.y, diffuse.z, m.dissolve};

        const bool hasMetallicTex = !m.metallic_texname.empty();
        mat.metallicFactor = (hasMetallicTex && m.metallic == 0.0F) ? 1.0F : m.metallic;
        // roughness's zero-factor clause is UNCONDITIONAL -- 0 means a perfect mirror, and no classic
        // Wavefront material means that.
        mat.roughnessFactor = (m.roughness == 0.0F) ? 1.0F : m.roughness;

        mat.emissiveFactor = Vec3{m.emission[0], m.emission[1], m.emission[2]};

        const bool hasAlphaTex = !m.alpha_texname.empty();
        // Blend when dissolve < 1 OR map_d is present; else Opaque. NEVER Mask -- Wavefront has no
        // cutoff.
        mat.alphaMode = (m.dissolve < 1.0F || hasAlphaTex) ? AlphaMode::Blend : AlphaMode::Opaque;

        // Fixed slot order (baseColor, metallicRoughness, normal, emissive) so `images` order is
        // deterministic.
        mat.baseColor = convertTextureSlot(m.diffuse_texname, m.diffuse_texopt, textureBaseDir, out);

        // metallicRoughness: roughness_texname (map_Pr), ELSE metallic_texname (map_Pm) -- glTF packs
        // both into ONE texture and only one can be bound. One warning when BOTH are present and DIFFER.
        if (!m.roughness_texname.empty() && !m.metallic_texname.empty() && m.roughness_texname != m.metallic_texname) {
            addWarning(out, "material '" + m.name +
                                "': both a roughness and a metallic texture are declared and differ; "
                                "only the roughness texture is used");
        }
        if (!m.roughness_texname.empty()) {
            mat.metallicRoughness = convertTextureSlot(m.roughness_texname, m.roughness_texopt, textureBaseDir, out);
        } else {
            mat.metallicRoughness = convertTextureSlot(m.metallic_texname, m.metallic_texopt, textureBaseDir, out);
        }

        // normal: normal_texname (norm), ELSE bump_texname (map_Bump/bump) -- NO warning: Blender's OBJ
        // exporter writes real tangent-space normal maps into map_Bump, and refusing that would lose
        // the normal map on most files in the wild.
        if (!m.normal_texname.empty()) {
            mat.normal = convertTextureSlot(m.normal_texname, m.normal_texopt, textureBaseDir, out);
            mat.normalScale = m.normal_texopt.bump_multiplier;
        } else if (!m.bump_texname.empty()) {
            mat.normal = convertTextureSlot(m.bump_texname, m.bump_texopt, textureBaseDir, out);
            mat.normalScale = m.bump_texopt.bump_multiplier;
        }
        // else: mat.normal stays disengaged, mat.normalScale stays its default 1 -- "1 when neither map
        // exists".

        mat.emissive = convertTextureSlot(m.emissive_texname, m.emissive_texopt, textureBaseDir, out);

        // mat.occlusion is ALWAYS disengaged -- map_Ka is an ambient COLOUR map, not an AO map, and
        // treating it as one is a guess the panel would print as a fact.

        if (hasNonDefaultTransform(m.diffuse_texopt) || hasNonDefaultTransform(m.roughness_texopt) ||
            hasNonDefaultTransform(m.metallic_texopt) || hasNonDefaultTransform(m.normal_texopt) ||
            hasNonDefaultTransform(m.bump_texopt) || hasNonDefaultTransform(m.emissive_texopt)) {
            addWarning(out,
                       "material '" + m.name + "': a texture declares -s/-o, which this importer does not represent");
        }

        rawToConverted[rawIndex] = mat.localId;  // mat.localId == its own position in out.model.materials
        out.model.materials.push_back(std::move(mat));
    }
    return rawToConverted;
}

// The ".mtl" arm (D6). `depth` is accepted and deliberately UNUSED: a .mtl's whole content is local,
// so this arm is depth-independent BY CONSTRUCTION, which is what makes AC-23's field-for-field
// equality structural rather than maintained.
[[nodiscard]] ImportResult importMtlOnly(std::string_view assetRelativeDir, std::span<const std::byte> bytes,
                                         const ImportSettings& /*settings*/, ImportDepth /*depth*/,
                                         std::span<const ExternalBuffer> /*external*/) {
    ImportResult result;
    if (looksLikeBinaryContent(bytes)) {  // AC-54: a renamed PNG/JPEG/GLB never reaches LoadMtl
        result.status = ImportStatus::Malformed;
        result.message = "this file is not text";
        return result;
    }
    SpanStreamBuf mtlBuf(bytes);
    std::istream mtlStream(&mtlBuf);
    std::map<std::string, int> materialMap;
    std::vector<tinyobj::material_t> materials;
    std::string warn;
    std::string err;
    tinyobj::LoadMtl(&materialMap, &materials, &mtlStream, &warn, &err);
    // §A-9: LoadMtl returns void -- there is no true/false signal here, unlike LoadObj. Both diagnostic
    // strings are treated identically.
    appendLibraryDiagnostics(result, warn);
    appendLibraryDiagnostics(result, err);

    // the .mtl arm builds no geometry, so there is nothing to resolve the raw->converted map against.
    (void)convertMaterials(materials, assetRelativeDir, warn + err, result);  // D6: textureBaseDir IS
                                                                              // assetRelativeDir
    result.model.summary.materialCount = result.model.materials.size();
    result.model.summary.imageCount = result.model.images.size();
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
    // D16: textureBaseDir for THIS arm is parentOf() of the FIRST mtllib path that actually
    // CONTRIBUTED text -- and, since more than one distinct contributing directory would make that
    // choice ambiguous, ONE warning fires when that happens. In the universal case (a .mtl beside its
    // .obj) every contributor shares the model's own directory and the warning never fires.
    std::string firstContributingDir;
    bool sawFirstContributingDir = false;
    bool multipleContributingDirs = false;
    // code-review round, gap 5: `uriFound[i]` replaces the old inline "no matching .mtl" warning -- the
    // warning itself is decided PER MTLLIB LINE, below, never per accepted candidate.
    std::vector<bool> uriFound(result.externalUris.size(), false);
    for (std::size_t i = 0; i < result.externalUris.size(); ++i) {
        for (const ExternalBuffer& buf : external) {
            if (buf.uri == result.externalUris[i]) {
                if (!mtlText.empty()) {
                    mtlText += '\n';
                }
                mtlText += buf.bytes;
                uriFound[i] = true;
                const std::string dir = parentOf(result.externalUris[i]);
                if (!sawFirstContributingDir) {
                    firstContributingDir = dir;
                    sawFirstContributingDir = true;
                } else if (dir != firstContributingDir) {
                    multipleContributingDirs = true;
                }
                break;
            }
        }
    }
    // code-review round, gap 5: D7/AC-22 say ONE warning per mtllib LINE whose candidates were ALL
    // unsupplied -- the ORIGINAL per-URI loop above warned once per unmatched CANDIDATE instead, so an
    // ordinary `mtllib my file.mtl` (the spaced file supplied under its correct reading) produced TWO
    // spurious warnings for its two unmatched TOKEN readings, and `mtllib a.mtl b.mtl` (both files
    // supplied separately) produced ONE spurious warning for its unmatched WHOLE-OPERAND reading -- both
    // ordinary Wavefront, both false noise. Group by source line: a line is "served" iff ANY of its own
    // candidates (by raw text) matches an ACCEPTED, FOUND externalUris entry; a served line warns never,
    // regardless of how many of its OTHER readings went unsupplied.
    std::unordered_map<std::string, std::size_t> indexOfRawCandidate;
    for (std::size_t i = 0; i < rawOperandFor.size(); ++i) {
        indexOfRawCandidate.emplace(rawOperandFor[i], i);
    }
    for (const MtllibLineGroup& group : scanObjMtllibLineGroups(bytes)) {
        bool lineHadAcceptedCandidate = false;
        bool lineServed = false;
        for (const std::string& candidate : group.candidates) {
            const auto found = indexOfRawCandidate.find(candidate);
            if (found == indexOfRawCandidate.end()) {
                continue;  // refused by classifyUri, or deduplicated into an earlier line's entry
            }
            lineHadAcceptedCandidate = true;
            if (uriFound[found->second]) {
                lineServed = true;
                break;
            }
        }
        if (lineHadAcceptedCandidate && !lineServed) {
            addWarning(result, "mtllib '" + group.wholeOperand + "': no matching .mtl was supplied");
        }
    }
    if (multipleContributingDirs) {
        addWarning(result,
                   "more than one distinct directory contributed a .mtl; texture paths are "
                   "resolved against the first one");
    }
    const std::string textureBaseDir = firstContributingDir;  // "" (the model's own dir) when none contributed

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

    // §D-6: material conversion, SHARED with the .mtl arm above -- what keeps a .mtl imported standalone
    // and the same .mtl imported through its .obj byte-identical in the fields both produce. AC-22: an
    // unsupplied .mtl leaves `materials` holding only phantom flushes (declaredWithEmptyName's own
    // comment), all silently dropped.
    //
    // RUN BEFORE GEOMETRY (code-review gap 1, BLOCKING; moved from after node-building), so the bucketing
    // loop below can resolve every `material_ids[face]` value through `materialIndexMap` rather than
    // through the library's own, now-DIVERGED index space. Nothing below this point needed geometry to
    // exist first -- `materials`, `textureBaseDir` and `warn`/`err` were all already available.
    const std::vector<std::uint32_t> materialIndexMap = convertMaterials(materials, textureBaseDir, warn + err, result);
    result.model.summary.materialCount = result.model.materials.size();
    result.model.summary.imageCount = result.model.images.size();

    // ---- §D-7: geometry conversion -------------------------------------------------------------
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
                    // code-review gap 1, BLOCKING: resolve through materialIndexMap, NEVER use the raw
                    // library index directly. `rawId` only indexes `materials` (the LIBRARY's own
                    // vector); convertMaterials above may have DROPPED this exact entry (a phantom
                    // flush, or an overflow past MAX_MATERIALS_PER_MODEL), in which case the map reads
                    // INVALID_SUBASSET here too, correctly, instead of a stale index past the end of
                    // out.model.materials.
                    resolved = materialIndexMap[static_cast<std::size_t>(rawId)];
                }
                if (resolved == INVALID_SUBASSET) {
                    // Two ways to land here, both worth ONE capped warning: rawId was >= materials.size()
                    // (a BAD reference into the library's own vector), or it was in range but the entry
                    // it named was DROPPED above (the case this gap closes -- it used to leave `resolved`
                    // at the stale raw index instead). A negative id (no `usemtl` at all) never reaches
                    // this branch and stays silent, exactly as before.
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

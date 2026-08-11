// Aero Engine — the Assimp backend for .dae/.ply/.stl (task 3.2.5). THE ONLY ASSIMP TRANSLATION UNIT
// IN THE TREE (INV-A1). See docs/10-engineering-log.md's 3.2.5 entry for the finished design.
//
// FORBIDDEN, PERMANENTLY, and each named here so this file's own documentation states the rule (which
// is exactly why the gate that checks them STRIPS COMMENTS first): Importer::ReadFile, DefaultIOSystem,
// aiImportFile, aiExportScene, Assimp::Exporter, ZipArchiveIOSystem, SetIOHandler(nullptr),
// DefaultLogger, UnregisterLoader, and the post-process flags PreTransformVertices, MakeLeftHanded,
// ConvertToLeftHanded, FlipUVs, FlipWindingOrder, GlobalScale, GenNormals, GenSmoothNormals,
// CalcTangentSpace, JoinIdenticalVertices, RemoveRedundantMaterials, GenBoundingBoxes, FindInvalidData,
// FindDegenerates.
//
// NEVER READS A FILE, NEVER LOGS, NEVER THROWS ACROSS ITS OWN BOUNDARY, NEVER TOUCHES <filesystem>.
#include "assimp_import.hpp"

#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <assimp/commonMetaData.h>  // AI_METADATA_SOURCE_FORMAT{,_VERSION} -- the A-5 assertion's key
#include <assimp/config.h>
#include <assimp/matrix4x4.h>
#include <assimp/postprocess.h>
#include <assimp/quaternion.h>
#include <assimp/scene.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// task 3.2.5 (A-3). THE MOST IMPORTANT TWENTY LINES IN THIS FILE.
//
// Importer::ReadFileFromMemory does NOT install a memory-only IO handler -- it WRAPS the installed one
// in a MemoryIOSystem that serves ONE magic filename from the buffer and DELEGATES EVERY OTHER PATH to
// the wrapped handler. Out of the box that wrapped handler is a real, unrestricted DefaultIOSystem, so
// a user-supplied .dae naming <init_from>/etc/passwd</init_from> would get a live read.
//
// And the obvious fix is a trap: SetIOHandler(nullptr) does NOT clear the handler, it INSTALLS a fresh
// DefaultIOSystem. The only sanctioned sequence in this tree is
//     Assimp::Importer importer;
//     importer.SetIOHandler(new RefusingIoSystem());   // ownership transfers to the Importer
//     importer.ReadFileFromMemory(...);
// and it is what makes "the editor supplies every byte" true for this backend as it is for the other
// three (fastgltf: FromBytes only; ufbx: an open_file_cb that cannot succeed; tinyobjloader: two
// istream overloads).
//
// Every method returns the "cannot" answer. getOsSeparator returns '/' on EVERY platform, deliberately,
// so no Assimp-internal path decision can diverge between the three CI lanes (R7).
class RefusingIoSystem final : public Assimp::IOSystem {
public:
    [[nodiscard]] bool Exists(const char* /*file*/) const override { return false; }
    [[nodiscard]] char getOsSeparator() const override { return '/'; }
    Assimp::IOStream* Open(const char* /*file*/, const char* /*mode*/) override { return nullptr; }
    void Close(Assimp::IOStream* /*file*/) override {}
    [[nodiscard]] bool ComparePaths(const char* /*one*/, const char* /*second*/) const override { return false; }
    bool PushDirectory(const std::string& /*path*/) override { return false; }
    [[nodiscard]] const std::string& CurrentDirectory() const override { return emptyDirectory(); }
    [[nodiscard]] size_t StackSize() const override { return 0; }
    bool PopDirectory() override { return false; }
    bool CreateDirectory(const std::string& /*path*/) override { return false; }
    bool ChangeDirectory(const std::string& /*path*/) override { return false; }
    bool DeleteFile(const std::string& /*file*/) override { return false; }

private:
    // CurrentDirectory() returns a const std::string& in Assimp's own signature, so it needs a stable
    // referent. A function-local static is the one shape that is thread-safe, needs no member, and
    // cannot be mistaken for state (this class is deliberately stateless).
    [[nodiscard]] static const std::string& emptyDirectory() {
        static const std::string empty;
        return empty;
    }
};

// TU-local suffix test, mirroring model_import.cpp's own endsWithFolded -- ITS OWN COPY, following the
// foldAscii/addWarning precedent this tree has now set four times. Only ever asked to distinguish the
// three extensions the dispatch has already narrowed to.
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

// gltf_import.cpp's / obj_import.cpp's own precedent: warningTotal is UNCAPPED; `warnings` stops at
// MAX_IMPORT_WARNINGS. No call site may push_back into `warnings` directly.
void addWarning(ImportResult& result, std::string text) {
    ++result.warningTotal;
    if (result.warnings.size() < MAX_IMPORT_WARNINGS) {
        result.warnings.push_back(std::move(text));
    }
}

// MONOTONE escalation, Ok < Truncated < the hard failures. The hard failures are returned directly by
// the phase that detects them and never come through here.
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

// task 3.2.5 (A-14, trap 1). aiMatrix4x4 is ALWAYS ROW-MAJOR -- Assimp's own header says so in those
// words, and a1/a2/a3/a4 is the first ROW. engine::Mat4 is COLUMN-major (mat4.hpp: columns[c] is the
// c-th basis vector, columns[3] is the translation). So this IS a TRANSPOSE, and it is the ONLY place
// in this file that converts a matrix. NEVER a memcpy, NEVER a bit_cast, NEVER an element-order-
// preserving copy: every one of those produces a plausible-looking WRONG model rather than a failure.
[[nodiscard]] Mat4 toMat4(const aiMatrix4x4& m) noexcept {
    Mat4 out;
    out.columns[0] = Vec4{m.a1, m.b1, m.c1, m.d1};
    out.columns[1] = Vec4{m.a2, m.b2, m.c2, m.d2};
    out.columns[2] = Vec4{m.a3, m.b3, m.c3, m.d3};
    out.columns[3] = Vec4{m.a4, m.b4, m.c4, m.d4};
    return out;
}

// task 3.2.5 (A-14, trap 2). aiQuaternion is {w, x, y, z}; engine::Quat is {x, y, z, w} (glTF's own
// accessor order). A NAMED FIELD COPY, never a 4-argument positional construction and never a memcpy.
[[nodiscard]] Quat toQuat(const aiQuaternion& q) noexcept { return Quat{q.x, q.y, q.z, q.w}; }

[[nodiscard]] Vec3 toVec3(const aiVector3D& v) noexcept { return Vec3{v.x, v.y, v.z}; }

// The Importer OWNS the aiScene and destroys it in its own destructor, so the two must live and die
// together. GetOrphanedScene() is DELIBERATELY NOT USED: taking ownership adds a second lifetime to get
// wrong, for no benefit, since the scene is consumed entirely inside one function.
//
// A value type holding the Importer BY VALUE (not by pointer): Assimp::Importer is non-copyable and
// non-movable, so this struct is neither -- which is exactly what stops it being returned by value from
// runAssimp. runAssimp therefore takes it BY REFERENCE from the caller's stack frame.
struct AssimpScene {
    Assimp::Importer importer;
    const aiScene* scene = nullptr;
};

// task 3.2.5 (A-6). Pinned EXPLICITLY, every flag stated even where it matches an Assimp default, so a
// bump cannot move behaviour silently (3.2.2's ufbx_bake_opts rule, second application). The FORBIDDEN
// set and the reason for each is in .claude/rules/editor.md; the ones that delete user data --
// aiProcess_FindInvalidData, aiProcess_FindDegenerates -- are the two most likely to be reached for on
// a bad day, and neither may ever appear here.
constexpr unsigned int ASSIMP_POST_PROCESS_FLAGS =
    aiProcess_Triangulate |           // polygons -> triangles; ImportedPrimitive has no other mode
    aiProcess_SortByPType |           // + AI_CONFIG_PP_SBP_REMOVE below: drops point/line meshes
    aiProcess_LimitBoneWeights |      // + AI_CONFIG_PP_LBW_MAX_WEIGHTS = 4: our joints/weights shape
    aiProcess_PopulateArmatureData |  // fills aiBone::mNode/mArmature, so a joint resolves to a node
    aiProcess_ValidateDataStructure;  // A-6b: a scene violating Assimp's own invariants is REFUSED,
                                      // with a message, rather than converted -- our converter walks
                                      // mFaces[i].mIndices[j] and would be the thing that reads out of
                                      // bounds. Every index is STILL range-checked by us (INV-A4).

// task 3.2.5 (A-5). The DISTINCTIVE FRAGMENT of the aiImporterDesc::mName the extension's one claimant
// carries. A SUBSTRING, not the whole string, so an upstream prose change does not turn every file of
// that format Malformed -- while a genuinely different loader ("Blender", "MD5", "glTF2") still fails
// loudly. The full strings, MEASURED against this build's own descriptor table, are recorded beside
// each; the exhaustive scan of all aiImporterDesc definitions in 6.0.4 confirms exactly one claimant
// per extension ("dae xml zae", "ply", "stl"), which is why the auto-detection fallback is unreachable
// for these three and why this assertion is the thing that would notice if that ever changed.
constexpr std::string_view DAE_LOADER_FRAGMENT = "Collada";  // full: "Collada Importer"
constexpr std::string_view PLY_LOADER_FRAGMENT = "PLY";      // full: "Stanford Polygon Library (PLY) Importer"
constexpr std::string_view STL_LOADER_FRAGMENT = "STL";      // full: "Stereolithography (STL) Importer"

// Loads `bytes` through Assimp with the filesystem sealed off, and REFUSES any scene the expected
// loader did not produce.
//
// Assimp's dispatch uses the extension hint FIRST: if exactly one registered loader claims it, that
// loader runs WITH NO SIGNATURE CHECK AT ALL. Exactly one claims each of dae/ply/stl in this build, so
// the all-loaders auto-detection fallback is unreachable for us. That is a property of the CURRENT
// descriptor table, not a guarantee -- hence the assertion below rather than a comment.
//
// Returns true on success, with `out.scene` non-null. On failure `result` already carries the status
// and the message.
[[nodiscard]] bool runAssimp(std::span<const std::byte> bytes, const char* hint,
                             std::string_view expectedLoaderFragment, AssimpScene& out, ImportResult& result) {
    // A-3: BEFORE ReadFileFromMemory, ALWAYS. Ownership of the handler transfers to the Importer, which
    // deletes it in ~Importer -- there is no second owner and no manual delete anywhere in this file.
    out.importer.SetIOHandler(new RefusingIoSystem());
    out.importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
    out.importer.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS, 4);

    out.scene = out.importer.ReadFileFromMemory(bytes.data(), bytes.size(), ASSIMP_POST_PROCESS_FLAGS, hint);
    if (out.scene == nullptr) {
        // A-6b/A-17: Assimp swallows its own exceptions and reports through GetErrorString(). A
        // ValidateDataStructure rejection arrives HERE, not as a throw -- and it is Malformed ("parsed,
        // but violates an invariant this importer requires"), never ParseFailed, so the panel can tell
        // the user which of the two happened.
        const std::string_view error = out.importer.GetErrorString();
        const bool validation =
            error.find("Validation") != std::string_view::npos || error.find("validation") != std::string_view::npos;
        result.status = validation ? ImportStatus::Malformed : ImportStatus::ParseFailed;
        result.message = error.empty() ? std::string("the file could not be parsed") : std::string(error);
        return false;
    }

    // A-5: WHICH loader ran, asserted rather than assumed. Importer::ReadFile writes the chosen
    // loader's own aiImporterDesc::mName into scene metadata under AI_METADATA_SOURCE_FORMAT.
    aiString sourceFormat;
    const bool haveFormat =
        out.scene->mMetaData != nullptr && out.scene->mMetaData->Get(AI_METADATA_SOURCE_FORMAT, sourceFormat);
    const std::string_view actual = haveFormat ? std::string_view(sourceFormat.C_Str()) : std::string_view();
    if (actual.find(expectedLoaderFragment) == std::string_view::npos) {
        out.scene = nullptr;
        result.status = ImportStatus::Malformed;
        result.message = std::format("this file was parsed by '{}', not by the expected '{}' loader",
                                     actual.empty() ? std::string_view("<unknown>") : actual, expectedLoaderFragment);
        return false;
    }

    // AI_SCENE_FLAGS_INCOMPLETE means the loader produced something usable but partial (a Collada file
    // whose only content is an unresolved cross-document instance_geometry is the reachable case, E10).
    // One warning; NEVER a failure -- the panel shows the counts, which is more useful than a refusal.
    if ((out.scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0U) {
        addWarning(result, "the file parsed as INCOMPLETE; some referenced content could not be resolved");
    }
    return true;
}

// task 3.2.5 (A-6). EXACT ZERO, never an epsilon: an epsilon test would delete a legitimately tiny UV
// set, and the rule this implements is about the REPRESENTATION ("a zero normal is indistinguishable
// from an absent one"), not about smallness.
[[nodiscard]] bool isExactlyZero(const Vec2& v) noexcept { return v.x == 0.0F && v.y == 0.0F; }
[[nodiscard]] bool isExactlyZero(const Vec3& v) noexcept { return v.x == 0.0F && v.y == 0.0F && v.z == 0.0F; }
[[nodiscard]] bool isExactlyZero(const Vec4& v) noexcept {
    return v.x == 0.0F && v.y == 0.0F && v.z == 0.0F && v.w == 0.0F;
}

// task 3.2.5 (A-6). The ONE useful thing aiProcess_FindInvalidData would have done, taken over so the
// MESH SURVIVES it -- that flag DELETES a mesh it finds invalid, and throws "No meshes remaining" when
// it deletes them all, which collides head-on with 3.2.1's D11 (an empty mesh survives so the panel can
// show the user why their file looks empty). `channel` is prose for the warning; the array is cleared
// in place and its attribute bit therefore never set (INV-A5 does the rest).
//
// POSITIONS ARE NEVER PASSED HERE: an all-zero position array is legitimate geometry at the origin.
template <typename T>
void dropAllZeroChannel(std::vector<T>& channel, std::string_view name, std::string_view meshName,
                        ImportResult& result) {
    if (channel.empty()) {
        return;
    }
    for (const T& value : channel) {
        if (!isExactlyZero(value)) {
            return;
        }
    }
    channel.clear();
    addWarning(result, std::format("mesh '{}': the {} channel was entirely zero and was dropped -- a zero "
                                   "value is indistinguishable from an absent one",
                                   meshName, name));
}

// task 3.2.5 (A-14/A-15/INV-A4/INV-A5). ONE ImportedMesh per aiMesh, ONE ImportedPrimitive inside it --
// aiMesh already carries exactly one mMaterialIndex, so 3.2.3's first-appearance bucketing has no
// analogue here. Runs at Full depth only: at Structure, a mesh keeps its IDENTITY (name, localId, an
// EMPTY primitive list) and .stl/.ply never reach here at all.
void convertMeshes(const aiScene& scene, const ImportSettings& settings, ImportDepth depth, ImportResult& result) {
    std::size_t totalVertices = 0;
    std::size_t totalIndices = 0;
    std::size_t totalPrimitives = 0;
    bool vertexCapReported = false;
    bool indexCapReported = false;
    bool primitiveCapReported = false;

    for (unsigned int mi = 0; mi < scene.mNumMeshes; ++mi) {
        const aiMesh& src = *scene.mMeshes[mi];
        ImportedMesh outMesh;
        outMesh.name = src.mName.C_Str();
        outMesh.localId = mi;  // the source's own mesh index (3.2.1's D13)

        // An empty mesh gets a POINT box (Aabb{}), NEVER the Aabb::empty() sentinel, whose NaN centre
        // would leak into the model bounds. Both shipped backends already do exactly this.
        const auto emitEmpty = [&result](ImportedMesh& mesh) {
            mesh.bounds = Aabb{};
            result.model.meshes.push_back(std::move(mesh));
        };

        if (depth == ImportDepth::Structure) {
            emitEmpty(outMesh);  // identity survives, content does not -- glTF's own split
            continue;
        }
        if (totalPrimitives >= MAX_PRIMITIVES_PER_MODEL) {
            if (!primitiveCapReported) {
                primitiveCapReported = true;
                escalate(result, ImportStatus::Truncated,
                         "the primitive count exceeds this importer's per-model limit");
            }
            emitEmpty(outMesh);
            continue;
        }

        ImportedPrimitive prim;
        prim.materialIndex = src.mMaterialIndex;  // RAW; remapped by applyMaterialMap, never used as-is

        const std::size_t vertexCount = src.mNumVertices;
        if (totalVertices + vertexCount > MAX_VERTICES_PER_MODEL) {
            if (!vertexCapReported) {
                vertexCapReported = true;
                escalate(result, ImportStatus::Truncated, "the vertex count exceeds this importer's per-model limit");
            }
            emitEmpty(outMesh);  // a COHERENT smaller model: this mesh contributes no primitive at all
            continue;
        }

        prim.positions.reserve(vertexCount);
        for (unsigned int v = 0; v < src.mNumVertices; ++v) {
            prim.positions.push_back(toVec3(src.mVertices[v]) * settings.scale);  // A22: POSITIONS only
        }
        if (src.HasNormals()) {
            prim.normals.reserve(vertexCount);
            for (unsigned int v = 0; v < src.mNumVertices; ++v) {
                prim.normals.push_back(toVec3(src.mNormals[v]));  // NEVER scaled
            }
        }
        // TANGENTS only when the loader supplied BOTH tangents and bitangents. aiProcess_CalcTangentSpace
        // is FORBIDDEN (A-6), so in practice this stays empty for all three formats and the arm exists to
        // be CORRECT rather than because it is expected to fire. `.w` is glTF's bitangent SIGN.
        if (src.mTangents != nullptr && src.mBitangents != nullptr && src.HasNormals()) {
            prim.tangents.reserve(vertexCount);
            for (unsigned int v = 0; v < src.mNumVertices; ++v) {
                const Vec3 n = toVec3(src.mNormals[v]);
                const Vec3 t = toVec3(src.mTangents[v]);
                const Vec3 b = toVec3(src.mBitangents[v]);
                const float sign = dot(cross(n, t), b) < 0.0F ? -1.0F : 1.0F;
                prim.tangents.push_back(Vec4{t.x, t.y, t.z, sign});
            }
        }
        for (unsigned int set = 0; set < 2; ++set) {
            if (src.mTextureCoords[set] == nullptr) {
                continue;
            }
            std::vector<Vec2>& target = set == 0 ? prim.uv0 : prim.uv1;
            target.reserve(vertexCount);
            for (unsigned int v = 0; v < src.mNumVertices; ++v) {
                target.push_back(Vec2{src.mTextureCoords[set][v].x, src.mTextureCoords[set][v].y});
            }
            if (src.mNumUVComponents[set] == 3) {
                // A 3D texture coordinate has NO field here. ONE warning per SET, never per vertex.
                addWarning(result, std::format("mesh '{}': UV set {} is 3-dimensional; the third component "
                                               "was dropped",
                                               outMesh.name, set));
            }
        }
        if (src.mColors[0] != nullptr) {
            prim.colors.reserve(vertexCount);
            for (unsigned int v = 0; v < src.mNumVertices; ++v) {
                const aiColor4D& c = src.mColors[0][v];
                prim.colors.push_back(Vec4{c.r, c.g, c.b, c.a});  // already RGBA; `a` verbatim
            }
        }

        dropAllZeroChannel(prim.normals, "normals", outMesh.name, result);
        dropAllZeroChannel(prim.tangents, "tangents", outMesh.name, result);
        dropAllZeroChannel(prim.uv0, "UV set 0", outMesh.name, result);
        dropAllZeroChannel(prim.uv1, "UV set 1", outMesh.name, result);
        dropAllZeroChannel(prim.colors, "vertex colours", outMesh.name, result);

        // INV-A4: EVERY index range-checked against mNumVertices BEFORE it is written. A face failing any
        // check is dropped WHOLE, never partially, with one capped warning.
        //
        // MEASURED, AND RECORDED SO IT IS NOT MISTAKEN FOR LIVE COVER: while aiProcess_ValidateDataStructure
        // is on (A-6b) this check is UNREACHABLE -- ValidateDSProcess runs FIRST, before every other
        // post-process step, and refuses an out-of-range index outright (AI31). Nothing downstream can
        // introduce one either. The flag is reversible in one token (R5) and this is what stands behind it
        // when it is, so the check stays and AI34 pins it in the source text.
        std::size_t droppedFaces = 0;
        std::size_t nonTriangleFaces = 0;
        prim.indices.reserve(static_cast<std::size_t>(src.mNumFaces) * 3U);
        for (unsigned int f = 0; f < src.mNumFaces; ++f) {
            const aiFace& face = src.mFaces[f];
            if (face.mNumIndices != 3) {
                ++nonTriangleFaces;  // unreachable after aiProcess_Triangulate; defence in depth
                continue;
            }
            bool inRange = true;
            for (unsigned int k = 0; k < 3; ++k) {
                if (face.mIndices[k] >= src.mNumVertices) {
                    inRange = false;
                    break;
                }
            }
            if (!inRange) {
                ++droppedFaces;
                continue;
            }
            if (totalIndices + 3U > MAX_INDICES_PER_MODEL) {
                if (!indexCapReported) {
                    indexCapReported = true;
                    escalate(result, ImportStatus::Truncated,
                             "the index count exceeds this importer's per-model limit");
                }
                break;
            }
            for (unsigned int k = 0; k < 3; ++k) {
                prim.indices.push_back(face.mIndices[k]);
            }
            totalIndices += 3U;
        }
        if (droppedFaces > 0) {
            addWarning(result, std::format("mesh '{}': {} face(s) referenced a vertex outside the mesh and "
                                           "were dropped",
                                           outMesh.name, droppedFaces));
        }
        if (nonTriangleFaces > 0) {
            addWarning(result, std::format("mesh '{}': {} face(s) were not triangles after triangulation and "
                                           "were dropped",
                                           outMesh.name, nonTriangleFaces));
        }

        // INV-A5: positions and indices are NEVER empty on a primitive that survives, and `attributes`
        // never claims a bit whose array is empty.
        if (!prim.indices.empty() && !prim.positions.empty()) {
            prim.attributes = VertexAttribute::Position;
            if (!prim.normals.empty()) {
                prim.attributes |= VertexAttribute::Normal;
            }
            if (!prim.tangents.empty()) {
                prim.attributes |= VertexAttribute::Tangent;
            }
            if (!prim.uv0.empty()) {
                prim.attributes |= VertexAttribute::TexCoord0;
            }
            if (!prim.uv1.empty()) {
                prim.attributes |= VertexAttribute::TexCoord1;
            }
            if (!prim.colors.empty()) {
                prim.attributes |= VertexAttribute::Color0;
            }
            Aabb primBounds = Aabb::empty();
            for (const Vec3& p : prim.positions) {
                primBounds.expand(p);
            }
            prim.bounds = primBounds;
            result.model.summary.bounds.expand(primBounds);  // FROM THE PRIMITIVE, never from the mesh
            result.model.summary.vertexCount += prim.positions.size();
            result.model.summary.triangleCount += prim.indices.size() / 3U;
            totalVertices += prim.positions.size();
            ++totalPrimitives;
            outMesh.primitives.push_back(std::move(prim));
        } else if (src.mNumFaces == 0) {
            // The MESH SURVIVES with zero primitives so the panel can show the user WHY their file looks
            // empty (3.2.1's D11). Unreachable while validation is on -- ValidateDS refuses a mesh with no
            // faces before we see it -- and kept for the same reason the range check above is.
            addWarning(result, std::format("mesh '{}': no triangles survived; point and line primitives are "
                                           "not imported",
                                           outMesh.name));
        }

        Aabb meshBounds = Aabb::empty();
        for (const ImportedPrimitive& p : outMesh.primitives) {
            meshBounds.expand(p.bounds);
        }
        outMesh.bounds = meshBounds.valid() ? meshBounds : Aabb{};
        result.model.summary.primitiveCount += outMesh.primitives.size();
        result.model.meshes.push_back(std::move(outMesh));
    }
    result.model.summary.meshCount = result.model.meshes.size();
}

// task 3.2.5 (A-12). ITERATIVE, depth-bounded, and localId == the node's position in
// ImportedModel::nodes BY CONSTRUCTION. misc-no-recursion is --warnings-as-errors on the Linux lane, but
// the deeper reason is that a .dae can declare an arbitrarily deep <node> chain and a recursive walk
// would be a stack overflow on a hostile file, which no sanitizer reports as anything but a crash.
//
// The coincidence localId == index is real for glTF and OBJ too and FALSE for FBX -- and 3.2.2's
// BLOCKING ASan heap-buffer-overflow was import_details_panel.cpp leaning on it. The panel's
// localId->index map still runs and still must; AI37 pins nodes[i].localId == i so a future change that
// breaks the coincidence is CAUGHT rather than discovered.
void convertNodes(const aiScene& scene, const ImportSettings& settings, ImportResult& result) {
    if (scene.mRootNode == nullptr) {
        return;  // defensive: every loader that produces no root throws instead
    }
    struct Pending {
        const aiNode* node = nullptr;
        std::uint32_t parent = INVALID_SUBASSET;
        std::uint32_t depth = 0;
    };
    std::vector<Pending> stack;
    stack.push_back(Pending{scene.mRootNode, INVALID_SUBASSET, 0});
    bool depthReported = false;
    bool nodeCapReported = false;

    while (!stack.empty()) {
        const Pending top = stack.back();
        stack.pop_back();

        if (top.depth > MAX_NODE_DEPTH) {
            if (!depthReported) {
                depthReported = true;
                escalate(result, ImportStatus::Truncated, "the node depth exceeds this importer's per-model limit");
            }
            continue;  // the SUBTREE is dropped; everything already emitted stays coherent
        }
        if (result.model.nodes.size() >= MAX_NODES_PER_MODEL) {
            if (!nodeCapReported) {
                nodeCapReported = true;
                escalate(result, ImportStatus::Truncated, "the node count exceeds this importer's per-model limit");
            }
            continue;
        }

        const auto index = static_cast<std::uint32_t>(result.model.nodes.size());
        ImportedNode out;
        out.name = top.node->mName.C_Str();
        out.localId = index;  // A-12: the walk position IS the id
        out.parent = top.parent;

        aiVector3D scaling;
        aiQuaternion rotation;
        aiVector3D position;
        top.node->mTransformation.Decompose(scaling, rotation, position);
        out.translation = toVec3(position);
        out.rotation = toQuat(rotation);  // A-14 trap 2, one of THREE call sites
        out.scale = toVec3(scaling);
        if (top.parent == INVALID_SUBASSET) {
            // import_settings.hpp's own rule: ROOT translations only. A non-root node's translation is
            // already expressed in its parent's scaled space.
            out.translation = out.translation * settings.scale;
        }
        if (top.node->mNumMeshes > 0) {
            out.meshIndex = top.node->mMeshes[0];
        }

        if (top.parent != INVALID_SUBASSET) {
            result.model.nodes[top.parent].children.push_back(index);  // never INVALID_SUBASSET
        } else {
            result.model.roots.push_back(index);
        }
        result.model.nodes.push_back(std::move(out));

        // A node referencing SEVERAL meshes gets one child per extra mesh: ImportedNode::meshIndex holds
        // exactly ONE index and a second is not representable. The split is NAMED (3.2.2's multiple-skin-
        // deformer precedent) -- one warning per node, giving the count.
        if (top.node->mNumMeshes > 1) {
            addWarning(result,
                       std::format("node '{}': {} meshes; split into {} extra child nodes",
                                   result.model.nodes[index].name, top.node->mNumMeshes, top.node->mNumMeshes - 1));
            for (unsigned int m = 1; m < top.node->mNumMeshes; ++m) {
                if (result.model.nodes.size() >= MAX_NODES_PER_MODEL) {
                    break;  // the shared cap message above already fired or will
                }
                const auto extraIndex = static_cast<std::uint32_t>(result.model.nodes.size());
                ImportedNode extra;
                extra.name = std::format("{}.{}", result.model.nodes[index].name, m);
                extra.localId = extraIndex;
                extra.parent = index;
                extra.meshIndex = top.node->mMeshes[m];
                result.model.nodes[index].children.push_back(extraIndex);
                result.model.nodes.push_back(std::move(extra));
            }
        }

        // PRE-ORDER with a stack means pushing children in REVERSE, so the first child is popped first
        // and `nodes` reads in document order. Getting this backwards is invisible in a one-child fixture
        // and wrong in every real file -- AI38's three-child fixture is what pins it.
        for (unsigned int c = top.node->mNumChildren; c > 0; --c) {
            stack.push_back(Pending{top.node->mChildren[c - 1], index, top.depth + 1});
        }
    }
    result.model.summary.nodeCount = result.model.nodes.size();
}

// 3.2.3's BLOCKING gap 1, avoided by construction rather than found by review: a RAW mMaterialIndex must
// never survive into ImportedPrimitive. The two index spaces coincide for every well-formed file and
// diverge the moment MAX_MATERIALS_PER_MODEL trims the tail -- or the moment importMaterials is false,
// which passes an EMPTY map here and is exactly what AC-48 requires, with no second code path.
void applyMaterialMap(const std::vector<std::uint32_t>& rawToConverted, ImportedModel& model) {
    for (ImportedMesh& mesh : model.meshes) {
        for (ImportedPrimitive& prim : mesh.primitives) {
            prim.materialIndex =
                prim.materialIndex < rawToConverted.size() ? rawToConverted[prim.materialIndex] : INVALID_SUBASSET;
        }
    }
}

// AC-19: the .ply Structure and Full passes must produce the IDENTICAL externalUris, entry for entry
// and order for order. Structure produces it from the header scan; Full seeds it from the SAME scan
// first, so the order is the header's. Lifted into one helper so the two call sites cannot drift.
void seedPlyExternalUris(std::span<const std::byte> bytes, std::string_view assetRelativeDir, ImportResult& result) {
    for (const std::string& name : scanPlyTextureFiles(bytes, MAX_EXTERNAL_URIS)) {
        const std::string folded = foldBackslashesToSlashes(name);  // A-19: fold BEFORE classify
        const UriClassification classified = classifyUri(folded, assetRelativeDir);
        if (classified.kind != UriClass::RelativePath) {
            continue;
        }
        bool seen = false;
        for (const std::string& existing : result.externalUris) {
            if (existing == classified.relativePath) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            result.externalUris.push_back(classified.relativePath);
        }
    }
}

// task 3.2.5 (A-7). The .stl and .ply arms share everything except their Structure pass, because
// neither enters the library there and both are one flat mesh at Full.
[[nodiscard]] ImportResult importStlOrPly(bool isPly, std::string_view assetRelativeDir,
                                          std::span<const std::byte> bytes, const ImportSettings& settings,
                                          ImportDepth depth) {
    ImportResult result;

    if (depth == ImportDepth::Structure) {
        // A-7: the LEAST work that produces the EXACT URI set, and for these two formats that means the
        // library is never entered at all.
        //   .stl -- STLLoader opens only pFile and mints one default material with NO texture, so the
        //           exact set is provably {} and there is nothing to scan.
        //   .ply -- the ONLY external reference a .ply can carry is a `TextureFile` line in its ASCII
        //           header, terminated by `end_header`, so a bounded header scan produces the set
        //           exactly, at a cost independent of the file's size.
        // This is a STATED DEVIATION from INV-M4 (counts/names/hierarchy disagree between the depths),
        // exactly as .obj has been since 3.2.3. AC-19 asserts the one thing the two depths DO owe.
        if (isPly) {
            seedPlyExternalUris(bytes, assetRelativeDir, result);
        }
        return result;  // status Ok, model EMPTY -- AC-18
    }

    if (isPly) {
        // AC-19's other half: seeded from the SAME scan the Structure pass uses, and seeded BEFORE
        // anything below can return, so the two depths agree about the URI set even when this import
        // goes on to REFUSE the file. The material pass's find-or-append dedup then collapses the
        // loader's own identical name onto this entry rather than appending a second one.
        seedPlyExternalUris(bytes, assetRelativeDir, result);

        // R8, and the ONE pre-allocation bound in this task: a .ply header that lies about its element
        // counts sends the loader into an unbounded grind (MEASURED -- see the function's own header
        // comment), and .ply reaches that from a ~120-byte file, so MAX_MODEL_FILE_BYTES does not cover
        // it. Malformed, not ParseFailed: the header PARSED, and it is what it says that is wrong.
        if (plyDeclaredCountsExceedBytes(bytes)) {
            result.status = ImportStatus::Malformed;
            result.message = "this PLY header declares more elements than the file contains";
            return result;
        }
    }

    AssimpScene loaded;
    if (!runAssimp(bytes, isPly ? "ply" : "stl", isPly ? PLY_LOADER_FRAGMENT : STL_LOADER_FRAGMENT, loaded, result)) {
        return result;
    }
    // convertMeshes -> convertNodes for these two formats, because neither has a skin or an animation and
    // the node pass only needs the mesh count. .dae's order is the OTHER way round and that one IS a
    // dependency -- do not "harmonise" them.
    convertMeshes(*loaded.scene, settings, depth, result);
    convertNodes(*loaded.scene, settings, result);
    applyMaterialMap(std::vector<std::uint32_t>{}, result.model);
    // .ply/.stl declare no unit and no axis, so SourceSpace stays ALL-DEFAULT (declared == false) and
    // the panel draws no Source Space row for them. Inventing one is the option this task's scoping
    // rejected; the row means something precisely because it is absent when the format declares nothing.
    return result;
}

// task 3.2.5 (A-7). Collada: a real parse at BOTH depths, with only the sample data skipped at
// Structure -- glTF's own shape, and the reason is that a .dae's texture paths are <library_images>
// entries resolved through <effect>s, with no cheap way to get the EXACT set.
[[nodiscard]] ImportResult importDae(std::string_view assetRelativeDir, std::span<const std::byte> bytes,
                                     const ImportSettings& settings, ImportDepth depth) {
    ImportResult result;

    // A-10: BEFORE the parse, from the raw bytes, because the loader CONSUMES <unit> and <up_axis> into
    // the root node's transformation and exposes neither afterwards. DISPLAY-ONLY.
    result.model.sourceSpace = scanColladaAssetSpace(bytes);

    AssimpScene loaded;
    if (!runAssimp(bytes, "dae", DAE_LOADER_FRAGMENT, loaded, result)) {
        return result;
    }
    // The two PROSE fields SourceSpace carries. Collada does put <contributor>'s children into the
    // metadata map (unlike <unit>/<up_axis>), so `authoring_tool` is readable; the format version comes
    // from AI_METADATA_SOURCE_FORMAT_VERSION, which the parser writes.
    if (loaded.scene->mMetaData != nullptr) {
        aiString tool;
        if (loaded.scene->mMetaData->Get("authoring_tool", tool)) {
            result.model.sourceSpace.generator = tool.C_Str();
        }
        aiString version;
        if (loaded.scene->mMetaData->Get(AI_METADATA_SOURCE_FORMAT_VERSION, version)) {
            result.model.sourceSpace.formatVersion = std::format("COLLADA {}", version.C_Str());
        }
    }
    (void)settings;
    (void)depth;
    (void)assetRelativeDir;  // step 6 wires it into convertMaterials; named here so the signature holds
    return result;
}

}  // namespace

ImportResult importAssimp(std::string_view fileName, std::string_view assetRelativeDir,
                          std::span<const std::byte> bytes, const ImportSettings& settings, ImportDepth depth,
                          std::span<const ExternalBuffer> external) {
    // A-8: NEVER READ. modelImporterNeedsExternalBuffers is false for all three extensions, so
    // ModelImportSession skips its whole first pass and supplies an empty span. The parameter exists
    // only so this signature matches importObj's; importMtlOnly ignores its own for the same reason.
    (void)external;

    // A-17: NO exception crosses the public API. Assimp swallows its OWN (ReadFileFromMemory catches
    // DeadlyImportError and (...) and returns nullptr), so unlike 3.2.3 this catch guards OUR conversion
    // -- which allocates std::vectors sized from user-controlled counts and can therefore reach
    // std::bad_alloc and std::length_error. The divergence in REASON is recorded rather than left to be
    // re-derived.
    try {
        if (endsWithFoldedLocal(fileName, ".dae")) {
            return importDae(assetRelativeDir, bytes, settings, depth);
        }
        return importStlOrPly(endsWithFoldedLocal(fileName, ".ply"), assetRelativeDir, bytes, settings, depth);
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

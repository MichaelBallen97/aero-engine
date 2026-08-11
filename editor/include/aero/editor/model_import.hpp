#pragma once
// Aero Engine — the canonical, format-agnostic, third-party-free imported model (task 3.2.1).
// PUBLIC, and the asset_cache.hpp / asset_meta.hpp shape verbatim: free of ImGui, SDL, entt,
// <filesystem>, <fstream> and every build gate. NOTHING HERE LOGS (INV-A3, an eighth application) --
// status is RETURNED, never printed.
//
// NO FASTGLTF ANYWHERE IN THIS HEADER OR ITS .cpp (INV-M1/AC-55). The glTF backend lives behind the
// src-private editor/src/gltf_import.hpp, exactly as the stb_image decoder lives behind
// editor/src/thumbnail_store.hpp (task 3.1.3, D18). importModel() dispatches on the file NAME and
// calls importGltf(); 3.2.2's ufbx, 3.2.3's tinyobjloader and 3.2.5's Assimp each add ONE arm here and
// ONE TU beside gltf_import.cpp, and touch nothing else.
//
// THE IMPORTER PERFORMS ZERO FILE READS (INV-M3). Every byte arrives as a parameter. That is what
// makes this surface provable from string literals with no disk on the critical path -- and it is what
// stops a user-supplied document from naming a file the editor then reads (D3, reason 1).
#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>
#include <aero/editor/import_settings.hpp>
#include <aero/editor/scene_bounds.hpp>  // Aabb -- REUSED, never re-declared (F8). Brings aero::scene,
                                         // a PUBLIC dep of aero_editor_core that is NOT gated on
                                         // AERO_REFLECT_TOOLS, so both reduced configurations build.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// D13: "no sub-asset here" -- a SENTINEL, never index 0. Index 0 is a real mesh, a real material and a
// real image, so INVALID_SUBASSET / nullopt is the only honest "absent" (the A4 engagement rule).
inline constexpr std::uint32_t INVALID_SUBASSET = 0xFFFFFFFFU;

// ---- D15's caps. Each declared BESIDE the type it bounds, each checked BEFORE the allocation it
// bounds -- with exactly TWO documented exceptions. The first is MAX_EMBEDDED_BYTES (plan §A-8):
// fastgltf's Parser::decodeDataUri allocates a data: URI's decoded payload DURING PARSE, before any of
// our code runs, so that cap is necessarily checked after the fact. The real pre-allocation bound for
// embedded data is MAX_MODEL_FILE_BYTES, which readFileBytes enforces WITHOUT OPENING THE FILE. The
// second is every cap the Assimp backend applies (task 3.2.5, see MAX_NODE_DEPTH below), for the
// identical reason: ReadFileFromMemory returns a fully-built aiScene or nothing.
inline constexpr std::uint64_t MAX_MODEL_FILE_BYTES = 256ULL * 1024 * 1024;
inline constexpr std::uint64_t MAX_EXTERNAL_BYTES_PER_MODEL = 512ULL * 1024 * 1024;
inline constexpr std::uint64_t MAX_EMBEDDED_BYTES = 128ULL * 1024 * 1024;  // checked AFTER parse (A8)
inline constexpr std::size_t MAX_EXTERNAL_URIS = 1024;
inline constexpr std::size_t MAX_NODES_PER_MODEL = 65536;
// task 3.2.3 (D18): bounds ImportedModel::materials. A .mtl is bounded only by MAX_MODEL_FILE_BYTES,
// and a 4 MB .mtl can declare tens of thousands of materials, each carrying five
// std::optional<ImportedTextureRef> and several strings. Checked BEFORE the reserve.
// RECORDED RATHER THAN SILENTLY UNEVEN: the glTF backend does NOT apply this cap today
// (asset.materials is uncapped there). That is a PRE-EXISTING gap; closing it is deliberately not this
// task's change, because "zero diff to gltf_import.{hpp,cpp}" is worth more than a cap nobody has hit.
inline constexpr std::size_t MAX_MATERIALS_PER_MODEL = 65536;
inline constexpr std::size_t MAX_PRIMITIVES_PER_MODEL = 4096;
inline constexpr std::size_t MAX_VERTICES_PER_MODEL = 8000000;
inline constexpr std::size_t MAX_INDICES_PER_MODEL = 24000000;
inline constexpr std::size_t MAX_JOINTS_PER_SKIN = 1024;
inline constexpr std::size_t MAX_ANIMATION_KEYS_PER_MODEL = 2000000;
// The value AND the shape of MAX_REPORTED_PER_CATEGORY (asset_meta.hpp:29), restated rather than
// included: a capped list plus an uncapped total. Do not include asset_meta.hpp for one integer.
inline constexpr std::size_t MAX_IMPORT_WARNINGS = 20;
// A6: how many extension names the MissingExtensions message lists before it says "and N more".
// extensionsRequired is user-supplied and unbounded, so the JOIN is capped exactly as the warning
// list is -- D15's posture applied to a STRING rather than to an allocation.
inline constexpr std::size_t MAX_REPORTED_REQUIRED_EXTENSIONS = 8;

// ---- task 3.2.2 (D16). Unlike the glTF caps above, these are enforced INSIDE ufbx -- its allocator
// options refuse BEFORE the allocation and report UFBX_ERROR_MEMORY_LIMIT / ALLOCATION_LIMIT /
// NODE_DEPTH_LIMIT, which map to ImportStatus::Truncated. That is strictly better than a post-hoc
// check. node_depth_limit matters more than it looks: ufbx's own header warns that the default of 0
// allows arbitrarily deep hierarchies, and misc-no-recursion is --warnings-as-errors here, so this
// tree's walk is iterative anyway -- the limit is defence for ufbx's internals, not for ours.
inline constexpr std::uint32_t MAX_FBX_NODE_DEPTH = 256;                    // -> opts.node_depth_limit
inline constexpr std::size_t MAX_FBX_TEMP_BYTES = 1024ULL * 1024 * 1024;    // -> temp_allocator.memory_limit
inline constexpr std::size_t MAX_FBX_RESULT_BYTES = 1024ULL * 1024 * 1024;  // -> result_allocator.memory_limit
inline constexpr std::size_t MAX_FBX_ALLOCATIONS = 4000000;                 // -> both allocation_limits

// ---- task 3.2.5 (A-12/A-15): bounds the node walk. Assimp has NO node-depth limit of its own
// (unlike ufbx, whose node_depth_limit MAX_FBX_NODE_DEPTH sets), and a .dae can declare an
// arbitrarily deep <node> chain. The walk is ITERATIVE (misc-no-recursion is --warnings-as-errors),
// so this bounds MEMORY and TIME, never the stack.
//
// NOTE, and this is a real difference from 3.2.2: every Assimp cap this backend applies is checked
// AFTER the library has allocated, because ReadFileFromMemory returns a fully-built aiScene or
// nothing. The real PRE-allocation bound is MAX_MODEL_FILE_BYTES, which readFileBytes enforces
// WITHOUT OPENING THE FILE -- the MAX_EMBEDDED_BYTES position above, a second occupant.
inline constexpr std::uint32_t MAX_NODE_DEPTH = 256;

// ---- attributes ------------------------------------------------------------------------------------
// A BITSET, not booleans: 3.3.1 switches on the COMBINATION to choose a vertex layout, and "which
// layout is this" is asked far more often than it is computed.
// std::uint32_t is DELIBERATE headroom for a future attribute (unlike every other enum in this header,
// which uses uint8_t) -- NOLINT below overrides performance-enum-size, which measures only today's 8
// bits and would otherwise ask for uint8_t.
// NOLINTNEXTLINE(performance-enum-size)
enum class VertexAttribute : std::uint32_t {
    None = 0,
    Position = 1U << 0,
    Normal = 1U << 1,
    Tangent = 1U << 2,
    TexCoord0 = 1U << 3,
    TexCoord1 = 1U << 4,
    Color0 = 1U << 5,
    Joints0 = 1U << 6,
    Weights0 = 1U << 7,
};
[[nodiscard]] constexpr VertexAttribute operator|(VertexAttribute a, VertexAttribute b) noexcept {
    return static_cast<VertexAttribute>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr VertexAttribute& operator|=(VertexAttribute& a, VertexAttribute b) noexcept { return a = a | b; }
[[nodiscard]] constexpr bool has(VertexAttribute set, VertexAttribute bit) noexcept {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(bit)) != 0U;
}

// ---- geometry --------------------------------------------------------------------------------------
// D5: PARALLEL ARRAYS. EMPTY == ABSENT, with exactly two exceptions -- `positions` and `indices` are
// ALWAYS non-empty on a primitive that survives import (INV-M5/F6). A single fat vertex would be 96
// bytes of which most is zeroes, and would make "does this primitive have tangents?" a question no
// field can answer, since a zero tangent is indistinguishable from an absent one.
struct ImportedPrimitive {
    std::uint32_t materialIndex = INVALID_SUBASSET;  // into ImportedModel::materials
    VertexAttribute attributes = VertexAttribute::None;
    std::vector<Vec3> positions;  // POST-scale (A22)
    std::vector<Vec3> normals;    // NEVER scaled
    std::vector<Vec4> tangents;   // .w is glTF's bitangent SIGN (+1/-1), never a magnitude. NEVER scaled.
    std::vector<Vec2> uv0;
    std::vector<Vec2> uv1;
    std::vector<Vec4> colors;                          // linear RGBA; a VEC3 source is widened with a = 1
    std::vector<std::array<std::uint16_t, 4>> joints;  // read as fastgltf::math::u16vec4, copied here
    std::vector<Vec4> weights;
    std::vector<std::uint32_t> indices;  // ALWAYS present; width normalized to u32 regardless of the
                                         // source's UNSIGNED_BYTE/SHORT/INT (F6/AC-24)
    Aabb bounds;                         // post-scale, folded from `positions` -- NEVER read from the document's own
                  // accessor min/max (plan A21): those are a CLAIM, not a fact, and predate `scale`
};

struct ImportedMesh {
    std::string name;           // the source's own `name`, verbatim; "" when absent (D13)
    std::uint32_t localId = 0;  // the source file's own mesh index (D13)
    std::vector<ImportedPrimitive> primitives;
    Aabb bounds;  // union of the primitives'; a POINT box when every primitive was skipped (D11) --
                  // an empty mesh SURVIVES so the panel can show the user why their file looks empty
};

// ---- materials -------------------------------------------------------------------------------------
// NOTE (plan A13): AlphaMode, AnimationPath and AnimationInterpolation each collide by NAME with a
// fastgltf enum. gltf_import.cpp is inside namespace engine::editor, so an unqualified name binds to
// OURS; fastgltf's must always be spelled fastgltf::X. NEVER write `using namespace fastgltf;`.
enum class AlphaMode : std::uint8_t { Opaque = 0, Mask, Blend };
enum class TextureWrap : std::uint8_t { Repeat = 0, ClampToEdge, MirroredRepeat };
enum class TextureFilter : std::uint8_t { Nearest = 0, Linear };
enum class MipFilter : std::uint8_t { None = 0, Nearest, Linear };

struct ImportedTextureRef {
    std::uint32_t imageIndex = INVALID_SUBASSET;  // into ImportedModel::images
    std::uint32_t uvSet = 0;                      // the n in TEXCOORD_n
    TextureWrap wrapU = TextureWrap::Repeat;      // glTF's documented default when no sampler (AC-28)
    TextureWrap wrapV = TextureWrap::Repeat;
    TextureFilter minFilter = TextureFilter::Linear;
    TextureFilter magFilter = TextureFilter::Linear;
    MipFilter mipFilter = MipFilter::Linear;
};

struct ImportedMaterial {
    std::string name;
    std::uint32_t localId = 0;
    Vec4 baseColorFactor = Vec4::one();
    float metallicFactor = 1.0F;
    float roughnessFactor = 1.0F;
    Vec3 emissiveFactor{};
    float normalScale = 1.0F;
    float occlusionStrength = 1.0F;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5F;
    bool doubleSided = false;
    // std::nullopt, NEVER a zero index -- index 0 is a real image (the A4 engagement rule, a third
    // use). Resolution is TWO hops (plan A10): the material slot's TextureInfo::textureIndex, then
    // Texture::imageIndex -- and BOTH can be absent.
    std::optional<ImportedTextureRef> baseColor;
    std::optional<ImportedTextureRef> metallicRoughness;
    std::optional<ImportedTextureRef> normal;
    std::optional<ImportedTextureRef> occlusion;
    std::optional<ImportedTextureRef> emissive;
};

// ---- images (references only -- nothing is EVER decoded here, D20) ----------------------------------
struct ImportedImage {
    std::string uri;           // as fastgltf produced it: ALREADY PERCENT-DECODED (plan A1); "" if embedded
    std::string relativePath;  // resolved, project-relative, '/'-separated; "" when unresolved/refused
    Guid guid;                 // nil unless `relativePath` names a known asset. A nil is NEVER a dependency.
    std::string mimeType;      // "" when the source gave none
    bool embedded = false;     // a data: URI or a GLB bufferView; NEVER a dependency (D14)
    std::string refusal;       // "" unless D14 refused it; the EXACT reason, shown by the panel -- this
                               // is what makes a broken texture reference visible instead of mysterious
};

// ---- hierarchy -------------------------------------------------------------------------------------
struct ImportedNode {
    std::string name;
    std::uint32_t localId = 0;
    std::uint32_t parent = INVALID_SUBASSET;
    std::vector<std::uint32_t> children;  // never contains INVALID_SUBASSET (plan A23)
    Vec3 translation{};                   // ALWAYS TRS; a `matrix` source is decomposed (F5)
    Quat rotation = Quat::identity();
    Vec3 scale = Vec3::one();
    std::uint32_t meshIndex = INVALID_SUBASSET;
    std::uint32_t skinIndex = INVALID_SUBASSET;
};

// ---- skins -----------------------------------------------------------------------------------------
struct ImportedSkin {
    std::string name;
    std::uint32_t localId = 0;
    std::vector<std::uint32_t> joints;              // NODE indices, SOURCE ORDER (AC-31)
    std::vector<Mat4> inverseBindMatrices;          // EXACTLY joints.size() at Full depth (INV-M7); identity
                                                    // when the source omitted the accessor (AC-32), never
                                                    // empty; deliberately EMPTY at Structure depth
    std::uint32_t skeletonRoot = INVALID_SUBASSET;  // recorded as-is even when not a joint (E24)
};

// ---- animation -------------------------------------------------------------------------------------
// No `Weights` enumerator, DELIBERATELY (D12) -- adding one later is additive (AssetMetaState::
// Reattached's precedent); shipping one nothing produces is a lie no switch can catch.
enum class AnimationPath : std::uint8_t { Translation = 0, Rotation, Scale };
enum class AnimationInterpolation : std::uint8_t { Linear = 0, Step, CubicSpline };

struct ImportedAnimationChannel {
    std::uint32_t targetNode = INVALID_SUBASSET;
    AnimationPath path = AnimationPath::Translation;
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
    std::vector<float> times;  // seconds, STRICTLY INCREASING (enforced on import, AC-34)
    // One Vec4 per VALUE. Translation/Scale use .xyz (w left 0); Rotation uses all four in glTF's own
    // {x,y,z,w} order. CUBICSPLINE stores THREE values per key, in glTF's own order -- inTangent,
    // value, outTangent -- so
    //     values.size() == times.size() * (interpolation == CubicSpline ? 3 : 1)
    // on every imported channel (INV-M6), enforced on import and re-asserted by MI68.
    std::vector<Vec4> values;
};

struct ImportedAnimation {
    std::string name;
    std::uint32_t localId = 0;
    float duration = 0.0F;  // max time across channels; 0 for a clip with no samples AND at Structure depth
    std::vector<ImportedAnimationChannel> channels;
};

// The source document's own declared space, BEFORE conversion (task 3.2.2, D19). ALL-DEFAULT for
// glTF, which is metres / Y-up / right-handed BY SPECIFICATION and therefore declares nothing -- the
// glTF backend never touches this struct, and AC-24's glTF half is that fact.
//
// `generator` and `formatVersion` are DISPLAY STRINGS. Never parsed, never switched on, never compared.
// They exist so a person looking at the panel can see WHICH tool wrote the file that needed converting.
struct SourceSpace {
    bool declared = false;      // false == the format declares no space; the panel draws nothing
    float unitMeters = 1.0F;    // the source's own unit, in metres (FBX: settings.unit_meters -- 0.01
                                // for a centimetre file). NOT original_unit_meters: that is an
                                // Autodesk round-trip property most exporters never write.
    char upAxis = 'Y';          // 'X' | 'Y' | 'Z', from settings.axes.up, which ufbx PRESERVES across
                                // the conversion (ufbx.h: "This contains the _original_ axes even if
                                // you supply ufbx_load_opts.target_axes"). '?' when UNKNOWN.
    std::string generator;      // "Blender 4.2", or "" -- prose
    std::string formatVersion;  // "FBX 7400 binary", or "" -- prose
};

// ---- the model, and the result envelope -------------------------------------------------------------
struct ImportSummary {
    std::size_t nodeCount = 0;
    std::size_t meshCount = 0;
    std::size_t primitiveCount = 0;  // SURVIVING primitives only -- a skipped one is not counted
    std::size_t materialCount = 0;
    std::size_t imageCount = 0;
    std::size_t skinCount = 0;
    std::size_t jointCount = 0;  // summed across skins
    std::size_t animationCount = 0;
    std::size_t vertexCount = 0;     // 0 at ImportDepth::Structure -- no accessor is decoded there
    std::size_t triangleCount = 0;   // 0 at ImportDepth::Structure, for the same reason
    float animationDuration = 0.0F;  // the LONGEST clip; 0 at Structure depth
    // Aabb::empty() at ImportDepth::Structure (plan A21) -- no position is decoded, so there is nothing
    // to fold. valid() is false there, and the panel shows "--" rather than a fake box.
    Aabb bounds = Aabb::empty();
};

struct ImportedModel {
    std::vector<ImportedNode> nodes;
    std::vector<std::uint32_t> roots;  // nodes with no parent, SOURCE ORDER (AC-21)
    std::vector<ImportedMesh> meshes;
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedImage> images;
    std::vector<ImportedSkin> skins;
    std::vector<ImportedAnimation> animations;
    ImportSummary summary;
    SourceSpace sourceSpace;  // task 3.2.2 (D19). APPENDED, NEVER INSERTED -- 3.1.2's A2 trap: an
                              // inserted field silently re-maps every positional aggregate
                              // initializer, and bool -> std::size_t is a promotion nothing
                              // diagnoses. Every test helper uses DESIGNATED initializers.
};

enum class ImportStatus : std::uint8_t {
    Ok = 0,
    Unsupported,       // no importer claims this extension -- NOTHING was read
    ParseFailed,       // fastgltf refused the document
    Malformed,         // parsed, but violates a glTF invariant this importer requires
    MissingExtension,  // extensionsRequired names something this build does not implement (D20/A6)
    MissingBuffer,     // a Full pass whose caller did not supply a buffer the Structure pass named
    Truncated,         // a D15 cap was hit; `model` is COHERENT but SMALLER, never partial-claiming-whole
};
// A switch with NO `default:`, so a future enumerator is a -Wswitch warning rather than a silent
// "unknown" (importChangeLabel / logAssetScan's ScanStatus switch are the precedents).
[[nodiscard]] std::string_view importStatusLabel(ImportStatus status) noexcept;

struct ImportResult {
    ImportStatus status = ImportStatus::Ok;
    // "" IFF status == Ok (plan A15 -- the spec's "|| Truncated" contradicts its own requirement that a
    // Truncated result name the cap it hit). Two caps in one import produce ONE Truncated and two
    // "; "-joined messages.
    std::string message;
    ImportedModel model;  // EMPTY unless status is Ok or Truncated
    // Every ACCEPTED external URI, as classifyUri resolved it (project-relative, '/'-separated),
    // deduplicated, in first-seen source order. The list the caller loads for a Full pass, and the list
    // the scan turns into dependency GUIDs. Refused URIs are NOT here -- they are on
    // ImportedImage::refusal and in `warnings`.
    std::vector<std::string> externalUris;
    std::vector<std::string> warnings;  // capped at MAX_IMPORT_WARNINGS
    std::size_t warningTotal = 0;       // UNCAPPED (the MAX_REPORTED_PER_CATEGORY shape, a fourth use)
};

// ---- the entry point --------------------------------------------------------------------------------
enum class ImportDepth : std::uint8_t {
    // Nodes, meshes' SHAPES, materials, images, skins' joints, clips' shapes, and every external URI.
    // NO vertex data, NO index data, NO animation samples, NO inverse bind matrices. Needs the
    // .gltf/.glb bytes and NOTHING ELSE. What the SCAN runs, budgeted (§D-6).
    Structure = 0,
    // Everything, including every accessor's decoded contents. Needs the bytes AND every external
    // buffer the Structure pass named. What the PANEL runs, for one asset, on demand.
    Full,
};

// One external buffer, supplied BY THE CALLER (D3/D4). `uri` matches an entry of a previous Structure
// pass's `externalUris` EXACTLY.
struct ExternalBuffer {
    std::string uri;
    std::string bytes;  // std::string is the BYTE container, as everywhere else in this tree
};

// Dispatches on `fileName`'s extension, ASCII-case-folded. .gltf/.glb -> importGltf; everything else ->
// Unsupported, WITHOUT READING ANYTHING (AC-44). `assetRelativeDir` is the model's OWN directory,
// project-relative, '/'-separated, NO trailing slash ("" for a model at the assets root) -- what D14
// resolves relative URIs against, and a STRING, never a path object.
//
// NEVER THROWS. NEVER READS A FILE. NEVER LOGS.
[[nodiscard]] ImportResult importModel(std::string_view fileName, std::string_view assetRelativeDir,
                                       std::span<const std::byte> bytes, const ImportSettings& settings,
                                       ImportDepth depth, std::span<const ExternalBuffer> external);

// The importer identity a file NAME implies, with no probe and no disk. ("", 0) when no importer
// claims it. THIS IS WHAT planImports COMPARES against the previous cache entry AND what phase 7.5
// RECORDS -- one pure function of the name, used by both, which is why a GLTF_IMPORTER_VERSION or
// FBX_IMPORTER_VERSION bump re-triggers imports for exactly the affected format and nothing else.
//
// It exists because 3.2.1's code review chose the shape deliberately: a TU-local copy of the suffix
// logic "would silently stop discriminating the moment 3.2.2 (ufbx) teaches the real predicate a new
// extension." This is that moment.
struct ImporterIdentity {
    std::string_view name;  // "" == no importer claims this file
    std::uint32_t version = 0;
    bool operator==(const ImporterIdentity&) const = default;
};
[[nodiscard]] ImporterIdentity modelImporterIdentity(std::string_view fileName) noexcept;

// TRUE iff a Full import of this file NEEDS bytes that the Structure pass named. glTF: YES (its
// buffers may be external .bin files). FBX: NO -- all geometry is inside the file, and its external
// URIs are TEXTURES, which this importer resolves for the DEPENDENCY GRAPH and never reads.
//
// ModelImportSession::service() gates its ENTIRE first pass on this, which is what stops a MISSING
// TEXTURE from blocking an FBX import whose geometry was in the file all along (E21 would otherwise
// keep the Structure result and call it Truncated).
//
// glTF: YES (its buffers may be external .bin files). FBX: NO -- all geometry is inside the file, and
// its external URIs are TEXTURES, which this importer resolves for the DEPENDENCY GRAPH and never
// reads. OBJ splits BY EXTENSION WITHIN ONE IMPORTER (task 3.2.3): `.obj` YES (its .mtl is a genuine
// external file the Full pass needs), `.mtl` NO (its whole content is local; its URIs are textures).
//
// The .mtl arm is not a nicety. Answering TRUE there would make ModelImportSession read every texture
// the library names -- up to MAX_EXTERNAL_BYTES_PER_MODEL of PNG bytes -- hand them to an arm that
// ignores every one, and, the moment they exceed the budget, take the E21 branch and report Truncated
// ("showing the document's structure only") for a result that was COMPLETE at Structure depth.
//
// PURE: dispatches on the file NAME, exactly as isImportableModelName does. 3.2.5's Assimp formats will
// answer per format.
[[nodiscard]] bool modelImporterNeedsExternalBuffers(std::string_view fileName) noexcept;

// ---- pure helpers (D14) -----------------------------------------------------------------------------
enum class UriClass : std::uint8_t {
    RelativePath = 0,    // the ONLY class that leads to a read
    DataUri,             // decoded by fastgltf itself; embedded; NEVER a dependency
    RefusedScheme,       // http:, https:, file:, ftp:, ... -- unconditionally, no setting enables it
    RefusedAbsolute,     // leading '/', a drive letter 'X:', or a UNC '\\'
    RefusedEscape,       // normalizes OUTSIDE the assets root
    RefusedBackslash,    // a URI's separator is '/'; a '\' is a Windows path leaking into a format that
                         // has no such concept (E26 -- refused IDENTICALLY on every platform, because
                         // this is a STRING test, never a path test)
    RefusedEmpty,        // "" or a path collapsing to nothing
    RefusedControlChar,  // any byte < 0x20 (plan A1 -- what "%zz" decodes to, plus %00 and newlines)
};

struct UriClassification {
    UriClass kind = UriClass::RelativePath;
    std::string relativePath;  // MEANINGFUL ONLY for RelativePath: normalized, project-relative, '/'-sep
    std::string reason;        // "" for the two accepting classes; the EXACT panel/warning text otherwise
};

// PURE. No disk, no <filesystem>, no clock, no global state.
//
// `uri` IS ALREADY PERCENT-DECODED (plan §A-1): fastgltf::URI decodes on construction (src/fastgltf.cpp
// -- URI::URI calls decodePercents), so sources::URI::uri.string() hands us decoded text. DO NOT DECODE
// AGAIN -- a file legitimately named "100%20.png" arrives as "100%20.png", and a second decode would
// silently turn it into "100 .png".
//
// Decoding BEFORE classification is also the SECURE order and is preserved by that split: an encoded
// scheme ("%68ttp://evil") has already become "http://evil" by the time this sees it, so the scheme
// refusal still fires.
//
// Test order, and it is load-bearing: control char -> empty -> backslash -> scheme (a ':' before the
// first '/') -> absolute -> data: -> normalize -> escape.
[[nodiscard]] UriClassification classifyUri(std::string_view uri, std::string_view assetRelativeDir);

// PURE: split on '/', drop "." and empty segments, pop on "..", REFUSE on underflow. Returns nullopt
// when the path escapes its root. NO <filesystem>, NO weakly_canonical, NO disk. Shared by classifyUri
// and tested independently (validateOrphanPath's shape, 3.1.3, a second application).
[[nodiscard]] std::optional<std::string> normalizeRelativePath(std::string_view path);

// Fold every '\' to '/'. THE FBX-FORMAT-SPECIFIC NORMALIZATION (task 3.2.2, D14): `textures\wood.png`
// is what every Maya and 3ds Max export writes, and classifyUri refuses a backslash outright and
// CORRECTLY -- a glTF URI has no such concept, and UriClass::RefusedBackslash stays reachable for it.
//
// CALLED BY THE FBX AND OBJ BACKENDS ONLY, and ALWAYS BEFORE classifyUri. Both formats carry
// filesystem PATHS rather than URIs -- FBX's `textures\wood.png` from Maya/3ds Max, and Wavefront's
// `mtllib`/`map_Kd` operands, which are plain paths by specification and are routinely written with
// backslashes by Windows exporters (task 3.2.3, D15). Fold-then-classify is the SECURE order, for the
// identical reason decode-then-classify is: `..\..\..\etc\passwd` has already become
// `../../../etc/passwd` by the time the escape check runs, so the refusal still fires. Folding AFTER
// classification would let a backslash traversal straight through.
//
// PURE: no disk, no <filesystem>, no locale. Byte-for-byte identical on all three platforms.
[[nodiscard]] std::string foldBackslashesToSlashes(std::string_view path);

// task 3.2.3 (D16). PURE, and this IS the .obj Structure pass. Returns every `mtllib` CANDIDATE in
// first-seen order, deduplicated BY RAW TEXT, capped at `maxNames`. For each matching line: the WHOLE
// trimmed operand first, then each whitespace-separated token.
//
// The whole operand LEADS because an ambiguous operand must be offered both ways and the filesystem
// must decide: `mtllib my file.mtl` yields "my file.mtl" (the correct reading) before "my"/"file.mtl",
// while `mtllib a.mtl b.mtl` yields a junk first candidate costing one failed read the session already
// handles by `continue`. That is strictly better than a heuristic, and it is why a backslash-escape
// rule is NOT needed (and would break D15).
//
// A line matches iff, after leading spaces/tabs only, it begins with `mtllib` followed by a space or a
// tab -- CASE-SENSITIVE, matching the library's own strncmp, so `# mtllib x` and `MTLLIB x` never
// match. NO percent-decoding: a Wavefront operand is a filesystem path, never a URI.
//
// Implemented as a thin wrapper over scanObjMtlLibsScan below (code-review round, gap 10) -- candidates
// only, for callers (this file's own test suite) that do not need the E4 count in the same pass.
[[nodiscard]] std::vector<std::string> scanObjMtlLibs(std::span<const std::byte> bytes, std::size_t maxNames);

// task 3.2.3 (D16), extended by the code-review round (gap 10). The RESULT of the single-pass mtllib
// scan: `candidates` is exactly scanObjMtlLibs's own return value; `emptyOperandLines` is E4's count of
// `mtllib` lines that matched the keyword+separator rule but trimmed to an EMPTY operand -- a count
// scanObjMtlLibs's own contract is silent about (D16: no candidate is pushed for one of these).
// Bundled so the CALLER (the .obj Structure pass, run on every phase 7.5 probe) never needs a SECOND
// full-file linear scan just to recover a count the first pass already computed while walking the same
// lines -- R5 measured the scan alone at ~107-113 ms on a ~150 MB file, roughly half of which was this
// second, now-removed pass.
struct ObjMtlLibScan {
    std::vector<std::string> candidates;
    std::size_t emptyOperandLines = 0;
};

// PURE, and this IS the .obj Structure pass, in ONE LINEAR SCAN. See scanObjMtlLibs and ObjMtlLibScan
// above for the candidate and count semantics respectively.
[[nodiscard]] ObjMtlLibScan scanObjMtlLibsScan(std::span<const std::byte> bytes, std::size_t maxNames);

// task 3.2.5 (A-7). PURE, and this IS the .ply Structure pass. Returns every `TextureFile` operand a
// PLY header declares, in first-seen order, deduplicated BY RAW TEXT, capped at `maxNames`, stopping
// at the `end_header` line (so it is bounded by the HEADER, not by the file: a 400 MB binary .ply
// costs a few hundred bytes of scanning).
//
// The rule MIRRORS the library's own rather than a corrected one, INCLUDING its trailing-character
// behaviour, because Structure and Full must agree about the URI set and Full goes through that code:
// a line matches iff, after leading spaces, it begins with `element` or `comment`, then the
// case-SENSITIVE token `TextureFile`, and the operand is the REST OF THE LINE MINUS ITS FINAL
// CHARACTER. AC-19 asserts the agreement on every fixture; do not "fix" this into the rule you would
// have written.
[[nodiscard]] std::vector<std::string> scanPlyTextureFiles(std::span<const std::byte> bytes, std::size_t maxNames);

// task 3.2.5 (A-10). PURE and DISPLAY-ONLY: reads at most `maxBytes` of a Collada document's leading
// text for the <asset> block's <unit meter="..."> and <up_axis>. Bounded because both COLLADA schemas
// require <asset> to be the FIRST child of <COLLADA>.
//
// It exists because Assimp's Collada loader CONSUMES both values into the root node's transformation
// and exposes NEITHER (they fall in ReadAssetInfo's if/else-if chain before the branch that feeds the
// metadata map). The result is NEVER fed back into geometry, NEVER compared and NEVER switched on --
// SourceSpace's own "DISPLAY STRINGS" rule, extended to its two numeric fields for the one format
// where the library gets there first. `declared` stays FALSE when neither element is found: a blank
// panel row, never a wrong model.
[[nodiscard]] SourceSpace scanColladaAssetSpace(std::span<const std::byte> bytes, std::size_t maxBytes = 65536);

// task 3.2.3 (AC-54). PURE: true iff any of the first `probeBytes` bytes is 0x00. A Wavefront file is
// TEXT; this is what stops a renamed PNG/JPEG/GLB being handed to a text parser at all. PNG, JPEG and
// GLB magic all trip it -- their length/version fields put a NUL inside the first 16 bytes.
[[nodiscard]] bool looksLikeBinaryContent(std::span<const std::byte> bytes, std::size_t probeBytes = 1024) noexcept;

// True iff `fileName` ends (ASCII-case-folded) in ".gltf", ".glb", ".fbx", ".obj" or ".mtl".
// 3.2.5 extends it further. 3.2.4 deliberately does NOT: .blend never becomes importable, because the
// SCAN must never spawn a process (3.2.4 D15), and phase 7.5 gates its probe on exactly this
// predicate. A .blend is converted to a GLB ABOVE importModel instead, by ModelImportSession's own
// arm. MI133 pins that -- together with modelImporterNeedsExternalBuffers("x.blend") == false, a
// SECOND fact that breaks independently.
//
// STILL DELIBERATELY NARROWER than asset_view.hpp's AssetKind::Model in one direction -- .blend/.dae/
// .ply/.stl are claimed there and unimportable here -- and, since task 3.2.3, WIDER in another: a .mtl
// is importable and is NOT AssetKind::Model at all (it classifies as AssetKind::Unknown). Keeping the
// two tables separate is what stops a format being silently promoted to "importable" by an edit to the
// kind table (3.1.3's isThumbnailDecodable precedent), and the .mtl asymmetry is exactly why.
//
// WHY .mtl IS CLAIMED (task 3.2.3, D4): phase 7.5 probes at Structure depth with NO external bytes, so
// a probe of chair.obj can name chair.mtl and can NEVER name wood.png. Claiming .mtl turns one two-hop
// problem into two ordinary one-hop edges, and 3.1.2's cascade (planImports step 4, a BFS over reverse
// edges) is transitive, so wood.png dirty => chair.mtl => chair.obj falls out with no new mechanism.
//
// A SUFFIX test on the FULL name (the isMetaFileName shape), never a last-extension split, so
// "archive.tar.obj" is importable and "model.obj.bak" is not.
[[nodiscard]] bool isImportableModelName(std::string_view fileName) noexcept;

}  // namespace engine::editor

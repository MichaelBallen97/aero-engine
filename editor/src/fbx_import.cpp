// Aero Engine — the FBX backend (task 3.2.2). THE ONLY ufbx TRANSLATION UNIT IN THE TREE (INV-F1).
//
// UFBX NEVER TOUCHES THE FILESYSTEM (D4). ufbx_load_memory over bytes readFileBytes already read is
// the ONLY load call. ufbx_load_file / ufbx_load_file_len / ufbx_load_stdio* / ufbx_load_stream must
// NEVER appear anywhere in this tree. `load_external_files` stays FALSE -- ufbx's own header says why:
// "This may be risky for untrusted data as the input files may contain references to arbitrary paths
// in the filesystem." And `open_file_cb` is set to a callback that CANNOT SUCCEED, so a future ufbx
// that opens a file for a reason `load_external_files` does not gate finds a closed door rather than
// a working stdio path. `obj_mtl_path`/`obj_mtl_data` are never set: ufbx notes those "sidestep
// load_external_files as they are explicitly requested", which is exactly why they stay untouched.
//
// THIS FILE HAS NO <filesystem> INCLUDE AND PERFORMS NO FILE OPERATION -- at source AND transitively,
// which is STRICTLY STRONGER than gltf_import.cpp (whose fastgltf entry point takes a path
// positionally). §V6 asserts it.
//
// THE IMPORTER WRITES NO COORDINATE MATH (INV-F7). No axis swap, no negation, no transpose, no
// quaternion reorder, no degree/radian conversion, no winding reversal. THE CONVERSION IS THREE
// ufbx_load_opts FIELDS -- target_axes, target_unit_meters, space_conversion -- and ufbx bakes the
// result into ufbx_node.local_transform, which this file copies (phase 3, Step 5). Verified by running
// ufbx v0.23.0: a Z-up centimetre source converts to metres with geometry_scale 0.01 and a -90-degree X
// rotation on the roots, and mirror_axis stays 0 (a pure rotation: no mirror, hence no winding change).
// Mat4 widening (appending (0,0,0,1)) and explicit double->float narrowing are the ONLY numeric
// operations performed on a ufbx value.
//
// EVERY PLATFORM-DEPENDENT ufbx DEFAULT IS SET EXPLICITLY (D18). opts.path_separator defaults to '\'
// on Windows and '/' everywhere else -- a platform-dependent default inside a function whose output
// three CI lanes must agree about, byte for byte, in tests comparing relativePath against literals.
//
// EVERY WALK IS ITERATIVE (misc-no-recursion is --warnings-as-errors on the Linux lane).
// NOTHING HERE LOGS (INV-A3). NOTHING HERE THROWS. NOTHING HERE ALLOCATES WITH new/delete.
//
// STEP BOUNDARY (recorded here rather than left implicit): Step 4 landed phases 1-2 -- the load, the
// error mapping, SourceSpace, ufbx's own warnings, and the cheap STRUCTURAL summary counts (element
// counts plus ufbx's own already-computed per-mesh vertex/triangle counts). Step 5 added phase 3 --
// nodes, hierarchy, helper nodes and the space conversion, which is where D6's whole conversion regime
// is either right or silently wrong (§D-4.5). THIS commit (Step 6) adds phases 4-5 -- textures ->
// ImportedImage (D14) and materials from `pbr` ONLY (D13), both running at BOTH depths since neither
// touches vertex content. Phases 6-8 (meshes, skins, animation) are NOT part of this file yet --
// `result.model.meshes/skins/animations` and `result.model.summary.bounds` stay at their defaults
// until those phases land.
#include "fbx_import.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <ufbx.h>  // <-- the ONE include of ufbx anywhere in this tree (AC-63)
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// ---- §D-4.1: RAII, so ufbx_free_scene runs on EVERY path including an early return in the middle of a
// later phase. Deliberately NOT ufbx's own C++ helpers: those sit behind UFBX_CPP conditionals, and
// depending on one would be a compile-time knob on a file INV-F5 forbids patching. Move-only, never
// copied.
class ScenePtr {
public:
    explicit ScenePtr(ufbx_scene* s) noexcept : ptr(s) {}
    ScenePtr(const ScenePtr&) = delete;
    ScenePtr& operator=(const ScenePtr&) = delete;
    ScenePtr(ScenePtr&& other) noexcept : ptr(std::exchange(other.ptr, nullptr)) {}
    ScenePtr& operator=(ScenePtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr = std::exchange(other.ptr, nullptr);
        }
        return *this;
    }
    ~ScenePtr() { reset(); }
    [[nodiscard]] const ufbx_scene* get() const noexcept { return ptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return ptr != nullptr; }

private:
    void reset() noexcept {
        if (ptr != nullptr) {
            ufbx_free_scene(ptr);
            ptr = nullptr;
        }
    }
    ufbx_scene* ptr = nullptr;
};

// The IDENTICAL shape as ScenePtr (§A-10), wrapping ufbx_baked_anim / ufbx_free_baked_anim -- so
// ufbx_bake_anim's result is freed on EVERY path, including an early `continue` for a clip that fails
// to bake (D12: one clip failing never fails the whole import).
class BakedAnimPtr {
public:
    explicit BakedAnimPtr(ufbx_baked_anim* p) noexcept : ptr(p) {}
    BakedAnimPtr(const BakedAnimPtr&) = delete;
    BakedAnimPtr& operator=(const BakedAnimPtr&) = delete;
    BakedAnimPtr(BakedAnimPtr&& other) noexcept : ptr(std::exchange(other.ptr, nullptr)) {}
    BakedAnimPtr& operator=(BakedAnimPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr = std::exchange(other.ptr, nullptr);
        }
        return *this;
    }
    ~BakedAnimPtr() { reset(); }
    [[nodiscard]] const ufbx_baked_anim* get() const noexcept { return ptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return ptr != nullptr; }

private:
    void reset() noexcept {
        if (ptr != nullptr) {
            ufbx_free_baked_anim(ptr);
            ptr = nullptr;
        }
    }
    ufbx_baked_anim* ptr = nullptr;
};

// Every ufbx string that crosses into ImportedModel (node names, mesh names, material names,
// filenames, the error description, warning descriptions) is converted with the TWO-ARGUMENT
// std::string ctor -- NEVER std::string(s.data). unicode_error_handling guarantees valid UTF-8, not
// the absence of an embedded NUL.
[[nodiscard]] std::string toStd(ufbx_string s) {
    return s.data == nullptr ? std::string() : std::string(s.data, s.length);
}

// ---- numeric conversions. ufbx_real is DOUBLE (ufbx's default and best-tested configuration;
// UFBX_REAL_IS_FLOAT is NOT defined -- see the vendored CMakeLists.txt). Every value crossing into
// ImportedModel is narrowed HERE and nowhere else.
[[nodiscard]] constexpr float f(ufbx_real v) noexcept { return static_cast<float>(v); }

[[nodiscard]] constexpr Vec2 toVec2(ufbx_vec2 v) noexcept { return Vec2{f(v.x), f(v.y)}; }
[[nodiscard]] constexpr Vec3 toVec3(ufbx_vec3 v) noexcept { return Vec3{f(v.x), f(v.y), f(v.z)}; }
[[nodiscard]] constexpr Vec4 toVec4(ufbx_vec4 v) noexcept { return Vec4{f(v.x), f(v.y), f(v.z), f(v.w)}; }

// ufbx_quat is {x, y, z, w} -- THE SAME COMPONENT ORDER as engine::Quat. NO REORDER ANYWHERE. A
// wrongly-ordered quaternion is still unit-length, so only a component-by-component comparison (FI15,
// FI17) catches it.
[[nodiscard]] constexpr Quat toQuat(ufbx_quat q) noexcept { return Quat{f(q.x), f(q.y), f(q.z), f(q.w)}; }

// ufbx_matrix is a 3x4 AFFINE matrix stored as FOUR COLUMNS (ufbx.h:365-367: "cols[0..2] are the X/Y/Z
// basis vectors, cols[3] is the translation"). engine::Mat4 stores columns too (mat4.hpp's own D8), so
// this is a WIDENING -- appending the implied (0,0,0,1) last row -- and NEVER A TRANSPOSE.
[[nodiscard]] Mat4 toMat4(const ufbx_matrix& m) noexcept {
    const Vec3 c0 = toVec3(m.cols[0]);
    const Vec3 c1 = toVec3(m.cols[1]);
    const Vec3 c2 = toVec3(m.cols[2]);
    const Vec3 c3 = toVec3(m.cols[3]);
    return Mat4{std::array<Vec4, 4>{Vec4{c0.x, c0.y, c0.z, 0.0F}, Vec4{c1.x, c1.y, c1.z, 0.0F},
                                    Vec4{c2.x, c2.y, c2.z, 0.0F}, Vec4{c3.x, c3.y, c3.z, 1.0F}}};
}

// ---- §D-4.2: the complete error table (AC-49). EVERY ufbx_error_type, with NO `default:` -- a ufbx
// bump that adds an enumerator is a -Wswitch diagnostic rather than a silent "unknown".
// importStatusLabel's own precedent (model_import.cpp).
//
// CORRECTION to the plan's own §A-16 (found by compiling, not assumed): `UFBX_ENUM_FORCE_WIDTH`
// DOES add a real enumerator here -- `UFBX_ERROR_TYPE_FORCE_32BIT = 0x7fffffff` -- because
// `UFBX_USE_EXPLICIT_ENUM` is (correctly) never defined anywhere in this build, so ufbx.h's `#else`
// branch (ufbx.h:227-233) is the one compiled. It is a C89/C99 ABI-width portability trick, never a
// value any real ufbx call returns, but the compiler cannot know that -- so it is handled explicitly,
// separately from the 24 real rows above, to keep -Wswitch meaningful for an actual future enumerator
// rather than silenced by a `default:` that would swallow one too.
[[nodiscard]] constexpr ImportStatus ufbxStatusFor(ufbx_error_type type) noexcept {
    switch (type) {
        case UFBX_ERROR_NONE:
            return ImportStatus::Ok;
        // A cap was hit INSIDE ufbx, before the allocation it bounds. The model is not partial-
        // claiming-whole: it is EMPTY and the message names the cap (D16).
        case UFBX_ERROR_MEMORY_LIMIT:
        case UFBX_ERROR_ALLOCATION_LIMIT:
        case UFBX_ERROR_NODE_DEPTH_LIMIT:
        case UFBX_ERROR_OUT_OF_MEMORY:
            return ImportStatus::Truncated;
        // The bytes are not an FBX document this build can begin to read.
        case UFBX_ERROR_UNRECOGNIZED_FILE_FORMAT:
        case UFBX_ERROR_EMPTY_FILE:
        case UFBX_ERROR_TRUNCATED_FILE:
            return ImportStatus::ParseFailed;
        // Parsed, but violates an invariant this importer requires.
        case UFBX_ERROR_UNSUPPORTED_VERSION:
        case UFBX_ERROR_BAD_INDEX:
        case UFBX_ERROR_BAD_NURBS:
        case UFBX_ERROR_INVALID_UTF8:
        case UFBX_ERROR_ZERO_VERTEX_SIZE:
        case UFBX_ERROR_TRUNCATED_VERTEX_STREAM:
        case UFBX_ERROR_DUPLICATE_OVERRIDE:
            return ImportStatus::Malformed;
        // Three more reasons, all landing on ParseFailed -- merged into ONE case-list (rather than
        // three consecutive one-line switches) because clang-tidy's bugprone-branch-clone flags three
        // adjacent branches with an identical body as a likely copy-paste bug; the reasons genuinely
        // differ, so they stay documented here instead of collapsed away:
        //  - A BUG IF EVER SEEN (FILE_NOT_FOUND / EXTERNAL_FILE_NOT_FOUND / IO): D4 means no file is
        //    ever opened. The message says so, in those words, so a report carrying it is immediately
        //    diagnosable.
        //  - Unreachable from this call site (CANCELLED / UNINITIALIZED_OPTIONS / UNSAFE_OPTIONS /
        //    FEATURE_DISABLED / THREADED_ASCII_PARSE / UNKNOWN): we never cancel, always
        //    value-initialise the opts, never set an unsafe option, never enable threading, and never
        //    disable a feature -- mapped anyway, because an unmapped enumerator is how a switch rots.
        //  - The UFBX_ENUM_FORCE_WIDTH sentinel (TYPE_FORCE_32BIT, this function's own header comment)
        //    -- a compiler ABI-width artefact, never a value ufbx actually returns.
        case UFBX_ERROR_FILE_NOT_FOUND:
        case UFBX_ERROR_EXTERNAL_FILE_NOT_FOUND:
        case UFBX_ERROR_IO:
        case UFBX_ERROR_CANCELLED:
        case UFBX_ERROR_UNINITIALIZED_OPTIONS:
        case UFBX_ERROR_UNSAFE_OPTIONS:
        case UFBX_ERROR_FEATURE_DISABLED:
        case UFBX_ERROR_THREADED_ASCII_PARSE:
        case UFBX_ERROR_UNKNOWN:
        case UFBX_ERROR_TYPE_FORCE_32BIT:
            return ImportStatus::ParseFailed;
    }
    return ImportStatus::ParseFailed;
}

// ufbx's own description VERBATIM, prefixed with our context, plus error.info when info_length > 0.
// The two-argument std::string, always (toStd). error.stack is NOT read -- UFBX_ENABLE_ERROR_STACK is
// deliberately not defined, so stack_size is 0.
[[nodiscard]] std::string ufbxMessage(const ufbx_error& err) {
    std::string out = std::format("ufbx: {}", toStd(err.description));
    if (err.info_length > 0) {
        out += std::format(" ({})", std::string(err.info, err.info_length));
    }
    if (err.type == UFBX_ERROR_FILE_NOT_FOUND || err.type == UFBX_ERROR_EXTERNAL_FILE_NOT_FOUND ||
        err.type == UFBX_ERROR_IO) {
        out += " -- this importer opens no file, so this indicates a bug";
    }
    return out;
}

// The MAX_REPORTED_PER_CATEGORY shape: a capped list plus an UNCAPPED total, so the panel can say "and
// N more" honestly. gltf_import.cpp's own addWarning verbatim -- deliberately a second copy, not a
// shared header, matching the foldAscii precedent (model_import.cpp / asset_meta.cpp / asset_cache.cpp
// each keep their own).
void addWarning(ImportResult& out, std::string text) {
    ++out.warningTotal;
    if (out.warnings.size() < MAX_IMPORT_WARNINGS) {
        out.warnings.push_back(std::move(text));
    }
}

// MONOTONE escalation, Ok < Truncated < Malformed. Two caps in one import produce ONE Truncated and
// "; "-joined messages (ImportResult's own contract). Unsupported/ParseFailed/MissingExtension/
// MissingBuffer are never reached through this function: Unsupported is the dispatch's own return,
// ParseFailed comes from the load failure's direct return, and MissingExtension/MissingBuffer simply
// never occur for FBX (D22) -- grouped at the highest rank anyway, gltf_import.cpp's escalate
// precedent.
void escalate(ImportResult& out, ImportStatus status, std::string_view why) {
    const auto rank = [](ImportStatus s) -> int {
        switch (s) {
            case ImportStatus::Ok:
                return 0;
            case ImportStatus::Truncated:
                return 1;
            case ImportStatus::Malformed:
                return 2;
            case ImportStatus::Unsupported:
            case ImportStatus::ParseFailed:
            case ImportStatus::MissingExtension:
            case ImportStatus::MissingBuffer:
                return 3;
        }
        return 3;
    };
    if (rank(status) > rank(out.status)) {
        out.status = status;
    }
    if (!why.empty()) {
        if (!out.message.empty()) {
            out.message += "; ";
        }
        out.message += why;
    }
}

// D4, defence in depth. ufbx's default open_file_cb is stdio. A future ufbx that opens a file for a
// reason `load_external_files` does not gate finds a callback that CANNOT SUCCEED. The parameters are
// unnamed because none is read -- there is no path this function would accept.
extern "C" bool refuseToOpenAnyFile(void*, ufbx_stream*, const char*, std::size_t, const ufbx_open_file_info*) {
    return false;
}

// D7/D8: explicit, fixed, greppable, and stable across a ufbx bump, rather than ufbx's own defaults.
// The panel labels a node bearing either suffix so a helper is recognizable rather than mysterious
// (Step 5 onward).
inline constexpr std::string_view GEOMETRY_HELPER_SUFFIX = "<geometry helper>";
inline constexpr std::string_view SCALE_HELPER_SUFFIX = "<scale helper>";
[[nodiscard]] constexpr ufbx_string ufbxLiteral(std::string_view s) noexcept { return ufbx_string{s.data(), s.size()}; }

// A3/D19: 'X'|'Y'|'Z' from a ufbx_coordinate_axis, '?' for UNKNOWN. No `default:` for the identical
// -Wswitch reason ufbxStatusFor has none -- ufbxStatusFor's own header comment records why the
// UFBX_ENUM_FORCE_WIDTH sentinel still needs its own case (a plan §A-16 correction, found by
// compiling).
[[nodiscard]] constexpr char axisLetter(ufbx_coordinate_axis axis) noexcept {
    switch (axis) {
        case UFBX_COORDINATE_AXIS_POSITIVE_X:
        case UFBX_COORDINATE_AXIS_NEGATIVE_X:
            return 'X';
        case UFBX_COORDINATE_AXIS_POSITIVE_Y:
        case UFBX_COORDINATE_AXIS_NEGATIVE_Y:
            return 'Y';
        case UFBX_COORDINATE_AXIS_POSITIVE_Z:
        case UFBX_COORDINATE_AXIS_NEGATIVE_Z:
            return 'Z';
        case UFBX_COORDINATE_AXIS_UNKNOWN:
        case UFBX_COORDINATE_AXIS_FORCE_32BIT:
            return '?';
    }
    return '?';
}

// A21: a DISPLAY-ONLY name for the exporter that wrote the file, never parsed or compared elsewhere.
[[nodiscard]] constexpr std::string_view exporterName(ufbx_exporter exporter) noexcept {
    switch (exporter) {
        case UFBX_EXPORTER_UNKNOWN:
            return "";
        case UFBX_EXPORTER_FBX_SDK:
            return "FBX SDK";
        case UFBX_EXPORTER_BLENDER_BINARY:
            return "Blender (binary)";
        case UFBX_EXPORTER_BLENDER_ASCII:
            return "Blender (ASCII)";
        case UFBX_EXPORTER_MOTION_BUILDER:
            return "MotionBuilder";
        case UFBX_EXPORTER_UFBX_WRITE:
            return "ufbx";
        case UFBX_EXPORTER_FORCE_32BIT:
            return "";
    }
    return "";
}

// A21: "" when the exporter is UNKNOWN and metadata.creator is empty -- which is every tier-0 fixture
// (a hand-written ASCII FBX reports exporter == UFBX_EXPORTER_UNKNOWN, exporter_version == 0,
// measured). Falls back to the free-text `creator` field, which a real DCC tool usually still writes
// even when ufbx cannot identify it as a known exporter.
[[nodiscard]] std::string generatorString(const ufbx_metadata& metadata) {
    if (metadata.exporter == UFBX_EXPORTER_UNKNOWN) {
        return toStd(metadata.creator);
    }
    return std::format("{} {}.{}.{}", exporterName(metadata.exporter), ufbx_version_major(metadata.exporter_version),
                       ufbx_version_minor(metadata.exporter_version), ufbx_version_patch(metadata.exporter_version));
}

// D14/D22: a DISPLAY-ONLY name for a non-FILE texture type, so the "not imported" warning names what it
// skipped. No `default:` for the identical -Wswitch reason every other switch in this file has none.
[[nodiscard]] constexpr std::string_view textureTypeName(ufbx_texture_type type) noexcept {
    switch (type) {
        case UFBX_TEXTURE_FILE:
            return "file";
        case UFBX_TEXTURE_LAYERED:
            return "layered";
        case UFBX_TEXTURE_PROCEDURAL:
            return "procedural";
        case UFBX_TEXTURE_SHADER:
            return "shader";
        case UFBX_TEXTURE_TYPE_FORCE_32BIT:
            return "unknown";
    }
    return "unknown";
}

// D14: everything after the last '/' OR '\', so it works on a Windows absolute `filename` BEFORE the
// fold runs (foldBackslashesToSlashes is called on the RESULT of this, not on `filename` directly).
// PURE, TU-local -- model_import.hpp's own basenameOf-shaped helper does not exist because no OTHER
// backend has needed one yet.
[[nodiscard]] std::string basenameOf(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? std::string(path) : std::string(path.substr(slash + 1));
}

// D13: engagement helper so every row of the material table reads the same way. `feature_disabled` ==
// "the shader turned this off"; `has_value` == "the file declared something here" -- ufbx.h is explicit
// that `.value_*` may hold a non-zero default even when `has_value == false`, so reading the value
// without this gate would silently promote a default into something the file never actually said.
[[nodiscard]] bool engaged(const ufbx_material_map& m) noexcept { return m.has_value && !m.feature_disabled; }

// D14's UV-set rule: `tex->uv_set` is a NAME, but the canonical model carries an INDEX on a
// material-level texture reference while UV sets live on MESHES. Resolve the name against `mesh`'s own
// `uv_sets` (the FIRST mesh using this material, found by the phase-5 pre-pass); take the index if it
// is 0 or 1 (the only two ImportedPrimitive carries). An EMPTY name is 0 with NO warning (E19); a name
// that cannot be checked at all -- no mesh, or `uv_sets` empty (Structure depth, where geometry content
// including UV SET NAMES is zeroed by `ignore_geometry`, exactly like every other per-mesh count in
// §A-4) -- is ALSO 0 with no warning, because there is nothing to check against, structurally, not
// because nothing was found. Only a REAL, non-empty `uv_sets` list that does not contain the name warns.
[[nodiscard]] std::uint32_t resolveUvSet(const ufbx_texture& tex, const ufbx_mesh* mesh, ImportResult& result) {
    const std::string uvSetName = toStd(tex.uv_set);
    if (uvSetName.empty()) {
        return 0;
    }
    if (mesh != nullptr && mesh->uv_sets.count > 0) {
        for (std::size_t i = 0; i < mesh->uv_sets.count; ++i) {
            if (toStd(mesh->uv_sets.data[i].name) == uvSetName) {
                if (i <= 1) {
                    return static_cast<std::uint32_t>(i);
                }
                break;  // found, but beyond the two sets ImportedPrimitive carries -- falls to the warning
            }
        }
        addWarning(result, std::format("texture '{}': UV set '{}' is not one of the mesh's first two UV sets; "
                                       "using UV set 0",
                                       toStd(tex.name), uvSetName));
    }
    return 0;
}

// D13's texture-slot resolution, ONE hop (an `ufbx_material_map` already names its `ufbx_texture*`
// directly -- unlike glTF's two-hop TextureInfo -> Texture -> image, there is no second table). Absent
// is ALWAYS `std::nullopt`, never index 0 (the A4 engagement rule, a further application). Deliberately
// `.find()`, never `.at()`: a texture connected to a material could in principle be a LAYERED/
// PROCEDURAL/SHADER texture that phase 4 warned about and skipped rather than turning into an
// ImportedImage, and `.at()` throwing on that would cross an API boundary with an exception this
// codebase forbids (§00's no-exceptions rule) -- `.find()` degrades it to `INVALID_SUBASSET` instead,
// which is not reachable by any fixture in this suite but is a strictly safer shape than the plan's own
// illustrative `.at()` snippet.
[[nodiscard]] std::optional<ImportedTextureRef> resolveTextureRef(
    const ufbx_material_map& map, const std::unordered_map<const ufbx_texture*, std::uint32_t>& imageIndexByTexture,
    const ufbx_mesh* firstMesh, ImportResult& result) {
    if (!map.texture_enabled || map.texture == nullptr) {
        return std::nullopt;
    }
    ImportedTextureRef ref;
    const auto it = imageIndexByTexture.find(map.texture);
    ref.imageIndex = (it != imageIndexByTexture.end()) ? it->second : INVALID_SUBASSET;
    // ufbx_wrap_mode has EXACTLY TWO enumerators (REPEAT, CLAMP) -- TextureWrap::MirroredRepeat is
    // UNREACHABLE from this backend (INV-F13/AC-38); it stays reachable through the glTF backend.
    ref.wrapU = map.texture->wrap_u == UFBX_WRAP_CLAMP ? TextureWrap::ClampToEdge : TextureWrap::Repeat;
    ref.wrapV = map.texture->wrap_v == UFBX_WRAP_CLAMP ? TextureWrap::ClampToEdge : TextureWrap::Repeat;
    // minFilter / magFilter / mipFilter keep ImportedTextureRef's defaults: FBX carries no filter
    // information ufbx exposes. A recorded limitation, not an accident.
    ref.uvSet = resolveUvSet(*map.texture, firstMesh, result);
    return ref;
}

// A24: one aggregated warning per mesh, per category. A small per-mesh tally accumulated during the
// part loop and flushed to addWarning AT MOST ONCE, after the loop -- what makes FI31's "exactly one
// warning" assertion structural rather than a discipline, and the only shape that stays O(1) in
// warnings for a 100 000-face mesh.
struct MeshTally {
    std::size_t emptyFaces = 0;
    std::size_t pointFaces = 0;
    std::size_t lineFaces = 0;
};

// D9's tangent-sign rule: a COMPARISON, not a synthesis. sign(dot(cross(normal, tangent), bitangent)),
// clamped to EXACTLY +1.0F or -1.0F -- never a literal 0, even for a degenerate corner (FI34's own
// invariant: "every .w exactly +1.0F or -1.0F").
[[nodiscard]] constexpr float tangentSign(Vec3 normal, Vec3 tangent, Vec3 bitangent) noexcept {
    return dot(cross(normal, tangent), bitangent) >= 0.0F ? 1.0F : -1.0F;
}

// D11/E13: for each cluster in deformer->clusters (BY POSITION), the SLOT it occupies in the
// ImportedSkin::joints array this same deformer produces -- i.e. the value phase 6 writes into a
// vertex's Joints0 entry, and the position phase 7 assigns ImportedSkin::joints[slot] at. Built
// IDENTICALLY by both phases (both skip a null bone_node in the SAME order), which is what keeps a
// per-vertex joint index and ImportedSkin::joints[] in agreement even in that (unreachable in
// practice: ufbx already drops such clusters when connect_broken_elements is false, never set here)
// defensive case.
[[nodiscard]] std::vector<std::uint32_t> clusterJointSlots(const ufbx_skin_deformer& deformer) {
    std::vector<std::uint32_t> slots(deformer.clusters.count, INVALID_SUBASSET);
    std::uint32_t next = 0;
    for (std::size_t i = 0; i < deformer.clusters.count; ++i) {
        if (deformer.clusters.data[i]->bone_node != nullptr) {
            slots[i] = next++;
        }
    }
    return slots;
}

// The 4-influence reduction (D11), in order, and every step load-bearing:
//   0. opts.clean_skin_weights already removed negative, zero and NaN weights.
//   1. ufbx_skin_vertex's slice is ALREADY SORTED BY DECREASING WEIGHT (ufbx.h:1957 -- A11) -- so this
//      is a PREFIX TAKE and the sort below is a no-op today. Kept anyway: it costs nothing on a small
//      window, it makes the ascending-cluster-index TIE-BREAK real rather than incidental (identical
//      on all three lanes), and it survives a ufbx that relaxes the guarantee.
//   2. Keep the four largest. Ties -> ascending cluster index.
//   3. RENORMALIZE -- ufbx.h:1959 is explicit the weights are NOT guaranteed normalized (A11).
//   4. A vertex whose kept weights sum to zero gets {0,0,0,0}/{0,0,0,0} -- NEVER a silent {1,0,0,0},
//      which would bind it to joint 0 (seed S23, FI60).
struct ReducedInfluences {
    std::array<std::uint16_t, 4> joints{};
    Vec4 weights{};
};

[[nodiscard]] ReducedInfluences reduceInfluences(const ufbx_skin_deformer& deformer,
                                                 const std::vector<std::uint32_t>& jointSlots,
                                                 std::uint32_t vertexIndex) {
    struct Influence {
        std::uint16_t joint;
        float weight;
        std::uint32_t cluster;
    };
    ReducedInfluences out;
    if (vertexIndex >= deformer.vertices.count) {
        return out;  // defensive; not reachable through a well-formed ufbx_scene
    }
    const ufbx_skin_vertex& sv = deformer.vertices.data[vertexIndex];
    std::vector<Influence> candidates;
    candidates.reserve(sv.num_weights);
    for (std::uint32_t w = 0; w < sv.num_weights; ++w) {
        const ufbx_skin_weight& sw = deformer.weights.data[sv.weight_begin + w];
        if (sw.cluster_index >= jointSlots.size() || jointSlots[sw.cluster_index] == INVALID_SUBASSET) {
            continue;  // E13: a dropped cluster contributes no influence
        }
        candidates.push_back({static_cast<std::uint16_t>(jointSlots[sw.cluster_index]), static_cast<float>(sw.weight),
                              sw.cluster_index});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Influence& a, const Influence& b) {
        if (a.weight != b.weight) {
            return a.weight > b.weight;
        }
        return a.cluster < b.cluster;  // ties -> ascending cluster index
    });
    if (candidates.size() > 4) {
        candidates.resize(4);
    }
    float sum = 0.0F;
    for (const Influence& c : candidates) {
        sum += c.weight;
    }
    if (sum > 0.0F) {
        std::array<float, 4> w{};
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            out.joints[i] = candidates[i].joint;
            w[i] = candidates[i].weight / sum;  // renormalize (A11 step 3)
        }
        out.weights = Vec4{w[0], w[1], w[2], w[3]};
    }
    // else: candidates summed to zero (or there were none) -- `out` stays value-initialised
    // {0,0,0,0}/{0,0,0,0}, never a lopsided {1,0,0,0} (seed S23, FI60).
    return out;
}

// D12/E16: appends one channel from a baked Vec3 key list (Translation/Scale). `times` is enforced
// STRICTLY INCREASING HERE, never assumed from ufbx (INV-F8) -- a key whose time is <= the previous
// ACCEPTED one is dropped, one warning. `keyBudget` is this call's share of the MODEL-WIDE
// MAX_ANIMATION_KEYS_PER_MODEL cap (D15); the return value is how many keys were actually appended,
// so the caller can track the running total without threading mutable state through this function.
[[nodiscard]] std::size_t appendVec3Channel(ImportResult& result, ImportedAnimation& anim, AnimationPath path,
                                            std::uint32_t targetNode, const ufbx_baked_vec3_list& keys,
                                            std::size_t keyBudget) {
    if (keys.count == 0) {
        return 0;
    }
    ImportedAnimationChannel channel;
    channel.targetNode = targetNode;
    channel.path = path;
    channel.interpolation = AnimationInterpolation::Linear;  // D12: EVERY channel Linear
    channel.times.reserve(keys.count);
    channel.values.reserve(keys.count);
    bool first = true;
    float lastTime = 0.0F;
    std::size_t appended = 0;
    bool droppedNonIncreasing = false;
    for (std::size_t i = 0; i < keys.count && appended < keyBudget; ++i) {
        const float t = f(keys.data[i].time);
        if (!first && t <= lastTime) {
            droppedNonIncreasing = true;  // E16
            continue;
        }
        channel.times.push_back(t);
        channel.values.push_back(toVec4(toVec3(keys.data[i].value), 0.0F));
        lastTime = t;
        first = false;
        ++appended;
    }
    if (droppedNonIncreasing) {
        addWarning(result,
                   std::format("animation '{}': a duplicate or non-increasing timestamp was dropped", anim.name));
    }
    if (!channel.times.empty()) {
        anim.channels.push_back(std::move(channel));
    }
    return appended;
}

// The IDENTICAL shape as appendVec3Channel, for a baked Quat key list (Rotation). ufbx_quat is
// {x,y,z,w} -- THE SAME COMPONENT ORDER as engine::Quat/Vec4 (no reorder, matching toQuat() above).
[[nodiscard]] std::size_t appendQuatChannel(ImportResult& result, ImportedAnimation& anim, AnimationPath path,
                                            std::uint32_t targetNode, const ufbx_baked_quat_list& keys,
                                            std::size_t keyBudget) {
    if (keys.count == 0) {
        return 0;
    }
    ImportedAnimationChannel channel;
    channel.targetNode = targetNode;
    channel.path = path;
    channel.interpolation = AnimationInterpolation::Linear;
    channel.times.reserve(keys.count);
    channel.values.reserve(keys.count);
    bool first = true;
    float lastTime = 0.0F;
    std::size_t appended = 0;
    bool droppedNonIncreasing = false;
    for (std::size_t i = 0; i < keys.count && appended < keyBudget; ++i) {
        const float t = f(keys.data[i].time);
        if (!first && t <= lastTime) {
            droppedNonIncreasing = true;
            continue;
        }
        const ufbx_quat& q = keys.data[i].value;
        channel.times.push_back(t);
        channel.values.push_back(Vec4{f(q.x), f(q.y), f(q.z), f(q.w)});
        lastTime = t;
        first = false;
        ++appended;
    }
    if (droppedNonIncreasing) {
        addWarning(result,
                   std::format("animation '{}': a duplicate or non-increasing timestamp was dropped", anim.name));
    }
    if (!channel.times.empty()) {
        anim.channels.push_back(std::move(channel));
    }
    return appended;
}

}  // namespace

ImportResult importFbx(std::string_view assetRelativeDir, std::span<const std::byte> bytes,
                       const ImportSettings& settings, ImportDepth depth, std::span<const ExternalBuffer> external) {
    // `external` is ALWAYS EMPTY for FBX (D5) and never read -- the parameter exists only so this
    // signature matches importGltf's. `assetRelativeDir` is not yet read (phase 4, Step 6, starts
    // reading it for texture URI resolution). `settings` is read starting THIS commit (phase 3's
    // settings.scale, below). Explicitly (void)-cast rather than left unnamed: every parameter keeps
    // the header's own name, which is what the STEP BOUNDARY comment above (and every reader after it)
    // reads against.
    (void)assetRelativeDir;
    (void)external;

    ImportResult result;

    // ---- phase 1: the load options, every field explicit. This block IS the design: every field is
    // set deliberately, including the ones that equal ufbx's default, so a future reader can tell
    // "considered and left" from "never seen" -- and so a ufbx bump cannot move this importer's output
    // silently.
    ufbx_load_opts opts = {};  // VALUE-INITIALISED. ufbx returns UFBX_ERROR_UNINITIALIZED_OPTIONS
                               // otherwise (_begin_zero/_end_zero guards). Never `ufbx_load_opts opts;`.

    // ---- D17: the format is FORCED. A file named chair.fbx whose contents are Wavefront OBJ must NOT
    // import through this arm -- 3.2.3 owns OBJ. Verified in ufbx.c (ufbxi_determine_format): both
    // detection branches gate on `format == UNKNOWN`, so the first line alone suffices; the other two
    // are belt-and-braces against a future reordering, and they also remove two platform-sensitive
    // inputs (a lookahead read and an extension match) from the set of things three lanes must agree
    // about.
    opts.file_format = UFBX_FILE_FORMAT_FBX;
    opts.no_format_from_content = true;
    opts.no_format_from_extension = true;

    // ---- D4: ufbx NEVER touches the filesystem.
    opts.load_external_files = false;             // explicit, though it is the zero default
    opts.ignore_missing_external_files = false;   // nothing external is ever requested, so nothing
                                                  // is ever missing
    opts.open_file_cb.fn = &refuseToOpenAnyFile;  // DEFENCE IN DEPTH: the default is stdio
    opts.open_file_cb.user = nullptr;

    // ---- D6: the space conversion regime. THIS IS THE DECISION THAT MAKES OR BREAKS THE TASK, and
    // the failure mode is silent -- every count in the panel stays right while the model is 100x too
    // large or lying on its side.
    // MODIFY_GEOMETRY is the ONLY mode where the panel's Bounds row is in metres:
    //   TRANSFORM_ROOT     parks the conversion scale on every root node, where it compounds with
    //                      ImportSettings::scale and leaves ImportedMesh::bounds in centimetres.
    //   ADJUST_TRANSFORMS  converts node translations but NOT geometry -- positions and every bounds
    //                      stay in centimetres while the hierarchy is in metres, and nothing is
    //                      internally inconsistent enough to notice.
    //   MODIFY_GEOMETRY    scales geometry AND adjusts transforms: positions, node translations,
    //                      bounds and inverse bind matrices are ALL in metres.
    // MEASURED against ufbx v0.23.0: a Z-up / UnitScaleFactor 1 source yields geometry_scale 0.01,
    // root_scale 1, a -90-degree X rotation on the roots, and mirror_axis 0.
    opts.target_axes = ufbx_axes_right_handed_y_up;  // aero/core/math.hpp: the engine's conventions
    opts.target_unit_meters = 1.0;                   // were chosen to match glTF 2.0 -- RH, Y-up, metres
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    // handedness_conversion_axis and handedness_conversion_retain_winding are LEFT AT THEIR DEFAULTS
    // DELIBERATELY. RH Z-up -> RH Y-up is a PURE ROTATION: no mirror, no winding change, and
    // metadata.mirror_axis comes back 0 (measured). A genuinely LEFT-handed source makes ufbx mirror
    // one axis and flip winding to compensate, which is exactly what the defaults mean.
    // opts.reverse_winding is NEVER set.

    // ---- D7/D8: helper nodes, because the canonical model has plain TRS nodes and a SHARED mesh
    // list. MODIFY_GEOMETRY here would bake the geometry transform into the vertices -- and one
    // ufbx_mesh instanced by two nodes with DIFFERENT geometry transforms cannot have one set of
    // positions, so it would force per-node mesh COPIES: a change to the canonical model's shape
    // driven by one format. (ufbx knows: its own header says MODIFY_GEOMETRY "will add helper nodes
    // ... if necessary".) PRESERVE leaves ufbx_node.geometry_transform for a consumer that has no
    // field for it, and every consumer that forgot would silently draw geometry in the wrong place.
    opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
    opts.geometry_transform_helper_name =
        ufbxLiteral(GEOMETRY_HELPER_SUFFIX);  // explicit, greppable,
                                              // stable across a ufbx bump, and labellable by the panel
    opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
    opts.scale_helper_name = ufbxLiteral(SCALE_HELPER_SUFFIX);
    // RETAIN is the default, set explicitly: both ADJUST_TO_* modes TRANSLATE OBJECTS to sit at their
    // pivot -- a derivation, and one whose own header notes it "results in geometric translation",
    // which would then feed back into the geometry-transform handling above.
    opts.pivot_handling = UFBX_PIVOT_HANDLING_RETAIN;

    // ---- D9: polygons are TRIANGULATED by us, per face, using ufbx's own tested implementation
    // (phase 6, Step 7). allow_empty_faces stays FALSE so ufbx removes zero-vertex faces itself and
    // reports UFBX_WARNING_EMPTY_FACE_REMOVED, which phase 2 forwards as one aggregated warning.
    opts.allow_empty_faces = false;
    // skip_mesh_parts stays FALSE, and that is LOAD-BEARING: setting it would leave material_parts
    // empty and force this task to write its own face-grouping pass (D10).
    opts.skip_mesh_parts = false;

    // ---- D11: skins (phase 7, Step 8).
    opts.clean_skin_weights = true;   // ufbx removes negative, zero and NaN weights before we see them
    opts.skip_skin_vertices = false;  // explicit: setting it empties exactly the data D11 reduces
    // connect_broken_elements stays FALSE (default): a skin cluster whose bone_node is null is DROPPED
    // by ufbx before we see it, which is what makes "never a nil entry in joints" structural (E13).

    // ---- D16: the caps, enforced INSIDE ufbx, before the allocation each bounds.
    opts.temp_allocator.memory_limit = MAX_FBX_TEMP_BYTES;
    opts.temp_allocator.allocation_limit = MAX_FBX_ALLOCATIONS;
    opts.result_allocator.memory_limit = MAX_FBX_RESULT_BYTES;
    opts.result_allocator.allocation_limit = MAX_FBX_ALLOCATIONS;
    opts.node_depth_limit = MAX_FBX_NODE_DEPTH;
    // huge_threshold is DELIBERATELY NOT set to 1. ufbx offers it as an ASan aid, and it is tempting
    // given the whole sanitizer argument for vendoring -- but gating it on a build configuration would
    // make the Debug and Release lanes exercise DIFFERENT allocation paths, so the sabotage matrix and
    // the test suite would be proving different code in different lanes. It is a LOCAL DEBUGGING
    // LEVER, not a shipped setting.

    // ---- D18: every platform-dependent default, pinned.
    opts.path_separator = '/';  // ufbx's default is '\' ON WINDOWS and '/' everywhere else
    opts.unicode_error_handling = UFBX_UNICODE_ERROR_HANDLING_REPLACEMENT_CHARACTER;
    // FBX names come out of Maya and 3ds Max and are frequently not valid UTF-8 (Shift-JIS and
    // Latin-1 are both common). They land in ImportedMesh::name -> the panel -> ImGui, which requires
    // valid UTF-8. ABORT_LOADING would fail a whole import over a Japanese bone name; UNSAFE_IGNORE
    // would feed invalid bytes to ImGui.

    // ---- D5: the depth, and it is OBSERVABLE rather than asserted -- ufbx sets three metadata flags.
    // NOT `opts.ignore_all_content` (FIX, corrected from an earlier draft of this importer): that flag
    // is EXACTLY `ignore_geometry + ignore_animation + ignore_embedded` (measured in ufbx.c's own
    // `ufbxi_load`, which folds it into the three sub-flags before anything else runs) -- so it also
    // sets `ignore_embedded`, and phase 4's embedded-texture branch below keys on `tex->content`/
    // `tex->video->content` being non-empty. With `ignore_embedded` true at Structure, that content is
    // NEVER populated, so an embedded texture's `RelativeFilename` falls through to the ordinary
    // EXTERNAL path and is recorded as a dependency -- while at Full it correctly has content and is
    // recorded as `embedded`. `Structure` and `Full` disagreed about the URI SET for every embedded
    // texture, breaking AC-20/INV-M4. `ignore_geometry`/`ignore_animation` alone zero every per-mesh
    // count and animation curve exactly as `ignore_all_content` did (§A-4's table is unaffected --
    // both are gated on `ignore_geometry` alone, verified by FI2/FI3 continuing to pass unchanged) --
    // `ignore_embedded` is the ONE sub-flag this importer needs to stay FALSE at every depth, so an
    // embedded texture is distinguishable from an external one identically at Structure and Full. The
    // probe already reads the whole `.fbx` into memory (D4), so the cost is ufbx copying embedded blobs
    // it has already parsed -- not extra I/O -- and stays bounded by MAX_EMBEDDED_BYTES either way.
    opts.ignore_geometry = (depth == ImportDepth::Structure);
    opts.ignore_animation = (depth == ImportDepth::Structure);
    // opts.ignore_embedded is NEVER set -- it stays false (the `= {}` zero-init above) at both depths.

    // ---- LEFT AT DEFAULTS, DELIBERATELY, so a future reader can tell "considered" from "never seen":
    //   thread_opts               -- never set. git grep JobSystem -- editor/ has been empty for five
    //                                tasks and stays empty; a synchronous one-frame hitch on a
    //                                deliberate click is the accepted cost (3.2.1 D4).
    //   generate_missing_normals  -- FALSE. Generating normals is derivation; 3.3.1's cook owns it.
    //   index_error_handling, strict, disable_quirks, allow_unsafe, retain_dom,
    //   evaluate_skinning, evaluate_caches, obj_*  -- all default.
    //   use_blender_pbr_material  -- set TRUE below (D13); listed here so the list stays complete.
    opts.use_blender_pbr_material = true;
    // Blender's FBX exporter converts PBR materials to legacy Phong DETERMINISTICALLY, and ufbx can
    // invert that conversion: its header says such materials "will be read as
    // UFBX_SHADER_BLENDER_PHONG, which means ufbx will be able to parse roughness and metallic
    // textures." A Blender-exported FBX is the single most likely real input to this importer, and
    // this is ufbx's own documented handling of it, not ours.

    ufbx_error err = {};
    const ScenePtr scene(ufbx_load_memory(bytes.data(), bytes.size(), &opts, &err));
    // `ufbx_load_memory` takes `const void*`; `bytes.data()` is `const std::byte*`, which converts
    // implicitly. No `reinterpret_cast`.
    if (!scene) {
        ImportResult failed;
        failed.status = ufbxStatusFor(err.type);
        failed.message = ufbxMessage(err);
        return failed;  // `model` is contractually EMPTY on every non-Ok/Truncated status
    }

    // ---- phase 2: SourceSpace, ufbx's own warnings, the E5/E6 guards, and the cheap structural
    // summary counts.
    const ufbx_scene& s = *scene.get();

    // D19 / A3: `settings.axes.up` is the FILE's declared up axis and ufbx PRESERVES it across the
    // conversion (ufbx.h: "This contains the _original_ axes even if you supply target_axes").
    // `original_axis_up` is NOT that -- it reads the Autodesk round-trip properties OriginalUpAxis /
    // OriginalUpAxisSign, which most exporters never write, and it comes back UNKNOWN for an ordinary
    // file (measured). Consulted only as a fallback.
    SourceSpace& space = result.model.sourceSpace;
    space.declared = true;  // FBX always declares a space
    space.upAxis = axisLetter(s.settings.axes.up != UFBX_COORDINATE_AXIS_UNKNOWN ? s.settings.axes.up
                                                                                 : s.settings.original_axis_up);
    space.unitMeters = f(s.settings.unit_meters);  // 0.01 for a centimetre file
    space.formatVersion = std::format("FBX {} {}", s.metadata.version, s.metadata.ascii ? "ascii" : "binary");
    space.generator = generatorString(s.metadata);  // "" when the exporter is UNKNOWN and
                                                    // metadata.creator is empty (A21)

    // E5: a declared unit that is not finite and positive. This cannot be guarded BEFORE the load --
    // target_unit_meters is set on the opts, while the source's own unit is only knowable after -- so
    // the guard is here. (The folded-bounds half of E5 is validated at the end of phase 6, Step 7,
    // once bounds are folded at all.)
    if (!(std::isfinite(space.unitMeters) && space.unitMeters > 0.0F)) {
        addWarning(result, std::format("the file declares a unit of {} metres per unit, which is not a "
                                       "usable scale; geometry is imported as authored",
                                       space.unitMeters));
    }
    // E6: axes ufbx could not use at all.
    if (!ufbx_coordinate_axes_valid(s.settings.axes)) {
        addWarning(result, "the file declares no usable coordinate axes; geometry is imported as authored");
    }

    // ufbx's OWN warnings, forwarded through the SAME capped list our warnings use. Each ufbx_warning
    // already carries a `count`, so an empty-face removal over 10 000 faces is ONE entry, not 10 000.
    for (const ufbx_warning& w : s.metadata.warnings) {
        addWarning(result, w.count > 1 ? std::format("{} (x{})", toStd(w.description), w.count) : toStd(w.description));
    }

    // task 3.2.2 (Step 4): cheap, STRUCTURAL summary counts, read directly from ufbx's own already-
    // computed scene/mesh fields -- NOT built from model.meshes/materials/... (phases 4-8, Steps 6-9,
    // build those vectors; they stay empty until then). meshCount/materialCount/skinCount/
    // animationCount are ELEMENT counts (scene.meshes.count etc.), unaffected by `ignore_geometry`/
    // `ignore_animation`, since element LISTS survive at Structure depth -- only their CONTENT is
    // zeroed (§A-4). vertexCount/triangleCount are likewise cheap: ufbx_mesh::num_vertices/
    // num_triangles are populated by ufbx itself during parsing, independent of OUR OWN phase-6
    // triangulation/index-generation pipeline, and correctly read 0 at Structure depth for the
    // identical reason (`ignore_geometry` zeroes them before we ever see them). imageCount is an
    // INTERIM proxy (every texture, not yet narrowed to UFBX_TEXTURE_FILE); phase 4 (Step 6) narrows
    // it. summary.bounds is
    // NOT folded here -- that is phase 6's "fold bounds" step (Step 7), which needs settings.scale and
    // the real per-primitive positions, never the raw unique position pool.
    result.model.summary.meshCount = s.meshes.count;
    result.model.summary.materialCount = s.materials.count;
    result.model.summary.imageCount = s.textures.count;
    result.model.summary.skinCount = s.skin_deformers.count;
    result.model.summary.animationCount = s.anim_stacks.count;
    for (const ufbx_mesh* mesh : s.meshes) {
        result.model.summary.vertexCount += mesh->num_vertices;
        result.model.summary.triangleCount += mesh->num_triangles;
    }

    // ---- phase 3: nodes, iterative, localId == ufbx typed_id. THIS IS WHERE D6'S WHOLE CONVERSION
    // REGIME IS EITHER RIGHT OR SILENTLY WRONG -- every count in the panel stays plausible while the
    // model is 100x too large or lying on its side, so §D-6's hand-computed literals (FI15/FI16 in
    // this commit's own test file) are what actually proves this, not this code's own shape.
    //
    // A13: scene.nodes[0] IS the root (is_root == true, name ""). allow_nodes_out_of_root stays false
    // (its default, never set above), so every authored node is reachable from it and NO ORPHAN SCAN
    // IS NEEDED. ImportedNode::localId is the RAW ufbx typed_id -- NOT the position in
    // ImportedModel::nodes -- because ufbx_baked_node::typed_id (phase 8's animation target, Step 9)
    // maps to scene.nodes[] and must resolve without a side table. nodeIndexByLocalId is what turns
    // one into the other, here and in every later phase that resolves a node reference.
    std::unordered_map<std::uint32_t, std::uint32_t> nodeIndexByLocalId;
    for (const ufbx_node* n : s.nodes) {
        if (n->is_root) {
            continue;  // the FBX root is a CONTAINER, not an authored node: emitting it would put a
                       // node in the panel's Hierarchy that appears in no DCC outliner
        }
        if (result.model.nodes.size() >= MAX_NODES_PER_MODEL) {
            escalate(result, ImportStatus::Truncated, "node cap (MAX_NODES_PER_MODEL) reached");
            break;
        }
        ImportedNode out;
        out.name = toStd(n->name);
        out.localId = n->typed_id;
        out.parent = (n->parent != nullptr && !n->parent->is_root) ? n->parent->typed_id : INVALID_SUBASSET;

        // THE WHOLE CONVERSION, for every space_conversion mode: ufbx has already baked target_axes,
        // target_unit_meters and the adjust_pre_* terms into local_transform (its own header says so
        // under adjust_pre_translation). Copy the three members. Nothing is swapped, negated,
        // transposed or reordered (INV-F7).
        out.translation = toVec3(n->local_transform.translation);
        out.rotation = toQuat(n->local_transform.rotation);
        out.scale = toVec3(n->local_transform.scale);
        out.meshIndex = INVALID_SUBASSET;  // filled by phase 6 (Step 7)
        out.skinIndex = INVALID_SUBASSET;  // filled by phase 7 (Step 8)
        nodeIndexByLocalId.emplace(out.localId, static_cast<std::uint32_t>(result.model.nodes.size()));
        result.model.nodes.push_back(std::move(out));
    }
    result.model.summary.nodeCount = result.model.nodes.size();

    // children, in a SECOND pass, so a forward reference is impossible; and `roots` from the FBX
    // root's own children, in ufbx's own order (AC-25). `n` itself is only ever READ here -- the
    // WRITE lands on a DIFFERENT node (its parent, found by index) -- so it is const.
    for (const ImportedNode& n : result.model.nodes) {
        if (n.parent != INVALID_SUBASSET) {
            const auto it = nodeIndexByLocalId.find(n.parent);
            if (it != nodeIndexByLocalId.end()) {
                result.model.nodes[it->second].children.push_back(n.localId);
            }
        }
    }
    for (const ufbx_node* child : s.root_node->children) {
        if (nodeIndexByLocalId.contains(child->typed_id)) {
            result.model.roots.push_back(child->typed_id);
        }
    }

    // A22: settings.scale applies to ROOT node translations ONLY -- the same three places glTF
    // touches (positions, root translations, inverse-bind translations). Composed with D6's unit
    // conversion by multiplying AFTER ufbx (never passed to ufbx as a second target_unit_meters),
    // which is what keeps SourceSpace's reported unit truthful.
    for (const std::uint32_t rootId : result.model.roots) {
        result.model.nodes[nodeIndexByLocalId.at(rootId)].translation *= settings.scale;
    }

    // ---- phase 4: textures -> ImportedImage (D14). Runs at BOTH depths (INV-M4): images and URIs are
    // scene ELEMENTS -- unaffected by `ignore_geometry`/`ignore_animation`, AND (FIX, above) unaffected
    // by `ignore_embedded` either, since that stays false at both depths -- and this is exactly what
    // phase 7.5's probe and the pre-Full panel read. imageCount was an INTERIM proxy in phase 2 (every
    // texture, including LAYERED/PROCEDURAL/SHADER); this phase NARROWS it to the FILE-type count
    // actually turned into an ImportedImage.
    std::unordered_map<const ufbx_texture*, std::uint32_t> imageIndexByTexture;
    bool externalUriCapHit = false;
    const auto recordExternalUri = [&](const std::string& path) {
        if (std::find(result.externalUris.begin(), result.externalUris.end(), path) != result.externalUris.end()) {
            return;  // already present -- E12's dedup, first-seen order
        }
        if (result.externalUris.size() >= MAX_EXTERNAL_URIS) {
            if (!externalUriCapHit) {
                escalate(result, ImportStatus::Truncated, "external URI cap (MAX_EXTERNAL_URIS) reached");
                externalUriCapHit = true;
            }
            return;
        }
        result.externalUris.push_back(path);
    };
    for (const ufbx_texture* tex : s.textures) {
        if (tex->type != UFBX_TEXTURE_FILE) {
            // D14/D22: LAYERED, PROCEDURAL and SHADER textures are SKIPPED, VISIBLY -- ImportedImage
            // has no way to express a blend stack, and inventing one here would foreclose a decision no
            // roadmap row has taken.
            addWarning(result, std::format("texture '{}' is a {} texture and is not imported", toStd(tex->name),
                                           textureTypeName(tex->type)));
            continue;
        }
        ImportedImage img;
        // EMBEDDED FIRST (3.2.1's D14, unchanged): content non-empty means no path, no GUID, and NEVER
        // a dependency. `ignore_embedded` is NEVER set (FIX, above) -- it stays false at BOTH depths,
        // so an embedded texture's content is populated, and this branch is taken, IDENTICALLY at
        // Structure and Full. An earlier draft folded `ignore_all_content` (which also sets
        // `ignore_embedded`) into this depth check; that made an embedded texture's content empty at
        // Structure, so it fell through to the external-path branch below and was wrongly recorded as
        // a dependency -- exactly the AC-20/INV-M4 disagreement FI2/FI3 now assert against directly.
        const ufbx_blob& content =
            tex->content.size > 0 ? tex->content : (tex->video != nullptr ? tex->video->content : ufbx_empty_blob);
        if (content.size > 0) {
            img.embedded = true;
            if (content.size > MAX_EMBEDDED_BYTES) {
                escalate(result, ImportStatus::Truncated, "an embedded texture exceeds MAX_EMBEDDED_BYTES");
            }
            imageIndexByTexture.emplace(tex, static_cast<std::uint32_t>(result.model.images.size()));
            result.model.images.push_back(std::move(img));
            continue;
        }
        // 1. relative_filename, falling back to the BASENAME of filename.
        // 2. absolute_filename is NEVER READ -- it is C:\Users\bob\Desktop\wood.png in every Autodesk
        //    export and is precisely D4's threat model. Not consulted, not fallen back to, not shown.
        std::string raw = toStd(tex->relative_filename);
        if (raw.empty()) {
            raw = basenameOf(toStd(tex->filename));
        }
        // 3. FOLD, THEN CLASSIFY -- the secure order (A19). Folding AFTER classification would let
        // `..\..\..\etc\passwd` past the escape check.
        img.uri = foldBackslashesToSlashes(raw);
        const UriClassification cls = classifyUri(img.uri, assetRelativeDir);
        if (cls.kind == UriClass::RelativePath) {
            img.relativePath = cls.relativePath;
            recordExternalUri(cls.relativePath);
        } else if (cls.kind == UriClass::DataUri) {
            img.embedded = true;  // an FBX path can't legitimately be a data: URI; treat as embedded
                                  // rather than as a read
        } else {
            img.refusal = cls.reason;  // the EXACT reason, shown by the panel (AC-52)
            addWarning(result, std::format("texture '{}': {}", toStd(tex->name), cls.reason));
        }
        imageIndexByTexture.emplace(tex, static_cast<std::uint32_t>(result.model.images.size()));
        result.model.images.push_back(std::move(img));
    }
    result.model.summary.imageCount = result.model.images.size();

    // ---- phase 5: materials from `pbr` ONLY (D13). Runs at BOTH depths -- a material has no vertex
    // content of its own, so nothing here is gated by `ignore_geometry`/`ignore_animation`. Skipped
    // entirely when `!settings.importMaterials`, but phase 4 already ran, so images and `externalUris`
    // survive regardless (AC-39's non-obvious half). `material->fbx` (the legacy Phong/Lambert maps) is
    // never read anywhere in this file -- §V6 greps for it.
    if (settings.importMaterials) {
        // The pre-pass phase 5 needs: "the first mesh using this material", for resolveUvSet. Reads
        // ONLY `mesh->materials` (a scene-element list), never vertex data -- valid at Structure depth
        // too, where it simply finds every mesh's `uv_sets` empty and every uvSet falls back to 0.
        std::unordered_map<std::uint32_t, const ufbx_mesh*> firstMeshForMaterial;
        for (const ufbx_mesh* mesh : s.meshes) {
            for (const ufbx_material* mat : mesh->materials) {
                firstMeshForMaterial.try_emplace(mat->typed_id, mesh);
            }
        }
        for (const ufbx_material* mat : s.materials) {
            ImportedMaterial out;
            out.name = toStd(mat->name);
            out.localId = mat->typed_id;
            const ufbx_material_pbr_maps& pbr = mat->pbr;
            const ufbx_material_features& feat = mat->features;

            if (engaged(pbr.base_color)) {
                out.baseColorFactor = toVec4(pbr.base_color.value_vec4);
                if (engaged(pbr.base_factor)) {
                    out.baseColorFactor = out.baseColorFactor * f(pbr.base_factor.value_real);
                }
            }
            if (feat.opacity.enabled && engaged(pbr.opacity)) {
                out.baseColorFactor.w = f(pbr.opacity.value_real);
            }
            if (engaged(pbr.metalness)) {
                out.metallicFactor = f(pbr.metalness.value_real);
            }
            // The ONE arithmetic transformation in the material path (INV-F13): a documented UNIT
            // INVERSION ufbx itself flags via `roughness_as_glossiness`, never a Phong derivation.
            if (feat.roughness_as_glossiness.enabled) {
                if (engaged(pbr.glossiness)) {
                    out.roughnessFactor = 1.0F - f(pbr.glossiness.value_real);
                }
            } else if (engaged(pbr.roughness)) {
                out.roughnessFactor = f(pbr.roughness.value_real);
            }
            if (engaged(pbr.emission_color)) {
                out.emissiveFactor = toVec3(pbr.emission_color.value_vec3);
                if (engaged(pbr.emission_factor)) {
                    out.emissiveFactor = out.emissiveFactor * f(pbr.emission_factor.value_real);
                }
            }
            // `Mask` is NEVER produced (INV-F13/AC-38's sibling) -- FBX has no cutoff concept, and
            // `Blend` is the conservative choice for a partially- or fully-transparent material.
            //
            // MEASURED CORRECTION to the plan's own D13 wording (which tested `pbr.opacity.value_real
            // < 1.0` unconditionally, with no `engaged()` gate): `ufbxi_fetch_maps` (ufbx.c) sets
            // `features.opacity.enabled = true` UNCONDITIONALLY for every shader whose capability mask
            // includes OPACITY (a shader-type CAPABILITY, not "opacity was authored") -- and the whole
            // `ufbx_material_pbr_maps` struct is memset to ZERO before any property is fetched, with NO
            // update-to-1.0 pass for `opacity` the way there is for the paired factor/color fields
            // (`ufbxi_update_factor`, called for base/specular/emission/... but never for opacity).
            // Executed: a material using such a shader that never authors an opacity value reports
            // `pbr.opacity.has_value == false` AND `pbr.opacity.value_real == 0.0`. Without this
            // `engaged()` gate, EVERY such material -- which is most of them, since "can this shader
            // express opacity" is a broad capability bit -- would read `0.0 < 1.0` and report `Blend`
            // for a material that never mentioned transparency at all. `engaged()` is NOT required for
            // the `texture_enabled` half: a texture-only opacity input (no constant factor at all) is
            // still `Blend`, matching D13's own texture-slot rule elsewhere in this table.
            out.alphaMode = (feat.opacity.enabled && ((engaged(pbr.opacity) && f(pbr.opacity.value_real) < 1.0F) ||
                                                      pbr.opacity.texture_enabled))
                                ? AlphaMode::Blend
                                : AlphaMode::Opaque;
            out.doubleSided = feat.double_sided.enabled;
            // normalScale / occlusionStrength / alphaCutoff are DELIBERATELY left at their struct
            // defaults -- FBX has no separate scale or cutoff concept. Recorded here, not silently
            // defaulted by omission (FI47).

            const auto meshIt = firstMeshForMaterial.find(mat->typed_id);
            const ufbx_mesh* firstMesh = meshIt != firstMeshForMaterial.end() ? meshIt->second : nullptr;
            out.baseColor = resolveTextureRef(pbr.base_color, imageIndexByTexture, firstMesh, result);
            // FBX has separate metalness/roughness texture slots where glTF has ONE combined
            // metallic-roughness texture; ImportedMaterial follows glTF's shape, so this picks
            // WHICHEVER of the two is connected, preferring metalness -- a decided approximation, not a
            // derivation.
            out.metallicRoughness = resolveTextureRef(pbr.metalness, imageIndexByTexture, firstMesh, result);
            if (!out.metallicRoughness.has_value()) {
                out.metallicRoughness = resolveTextureRef(pbr.roughness, imageIndexByTexture, firstMesh, result);
            }
            out.normal = resolveTextureRef(pbr.normal_map, imageIndexByTexture, firstMesh, result);
            out.occlusion = resolveTextureRef(pbr.ambient_occlusion, imageIndexByTexture, firstMesh, result);
            out.emissive = resolveTextureRef(pbr.emission_color, imageIndexByTexture, firstMesh, result);

            result.model.materials.push_back(std::move(out));
        }
    }
    result.model.summary.materialCount = result.model.materials.size();

    // ---- phase 6: meshes, triangulation, index generation (D9/D10). Full depth only for real
    // content; Structure gets ONE empty placeholder primitive per mesh (§A-4 -- `ignore_geometry`
    // zeroes every per-mesh count and empties material_parts, so there is nothing structural left to
    // report but the mesh's own identity). Three running MODEL-WIDE caps (D15), each checked BEFORE
    // the allocation it bounds, using COUNTS ufbx already computed (no allocation to read them).
    //
    // NOTE ON summary.vertexCount/triangleCount: phase 2 ALREADY set these from ufbx's own
    // mesh->num_vertices/num_triangles (real, cheap, and -- critically -- UNAFFECTED by this phase's
    // own triangulation/dedup choices). This phase does NOT re-derive them: doing so would double-
    // count against phase 2's number for the simple fixtures in this suite, and would produce a
    // DIFFERENT (larger) number for any real mesh with a UV seam or a hard-edge normal, where ufbx's
    // logical vertex count and this phase's post-triangulation unique-vertex count genuinely diverge.
    std::size_t primitiveTotal = 0;
    std::size_t vertexTotal = 0;
    std::size_t indexTotal = 0;
    bool primitiveCapHit = false;
    bool vertexCapHit = false;
    bool indexCapHit = false;
    // ufbx_mesh::typed_id -> this model's ImportedMesh position, so the meshIndex pass below (which
    // walks s.nodes, not s.meshes) resolves node->mesh in one hop. meshSkinIndexByTypedId is the
    // identical shape for ImportedNode::skinIndex, filled by phase 7 below (D11, Full only).
    std::unordered_map<std::uint32_t, std::uint32_t> meshIndexByTypedId;
    std::unordered_map<std::uint32_t, std::uint32_t> meshSkinIndexByTypedId;

    for (const ufbx_mesh* mesh : s.meshes) {
        ImportedMesh outMesh;
        outMesh.name = toStd(mesh->name);
        outMesh.localId = mesh->typed_id;

        if (depth == ImportDepth::Structure) {
            outMesh.primitives.emplace_back();  // materialIndex INVALID_SUBASSET, attributes None
            outMesh.bounds = Aabb{};            // a point box: the "nothing survived" collapse
            ++result.model.summary.primitiveCount;
            meshIndexByTypedId.emplace(mesh->typed_id, static_cast<std::uint32_t>(result.model.meshes.size()));
            result.model.meshes.push_back(std::move(outMesh));
            continue;
        }

        // ---- phase 7: skins (D11, Full only -- §A-4's skins[].joints is EMPTY at Structure depth,
        // matching the mesh content it is keyed on; skinCount alone stays real at both depths, from
        // phase 2's own cheap element count, untouched here). Resolved per-mesh HERE, before the
        // per-part loop below, because the per-vertex joint/weight reduction (§D-4.8 point 7) must run
        // INSIDE that same loop, keyed on THIS deformer and its joint-slot mapping.
        const ufbx_skin_deformer* deformer =
            (settings.importSkins && mesh->skin_deformers.count > 0) ? mesh->skin_deformers.data[0] : nullptr;
        std::vector<std::uint32_t> jointSlots;
        bool skinEngaged = false;
        if (deformer != nullptr) {
            if (deformer->clusters.count > MAX_JOINTS_PER_SKIN) {
                // E14: a partial palette binds vertices to wrong bones -- the skin is DROPPED, not
                // truncated, and (unlike a normal cap) the MESH also carries no joints/weights at all:
                // `skinEngaged` stays false, so the per-vertex reduction below never runs for it.
                escalate(result, ImportStatus::Truncated, "joint cap (MAX_JOINTS_PER_SKIN) reached");
            } else {
                jointSlots = clusterJointSlots(*deformer);
                ImportedSkin outSkin;
                outSkin.name = toStd(deformer->name);
                outSkin.localId = deformer->typed_id;
                outSkin.joints.reserve(deformer->clusters.count);
                for (std::size_t ci = 0; ci < deformer->clusters.count; ++ci) {
                    const ufbx_skin_cluster* cluster = deformer->clusters.data[ci];
                    if (cluster->bone_node == nullptr) {
                        // E13: defensive -- ufbx already drops these (connect_broken_elements ==
                        // false, never set here), so this is not reachable by any fixture in this
                        // suite.
                        addWarning(result, std::format("skin '{}': a cluster has no bone; skipped", outSkin.name));
                        continue;
                    }
                    outSkin.joints.push_back(cluster->bone_node->typed_id);
                    Mat4 ibm = toMat4(cluster->geometry_to_bone);
                    // A22: settings.scale multiplies ONLY the translation column.
                    ibm.columns[3].x *= settings.scale;
                    ibm.columns[3].y *= settings.scale;
                    ibm.columns[3].z *= settings.scale;
                    outSkin.inverseBindMatrices.push_back(ibm);
                }
                outSkin.skeletonRoot = INVALID_SUBASSET;  // recorded, not derived (D11): FBX has no
                                                          // single field for it, and computing a common
                                                          // ancestor would be a derivation
                if (deformer->max_weights_per_vertex > 4) {
                    addWarning(result, std::format("mesh '{}': up to {} skinning influences per vertex; "
                                                   "kept the largest 4",
                                                   outMesh.name, deformer->max_weights_per_vertex));
                }
                result.model.summary.jointCount += outSkin.joints.size();
                meshSkinIndexByTypedId.emplace(mesh->typed_id, static_cast<std::uint32_t>(result.model.skins.size()));
                result.model.skins.push_back(std::move(outSkin));
                skinEngaged = true;
            }
        }

        // 3.2.1's code-review BLOCKING-3 lesson, applied here from the start: fold into a LOCAL
        // accumulator that starts at Aabb::empty(), NEVER directly into outMesh.bounds -- whose
        // default is a VALID point box at the origin, so folding straight into it would union the
        // origin into every mesh regardless of where its geometry actually sits.
        Aabb meshBounds = Aabb::empty();
        MeshTally tally;
        // ufbx_triangulate_face writes at most max_face_triangles * 3 indices. Sized ONCE PER MESH,
        // never per face.
        std::vector<std::uint32_t> faceIndices(std::max<std::size_t>(mesh->max_face_triangles, 1) * 3);

        for (std::size_t partIdx = 0; partIdx < mesh->material_parts.count; ++partIdx) {
            const ufbx_mesh_part& part = mesh->material_parts.data[partIdx];
            tally.emptyFaces += part.num_empty_faces;
            tally.pointFaces += part.num_point_faces;
            tally.lineFaces += part.num_line_faces;
            if (part.num_triangles == 0) {
                continue;  // every face in this part was empty/point/line -- already tallied above
            }

            if (primitiveTotal >= MAX_PRIMITIVES_PER_MODEL) {
                if (!primitiveCapHit) {
                    escalate(result, ImportStatus::Truncated, "primitive cap (MAX_PRIMITIVES_PER_MODEL) reached");
                    primitiveCapHit = true;
                }
                continue;
            }
            const std::size_t partCornerCount = part.num_triangles * 3;
            if (indexTotal + partCornerCount > MAX_INDICES_PER_MODEL) {
                if (!indexCapHit) {
                    escalate(result, ImportStatus::Truncated, "index cap (MAX_INDICES_PER_MODEL) reached");
                    indexCapHit = true;
                }
                continue;
            }
            if (vertexTotal + partCornerCount > MAX_VERTICES_PER_MODEL) {
                if (!vertexCapHit) {
                    escalate(result, ImportStatus::Truncated, "vertex cap (MAX_VERTICES_PER_MODEL) reached");
                    vertexCapHit = true;
                }
                continue;
            }

            // Triangulate every real face in this part into the RAW ufbx "combined vertex/attribute"
            // index space (0..mesh->num_indices) -- NOT logical vertex indices. This is the space
            // ufbx_get_vertex_vec3/2/4 index into directly (MEASURED against the vendored ufbx.c: a
            // one-quad mesh has num_indices==4, and ufbx_triangulate_face's own output re-uses values
            // from that same 4-entry space, e.g. `0 1 2 2 3 0` for a quad's two triangles).
            std::vector<std::uint32_t> triIndices;
            triIndices.reserve(partCornerCount);
            for (const std::uint32_t faceIdx : part.face_indices) {
                const ufbx_face face = mesh->faces.data[faceIdx];
                if (face.num_indices < 3) {
                    continue;  // empty/point/line -- already tallied at the part level above
                }
                const std::uint32_t numTris = ufbx_triangulate_face(faceIndices.data(), faceIndices.size(), mesh, face);
                for (std::uint32_t t = 0; t < numTris * 3; ++t) {
                    triIndices.push_back(faceIndices[t]);
                }
            }
            if (triIndices.empty()) {
                continue;  // every face in this part was degenerate after triangulation
            }

            const bool hasNormal = mesh->vertex_normal.exists;
            const bool hasUv0 = mesh->vertex_uv.exists;
            const bool hasUv1 = mesh->uv_sets.count > 1 && mesh->uv_sets.data[1].vertex_uv.exists;
            const bool hasColor = mesh->vertex_color.exists;
            // F7: a tangent basis with no normal to compare against is meaningless -- gated on all
            // three existing, a small strengthening beyond D9's literal "both tangent and bitangent"
            // wording, harmless in practice (a real mesh with tangents always has normals too).
            const bool hasTangent = hasNormal && mesh->vertex_tangent.exists && mesh->vertex_bitangent.exists;
            const bool hasSkin = skinEngaged;

            // ONE STREAM PER ATTRIBUTE (A12), each a packed array of a single POD, each VALUE-
            // INITIALISED regardless (ufbx.h:4068 asks for it, and it costs nothing here: ufbx_vec2/3/4
            // are unions with ufbx_real == double, so there is no padding on any of the three ABIs).
            // The tangent stream is FOUR-WIDE (xyz + the computed SIGN, §D-4.8 point 6) so uniqueness
            // naturally accounts for a seam where the sign flips even when normal/tangent/uv do not.
            // The joints/weights streams are ALSO ufbx_vec4 -- REAL numbers holding EXACT small
            // integers (a joint SLOT index, well within double's exact-integer range) and the already-
            // renormalized weights respectively; this is what lets the 4-influence reduction (D11)
            // participate in the SAME memcmp-based dedup as every other attribute.
            std::vector<ufbx_vec3> stagePositions(triIndices.size(), ufbx_vec3{});
            std::vector<ufbx_vec3> stageNormals(hasNormal ? triIndices.size() : 0, ufbx_vec3{});
            std::vector<ufbx_vec2> stageUv0(hasUv0 ? triIndices.size() : 0, ufbx_vec2{});
            std::vector<ufbx_vec2> stageUv1(hasUv1 ? triIndices.size() : 0, ufbx_vec2{});
            std::vector<ufbx_vec4> stageColors(hasColor ? triIndices.size() : 0, ufbx_vec4{});
            std::vector<ufbx_vec4> stageTangentSign(hasTangent ? triIndices.size() : 0, ufbx_vec4{});
            std::vector<ufbx_vec4> stageJoints(hasSkin ? triIndices.size() : 0, ufbx_vec4{});
            std::vector<ufbx_vec4> stageWeights(hasSkin ? triIndices.size() : 0, ufbx_vec4{});

            for (std::size_t k = 0; k < triIndices.size(); ++k) {
                const std::uint32_t ix = triIndices[k];
                stagePositions[k] = ufbx_get_vertex_vec3(&mesh->vertex_position, ix);
                if (hasNormal) {
                    stageNormals[k] = ufbx_get_vertex_vec3(&mesh->vertex_normal, ix);
                }
                if (hasUv0) {
                    stageUv0[k] = ufbx_get_vertex_vec2(&mesh->vertex_uv, ix);
                }
                if (hasUv1) {
                    stageUv1[k] = ufbx_get_vertex_vec2(&mesh->uv_sets.data[1].vertex_uv, ix);
                }
                if (hasColor) {
                    stageColors[k] = ufbx_get_vertex_vec4(&mesh->vertex_color, ix);
                }
                if (hasTangent) {
                    const ufbx_vec3 t = ufbx_get_vertex_vec3(&mesh->vertex_tangent, ix);
                    const ufbx_vec3 b = ufbx_get_vertex_vec3(&mesh->vertex_bitangent, ix);
                    const float sign = tangentSign(toVec3(stageNormals[k]), toVec3(t), toVec3(b));
                    stageTangentSign[k] = ufbx_vec4{t.x, t.y, t.z, sign};
                }
                if (hasSkin) {
                    // §D-4.8 point 7: the logical VERTEX this corner refers to (mesh->vertex_indices,
                    // the SAME array vertex_position.indices mirrors) is what deformer->vertices[] is
                    // keyed on -- NOT `ix` itself, which is the per-CORNER index space.
                    const std::uint32_t vertexIndex = mesh->vertex_indices.data[ix];
                    const ReducedInfluences inf = reduceInfluences(*deformer, jointSlots, vertexIndex);
                    stageJoints[k] = ufbx_vec4{static_cast<double>(inf.joints[0]), static_cast<double>(inf.joints[1]),
                                               static_cast<double>(inf.joints[2]), static_cast<double>(inf.joints[3])};
                    stageWeights[k] = ufbx_vec4{inf.weights.x, inf.weights.y, inf.weights.z, inf.weights.w};
                }
            }

            std::vector<ufbx_vertex_stream> streams;
            streams.push_back({stagePositions.data(), stagePositions.size(), sizeof(ufbx_vec3)});
            if (hasNormal) {
                streams.push_back({stageNormals.data(), stageNormals.size(), sizeof(ufbx_vec3)});
            }
            if (hasUv0) {
                streams.push_back({stageUv0.data(), stageUv0.size(), sizeof(ufbx_vec2)});
            }
            if (hasUv1) {
                streams.push_back({stageUv1.data(), stageUv1.size(), sizeof(ufbx_vec2)});
            }
            if (hasColor) {
                streams.push_back({stageColors.data(), stageColors.size(), sizeof(ufbx_vec4)});
            }
            if (hasTangent) {
                streams.push_back({stageTangentSign.data(), stageTangentSign.size(), sizeof(ufbx_vec4)});
            }
            if (hasSkin) {
                streams.push_back({stageJoints.data(), stageJoints.size(), sizeof(ufbx_vec4)});
                streams.push_back({stageWeights.data(), stageWeights.size(), sizeof(ufbx_vec4)});
            }

            ufbx_error genErr = {};
            const std::size_t uniqueVertices = ufbx_generate_indices(streams.data(), streams.size(), triIndices.data(),
                                                                     triIndices.size(), nullptr, &genErr);
            if (genErr.type != UFBX_ERROR_NONE) {
                // Defensive: NOT reachable by any fixture in this suite (a null allocator has no
                // configured limit to hit), kept because ufbx_generate_indices' own signature can fail.
                addWarning(result, std::format("mesh '{}' part {}: {}", outMesh.name, partIdx, ufbxMessage(genErr)));
                continue;
            }

            ImportedPrimitive outPrim;
            // §A-23: bounds-checked, INVALID_SUBASSET on any violation, NEVER a warning -- a mesh with
            // no materials connected is the ordinary case, not an error.
            outPrim.materialIndex = (settings.importMaterials && partIdx < mesh->materials.count &&
                                     mesh->materials.data[partIdx] != nullptr)
                                        ? mesh->materials.data[partIdx]->typed_id
                                        : INVALID_SUBASSET;
            outPrim.attributes = VertexAttribute::Position;

            outPrim.positions.reserve(uniqueVertices);
            for (std::size_t i = 0; i < uniqueVertices; ++i) {
                outPrim.positions.push_back(toVec3(stagePositions[i]) * settings.scale);  // A22: POST-scale
            }
            if (hasNormal) {
                outPrim.normals.reserve(uniqueVertices);
                for (std::size_t i = 0; i < uniqueVertices; ++i) {
                    outPrim.normals.push_back(toVec3(stageNormals[i]));  // NEVER scaled
                }
                outPrim.attributes |= VertexAttribute::Normal;
            }
            if (hasUv0) {
                outPrim.uv0.reserve(uniqueVertices);
                for (std::size_t i = 0; i < uniqueVertices; ++i) {
                    outPrim.uv0.push_back(toVec2(stageUv0[i]));
                }
                outPrim.attributes |= VertexAttribute::TexCoord0;
            }
            if (hasUv1) {
                outPrim.uv1.reserve(uniqueVertices);
                for (std::size_t i = 0; i < uniqueVertices; ++i) {
                    outPrim.uv1.push_back(toVec2(stageUv1[i]));
                }
                outPrim.attributes |= VertexAttribute::TexCoord1;
            }
            if (hasColor) {
                outPrim.colors.reserve(uniqueVertices);
                for (std::size_t i = 0; i < uniqueVertices; ++i) {
                    outPrim.colors.push_back(toVec4(stageColors[i]));
                }
                outPrim.attributes |= VertexAttribute::Color0;
            }
            if (hasTangent) {
                outPrim.tangents.reserve(uniqueVertices);
                for (std::size_t i = 0; i < uniqueVertices; ++i) {
                    outPrim.tangents.push_back(toVec4(stageTangentSign[i]));  // NEVER scaled
                }
                outPrim.attributes |= VertexAttribute::Tangent;
            }
            if (hasSkin) {
                outPrim.joints.reserve(uniqueVertices);
                outPrim.weights.reserve(uniqueVertices);
                for (std::size_t i = 0; i < uniqueVertices; ++i) {
                    const ufbx_vec4& j = stageJoints[i];
                    outPrim.joints.push_back({static_cast<std::uint16_t>(j.x), static_cast<std::uint16_t>(j.y),
                                              static_cast<std::uint16_t>(j.z), static_cast<std::uint16_t>(j.w)});
                    outPrim.weights.push_back(toVec4(stageWeights[i]));  // NEVER scaled
                }
                outPrim.attributes |= VertexAttribute::Joints0 | VertexAttribute::Weights0;
            }
            outPrim.indices = std::move(triIndices);  // rewritten IN PLACE by ufbx_generate_indices

            Aabb primBounds = Aabb::empty();
            for (const Vec3& p : outPrim.positions) {
                primBounds.expand(p);
            }
            outPrim.bounds = primBounds;
            // 3.2.1's BLOCKING-3 lesson, restated: fold from THIS SAME primBounds, never from
            // outMesh.bounds after the fact.
            meshBounds.expand(primBounds);
            result.model.summary.bounds.expand(primBounds);

            ++result.model.summary.primitiveCount;
            ++primitiveTotal;
            vertexTotal += uniqueVertices;
            indexTotal += outPrim.indices.size();

            outMesh.primitives.push_back(std::move(outPrim));
        }

        // Flush the tally: AT MOST one warning naming whichever counts are non-zero (A24/FI31).
        if (tally.pointFaces > 0 || tally.lineFaces > 0 || tally.emptyFaces > 0) {
            addWarning(result, std::format("mesh '{}': skipped {} point face(s), {} line face(s), {} empty face(s)",
                                           outMesh.name, tally.pointFaces, tally.lineFaces, tally.emptyFaces));
        }

        // E9: a mesh whose every part was skipped SURVIVES as a point box at the origin -- the
        // untouched aggregate default, Aabb{} -- never the invalid Aabb::empty() sentinel meshBounds
        // started from. A mesh that DID gain real geometry keeps its real, off-origin-capable fold.
        outMesh.bounds = meshBounds.valid() ? meshBounds : Aabb{};
        meshIndexByTypedId.emplace(mesh->typed_id, static_cast<std::uint32_t>(result.model.meshes.size()));
        result.model.meshes.push_back(std::move(outMesh));
    }
    result.model.summary.meshCount = result.model.meshes.size();

    // ImportedNode::meshIndex/skinIndex, assigned by walking s.nodes once more: node->mesh != nullptr
    // -> meshIndexByTypedId's/meshSkinIndexByTypedId's position for that ufbx_mesh (D11: "ImportedNode
    // ::skinIndex on the node that carries the mesh"). Two nodes (or a node and its geometry-transform
    // helper) referencing one ufbx_mesh both point at the same mesh/skin index (AC-27).
    for (const ufbx_node* n : s.nodes) {
        if (n->is_root || n->mesh == nullptr) {
            continue;
        }
        const auto nodeIt = nodeIndexByLocalId.find(n->typed_id);
        if (nodeIt == nodeIndexByLocalId.end()) {
            continue;
        }
        if (const auto meshIt = meshIndexByTypedId.find(n->mesh->typed_id); meshIt != meshIndexByTypedId.end()) {
            result.model.nodes[nodeIt->second].meshIndex = meshIt->second;
        }
        if (const auto skinIt = meshSkinIndexByTypedId.find(n->mesh->typed_id);
            skinIt != meshSkinIndexByTypedId.end()) {
            result.model.nodes[nodeIt->second].skinIndex = skinIt->second;
        }
    }

    // CORRECTION found while implementing phase 8 (Step 9), fixed here alongside it: §A-4's own table
    // says "skins[].joints -- empty" for FBX at Structure depth -- naming the SUB-FIELD, not the whole
    // `skins` vector. `ufbx_skin_deformer` is a scene element (like a material), unaffected by
    // ignore_all_content, so its NAME/IDENTITY survives at Structure depth exactly like meshes' own
    // "identity survives, content does not" split -- ONE shell ImportedSkin per deformer, `joints` /
    // `inverseBindMatrices` / `skeletonRoot` left at their struct defaults (empty / empty /
    // INVALID_SUBASSET). The phase-6-retrofitted pass above already builds every ImportedSkin with
    // real content at Full depth; this is Structure's own, separate, scene-wide pass (skin deformers
    // are NOT per-mesh the way meshes themselves are, so this does not belong inside that loop).
    if (depth == ImportDepth::Structure && settings.importSkins) {
        for (const ufbx_skin_deformer* deformer : s.skin_deformers) {
            ImportedSkin shell;
            shell.name = toStd(deformer->name);
            shell.localId = deformer->typed_id;
            result.model.skins.push_back(std::move(shell));
        }
    }
    // OVERWRITES phase 2's cheap element count with the ACTUAL populated size -- materials'/meshes'
    // own established pattern (gltf_import.cpp's identical `summary.skinCount = result.model.skins.
    // size()` at the end of ITS OWN skins phase). This is what makes BOTH halves of the contract hold
    // simultaneously: real at Structure depth (the shell pass above populates one entry per deformer,
    // so `skins.size()` still equals the raw count there -- A13) AND zero when `settings.importSkins`
    // is false (FI63 -- neither the shell pass above nor the Full-depth pass inside the mesh loop runs
    // in that case, so `skins` stays empty at BOTH depths).
    result.model.summary.skinCount = result.model.skins.size();

    // ---- phase 8: animations -> ImportedAnimation (D12). Runs at BOTH depths -- ufbx_anim_stack is a
    // scene element, unaffected by `ignore_geometry`/`ignore_animation` -- but its CONTENT (baked
    // keyframes) is Full-only: at Structure depth every clip is a SHELL (name/localId real, duration
    // == 0, no channels, §A-4's own table), the identical split phase 7's skin shell above now also
    // follows. Skipped entirely when !settings.importAnimations (AC-47b), at BOTH depths, matching
    // materials'/skins' own posture (§A-4's "structural identity survives" rule is about
    // `ignore_geometry`/`ignore_animation`, not about a user's own import-setting choice).
    if (settings.importAnimations) {
        std::size_t keyTotal = 0;
        bool keyCapHit = false;
        for (const ufbx_anim_stack* stack : s.anim_stacks) {
            ImportedAnimation outAnim;
            outAnim.name = toStd(stack->name);
            outAnim.localId = stack->typed_id;

            if (depth == ImportDepth::Structure) {
                result.model.animations.push_back(std::move(outAnim));  // duration 0, no channels
                continue;
            }

            ufbx_bake_opts bake = {};  // VALUE-INITIALISED (_begin_zero/_end_zero, A15)
            // D12: every field that affects OUTPUT is pinned EXPLICITLY, even where it equals ufbx's
            // default, so a ufbx bump cannot silently move key counts or sample times.
            bake.trim_start_time = true;        // a clip authored on frames [30,60] starts at 0 -- what makes
                                                // ImportedAnimation::duration mean the same thing it means
                                                // for a glTF clip
            bake.resample_rate = 30.0;          // ufbx's default today; PINNED
            bake.minimum_sample_rate = 19.5;    // ufbx's default: do not re-resample already-resampled
                                                // curves
            bake.max_keyframe_segments = 32;    // ufbx's default; PINNED
            bake.key_reduction_enabled = true;  // linear runs collapse; keeps counts honest (R7)
            // key_reduction_threshold (1e-6) / key_reduction_passes (4) left at ufbx's defaults -- they
            // trade fidelity for size and there is no evidence yet for a different point on that curve.
            ufbx_error bakeErr = {};
            const BakedAnimPtr baked(ufbx_bake_anim(&s, stack->anim, &bake, &bakeErr));
            if (!baked) {
                // one clip failing never fails the whole import (D12)
                addWarning(result, std::format("animation '{}': {}", outAnim.name, ufbxMessage(bakeErr)));
                continue;
            }

            // E15: a stack with zero baked nodes imports as a clip with duration == 0 and no channels
            // -- not an error, simply nothing to append below.
            for (const ufbx_baked_node& bn : baked.get()->nodes) {
                if (!nodeIndexByLocalId.contains(bn.typed_id)) {
                    // E17: an unresolved typed_id drops the WHOLE baked node (all three of its
                    // channels), never an out-of-bounds read.
                    addWarning(result,
                               std::format("animation '{}': target node not found; channel dropped", outAnim.name));
                    continue;
                }
                // §D-4.10: targetNode is bn.typed_id itself (a localId, §A-13), NEVER the vector index
                // -- resolves correctly for an ordinary node AND for a geometry-transform helper node
                // that is itself animated (E18): a helper looks like it should be special; it is not,
                // and typed_id resolves it like any other.
                //
                // MEASURED, a completion the plan's own §D-4.10 left implicit: ufbx_bake_anim ALWAYS
                // returns all three key lists (translation/rotation/scale) for every baked node, even
                // for a component nobody animated -- key_reduction_enabled collapses an unanimated
                // component to exactly ONE key, and `constant_translation`/`constant_rotation`/
                // `constant_scale` are what SAYS SO (executed: baking a translation-only animation
                // still returns `rotation_keys.count == 1` and `scale_keys.count == 1`, both flagged
                // constant). Gating on these flags is what keeps `channels` free of degenerate
                // single-key "channels" for properties nobody actually animated.
                if (!bn.constant_translation && keyTotal < MAX_ANIMATION_KEYS_PER_MODEL) {
                    keyTotal += appendVec3Channel(result, outAnim, AnimationPath::Translation, bn.typed_id,
                                                  bn.translation_keys, MAX_ANIMATION_KEYS_PER_MODEL - keyTotal);
                }
                if (!bn.constant_rotation && keyTotal < MAX_ANIMATION_KEYS_PER_MODEL) {
                    keyTotal += appendQuatChannel(result, outAnim, AnimationPath::Rotation, bn.typed_id,
                                                  bn.rotation_keys, MAX_ANIMATION_KEYS_PER_MODEL - keyTotal);
                }
                if (!bn.constant_scale && keyTotal < MAX_ANIMATION_KEYS_PER_MODEL) {
                    keyTotal += appendVec3Channel(result, outAnim, AnimationPath::Scale, bn.typed_id, bn.scale_keys,
                                                  MAX_ANIMATION_KEYS_PER_MODEL - keyTotal);
                }
            }
            if (keyTotal >= MAX_ANIMATION_KEYS_PER_MODEL && !keyCapHit) {
                escalate(result, ImportStatus::Truncated, "animation key cap (MAX_ANIMATION_KEYS_PER_MODEL) reached");
                keyCapHit = true;
            }

            // duration = the LONGEST channel's own last (already-trimmed) time.
            for (const ImportedAnimationChannel& channel : outAnim.channels) {
                if (!channel.times.empty()) {
                    outAnim.duration = std::max(outAnim.duration, channel.times.back());
                }
            }
            result.model.summary.animationDuration = std::max(result.model.summary.animationDuration, outAnim.duration);
            result.model.animations.push_back(std::move(outAnim));
        }
    }
    // Same overwrite discipline as skinCount above (and materials'/meshes' own established pattern):
    // real at Structure depth (the shell push above populates one entry per stack, so `animations.
    // size()` still equals the raw stack count there -- A13) AND zero when `settings.importAnimations`
    // is false (FI71 -- `animations` never gets a single push in that case, at either depth).
    result.model.summary.animationCount = result.model.animations.size();

    // E5's second half: summary.triangleCount is phase 2's cheap ufbx-native count
    // (mesh->num_triangles), deliberately NOT re-derived above. Gated on TRIANGLES, not merely
    // vertices (FI36's own finding, corrected from an earlier draft of this check): a mesh can
    // legitimately have real vertices but ZERO triangles (every face a point or a line, E9's own
    // fixture) and produce no primitive at all -- that is not corruption, and escalating on it would
    // make FI36's degenerate-mesh case, which has vertices but no triangulatable content, misreport
    // Malformed. A real triangle count with an invalid fold, on the other hand, means the file's
    // declared unit corrupted the geometry: Aabb::expand() silently IGNORES a non-finite point
    // (std::min/std::max with a NaN operand return the unchanged current value), so a fully-corrupted
    // mesh's bounds stays stuck at the Aabb::empty() sentinel rather than becoming a visibly-broken box.
    if (result.model.summary.triangleCount > 0 && !result.model.summary.bounds.valid()) {
        escalate(result, ImportStatus::Malformed, "the file's declared unit produced non-finite geometry");
    }

    return result;
}

}  // namespace engine::editor

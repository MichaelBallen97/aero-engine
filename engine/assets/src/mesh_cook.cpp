// Aero Engine — the mesh cook (task 3.3.1): classify -> sort -> caps -> layouts -> width -> offsets
// -> emit. See mesh_cook.hpp for the contract and docs/09-file-formats.md section 9 for the format.
// NEVER THROWS. NEVER READS A FILE. NEVER LOGS.
//
// NO std::map, std::unordered_map, std::set OR std::unordered_set ANYWHERE IN THIS FILE (A-5). There
// must be no iteration order for the output to depend on, and MSVC's node-based containers are not
// nothrow-movable (3.1.2's R9, measured in CI as C2607). Grouping is a sorted vector.
#include <aero/assets/mesh_cook.hpp>
#include <aero/core/profiler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::assets {
namespace {

// The same field offsets cooked_mesh.cpp reads, restated here because the two TUs are the writer and
// the reader of one table and neither owns the other. docs/09 section 9 is the normative source.
constexpr std::size_t H_FORMAT_VERSION = 8;
constexpr std::size_t H_COOKER_VERSION = 12;
constexpr std::size_t H_GUID_HI = 16;
constexpr std::size_t H_GUID_LO = 24;
constexpr std::size_t H_SECTION_COUNT = 36;
constexpr std::size_t H_SUBMESH_COUNT = 40;
constexpr std::size_t H_INDEX_COUNT = 44;
constexpr std::size_t H_INDEX_TYPE = 48;
constexpr std::size_t H_ATTRIBUTE_COUNT = 52;
constexpr std::size_t H_TOTAL_BYTES = 56;
constexpr std::size_t H_BOUNDS_MIN = 64;
constexpr std::size_t H_BOUNDS_MAX = 76;
constexpr std::size_t H_INDEX_DATA_OFFSET = 88;

constexpr std::size_t S_FIRST_ATTRIBUTE = 0;
constexpr std::size_t S_ATTRIBUTE_COUNT = 4;
constexpr std::size_t S_VERTEX_STRIDE = 8;
constexpr std::size_t S_VERTEX_COUNT = 12;
constexpr std::size_t S_VERTEX_DATA_OFFSET = 16;
constexpr std::size_t S_VERTEX_DATA_BYTES = 24;

constexpr std::size_t M_SECTION_INDEX = 0;
constexpr std::size_t M_FIRST_INDEX = 4;
constexpr std::size_t M_INDEX_COUNT = 8;
constexpr std::size_t M_MATERIAL = 12;
constexpr std::size_t M_SOURCE_MESH = 16;
constexpr std::size_t M_SOURCE_PRIMITIVE = 20;
constexpr std::size_t M_BOUNDS_MIN = 32;
constexpr std::size_t M_BOUNDS_MAX = 44;

constexpr std::size_t A_SEMANTIC = 0;
constexpr std::size_t A_FORMAT = 2;
constexpr std::size_t A_OFFSET = 4;

// The value Aabb::empty() uses -- the INVERTED sentinel, folded away by the first point.
constexpr float INF = std::numeric_limits<float>::infinity();

[[nodiscard]] constexpr std::uint32_t bitOf(CookedVertexSemantic s) noexcept {
    return 1U << static_cast<std::uint32_t>(s);
}

[[nodiscard]] constexpr std::uint64_t alignUp(std::uint64_t v, std::uint64_t a) noexcept {
    return (v + (a - 1)) & ~(a - 1);
}

// One per accepted primitive: built in phase 1, sorted in phase 2, assigned a section in phase 4.
//
// vertexCount and indexCount are u64 DELIBERATELY, and this is a correction to the plan's own §D-6:
// it gave phase 1 a defensive drop for `positions.size() > MAX_COOKED_VERTICES` so that phase 3's
// accumulators could not wrap. That drop would make MC42/MC45 -- the vertex cap's own proofs --
// unreachable, because a primitive dropped in phase 1 never reaches the cap loop and the status would
// be Ok rather than Truncated. Widening the two counters closes the same overflow exactly, with no
// behavioural arm at all: every ACCEPTED descriptor is bounded by the caps and therefore fits u32.
struct Descriptor {
    const MeshCookPrimitive* prim = nullptr;  // never null; points into the caller's span
    std::uint32_t mask = 0;                   // the SURVIVING attribute set, bit n == semantic n
    std::uint64_t vertexCount = 0;            // == prim->positions.size()
    std::uint64_t indexCount = 0;
    std::uint32_t sectionIndex = 0;       // filled in phase 4
    std::uint32_t sectionVertexBase = 0;  // filled in phase 4; what indices are rebased by
    std::uint32_t firstIndex = 0;         // filled in phase 4
};

// The warning list is CAPPED and the total is not (the MAX_REPORTED_PER_CATEGORY shape). Every
// warning in this file goes through here so the cap cannot be forgotten at one site.
void addWarning(MeshCookResult& out, std::string text) {
    ++out.warningTotal;
    if (out.warnings.size() < MAX_COOK_WARNINGS) {
        out.warnings.push_back(std::move(text));
    }
}

// std::min/std::max with the ACCUMULATOR FIRST, matching editor/src/scene_bounds.cpp's Aabb::expand
// bit for bit. The argument order is NOT cosmetic: std::min(a, b) returns `b < a ? b : a`, so with
// -0.0f and +0.0f the two orders return different zeros -- a difference invisible in every printed
// value and fatal to the byte-for-byte comparison AC-27 rests on.
void expandBox(CookedBounds& box, Vec3 p) noexcept {
    box.min = Vec3{std::min(box.min.x, p.x), std::min(box.min.y, p.y), std::min(box.min.z, p.z)};
    box.max = Vec3{std::max(box.max.x, p.x), std::max(box.max.y, p.y), std::max(box.max.z, p.z)};
}

void putVec2(std::span<std::byte> out, std::size_t off, Vec2 v) noexcept {
    putF32(out, off + 0, v.x);
    putF32(out, off + 4, v.y);
}
void putVec3(std::span<std::byte> out, std::size_t off, Vec3 v) noexcept {
    putF32(out, off + 0, v.x);
    putF32(out, off + 4, v.y);
    putF32(out, off + 8, v.z);
}
void putVec4(std::span<std::byte> out, std::size_t off, Vec4 v) noexcept {
    putF32(out, off + 0, v.x);
    putF32(out, off + 4, v.y);
    putF32(out, off + 8, v.z);
    putF32(out, off + 12, v.w);
}

// The names the demote and pairing warnings use. A switch with no `default:`, like every other
// total switch in this subsystem.
[[nodiscard]] std::string_view semanticName(CookedVertexSemantic s) noexcept {
    switch (s) {
        case CookedVertexSemantic::Position:
            return "positions";
        case CookedVertexSemantic::Normal:
            return "normals";
        case CookedVertexSemantic::Tangent:
            return "tangents";
        case CookedVertexSemantic::TexCoord0:
            return "uv0";
        case CookedVertexSemantic::TexCoord1:
            return "uv1";
        case CookedVertexSemantic::Color0:
            return "colors";
        case CookedVertexSemantic::Joints0:
            return "joints";
        case CookedVertexSemantic::Weights0:
            return "weights";
    }
    return "unknown";
}

// ---- phase 1: classify ---------------------------------------------------------------------
// One pass per primitive, every decision LOCAL, so input order cannot matter here.
//
// THE COOK DROPS WHOLE PRIMITIVES AND DEMOTES WHOLE ATTRIBUTES, never anything partial (A-9).
// Returns false when the primitive is dropped whole; `d` is filled only when it returns true.
[[nodiscard]] bool classify(const MeshCookPrimitive& p, MeshCookResult& out, Descriptor& d) {
    const std::size_t n = p.positions.size();
    if (n == 0) {
        addWarning(out, std::format("mesh {} primitive {} has no positions and was dropped", p.sourceMeshIndex,
                                    p.sourcePrimitiveIndex));
        return false;
    }
    if (p.indices.empty()) {
        addWarning(out, std::format("mesh {} primitive {} has no indices and was dropped", p.sourceMeshIndex,
                                    p.sourcePrimitiveIndex));
        return false;
    }
    if (p.indices.size() % 3 != 0) {
        addWarning(out, std::format("mesh {} primitive {} has {} indices, not a multiple of 3, and was dropped",
                                    p.sourceMeshIndex, p.sourcePrimitiveIndex, p.indices.size()));
        return false;
    }
    // The index range is compared against positions.size(), NOT against any optional array: an
    // optional array shorter than positions is DEMOTED below, never a reason to refuse an index.
    for (std::size_t i = 0; i < p.indices.size(); ++i) {
        if (p.indices[i] >= n) {
            addWarning(out, std::format("mesh {} primitive {} index {} addresses vertex {} of {}, and was dropped",
                                        p.sourceMeshIndex, p.sourcePrimitiveIndex, i, p.indices[i], n));
            return false;
        }
    }
    // std::isfinite on each component, stopping at the first failure. Not a NaN != NaN trick: that
    // misses the +/-inf half, which is exactly the half that makes Aabb::valid() false.
    for (std::size_t v = 0; v < n; ++v) {
        const Vec3& q = p.positions[v];
        if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) {
            addWarning(out, std::format("mesh {} primitive {} has a non-finite position at vertex {} and was dropped",
                                        p.sourceMeshIndex, p.sourcePrimitiveIndex, v));
            return false;
        }
    }

    // An optional array is present only if it is non-empty AND exactly as long as `positions`
    // (EMPTY == ABSENT, the ImportedPrimitive rule verbatim). A length MISMATCH is demoted with one
    // warning and the primitive still cooks -- dropping it would lose geometry over a shading array.
    struct OptionalArray {
        CookedVertexSemantic semantic;
        std::size_t size;
    };
    const std::array<OptionalArray, 7> optionals = {
        OptionalArray{CookedVertexSemantic::Normal, p.normals.size()},
        OptionalArray{CookedVertexSemantic::Tangent, p.tangents.size()},
        OptionalArray{CookedVertexSemantic::TexCoord0, p.uv0.size()},
        OptionalArray{CookedVertexSemantic::TexCoord1, p.uv1.size()},
        OptionalArray{CookedVertexSemantic::Color0, p.colors.size()},
        OptionalArray{CookedVertexSemantic::Joints0, p.joints.size()},
        OptionalArray{CookedVertexSemantic::Weights0, p.weights.size()},
    };
    std::uint32_t mask = bitOf(CookedVertexSemantic::Position);
    for (const OptionalArray& oa : optionals) {
        if (oa.size == 0) {
            continue;  // absent, and that is not a defect
        }
        if (oa.size != n) {
            addWarning(out,
                       std::format("mesh {} primitive {}'s {} array has {} entries for {} vertices and was "
                                   "ignored",
                                   p.sourceMeshIndex, p.sourcePrimitiveIndex, semanticName(oa.semantic), oa.size, n));
            continue;
        }
        mask |= bitOf(oa.semantic);
    }

    // AC-20: joints and weights are cooked TOGETHER or not at all. Half a skin is worse than none --
    // a consumer cannot use joint indices without their weights, and a vertex layout carrying one is
    // a trap for the pipeline that reads it.
    const bool haveJoints = (mask & bitOf(CookedVertexSemantic::Joints0)) != 0;
    const bool haveWeights = (mask & bitOf(CookedVertexSemantic::Weights0)) != 0;
    if (haveJoints != haveWeights) {
        addWarning(out, std::format("mesh {} primitive {} has {} without {}; both were dropped", p.sourceMeshIndex,
                                    p.sourcePrimitiveIndex,
                                    haveJoints ? semanticName(CookedVertexSemantic::Joints0)
                                               : semanticName(CookedVertexSemantic::Weights0),
                                    haveJoints ? semanticName(CookedVertexSemantic::Weights0)
                                               : semanticName(CookedVertexSemantic::Joints0)));
        mask &= ~bitOf(CookedVertexSemantic::Joints0);
        mask &= ~bitOf(CookedVertexSemantic::Weights0);
    }

    d.prim = &p;
    d.mask = mask;
    d.vertexCount = n;
    d.indexCount = p.indices.size();
    return true;
}

// ---- phase 7: emit one vertex's attributes ---------------------------------------------------
void writeVertex(std::span<std::byte> out, std::size_t base, const std::vector<CookedVertexAttribute>& attributes,
                 std::uint32_t firstAttribute, std::uint32_t attributeCount, const MeshCookPrimitive& p,
                 std::size_t v) noexcept {
    for (std::uint32_t a = 0; a < attributeCount; ++a) {
        const CookedVertexAttribute& attr = attributes[firstAttribute + a];
        const std::size_t off = base + attr.offset;
        switch (attr.semantic) {
            case CookedVertexSemantic::Position:
                putVec3(out, off, p.positions[v]);
                break;
            case CookedVertexSemantic::Normal:
                putVec3(out, off, p.normals[v]);
                break;
            case CookedVertexSemantic::Tangent:
                putVec4(out, off, p.tangents[v]);
                break;
            case CookedVertexSemantic::TexCoord0:
                putVec2(out, off, p.uv0[v]);
                break;
            case CookedVertexSemantic::TexCoord1:
                putVec2(out, off, p.uv1[v]);
                break;
            case CookedVertexSemantic::Color0:
                putVec4(out, off, p.colors[v]);
                break;
            case CookedVertexSemantic::Joints0:
                // u16 -> u32, LOSSLESS (D12). rhi::VertexFormat has no unsigned-16x4 enumerator, and
                // UByte4Norm is normalized and therefore wrong for an index.
                for (std::size_t k = 0; k < 4; ++k) {
                    putU32(out, off + (k * 4), static_cast<std::uint32_t>(p.joints[v][k]));
                }
                break;
            case CookedVertexSemantic::Weights0:
                putVec4(out, off, p.weights[v]);
                break;
        }
    }
}

MeshCookResult cookMeshImpl(const MeshCookInput& input) {
    MeshCookResult out;

    // ---- phase 1: classify ----------------------------------------------------------------
    std::vector<Descriptor> descriptors;
    descriptors.reserve(input.primitives.size());
    for (const MeshCookPrimitive& p : input.primitives) {
        Descriptor d;
        if (!classify(p, out, d)) {
            ++out.stats.droppedPrimitiveCount;
            continue;
        }
        descriptors.push_back(d);
    }

    // ---- phase 2: sort. A sorted VECTOR, never a map, and it runs BEFORE the caps (C2). -----
    std::stable_sort(descriptors.begin(), descriptors.end(), [](const Descriptor& a, const Descriptor& b) {
        if (a.mask != b.mask) {
            return a.mask < b.mask;
        }
        if (a.prim->sourceMeshIndex != b.prim->sourceMeshIndex) {
            return a.prim->sourceMeshIndex < b.prim->sourceMeshIndex;
        }
        return a.prim->sourcePrimitiveIndex < b.prim->sourcePrimitiveIndex;
    });

    // The ONE non-total case in the ordering key, DIAGNOSED rather than assumed away (C3). One
    // linear pass over adjacent pairs of an already-sorted vector. Nothing is dropped: dropping
    // would lose geometry over a caller's bookkeeping, and the status stays Ok.
    std::size_t collisions = 0;
    std::size_t firstCollision = 0;
    for (std::size_t i = 1; i < descriptors.size(); ++i) {
        const Descriptor& a = descriptors[i - 1];
        const Descriptor& b = descriptors[i];
        if (a.mask == b.mask && a.prim->sourceMeshIndex == b.prim->sourceMeshIndex &&
            a.prim->sourcePrimitiveIndex == b.prim->sourcePrimitiveIndex) {
            if (collisions == 0) {
                firstCollision = i;
            }
            ++collisions;
        }
    }
    if (collisions > 0) {
        const Descriptor& d = descriptors[firstCollision];
        addWarning(out, std::format("mesh {} primitive {} shares its ordering key with another primitive "
                                    "({} collisions); byte-identity across a reordering is not guaranteed "
                                    "for this input",
                                    d.prim->sourceMeshIndex, d.prim->sourcePrimitiveIndex, collisions));
    }

    // ---- phase 3: cap-bounded acceptance, over the SORTED order (C2) ------------------------
    // The accepted set is a function of the SORTED order, and the sorted order is a function of the
    // SET, so which primitives survive a cap does not depend on the order the caller handed them in.
    // Running this before the sort -- as the spec had it -- makes a shuffled input produce a
    // different file, and only on inputs that trip a cap, which is exactly the combination a green
    // suite would not otherwise exercise.
    //
    // EVERY violated cap latches its OWN bool; acceptance stops at the first candidate that violates
    // ANY of them, leaving a coherent prefix rather than a hole. Two caps therefore produce two
    // messages only when the SAME candidate violates both (A-6).
    std::uint64_t vertexBudget = 0;
    std::uint64_t indexBudget = 0;
    std::uint64_t submeshBudget = 0;
    bool capVertices = false;
    bool capIndices = false;
    bool capSubmeshes = false;
    std::size_t accepted = 0;
    for (const Descriptor& d : descriptors) {
        // u64 accumulators, so the + cannot wrap before the comparison.
        const bool overVertices = vertexBudget + d.vertexCount > MAX_COOKED_VERTICES;
        const bool overIndices = indexBudget + d.indexCount > MAX_COOKED_INDICES;
        const bool overSubmeshes = submeshBudget + 1 > MAX_COOKED_SUBMESHES;
        if (overVertices || overIndices || overSubmeshes) {
            capVertices = capVertices || overVertices;
            capIndices = capIndices || overIndices;
            capSubmeshes = capSubmeshes || overSubmeshes;
            break;
        }
        vertexBudget += d.vertexCount;
        indexBudget += d.indexCount;
        ++submeshBudget;
        ++accepted;
    }
    descriptors.resize(accepted);

    // Assembled in a FIXED order (vertices, indices, submeshes) and joined with "; ", so the same
    // input always produces the same string. Each message names its own constant and its own
    // quantity; two cap sites sharing a constant would get different wording, and there are none in
    // v1, but the rule is stated so 3.3.2 inherits it.
    if (capVertices) {
        out.message = std::format("the model exceeds MAX_COOKED_VERTICES ({}); the cooked file holds {} vertices",
                                  MAX_COOKED_VERTICES, vertexBudget);
    }
    if (capIndices) {
        if (!out.message.empty()) {
            out.message += "; ";
        }
        out.message += std::format("the model exceeds MAX_COOKED_INDICES ({}); the cooked file holds {} indices",
                                   MAX_COOKED_INDICES, indexBudget);
    }
    if (capSubmeshes) {
        if (!out.message.empty()) {
            out.message += "; ";
        }
        out.message += std::format("the model exceeds MAX_COOKED_SUBMESHES ({}); the cooked file holds {} submeshes",
                                   MAX_COOKED_SUBMESHES, submeshBudget);
    }
    if (capVertices || capIndices || capSubmeshes) {
        out.status = MeshCookStatus::Truncated;
    }

    // ---- phase 4: sections and layouts ------------------------------------------------------
    // Equal masks are ADJACENT after the sort, so one linear pass builds every section.
    std::vector<CookedVertexAttribute> attributes;
    std::vector<CookedSection> sections;
    std::uint64_t indexTotal = 0;
    for (std::size_t i = 0; i < descriptors.size();) {
        const std::uint32_t mask = descriptors[i].mask;
        CookedSection section;
        section.firstAttribute = static_cast<std::uint32_t>(attributes.size());
        std::uint32_t offset = 0;
        // ASCENDING SEMANTIC CODE -- this IS the layout rule, and the offsets accumulate in exactly
        // that order. No padding between attributes: every v1 format's size is a multiple of 4, so
        // the stride is too.
        for (std::uint32_t bit = 0; bit < COOKED_SEMANTIC_COUNT; ++bit) {
            const auto semantic = static_cast<CookedVertexSemantic>(bit);
            if ((mask & bitOf(semantic)) == 0) {
                continue;
            }
            const CookedVertexFormat format = cookedFormatForSemantic(semantic);
            attributes.push_back(CookedVertexAttribute{semantic, format, offset});
            offset += cookedVertexFormatBytes(format);
        }
        section.attributeCount = static_cast<std::uint32_t>(attributes.size()) - section.firstAttribute;
        section.vertexStride = offset;

        const auto sectionIndex = static_cast<std::uint32_t>(sections.size());
        while (i < descriptors.size() && descriptors[i].mask == mask) {
            descriptors[i].sectionIndex = sectionIndex;
            descriptors[i].sectionVertexBase = section.vertexCount;
            descriptors[i].firstIndex = static_cast<std::uint32_t>(indexTotal);
            section.vertexCount += static_cast<std::uint32_t>(descriptors[i].vertexCount);
            indexTotal += descriptors[i].indexCount;
            ++i;
        }
        sections.push_back(section);
    }

    // THE TWO UNREACHABLE CAPS, kept as defence in depth and SAID SO rather than faked with
    // synthetic input. Position is mandatory, so an attribute mask has at most 2^7 = 128 values --
    // and AC-20's joints/weights pairing rule makes half of those unreachable too, because a mask
    // carrying exactly one of the pair is cleared to neither. Only 2^5 x 2 = 64 masks can ever be
    // produced, so the cook can reach at most 64 sections against a cap of 128 and at most 288
    // attributes against a cap of 1024. NO CASE DRIVES EITHER ARM, and none can be written: the
    // parser, where a header claiming 129 sections is refused, is where these constants are actually
    // proven.
    if (sections.size() > MAX_COOKED_SECTIONS || attributes.size() > MAX_COOKED_ATTRIBUTES) {
        out.status = MeshCookStatus::Truncated;
        out.message = std::format("the cooked layout needs {} sections and {} attributes, over the caps of {} and {}",
                                  sections.size(), attributes.size(), MAX_COOKED_SECTIONS, MAX_COOKED_ATTRIBUTES);
        out.bytes.clear();
        return out;
    }

    // ---- phase 5: the file-level index width ------------------------------------------------
    // `<= 65536`, not `< 65536`: a section with exactly 65536 vertices has a maximum index of 65535,
    // which Uint16 represents. Indices are SECTION-RELATIVE, so the bound that matters is the
    // largest section, not the file total.
    const bool allSmall =
        std::all_of(sections.begin(), sections.end(), [](const CookedSection& s) { return s.vertexCount <= 65536; });
    const CookedIndexType indexType = allSmall ? CookedIndexType::Uint16 : CookedIndexType::Uint32;

    // ---- phase 6: offsets, one arithmetic pass, all in u64 ----------------------------------
    std::uint64_t cursor = COOKED_MESH_HEADER_BYTES;
    cursor += static_cast<std::uint64_t>(COOKED_MESH_ATTRIBUTE_BYTES) * attributes.size();
    cursor += static_cast<std::uint64_t>(COOKED_MESH_SECTION_BYTES) * sections.size();
    cursor += static_cast<std::uint64_t>(COOKED_MESH_SUBMESH_BYTES) * descriptors.size();
    const std::size_t attrBase = COOKED_MESH_HEADER_BYTES;
    const std::size_t sectionBase = attrBase + (COOKED_MESH_ATTRIBUTE_BYTES * attributes.size());
    const std::size_t submeshBase = sectionBase + (COOKED_MESH_SECTION_BYTES * sections.size());
    for (CookedSection& s : sections) {
        cursor = alignUp(cursor, COOKED_MESH_ALIGNMENT);
        s.vertexDataOffset = cursor;
        s.vertexDataBytes = static_cast<std::uint64_t>(s.vertexCount) * s.vertexStride;
        cursor += s.vertexDataBytes;
    }
    cursor = alignUp(cursor, COOKED_MESH_ALIGNMENT);
    const std::uint64_t indexDataOffset = cursor;
    cursor += indexTotal * cookedIndexTypeBytes(indexType);
    const std::uint64_t totalBytes = alignUp(cursor, COOKED_MESH_ALIGNMENT);

    // A THIRD UNREACHABLE ARM, for the same reason and stated the same way: with the caps of phase 3
    // enforced, the largest legal file is 8000000*104 + 24000000*4 ~= 928 MB, comfortably under the
    // 2 GB parser early-out. Refusing here rather than allocating is what keeps a future cap change
    // from producing a file the parser would reject.
    if (totalBytes > MAX_COOKED_MESH_BYTES) {
        out.status = MeshCookStatus::Truncated;
        out.message =
            std::format("the cooked file would be {} bytes, over the {}-byte cap", totalBytes, MAX_COOKED_MESH_BYTES);
        out.bytes.clear();
        return out;
    }

    // ---- phase 7: emit ----------------------------------------------------------------------
    // ZERO-INITIALIZED: this is what makes every pad byte and every gap zero without a second pass.
    std::vector<std::byte> bytes(static_cast<std::size_t>(totalBytes));
    const std::span<std::byte> o(bytes);

    for (std::size_t i = 0; i < COOKED_MESH_MAGIC.size(); ++i) {
        bytes[i] = static_cast<std::byte>(COOKED_MESH_MAGIC[i]);
    }
    putU32(o, H_FORMAT_VERSION, COOKED_MESH_FORMAT_VERSION);
    putU32(o, H_COOKER_VERSION, COOKED_MESH_COOKER_VERSION);
    putU64(o, H_GUID_HI, input.sourceGuid.hi);
    putU64(o, H_GUID_LO, input.sourceGuid.lo);
    putU32(o, H_SECTION_COUNT, static_cast<std::uint32_t>(sections.size()));
    putU32(o, H_SUBMESH_COUNT, static_cast<std::uint32_t>(descriptors.size()));
    putU32(o, H_INDEX_COUNT, static_cast<std::uint32_t>(indexTotal));
    putU32(o, H_INDEX_TYPE, static_cast<std::uint32_t>(indexType));
    putU32(o, H_ATTRIBUTE_COUNT, static_cast<std::uint32_t>(attributes.size()));
    putU64(o, H_TOTAL_BYTES, totalBytes);
    putU64(o, H_INDEX_DATA_OFFSET, indexDataOffset);

    for (std::size_t a = 0; a < attributes.size(); ++a) {
        const std::size_t base = attrBase + (a * COOKED_MESH_ATTRIBUTE_BYTES);
        putU16(o, base + A_SEMANTIC, static_cast<std::uint16_t>(attributes[a].semantic));
        putU16(o, base + A_FORMAT, static_cast<std::uint16_t>(attributes[a].format));
        putU32(o, base + A_OFFSET, attributes[a].offset);
    }
    for (std::size_t s = 0; s < sections.size(); ++s) {
        const std::size_t base = sectionBase + (s * COOKED_MESH_SECTION_BYTES);
        putU32(o, base + S_FIRST_ATTRIBUTE, sections[s].firstAttribute);
        putU32(o, base + S_ATTRIBUTE_COUNT, sections[s].attributeCount);
        putU32(o, base + S_VERTEX_STRIDE, sections[s].vertexStride);
        putU32(o, base + S_VERTEX_COUNT, sections[s].vertexCount);
        putU64(o, base + S_VERTEX_DATA_OFFSET, sections[s].vertexDataOffset);
        putU64(o, base + S_VERTEX_DATA_BYTES, sections[s].vertexDataBytes);
    }
    // The submesh table is written BEFORE the vertex regions, so each box is back-patched as its
    // vertices are written. Its entry offset is known and the record is fixed-size; the alternative
    // -- a second full pass over the vertex data -- was rejected because back-patching touches each
    // vertex once.
    for (std::size_t m = 0; m < descriptors.size(); ++m) {
        const std::size_t base = submeshBase + (m * COOKED_MESH_SUBMESH_BYTES);
        putU32(o, base + M_SECTION_INDEX, descriptors[m].sectionIndex);
        putU32(o, base + M_FIRST_INDEX, descriptors[m].firstIndex);
        putU32(o, base + M_INDEX_COUNT, static_cast<std::uint32_t>(descriptors[m].indexCount));
        putU32(o, base + M_MATERIAL, descriptors[m].prim->materialIndex);
        putU32(o, base + M_SOURCE_MESH, descriptors[m].prim->sourceMeshIndex);
        putU32(o, base + M_SOURCE_PRIMITIVE, descriptors[m].prim->sourcePrimitiveIndex);
    }

    CookedBounds modelBox{Vec3{INF, INF, INF}, Vec3{-INF, -INF, -INF}};
    std::uint64_t vertexTotal = 0;
    for (std::size_t m = 0; m < descriptors.size(); ++m) {
        const Descriptor& d = descriptors[m];
        const CookedSection& section = sections[d.sectionIndex];
        CookedBounds box{Vec3{INF, INF, INF}, Vec3{-INF, -INF, -INF}};
        for (std::uint64_t v = 0; v < d.vertexCount; ++v) {
            const std::size_t vertexBase = static_cast<std::size_t>(section.vertexDataOffset) +
                                           ((static_cast<std::size_t>(d.sectionVertexBase) + v) * section.vertexStride);
            writeVertex(o, vertexBase, attributes, section.firstAttribute, section.attributeCount, *d.prim, v);
            // The fold runs over EVERY position WRITTEN, not over the subset an index reaches -- the
            // importer folds ImportedPrimitive::bounds the same way, and AC-27 compares the two byte
            // for byte.
            expandBox(box, d.prim->positions[v]);
        }
        vertexTotal += d.vertexCount;
        const std::size_t base = submeshBase + (m * COOKED_MESH_SUBMESH_BYTES);
        putVec3(o, base + M_BOUNDS_MIN, box.min);
        putVec3(o, base + M_BOUNDS_MAX, box.max);
        // The model box is the union of the EMITTED submeshes' boxes, folded exactly as
        // Aabb::expand(const Aabb&) folds -- min then max, accumulator first.
        //
        // THE FOLD ORDER IS EMISSION ORDER, i.e. the sorted (mask, sourceMeshIndex,
        // sourcePrimitiveIndex) order, and THAT CANNOT CHANGE: the model box is written into the
        // header, so folding in the caller's input order would make a shuffled input produce
        // different header bytes -- the exact thing AC-29 forbids. The importer folds its own model
        // box in SOURCE order instead, so the two can disagree in one way and one way only: the SIGN
        // OF A ZERO, because std::min/std::max are order-independent for every float pair except
        // (+0.0f, -0.0f). No box is wrong either way -- a +-0 bound is the same box -- and docs/09
        // section 9.10 states the caveat as part of the format's determinism contract.
        expandBox(modelBox, box.min);
        expandBox(modelBox, box.max);
    }

    std::uint64_t indexCursor = indexDataOffset;
    const std::uint32_t indexWidth = cookedIndexTypeBytes(indexType);
    for (const Descriptor& d : descriptors) {
        for (const std::uint32_t k : d.prim->indices) {
            const std::uint32_t value = k + d.sectionVertexBase;  // SECTION-RELATIVE (AC-13)
            if (indexType == CookedIndexType::Uint16) {
                putU16(o, static_cast<std::size_t>(indexCursor), static_cast<std::uint16_t>(value));
            } else {
                putU32(o, static_cast<std::size_t>(indexCursor), value);
            }
            indexCursor += indexWidth;
        }
    }

    // A file with no submeshes stores a POINT BOX AT THE ORIGIN, never Aabb::empty()'s inverted
    // sentinel, whose centre is NaN. The 3.2.3 empty-mesh precedent.
    if (descriptors.empty()) {
        modelBox = CookedBounds{Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, 0.0F}};
        addWarning(out, "the model has no cookable primitives; the cooked file is a valid empty container");
    }
    putVec3(o, H_BOUNDS_MIN, modelBox.min);
    putVec3(o, H_BOUNDS_MAX, modelBox.max);

    out.stats.sectionCount = static_cast<std::uint32_t>(sections.size());
    out.stats.submeshCount = static_cast<std::uint32_t>(descriptors.size());
    out.stats.vertexCount = static_cast<std::uint32_t>(vertexTotal);
    out.stats.indexCount = static_cast<std::uint32_t>(indexTotal);
    out.stats.byteSize = totalBytes;
    out.bytes = std::move(bytes);
    return out;
}

}  // namespace

MeshCookResult cookMesh(const MeshCookInput& input) {
    AERO_PROFILE_ZONE_NAMED("assets::cookMesh");
    try {
        return cookMeshImpl(input);
    } catch (...) {
        // C6: the worst case is ~928 MB in one vector and the API promises never to throw. INV-C3 is
        // restated for this one arm: `bytes` is EMPTY only here, and the status says so.
        MeshCookResult out;
        out.status = MeshCookStatus::Truncated;
        out.message = "the cooked output could not be allocated";
        return out;
    }
}

}  // namespace engine::assets

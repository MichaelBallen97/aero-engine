// Aero Engine — the cooked mesh container v1: labels, accessors and the hostile-input parser
// (task 3.3.1). See cooked_mesh.hpp for the contract and docs/09-file-formats.md section 9 for the
// normative format. NEVER THROWS. NEVER READS A FILE. NEVER LOGS. Allocates nothing before the count
// it is allocating for has been validated against a frozen cap.
#include <aero/assets/cooked_mesh.hpp>
#include <aero/core/profiler.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace engine::assets {
namespace {

// The header's field offsets, named ONCE. docs/09 section 9.2 is the normative table; these mirror it
// and nothing else in this TU spells a header offset as a literal.
constexpr std::size_t H_MAGIC = 0;
constexpr std::size_t H_FORMAT_VERSION = 8;
constexpr std::size_t H_COOKER_VERSION = 12;
constexpr std::size_t H_GUID_HI = 16;
constexpr std::size_t H_GUID_LO = 24;
constexpr std::size_t H_FLAGS = 32;
constexpr std::size_t H_SECTION_COUNT = 36;
constexpr std::size_t H_SUBMESH_COUNT = 40;
constexpr std::size_t H_INDEX_COUNT = 44;
constexpr std::size_t H_INDEX_TYPE = 48;
constexpr std::size_t H_ATTRIBUTE_COUNT = 52;
constexpr std::size_t H_TOTAL_BYTES = 56;
constexpr std::size_t H_BOUNDS_MIN = 64;
constexpr std::size_t H_BOUNDS_MAX = 76;
constexpr std::size_t H_INDEX_DATA_OFFSET = 88;
static_assert(H_INDEX_DATA_OFFSET + 8 == COOKED_MESH_HEADER_BYTES);

// Section-record field offsets.
constexpr std::size_t S_FIRST_ATTRIBUTE = 0;
constexpr std::size_t S_ATTRIBUTE_COUNT = 4;
constexpr std::size_t S_VERTEX_STRIDE = 8;
constexpr std::size_t S_VERTEX_COUNT = 12;
constexpr std::size_t S_VERTEX_DATA_OFFSET = 16;
constexpr std::size_t S_VERTEX_DATA_BYTES = 24;
static_assert(S_VERTEX_DATA_BYTES + 8 == COOKED_MESH_SECTION_BYTES);

// Submesh-record field offsets.
constexpr std::size_t M_SECTION_INDEX = 0;
constexpr std::size_t M_FIRST_INDEX = 4;
constexpr std::size_t M_INDEX_COUNT = 8;
constexpr std::size_t M_MATERIAL = 12;
constexpr std::size_t M_SOURCE_MESH = 16;
constexpr std::size_t M_SOURCE_PRIMITIVE = 20;
constexpr std::size_t M_RESERVED0 = 24;
constexpr std::size_t M_BOUNDS_MIN = 32;
constexpr std::size_t M_BOUNDS_MAX = 44;
constexpr std::size_t M_RESERVED1 = 56;
static_assert(M_RESERVED1 + 8 == COOKED_MESH_SUBMESH_BYTES);

// Attribute-record field offsets.
constexpr std::size_t A_SEMANTIC = 0;
constexpr std::size_t A_FORMAT = 2;
constexpr std::size_t A_OFFSET = 4;
static_assert(A_OFFSET + 4 == COOKED_MESH_ATTRIBUTE_BYTES);

// EVERY range check in this file goes through this, and it is written as SUBTRACTION so no arithmetic
// can wrap (AC-25). A crafted header with an offset near UINT64_MAX is refused, not accepted.
[[nodiscard]] constexpr bool fits(std::uint64_t offset, std::uint64_t length, std::uint64_t size) noexcept {
    return length <= size && offset <= size - length;
}

[[nodiscard]] CookedMeshParseResult refuse(CookedMeshStatus status, std::string message) {
    CookedMeshParseResult out;
    out.status = status;
    out.message = std::move(message);
    return out;
}

[[nodiscard]] CookedBounds readBounds(std::span<const std::byte> b, std::size_t minOff, std::size_t maxOff) {
    CookedBounds out;
    out.min = Vec3{getF32(b, minOff), getF32(b, minOff + 4), getF32(b, minOff + 8)};
    out.max = Vec3{getF32(b, maxOff), getF32(b, maxOff + 4), getF32(b, maxOff + 8)};
    return out;
}

}  // namespace

std::string_view cookedMeshStatusLabel(CookedMeshStatus status) noexcept {
    switch (status) {
        case CookedMeshStatus::Ok:
            return "Ok";
        case CookedMeshStatus::TooSmall:
            return "Too small";
        case CookedMeshStatus::BadMagic:
            return "Bad magic";
        case CookedMeshStatus::UnsupportedVersion:
            return "Unsupported version";
        case CookedMeshStatus::ReservedNotZero:
            return "Reserved field not zero";
        case CookedMeshStatus::SizeMismatch:
            return "Size mismatch";
        case CookedMeshStatus::CapExceeded:
            return "Cap exceeded";
        case CookedMeshStatus::BadTable:
            return "Bad table";
        case CookedMeshStatus::BadRange:
            return "Bad range";
        case CookedMeshStatus::BadLayout:
            return "Bad layout";
    }
    return "Unknown";  // unreachable; the switch has no default so a new enumerator is a -Wswitch error
}

// The fits() re-check in both accessors is deliberate belt-and-braces: the parse already validated
// both ranges, so it can never fire on an Ok mesh. It is what makes them total against a
// HAND-CONSTRUCTED CookedMesh, which a test can build and a caller could.
std::span<const std::byte> sectionVertexBytes(const CookedMesh& mesh, std::uint32_t sectionIndex) noexcept {
    if (sectionIndex >= mesh.sections.size()) {
        return {};
    }
    const CookedSection& s = mesh.sections[sectionIndex];
    if (!fits(s.vertexDataOffset, s.vertexDataBytes, mesh.bytes.size())) {
        return {};
    }
    return mesh.bytes.subspan(static_cast<std::size_t>(s.vertexDataOffset),
                              static_cast<std::size_t>(s.vertexDataBytes));
}

std::span<const std::byte> indexBytes(const CookedMesh& mesh) noexcept {
    const std::uint64_t n = static_cast<std::uint64_t>(mesh.indexCount) * cookedIndexTypeBytes(mesh.indexType);
    if (!fits(mesh.indexDataOffset, n, mesh.bytes.size())) {
        return {};
    }
    return mesh.bytes.subspan(static_cast<std::size_t>(mesh.indexDataOffset), static_cast<std::size_t>(n));
}

CookedMeshParseResult parseCookedMesh(std::span<const std::byte> bytes) {
    AERO_PROFILE_ZONE_NAMED("assets::parseCookedMesh");

    // 1. shorter than the header.
    if (bytes.size() < COOKED_MESH_HEADER_BYTES) {
        return refuse(CookedMeshStatus::TooSmall, std::format("the buffer is {} bytes, shorter than the {}-byte header",
                                                              bytes.size(), COOKED_MESH_HEADER_BYTES));
    }
    // 2. absurdly large -- a cheap early-out BEFORE anything is interpreted.
    if (bytes.size() > MAX_COOKED_MESH_BYTES) {
        return refuse(CookedMeshStatus::CapExceeded,
                      std::format("the buffer is {} bytes, over the {}-byte cooked mesh cap", bytes.size(),
                                  MAX_COOKED_MESH_BYTES));
    }

    // 3. magic, then version. Compared BYTE BY BYTE against COOKED_MESH_MAGIC -- never a memcmp of a
    //    reinterpret_cast'd pointer, for the same reason nothing else in this subsystem does that.
    for (std::size_t i = 0; i < COOKED_MESH_MAGIC.size(); ++i) {
        if (bytes[H_MAGIC + i] != static_cast<std::byte>(COOKED_MESH_MAGIC[i])) {
            return refuse(CookedMeshStatus::BadMagic, "the buffer does not begin with AEROMESH");
        }
    }
    const std::uint32_t formatVersion = getU32(bytes, H_FORMAT_VERSION);
    if (formatVersion != COOKED_MESH_FORMAT_VERSION) {
        return refuse(CookedMeshStatus::UnsupportedVersion,
                      std::format("cooked mesh format version {} (this build reads version {})", formatVersion,
                                  COOKED_MESH_FORMAT_VERSION));
    }
    // 4. the header's reserved space. A REFUSAL, deliberately -- see the header's note and docs/09.
    if (getU32(bytes, H_FLAGS) != 0) {
        return refuse(CookedMeshStatus::ReservedNotZero, "the header's reserved flags field is not zero");
    }
    // 5. totalBytes must equal the buffer's own size.
    const std::uint64_t totalBytes = getU64(bytes, H_TOTAL_BYTES);
    if (totalBytes != bytes.size()) {
        return refuse(
            CookedMeshStatus::SizeMismatch,
            std::format("the header declares {} total bytes but the buffer holds {}", totalBytes, bytes.size()));
    }

    // 6. counts against their caps, and indexType against its two legal values. NOTHING IS ALLOCATED
    //    UNTIL THIS BLOCK HAS PASSED -- that is the whole point of doing it here.
    const std::uint32_t attributeCount = getU32(bytes, H_ATTRIBUTE_COUNT);
    const std::uint32_t sectionCount = getU32(bytes, H_SECTION_COUNT);
    const std::uint32_t submeshCount = getU32(bytes, H_SUBMESH_COUNT);
    const std::uint32_t indexCount = getU32(bytes, H_INDEX_COUNT);
    if (attributeCount > MAX_COOKED_ATTRIBUTES) {
        return refuse(CookedMeshStatus::CapExceeded,
                      std::format("the header declares {} attributes, over the cap of {}", attributeCount,
                                  MAX_COOKED_ATTRIBUTES));
    }
    if (sectionCount > MAX_COOKED_SECTIONS) {
        return refuse(CookedMeshStatus::CapExceeded, std::format("the header declares {} sections, over the cap of {}",
                                                                 sectionCount, MAX_COOKED_SECTIONS));
    }
    if (submeshCount > MAX_COOKED_SUBMESHES) {
        return refuse(CookedMeshStatus::CapExceeded, std::format("the header declares {} submeshes, over the cap of {}",
                                                                 submeshCount, MAX_COOKED_SUBMESHES));
    }
    if (indexCount > MAX_COOKED_INDICES) {
        return refuse(CookedMeshStatus::CapExceeded, std::format("the header declares {} indices, over the cap of {}",
                                                                 indexCount, MAX_COOKED_INDICES));
    }
    const std::uint32_t indexTypeRaw = getU32(bytes, H_INDEX_TYPE);
    if (indexTypeRaw > 1) {
        return refuse(CookedMeshStatus::BadTable,
                      std::format("index type code {} is neither 0 (u16) nor 1 (u32)", indexTypeRaw));
    }

    // 7. the three table regions, computed in u64. With the caps above this can never overflow
    //    (96 + 8*1024 + 32*128 + 64*65536 ~= 4.2 MB), which is exactly why step 6 precedes it.
    const std::uint64_t tableEnd = COOKED_MESH_HEADER_BYTES +
                                   (std::uint64_t{attributeCount} * COOKED_MESH_ATTRIBUTE_BYTES) +
                                   (std::uint64_t{sectionCount} * COOKED_MESH_SECTION_BYTES) +
                                   (std::uint64_t{submeshCount} * COOKED_MESH_SUBMESH_BYTES);
    if (tableEnd > bytes.size()) {
        return refuse(CookedMeshStatus::BadTable,
                      std::format("the three tables end at {} but the buffer holds {} bytes", tableEnd, bytes.size()));
    }

    CookedMesh mesh;  // ONLY NOW is anything reserved.
    mesh.attributes.reserve(attributeCount);
    mesh.sections.reserve(sectionCount);
    mesh.submeshes.reserve(submeshCount);

    // 8. attributes: an unknown semantic or format code is BadLayout.
    const std::size_t attrBase = COOKED_MESH_HEADER_BYTES;
    for (std::uint32_t i = 0; i < attributeCount; ++i) {
        const std::size_t o = attrBase + (std::size_t{i} * COOKED_MESH_ATTRIBUTE_BYTES);
        const std::uint16_t semantic = getU16(bytes, o + A_SEMANTIC);
        const std::uint16_t format = getU16(bytes, o + A_FORMAT);
        if (semantic >= COOKED_SEMANTIC_COUNT) {
            return refuse(CookedMeshStatus::BadLayout,
                          std::format("attribute {} declares unknown semantic code {}", i, semantic));
        }
        if (format > static_cast<std::uint16_t>(CookedVertexFormat::Uint4)) {
            return refuse(CookedMeshStatus::BadLayout,
                          std::format("attribute {} declares unknown format code {}", i, format));
        }
        mesh.attributes.push_back(CookedVertexAttribute{static_cast<CookedVertexSemantic>(semantic),
                                                        static_cast<CookedVertexFormat>(format),
                                                        getU32(bytes, o + A_OFFSET)});
    }

    // 9. sections. SEVEN checks, and C9 adds the first three.
    const std::size_t sectionBase = attrBase + (std::size_t{attributeCount} * COOKED_MESH_ATTRIBUTE_BYTES);
    for (std::uint32_t i = 0; i < sectionCount; ++i) {
        const std::size_t o = sectionBase + (std::size_t{i} * COOKED_MESH_SECTION_BYTES);
        CookedSection s;
        s.firstAttribute = getU32(bytes, o + S_FIRST_ATTRIBUTE);
        s.attributeCount = getU32(bytes, o + S_ATTRIBUTE_COUNT);
        s.vertexStride = getU32(bytes, o + S_VERTEX_STRIDE);
        s.vertexCount = getU32(bytes, o + S_VERTEX_COUNT);
        s.vertexDataOffset = getU64(bytes, o + S_VERTEX_DATA_OFFSET);
        s.vertexDataBytes = getU64(bytes, o + S_VERTEX_DATA_BYTES);

        // C9.1 -- the cook never emits one (Position is mandatory) and a section declaring no
        // attributes is meaningless to every consumer.
        if (s.attributeCount == 0) {
            return refuse(CookedMeshStatus::BadLayout, std::format("section {} declares no attributes", i));
        }
        // C9.2 -- THE SHARPEST OF THE THREE. With a zero stride, vertexDataBytes == vertexCount * 0
        // == 0 satisfies the consistency check below for ANY vertexCount, so a header could claim
        // four billion vertices backed by no bytes at all, and every consumer then divides by it.
        if (s.vertexStride == 0) {
            return refuse(CookedMeshStatus::BadLayout, std::format("section {} declares a zero vertex stride", i));
        }
        // C9.3 -- a writer invariant (AC-10) made a reader refusal for one modulo, which closes the
        // misaligned-attribute class outright.
        if (s.vertexStride % 4 != 0) {
            return refuse(CookedMeshStatus::BadLayout,
                          std::format("section {} declares vertex stride {}, not a multiple of 4", i, s.vertexStride));
        }
        // the section's attribute slice must lie inside the attribute table -- SUBTRACTION.
        if (s.attributeCount > attributeCount || s.firstAttribute > attributeCount - s.attributeCount) {
            return refuse(CookedMeshStatus::BadTable,
                          std::format("section {} names attributes [{}, {}) outside a table of {}", i, s.firstAttribute,
                                      std::uint64_t{s.firstAttribute} + s.attributeCount, attributeCount));
        }
        if (s.vertexCount > MAX_COOKED_VERTICES) {
            return refuse(CookedMeshStatus::CapExceeded, std::format("section {} declares {} vertices, over the cap "
                                                                     "of {}",
                                                                     i, s.vertexCount, MAX_COOKED_VERTICES));
        }
        if (s.vertexDataBytes != std::uint64_t{s.vertexCount} * s.vertexStride) {
            return refuse(CookedMeshStatus::BadRange,
                          std::format("section {} declares {} vertex bytes for {} vertices of stride {}", i,
                                      s.vertexDataBytes, s.vertexCount, s.vertexStride));
        }
        if (s.vertexDataOffset % COOKED_MESH_ALIGNMENT != 0) {
            return refuse(CookedMeshStatus::BadRange,
                          std::format("section {}'s vertex data offset {} is not a multiple of {}", i,
                                      s.vertexDataOffset, COOKED_MESH_ALIGNMENT));
        }
        if (!fits(s.vertexDataOffset, s.vertexDataBytes, bytes.size())) {
            return refuse(CookedMeshStatus::BadRange,
                          std::format("section {}'s vertex region [{}, +{}) leaves a buffer of {} bytes", i,
                                      s.vertexDataOffset, s.vertexDataBytes, bytes.size()));
        }
        // every attribute inside the stride, and no duplicate semantic. An 8-bit mask, no allocation.
        std::uint32_t seen = 0;
        for (std::uint32_t a = 0; a < s.attributeCount; ++a) {
            const CookedVertexAttribute& attr = mesh.attributes[s.firstAttribute + a];
            const std::uint32_t width = cookedVertexFormatBytes(attr.format);
            if (width > s.vertexStride || attr.offset > s.vertexStride - width) {
                return refuse(CookedMeshStatus::BadLayout,
                              std::format("section {}'s attribute {} occupies [{}, +{}) of a stride of {}", i, a,
                                          attr.offset, width, s.vertexStride));
            }
            const std::uint32_t bit = 1U << static_cast<std::uint32_t>(attr.semantic);
            if ((seen & bit) != 0) {
                return refuse(CookedMeshStatus::BadLayout, std::format("section {} declares semantic {} twice", i,
                                                                       static_cast<std::uint32_t>(attr.semantic)));
            }
            seen |= bit;
        }
        mesh.sections.push_back(s);
    }

    // 10. submeshes. The bound for firstIndex/indexCount is the HEADER's indexCount, not the buffer
    //     size -- and it is still written as subtraction.
    const std::size_t submeshBase = sectionBase + (std::size_t{sectionCount} * COOKED_MESH_SECTION_BYTES);
    for (std::uint32_t i = 0; i < submeshCount; ++i) {
        const std::size_t o = submeshBase + (std::size_t{i} * COOKED_MESH_SUBMESH_BYTES);
        if (getU64(bytes, o + M_RESERVED0) != 0 || getU64(bytes, o + M_RESERVED1) != 0) {
            return refuse(CookedMeshStatus::ReservedNotZero,
                          std::format("submesh {} has a non-zero reserved field", i));
        }
        CookedSubmesh m;
        m.sectionIndex = getU32(bytes, o + M_SECTION_INDEX);
        m.firstIndex = getU32(bytes, o + M_FIRST_INDEX);
        m.indexCount = getU32(bytes, o + M_INDEX_COUNT);
        m.materialIndex = getU32(bytes, o + M_MATERIAL);
        m.sourceMeshIndex = getU32(bytes, o + M_SOURCE_MESH);
        m.sourcePrimitiveIndex = getU32(bytes, o + M_SOURCE_PRIMITIVE);
        m.bounds = readBounds(bytes, o + M_BOUNDS_MIN, o + M_BOUNDS_MAX);
        if (m.sectionIndex >= sectionCount) {
            return refuse(CookedMeshStatus::BadRange,
                          std::format("submesh {} names section {} of {}", i, m.sectionIndex, sectionCount));
        }
        if (m.indexCount > indexCount || m.firstIndex > indexCount - m.indexCount) {
            return refuse(CookedMeshStatus::BadRange,
                          std::format("submesh {} names indices [{}, {}) of {}", i, m.firstIndex,
                                      std::uint64_t{m.firstIndex} + m.indexCount, indexCount));
        }
        mesh.submeshes.push_back(m);
    }

    // 11. the index region.
    const std::uint64_t indexDataOffset = getU64(bytes, H_INDEX_DATA_OFFSET);
    if (indexDataOffset % COOKED_MESH_ALIGNMENT != 0) {
        return refuse(CookedMeshStatus::BadRange, std::format("the index data offset {} is not a multiple of {}",
                                                              indexDataOffset, COOKED_MESH_ALIGNMENT));
    }
    const auto indexType = static_cast<CookedIndexType>(indexTypeRaw);
    const std::uint64_t indexBytesTotal = std::uint64_t{indexCount} * cookedIndexTypeBytes(indexType);
    if (!fits(indexDataOffset, indexBytesTotal, bytes.size())) {
        return refuse(CookedMeshStatus::BadRange, std::format("the index region [{}, +{}) leaves a buffer of {} bytes",
                                                              indexDataOffset, indexBytesTotal, bytes.size()));
    }

    // 12. done. `bytes` is retained AS IS; the two accessors are now total by construction.
    mesh.formatVersion = formatVersion;
    mesh.cookerVersion = getU32(bytes, H_COOKER_VERSION);
    mesh.sourceGuid = Guid{getU64(bytes, H_GUID_HI), getU64(bytes, H_GUID_LO)};
    mesh.indexType = indexType;
    mesh.indexCount = indexCount;
    mesh.indexDataOffset = indexDataOffset;
    mesh.bounds = readBounds(bytes, H_BOUNDS_MIN, H_BOUNDS_MAX);
    mesh.bytes = bytes;
    return CookedMeshParseResult{CookedMeshStatus::Ok, std::string{}, std::move(mesh)};
}

}  // namespace engine::assets

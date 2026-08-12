#pragma once
// Aero Engine — the cooked mesh container v1 (task 3.3.1). THE TREE'S FIRST BINARY FORMAT.
//
// PURE: no disk, no <fstream>, no <filesystem>, no logging, no third party, no GPU, no per-OS macro.
// This header and its .cpp include <aero/core/guid.hpp>, <aero/core/math.hpp> and the standard
// library, and nothing else, ever.
//
// The normative specification of this format is docs/09-file-formats.md section 9. THIS HEADER IS NOT
// THE SPEC -- if the two ever disagree, docs/09 wins and one of them is a bug. The four
// COOKED_MESH_*_BYTES constants below are the ONLY sizes: `sizeof` is never taken of an on-disk
// record anywhere in this subsystem (INV-C5), because a struct's size is a compiler's opinion and a
// format's is not.
//
// DETERMINISM IS BY CONSTRUCTION (D7), not by test:
//   * no struct memcpy, ever -- every field goes through the put*/get* primitives below, so compiler
//     padding, member reordering and alignment choices cannot reach the file;
//   * no hash container anywhere in this subsystem -- no std::map, no std::unordered_map, no set;
//   * all padding is explicitly zero, because the output buffer is allocated zero-initialized;
//   * no timestamp, no path, no hostname, no user name, no build id. The only provenance fields are
//     cookerVersion (a compile-time constant) and the caller-supplied sourceGuid;
//   * floats are re-emitted bit-for-bit via std::bit_cast; the cook performs no floating-point
//     ARITHMETIC on vertex data at all.
//
// LITTLE-ENDIAN, DECLARED (D8). All three target hosts are little-endian; the format says LE rather
// than "native" and the primitives do explicit byte assembly, so it stays correct on a hypothetical
// big-endian host with no version bump. That costs a few instructions in a build-time tool.
#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

// ---- identity ------------------------------------------------------------------------------
inline constexpr std::string_view COOKED_MESH_MAGIC = "AEROMESH";  // 8 ASCII bytes, no NUL
inline constexpr std::uint32_t COOKED_MESH_FORMAT_VERSION = 1;
// Bumped when cooked OUTPUT changes for unchanged input (a cache-invalidation signal). NOT the same
// thing as formatVersion, which is bumped when an older READER can no longer read the file -- and
// note that occupying a reserved field or adding a semantic/format code is a formatVersion bump, not
// a cookerVersion one, because the parser REFUSES both (docs/09 section 9's versioning rules).
inline constexpr std::uint32_t COOKED_MESH_COOKER_VERSION = 1;
inline constexpr std::uint32_t COOKED_INVALID_MATERIAL = 0xFFFFFFFFU;

// ---- record sizes. The ONLY sizes. See INV-C5 above. -----------------------------------------
inline constexpr std::size_t COOKED_MESH_HEADER_BYTES = 96;
inline constexpr std::size_t COOKED_MESH_ATTRIBUTE_BYTES = 8;
inline constexpr std::size_t COOKED_MESH_SECTION_BYTES = 32;
inline constexpr std::size_t COOKED_MESH_SUBMESH_BYTES = 64;
// The alignment of the two STORED offsets (CookedSection::vertexDataOffset and the header's
// indexDataOffset) and of every bulk region's start. It is deliberately NOT the alignment of the
// three TABLES: those are packed with no padding at implicit 8-byte-aligned starts the reader
// derives, so with an odd attributeCount the section table legitimately begins at 104. Nothing is
// lost -- no table is ever memcpy'd or cast, every field goes through get*.
inline constexpr std::size_t COOKED_MESH_ALIGNMENT = 16;

// ---- frozen enums. THESE VALUES ARE PART OF THE FORMAT AND NEVER CHANGE. -----------------------
// The semantic codes mirror editor::VertexAttribute's BIT POSITIONS exactly (Position = 1<<0 ...
// Weights0 = 1<<7), so "bit n set" means "semantic n present". The two enums live in different layers
// and neither includes the other -- engine/assets may never include an editor header -- so that
// correspondence is asserted in the editor test tier, which is the one place both are visible.
//
// The underlying types are the FORMAT's widths, not a size optimization: each of the three is the
// exact width of the field it is written to and read from. NOLINT because a narrower type would
// change the on-disk record, which is the one thing that must never happen.
// NOLINTNEXTLINE(performance-enum-size)
enum class CookedVertexSemantic : std::uint16_t {
    Position = 0,
    Normal = 1,
    Tangent = 2,
    TexCoord0 = 3,
    TexCoord1 = 4,
    Color0 = 5,
    Joints0 = 6,
    Weights0 = 7,
};
inline constexpr std::uint16_t COOKED_SEMANTIC_COUNT = 8;

// Explicitly numbered and frozen FOREVER, and that is the whole reason this enum exists rather than
// rhi::VertexFormat being written to disk (see engine/assets/CMakeLists.txt's comment).
// NOLINTNEXTLINE(performance-enum-size)
enum class CookedVertexFormat : std::uint16_t { Float2 = 0, Float3 = 1, Float4 = 2, Uint4 = 3 };
// NOLINTNEXTLINE(performance-enum-size)
enum class CookedIndexType : std::uint32_t { Uint16 = 0, Uint32 = 1 };

// Both TOTAL, both switches with NO `default:` -- a future enumerator is a -Wswitch failure on the
// Linux lane rather than a silent zero (the importStatusLabel precedent).
[[nodiscard]] constexpr std::uint32_t cookedVertexFormatBytes(CookedVertexFormat f) noexcept {
    switch (f) {
        case CookedVertexFormat::Float2:
            return 8;
        case CookedVertexFormat::Float3:
            return 12;
        // Float4 and Uint4 share an arm because they share a WIDTH, not because they are the same
        // thing -- a Uint4 joint index is four u32s, not four floats. Merged rather than written out
        // twice only because bugprone-branch-clone rejects consecutive identical branches.
        case CookedVertexFormat::Float4:
        case CookedVertexFormat::Uint4:
            return 16;
    }
    return 0;
}
[[nodiscard]] constexpr std::uint32_t cookedIndexTypeBytes(CookedIndexType t) noexcept {
    switch (t) {
        case CookedIndexType::Uint16:
            return 2;
        case CookedIndexType::Uint32:
            return 4;
    }
    return 0;
}

// v1's semantic -> format table, TOTAL and FROZEN. Max stride 104, min 12 (position only).
//   Position Float3 12 | Normal Float3 12 | Tangent Float4 16 | TexCoord0 Float2 8
//   TexCoord1 Float2 8 | Color0 Float4 16 | Joints0 Uint4 16  | Weights0 Float4 16
// Joints0 is Uint4 rather than a u16x4 because rhi::VertexFormat has NO unsigned-16x4 enumerator and
// UByte4Norm is NORMALIZED and therefore wrong for an index. Skinned vertices pay 8 bytes each for
// that; the v2 fix is rhi::VertexFormat::UShort4 plus a CookedVertexFormat sibling, which is a
// formatVersion bump (R2).
[[nodiscard]] constexpr CookedVertexFormat cookedFormatForSemantic(CookedVertexSemantic s) noexcept {
    switch (s) {
        // Adjacent semantics that share a format share an arm -- bugprone-branch-clone rejects
        // consecutive identical branches. The table above is the normative row-per-semantic form.
        case CookedVertexSemantic::Position:
        case CookedVertexSemantic::Normal:
            return CookedVertexFormat::Float3;
        case CookedVertexSemantic::Tangent:
            return CookedVertexFormat::Float4;
        case CookedVertexSemantic::TexCoord0:
        case CookedVertexSemantic::TexCoord1:
            return CookedVertexFormat::Float2;
        case CookedVertexSemantic::Color0:
            return CookedVertexFormat::Float4;
        case CookedVertexSemantic::Joints0:
            return CookedVertexFormat::Uint4;
        case CookedVertexSemantic::Weights0:
            return CookedVertexFormat::Float4;
    }
    return CookedVertexFormat::Float3;
}

// ---- caps. Enforced by BOTH the writer and the parser. ---------------------------------------
// MAX_COOKED_SECTIONS is exactly the number of representable layouts: Position is mandatory, so a
// mask has at most 2^7 distinct values. Well-formed input can REACH 128 sections and can never EXCEED
// them, so the cook's own Truncated arm for this cap (and for MAX_COOKED_ATTRIBUTES, which is derived
// from it) is UNREACHABLE and is defence in depth. The cap exists for the PARSER, where a header
// claiming 129 sections is where it is actually proven.
inline constexpr std::uint32_t MAX_COOKED_SECTIONS = 128;
inline constexpr std::uint32_t MAX_COOKED_ATTRIBUTES = 1024;  // 128 sections x 8 semantics
inline constexpr std::uint32_t MAX_COOKED_SUBMESHES = 65536;
inline constexpr std::uint32_t MAX_COOKED_VERTICES = 8000000;  // mirrors MAX_VERTICES_PER_MODEL
inline constexpr std::uint32_t MAX_COOKED_INDICES = 24000000;  // mirrors MAX_INDICES_PER_MODEL
// A cheap parser early-out. It can NEVER refuse a legitimately-cooked artifact: the caps above bound
// the largest legal file at 8000000*104 + 24000000*4 ~= 928 MB.
inline constexpr std::uint64_t MAX_COOKED_MESH_BYTES = 2ULL * 1024 * 1024 * 1024;
inline constexpr std::size_t MAX_COOK_WARNINGS = 20;  // mirrors MAX_IMPORT_WARNINGS

// ---- the byte primitives. THE ONLY PLACE BYTES ARE FORMED IN THIS SUBSYSTEM. -------------------
// PUBLIC and constexpr on purpose (plan C4): the endianness is then a static_assert rather than a
// test, so a big-endian mistake is a BUILD failure. They are also part of the format's meaning, which
// is why they are not hidden in a src-private header.
//
// TOTAL: an out-of-range offset writes nothing and reads 0. The parser validates every range before
// calling, so this is defence in depth, not the primary guard. `buffer`+`offset` rather than a bare
// pointer keeps raw pointer arithmetic out of the emitter entirely.
constexpr void putU16(std::span<std::byte> buffer, std::size_t offset, std::uint16_t value) noexcept {
    if (offset > buffer.size() || buffer.size() - offset < 2) {
        return;
    }
    buffer[offset + 0] = static_cast<std::byte>(value & 0xFFU);
    buffer[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}
constexpr void putU32(std::span<std::byte> buffer, std::size_t offset, std::uint32_t value) noexcept {
    if (offset > buffer.size() || buffer.size() - offset < 4) {
        return;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        buffer[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}
constexpr void putU64(std::span<std::byte> buffer, std::size_t offset, std::uint64_t value) noexcept {
    if (offset > buffer.size() || buffer.size() - offset < 8) {
        return;
    }
    for (std::size_t i = 0; i < 8; ++i) {
        buffer[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}
// THE ONE PLACE A FLOAT BECOMES BYTES. std::bit_cast rather than a union or a memcpy through char*:
// it is the only spelling that is neither UB nor a strict-aliasing argument, and it is constexpr.
constexpr void putF32(std::span<std::byte> buffer, std::size_t offset, float value) noexcept {
    putU32(buffer, offset, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] constexpr std::uint16_t getU16(std::span<const std::byte> buffer, std::size_t offset) noexcept {
    if (offset > buffer.size() || buffer.size() - offset < 2) {
        return 0;
    }
    const auto lo = static_cast<std::uint32_t>(buffer[offset + 0]);
    const auto hi = static_cast<std::uint32_t>(buffer[offset + 1]);
    return static_cast<std::uint16_t>(lo | (hi << 8U));
}
[[nodiscard]] constexpr std::uint32_t getU32(std::span<const std::byte> buffer, std::size_t offset) noexcept {
    if (offset > buffer.size() || buffer.size() - offset < 4) {
        return 0;
    }
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(buffer[offset + i]) << (8U * i);
    }
    return v;
}
[[nodiscard]] constexpr std::uint64_t getU64(std::span<const std::byte> buffer, std::size_t offset) noexcept {
    if (offset > buffer.size() || buffer.size() - offset < 8) {
        return 0;
    }
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(buffer[offset + i]) << (8U * i);
    }
    return v;
}
[[nodiscard]] constexpr float getF32(std::span<const std::byte> buffer, std::size_t offset) noexcept {
    return std::bit_cast<float>(getU32(buffer, offset));
}

// THE ENDIANNESS PROOF. Not a test -- a build failure. Seeding a big-endian putU32 does not redden a
// case, it fails to compile, which is the strongest available outcome for the single property this
// whole format rests on.
namespace detail {
[[nodiscard]] constexpr bool byteOrderRoundTrip() noexcept {
    std::array<std::byte, 8> b{};
    const std::span<std::byte> w(b);
    putU32(w, 0, 0x12345678U);
    const bool littleEndian =
        b[0] == std::byte{0x78} && b[1] == std::byte{0x56} && b[2] == std::byte{0x34} && b[3] == std::byte{0x12};
    putU16(w, 4, 0xABCDU);
    const bool u16Ok = b[4] == std::byte{0xCD} && b[5] == std::byte{0xAB};
    const std::span<const std::byte> r(b);
    const bool inverts = getU32(r, 0) == 0x12345678U && getU16(r, 4) == 0xABCDU;
    return littleEndian && u16Ok && inverts;
}
[[nodiscard]] constexpr bool floatRoundTrip() noexcept {
    std::array<std::byte, 4> b{};
    putF32(std::span<std::byte>(b), 0, 1.0F);
    // 1.0f is 0x3F800000, so little-endian bytes are 00 00 80 3F.
    return b[0] == std::byte{0x00} && b[1] == std::byte{0x00} && b[2] == std::byte{0x80} && b[3] == std::byte{0x3F} &&
           getF32(std::span<const std::byte>(b), 0) == 1.0F;
}
}  // namespace detail
static_assert(detail::byteOrderRoundTrip(), "the cooked mesh container is little-endian BY DEFINITION");
static_assert(detail::floatRoundTrip(), "float bytes must be IEEE-754 binary32, little-endian");

// ---- the parsed records ----------------------------------------------------------------------
struct CookedBounds {
    Vec3 min{};
    Vec3 max{};
};

struct CookedVertexAttribute {
    CookedVertexSemantic semantic = CookedVertexSemantic::Position;
    CookedVertexFormat format = CookedVertexFormat::Float3;
    std::uint32_t offset = 0;  // bytes from the start of a vertex
};

struct CookedSection {
    std::uint32_t firstAttribute = 0;
    std::uint32_t attributeCount = 0;
    std::uint32_t vertexStride = 0;
    std::uint32_t vertexCount = 0;
    std::uint64_t vertexDataOffset = 0;  // absolute, 16-aligned
    std::uint64_t vertexDataBytes = 0;   // == vertexCount * vertexStride
};

struct CookedSubmesh {
    std::uint32_t sectionIndex = 0;
    std::uint32_t firstIndex = 0;  // in index UNITS, into the file's single index buffer
    std::uint32_t indexCount = 0;
    std::uint32_t materialIndex = COOKED_INVALID_MATERIAL;
    // The POSITION in ImportedModel::meshes and in that mesh's `primitives` -- NOT ImportedMesh::
    // localId, which for FBX is a raw ufbx typed_id rather than a dense index. This is the value a
    // consumer resolves (ImportedNode::meshIndex holds the same thing).
    std::uint32_t sourceMeshIndex = 0;
    std::uint32_t sourcePrimitiveIndex = 0;
    CookedBounds bounds;
};

// LIFETIME: `bytes` IS the buffer handed to parseCookedMesh, retained as a span. Every table's
// offsets are absolute into it, so they mean exactly what the file says and need no rebasing. The
// three tables are OWNED copies (bounded by the caps, so always small); the bulk data is NEVER
// copied -- that is the whole promise of this format. A CookedMesh outliving its buffer is a dangling
// read, and the only defence is this comment plus the two accessors below, which are the sanctioned
// way to reach bulk data. Nothing else should index `bytes` by hand.
struct CookedMesh {
    std::uint32_t formatVersion = 0;
    std::uint32_t cookerVersion = 0;
    Guid sourceGuid;
    CookedIndexType indexType = CookedIndexType::Uint16;
    CookedBounds bounds;
    std::uint32_t indexCount = 0;
    std::uint64_t indexDataOffset = 0;
    std::vector<CookedVertexAttribute> attributes;
    std::vector<CookedSection> sections;
    std::vector<CookedSubmesh> submeshes;
    std::span<const std::byte> bytes;
};

// Both TOTAL on a mesh parseCookedMesh returned Ok for: every offset and length they use was
// validated during the parse, so neither can be handed an out-of-range range. An out-of-range
// sectionIndex returns an EMPTY span rather than reading -- a caller bug must not become a read.
[[nodiscard]] std::span<const std::byte> sectionVertexBytes(const CookedMesh& mesh,
                                                            std::uint32_t sectionIndex) noexcept;
[[nodiscard]] std::span<const std::byte> indexBytes(const CookedMesh& mesh) noexcept;

enum class CookedMeshStatus : std::uint8_t {
    Ok = 0,
    TooSmall,            // shorter than the header
    BadMagic,            //
    UnsupportedVersion,  //
    ReservedNotZero,     // a reserved field is non-zero -- a REFUSAL, deliberately, not a tolerated unknown
    SizeMismatch,        // totalBytes != the buffer's own size
    CapExceeded,         //
    BadTable,            // a table region does not fit, or indexType is not 0/1
    BadRange,            // an offset/length pair does not fit, or is misaligned, or is inconsistent
    BadLayout,           // an unknown code, a duplicate semantic, or an attribute outside its stride
};
// A switch with NO `default:` (the importStatusLabel precedent).
[[nodiscard]] std::string_view cookedMeshStatusLabel(CookedMeshStatus status) noexcept;

struct CookedMeshParseResult {
    CookedMeshStatus status = CookedMeshStatus::Ok;
    std::string message;  // "" IFF status == Ok
    CookedMesh mesh;      // meaningful only when status == Ok
};

// NEVER THROWS. NEVER READS A FILE. NEVER LOGS.
//
// Written to the importer's hostile-input standard from day one (D13), because at Phase 5 this reads
// bytes out of a .pak that may have been shipped, patched, truncated by a failed download, or
// crafted. Every range check is a SUBTRACTION against the known-good size (`len > size || off > size
// - len`), never an addition that can wrap; nothing is allocated before the count it is allocating
// for has been checked against a frozen cap.
//
// TWO THINGS IT DELIBERATELY DOES NOT CHECK, both stated rather than discovered:
//   1. individual index VALUES against their section's vertexCount -- that is O(indexCount) work on up
//      to 24 million entries on every load, and the consumer that uploads to the GPU is where an
//      out-of-range index is a driver concern. First-party cooked files are always in range (the cook
//      validates them). The Phase 5 answer is an opt-in parseCookedMeshStrict, chosen by the caller.
//   2. whether the vertex regions and the index region OVERLAP each other or the tables. Every read
//      through the two accessors is bounds-checked against the buffer, so an overlap is a wrong
//      picture, never a memory error; refusing it needs an interval sort over up to 129 ranges and
//      buys nothing an attacker can use.
[[nodiscard]] CookedMeshParseResult parseCookedMesh(std::span<const std::byte> bytes);

}  // namespace engine::assets

// tests/cooked_mesh_test.cpp -- task 3.3.1: the .aeromesh container v1. A TU of aero_tests, which
// supplies main() from test_main.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier-0: no GPU, no window, no disk. Every buffer this file parses is built BY THIS FILE, byte by
// byte, from docs/09 section 9's own rules -- deliberately, so a cook bug can never mask a parser
// bug and every refusal arm mutates exactly one field of something already valid.
#include <aero/assets/cooked_mesh.hpp>
#include <aero/rhi/types.hpp>

#include "cooked_mesh_golden.hpp"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using engine::assets::cookedFormatForSemantic;
using engine::assets::CookedIndexType;
using engine::assets::cookedIndexTypeBytes;
using engine::assets::CookedMeshStatus;
using engine::assets::cookedMeshStatusLabel;
using engine::assets::CookedVertexFormat;
using engine::assets::cookedVertexFormatBytes;
using engine::assets::CookedVertexSemantic;

namespace {

// The ten CookedMeshStatus enumerators, in declaration order. A table rather than a loop over an
// integer range: the enum has no sentinel and adding one to a range-based loop would be a silent
// out-of-range cast.
constexpr std::array<CookedMeshStatus, 10> ALL_STATUSES = {CookedMeshStatus::Ok,
                                                           CookedMeshStatus::TooSmall,
                                                           CookedMeshStatus::BadMagic,
                                                           CookedMeshStatus::UnsupportedVersion,
                                                           CookedMeshStatus::ReservedNotZero,
                                                           CookedMeshStatus::SizeMismatch,
                                                           CookedMeshStatus::CapExceeded,
                                                           CookedMeshStatus::BadTable,
                                                           CookedMeshStatus::BadRange,
                                                           CookedMeshStatus::BadLayout};

constexpr std::array<CookedVertexSemantic, 8> ALL_SEMANTICS = {
    CookedVertexSemantic::Position,  CookedVertexSemantic::Normal,    CookedVertexSemantic::Tangent,
    CookedVertexSemantic::TexCoord0, CookedVertexSemantic::TexCoord1, CookedVertexSemantic::Color0,
    CookedVertexSemantic::Joints0,   CookedVertexSemantic::Weights0};

// ---- the field offsets, restated INDEPENDENTLY of cooked_mesh.cpp's anonymous namespace ---------
// Deliberately a second copy, transcribed from docs/09 section 9's tables rather than from the
// parser: a test that shared the parser's own constants could not catch a header field moving.
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
constexpr std::size_t M_RESERVED0 = 24;
constexpr std::size_t M_BOUNDS_MIN = 32;
constexpr std::size_t M_BOUNDS_MAX = 44;
constexpr std::size_t M_RESERVED1 = 56;

constexpr std::size_t A_SEMANTIC = 0;
constexpr std::size_t A_FORMAT = 2;
constexpr std::size_t A_OFFSET = 4;

// ---- the three hand-built buffers ---------------------------------------------------------------
// EMPTY      -- 96 bytes, zero counts. The smallest thing that is not TooSmall.
// MINIMAL    -- 272 bytes: one position-only triangle. attributeCount is ODD, so the section table
//               starts at 104 and the submesh table at 136, NEITHER 16-aligned. That is C1's proof
//               and it is baked into the shape on purpose.
// TWO_SECTION -- 480 bytes: a position-only triangle and a position+normal+uv0 triangle, with a
//               non-nil GUID so both header u64 halves are byte-visible.
enum class Shape : std::uint8_t { Empty, Minimal, TwoSection };

void put16(std::vector<std::byte>& b, std::size_t off, std::uint16_t v) {
    engine::assets::putU16(std::span<std::byte>(b), off, v);
}
void put32(std::vector<std::byte>& b, std::size_t off, std::uint32_t v) {
    engine::assets::putU32(std::span<std::byte>(b), off, v);
}
void put64(std::vector<std::byte>& b, std::size_t off, std::uint64_t v) {
    engine::assets::putU64(std::span<std::byte>(b), off, v);
}
void putVec3(std::vector<std::byte>& b, std::size_t off, float x, float y, float z) {
    engine::assets::putF32(std::span<std::byte>(b), off + 0, x);
    engine::assets::putF32(std::span<std::byte>(b), off + 4, y);
    engine::assets::putF32(std::span<std::byte>(b), off + 8, z);
}
void putVec2(std::vector<std::byte>& b, std::size_t off, float x, float y) {
    engine::assets::putF32(std::span<std::byte>(b), off + 0, x);
    engine::assets::putF32(std::span<std::byte>(b), off + 4, y);
}
void putMagic(std::vector<std::byte>& b) {
    for (std::size_t i = 0; i < engine::assets::COOKED_MESH_MAGIC.size(); ++i) {
        b[i] = static_cast<std::byte>(engine::assets::COOKED_MESH_MAGIC[i]);
    }
}

std::vector<std::byte> makeContainer(Shape shape) {
    if (shape == Shape::Empty) {
        std::vector<std::byte> b(96);
        putMagic(b);
        put32(b, H_FORMAT_VERSION, 1);
        put32(b, H_COOKER_VERSION, 1);
        put64(b, H_TOTAL_BYTES, 96);
        put64(b, H_INDEX_DATA_OFFSET, 96);
        return b;
    }
    if (shape == Shape::Minimal) {
        std::vector<std::byte> b(272);
        putMagic(b);
        put32(b, H_FORMAT_VERSION, 1);
        put32(b, H_COOKER_VERSION, 1);
        put32(b, H_SECTION_COUNT, 1);
        put32(b, H_SUBMESH_COUNT, 1);
        put32(b, H_INDEX_COUNT, 3);
        put32(b, H_INDEX_TYPE, 0);
        put32(b, H_ATTRIBUTE_COUNT, 1);
        put64(b, H_TOTAL_BYTES, 272);
        putVec3(b, H_BOUNDS_MIN, 0.0F, 0.0F, 0.0F);
        putVec3(b, H_BOUNDS_MAX, 1.0F, 1.0F, 0.0F);
        put64(b, H_INDEX_DATA_OFFSET, 256);
        // attribute 0 @96: Position, Float3, offset 0
        put16(b, 96 + A_SEMANTIC, 0);
        put16(b, 96 + A_FORMAT, 1);
        put32(b, 96 + A_OFFSET, 0);
        // section 0 @104
        put32(b, 104 + S_FIRST_ATTRIBUTE, 0);
        put32(b, 104 + S_ATTRIBUTE_COUNT, 1);
        put32(b, 104 + S_VERTEX_STRIDE, 12);
        put32(b, 104 + S_VERTEX_COUNT, 3);
        put64(b, 104 + S_VERTEX_DATA_OFFSET, 208);
        put64(b, 104 + S_VERTEX_DATA_BYTES, 36);
        // submesh 0 @136
        put32(b, 136 + M_SECTION_INDEX, 0);
        put32(b, 136 + M_FIRST_INDEX, 0);
        put32(b, 136 + M_INDEX_COUNT, 3);
        put32(b, 136 + M_MATERIAL, engine::assets::COOKED_INVALID_MATERIAL);
        put32(b, 136 + M_SOURCE_MESH, 0);
        put32(b, 136 + M_SOURCE_PRIMITIVE, 0);
        putVec3(b, 136 + M_BOUNDS_MIN, 0.0F, 0.0F, 0.0F);
        putVec3(b, 136 + M_BOUNDS_MAX, 1.0F, 1.0F, 0.0F);
        // vertices @208, indices @256
        putVec3(b, 208 + 0, 0.0F, 0.0F, 0.0F);
        putVec3(b, 208 + 12, 1.0F, 0.0F, 0.0F);
        putVec3(b, 208 + 24, 0.0F, 1.0F, 0.0F);
        put16(b, 256 + 0, 0);
        put16(b, 256 + 2, 1);
        put16(b, 256 + 4, 2);
        return b;
    }
    std::vector<std::byte> b(480);
    putMagic(b);
    put32(b, H_FORMAT_VERSION, 1);
    put32(b, H_COOKER_VERSION, 1);
    put64(b, H_GUID_HI, 0x0123456789ABCDEFULL);
    put64(b, H_GUID_LO, 0xFEDCBA9876543210ULL);
    put32(b, H_SECTION_COUNT, 2);
    put32(b, H_SUBMESH_COUNT, 2);
    put32(b, H_INDEX_COUNT, 6);
    put32(b, H_INDEX_TYPE, 0);
    put32(b, H_ATTRIBUTE_COUNT, 4);
    put64(b, H_TOTAL_BYTES, 480);
    putVec3(b, H_BOUNDS_MIN, 0.0F, 0.0F, 0.0F);
    putVec3(b, H_BOUNDS_MAX, 2.0F, 1.0F, 1.0F);
    put64(b, H_INDEX_DATA_OFFSET, 464);
    // attributes @96: section 0's one, then section 1's three in ascending semantic order.
    put16(b, 96 + A_SEMANTIC, 0);
    put16(b, 96 + A_FORMAT, 1);
    put32(b, 96 + A_OFFSET, 0);
    put16(b, 104 + A_SEMANTIC, 0);
    put16(b, 104 + A_FORMAT, 1);
    put32(b, 104 + A_OFFSET, 0);
    put16(b, 112 + A_SEMANTIC, 1);
    put16(b, 112 + A_FORMAT, 1);
    put32(b, 112 + A_OFFSET, 12);
    put16(b, 120 + A_SEMANTIC, 3);
    put16(b, 120 + A_FORMAT, 0);
    put32(b, 120 + A_OFFSET, 24);
    // section 0 @128, section 1 @160
    put32(b, 128 + S_FIRST_ATTRIBUTE, 0);
    put32(b, 128 + S_ATTRIBUTE_COUNT, 1);
    put32(b, 128 + S_VERTEX_STRIDE, 12);
    put32(b, 128 + S_VERTEX_COUNT, 3);
    put64(b, 128 + S_VERTEX_DATA_OFFSET, 320);
    put64(b, 128 + S_VERTEX_DATA_BYTES, 36);
    put32(b, 160 + S_FIRST_ATTRIBUTE, 1);
    put32(b, 160 + S_ATTRIBUTE_COUNT, 3);
    put32(b, 160 + S_VERTEX_STRIDE, 32);
    put32(b, 160 + S_VERTEX_COUNT, 3);
    put64(b, 160 + S_VERTEX_DATA_OFFSET, 368);
    put64(b, 160 + S_VERTEX_DATA_BYTES, 96);
    // submesh 0 @192, submesh 1 @256
    put32(b, 192 + M_SECTION_INDEX, 0);
    put32(b, 192 + M_FIRST_INDEX, 0);
    put32(b, 192 + M_INDEX_COUNT, 3);
    put32(b, 192 + M_MATERIAL, engine::assets::COOKED_INVALID_MATERIAL);
    putVec3(b, 192 + M_BOUNDS_MIN, 0.0F, 0.0F, 0.0F);
    putVec3(b, 192 + M_BOUNDS_MAX, 1.0F, 1.0F, 0.0F);
    put32(b, 256 + M_SECTION_INDEX, 1);
    put32(b, 256 + M_FIRST_INDEX, 3);
    put32(b, 256 + M_INDEX_COUNT, 3);
    put32(b, 256 + M_MATERIAL, 7);
    put32(b, 256 + M_SOURCE_MESH, 1);
    put32(b, 256 + M_SOURCE_PRIMITIVE, 0);
    putVec3(b, 256 + M_BOUNDS_MIN, 2.0F, 0.0F, 0.0F);
    putVec3(b, 256 + M_BOUNDS_MAX, 2.0F, 1.0F, 1.0F);
    // section 0's vertices @320 (stride 12), section 1's @368 (stride 32)
    putVec3(b, 320 + 0, 0.0F, 0.0F, 0.0F);
    putVec3(b, 320 + 12, 1.0F, 0.0F, 0.0F);
    putVec3(b, 320 + 24, 0.0F, 1.0F, 0.0F);
    const std::array<std::array<float, 3>, 3> positions = {std::array<float, 3>{2.0F, 0.0F, 0.0F},
                                                           std::array<float, 3>{2.0F, 1.0F, 0.0F},
                                                           std::array<float, 3>{2.0F, 0.0F, 1.0F}};
    const std::array<std::array<float, 2>, 3> uvs = {std::array<float, 2>{0.0F, 0.0F}, std::array<float, 2>{1.0F, 0.0F},
                                                     std::array<float, 2>{0.0F, 1.0F}};
    for (std::size_t v = 0; v < 3; ++v) {
        const std::size_t base = 368 + (v * 32);
        putVec3(b, base + 0, positions[v][0], positions[v][1], positions[v][2]);
        putVec3(b, base + 12, 1.0F, 0.0F, 0.0F);
        putVec2(b, base + 24, uvs[v][0], uvs[v][1]);
    }
    for (std::size_t i = 0; i < 6; ++i) {
        put16(b, 464 + (i * 2), static_cast<std::uint16_t>(i % 3));
    }
    return b;
}

// Where the one section/submesh of MINIMAL and the two of TWO_SECTION live, so a mutation case
// spells an intent rather than an arithmetic expression.
constexpr std::size_t MIN_SECTION0 = 104;
constexpr std::size_t MIN_SUBMESH0 = 136;
constexpr std::size_t TWO_SECTION1 = 160;

engine::assets::CookedMeshParseResult parse(const std::vector<std::byte>& b) {
    return engine::assets::parseCookedMesh(std::span<const std::byte>(b));
}

// splitmix64, the guid.cpp shape: a fixed-seed, allocation-free, platform-identical stream so CM40's
// 4096 buffers are the SAME 4096 buffers on every lane and in every run.
class Splitmix {
public:
    explicit constexpr Splitmix(std::uint64_t seed) noexcept : state(seed) {}
    [[nodiscard]] constexpr std::uint64_t next() noexcept {
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }

private:
    std::uint64_t state;
};

}  // namespace

TEST_CASE("cooked mesh: the four record sizes and the alignment are frozen (CM1)") {
    // static_assert, not CHECK: a change to any of these is a BUILD failure, which is the strongest
    // available statement about a number that is part of an on-disk format.
    static_assert(engine::assets::COOKED_MESH_HEADER_BYTES == 96);
    static_assert(engine::assets::COOKED_MESH_ATTRIBUTE_BYTES == 8);
    static_assert(engine::assets::COOKED_MESH_SECTION_BYTES == 32);
    static_assert(engine::assets::COOKED_MESH_SUBMESH_BYTES == 64);
    static_assert(engine::assets::COOKED_MESH_ALIGNMENT == 16);
    CHECK(engine::assets::COOKED_MESH_HEADER_BYTES == 96);
    CHECK(engine::assets::COOKED_MESH_ATTRIBUTE_BYTES == 8);
    CHECK(engine::assets::COOKED_MESH_SECTION_BYTES == 32);
    CHECK(engine::assets::COOKED_MESH_SUBMESH_BYTES == 64);
    CHECK(engine::assets::COOKED_MESH_ALIGNMENT == 16);
}

TEST_CASE("cooked mesh: the magic is eight ASCII bytes with no NUL, and the version is 1 (CM2)") {
    REQUIRE(engine::assets::COOKED_MESH_MAGIC.size() == 8);
    CHECK(engine::assets::COOKED_MESH_MAGIC == std::string_view{"AEROMESH"});
    for (const char c : engine::assets::COOKED_MESH_MAGIC) {
        CHECK(c != '\0');
        CHECK(static_cast<unsigned char>(c) < 0x80U);
    }
    CHECK(engine::assets::COOKED_MESH_FORMAT_VERSION == 1U);
    CHECK(engine::assets::COOKED_MESH_COOKER_VERSION == 1U);
    CHECK(engine::assets::COOKED_INVALID_MATERIAL == 0xFFFFFFFFU);
}

TEST_CASE("cooked mesh: cookedVertexFormatBytes is total and returns 8/12/16/16 (CM3)") {
    CHECK(cookedVertexFormatBytes(CookedVertexFormat::Float2) == 8U);
    CHECK(cookedVertexFormatBytes(CookedVertexFormat::Float3) == 12U);
    CHECK(cookedVertexFormatBytes(CookedVertexFormat::Float4) == 16U);
    CHECK(cookedVertexFormatBytes(CookedVertexFormat::Uint4) == 16U);
    // Every v1 format's size is a multiple of 4, which is what makes a packed stride a multiple of 4
    // without a single pad byte between attributes.
    for (const CookedVertexFormat f : {CookedVertexFormat::Float2, CookedVertexFormat::Float3,
                                       CookedVertexFormat::Float4, CookedVertexFormat::Uint4}) {
        CHECK(cookedVertexFormatBytes(f) % 4U == 0U);
        CHECK(cookedVertexFormatBytes(f) > 0U);
    }
}

TEST_CASE("cooked mesh: cookedIndexTypeBytes is total and returns 2/4 (CM4)") {
    CHECK(cookedIndexTypeBytes(CookedIndexType::Uint16) == 2U);
    CHECK(cookedIndexTypeBytes(CookedIndexType::Uint32) == 4U);
    CHECK(static_cast<std::uint32_t>(CookedIndexType::Uint16) == 0U);
    CHECK(static_cast<std::uint32_t>(CookedIndexType::Uint32) == 1U);
}

TEST_CASE("cooked mesh: cookedFormatForSemantic matches the frozen v1 table row for row (CM5)") {
    CHECK(cookedFormatForSemantic(CookedVertexSemantic::Position) == CookedVertexFormat::Float3);
    CHECK(cookedFormatForSemantic(CookedVertexSemantic::Normal) == CookedVertexFormat::Float3);
    CHECK(cookedFormatForSemantic(CookedVertexSemantic::Tangent) == CookedVertexFormat::Float4);
    CHECK(cookedFormatForSemantic(CookedVertexSemantic::TexCoord0) == CookedVertexFormat::Float2);
    CHECK(cookedFormatForSemantic(CookedVertexSemantic::TexCoord1) == CookedVertexFormat::Float2);
    CHECK(cookedFormatForSemantic(CookedVertexSemantic::Color0) == CookedVertexFormat::Float4);
    CHECK(cookedFormatForSemantic(CookedVertexSemantic::Joints0) == CookedVertexFormat::Uint4);
    CHECK(cookedFormatForSemantic(CookedVertexSemantic::Weights0) == CookedVertexFormat::Float4);

    // The all-attributes stride the layout builder produces: 12+12+16+8+8+16+16+16 == 104.
    std::uint32_t stride = 0;
    for (const CookedVertexSemantic s : ALL_SEMANTICS) {
        stride += cookedVertexFormatBytes(cookedFormatForSemantic(s));
    }
    CHECK(stride == 104U);
}

TEST_CASE("cooked mesh: cookedMeshStatusLabel is distinct and non-empty for every enumerator (CM6)") {
    std::vector<std::string_view> seen;
    for (const CookedMeshStatus s : ALL_STATUSES) {
        const std::string_view label = cookedMeshStatusLabel(s);
        CHECK_FALSE(label.empty());
        CHECK(label != std::string_view{"Unknown"});
        for (const std::string_view previous : seen) {
            CHECK(label != previous);
        }
        seen.push_back(label);
    }
    CHECK(seen.size() == 10U);
    CHECK(cookedMeshStatusLabel(CookedMeshStatus::Ok) == std::string_view{"Ok"});
}

TEST_CASE("cooked mesh: the byte primitives round-trip at runtime and are total (CM7)") {
    std::array<std::byte, 32> raw{};
    const std::span<std::byte> w(raw);
    const std::span<const std::byte> r(raw);

    SUBCASE("u16/u32/u64 battery") {
        for (const std::uint16_t v : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0x00FF}, std::uint16_t{0xFF00},
                                      std::numeric_limits<std::uint16_t>::max()}) {
            engine::assets::putU16(w, 3, v);
            CHECK(engine::assets::getU16(r, 3) == v);
        }
        for (const std::uint32_t v : {std::uint32_t{0}, std::uint32_t{1}, std::uint32_t{0x12345678},
                                      std::numeric_limits<std::uint32_t>::max()}) {
            engine::assets::putU32(w, 5, v);
            CHECK(engine::assets::getU32(r, 5) == v);
        }
        for (const std::uint64_t v : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x0123456789ABCDEF},
                                      std::numeric_limits<std::uint64_t>::max()}) {
            engine::assets::putU64(w, 9, v);
            CHECK(engine::assets::getU64(r, 9) == v);
        }
    }

    SUBCASE("float battery, compared by BITS so -0.0f and +0.0f are distinguishable") {
        const std::array<float, 8> values = {0.0F,
                                             -0.0F,
                                             1.0F,
                                             -1.0F,
                                             1e38F,
                                             std::numeric_limits<float>::denorm_min(),
                                             std::numeric_limits<float>::infinity(),
                                             -std::numeric_limits<float>::infinity()};
        for (const float v : values) {
            engine::assets::putF32(w, 12, v);
            CHECK(std::bit_cast<std::uint32_t>(engine::assets::getF32(r, 12)) == std::bit_cast<std::uint32_t>(v));
        }
        // -0.0f and +0.0f MUST differ on the wire; == would not see it.
        engine::assets::putF32(w, 12, -0.0F);
        const std::uint32_t negativeZero = engine::assets::getU32(r, 12);
        engine::assets::putF32(w, 12, 0.0F);
        CHECK(engine::assets::getU32(r, 12) != negativeZero);
    }

    SUBCASE("little-endian byte assembly is observable, not assumed") {
        engine::assets::putU32(w, 0, 0x12345678U);
        CHECK(raw[0] == std::byte{0x78});
        CHECK(raw[1] == std::byte{0x56});
        CHECK(raw[2] == std::byte{0x34});
        CHECK(raw[3] == std::byte{0x12});
        engine::assets::putU64(w, 8, 0x0123456789ABCDEFULL);
        CHECK(raw[8] == std::byte{0xEF});
        CHECK(raw[15] == std::byte{0x01});
    }

    SUBCASE("TOTAL: an out-of-range offset writes nothing and reads 0") {
        std::array<std::byte, 4> small{};
        const std::span<std::byte> sw(small);
        const std::span<const std::byte> sr(small);
        engine::assets::putU32(sw, 0, 0xAABBCCDDU);  // the one write that fits

        // Each of these must be a no-op: past the end, straddling the end, and offsets so large that
        // an `offset + length` formulation would WRAP.
        engine::assets::putU16(sw, 4, 0xFFFFU);
        engine::assets::putU16(sw, 3, 0xFFFFU);
        engine::assets::putU32(sw, 1, 0xFFFFFFFFU);
        engine::assets::putU64(sw, 0, 0xFFFFFFFFFFFFFFFFULL);
        engine::assets::putF32(sw, 2, 1.0F);
        engine::assets::putU16(sw, std::numeric_limits<std::size_t>::max(), 0xFFFFU);
        engine::assets::putU32(sw, std::numeric_limits<std::size_t>::max() - 2, 0xFFFFFFFFU);
        CHECK(engine::assets::getU32(sr, 0) == 0xAABBCCDDU);

        CHECK(engine::assets::getU16(sr, 4) == 0U);
        CHECK(engine::assets::getU16(sr, 3) == 0U);
        CHECK(engine::assets::getU32(sr, 1) == 0U);
        CHECK(engine::assets::getU64(sr, 0) == 0U);
        CHECK(std::bit_cast<std::uint32_t>(engine::assets::getF32(sr, 2)) == 0U);
        CHECK(engine::assets::getU16(sr, std::numeric_limits<std::size_t>::max()) == 0U);
        CHECK(engine::assets::getU32(sr, std::numeric_limits<std::size_t>::max() - 2) == 0U);
    }

    SUBCASE("an EMPTY span is total too") {
        const std::span<std::byte> none;
        const std::span<const std::byte> noneConst;
        engine::assets::putU32(none, 0, 0xFFFFFFFFU);
        CHECK(engine::assets::getU32(noneConst, 0) == 0U);
        CHECK(engine::assets::getU64(noneConst, 0) == 0U);
    }
}

TEST_CASE("cooked mesh: putF32/getF32 preserve a quiet and a signalling NaN bit for bit (CM8)") {
    // The cook never CREATES a NaN -- it does no floating-point arithmetic on vertex data at all --
    // but a tangent or a colour can legitimately carry one, and its bits must survive the round trip
    // unchanged. Compared through std::bit_cast, because NaN == NaN is false by definition.
    std::array<std::byte, 4> raw{};
    const std::span<std::byte> w(raw);
    const std::span<const std::byte> r(raw);

    const std::array<std::uint32_t, 4> patterns = {
        0x7FC00000U,  // canonical quiet NaN
        0xFFC00001U,  // negative quiet NaN with a payload
        0x7FA00000U,  // signalling NaN
        0x7F800001U,  // signalling NaN, minimal payload
    };
    for (const std::uint32_t bits : patterns) {
        engine::assets::putF32(w, 0, std::bit_cast<float>(bits));
        CHECK(engine::assets::getU32(r, 0) == bits);
        CHECK(std::bit_cast<std::uint32_t>(engine::assets::getF32(r, 0)) == bits);
    }
}

// =================================================================================================
// The parser. Every refusal arm below mutates exactly ONE field of a buffer that parses Ok, so what
// the case proves is that field's check and nothing else.
// =================================================================================================

TEST_CASE("cooked mesh: a hand-built empty container parses Ok with every field zero (CM9)") {
    const std::vector<std::byte> b = makeContainer(Shape::Empty);
    const auto r = parse(b);
    REQUIRE(r.status == CookedMeshStatus::Ok);
    CHECK(r.message.empty());
    CHECK(r.mesh.formatVersion == 1U);
    CHECK(r.mesh.cookerVersion == 1U);
    CHECK_FALSE(r.mesh.sourceGuid.valid());
    CHECK(r.mesh.indexType == CookedIndexType::Uint16);
    CHECK(r.mesh.indexCount == 0U);
    CHECK(r.mesh.attributes.empty());
    CHECK(r.mesh.sections.empty());
    CHECK(r.mesh.submeshes.empty());
    CHECK(r.mesh.indexDataOffset == 96U);
    CHECK(r.mesh.bounds.min == engine::Vec3{0.0F, 0.0F, 0.0F});
    CHECK(r.mesh.bounds.max == engine::Vec3{0.0F, 0.0F, 0.0F});
    CHECK(engine::assets::indexBytes(r.mesh).empty());
}

TEST_CASE("cooked mesh: a hand-built minimal container reads back field for field (CM10)") {
    const std::vector<std::byte> b = makeContainer(Shape::Minimal);
    const auto r = parse(b);
    REQUIRE(r.status == CookedMeshStatus::Ok);
    CHECK(r.mesh.formatVersion == 1U);
    CHECK(r.mesh.cookerVersion == 1U);
    CHECK(r.mesh.indexType == CookedIndexType::Uint16);
    CHECK(r.mesh.indexCount == 3U);
    CHECK(r.mesh.indexDataOffset == 256U);
    CHECK(r.mesh.bounds.max == engine::Vec3{1.0F, 1.0F, 0.0F});

    REQUIRE(r.mesh.attributes.size() == 1U);
    CHECK(r.mesh.attributes[0].semantic == CookedVertexSemantic::Position);
    CHECK(r.mesh.attributes[0].format == CookedVertexFormat::Float3);
    CHECK(r.mesh.attributes[0].offset == 0U);

    REQUIRE(r.mesh.sections.size() == 1U);
    CHECK(r.mesh.sections[0].firstAttribute == 0U);
    CHECK(r.mesh.sections[0].attributeCount == 1U);
    CHECK(r.mesh.sections[0].vertexStride == 12U);
    CHECK(r.mesh.sections[0].vertexCount == 3U);
    CHECK(r.mesh.sections[0].vertexDataOffset == 208U);
    CHECK(r.mesh.sections[0].vertexDataBytes == 36U);

    REQUIRE(r.mesh.submeshes.size() == 1U);
    CHECK(r.mesh.submeshes[0].sectionIndex == 0U);
    CHECK(r.mesh.submeshes[0].firstIndex == 0U);
    CHECK(r.mesh.submeshes[0].indexCount == 3U);
    CHECK(r.mesh.submeshes[0].materialIndex == engine::assets::COOKED_INVALID_MATERIAL);
    CHECK(r.mesh.submeshes[0].sourceMeshIndex == 0U);
    CHECK(r.mesh.submeshes[0].sourcePrimitiveIndex == 0U);
    CHECK(r.mesh.submeshes[0].bounds.max == engine::Vec3{1.0F, 1.0F, 0.0F});

    // AC-4, restated to the two STORED offsets (C1): both are multiples of 16, while the section
    // table legitimately begins at 104 and the submesh table at 136 because attributeCount is odd.
    CHECK(r.mesh.sections[0].vertexDataOffset % 16U == 0U);
    CHECK(r.mesh.indexDataOffset % 16U == 0U);
}

TEST_CASE("cooked mesh: a two-section container reads back both regions (CM11)") {
    const std::vector<std::byte> b = makeContainer(Shape::TwoSection);
    const auto r = parse(b);
    REQUIRE(r.status == CookedMeshStatus::Ok);
    CHECK(r.mesh.sourceGuid.hi == 0x0123456789ABCDEFULL);
    CHECK(r.mesh.sourceGuid.lo == 0xFEDCBA9876543210ULL);
    REQUIRE(r.mesh.sections.size() == 2U);
    REQUIRE(r.mesh.submeshes.size() == 2U);
    REQUIRE(r.mesh.attributes.size() == 4U);

    const auto s0 = engine::assets::sectionVertexBytes(r.mesh, 0);
    const auto s1 = engine::assets::sectionVertexBytes(r.mesh, 1);
    REQUIRE(s0.size() == 36U);
    REQUIRE(s1.size() == 96U);
    // Section 0's first vertex is (0,0,0); section 1's first is (2,0,0) with normal (1,0,0).
    CHECK(engine::assets::getF32(s0, 0) == 0.0F);
    CHECK(engine::assets::getF32(s0, 12) == 1.0F);
    CHECK(engine::assets::getF32(s1, 0) == 2.0F);
    CHECK(engine::assets::getF32(s1, 12) == 1.0F);
    CHECK(engine::assets::getF32(s1, 24) == 0.0F);
    // Section 1's layout: Position(0) Normal(12) TexCoord0(24), stride 32.
    CHECK(r.mesh.sections[1].vertexStride == 32U);
    CHECK(r.mesh.attributes[1].offset == 0U);
    CHECK(r.mesh.attributes[2].offset == 12U);
    CHECK(r.mesh.attributes[3].offset == 24U);
    CHECK(r.mesh.submeshes[1].firstIndex == 3U);
    CHECK(r.mesh.submeshes[1].materialIndex == 7U);
    CHECK(r.mesh.submeshes[1].sourceMeshIndex == 1U);
}

TEST_CASE("cooked mesh: the accessors return exact ranges and COPY NOTHING (CM12)") {
    const std::vector<std::byte> b = makeContainer(Shape::TwoSection);
    const auto r = parse(b);
    REQUIRE(r.status == CookedMeshStatus::Ok);

    const auto idx = engine::assets::indexBytes(r.mesh);
    CHECK(idx.size() == 6U * 2U);  // indexCount x cookedIndexTypeBytes(Uint16)
    CHECK(engine::assets::getU16(idx, 0) == 0U);
    CHECK(engine::assets::getU16(idx, 6) == 0U);
    CHECK(engine::assets::getU16(idx, 10) == 2U);

    // Compared by data(), not by value: a copy would pass a value comparison and fail this.
    CHECK(idx.data() == b.data() + 464);
    CHECK(engine::assets::sectionVertexBytes(r.mesh, 0).data() == b.data() + 320);
    CHECK(engine::assets::sectionVertexBytes(r.mesh, 1).data() == b.data() + 368);
    CHECK(r.mesh.bytes.data() == b.data());
    CHECK(r.mesh.bytes.size() == b.size());
}

TEST_CASE("cooked mesh: an out-of-range sectionIndex returns an EMPTY span, never a read (CM13)") {
    const std::vector<std::byte> b = makeContainer(Shape::Minimal);
    const auto r = parse(b);
    REQUIRE(r.status == CookedMeshStatus::Ok);
    CHECK(engine::assets::sectionVertexBytes(r.mesh, 1).empty());
    CHECK(engine::assets::sectionVertexBytes(r.mesh, 0xFFFFFFFFU).empty());
    // And on a HAND-CONSTRUCTED mesh the parser never saw, which is what makes the accessors' own
    // fits() re-check load-bearing rather than decorative.
    engine::assets::CookedMesh forged;
    forged.sections.push_back(engine::assets::CookedSection{0, 1, 12, 3, 1024, 36});
    forged.bytes = std::span<const std::byte>(b);
    CHECK(engine::assets::sectionVertexBytes(forged, 0).empty());
    forged.indexCount = 1000000;
    forged.indexDataOffset = 256;
    CHECK(engine::assets::indexBytes(forged).empty());
}

TEST_CASE("cooked mesh: the 96-byte boundary, from both sides (CM14)") {
    for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{95}}) {
        const std::vector<std::byte> b(n);
        const auto r = engine::assets::parseCookedMesh(std::span<const std::byte>(b));
        CHECK(r.status == CookedMeshStatus::TooSmall);
        CHECK_FALSE(r.message.empty());
    }
    const std::vector<std::byte> ok = makeContainer(Shape::Empty);
    REQUIRE(ok.size() == 96U);
    CHECK(parse(ok).status == CookedMeshStatus::Ok);
}

TEST_CASE("cooked mesh: a buffer over the byte cap is refused before anything is interpreted (CM15)") {
    // A span over a FAKE size rather than a real 2 GB allocation: the parser reaches its size check
    // before it touches a byte, so nothing is ever read through this span. The 96 real bytes behind
    // it are a valid container, which is the point -- the refusal is the size, not the content.
    const std::vector<std::byte> real = makeContainer(Shape::Empty);
    const std::span<const std::byte> oversized(real.data(),
                                               static_cast<std::size_t>(engine::assets::MAX_COOKED_MESH_BYTES) + 1U);
    const auto r = engine::assets::parseCookedMesh(oversized);
    CHECK(r.status == CookedMeshStatus::CapExceeded);
    CHECK_FALSE(r.message.empty());
}

TEST_CASE("cooked mesh: a flipped magic byte in any of the eight positions is BadMagic (CM16)") {
    for (std::size_t i = 0; i < 8; ++i) {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        b[i] = b[i] ^ std::byte{0x01};
        CHECK(parse(b).status == CookedMeshStatus::BadMagic);
    }
}

TEST_CASE("cooked mesh: any formatVersion but 1 is UnsupportedVersion (CM17)") {
    for (const std::uint32_t v : {std::uint32_t{0}, std::uint32_t{2}, std::numeric_limits<std::uint32_t>::max()}) {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put32(b, H_FORMAT_VERSION, v);
        CHECK(parse(b).status == CookedMeshStatus::UnsupportedVersion);
    }
}

TEST_CASE("cooked mesh: a non-zero header flags field is a REFUSAL (CM18)") {
    std::vector<std::byte> b = makeContainer(Shape::Minimal);
    put32(b, H_FLAGS, 1);
    CHECK(parse(b).status == CookedMeshStatus::ReservedNotZero);
    put32(b, H_FLAGS, 0x80000000U);
    CHECK(parse(b).status == CookedMeshStatus::ReservedNotZero);
}

TEST_CASE("cooked mesh: either non-zero submesh reserved field is a REFUSAL (CM19)") {
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put64(b, MIN_SUBMESH0 + M_RESERVED0, 1);
        CHECK(parse(b).status == CookedMeshStatus::ReservedNotZero);
    }
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put64(b, MIN_SUBMESH0 + M_RESERVED1, 0x8000000000000000ULL);
        CHECK(parse(b).status == CookedMeshStatus::ReservedNotZero);
    }
}

TEST_CASE("cooked mesh: totalBytes must equal the buffer's own size, both directions (CM20)") {
    for (const std::uint64_t n : {std::uint64_t{271}, std::uint64_t{273}}) {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put64(b, H_TOTAL_BYTES, n);
        CHECK(parse(b).status == CookedMeshStatus::SizeMismatch);
    }
}

TEST_CASE("cooked mesh: each of the four header counts one above its cap is CapExceeded (CM21)") {
    struct Arm {
        std::size_t offset;
        std::uint32_t value;
    };
    const std::array<Arm, 4> arms = {Arm{H_ATTRIBUTE_COUNT, engine::assets::MAX_COOKED_ATTRIBUTES + 1},
                                     Arm{H_SECTION_COUNT, engine::assets::MAX_COOKED_SECTIONS + 1},
                                     Arm{H_SUBMESH_COUNT, engine::assets::MAX_COOKED_SUBMESHES + 1},
                                     Arm{H_INDEX_COUNT, engine::assets::MAX_COOKED_INDICES + 1}};
    for (const Arm& arm : arms) {
        std::vector<std::byte> b = makeContainer(Shape::Empty);
        put32(b, arm.offset, arm.value);
        CHECK(parse(b).status == CookedMeshStatus::CapExceeded);
    }
}

TEST_CASE("cooked mesh: an index type code outside {0,1} is BadTable (CM22)") {
    for (const std::uint32_t code : {std::uint32_t{2}, std::numeric_limits<std::uint32_t>::max()}) {
        std::vector<std::byte> b = makeContainer(Shape::Empty);
        put32(b, H_INDEX_TYPE, code);
        CHECK(parse(b).status == CookedMeshStatus::BadTable);
    }
}

TEST_CASE("cooked mesh: a table that does not fit is refused BEFORE anything is reserved (CM23)") {
    // submeshCount is at the cap, NOT over it, so the cap check in step 6 passes and step 7's table
    // arithmetic is what refuses -- against a 96-byte buffer. The proof that nothing was allocated
    // for 65536 submeshes is structural (step 7 precedes every reserve) and observable: this case
    // runs in microseconds under ASan rather than reserving 4 MB.
    std::vector<std::byte> b = makeContainer(Shape::Empty);
    put32(b, H_SUBMESH_COUNT, engine::assets::MAX_COOKED_SUBMESHES);
    CHECK(parse(b).status == CookedMeshStatus::BadTable);
    // The same, one table down: an attribute count at its cap against the same 96 bytes.
    std::vector<std::byte> c = makeContainer(Shape::Empty);
    put32(c, H_ATTRIBUTE_COUNT, engine::assets::MAX_COOKED_ATTRIBUTES);
    CHECK(parse(c).status == CookedMeshStatus::BadTable);
}

TEST_CASE("cooked mesh: a section's attribute slice must lie inside the attribute table (CM24)") {
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put32(b, MIN_SECTION0 + S_ATTRIBUTE_COUNT, 2);  // the table holds one
        CHECK(parse(b).status == CookedMeshStatus::BadTable);
    }
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put32(b, MIN_SECTION0 + S_FIRST_ATTRIBUTE, 1);  // [1, 2) of a table of 1
        CHECK(parse(b).status == CookedMeshStatus::BadTable);
    }
}

TEST_CASE("cooked mesh: vertexDataBytes must equal vertexCount x stride, both directions (CM25)") {
    for (const std::uint64_t n : {std::uint64_t{35}, std::uint64_t{37}}) {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put64(b, MIN_SECTION0 + S_VERTEX_DATA_BYTES, n);
        CHECK(parse(b).status == CookedMeshStatus::BadRange);
    }
}

TEST_CASE("cooked mesh: a vertex region that is not 16-aligned is BadRange (CM26)") {
    std::vector<std::byte> b = makeContainer(Shape::Minimal);
    put64(b, MIN_SECTION0 + S_VERTEX_DATA_OFFSET, 8);
    CHECK(parse(b).status == CookedMeshStatus::BadRange);
}

TEST_CASE("cooked mesh: a vertex region that does not fit is BadRange (CM27)") {
    std::vector<std::byte> b = makeContainer(Shape::Minimal);
    put64(b, MIN_SECTION0 + S_VERTEX_DATA_OFFSET, 256);  // 256 + 36 > 272
    CHECK(parse(b).status == CookedMeshStatus::BadRange);
}

TEST_CASE("cooked mesh: an attribute outside its section's stride is BadLayout (CM28)") {
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put32(b, 96 + A_OFFSET, 4);  // 4 + 12 > 12
        CHECK(parse(b).status == CookedMeshStatus::BadLayout);
    }
    {
        // A width WIDER than the whole stride -- the `width > stride` half of the same check.
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put16(b, 96 + A_FORMAT, static_cast<std::uint16_t>(CookedVertexFormat::Float4));
        CHECK(parse(b).status == CookedMeshStatus::BadLayout);
    }
}

TEST_CASE("cooked mesh: a duplicated semantic within one section is BadLayout (CM29)") {
    std::vector<std::byte> b = makeContainer(Shape::TwoSection);
    put16(b, 112 + A_SEMANTIC, 0);  // section 1's Normal becomes a second Position
    CHECK(parse(b).status == CookedMeshStatus::BadLayout);
}

TEST_CASE("cooked mesh: an unknown semantic or format code is BadLayout (CM30)") {
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put16(b, 96 + A_SEMANTIC, engine::assets::COOKED_SEMANTIC_COUNT);
        CHECK(parse(b).status == CookedMeshStatus::BadLayout);
    }
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put16(b, 96 + A_FORMAT, 4);
        CHECK(parse(b).status == CookedMeshStatus::BadLayout);
    }
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put16(b, 96 + A_SEMANTIC, std::numeric_limits<std::uint16_t>::max());
        CHECK(parse(b).status == CookedMeshStatus::BadLayout);
    }
}

TEST_CASE("cooked mesh: the crafted-offset battery is refused, never wrapped (CM31)") {
    // Every one of these would be ACCEPTED by a range check written as `offset + length > size`,
    // because the addition wraps. Written as subtraction, each is refused and nothing is read --
    // which ASan/UBSan are the second half of the proof for.
    constexpr std::uint64_t U64_MAX = std::numeric_limits<std::uint64_t>::max();
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put64(b, MIN_SECTION0 + S_VERTEX_DATA_OFFSET, U64_MAX);
        CHECK(parse(b).status == CookedMeshStatus::BadRange);
    }
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put64(b, MIN_SECTION0 + S_VERTEX_DATA_OFFSET, U64_MAX - 15);  // 16-ALIGNED, so the fit is the check
        CHECK(parse(b).status == CookedMeshStatus::BadRange);
    }
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put64(b, MIN_SECTION0 + S_VERTEX_DATA_BYTES, U64_MAX);
        CHECK(parse(b).status == CookedMeshStatus::BadRange);
    }
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put64(b, H_INDEX_DATA_OFFSET, U64_MAX - 15);
        CHECK(parse(b).status == CookedMeshStatus::BadRange);
    }
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put32(b, MIN_SUBMESH0 + M_FIRST_INDEX, std::numeric_limits<std::uint32_t>::max());
        put32(b, MIN_SUBMESH0 + M_INDEX_COUNT, 1);
        CHECK(parse(b).status == CookedMeshStatus::BadRange);
    }
}

TEST_CASE("cooked mesh: a submesh naming a section that does not exist is BadRange (CM32)") {
    std::vector<std::byte> b = makeContainer(Shape::Minimal);
    put32(b, MIN_SUBMESH0 + M_SECTION_INDEX, 1);  // sectionCount is 1, so index 1 is one past
    CHECK(parse(b).status == CookedMeshStatus::BadRange);
}

TEST_CASE("cooked mesh: a submesh index range one past the header's indexCount is BadRange (CM33)") {
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put32(b, MIN_SUBMESH0 + M_FIRST_INDEX, 1);  // [1, 4) of 3
        CHECK(parse(b).status == CookedMeshStatus::BadRange);
    }
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put32(b, MIN_SUBMESH0 + M_INDEX_COUNT, 4);
        CHECK(parse(b).status == CookedMeshStatus::BadRange);
    }
}

TEST_CASE("cooked mesh: the index region is alignment- and range-checked (CM34)") {
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put64(b, H_INDEX_DATA_OFFSET, 8);
        CHECK(parse(b).status == CookedMeshStatus::BadRange);
    }
    {
        std::vector<std::byte> b = makeContainer(Shape::Minimal);
        put64(b, H_INDEX_DATA_OFFSET, 272);  // 272 + 6 > 272
        CHECK(parse(b).status == CookedMeshStatus::BadRange);
    }
}

TEST_CASE("cooked mesh: a section declaring zero attributes is BadLayout (CM35)") {
    std::vector<std::byte> b = makeContainer(Shape::Minimal);
    put32(b, MIN_SECTION0 + S_ATTRIBUTE_COUNT, 0);
    CHECK(parse(b).status == CookedMeshStatus::BadLayout);
}

TEST_CASE("cooked mesh: a zero vertex stride is BadLayout (CM36)") {
    // THE HOLE THIS CLOSES: with stride 0, vertexDataBytes == vertexCount * 0 == 0 satisfies the
    // consistency check for ANY vertexCount, so without this refusal a header could claim four
    // billion vertices backed by zero bytes and be accepted -- and every consumer then divides by it.
    std::vector<std::byte> b = makeContainer(Shape::Minimal);
    put32(b, MIN_SECTION0 + S_VERTEX_STRIDE, 0);
    put64(b, MIN_SECTION0 + S_VERTEX_DATA_BYTES, 0);
    put32(b, MIN_SECTION0 + S_VERTEX_COUNT, std::numeric_limits<std::uint32_t>::max());
    CHECK(parse(b).status == CookedMeshStatus::BadLayout);
}

TEST_CASE("cooked mesh: a vertex stride that is not a multiple of four is BadLayout (CM37)") {
    std::vector<std::byte> b = makeContainer(Shape::Minimal);
    put32(b, MIN_SECTION0 + S_VERTEX_STRIDE, 13);
    put64(b, MIN_SECTION0 + S_VERTEX_DATA_BYTES, 39);
    CHECK(parse(b).status == CookedMeshStatus::BadLayout);
}

TEST_CASE("cooked mesh: a non-zero PADDING byte still parses Ok (CM38)") {
    // The writer always zeroes padding; the reader deliberately does not care. That asymmetry is
    // what stops a future writer that pads differently from being locked out (E12).
    std::vector<std::byte> b = makeContainer(Shape::Minimal);
    b[271] = std::byte{0xFF};  // the last tail-pad byte, past the index region's end at 262
    CHECK(parse(b).status == CookedMeshStatus::Ok);
    b[250] = std::byte{0x7F};  // the gap between the vertex region's end (244) and the index at 256
    CHECK(parse(b).status == CookedMeshStatus::Ok);
}

TEST_CASE("cooked mesh: message is empty if and only if the status is Ok (CM39)") {
    struct Arm {
        Shape shape;
        std::size_t offset;
        std::uint64_t value;
        int width;  // 2, 4 or 8
        CookedMeshStatus expected;
    };
    const std::array<Arm, 12> arms = {
        Arm{Shape::Minimal, H_FORMAT_VERSION, 2, 4, CookedMeshStatus::UnsupportedVersion},
        Arm{Shape::Minimal, H_FLAGS, 1, 4, CookedMeshStatus::ReservedNotZero},
        Arm{Shape::Minimal, H_TOTAL_BYTES, 999, 8, CookedMeshStatus::SizeMismatch},
        Arm{Shape::Empty, H_SECTION_COUNT, engine::assets::MAX_COOKED_SECTIONS + 1, 4, CookedMeshStatus::CapExceeded},
        Arm{Shape::Empty, H_INDEX_TYPE, 9, 4, CookedMeshStatus::BadTable},
        Arm{Shape::Empty, H_SUBMESH_COUNT, engine::assets::MAX_COOKED_SUBMESHES, 4, CookedMeshStatus::BadTable},
        Arm{Shape::Minimal, MIN_SECTION0 + S_ATTRIBUTE_COUNT, 0, 4, CookedMeshStatus::BadLayout},
        Arm{Shape::Minimal, MIN_SECTION0 + S_VERTEX_STRIDE, 0, 4, CookedMeshStatus::BadLayout},
        Arm{Shape::Minimal, MIN_SECTION0 + S_VERTEX_DATA_OFFSET, 8, 8, CookedMeshStatus::BadRange},
        Arm{Shape::Minimal, MIN_SUBMESH0 + M_SECTION_INDEX, 5, 4, CookedMeshStatus::BadRange},
        Arm{Shape::Minimal, MIN_SUBMESH0 + M_RESERVED0, 1, 8, CookedMeshStatus::ReservedNotZero},
        Arm{Shape::Minimal, 96 + A_SEMANTIC, 8, 2, CookedMeshStatus::BadLayout},
    };
    for (const Arm& arm : arms) {
        std::vector<std::byte> b = makeContainer(arm.shape);
        if (arm.width == 2) {
            put16(b, arm.offset, static_cast<std::uint16_t>(arm.value));
        } else if (arm.width == 4) {
            put32(b, arm.offset, static_cast<std::uint32_t>(arm.value));
        } else {
            put64(b, arm.offset, arm.value);
        }
        const auto r = parse(b);
        CHECK(r.status == arm.expected);
        CHECK_FALSE(r.message.empty());
    }
    // The other half of the "if and only if": each of the three valid shapes is Ok with no message.
    for (const Shape shape : {Shape::Empty, Shape::Minimal, Shape::TwoSection}) {
        const std::vector<std::byte> b = makeContainer(shape);
        const auto r = parse(b);
        CHECK(r.status == CookedMeshStatus::Ok);
        CHECK(r.message.empty());
    }
}

TEST_CASE("cooked mesh: 4096 pseudo-random buffers always return a status and never throw (CM40)") {
    // The fuzz-shaped totality check. A fixed seed, so this is the same 4096 buffers on every lane
    // and in every run: a failure here is reproducible, not a lottery ticket.
    Splitmix rng(0x3311ULL);
    std::size_t okCount = 0;
    for (int iteration = 0; iteration < 4096; ++iteration) {
        const auto length = static_cast<std::size_t>(rng.next() % 400U);
        std::vector<std::byte> b(length);
        for (std::size_t i = 0; i < length; ++i) {
            b[i] = static_cast<std::byte>(rng.next() & 0xFFU);
        }
        // Half the buffers get a valid magic and version, so the walk reaches past step 3 instead of
        // bouncing off BadMagic 4095 times out of 4096.
        if (length >= 96 && (iteration % 2) == 0) {
            putMagic(b);
            put32(b, H_FORMAT_VERSION, 1);
        }
        const auto r = parse(b);
        if (r.status == CookedMeshStatus::Ok) {
            ++okCount;
            CHECK(r.message.empty());
        } else {
            CHECK_FALSE(r.message.empty());
        }
        CHECK_FALSE(cookedMeshStatusLabel(r.status).empty());
    }
    // Random bytes essentially never form a valid container; the assertion is that the sweep RAN,
    // not that it produced a particular mix.
    CHECK(okCount <= 4096U);
}

// =================================================================================================
// The three frozen byte goldens, read back through the parser. The derived-fact checks and the byte
// comparison are BOTH kept: the derived checks say WHICH rule broke, the byte comparison says that
// SOMETHING did.
// =================================================================================================

namespace {

std::vector<std::byte> asBytes(std::span<const std::uint8_t> golden) {
    std::vector<std::byte> out;
    out.reserve(golden.size());
    for (const std::uint8_t v : golden) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

}  // namespace

TEST_CASE("cooked mesh: COOKED_GOLDEN_EMPTY parses Ok and decodes to its documented fields (CM41)") {
    const std::vector<std::byte> b = asBytes(aero_test::COOKED_GOLDEN_EMPTY);
    REQUIRE(b.size() == 96U);
    const auto r = parse(b);
    REQUIRE(r.status == CookedMeshStatus::Ok);
    CHECK(r.mesh.formatVersion == 1U);
    CHECK(r.mesh.cookerVersion == 1U);
    CHECK(r.mesh.sourceGuid.hi == 0U);
    CHECK(r.mesh.sourceGuid.lo == 0U);
    CHECK(r.mesh.indexType == CookedIndexType::Uint16);
    CHECK(r.mesh.indexCount == 0U);
    CHECK(r.mesh.attributes.empty());
    CHECK(r.mesh.sections.empty());
    CHECK(r.mesh.submeshes.empty());
    CHECK(r.mesh.indexDataOffset == 96U);
    // A POINT box at the origin, not the inverted sentinel: this is the byte-level statement of it.
    CHECK(r.mesh.bounds.min == engine::Vec3{0.0F, 0.0F, 0.0F});
    CHECK(r.mesh.bounds.max == engine::Vec3{0.0F, 0.0F, 0.0F});
}

TEST_CASE("cooked mesh: COOKED_GOLDEN_TRIANGLE parses Ok and decodes to its documented fields (CM42)") {
    const std::vector<std::byte> b = asBytes(aero_test::COOKED_GOLDEN_TRIANGLE);
    REQUIRE(b.size() == 272U);
    const auto r = parse(b);
    REQUIRE(r.status == CookedMeshStatus::Ok);
    CHECK(r.mesh.sourceGuid.hi == 0U);
    CHECK(r.mesh.indexCount == 3U);
    CHECK(r.mesh.indexType == CookedIndexType::Uint16);
    CHECK(r.mesh.indexDataOffset == 256U);
    CHECK(r.mesh.bounds.min == engine::Vec3{0.0F, 0.0F, 0.0F});
    CHECK(r.mesh.bounds.max == engine::Vec3{1.0F, 1.0F, 0.0F});
    REQUIRE(r.mesh.attributes.size() == 1U);
    CHECK(r.mesh.attributes[0].semantic == CookedVertexSemantic::Position);
    CHECK(r.mesh.attributes[0].format == CookedVertexFormat::Float3);
    CHECK(r.mesh.attributes[0].offset == 0U);
    REQUIRE(r.mesh.sections.size() == 1U);
    CHECK(r.mesh.sections[0].vertexStride == 12U);
    CHECK(r.mesh.sections[0].vertexCount == 3U);
    CHECK(r.mesh.sections[0].vertexDataOffset == 208U);
    CHECK(r.mesh.sections[0].vertexDataBytes == 36U);
    REQUIRE(r.mesh.submeshes.size() == 1U);
    CHECK(r.mesh.submeshes[0].materialIndex == engine::assets::COOKED_INVALID_MATERIAL);
    CHECK(r.mesh.submeshes[0].bounds.max == engine::Vec3{1.0F, 1.0F, 0.0F});
    // C1 IN BYTES: attributeCount is ODD, so the section table starts at 104 and the submesh table
    // at 136, and NEITHER is 16-aligned. Padding either to 16 would change this golden's size.
    CHECK(96U + (8U * r.mesh.attributes.size()) == 104U);
    CHECK(104U + (32U * r.mesh.sections.size()) == 136U);
    const auto idx = engine::assets::indexBytes(r.mesh);
    REQUIRE(idx.size() == 6U);
    CHECK(engine::assets::getU16(idx, 0) == 0U);
    CHECK(engine::assets::getU16(idx, 2) == 1U);
    CHECK(engine::assets::getU16(idx, 4) == 2U);
}

TEST_CASE("cooked mesh: COOKED_GOLDEN_MIXED parses Ok and pins the ordering rules (CM43)") {
    const std::vector<std::byte> b = asBytes(aero_test::COOKED_GOLDEN_MIXED);
    REQUIRE(b.size() == 480U);
    const auto r = parse(b);
    REQUIRE(r.status == CookedMeshStatus::Ok);
    CHECK(r.mesh.sourceGuid.hi == 0x0123456789ABCDEFULL);
    CHECK(r.mesh.sourceGuid.lo == 0xFEDCBA9876543210ULL);
    CHECK(r.mesh.indexCount == 6U);
    CHECK(r.mesh.indexDataOffset == 464U);
    CHECK(r.mesh.bounds.max == engine::Vec3{2.0F, 1.0F, 1.0F});
    REQUIRE(r.mesh.attributes.size() == 4U);
    REQUIRE(r.mesh.sections.size() == 2U);
    REQUIRE(r.mesh.submeshes.size() == 2U);
    // ASCENDING MASK, not input order: the position-only primitive was supplied SECOND and is
    // section 0; the three-attribute one was supplied FIRST and is section 1.
    CHECK(r.mesh.sections[0].vertexStride == 12U);
    CHECK(r.mesh.sections[0].vertexDataOffset == 320U);
    CHECK(r.mesh.sections[0].vertexDataBytes == 36U);
    CHECK(r.mesh.sections[1].firstAttribute == 1U);
    CHECK(r.mesh.sections[1].attributeCount == 3U);
    CHECK(r.mesh.sections[1].vertexStride == 32U);
    CHECK(r.mesh.sections[1].vertexDataOffset == 368U);
    CHECK(r.mesh.sections[1].vertexDataBytes == 96U);
    CHECK(r.mesh.submeshes[0].sourceMeshIndex == 0U);
    CHECK(r.mesh.submeshes[0].materialIndex == engine::assets::COOKED_INVALID_MATERIAL);
    CHECK(r.mesh.submeshes[1].sourceMeshIndex == 1U);
    CHECK(r.mesh.submeshes[1].firstIndex == 3U);
    CHECK(r.mesh.submeshes[1].materialIndex == 7U);
    CHECK(r.mesh.submeshes[1].bounds.min == engine::Vec3{2.0F, 0.0F, 0.0F});
    CHECK(r.mesh.submeshes[1].bounds.max == engine::Vec3{2.0F, 1.0F, 1.0F});
    // Section 1's region ends EXACTLY on the index region's start, so padding is COMPUTED rather
    // than unconditional: 368 + 96 == 464.
    CHECK(r.mesh.sections[1].vertexDataOffset + r.mesh.sections[1].vertexDataBytes == r.mesh.indexDataOffset);
}

TEST_CASE("cooked mesh: every STORED offset is 16-aligned and every gap is zero (CM44)") {
    // AC-4 as this plan restates it (C1): the rule governs the two offsets the format actually
    // STORES -- CookedSection::vertexDataOffset and the header's indexDataOffset -- plus the
    // zero-padding rule. The three tables are packed at implicit 8-byte-aligned starts the reader
    // derives, which is why Golden B's section table legitimately begins at 104.
    struct Golden {
        const char* name;
        std::vector<std::byte> bytes;
    };
    std::vector<Golden> goldens;
    goldens.push_back({"empty", asBytes(aero_test::COOKED_GOLDEN_EMPTY)});
    goldens.push_back({"triangle", asBytes(aero_test::COOKED_GOLDEN_TRIANGLE)});
    goldens.push_back({"mixed", asBytes(aero_test::COOKED_GOLDEN_MIXED)});

    for (const Golden& g : goldens) {
        CAPTURE(g.name);
        const auto r = parse(g.bytes);
        REQUIRE(r.status == CookedMeshStatus::Ok);
        CHECK(r.mesh.indexDataOffset % 16U == 0U);
        std::uint64_t cursor =
            96U + (8U * r.mesh.attributes.size()) + (32U * r.mesh.sections.size()) + (64U * r.mesh.submeshes.size());
        for (const auto& s : r.mesh.sections) {
            CHECK(s.vertexDataOffset % 16U == 0U);
            CHECK(s.vertexDataOffset >= cursor);
            for (std::uint64_t i = cursor; i < s.vertexDataOffset; ++i) {
                CHECK(g.bytes[static_cast<std::size_t>(i)] == std::byte{0});
            }
            cursor = s.vertexDataOffset + s.vertexDataBytes;
        }
        CHECK(r.mesh.indexDataOffset >= cursor);
        for (std::uint64_t i = cursor; i < r.mesh.indexDataOffset; ++i) {
            CHECK(g.bytes[static_cast<std::size_t>(i)] == std::byte{0});
        }
        const std::uint64_t indexEnd =
            r.mesh.indexDataOffset + (std::uint64_t{r.mesh.indexCount} * cookedIndexTypeBytes(r.mesh.indexType));
        for (std::uint64_t i = indexEnd; i < g.bytes.size(); ++i) {
            CHECK(g.bytes[static_cast<std::size_t>(i)] == std::byte{0});
        }
    }
}

TEST_CASE("cooked mesh: the mixed golden's second section is ascending semantic order (CM45)") {
    const std::vector<std::byte> b = asBytes(aero_test::COOKED_GOLDEN_MIXED);
    const auto r = parse(b);
    REQUIRE(r.status == CookedMeshStatus::Ok);
    REQUIRE(r.mesh.attributes.size() == 4U);
    CHECK(r.mesh.attributes[1].semantic == CookedVertexSemantic::Position);
    CHECK(r.mesh.attributes[1].format == CookedVertexFormat::Float3);
    CHECK(r.mesh.attributes[1].offset == 0U);
    CHECK(r.mesh.attributes[2].semantic == CookedVertexSemantic::Normal);
    CHECK(r.mesh.attributes[2].format == CookedVertexFormat::Float3);
    CHECK(r.mesh.attributes[2].offset == 12U);
    CHECK(r.mesh.attributes[3].semantic == CookedVertexSemantic::TexCoord0);
    CHECK(r.mesh.attributes[3].format == CookedVertexFormat::Float2);
    CHECK(r.mesh.attributes[3].offset == 24U);
    CHECK(r.mesh.sections[1].vertexStride == 32U);
    // Strictly ascending semantic code AND strictly ascending offset, and the offsets accumulate
    // exactly the widths of the attributes before them -- no padding between attributes.
    std::uint32_t expected = 0;
    for (std::uint32_t a = 0; a < 3; ++a) {
        const auto& attr = r.mesh.attributes[1 + a];
        CHECK(attr.offset == expected);
        expected += cookedVertexFormatBytes(attr.format);
    }
    CHECK(expected == r.mesh.sections[1].vertexStride);
}

TEST_CASE("cooked mesh: the mixed golden's interleaving reads back at base + i*stride + offset (CM46)") {
    const std::vector<std::byte> b = asBytes(aero_test::COOKED_GOLDEN_MIXED);
    const auto r = parse(b);
    REQUIRE(r.status == CookedMeshStatus::Ok);

    const auto s0 = engine::assets::sectionVertexBytes(r.mesh, 0);
    REQUIRE(s0.size() == 36U);
    const std::array<std::array<float, 3>, 3> expected0 = {std::array<float, 3>{0.0F, 0.0F, 0.0F},
                                                           std::array<float, 3>{1.0F, 0.0F, 0.0F},
                                                           std::array<float, 3>{0.0F, 1.0F, 0.0F}};
    for (std::size_t v = 0; v < 3; ++v) {
        for (std::size_t k = 0; k < 3; ++k) {
            CHECK(engine::assets::getF32(s0, (v * 12) + (k * 4)) == expected0[v][k]);
        }
    }

    const auto s1 = engine::assets::sectionVertexBytes(r.mesh, 1);
    REQUIRE(s1.size() == 96U);
    const std::array<std::array<float, 3>, 3> positions = {std::array<float, 3>{2.0F, 0.0F, 0.0F},
                                                           std::array<float, 3>{2.0F, 1.0F, 0.0F},
                                                           std::array<float, 3>{2.0F, 0.0F, 1.0F}};
    const std::array<std::array<float, 2>, 3> uvs = {std::array<float, 2>{0.0F, 0.0F}, std::array<float, 2>{1.0F, 0.0F},
                                                     std::array<float, 2>{0.0F, 1.0F}};
    for (std::size_t v = 0; v < 3; ++v) {
        const std::size_t base = v * 32;
        for (std::size_t k = 0; k < 3; ++k) {
            CHECK(engine::assets::getF32(s1, base + 0 + (k * 4)) == positions[v][k]);
        }
        CHECK(engine::assets::getF32(s1, base + 12) == 1.0F);  // normal.x
        CHECK(engine::assets::getF32(s1, base + 16) == 0.0F);
        CHECK(engine::assets::getF32(s1, base + 20) == 0.0F);
        CHECK(engine::assets::getF32(s1, base + 24) == uvs[v][0]);
        CHECK(engine::assets::getF32(s1, base + 28) == uvs[v][1]);
    }
}

TEST_CASE("cooked mesh: CookedVertexFormat maps to four distinct rhi::VertexFormat values (CM47)") {
    // aero_tests already links aero::rhi, which is what makes this assertable HERE. aero::assets
    // deliberately does not and never will: rhi::VertexFormat's fifteen enumerators have IMPLICIT
    // values, so a reorder is legal and invisible, and writing those numbers into a file would make
    // every previously-cooked artifact decode to the wrong formats afterwards.
    //
    // A switch with NO `default:`, so a new CookedVertexFormat enumerator is a -Wswitch BUILD FAILURE
    // on the Linux lane rather than a silently uncovered row.
    auto toRhi = [](CookedVertexFormat f) {
        switch (f) {
            case CookedVertexFormat::Float2:
                return engine::rhi::VertexFormat::Float2;
            case CookedVertexFormat::Float3:
                return engine::rhi::VertexFormat::Float3;
            case CookedVertexFormat::Float4:
                return engine::rhi::VertexFormat::Float4;
            case CookedVertexFormat::Uint4:
                return engine::rhi::VertexFormat::Uint4;
        }
        return engine::rhi::VertexFormat::Float;
    };
    const std::array<CookedVertexFormat, 4> all = {CookedVertexFormat::Float2, CookedVertexFormat::Float3,
                                                   CookedVertexFormat::Float4, CookedVertexFormat::Uint4};
    // PAIRWISE DISTINCT -- a real, computed fact about rhi, not a claim about it.
    for (std::size_t i = 0; i < all.size(); ++i) {
        for (std::size_t j = i + 1; j < all.size(); ++j) {
            CHECK(toRhi(all[i]) != toRhi(all[j]));
        }
    }
    // The byte size claimed for each rhi::VertexFormat is a LITERAL here, and that is deliberate:
    // aero::rhi publishes NO vertex-format size function (measured -- the only place a VertexFormat
    // is interpreted is the private SDL_GPU backend's toSdl). This half is a statement about the
    // enumerator's documented meaning, not a computed check. When the first pipeline lands
    // (3.4.1 / Phase 5) and rhi gains a size function, this tightens to a computed comparison.
    CHECK(cookedVertexFormatBytes(CookedVertexFormat::Float2) == 8U);   // rhi::VertexFormat::Float2 is 2 x f32
    CHECK(cookedVertexFormatBytes(CookedVertexFormat::Float3) == 12U);  // Float3 is 3 x f32
    CHECK(cookedVertexFormatBytes(CookedVertexFormat::Float4) == 16U);  // Float4 is 4 x f32
    CHECK(cookedVertexFormatBytes(CookedVertexFormat::Uint4) == 16U);   // Uint4 is 4 x u32
    // The index types line up one for one too.
    CHECK(cookedIndexTypeBytes(CookedIndexType::Uint16) == 2U);
    CHECK(cookedIndexTypeBytes(CookedIndexType::Uint32) == 4U);
    CHECK(static_cast<std::uint8_t>(engine::rhi::IndexType::Uint16) == 0U);
    CHECK(static_cast<std::uint8_t>(engine::rhi::IndexType::Uint32) == 1U);
}

TEST_CASE("cooked mesh: COOKED_SEMANTIC_COUNT bounds every semantic (CM48)") {
    // The pin that stops a ninth semantic being added without the parser's bound moving with it: the
    // parser refuses `semantic >= COOKED_SEMANTIC_COUNT`, so a new enumerator whose value is not
    // below the count would be refused in every file the cook writes.
    static_assert(engine::assets::COOKED_SEMANTIC_COUNT == 8);
    CHECK(engine::assets::COOKED_SEMANTIC_COUNT == 8U);
    CHECK(ALL_SEMANTICS.size() == engine::assets::COOKED_SEMANTIC_COUNT);
    for (const CookedVertexSemantic s : ALL_SEMANTICS) {
        CHECK(static_cast<std::uint16_t>(s) < engine::assets::COOKED_SEMANTIC_COUNT);
    }
    // And they are 0..7 with no gaps, which is what makes "bit n set means semantic n present" true.
    for (std::size_t i = 0; i < ALL_SEMANTICS.size(); ++i) {
        CHECK(static_cast<std::uint16_t>(ALL_SEMANTICS[i]) == static_cast<std::uint16_t>(i));
    }
}

TEST_CASE("cooked mesh: a header count of UINT32_MAX is refused BEFORE anything is reserved (CM49)") {
    // THE GAP SEED S16 FOUND. CM23 already proves the TABLE arithmetic runs before the three
    // reserves, but every count it and CM21 use is cap+1 -- 1025 attributes, 129 sections, 65537
    // submeshes -- and reserving for any of those succeeds in microseconds, so moving all three
    // reserves ABOVE the cap checks reddened nothing at all.
    //
    // These four counts are the ones that make the ordering observable: reserving for 4 294 967 295
    // records is 34 GB of attributes or 274 GB of submeshes, which the allocator refuses. Because
    // parseCookedMesh catches nothing and promises never to throw, a reserve placed before the cap
    // check turns each arm into an uncaught bad_alloc or a sanitiser abort rather than a refusal --
    // which is exactly the shape the ordering exists to prevent.
    struct Arm {
        const char* name;
        std::size_t offset;
    };
    const std::array<Arm, 4> arms = {Arm{"attributeCount", H_ATTRIBUTE_COUNT}, Arm{"sectionCount", H_SECTION_COUNT},
                                     Arm{"submeshCount", H_SUBMESH_COUNT}, Arm{"indexCount", H_INDEX_COUNT}};
    for (const Arm& arm : arms) {
        CAPTURE(arm.name);
        std::vector<std::byte> b = makeContainer(Shape::Empty);
        put32(b, arm.offset, std::numeric_limits<std::uint32_t>::max());
        const auto r = parse(b);
        CHECK(r.status == CookedMeshStatus::CapExceeded);
        CHECK_FALSE(r.message.empty());
        // Nothing was kept either: a refusal returns a default-constructed mesh, never a partly
        // populated one whose vectors a caller could walk.
        CHECK(r.mesh.attributes.empty());
        CHECK(r.mesh.sections.empty());
        CHECK(r.mesh.submeshes.empty());
    }
}

// =================================================================================================
// CM50 is the ONE case in this TU that touches the disk, and the TU's own header says "no disk except
// the golden header". That sentence was true when it was written and this case is the stated
// exception, not a drift: the property below has no runtime observable anywhere in this tree.
// =================================================================================================

namespace {

// A source line with its `//` comment removed -- the MI42c / OI77 / MS41 / AI2 shape, this TU's own
// copy. Without it the gate below would match the parser's OWN prose about the ordering it enforces
// and pass for the wrong reason (the AC-56 lesson, one layer down from the editor tier).
[[nodiscard]] std::string_view codeOf(std::string_view line) {
    const std::size_t commentStart = line.find("//");
    return commentStart == std::string_view::npos ? line : line.substr(0, commentStart);
}

[[nodiscard]] std::string strippedAssetsSource(const std::string& relativePath) {
    const std::string path = std::string(AERO_ASSETS_SRC_DIR) + "/" + relativePath;
    std::ifstream stream{path, std::ios::binary};
    REQUIRE_MESSAGE(stream.good(), path);
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(text.empty());
    std::string out;
    out.reserve(text.size());
    std::string_view remaining = text;
    while (true) {
        const std::size_t newline = remaining.find('\n');
        const std::string_view line = newline == std::string_view::npos ? remaining : remaining.substr(0, newline);
        out.append(codeOf(line));
        out.push_back('\n');
        if (newline == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(newline + 1U);
    }
    return out;
}

}  // namespace

TEST_CASE("cooked mesh: every header count is capped BEFORE the parser reserves anything (CM50)") {
    // THE GAP SEED S16 FOUND, and the reason this is source text rather than a behaviour.
    //
    // "Nothing is allocated before the count it is allocating for has been checked against a frozen
    // cap" is the parser's whole defence against a hostile header, and it is UNOBSERVABLE from a test
    // on this platform: with the three reserves deliberately moved above the four cap checks, a header
    // declaring 4 294 967 295 submeshes reserves 274 GB of ADDRESS SPACE, which macOS hands out
    // lazily and ASan's allocator permits -- measured, not assumed. CM49's four arms stayed green
    // throughout. A lane whose allocator commits rather than reserves would fail instead, which is
    // exactly the portability asymmetry that makes a behavioural case the wrong instrument here.
    //
    // So the ORDER is asserted in the text, over comment-stripped source so the file's own
    // explanation of the rule cannot satisfy the gate.
    const std::string code = strippedAssetsSource("cooked_mesh.cpp");

    const std::size_t firstReserve = code.find(".reserve(");
    REQUIRE(firstReserve != std::string::npos);
    // Exactly three, and they are the three CookedMesh vectors. A fourth would need its own proof.
    std::size_t reserveCount = 0;
    for (std::size_t at = code.find(".reserve("); at != std::string::npos; at = code.find(".reserve(", at + 1U)) {
        ++reserveCount;
    }
    CHECK(reserveCount == 3U);

    // The four header counts, each compared against its own frozen cap, all textually ABOVE the first
    // reserve. Written as the comparison rather than the message, so rewording a diagnostic is free.
    for (const std::string_view check : {"attributeCount > MAX_COOKED_ATTRIBUTES", "sectionCount > MAX_COOKED_SECTIONS",
                                         "submeshCount > MAX_COOKED_SUBMESHES", "indexCount > MAX_COOKED_INDICES"}) {
        CAPTURE(check);
        const std::size_t at = code.find(check);
        REQUIRE(at != std::string::npos);  // anti-vacuity: a renamed check must fail, never pass
        CHECK(at < firstReserve);
    }
}

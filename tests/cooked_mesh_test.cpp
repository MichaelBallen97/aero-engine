// tests/cooked_mesh_test.cpp -- task 3.3.1: the .aeromesh container v1. A TU of aero_tests, which
// supplies main() from test_main.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier-0: no GPU, no window, no disk. Every buffer this file parses is built BY THIS FILE, byte by
// byte, from docs/09 section 9's own rules -- deliberately, so a cook bug can never mask a parser
// bug and every refusal arm mutates exactly one field of something already valid.
#include <aero/assets/cooked_mesh.hpp>

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
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

// Aero Engine — rhi texture-format utility tests (task 0.4.1, AC-9; grown at task 3.4.1 for the six
// BC block formats and the block-unit size contract). Exhaustive over every real TextureFormat value
// (0 .. Count-1): classification (isDepthFormat/hasStencilComponent/isSrgbFormat), texelBlockSize,
// texelBlockWidth/Height, textureLevelByteSize, and toString. doctest cannot print
// `enum class : uint8_t` cleanly (it streams as a raw byte), so loops iterate `int` and attach
// INFO("format value ", v) so a failure names the offending value (see the 0.4.1 spec's J-6 note).
//
// Every case-local table below carries a LITERAL row count, never TABLE.size(): a guard derived from
// the table it guards cannot see a deleted row (task 3.3.2's anti-vacuity lesson).
#include <aero/rhi/format.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
// <ostream> is load-bearing on MSVC: doctest stringifies the std::string_view operands of the
// toString CHECKs below via the stdlib's operator<<(std::ostream&, std::string_view), which MS STL
// defines inline in <string_view> against an INCOMPLETE std::basic_ostream — only <ostream> completes
// it. libc++/libstdc++ are self-sufficient, so the gap only surfaces on the Windows lane.
#include <ostream>
#include <type_traits>

using engine::rhi::hasStencilComponent;
using engine::rhi::isDepthFormat;
using engine::rhi::isSrgbFormat;
using engine::rhi::texelBlockHeight;
using engine::rhi::texelBlockSize;
using engine::rhi::texelBlockWidth;
using engine::rhi::TextureFormat;
using engine::rhi::textureLevelByteSize;
using engine::rhi::toString;

TEST_CASE("rhi format: enum layout — uint8_t underlying, Invalid == 0, Count == 25") {
    static_assert(std::is_same_v<std::underlying_type_t<TextureFormat>, std::uint8_t>);
    static_assert(static_cast<int>(TextureFormat::Invalid) == 0);
    // Pins append-only ordering: 1 sentinel (Invalid) + 19 color formats (13 uncompressed + the six
    // BC block formats appended at task 3.4.1) + 5 depth formats.
    static_assert(static_cast<int>(TextureFormat::Count) == 25);
    // The six append AFTER every depth format and BEFORE Count — appending is legal precisely
    // because rhi enum values are never serialized (task 3.3.1 built CookedVertexFormat rather than
    // write rhi::VertexFormat to disk; CookedTextureFormat exists for the same reason).
    static_assert(static_cast<int>(TextureFormat::BC1RGBAUnorm) > static_cast<int>(TextureFormat::D32FloatS8Uint));
    static_assert(static_cast<int>(TextureFormat::BC5RGUnorm) + 1 == static_cast<int>(TextureFormat::Count));
    CHECK(true);
}

TEST_CASE("rhi format: classification — exactly 5 depth formats") {
    int depthCount = 0;
    for (int v = 0; v < static_cast<int>(TextureFormat::Count); ++v) {
        const auto format = static_cast<TextureFormat>(v);
        INFO("format value ", v);
        if (isDepthFormat(format)) {
            ++depthCount;
        }
    }
    CHECK(depthCount == 5);

    CHECK(isDepthFormat(TextureFormat::D16Unorm));
    CHECK(isDepthFormat(TextureFormat::D24Unorm));
    CHECK(isDepthFormat(TextureFormat::D32Float));
    CHECK(isDepthFormat(TextureFormat::D24UnormS8Uint));
    CHECK(isDepthFormat(TextureFormat::D32FloatS8Uint));
}

TEST_CASE("rhi format: classification — exactly 2 formats have a stencil component, both depth") {
    int stencilCount = 0;
    for (int v = 0; v < static_cast<int>(TextureFormat::Count); ++v) {
        const auto format = static_cast<TextureFormat>(v);
        INFO("format value ", v);
        if (hasStencilComponent(format)) {
            ++stencilCount;
            CHECK(isDepthFormat(format));
        }
    }
    CHECK(stencilCount == 2);
    CHECK(hasStencilComponent(TextureFormat::D24UnormS8Uint));
    CHECK(hasStencilComponent(TextureFormat::D32FloatS8Uint));
}

TEST_CASE("rhi format: classification — exactly 4 sRGB formats, none depth") {
    int srgbCount = 0;
    for (int v = 0; v < static_cast<int>(TextureFormat::Count); ++v) {
        const auto format = static_cast<TextureFormat>(v);
        INFO("format value ", v);
        if (isSrgbFormat(format)) {
            ++srgbCount;
            CHECK_FALSE(isDepthFormat(format));
        }
    }
    CHECK(srgbCount == 4);
    CHECK(isSrgbFormat(TextureFormat::RGBA8UnormSrgb));
    CHECK(isSrgbFormat(TextureFormat::BGRA8UnormSrgb));
    CHECK(isSrgbFormat(TextureFormat::BC1RGBAUnormSrgb));
    CHECK(isSrgbFormat(TextureFormat::BC3RGBAUnormSrgb));
    // AC-1's deliberate absence, pinned as an assertion rather than a comment: BC4/BC5 have no sRGB
    // variant in Vulkan or SDL, which is what keeps "an sRGB normal map" unspellable end to end. The
    // count of 4 above is what makes this exhaustive — a seventh sRGB enumerator would redden it.
    CHECK_FALSE(isSrgbFormat(TextureFormat::BC4RUnorm));
    CHECK_FALSE(isSrgbFormat(TextureFormat::BC5RGUnorm));
}

TEST_CASE("rhi format: classification — all three classifiers are false on Invalid and Count") {
    CHECK_FALSE(isDepthFormat(TextureFormat::Invalid));
    CHECK_FALSE(hasStencilComponent(TextureFormat::Invalid));
    CHECK_FALSE(isSrgbFormat(TextureFormat::Invalid));
    CHECK_FALSE(isDepthFormat(TextureFormat::Count));
    CHECK_FALSE(hasStencilComponent(TextureFormat::Count));
    CHECK_FALSE(isSrgbFormat(TextureFormat::Count));
}

TEST_CASE("rhi format: texelBlockSize exact table for the 13 uncompressed color formats") {
    CHECK(texelBlockSize(TextureFormat::R8Unorm) == 1);
    CHECK(texelBlockSize(TextureFormat::RG8Unorm) == 2);
    CHECK(texelBlockSize(TextureFormat::RGBA8Unorm) == 4);
    CHECK(texelBlockSize(TextureFormat::RGBA8UnormSrgb) == 4);
    CHECK(texelBlockSize(TextureFormat::BGRA8Unorm) == 4);
    CHECK(texelBlockSize(TextureFormat::BGRA8UnormSrgb) == 4);
    CHECK(texelBlockSize(TextureFormat::R16Float) == 2);
    CHECK(texelBlockSize(TextureFormat::RG16Float) == 4);
    CHECK(texelBlockSize(TextureFormat::RGBA16Float) == 8);
    CHECK(texelBlockSize(TextureFormat::R32Float) == 4);
    CHECK(texelBlockSize(TextureFormat::RG32Float) == 8);
    CHECK(texelBlockSize(TextureFormat::RGBA32Float) == 16);
    CHECK(texelBlockSize(TextureFormat::R11G11B10Ufloat) == 4);
}

TEST_CASE("rhi format: texelBlockSize is bytes per 4x4 BLOCK for the six BC formats (task 3.4.1)") {
    struct BlockRow {
        TextureFormat format;
        std::uint32_t blockBytes;
    };
    // 8 for the one-endpoint-pair families (BC1 colour, BC4 single channel), 16 for the two that
    // carry a second block (BC3 = BC1 colour + BC4 alpha; BC5 = two BC4 blocks).
    constexpr std::array ROWS{
        BlockRow{TextureFormat::BC1RGBAUnorm, 8},  BlockRow{TextureFormat::BC1RGBAUnormSrgb, 8},
        BlockRow{TextureFormat::BC3RGBAUnorm, 16}, BlockRow{TextureFormat::BC3RGBAUnormSrgb, 16},
        BlockRow{TextureFormat::BC4RUnorm, 8},     BlockRow{TextureFormat::BC5RGUnorm, 16},
    };
    CHECK(ROWS.size() == 6);  // LITERAL, never ROWS.size() on both sides — a deleted row reddens here

    std::size_t checked = 0;
    for (const BlockRow& row : ROWS) {
        INFO("format ", toString(row.format));
        CHECK(texelBlockSize(row.format) == row.blockBytes);
        ++checked;
    }
    CHECK(checked == 6);  // LITERAL: proves the loop ran, independently of the table's own length
}

TEST_CASE("rhi format: texelBlockSize is 0 for every depth format, Invalid, and Count") {
    CHECK(texelBlockSize(TextureFormat::D16Unorm) == 0);
    CHECK(texelBlockSize(TextureFormat::D24Unorm) == 0);
    CHECK(texelBlockSize(TextureFormat::D32Float) == 0);
    CHECK(texelBlockSize(TextureFormat::D24UnormS8Uint) == 0);
    CHECK(texelBlockSize(TextureFormat::D32FloatS8Uint) == 0);
    CHECK(texelBlockSize(TextureFormat::Invalid) == 0);
    CHECK(texelBlockSize(TextureFormat::Count) == 0);
}

TEST_CASE("rhi format: texelBlockSize is 0 iff depth or Invalid, over every real value") {
    for (int v = 0; v < static_cast<int>(TextureFormat::Count); ++v) {
        const auto format = static_cast<TextureFormat>(v);
        INFO("format value ", v);
        const bool zeroSized = texelBlockSize(format) == 0;
        const bool depthOrInvalid = isDepthFormat(format) || format == TextureFormat::Invalid;
        CHECK(zeroSized == depthOrInvalid);
    }
}

TEST_CASE("rhi format: texelBlockWidth/Height are total over the enum (task 3.4.1)") {
    int fourByFour = 0;
    int oneByOne = 0;
    int zeroExtent = 0;
    // Count is included deliberately: the sentinel must answer 0 like Invalid does, not fall through
    // to the 1x1 arm.
    for (int v = 0; v <= static_cast<int>(TextureFormat::Count); ++v) {
        const auto format = static_cast<TextureFormat>(v);
        INFO("format value ", v);
        const std::uint32_t blockW = texelBlockWidth(format);
        const std::uint32_t blockH = texelBlockHeight(format);
        // Square blocks throughout today (ASTC at task 6.3.1 is what breaks this, and the two
        // functions exist so that it breaks loudly on one axis rather than silently on both).
        CHECK(blockW == blockH);
        // The one structural invariant tying the three functions together: a format with no block
        // size has no block extent either, so no caller can compute a size from a 0-byte format.
        CHECK((texelBlockSize(format) == 0) == (blockW == 0));
        if (blockW == 4) {
            ++fourByFour;
        } else if (blockW == 1) {
            ++oneByOne;
        } else {
            CHECK(blockW == 0);
            ++zeroExtent;
        }
    }
    CHECK(fourByFour == 6);  // exactly the six BC formats
    CHECK(oneByOne == 13);   // exactly the 13 uncompressed color formats
    CHECK(zeroExtent == 7);  // 5 depth + Invalid + Count
    CHECK(texelBlockWidth(TextureFormat::BC1RGBAUnorm) == 4);
    CHECK(texelBlockHeight(TextureFormat::BC5RGUnorm) == 4);
    CHECK(texelBlockWidth(TextureFormat::RGBA8Unorm) == 1);
    CHECK(texelBlockHeight(TextureFormat::R11G11B10Ufloat) == 1);
    CHECK(texelBlockWidth(TextureFormat::D16Unorm) == 0);
    CHECK(texelBlockWidth(TextureFormat::Invalid) == 0);
    CHECK(texelBlockWidth(TextureFormat::Count) == 0);
}

TEST_CASE("rhi format: textureLevelByteSize degenerates to size * w * h for every 1x1-block format") {
    struct Extent {
        std::uint32_t width;
        std::uint32_t height;
    };
    // Deliberately includes odd, prime and 1-wide extents: for a 1x1-block format the ceil-div must
    // vanish, so any of these disagreeing would mean the new formula changed an existing caller.
    constexpr std::array EXTENTS{
        Extent{1, 1}, Extent{2, 2}, Extent{4, 4}, Extent{5, 3}, Extent{7, 5}, Extent{256, 1}, Extent{17, 33},
    };
    CHECK(EXTENTS.size() == 7);  // LITERAL

    std::size_t checked = 0;
    for (int v = 0; v < static_cast<int>(TextureFormat::Count); ++v) {
        const auto format = static_cast<TextureFormat>(v);
        if (texelBlockWidth(format) != 1) {
            continue;  // depth/Invalid (0) and the six BC formats (4) are the other two cases below
        }
        for (const Extent& extent : EXTENTS) {
            INFO("format value ", v, " at ", extent.width, "x", extent.height);
            const std::uint64_t expected = std::uint64_t{texelBlockSize(format)} * extent.width * extent.height;
            CHECK(textureLevelByteSize(format, extent.width, extent.height) == expected);
            ++checked;
        }
    }
    CHECK(checked == 13 * 7);  // LITERAL: 13 uncompressed color formats x 7 extents
}

TEST_CASE("rhi format: textureLevelByteSize is the ceil-div block formula for BC, 0 where unsized") {
    struct SizeRow {
        TextureFormat format;
        std::uint32_t width;
        std::uint32_t height;
        std::uint64_t expected;
    };
    // The same arithmetic docs/09 section 10 fixes for a cooked level: blocksX * blocksY *
    // bytesPerBlock with blocksN = ceil(extent / 4). The mip-tail rows (2x2, 1x1) are the ones that
    // make a BC upload ONE block rather than a fraction of one. (Task 3.4.1's PB2 cross-checks this
    // against the cooked container's own level arithmetic once the render bridge exists; this case
    // pins the rhi side on its own so the formula is never unwitnessed in its own module.)
    constexpr std::array ROWS{
        SizeRow{TextureFormat::BC1RGBAUnorm, 4, 4, 8},
        SizeRow{TextureFormat::BC1RGBAUnorm, 2, 2, 8},
        SizeRow{TextureFormat::BC1RGBAUnorm, 1, 1, 8},
        SizeRow{TextureFormat::BC1RGBAUnorm, 5, 3, 16},
        SizeRow{TextureFormat::BC1RGBAUnormSrgb, 8, 8, 32},
        SizeRow{TextureFormat::BC3RGBAUnorm, 8, 8, 64},
        SizeRow{TextureFormat::BC3RGBAUnormSrgb, 2, 2, 16},
        SizeRow{TextureFormat::BC4RUnorm, 7, 5, 32},
        // 5x3 BC5 is 2 blocks of 16 = 32, NOT 16 — the same number the committed 5x3 BC5 golden
        // spells in its own layout comment ("368..399  level 0 (5x3), 2 blocks of 16").
        SizeRow{TextureFormat::BC5RGUnorm, 5, 3, 32},
        SizeRow{TextureFormat::BC5RGUnorm, 16, 16, 256},
    };
    CHECK(ROWS.size() == 10);  // LITERAL

    std::size_t checked = 0;
    for (const SizeRow& row : ROWS) {
        INFO("format ", toString(row.format), " at ", row.width, "x", row.height);
        CHECK(textureLevelByteSize(row.format, row.width, row.height) == row.expected);
        ++checked;
    }
    CHECK(checked == 10);  // LITERAL

    // 0 whenever texelBlockSize is 0 — the whole depth family plus both sentinels, at a real extent.
    for (int v = 0; v <= static_cast<int>(TextureFormat::Count); ++v) {
        const auto format = static_cast<TextureFormat>(v);
        if (texelBlockSize(format) != 0) {
            continue;
        }
        INFO("format value ", v);
        CHECK(textureLevelByteSize(format, 4, 4) == 0);
    }
    // Degenerate extents answer 0 for a sized format too, exactly as the old w * h formula did.
    CHECK(textureLevelByteSize(TextureFormat::RGBA8Unorm, 0, 4) == 0);
    CHECK(textureLevelByteSize(TextureFormat::BC1RGBAUnorm, 4, 0) == 0);
}

TEST_CASE("rhi format: toString is non-empty and equals \"Invalid\" only for Invalid") {
    for (int v = 0; v < static_cast<int>(TextureFormat::Count); ++v) {
        const auto format = static_cast<TextureFormat>(v);
        INFO("format value ", v);
        CHECK_FALSE(toString(format).empty());
        CHECK((toString(format) == "Invalid") == (format == TextureFormat::Invalid));
    }
}

TEST_CASE("rhi format: toString is unique across every real value") {
    const int count = static_cast<int>(TextureFormat::Count);
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            INFO("format values ", i, " and ", j);
            CHECK(toString(static_cast<TextureFormat>(i)) != toString(static_cast<TextureFormat>(j)));
        }
    }
}

TEST_CASE("rhi format: toString verbatim spot-checks") {
    CHECK(toString(TextureFormat::RGBA8Unorm) == "RGBA8Unorm");
    CHECK(toString(TextureFormat::D32Float) == "D32Float");
    // The six BC spellings, pinned exactly (task 3.4.1): these strings reach users through
    // createTexture's and uploadTexture's ERROR messages, so a typo is a support-question generator.
    CHECK(toString(TextureFormat::BC1RGBAUnorm) == "BC1RGBAUnorm");
    CHECK(toString(TextureFormat::BC1RGBAUnormSrgb) == "BC1RGBAUnormSrgb");
    CHECK(toString(TextureFormat::BC3RGBAUnorm) == "BC3RGBAUnorm");
    CHECK(toString(TextureFormat::BC3RGBAUnormSrgb) == "BC3RGBAUnormSrgb");
    CHECK(toString(TextureFormat::BC4RUnorm) == "BC4RUnorm");
    CHECK(toString(TextureFormat::BC5RGUnorm) == "BC5RGUnorm");
    // Implementation detail, loosenable later (plan §C-3): the spec only requires "Invalid" to be
    // exclusive to TextureFormat::Invalid among the REAL values (F5); it explicitly allows the
    // Count sentinel to share the string, and format.cpp's total switch's default case does.
    CHECK(toString(TextureFormat::Count) == "Invalid");
}

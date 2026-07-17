// Aero Engine — rhi texture-format utility tests (task 0.4.1, AC-9). Exhaustive over every real
// TextureFormat value (0 .. Count-1): classification (isDepthFormat/hasStencilComponent/
// isSrgbFormat), texelBlockSize, and toString. doctest cannot print `enum class : uint8_t` cleanly
// (it streams as a raw byte), so loops iterate `int` and attach INFO("format value ", v) so a
// failure names the offending value (see the 0.4.1 spec's J-6 note).
#include <aero/rhi/format.hpp>

#include <doctest/doctest.h>

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
using engine::rhi::texelBlockSize;
using engine::rhi::TextureFormat;
using engine::rhi::toString;

TEST_CASE("rhi format: enum layout — uint8_t underlying, Invalid == 0, Count == 19") {
    static_assert(std::is_same_v<std::underlying_type_t<TextureFormat>, std::uint8_t>);
    static_assert(static_cast<int>(TextureFormat::Invalid) == 0);
    // Pins append-only ordering: 1 sentinel (Invalid) + 13 color formats + 5 depth formats.
    static_assert(static_cast<int>(TextureFormat::Count) == 19);
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

TEST_CASE("rhi format: classification — exactly 2 sRGB formats, neither depth") {
    int srgbCount = 0;
    for (int v = 0; v < static_cast<int>(TextureFormat::Count); ++v) {
        const auto format = static_cast<TextureFormat>(v);
        INFO("format value ", v);
        if (isSrgbFormat(format)) {
            ++srgbCount;
            CHECK_FALSE(isDepthFormat(format));
        }
    }
    CHECK(srgbCount == 2);
    CHECK(isSrgbFormat(TextureFormat::RGBA8UnormSrgb));
    CHECK(isSrgbFormat(TextureFormat::BGRA8UnormSrgb));
}

TEST_CASE("rhi format: classification — all three classifiers are false on Invalid and Count") {
    CHECK_FALSE(isDepthFormat(TextureFormat::Invalid));
    CHECK_FALSE(hasStencilComponent(TextureFormat::Invalid));
    CHECK_FALSE(isSrgbFormat(TextureFormat::Invalid));
    CHECK_FALSE(isDepthFormat(TextureFormat::Count));
    CHECK_FALSE(hasStencilComponent(TextureFormat::Count));
    CHECK_FALSE(isSrgbFormat(TextureFormat::Count));
}

TEST_CASE("rhi format: texelBlockSize exact table for the 13 color formats") {
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
    // Implementation detail, loosenable later (plan §C-3): the spec only requires "Invalid" to be
    // exclusive to TextureFormat::Invalid among the REAL values (F5); it explicitly allows the
    // Count sentinel to share the string, and format.cpp's total switch's default case does.
    CHECK(toString(TextureFormat::Count) == "Invalid");
}

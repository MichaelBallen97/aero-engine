// tests/editor/texture_cook_source_test.cpp -- task 3.3.2: the image bytes -> texture cook adapter,
// and the ONE place in the tree where a real decoded image meets assets::cookTexture. A TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, tier-0 (the mesh_cook_source_test.cpp precedent): texture_cook_source.hpp depends only on
// aero/assets/cooked_texture.hpp, and aero::assets is a PUBLIC, UNGATED dependency of
// aero_editor_core -- so every case here must be PRESENT and PASSING in all three build
// configurations. No GPU, no window, no ImGui context, no sleeps, and no disk except the two
// committed PNG fixtures.
//
// NEVER WRITE `CHECK(format == CookedTextureFormat::X)` IN THIS TU. doctest's DOCTEST_STRINGIFY
// expands to an UNQUALIFIED `toString(...)`, so ADL finds engine::assets::toString(CookedTextureFormat)
// -- a non-template exact match that beats doctest's own template -- and the decomposer then tries
// `std::string_view + const char*`, which is a hard compile error on EVERY lane, inside doctest.h
// rather than at the CHECK. Compare through toString() on both sides, exactly as
// tests/cooked_texture_test.cpp does.
//
// THE TWO COMMITTED PNG FIXTURES ARE THE TREE'S FIRST IMAGES OF ANY KIND. Before this task
// `git ls-files | grep -iE '\.(png|jpg|tga|bmp)$'` returned nothing at all, so "decode a committed
// fixture" had no fixture to decode. TGA and BMP are built BYTE BY BYTE in this file instead -- both
// are hand-writable with no compressor -- which gets two more claimed extensions genuinely decoded
// without a second committed blob. .jpg, .jpeg, .gif and .psd are covered BY NAME ONLY, through
// isCookableTextureName: their decoders are stb_image's and are not exercised here, and a case that
// only looked like proof would be worse than this sentence.
#include <aero/assets/cooked_texture.hpp>
#include <aero/assets/texture_cook.hpp>
#include <aero/editor/asset_view.hpp>
#include <aero/editor/text_file.hpp>
#include <aero/editor/texture_cook_source.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
// <ostream> is load-bearing on MSVC (the 0.4.1 trap): doctest stringifies the std::string_view
// operands of the toString CHECKs below through the stdlib's operator<<(std::ostream&,
// std::string_view), which MS STL defines inline in <string_view> against an INCOMPLETE
// std::basic_ostream -- only <ostream> completes it. libc++ and libstdc++ are self-sufficient, so
// omitting it builds clean on macOS and Linux and fails only on the Windows lane, with errors pointing
// inside the STL headers rather than at the CHECK.
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using engine::assets::CookedTextureFormat;
using engine::assets::toString;
using engine::editor::chooseTextureFormat;
using engine::editor::DecodedImage;
using engine::editor::decodeImageRgba8;
using engine::editor::isCookableTextureName;
using engine::editor::isHdrTextureName;

namespace {

// The per-axis bound every case passes unless it is the one testing the refusal.
constexpr std::uint32_t ANY_DIMENSION = engine::assets::MAX_TEXTURE_DIMENSION;

[[nodiscard]] std::vector<std::byte> toBytes(std::span<const std::uint8_t> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const std::uint8_t v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// Reads a committed fixture through AERO_ASSET_FIXTURES_DIR (ALREADY defined on this target -- no
// second definition is added, which would be a drift surface) and REQUIREs success. A missing fixture
// is a FAILURE, never a skip.
[[nodiscard]] std::string readFixture(const std::string& name) {
    const std::string path = std::string(AERO_ASSET_FIXTURES_DIR) + "/" + name;
    const engine::editor::FileBytesResult read = engine::editor::readFileBytes(path, 1U << 20U);
    REQUIRE_MESSAGE(read.bytes.has_value(), path);
    return *read.bytes;
}

[[nodiscard]] std::span<const std::byte> asBytes(const std::string& text) noexcept {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

// ---- a hand-built, UNCOMPRESSED 32-bit TGA -------------------------------------------------------
// 18-byte header then raw BGRA, bottom-up by default -- except that bit 5 of the image descriptor
// byte (offset 17) flips the origin to TOP-LEFT, which is what this builder sets so the expected
// bytes read in the same order they are written. No compressor, no library, no fixture file.
[[nodiscard]] std::vector<std::byte> buildTga32(std::uint16_t width, std::uint16_t height,
                                                std::span<const std::uint8_t> rgba) {
    std::vector<std::byte> out;
    out.reserve(18U + rgba.size());
    // Zero-initialized then assigned BY OFFSET, rather than as an eighteen-element initializer list:
    // every field of a TGA header is identified by its offset in the format's own documentation, and
    // an initializer list spells eleven zeroes whose meaning a reader has to count out.
    // Offsets 0 (idLength), 1 (colourMapType: none) and 3..11 (the colour-map spec and the x/y
    // origins) all stay zero, which is what the value-initialization above gives them.
    std::array<std::uint8_t, 18> header{};
    header[2] = 2;                                          // imageType: uncompressed true-colour
    header[12] = static_cast<std::uint8_t>(width & 0xFF);   // width, little-endian
    header[13] = static_cast<std::uint8_t>(width >> 8U);    //
    header[14] = static_cast<std::uint8_t>(height & 0xFF);  // height, little-endian
    header[15] = static_cast<std::uint8_t>(height >> 8U);   //
    header[16] = 32;                                        // bits per pixel
    header[17] = 0x28;                                      // 8 alpha bits + TOP-LEFT origin (bit 5)
    for (const std::uint8_t b : header) {
        out.push_back(static_cast<std::byte>(b));
    }
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        out.push_back(static_cast<std::byte>(rgba[i + 2]));  // B
        out.push_back(static_cast<std::byte>(rgba[i + 1]));  // G
        out.push_back(static_cast<std::byte>(rgba[i + 0]));  // R
        out.push_back(static_cast<std::byte>(rgba[i + 3]));  // A
    }
    return out;
}

// ---- a hand-built, UNCOMPRESSED 24-bit BMP -------------------------------------------------------
// 14-byte file header, 40-byte BITMAPINFOHEADER, then BGR rows padded to four bytes and stored
// BOTTOM-UP (a positive biHeight). The bottom-up storage is the point: the decoded image must come
// back TOP-LEFT origin, which is what our files' KTXorientation `rd` asserts about them.
[[nodiscard]] std::vector<std::byte> buildBmp24(std::uint32_t width, std::uint32_t height,
                                                std::span<const std::uint8_t> rgb) {
    const std::size_t rowBytes = ((static_cast<std::size_t>(width) * 3U) + 3U) & ~std::size_t{3};
    const std::size_t pixelBytes = rowBytes * height;
    const std::size_t fileBytes = 54U + pixelBytes;
    std::vector<std::byte> out(fileBytes, std::byte{0});
    const auto put16 = [&out](std::size_t at, std::uint32_t v) {
        out[at] = static_cast<std::byte>(v & 0xFF);
        out[at + 1] = static_cast<std::byte>((v >> 8U) & 0xFF);
    };
    const auto put32 = [&out](std::size_t at, std::uint32_t v) {
        for (std::size_t i = 0; i < 4; ++i) {
            out[at + i] = static_cast<std::byte>((v >> (8U * i)) & 0xFF);
        }
    };
    out[0] = static_cast<std::byte>('B');
    out[1] = static_cast<std::byte>('M');
    put32(2, static_cast<std::uint32_t>(fileBytes));
    put32(10, 54);  // pixel data offset
    put32(14, 40);  // BITMAPINFOHEADER size
    put32(18, width);
    put32(22, height);  // POSITIVE -> bottom-up rows
    put16(26, 1);       // planes
    put16(28, 24);      // bits per pixel
    put32(34, static_cast<std::uint32_t>(pixelBytes));
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t dstRow = 54U + (static_cast<std::size_t>(height - 1U - y) * rowBytes);
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t src = ((static_cast<std::size_t>(y) * width) + x) * 3U;
            out[dstRow + (std::size_t{3} * x) + 0] = static_cast<std::byte>(rgb[src + 2]);  // B
            out[dstRow + (std::size_t{3} * x) + 1] = static_cast<std::byte>(rgb[src + 1]);  // G
            out[dstRow + (std::size_t{3} * x) + 2] = static_cast<std::byte>(rgb[src + 0]);  // R
        }
    }
    return out;
}

// A source line with its `//` comment removed -- the MS41 / AI3 shape, this TU's own copy. TK18
// depends on it, and it is what makes a gate survive the gated file's OWN documentation of the token
// it forbids: texture_cook_source.cpp says "STBI_NO_FAILURE_STRINGS IS DELIBERATELY NOT DEFINED HERE"
// in a comment, which an unstripped grep would read as a definition.
[[nodiscard]] std::string_view codeOf(std::string_view line) {
    const std::size_t commentStart = line.find("//");
    return commentStart == std::string_view::npos ? line : line.substr(0, commentStart);
}

[[nodiscard]] std::string strippedEditorSource(const std::string& relativePath) {
    const std::string path = std::string(AERO_EDITOR_SRC_DIR) + "/" + relativePath;
    const engine::editor::FileReadResult read = engine::editor::readTextFile(path);
    REQUIRE_MESSAGE(read.text.has_value(), path);
    std::string out;
    out.reserve(read.text->size());
    std::string_view remaining = *read.text;
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

// An opaque RGBA8 block of `texels` identical texels, for the chooseTextureFormat cases.
[[nodiscard]] std::vector<std::byte> solidRgba(std::size_t texels, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                               std::uint8_t a) {
    std::vector<std::byte> out;
    out.reserve(texels * 4U);
    for (std::size_t i = 0; i < texels; ++i) {
        out.push_back(static_cast<std::byte>(r));
        out.push_back(static_cast<std::byte>(g));
        out.push_back(static_cast<std::byte>(b));
        out.push_back(static_cast<std::byte>(a));
    }
    return out;
}

}  // namespace

TEST_CASE("isCookableTextureName claims exactly seven extensions and NOT .hdr (TK1)") {
    for (const std::string_view claimed : {"a.png", "a.jpg", "a.jpeg", "a.tga", "a.bmp", "a.gif", "a.psd"}) {
        INFO("claimed: ", claimed);
        CHECK(isCookableTextureName(claimed));
    }
    // ASCII case folding, both directions, on every claimed extension.
    for (const std::string_view shouted : {"A.PNG", "A.Jpg", "A.JPEG", "A.TGA", "A.Bmp", "A.GIF", "A.PsD"}) {
        INFO("shouted: ", shouted);
        CHECK(isCookableTextureName(shouted));
    }
    // .hdr is the one refused BY NAME, and the rest are simply not ours. "a." is a trailing dot,
    // which is no extension at all -- asset_view.cpp's rawExtensionOf rule, reproduced here.
    for (const std::string_view refused : {"a.hdr", "a.HDR", "a.ktx2", "a.dds", "a.gltf", "a.exr", "a", "a."}) {
        INFO("refused: ", refused);
        CHECK_FALSE(isCookableTextureName(refused));
    }
    // A LEADING dot is an extension, not a hidden file, exactly as it is for classifyAssetKind: the
    // rule is "everything after the last dot", and it has no notion of a stem. Recorded rather than
    // left to be rediscovered, because the two readings are easy to confuse.
    CHECK(isCookableTextureName(".png"));
}

TEST_CASE("isHdrTextureName is true for .hdr alone, in either case (TK2)") {
    CHECK(isHdrTextureName("x.hdr"));
    CHECK(isHdrTextureName("x.HDR"));
    CHECK(isHdrTextureName("some/dir/x.Hdr"));
    for (const std::string_view other : {"x.hdrx", "x.hd", "hdr", "x.png", "x.", "x"}) {
        INFO("other: ", other);
        CHECK_FALSE(isHdrTextureName(other));
    }
}

TEST_CASE("the cookable table is SEPARATE from the thumbnail-decodable one (TK3)") {
    // Asserted side by side in one case, because that is the shape a future "derive one from the
    // other" edit reddens. .hdr is stb-DECODABLE -- stbi_load happily tone-maps it -- and precisely
    // not COOKABLE, which is the whole distinction.
    CHECK(engine::editor::isThumbnailDecodable("x.hdr"));
    CHECK_FALSE(isCookableTextureName("x.hdr"));
    // And the two agree everywhere else they overlap, which is what makes the .hdr row a deliberate
    // difference rather than an accident of two independently drifting tables.
    for (const std::string_view shared : {"x.png", "x.jpg", "x.jpeg", "x.tga", "x.bmp", "x.gif", "x.psd"}) {
        INFO("shared: ", shared);
        CHECK(engine::editor::isThumbnailDecodable(shared));
        CHECK(isCookableTextureName(shared));
    }
}

TEST_CASE("the cookable table has exactly SEVEN entries, counted through the predicate (TK4)") {
    // Counted over a fixed candidate list that is a strict superset of the table, so a table that
    // GREW is caught as surely as one that shrank. Deriving the candidate list from the table would
    // be circular.
    constexpr std::array<std::string_view, 14> CANDIDATES{"png", "jpg", "jpeg", "tga",  "bmp", "gif",  "psd",
                                                          "hdr", "dds", "ktx2", "webp", "exr", "tiff", "pnm"};
    std::size_t claimed = 0;
    std::size_t visited = 0;
    for (const std::string_view ext : CANDIDATES) {
        if (isCookableTextureName(std::string("x.").append(ext))) {
            ++claimed;
        }
        ++visited;
    }
    REQUIRE(visited == 14);  // anti-vacuity: the loop actually ran
    CHECK(claimed == 7);
}

TEST_CASE("chooseTextureFormat answers BC1 in the requested colour space for an opaque image (TK5)") {
    const std::vector<std::byte> opaque = solidRgba(16, 10, 20, 30, 255);
    CHECK(toString(chooseTextureFormat(opaque, true)) == toString(CookedTextureFormat::Bc1RgbSrgb));
    CHECK(toString(chooseTextureFormat(opaque, false)) == toString(CookedTextureFormat::Bc1RgbUnorm));
}

TEST_CASE("the alpha boundary is 254 vs 255, in both colour spaces (TK6)") {
    // ONE texel of alpha 254 among fifteen opaque ones is enough, and 255 everywhere is not. A rule
    // written as "mostly opaque" or "> N transparent texels" would pass the first half and fail here.
    std::vector<std::byte> almostOpaque = solidRgba(16, 10, 20, 30, 255);
    CHECK(toString(chooseTextureFormat(almostOpaque, true)) == toString(CookedTextureFormat::Bc1RgbSrgb));
    almostOpaque[(7U * 4U) + 3U] = static_cast<std::byte>(254);
    CHECK(toString(chooseTextureFormat(almostOpaque, true)) == toString(CookedTextureFormat::Bc3Srgb));
    CHECK(toString(chooseTextureFormat(almostOpaque, false)) == toString(CookedTextureFormat::Bc3Unorm));
}

TEST_CASE("chooseTextureFormat NEVER returns BC4, BC5 or either RGBA8 format (TK7)") {
    // Those three encode INTENT, which pixels cannot reveal. A red-only image and a grey image are
    // the two shapes a content scan is most likely to "recognise" as single-channel; neither may move
    // the answer off BC1/BC3.
    const std::array<std::vector<std::byte>, 4> corpus{
        solidRgba(16, 200, 0, 0, 255),      // red only
        solidRgba(16, 128, 128, 128, 255),  // grey
        solidRgba(16, 0, 0, 0, 255),        // black
        solidRgba(16, 7, 7, 7, 200),        // grey WITH alpha
    };
    std::size_t visited = 0;
    for (const std::vector<std::byte>& image : corpus) {
        for (const bool srgb : {true, false}) {
            const CookedTextureFormat chosen = chooseTextureFormat(image, srgb);
            CHECK(toString(chosen) != toString(CookedTextureFormat::Bc4Unorm));
            CHECK(toString(chosen) != toString(CookedTextureFormat::Bc5Unorm));
            CHECK(toString(chosen) != toString(CookedTextureFormat::Rgba8Unorm));
            CHECK(toString(chosen) != toString(CookedTextureFormat::Rgba8Srgb));
            CHECK(engine::assets::isSrgbCookedFormat(chosen) == srgb);
            ++visited;
        }
    }
    REQUIRE(visited == corpus.size() * 2U);
}

TEST_CASE("chooseTextureFormat on an empty or short span finds no alpha and answers BC1 (TK8)") {
    CHECK(toString(chooseTextureFormat({}, false)) == toString(CookedTextureFormat::Bc1RgbUnorm));
    CHECK(toString(chooseTextureFormat({}, true)) == toString(CookedTextureFormat::Bc1RgbSrgb));
    // Three bytes: there is no alpha byte at index 3 to read, so the scan must not read one.
    const std::vector<std::byte> partial = {std::byte{1}, std::byte{2}, std::byte{3}};
    CHECK(toString(chooseTextureFormat(partial, false)) == toString(CookedTextureFormat::Bc1RgbUnorm));
}

TEST_CASE("decodeImageRgba8 reads a hand-built 32-bit TGA back byte for byte (TK9)") {
    constexpr std::array<std::uint8_t, 24> SOURCE{
        10, 20, 30, 255, 40, 50, 60, 200, 70, 80, 90, 128,  // row 0
        11, 21, 31, 255, 41, 51, 61, 199, 71, 81, 91, 127,  // row 1
    };
    const std::vector<std::byte> tga = buildTga32(3, 2, SOURCE);
    const DecodedImage image = decodeImageRgba8(tga, ANY_DIMENSION);
    CHECK(image.error.empty());
    REQUIRE(image.width == 3);
    REQUIRE(image.height == 2);
    REQUIRE(image.rgba8.size() == SOURCE.size());
    for (std::size_t i = 0; i < SOURCE.size(); ++i) {
        INFO("byte ", i);
        CHECK(std::to_integer<std::uint32_t>(image.rgba8[i]) == SOURCE[i]);
    }
}

TEST_CASE("decodeImageRgba8 reads a BOTTOM-UP 24-bit BMP back TOP-LEFT origin (TK10)") {
    // The BMP is stored bottom-up on disk; the decoded image must come back top-left origin, which is
    // exactly what our own files' `KTXorientation rd` asserts about them. A decoder that handed back
    // the stored order would make every cooked BMP vertically mirrored, and nothing downstream could
    // tell.
    constexpr std::array<std::uint8_t, 18> SOURCE{
        10,  20,  30,  40,  50,  60,  70,  80,  90,   // row 0 (the TOP row)
        110, 120, 130, 140, 150, 160, 170, 180, 190,  // row 1 (the BOTTOM row)
    };
    const std::vector<std::byte> bmp = buildBmp24(3, 2, SOURCE);
    const DecodedImage image = decodeImageRgba8(bmp, ANY_DIMENSION);
    CHECK(image.error.empty());
    REQUIRE(image.width == 3);
    REQUIRE(image.height == 2);
    REQUIRE(image.rgba8.size() == 24);
    for (std::size_t texel = 0; texel < 6; ++texel) {
        INFO("texel ", texel);
        CHECK(std::to_integer<std::uint32_t>(image.rgba8[(texel * 4U) + 0U]) == SOURCE[texel * 3U]);
        CHECK(std::to_integer<std::uint32_t>(image.rgba8[(texel * 4U) + 1U]) == SOURCE[(texel * 3U) + 1U]);
        CHECK(std::to_integer<std::uint32_t>(image.rgba8[(texel * 4U) + 2U]) == SOURCE[(texel * 3U) + 2U]);
        CHECK(std::to_integer<std::uint32_t>(image.rgba8[(texel * 4U) + 3U]) == 255U);  // req_comp = 4
    }
}

TEST_CASE("decodeImageRgba8 reads the committed 5x3 PNG, opaque and odd in both axes (TK11)") {
    const std::string bytes = readFixture("texture-rgb-5x3.png");
    const DecodedImage image = decodeImageRgba8(asBytes(bytes), ANY_DIMENSION);
    CHECK(image.error.empty());
    REQUIRE(image.width == 5);
    REQUIRE(image.height == 3);
    REQUIRE(image.rgba8.size() == 5U * 3U * 4U);
    // The fixture is a 3-channel PNG: r = 40x + 20, g = 70y + 30, b = 200 - 30x, and stb fills alpha
    // with 255 because req_comp is 4. The four corners, written out.
    const auto texel = [&image](std::uint32_t x, std::uint32_t y, std::size_t channel) {
        return std::to_integer<std::uint32_t>(image.rgba8[((((std::size_t{y} * 5U) + x) * 4U) + channel)]);
    };
    CHECK(texel(0, 0, 0) == 20);
    CHECK(texel(0, 0, 1) == 30);
    CHECK(texel(0, 0, 2) == 200);
    CHECK(texel(4, 0, 0) == 180);
    CHECK(texel(0, 2, 1) == 170);
    CHECK(texel(4, 2, 2) == 80);
    // OPAQUE everywhere, which is what makes it the `auto` -> BC1 half of the pair.
    for (std::size_t i = 3; i < image.rgba8.size(); i += 4) {
        CHECK(std::to_integer<std::uint32_t>(image.rgba8[i]) == 255U);
    }
    CHECK(toString(chooseTextureFormat(image.rgba8, false)) == toString(CookedTextureFormat::Bc1RgbUnorm));
}

TEST_CASE("decodeImageRgba8 reads the committed 8x8 PNG and its alpha drives auto to BC3 (TK12)") {
    const std::string bytes = readFixture("texture-rgba-8x8.png");
    const DecodedImage image = decodeImageRgba8(asBytes(bytes), ANY_DIMENSION);
    CHECK(image.error.empty());
    REQUIRE(image.width == 8);
    REQUIRE(image.height == 8);
    REQUIRE(image.rgba8.size() == 8U * 8U * 4U);
    std::size_t belowOpaque = 0;
    for (std::size_t i = 3; i < image.rgba8.size(); i += 4) {
        if (std::to_integer<std::uint32_t>(image.rgba8[i]) < 255U) {
            ++belowOpaque;
        }
    }
    CHECK(belowOpaque == 16);  // the bottom-right 4x4 quadrant, at alpha 128
    CHECK(toString(chooseTextureFormat(image.rgba8, true)) == toString(CookedTextureFormat::Bc3Srgb));
    CHECK(toString(chooseTextureFormat(image.rgba8, false)) == toString(CookedTextureFormat::Bc3Unorm));
}

TEST_CASE("a truncated PNG decodes to an empty image with a non-empty reason (TK13)") {
    const std::string bytes = readFixture("texture-rgba-8x8.png");
    REQUIRE(bytes.size() > 20);
    const std::string truncated = bytes.substr(0, 20);
    const DecodedImage image = decodeImageRgba8(asBytes(truncated), ANY_DIMENSION);
    CHECK(image.rgba8.empty());
    CHECK(image.width == 0);
    CHECK(image.height == 0);
    // NON-EMPTY, and that is correction C11's whole point: this TU's stb_image implementation does
    // NOT define STBI_NO_FAILURE_STRINGS, unlike thumbnail_store.cpp's, so the CLI has a reason to
    // report rather than a bare "decode failed".
    CHECK_FALSE(image.error.empty());
}

TEST_CASE("an empty span and four bytes of garbage both refuse with a reason (TK14)") {
    const DecodedImage empty = decodeImageRgba8({}, ANY_DIMENSION);
    CHECK(empty.rgba8.empty());
    CHECK_FALSE(empty.error.empty());

    const std::array<std::uint8_t, 4> garbageBytes{0xDE, 0xAD, 0xBE, 0xEF};
    const std::vector<std::byte> garbage = toBytes(garbageBytes);
    const DecodedImage bad = decodeImageRgba8(garbage, ANY_DIMENSION);
    CHECK(bad.rgba8.empty());
    CHECK_FALSE(bad.error.empty());
}

TEST_CASE("maxDimension refuses an over-large image and names BOTH numbers (TK15)") {
    constexpr std::array<std::uint8_t, 24> SOURCE{
        10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255,  //
        11, 21, 31, 255, 41, 51, 61, 255, 71, 81, 91, 255,  //
    };
    const std::vector<std::byte> tga = buildTga32(3, 2, SOURCE);
    const DecodedImage refused = decodeImageRgba8(tga, 2);
    CHECK(refused.rgba8.empty());
    REQUIRE_FALSE(refused.error.empty());
    // Both numbers, so the message is actionable. Their presence is also the only observable proof
    // that the refusal came from OUR header check rather than from stb_image: stbi_info_from_memory
    // is what ran, and it never allocates the pixels.
    CHECK(refused.error.find("3x2") != std::string::npos);
    CHECK(refused.error.find("2-texel") != std::string::npos);
    // And the SAME image at a bound it fits under decodes normally, so the case is not merely
    // asserting that a small bound breaks everything.
    const DecodedImage accepted = decodeImageRgba8(tga, 3);
    CHECK(accepted.error.empty());
    CHECK(accepted.rgba8.size() == SOURCE.size());
}

TEST_CASE("a successful decode always returns exactly width * height * 4 bytes (TK16)") {
    std::size_t visited = 0;
    for (const std::string& name : {std::string("texture-rgb-5x3.png"), std::string("texture-rgba-8x8.png")}) {
        const std::string bytes = readFixture(name);
        const DecodedImage image = decodeImageRgba8(asBytes(bytes), ANY_DIMENSION);
        REQUIRE(image.error.empty());
        CHECK(image.rgba8.size() == static_cast<std::size_t>(image.width) * image.height * 4U);
        ++visited;
    }
    // A hand-built TGA and BMP too, so the property is not only about one decoder.
    constexpr std::array<std::uint8_t, 24> RGBA{
        1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255, 13, 14, 15, 255, 16, 17, 18, 255,
    };
    constexpr std::array<std::uint8_t, 18> RGB{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
    };
    for (const std::vector<std::byte>& blob : {buildTga32(3, 2, RGBA), buildBmp24(3, 2, RGB)}) {
        const DecodedImage image = decodeImageRgba8(blob, ANY_DIMENSION);
        REQUIRE(image.error.empty());
        CHECK(image.rgba8.size() == static_cast<std::size_t>(image.width) * image.height * 4U);
        ++visited;
    }
    REQUIRE(visited == 4);
}

TEST_CASE("the whole adapter path composes: decode -> choose -> cook -> parse (TK17)") {
    // The ONE case that proves the two layers compose. Everything above this line is about one
    // function; this is the pipeline the CLI runs, minus the process boundary.
    const std::string bytes = readFixture("texture-rgb-5x3.png");
    const DecodedImage image = decodeImageRgba8(asBytes(bytes), engine::assets::MAX_TEXTURE_DIMENSION);
    REQUIRE(image.error.empty());
    REQUIRE(image.width == 5);
    REQUIRE(image.height == 3);

    const CookedTextureFormat format = chooseTextureFormat(image.rgba8, false);
    CHECK(toString(format) == toString(CookedTextureFormat::Bc1RgbUnorm));

    engine::assets::TextureCookInput input;
    input.width = image.width;
    input.height = image.height;
    input.rgba8 = image.rgba8;
    input.format = format;
    const engine::assets::TextureCookResult cooked = engine::assets::cookTexture(input);
    REQUIRE(cooked.status == engine::assets::TextureCookStatus::Ok);
    REQUIRE_FALSE(cooked.bytes.empty());

    const engine::assets::CookedTextureParse parse = engine::assets::parseCookedTexture(cooked.bytes);
    CHECK(parse.status == engine::assets::CookedTextureStatus::Ok);
    CHECK(parse.message.empty());
    CHECK(toString(parse.view.format()) == toString(CookedTextureFormat::Bc1RgbUnorm));
    CHECK(parse.view.width() == 5);
    CHECK(parse.view.height() == 3);
    CHECK(parse.view.levelCount() == 3);
}

TEST_CASE("the stb macro block keeps both STATIC macros and deliberately OMITS the failure-string one (TK18)") {
    // The assimp_import_test.cpp:846 precedent, extended. The first half is why a SECOND stb_image
    // implementation is legal in this tree at all: STB_IMAGE_STATIC gives it internal linkage, so it
    // cannot collide with thumbnail_store.cpp's or with the one the assimp port compiles.
    const std::string code = strippedEditorSource("texture_cook_source.cpp");
    CHECK(code.find("#define STB_IMAGE_IMPLEMENTATION") != std::string::npos);
    CHECK(code.find("#define STB_IMAGE_STATIC") != std::string::npos);
    CHECK(code.find("#define STBI_NO_STDIO") != std::string::npos);

    // The second half is correction C11, and it is what stops somebody copying thumbnail_store.cpp's
    // macro block wholesale and silently losing every decode reason -- the reason TK13 asserts is
    // non-empty. Read from COMMENT-STRIPPED text: this file documents the omission in prose, which an
    // unstripped grep would read as the definition it is warning against.
    CHECK(code.find("STBI_NO_FAILURE_STRINGS") == std::string::npos);

    // And the sibling TU still defines it, so the two are provably different rather than accidentally
    // the same. Its own comment there says why: it reports a STATE, not text.
    const std::string thumbnail = strippedEditorSource("thumbnail_store.cpp");
    CHECK(thumbnail.find("#define STB_IMAGE_STATIC") != std::string::npos);
    CHECK(thumbnail.find("#define STBI_NO_FAILURE_STRINGS") != std::string::npos);
}

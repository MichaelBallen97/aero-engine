#pragma once
// Aero Engine — the cooked texture container v1 (task 3.3.2). A STRICT SUBSET OF KHRONOS KTX2, and a
// real one: `ktx info`, `ktx validate` and RenderDoc open these files.
//
// PURE: no disk, no <fstream>, no <filesystem>, no logging, no third party, no GPU, no per-OS macro,
// AND NO FLOATING POINT AT ALL -- no float, no double, no <cmath>, no runtime table generation. That
// last one is not style: a float in this subsystem is a byte-identity hazard on three CI lanes,
// because FMA contraction and libm both differ across them, and task 3.3.3 turns cross-platform
// byte-identity into a CI job for BOTH cook kinds.
//
// The normative specification is docs/09-file-formats.md section 10. THIS HEADER IS NOT THE SPEC --
// if the two disagree, docs/09 wins and one of them is a bug.
//
// INTEROP IS ONE-DIRECTIONAL, DELIBERATELY. Our files open in any conforming KTX2 reader. Arbitrary
// third-party KTX2 files do NOT open in us: a valid file from another tool may legitimately differ in
// its DFD (the spec allows a sample's KHR_DF_SAMPLE_DATATYPE_LINEAR qualifier bit to differ), carry
// key/value data we do not write, or use a vkFormat outside our eight. This is a first-party cooked-
// asset reader, not a general loader. A general KTX2 IMPORTER is a different feature.
//
// BYTES ARE FORMED THROUGH cooked_mesh.hpp's EIGHT PRIMITIVES AND NOWHERE ELSE. There is no second
// set in this subsystem: a second set is a second place for a big-endian mistake to hide, and the
// static_assert only protects the set it is attached to. putF32/getF32 go unused here (KTX2 has no
// float field in our subset), and that is not a reason to split the header.
#include <aero/assets/cooked_mesh.hpp>  // the eight byte primitives + their endianness static_assert
#include <aero/core/guid.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>  // std::lcm. NOT <cmath>: std::lcm is integer-only, so the no-floating-point rule
                    // above is untouched by this include. Do not "tidy" it away.
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

// ---- identity ----------------------------------------------------------------------------------
// «KTX 20» wrapped in the bytes that make a text-mode write, a stray CR/LF translation or a 7-bit
// transport corrupt the file at byte 0 rather than silently deeper in.
inline constexpr std::array<std::uint8_t, 12> KTX2_IDENTIFIER{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                                              0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

// Bumped when the same input cooks to DIFFERENT BYTES -- a filter change, an encoder improvement, a
// table fix. It never gates a parse. It is visible in the file only through KTXwriter's value, which
// is why the two constants below must move together and the KTX2_KVD_BYTES static_assert enforces it.
inline constexpr std::uint32_t COOKED_TEXTURE_COOKER_VERSION = 1;
// Names the COOK, never the CLI: engine/assets must not know that aero_cooker exists, and a
// caller-supplied writer string would be a determinism surface (two callers, two different files).
inline constexpr std::string_view COOKED_TEXTURE_WRITER_ID = "Aero Engine texture cook 1";

// ---- the format enum. FROZEN, and the values ARE Khronos's. --------------------------------------
// Unlike rhi::VertexFormat (whose implicit values task 3.3.1 refused to serialize for exactly this
// reason), these are a published, immutable EXTERNAL contract, so there is no second enum to keep in
// sync and no mapping table.
//
// There is deliberately NO Bc4Srgb and NO Bc5Srgb: Vulkan defines neither -- the enum runs 139
// BC4_UNORM, 140 BC4_SNORM, 141 BC5_UNORM, 142 BC5_SNORM with no sRGB value among them -- which is
// what makes "an sRGB normal map" UNSPELLABLE rather than merely rejected.
//
// VK_FORMAT_BC1_RGBA_UNORM_BLOCK (133) and _SRGB_ (134) exist and are DELIBERATELY ABSENT: those are
// the punch-through-alpha variants, and this cook's BC1 encoder always emits opaque four-colour
// blocks. Do not "complete the set" -- a value here is a promise the encoder must keep.
//
// The underlying type is the FORMAT's width (the file's vkFormat field is a u32), not a size
// optimization; a narrower type would change what is written to disk.
// NOLINTNEXTLINE(performance-enum-size)
enum class CookedTextureFormat : std::uint32_t {
    Rgba8Unorm = 37,
    Rgba8Srgb = 43,
    Bc1RgbUnorm = 131,
    Bc1RgbSrgb = 132,
    Bc3Unorm = 137,
    Bc3Srgb = 138,
    Bc4Unorm = 139,
    Bc5Unorm = 141,
};

// ---- the total predicates. Every switch has NO `default:` ---------------------------------------
// so a future enumerator is a -Wswitch failure on the Linux lane rather than a silent zero (the
// importStatusLabel / cookedVertexFormatBytes precedent).

// THE ONE EXCEPTION to the rule above, and the reason is structural: this takes a RAW u32 straight
// out of a hostile file, so it cannot switch over the enum's own domain without first deciding the
// question it is being asked. The value is cast in -- legal and never UB, because the enum has a
// fixed underlying type, so every u32 is a valid value of it -- and the eight case labels still give
// -Wswitch its coverage check. The trailing `return false` is what the cast-in shape costs.
[[nodiscard]] constexpr bool isCookedTextureFormat(std::uint32_t value) noexcept {
    switch (static_cast<CookedTextureFormat>(value)) {
        case CookedTextureFormat::Rgba8Unorm:
        case CookedTextureFormat::Rgba8Srgb:
        case CookedTextureFormat::Bc1RgbUnorm:
        case CookedTextureFormat::Bc1RgbSrgb:
        case CookedTextureFormat::Bc3Unorm:
        case CookedTextureFormat::Bc3Srgb:
        case CookedTextureFormat::Bc4Unorm:
        case CookedTextureFormat::Bc5Unorm:
            return true;
    }
    return false;
}

// THE colour space, carried by the format enumerator and nowhere else. There is no `bool srgb`
// anywhere in this subsystem: folding the colour space into the format is what makes {Bc5Unorm,
// srgb=true} unspellable rather than something to validate, log about and eventually get wrong.
[[nodiscard]] constexpr bool isSrgbCookedFormat(CookedTextureFormat format) noexcept {
    switch (format) {
        case CookedTextureFormat::Rgba8Srgb:
        case CookedTextureFormat::Bc1RgbSrgb:
        case CookedTextureFormat::Bc3Srgb:
            return true;
        case CookedTextureFormat::Rgba8Unorm:
        case CookedTextureFormat::Bc1RgbUnorm:
        case CookedTextureFormat::Bc3Unorm:
        case CookedTextureFormat::Bc4Unorm:
        case CookedTextureFormat::Bc5Unorm:
            return false;
    }
    return false;
}

[[nodiscard]] constexpr std::uint32_t cookedTextureBlockWidth(CookedTextureFormat format) noexcept {
    switch (format) {
        // The two uncompressed formats share an arm because they share a block EXTENT, not because
        // they are the same thing -- one is sRGB-encoded and the other is not. Merged rather than
        // written out twice only because bugprone-branch-clone rejects consecutive identical
        // branches; the same applies to the six BCn arms below.
        case CookedTextureFormat::Rgba8Unorm:
        case CookedTextureFormat::Rgba8Srgb:
            return 1;
        case CookedTextureFormat::Bc1RgbUnorm:
        case CookedTextureFormat::Bc1RgbSrgb:
        case CookedTextureFormat::Bc3Unorm:
        case CookedTextureFormat::Bc3Srgb:
        case CookedTextureFormat::Bc4Unorm:
        case CookedTextureFormat::Bc5Unorm:
            return 4;
    }
    return 0;
}

// Every BCn block in this subset is SQUARE, so this repeats cookedTextureBlockWidth exactly today.
// It is a separate function rather than an alias because the two are separate FACTS about a format:
// ASTC (task 6.3.1) has 4x4, 5x4, 6x5 ... blocks, and a consumer that assumed one call answered both
// questions would then be silently wrong for every non-square one.
[[nodiscard]] constexpr std::uint32_t cookedTextureBlockHeight(CookedTextureFormat format) noexcept {
    switch (format) {
        case CookedTextureFormat::Rgba8Unorm:
        case CookedTextureFormat::Rgba8Srgb:
            return 1;
        case CookedTextureFormat::Bc1RgbUnorm:
        case CookedTextureFormat::Bc1RgbSrgb:
        case CookedTextureFormat::Bc3Unorm:
        case CookedTextureFormat::Bc3Srgb:
        case CookedTextureFormat::Bc4Unorm:
        case CookedTextureFormat::Bc5Unorm:
            return 4;
    }
    return 0;
}

// Bytes per BLOCK, never per texel -- which is exactly the contract rhi::texelBlockSize does NOT
// have (it is documented as bytes per TEXEL), and exactly why extending rhi::TextureFormat with BCn
// is a contract change rather than an enumerator addition. That belongs to task 3.4.1.
[[nodiscard]] constexpr std::uint32_t cookedTextureBlockBytes(CookedTextureFormat format) noexcept {
    switch (format) {
        case CookedTextureFormat::Rgba8Unorm:
        case CookedTextureFormat::Rgba8Srgb:
            return 4;
        case CookedTextureFormat::Bc1RgbUnorm:
        case CookedTextureFormat::Bc1RgbSrgb:
            return 8;
        case CookedTextureFormat::Bc3Unorm:
        case CookedTextureFormat::Bc3Srgb:
            return 16;
        case CookedTextureFormat::Bc4Unorm:
            return 8;
        case CookedTextureFormat::Bc5Unorm:
            return 16;
    }
    return 0;
}

// KTX2's mipPadding rule: a level's byteOffset is aligned to lcm(texelBlockSize, 4).
//
// WRITTEN AS THE lcm IT IS, not as `return cookedTextureBlockBytes(format);`. The two are equal for
// all eight of today's formats because every one of 4, 8 and 16 is already a multiple of 4 -- but
// that is a COINCIDENCE OF THIS SUBSET, not a general truth, and a future 12-byte-block format would
// silently get the wrong alignment. <numeric>'s std::lcm is integer-only and constexpr.
[[nodiscard]] constexpr std::uint32_t cookedTextureLevelAlignment(CookedTextureFormat format) noexcept {
    return static_cast<std::uint32_t>(std::lcm(cookedTextureBlockBytes(format), 4U));
}

[[nodiscard]] std::string_view toString(CookedTextureFormat format) noexcept;

// The frozen Data Format Descriptor for `format`, byte for byte. TOTAL: every enumerator has one and
// every span is 44, 60 or 92 bytes long. The writer emits these verbatim and the parser compares
// against them -- which is what makes our reader STRICTER than KTX2 itself and interop
// one-directional (see the header note above). The tables and their derivation live in the .cpp.
[[nodiscard]] std::span<const std::uint8_t> cookedTextureDescriptorBytes(CookedTextureFormat format) noexcept;

// ---- sizes. The ONLY sizes. `sizeof` is never taken of an on-disk record (INV-C5, inherited). -----
inline constexpr std::size_t KTX2_HEADER_BYTES = 80;        // identifier + the 9-u32 header + the Index
inline constexpr std::size_t KTX2_LEVEL_RECORD_BYTES = 24;  // three u64
inline constexpr std::size_t KTX2_KVD_BYTES = 120;          // three keys, ALWAYS -- see below
inline constexpr std::size_t KTX2_DFD_BYTES_1_SAMPLE = 44;
inline constexpr std::size_t KTX2_DFD_BYTES_2_SAMPLE = 60;
inline constexpr std::size_t KTX2_DFD_BYTES_4_SAMPLE = 92;

namespace detail {
// KTX2 pads a key/value record's payload to a four-byte boundary; the writer's single padding site
// and the parser's record walk both need the same arithmetic, so it is spelled once.
[[nodiscard]] constexpr std::size_t align4(std::size_t value) noexcept { return ((value + 3) / 4) * 4; }
// One record is `4 (keyAndValueByteLength) + align4(keyBytes + valueBytes)`, with both lengths
// INCLUDING their NUL terminator.
[[nodiscard]] constexpr std::size_t kvdRecordBytes(std::size_t keyBytes, std::size_t valueBytes) noexcept {
    return 4 + align4(keyBytes + valueBytes);
}
}  // namespace detail

// KTX2_KVD_BYTES is a CLOSED FORM, not a magic number:
//   AeroSourceGuid  key 15 + value 33 (32 lowercase hex + NUL)  -> record 52
//   KTXorientation  key 15 + value  3 ("rd" + NUL)              -> record 24
//   KTXwriter       key 10 + value 27 (the writer id + NUL)     -> record 44
// A bump of COOKED_TEXTURE_COOKER_VERSION that lengthens the writer string moves the third row, and
// this static_assert is what turns that into a BUILD FAILURE instead of a silently wrong
// kvdByteLength and every offset after it.
static_assert(KTX2_KVD_BYTES == detail::kvdRecordBytes(15, 33) + detail::kvdRecordBytes(15, 3) +
                                    detail::kvdRecordBytes(10, COOKED_TEXTURE_WRITER_ID.size() + 1),
              "KTX2_KVD_BYTES no longer matches the three records it is the sum of");

// ---- frozen caps. Every one checked BEFORE the allocation it bounds. ----------------------------
inline constexpr std::uint32_t MAX_TEXTURE_DIMENSION = 16384;
inline constexpr std::uint32_t MAX_TEXTURE_LEVELS = 15;  // floor(log2(16384)) + 1
// A DERIVATION, not a restatement: MAX_TEXTURE_DIMENSION == 2^(MAX_TEXTURE_LEVELS - 1) is exactly
// "MAX_TEXTURE_LEVELS is the full chain length of the largest legal image", and it also pins the
// dimension cap to a power of two, which the shift-based level loop assumes.
static_assert(MAX_TEXTURE_DIMENSION == 1U << (MAX_TEXTURE_LEVELS - 1),
              "MAX_TEXTURE_LEVELS must stay floor(log2(MAX_TEXTURE_DIMENSION)) + 1");

// A cheap parser early-out AND a real cook refusal. The consequence is stated rather than
// discovered: a 16384^2 Rgba8* cook needs 1.07 GB for level 0 alone and IS REFUSED by this cap. The
// maximum dimension is reachable for BC1/BC4 (~179 MB with the full chain) and BC3/BC5 (~358 MB) and
// NOT for RGBA8 -- deliberately, because the uncompressed path is an escape hatch for small textures
// rather than a way to ship a 1 GB artifact. The refusal message names the cap, not the format.
inline constexpr std::uint64_t MAX_COOKED_TEXTURE_BYTES = 512ULL * 1024 * 1024;

// ---- the parse statuses -------------------------------------------------------------------------
enum class CookedTextureStatus : std::uint8_t {
    Ok = 0,
    TooSmall,           // shorter than the 80-byte header, or the level index does not fit
    BadIdentifier,      //
    UnsupportedFormat,  // vkFormat outside the eight, or typeSize != 1
    UnsupportedShape,   // pixelDepth != 0, layerCount != 0, faceCount != 1, or a PARTIAL mip pyramid
                        // (a levelCount strictly between 1 and the image's full chain -- over-cap and
                        // zero counts are CapExceeded, since those are range violations and this is
                        // not: 2 is inside 1..4 for an 8x8 image and still not a shape v1 stores)
    Supercompressed,    // supercompressionScheme != 0 -- a DISTINCT status, because "this is a Basis
                        // or Zstd file" is a genuinely different thing to tell a user than "corrupt"
    CapExceeded,        // a dimension, levelCount, or the buffer itself is over a frozen cap
    BadTable,           // the DFD/KVD/level-index REGIONS do not fit or do not tile
    BadRange,           // a level's offset/length is misaligned, inconsistent or leaves the buffer
    BadDescriptor,      // the DFD does not byte-match the frozen table for this vkFormat
};
// A switch with NO `default:` (the cookedMeshStatusLabel precedent).
[[nodiscard]] std::string_view cookedTextureStatusLabel(CookedTextureStatus status) noexcept;

// ---- the parsed view ----------------------------------------------------------------------------
struct CookedTextureLevel {
    std::uint64_t byteOffset = 0;  // absolute into the parsed buffer, cookedTextureLevelAlignment-aligned
    std::uint64_t byteLength = 0;
};

struct CookedTextureParse;

// LIFETIME: `bytes` IS the buffer handed to parseCookedTexture, retained as a span. Every level's
// offset is absolute into it, so it means exactly what the file says and needs no rebasing. The level
// records are an OWNED copy (bounded by MAX_TEXTURE_LEVELS, so at most 15 of them); the level data is
// NEVER copied -- that is the whole promise of this format. A CookedTextureView outliving its buffer
// is a dangling read, and the only defences are this comment and the accessors below, which are the
// sanctioned way to reach level data. Nothing else should index `bytes` by hand.
class CookedTextureView {
public:
    [[nodiscard]] CookedTextureFormat format() const noexcept { return formatValue; }
    [[nodiscard]] std::uint32_t width() const noexcept { return widthValue; }
    [[nodiscard]] std::uint32_t height() const noexcept { return heightValue; }
    [[nodiscard]] std::uint32_t levelCount() const noexcept { return static_cast<std::uint32_t>(levels.size()); }
    // Nil if the file carried no AeroSourceGuid key, or carried a malformed one. Neither is a refusal:
    // a missing or unreadable provenance key is not a corrupt image.
    [[nodiscard]] Guid sourceGuid() const noexcept { return sourceGuidValue; }
    [[nodiscard]] const CookedTextureLevel& levelRecord(std::uint32_t level) const noexcept;

    // 0 if `level` is out of range. In range the shift is bounded by MAX_TEXTURE_LEVELS - 1 == 14, so
    // it can never reach the width of a std::uint32_t -- shifting by 32 or more would be UB and UBSan
    // catches it on the Debug lanes, which is why the bound is stated here rather than assumed.
    [[nodiscard]] std::uint32_t levelWidth(std::uint32_t level) const noexcept;
    [[nodiscard]] std::uint32_t levelHeight(std::uint32_t level) const noexcept;

    // TOTAL on a view parseCookedTexture returned Ok for. An out-of-range level returns an EMPTY span
    // rather than reading -- a caller bug must not become a read. The fits() re-check inside is
    // deliberate belt-and-braces: it can never fire on an Ok view, and it is what makes the accessor
    // total against a HAND-CONSTRUCTED view, which a test can build and a caller could.
    [[nodiscard]] std::span<const std::byte> levelBytes(std::uint32_t level) const noexcept;

private:
    friend CookedTextureParse parseCookedTexture(std::span<const std::byte> bytes);

    CookedTextureFormat formatValue = CookedTextureFormat::Bc1RgbSrgb;
    std::uint32_t widthValue = 0;
    std::uint32_t heightValue = 0;
    Guid sourceGuidValue;
    std::vector<CookedTextureLevel> levels;  // at most MAX_TEXTURE_LEVELS
    std::span<const std::byte> bytes;
};

struct CookedTextureParse {
    CookedTextureStatus status = CookedTextureStatus::Ok;
    std::string message;     // "" IFF status == Ok
    CookedTextureView view;  // meaningful only when status == Ok
};

// NEVER THROWS. NEVER READS A FILE. NEVER LOGS.
//
// Written to the hostile-input standard from day one, because at Phase 5 this reads bytes out of a
// .pak that may have been shipped, patched, truncated by a failed download, or crafted. Every range
// check is a SUBTRACTION against the known-good size, never an addition that can wrap; nothing is
// allocated before the count it is allocating for has been checked against a frozen cap.
//
// DELIBERATELY STRICTER THAN KTX2 IN ONE PLACE: the DFD must byte-match the frozen table for the
// declared vkFormat. The spec explicitly permits a sample's KHR_DF_SAMPLE_DATATYPE_LINEAR qualifier
// bit to differ, so a perfectly valid third-party file can be refused here. That is correct for a
// first-party cooked-asset reader and wrong for a general loader, and this is the former.
//
// THREE THINGS IT DELIBERATELY DOES NOT CHECK:
//   1. that the levels do not overlap each other, the tables or the key/value data. Every read goes
//      through levelBytes(level), which is bounds-checked against the buffer, so an overlap is a
//      wrong PICTURE, never a memory error -- the same reasoning and the same Phase 5 trigger as
//      docs/09 section 9.12's second residual.
//   2. that there are no trailing bytes after the last level. The writer emits none; the reader
//      tolerates them, exactly as the mesh reader tolerates non-zero trailing padding, so a future
//      writer that pads differently is not locked out of a format whose meaningful content it
//      reproduced.
//   3. the block CONTENTS. There is no such thing as an invalid BCn block; every 8- or 16-byte
//      pattern decodes to something.
[[nodiscard]] CookedTextureParse parseCookedTexture(std::span<const std::byte> bytes);

}  // namespace engine::assets

#pragma once
// Aero Engine — the texture cook (task 3.3.2): RGBA8 texels in, a conforming KTX2 file out. The
// PRODUCER half of the cooked texture container; cooked_texture.hpp is the FORMAT half, and a runtime
// that never cooks anything needs only that one.
//
// PURE: no disk, no <fstream>, no <filesystem>, no logging, no third party, no GPU, no per-OS macro,
// AND NO FLOATING POINT AT ALL. The sRGB transfer function is a pow, so it is evaluated exactly ZERO
// times at runtime: both gamma tables are constexpr arrays of committed literals in the .cpp,
// generated once by a throwaway script recorded in docs/10-engineering-log.md. A table built at
// startup with std::pow would put a libm implementation into the output bytes, which is worse than
// the FMA-contraction hazard the first-party block encoders exist to avoid -- libm differs between
// three C libraries rather than between two compilers' contraction policies.
//
// THE COOK CONVERTS NOTHING beyond compressing and mipping. No premultiplied alpha, no vertical flip,
// no resize to power-of-two, no crop, no colour-space conversion beyond the sRGB/linear round trip the
// mip filter needs internally, no normal-map renormalization after filtering, no channel swizzle, no
// alpha-coverage preservation -- AND NO SETTING FOR ANY OF THEM. There is no TextureCookSettings type,
// for the same reason there is no MeshCookSettings: an empty settings struct is a shape that invites a
// field.
#include <aero/assets/cooked_texture.hpp>
#include <aero/core/guid.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::assets {

struct TextureCookInput {
    Guid sourceGuid;  // may be nil; nil is legal and deterministic
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::span<const std::byte> rgba8;  // EXACTLY width * height * 4, row-major, TOP-LEFT ORIGIN
    CookedTextureFormat format = CookedTextureFormat::Bc1RgbSrgb;
    bool generateMips = true;
};

struct TextureCookStats {
    std::uint32_t levelCount = 0;
    std::uint32_t blockCount = 0;  // summed over every level
    std::uint64_t byteSize = 0;
    std::uint64_t sourceByteSize = 0;  // width * height * 4, for a ratio a caller may want to report
};

// BINARY, deliberately unlike MeshCookStatus's three values. The mesh cook can DROP primitives and
// still produce a coherent, smaller artifact, so it needs a Truncated state. A texture cannot be
// partially cooked -- there is no "some of the image" -- so `bytes` is empty IFF Refused, which is a
// stronger and simpler invariant, and the CLI's error path never has to distinguish "coherent but
// smaller" from "nothing".
enum class TextureCookStatus : std::uint8_t { Ok = 0, Refused };

struct TextureCookResult {
    TextureCookStatus status = TextureCookStatus::Ok;
    std::string message;  // "" IFF Ok
    // v1's cook emits NO warning at all. The vector and its uncapped total exist because the shape is
    // the house's and because the first thing that wants one -- a future "this image has no alpha, so
    // BC3 wastes half the file" note -- should not have to change the result type. Do NOT write a
    // synthetic warning to make the field look used.
    std::vector<std::string> warnings;  // capped at MAX_COOK_WARNINGS, reused from cooked_mesh.hpp
    std::size_t warningTotal = 0;       // UNCAPPED
    std::vector<std::byte> bytes;       // EMPTY IFF status == Refused
    TextureCookStats stats;
};

// NEVER THROWS. NEVER READS A FILE. NEVER LOGS. NO FLOATING POINT.
//
// validate -> level count -> mip chain (level p from level p-1) -> per level: block loop -> assemble.
// FORMAT SELECTION IS NOT A STEP: the format arrives in the input, because the policy that picks it
// (the editor's chooseTextureFormat) lives one layer up and is called BEFORE this. Mechanism inside,
// policy outside -- task 3.3.1's rule, restated.
[[nodiscard]] TextureCookResult cookTexture(const TextureCookInput& input);

// A TESTING SEAM, not a public API. These four are the pieces aero_tests drives directly -- the mip
// arithmetic, the two gamma directions and the filter -- because each is independently checkable and
// none is worth re-deriving through a whole cook. A consumer outside aero_tests calling into
// `detail` is a review finding, not a supported use.
namespace detail {

// floor(log2(max(width, height))) + 1, by an integer shift loop and never std::log2. 0 for a
// degenerate 0-extent input, which the cook refuses long before it gets here.
[[nodiscard]] std::uint32_t mipLevelCount(std::uint32_t width, std::uint32_t height) noexcept;

// The two committed gamma tables, reachable only through these. SRGB_TO_LINEAR is 16-bit fixed point
// and its inverse is a binary search over the 255 midpoints between consecutive forward entries, so
// the pair round-trips all 256 values BY CONSTRUCTION rather than by luck.
[[nodiscard]] std::uint16_t srgbToLinear(std::uint8_t encoded) noexcept;
[[nodiscard]] std::uint8_t linearToSrgb(std::uint16_t linear) noexcept;

// Halves `src` into `dst` with the integer polyphase box filter. `dst` must be exactly
// max(1, srcWidth >> 1) * max(1, srcHeight >> 1) * 4 bytes and `src` exactly srcWidth * srcHeight * 4;
// a mismatch writes NOTHING, which makes a caller bug an empty result rather than a read.
//
// `srgb` decides the colour space of R, G and B only. ALPHA IS ALWAYS AVERAGED AS STORED, in both
// cases: alpha is coverage, never a gamma-encoded colour.
void downsampleRgba8(std::span<const std::byte> src, std::uint32_t srcWidth, std::uint32_t srcHeight,
                     std::span<std::byte> dst, bool srgb) noexcept;

}  // namespace detail

}  // namespace engine::assets

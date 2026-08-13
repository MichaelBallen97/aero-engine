#pragma once
// Aero Engine — the image bytes -> texture cook adapter (task 3.3.2). The editor's ENTIRE contribution
// to this task: three pure functions, a decode, and no UI. No panel change, no menu item, no
// cook-on-import, no Library/ write, no .meta change, no EditorApp edit -- asset_database.cpp,
// asset_view.{hpp,cpp}, asset_meta.{hpp,cpp}, import_details_panel.cpp, editor_app.cpp,
// model_import_session.{hpp,cpp} and thumbnail_store.{hpp,cpp} are all BYTE-IDENTICAL to main
// afterwards (AC-44).
//
// It lives in /editor rather than inside tools/cooker so it is exercised by aero_editor_shell_test
// against real inputs, and so the editor's own future cook path and the CLI share ONE mapping rather
// than two. It CANNOT live in engine/assets: stb_image is a vcpkg package and aero_assets links none,
// which is what makes that target's PRIVATE links a real compile-time boundary (R12).
//
// PURE: no disk (stbi_load_from_memory only -- STBI_NO_STDIO removes the path-taking overload from
// the TU entirely), no ImGui, no SDL, no <filesystem>, no logging.
#include <aero/assets/cooked_texture.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

// The cap on the COMPRESSED SOURCE FILE the CLI reads before handing it here, sitting beside the
// decode it bounds rather than in the tool. 64 MiB, matching thumbnail_cache.hpp's
// MAX_THUMBNAIL_SOURCE_BYTES rather than model_import.hpp's 256 MiB MAX_MODEL_FILE_BYTES: this bounds
// a compressed file whose DECODED pixel count is bounded separately and per-axis by
// decodeImageRgba8's own maxDimension, so a second, generous byte ceiling would buy nothing. The
// number is normative -- docs/09 section 10 states it.
inline constexpr std::uint64_t MAX_TEXTURE_FILE_BYTES = 64ULL * 1024ULL * 1024ULL;

struct DecodedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> rgba8;  // EXACTLY width * height * 4 on success; EMPTY on failure
    std::string error;             // "" IFF the decode succeeded
};

// The SECOND stb_image implementation TU in this tree. `maxDimension` refuses an over-large image
// BEFORE the decode allocates -- stbi_info_from_memory reads only the header, so a 40000x40000 PNG
// costs a header parse rather than 6.4 GB. Pass assets::MAX_TEXTURE_DIMENSION, which is the bound the
// cook itself will apply.
//
// NOT the same bound as thumbnail_cache.hpp's MAX_THUMBNAIL_SOURCE_PIXELS: that one caps a PIXEL
// COUNT for a decode whose output is thrown away at 128x128, this one caps a PER-AXIS extent for a
// decode whose output is cooked. They bound different things and must not be unified.
//
// NEVER THROWS. The decoded pointer never escapes this function: it is owned for the length of the
// copy and freed on every path, including the early returns.
[[nodiscard]] DecodedImage decodeImageRgba8(std::span<const std::byte> fileBytes, std::uint32_t maxDimension);

// The AUTO rule, pure and separable from the cook on purpose: BC3 iff ANY texel's alpha is below 255,
// else BC1, in the requested colour space. It NEVER returns Bc4Unorm, Bc5Unorm, Rgba8Unorm or
// Rgba8Srgb -- those encode INTENT, which pixels cannot reveal. A texture whose green and blue happen
// to equal red in one asset is not a single-channel texture, and a cook that silently discarded
// channels on a content scan would eventually discard one that mattered.
//
// Takes the byte span rather than a DecodedImage, so a test can drive it with four bytes. A span
// shorter than four bytes, or an empty one, finds no alpha and answers BC1.
[[nodiscard]] assets::CookedTextureFormat chooseTextureFormat(std::span<const std::byte> rgba8, bool srgb) noexcept;

// The SEVEN claimed extensions: png jpg jpeg tga bmp gif psd. A THIRD extension table, deliberately
// NOT derived from asset_view.cpp's THUMBNAIL_DECODABLE_EXTENSIONS (whose own comment sets that
// precedent) -- .hdr is stb-decodable and is deliberately ABSENT here, because stbi_load does not
// FAIL on a Radiance file: it silently applies a fixed gamma-2.2 tone map and hands back 8-bit LDR
// bytes, which cooks to a plausible artifact that is quietly wrong. Deriving one table from the other
// is exactly what would let a future edit to one silently move the other.
[[nodiscard]] bool isCookableTextureName(std::string_view fileName) noexcept;

// True for ".hdr" alone, so a refusal can name the reason rather than saying "no importer claims it".
[[nodiscard]] bool isHdrTextureName(std::string_view fileName) noexcept;

}  // namespace engine::editor

// Aero Engine — the shared source-image-file -> GPU-texture chain (task 3.1.5, §D-14/§0.25). See
// texture_load.hpp for the ownership rule. The body below is MaterialPreview::loadOneTexture's own
// chain, moved rather than rewritten: every comment, every arm and every one of the six user-facing
// sentences is the text 3.4.2 shipped, so the preview's messages are byte-identical after the
// extraction and its existing cases stay green untouched.
#include "texture_load.hpp"

#include <aero/assets/cooked_texture.hpp>
#include <aero/assets/texture_cook.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/project_files.hpp>
#include <aero/editor/text_file.hpp>
#include <aero/editor/texture_cook_source.hpp>
#include <aero/render/texture_upload.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace engine::editor {

namespace {

// A cooked artifact is not a SOURCE, so it belongs to none of the four extension tables the editor
// already keeps -- asset_view's Texture kind (10 extensions, presentation), its thumbnail-decodable
// subset (stb), texture_cook_source's cookable set (7, and deliberately not .hdr) and the importer
// tables. Each answers a different question; this one answers "is this already the thing the bridge
// uploads?", and deriving it from any of the others is exactly what would let an edit to one silently
// move this. ASCII-case-folded, the extensionEqualsFolded posture.
[[nodiscard]] bool hasKtx2Extension(std::string_view leaf) noexcept {
    constexpr std::string_view SUFFIX = ".ktx2";
    if (leaf.size() <= SUFFIX.size()) {
        return false;
    }
    // Indexed rather than substr'd: substr THROWS on a bad position, so clang-tidy's
    // bugprone-exception-escape refuses it inside a noexcept function even behind the size guard above.
    const std::size_t offset = leaf.size() - SUFFIX.size();
    for (std::size_t i = 0; i < SUFFIX.size(); ++i) {
        char c = leaf[offset + i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != SUFFIX[i]) {
            return false;
        }
    }
    return true;
}

// The cooker's own asBytes, restated here rather than shared: readFileBytes hands back a std::string
// used as a BYTE container, and every consumer below takes a std::span<const std::byte>.
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- thumbnail_store.cpp:98's idiom
[[nodiscard]] std::span<const std::byte> asBytes(const std::string& text) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

}  // namespace

LoadedTexture loadTextureFromSourceFile(rhi::Device& device, std::string_view absolutePathUtf8, bool srgb) {
    const std::string_view leaf = leafOf(absolutePathUtf8);
    const FileBytesResult source = readFileBytes(absolutePathUtf8, MAX_TEXTURE_FILE_BYTES);
    if (!source.bytes.has_value()) {
        return {{},
                source.refusedByCap ? "This texture is too large to preview (over 64 MiB)."
                                    : "This texture could not be read: " + source.error};
    }

    // The parse-side view BORROWS its buffer (cooked_texture.hpp's lifetime note), so both the source
    // bytes and any cooked bytes must outlive the upload below -- hence both locals live to the end of
    // this function. Moving this code without moving those two locals' scope is exactly the S22 class
    // of defect one subsystem over.
    std::vector<std::byte> cookedBytes;
    std::span<const std::byte> artifact = asBytes(*source.bytes);
    if (!hasKtx2Extension(leaf)) {
        if (isHdrTextureName(leaf)) {
            // NAMED SPECIFICALLY, which is why isHdrTextureName exists: stb does not FAIL on a
            // Radiance file, it silently tone-maps it to 8-bit, so a cook of one is plausible and
            // quietly wrong (3.3.2's own recorded reason).
            return {{}, "Radiance .hdr images are not previewable (v1 cooks 8-bit textures only)."};
        }
        if (!isCookableTextureName(leaf)) {
            return {{}, "This file type cannot be previewed as a texture."};
        }
        // The real chain in miniature: decode -> cook -> parse -> upload. The decode is
        // header-checked before it allocates, and capped per axis by the cook's own bound.
        const DecodedImage image = decodeImageRgba8(artifact, assets::MAX_TEXTURE_DIMENSION);
        if (!image.error.empty()) {
            return {{}, "This image could not be decoded: " + image.error};
        }
        // RGBA8, never the cooker's auto BC rule (D7): a BC format refuses a non-block-aligned top
        // level at upload, so an odd-sized PNG would need an RGBA8 arm anyway. Making that arm the
        // whole path removes the fork -- at the cost of showing no block-compression artifacts, which
        // is a stated known-and-expected rather than a defect.
        //
        // sourceGuid IS NIL HERE, and that is inert rather than a loss: this artifact never reaches a
        // disk, a cache key or a determinism manifest -- it is cooked, parsed and uploaded inside this
        // one function, and createTextureFromCookedTexture reads only the view's format, extent and
        // levels. The caller's guid stayed with the caller, beside the record lookup (§0.25).
        const assets::TextureCookResult cooked = assets::cookTexture(
            {.sourceGuid = Guid{},
             .width = image.width,
             .height = image.height,
             .rgba8 = image.rgba8,
             .format = srgb ? assets::CookedTextureFormat::Rgba8Srgb : assets::CookedTextureFormat::Rgba8Unorm,
             .generateMips = true});
        if (cooked.status != assets::TextureCookStatus::Ok) {
            return {{}, "This image could not be cooked: " + cooked.message};
        }
        cookedBytes = cooked.bytes;
        artifact = cookedBytes;
    }
    // A .ktx2 SOURCE ARRIVES HERE UNTOUCHED: it is already an artifact, and its own format enumerator
    // governs everything the slot might have wanted to say -- colour space included, because a cooked
    // artifact's colour space IS the format and never a flag (3.3.2's INV). The caller's srgb bit still
    // distinguishes two slots referencing it, which costs one redundant upload and keeps the rule
    // uniform.
    const assets::CookedTextureParse parsed = assets::parseCookedTexture(artifact);
    if (parsed.status != assets::CookedTextureStatus::Ok) {
        return {{},
                "This cooked texture could not be read (" +
                    std::string(assets::cookedTextureStatusLabel(parsed.status)) + "): " + parsed.message};
    }
    // THE 3.4.1 BRIDGE, unchanged, and the returned texture is THE CALLER'S TO DESTROY: the material
    // registry borrows it, and destroyMaterial never touches an rhi::TextureHandle.
    const rhi::TextureHandle uploaded = render::createTextureFromCookedTexture(device, parsed.view);
    if (!uploaded.valid()) {
        // 3.4.1's refusal semantics verbatim -- a non-block-aligned .ktx2 top level keeps them, and
        // the bridge has already logged the artifact-level reason.
        return {{}, "The GPU refused this texture (see the Console for the reason)."};
    }
    return {uploaded, {}};
}

}  // namespace engine::editor

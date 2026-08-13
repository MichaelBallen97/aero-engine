// Aero Engine — the image bytes -> texture cook adapter (task 3.3.2). See texture_cook_source.hpp for
// the contract. PURE: no disk, no ImGui, no SDL, no <filesystem>, no logging.
#include <aero/editor/texture_cook_source.hpp>

#include <array>  // EXPLICIT: std::array is not transitive on libstdc++ or MSVC (3.1.1's BLOCKING-1),
                  // and modernize-avoid-c-arrays forbids the alternative. asset_view.cpp:61-62 carries
                  // the same note for the same reason.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ---- stb hygiene, ABOVE the include, all in this one TU ------------------------------------------
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC  // STBIDEF -> static, so THIS implementation exports no symbol at all. The
                          // tree already compiles one other stb_image implementation
                          // (thumbnail_store.cpp) and the assimp port compiles a third, unprefixed
                          // one; internal linkage makes all of them structurally unable to collide,
                          // whichever archive members the linker chooses to pull.
#define STBI_NO_STDIO     // removes stbi_load(const char*) from the TU ENTIRELY, so "all disk access
                          // goes through the editor's own primitives" (INV-2) cannot be broken here by
                          // accident, and <stdio.h> never reaches this line
// STBI_NO_FAILURE_STRINGS IS DELIBERATELY NOT DEFINED HERE, unlike thumbnail_store.cpp: that TU
// reports a STATE (Ready/Failed/Skipped) and needs no text, while this one is the CLI's only source of
// a human-readable decode reason -- an exit-2 message that says "decode failed" and nothing else is a
// message nobody can act on. Each implementation is `static`, so each carries its OWN
// stbi__g_failure_reason and the two cannot interfere. Do not "align" this macro block with
// thumbnail_store.cpp's; the difference is the point.
// NOLINTBEGIN -- vendored, third-party code this project neither owns nor may patch.
#include <stb_image.h>
// NOLINTEND

namespace engine::editor {

namespace {

// ASCII-only, locale-independent. NEVER std::tolower(char): UTF-8 continuation bytes are NEGATIVE as
// char, which is UB and trips bugprone-signed-char-misuse (asset_view.cpp:20-23's precedent, copied
// TU-locally -- there is no shared header for a two-line function).
constexpr unsigned char foldAscii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

// The RAW bytes of the extension, or empty when there is none OR when the last '.' is the final byte
// ("x." has no extension). The pointer+size constructor NEVER throws, unlike substr.
[[nodiscard]] std::string_view rawExtensionOf(std::string_view fileName) noexcept {
    const std::size_t dot = fileName.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 == fileName.size()) {
        return {};
    }
    return std::string_view(fileName.data() + dot + 1, fileName.size() - dot - 1);
}

[[nodiscard]] bool extensionEqualsFolded(std::string_view rawExt, std::string_view lowerLiteral) noexcept {
    if (rawExt.size() != lowerLiteral.size()) {
        return false;
    }
    for (std::size_t i = 0; i < rawExt.size(); ++i) {
        if (foldAscii(static_cast<unsigned char>(rawExt[i])) != static_cast<unsigned char>(lowerLiteral[i])) {
            return false;
        }
    }
    return true;
}

// THE THIRD extension table in this tree, and deliberately not derived from either of the other two.
// .hdr is absent on purpose -- see the header. .ktx2 and .dds are absent because re-cooking a cooked
// artifact is not a workflow and neither is stb-decodable anyway.
constexpr std::array<std::string_view, 7> COOKABLE_TEXTURE_EXTENSIONS{"png", "jpg", "jpeg", "tga", "bmp", "gif", "psd"};

}  // namespace

bool isCookableTextureName(std::string_view fileName) noexcept {
    const std::string_view ext = rawExtensionOf(fileName);
    if (ext.empty()) {
        return false;
    }
    for (const std::string_view claimed : COOKABLE_TEXTURE_EXTENSIONS) {
        if (extensionEqualsFolded(ext, claimed)) {
            return true;
        }
    }
    return false;
}

bool isHdrTextureName(std::string_view fileName) noexcept {
    return extensionEqualsFolded(rawExtensionOf(fileName), "hdr");
}

assets::CookedTextureFormat chooseTextureFormat(std::span<const std::byte> rgba8, bool srgb) noexcept {
    // Every fourth byte, starting at 3. A span whose size is not a multiple of four cannot have its
    // trailing partial texel's alpha read at all, which is why the bound is `alpha < size()` rather
    // than an index computed from a texel count.
    for (std::size_t alpha = 3; alpha < rgba8.size(); alpha += 4) {
        if (std::to_integer<std::uint32_t>(rgba8[alpha]) < 255U) {
            return srgb ? assets::CookedTextureFormat::Bc3Srgb : assets::CookedTextureFormat::Bc3Unorm;
        }
    }
    return srgb ? assets::CookedTextureFormat::Bc1RgbSrgb : assets::CookedTextureFormat::Bc1RgbUnorm;
}

DecodedImage decodeImageRgba8(std::span<const std::byte> fileBytes, std::uint32_t maxDimension) {
    DecodedImage out;
    if (fileBytes.empty()) {
        out.error = "the file is empty";
        return out;
    }
    // The narrowing to stb's own `int` length must be provably safe from THIS function alone, not
    // from a constant in another header -- unreachable behind the 64 MiB read cap, present anyway.
    if (fileBytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        out.error = "the file is larger than this decoder can address";
        return out;
    }
    const int length = static_cast<int>(fileBytes.size());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) -- thumbnail_store.cpp:98's idiom
    const auto* const raw = reinterpret_cast<const unsigned char*>(fileBytes.data());

    // THE HEADER FIRST. stbi_info_from_memory reads only the header, so an over-large image costs a
    // header parse rather than its full decode allocation -- which is the whole reason maxDimension is
    // a parameter of this function rather than a check the caller does afterwards.
    int infoW = 0;
    int infoH = 0;
    int infoComp = 0;
    if (stbi_info_from_memory(raw, length, &infoW, &infoH, &infoComp) == 0) {
        const char* const reason = stbi_failure_reason();
        out.error = reason != nullptr ? std::string(reason) : std::string("the image header could not be read");
        return out;
    }
    if (infoW <= 0 || infoH <= 0) {
        out.error = "the image declares a non-positive extent";
        return out;
    }
    const auto declaredW = static_cast<std::uint32_t>(infoW);
    const auto declaredH = static_cast<std::uint32_t>(infoH);
    if (declaredW > maxDimension || declaredH > maxDimension) {
        // Both numbers, so the message is actionable rather than a bare refusal.
        out.error = "the image is " + std::to_string(declaredW) + "x" + std::to_string(declaredH) + ", above the " +
                    std::to_string(maxDimension) + "-texel per-axis limit";
        return out;
    }

    int decodedW = 0;
    int decodedH = 0;
    int decodedComp = 0;
    stbi_uc* const rawPixels = stbi_load_from_memory(raw, length, &decodedW, &decodedH, &decodedComp, 4);
    if (rawPixels == nullptr) {
        const char* const reason = stbi_failure_reason();
        out.error = reason != nullptr ? std::string(reason) : std::string("the image could not be decoded");
        return out;
    }
    // OWNED FROM HERE. Every path below returns through this guard, which is what makes the early
    // returns safe -- a naked stbi_image_free beside three returns is how one of them gets forgotten.
    const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{rawPixels, &stbi_image_free};
    if (decodedW <= 0 || decodedH <= 0) {
        out.error = "the decoded image has a non-positive extent";  // defensive; unreachable above
        return out;
    }
    out.width = static_cast<std::uint32_t>(decodedW);
    out.height = static_cast<std::uint32_t>(decodedH);
    const std::size_t byteCount = static_cast<std::size_t>(out.width) * out.height * 4U;
    out.rgba8.resize(byteCount);
    for (std::size_t i = 0; i < byteCount; ++i) {
        out.rgba8[i] = static_cast<std::byte>(pixels.get()[i]);
    }
    return out;
}

}  // namespace engine::editor

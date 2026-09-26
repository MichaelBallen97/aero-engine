// editor/src/material_card.cpp -- task E.4.5: the material card's pure model. Every rule is stated in
// material_card.hpp; this file adds none of its own. PURE: no ImGui, no GPU, no <filesystem>, no logging.
#include <aero/editor/material_card.hpp>
#include <aero/render/environment.hpp>
#include <aero/render/lighting.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace engine::editor {

namespace {

// ASCII-only and locale-independent -- asset_view.cpp's foldAscii, TU-locally, because there is no shared
// header for a two-line function (that file's own recorded precedent). NEVER std::tolower(char): a UTF-8
// continuation byte is NEGATIVE as a char, which is undefined behaviour there.
[[nodiscard]] constexpr unsigned char foldAscii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

// Rule 1's set plus the space itself: everything that collapses into one separating space.
[[nodiscard]] constexpr bool isNameSpace(unsigned char c) noexcept { return c < 0x20U || c == 0x7FU || c == ' '; }

// A UTF-8 continuation byte is 10xxxxxx.
[[nodiscard]] constexpr bool isContinuationByte(unsigned char c) noexcept { return (c & 0xC0U) == 0x80U; }

// A display-space channel -> its byte. `encoded` is FINITE here by construction (materialSwatchColor refuses
// a non-finite input first, and tonemapAndEncode maps every finite input to [0, 1]); the clamp is what makes
// the narrowing provably safe from THIS function alone, not a NaN defence -- std::clamp would not be one.
[[nodiscard]] std::uint8_t swatchByte(float encoded) noexcept {
    const float clamped = std::clamp(encoded, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0F));
}

// 0.2126 / 0.7152 / 0.0722 in ten-thousandths -- they sum to exactly 10 000, so grey g weighs 10 000 g and
// the threshold 128 * 10 000 is crossed between 127 and 128 with no rounding anywhere.
constexpr std::uint32_t LUMA_R = 2126U;
constexpr std::uint32_t LUMA_G = 7152U;
constexpr std::uint32_t LUMA_B = 722U;
constexpr std::uint32_t DARK_LABEL_LUMA_THRESHOLD = 128U * 10000U;

}  // namespace

MaterialPreviewLighting materialThumbnailLighting() noexcept {
    // Designated, in DECLARATION order (EnvironmentData: backgroundMode, skyColor, horizonColor, groundColor,
    // solidColor, ambientMode, ambientColor, ambientIntensity; DirectionalLightData: direction, color,
    // intensity, castsShadows, ...). An omitted field keeps its own default, so a field either struct grows
    // later compiles here at its default rather than shifting a positional list.
    return MaterialPreviewLighting{
        .environment = render::EnvironmentData{.backgroundMode = render::BackgroundMode::Sky,
                                               .skyColor = Vec3{0.34F, 0.38F, 0.46F},
                                               .horizonColor = Vec3{0.62F, 0.62F, 0.64F},
                                               .groundColor = Vec3{0.20F, 0.19F, 0.18F},
                                               .ambientMode = render::AmbientMode::Hemisphere,
                                               .ambientIntensity = 0.8F},
        // The travel direction of a key light over the viewer's left shoulder at the orbit angle 0.6: the
        // eye sits at (2.476, 1.2, 1.694), and 0.7 * toEye + 0.8 * up + 0.5 * left normalises to
        // (0.189, 0.791, 0.582) -- the light travels the opposite way. materialPreviewView turns shadows
        // off on the view; castsShadows is false here too, because a thumbnail has no caster.
        .sun = render::DirectionalLightData{.direction = normalize(Vec3{-0.19F, -0.79F, -0.58F}),
                                            .color = Vec3{1.0F, 0.97F, 0.92F},
                                            .intensity = 2.2F,
                                            .castsShadows = false},
        .hasSun = true};
}

render::TonemapParams materialThumbnailTonemap() noexcept {
    return render::sanitizeTonemapParams(render::TonemapParams{});
}

MaterialDisplayName sanitizeMaterialDisplayName(std::string_view documentName) {
    MaterialDisplayName result;
    std::string& text = result.text;
    text.reserve(std::min(documentName.size(), MAX_MATERIAL_DISPLAY_NAME_BYTES + 1U));
    bool spacePending = false;
    for (const char raw : documentName) {
        if (isNameSpace(static_cast<unsigned char>(raw))) {
            spacePending = !text.empty();  // rule 1, and rule 2's LEADING trim: nothing is written yet
            continue;
        }
        if (spacePending) {
            text.push_back(' ');   // rule 2: a run is ONE space, written only when a non-space follows it --
            spacePending = false;  // which is also rule 2's TRAILING trim, for free
        }
        text.push_back(raw);
        if (text.size() > MAX_MATERIAL_DISPLAY_NAME_BYTES) {
            result.truncated = true;  // a non-space byte lies past the cap; nothing further can change that
            break;                    // -- and a 40 MB name costs 97 bytes of work, not 40 MB
        }
    }
    if (!result.truncated) {
        return result;  // rule 3 falls out: an all-space name never wrote a byte
    }
    std::size_t cut = MAX_MATERIAL_DISPLAY_NAME_BYTES;
    while (cut > 0 && isContinuationByte(static_cast<unsigned char>(text[cut]))) {
        --cut;  // never slice a multi-byte sequence -- elideForCaption's own step-back
    }
    while (cut > 0 && text[cut - 1U] == ' ') {
        --cut;  // "Studio …" reads as a gap before the ellipsis; "Studio…" does not
    }
    text.resize(cut);
    text += MATERIAL_DISPLAY_NAME_ELLIPSIS;
    return result;
}

bool materialNameMatchesStem(std::string_view name, std::string_view fileName) noexcept {
    const std::size_t dot = fileName.find_last_of('.');
    const std::size_t stemLength =
        (dot == std::string_view::npos || dot + 1U == fileName.size()) ? fileName.size() : dot;
    if (name.size() != stemLength) {
        return false;
    }
    for (std::size_t i = 0; i < stemLength; ++i) {
        if (foldAscii(static_cast<unsigned char>(name[i])) != foldAscii(static_cast<unsigned char>(fileName[i]))) {
            return false;
        }
    }
    return true;
}

std::optional<IconColor> materialSwatchColor(const MaterialDocument& document) noexcept {
    const Vec4& base = document.baseColorFactor;
    if (!std::isfinite(base.x) || !std::isfinite(base.y) || !std::isfinite(base.z)) {
        return std::nullopt;  // a REFUSAL: the host keeps the kind colour (E.2.2's "test isfinite FIRST")
    }
    const Vec3 encoded = render::tonemapAndEncode(Vec3{base.x, base.y, base.z}, materialThumbnailTonemap());
    return IconColor{.r = swatchByte(encoded.x), .g = swatchByte(encoded.y), .b = swatchByte(encoded.z), .a = 255U};
}

bool materialSwatchWantsDarkLabel(IconColor swatch) noexcept {
    const std::uint32_t weighted = (LUMA_R * swatch.r) + (LUMA_G * swatch.g) + (LUMA_B * swatch.b);
    return weighted >= DARK_LABEL_LUMA_THRESHOLD;
}

MaterialCard materialCardFor(const MaterialDocument& document) {
    MaterialDisplayName name = sanitizeMaterialDisplayName(document.name);
    const std::optional<IconColor> swatch = materialSwatchColor(document);
    return MaterialCard{.displayName = std::move(name.text),
                        .displayNameTruncated = name.truncated,
                        .swatch = swatch.value_or(IconColor{}),
                        .hasSwatch = swatch.has_value()};
}

std::string_view materialCardSubtitle(const MaterialCard* card, std::string_view fileName) noexcept {
    if (card == nullptr || card->displayName.empty()) {
        return {};
    }
    if (!card->displayNameTruncated && materialNameMatchesStem(card->displayName, fileName)) {
        return {};  // rule 4: "brass.aeromat  -  brass" is noise
    }
    return card->displayName;
}

std::optional<IconColor> materialCardTint(const MaterialCard* card) noexcept {
    if (card == nullptr || !card->hasSwatch) {
        return std::nullopt;
    }
    return card->swatch;
}

std::string materialCardRowText(std::string_view fileName, std::string_view subtitle) {
    std::string text(fileName);
    if (subtitle.empty()) {
        return text;  // byte-identical to the row every host drew before this task
    }
    text.reserve(fileName.size() + MATERIAL_CARD_SEPARATOR.size() + subtitle.size());
    text += MATERIAL_CARD_SEPARATOR;
    text += subtitle;
    return text;
}

}  // namespace engine::editor

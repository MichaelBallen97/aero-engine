// Aero Engine — the pure document->render bridge for materials (task 3.4.2, D8). PURE: no ImGui, no
// <filesystem>, no <fstream>, no GPU call, no logging. Every function here is provable from a
// MaterialDocument literal with no context of any kind, which is what puts the whole of AC-25 on the
// tier-0 shell test rather than behind a device.
//
// EVERY ENUM SWITCH BELOW CARRIES NO `default:` (INV-8). A fifth wrap/filter/mip/alpha token is then a
// -Wswitch failure on the Linux lane rather than a silent fallback to the first enumerator -- and
// docs/09 section 11.6 makes adding a token a version bump, so the compile error is the point.
//
// The three SLOT-INDEX switches are the exception, and they must be: they switch on a std::size_t, so
// -Wswitch has nothing to enforce and clang-tidy's bugprone-switch-missing-default-case
// (--warnings-as-errors on the Linux lane) rejects a non-enum switch without one outright. They take
// render::materialSlotAt()'s own shape instead -- `default:` answering baseColor -- which is the
// authority for "which slot is index k" and the thing ME14 walks against.
#include <aero/editor/material_edit.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine::editor {

namespace {

// The one spelling of the extension this file appends. asset_view.cpp's MATERIAL_EXTENSIONS holds the
// same four letters for CLASSIFICATION; the two tables are separate by the same recorded design that
// keeps the browser's kind table and the thumbnail table apart.
constexpr std::string_view MATERIAL_FILE_SUFFIX = ".aeromat";

// ASCII-only, locale-independent. NEVER std::tolower(char): UTF-8 continuation bytes are NEGATIVE as
// char, which is UB and trips bugprone-signed-char-misuse (asset_view.cpp's own note, copied
// TU-locally -- there is no shared header for a two-line function).
constexpr unsigned char foldAscii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] bool namesEqualFolded(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (foldAscii(static_cast<unsigned char>(a[i])) != foldAscii(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

render::MaterialAlpha materialAlphaFor(MaterialAlphaMode mode) noexcept {
    switch (mode) {
        case MaterialAlphaMode::Opaque:
            return render::MaterialAlpha::Opaque;
        case MaterialAlphaMode::Mask:
            return render::MaterialAlpha::Mask;
        case MaterialAlphaMode::Blend:
            return render::MaterialAlpha::Blend;
    }
    return render::MaterialAlpha::Opaque;  // unreachable; enumerated so a new mode is a -Wswitch warning
}

render::MaterialParams materialParamsFor(const MaterialDocument& doc) noexcept {
    return render::MaterialParams{
        .baseColorFactor = doc.baseColorFactor,
        .emissiveFactor = doc.emissiveFactor,
        .metallicFactor = doc.metallicFactor,
        .roughnessFactor = doc.roughnessFactor,
        .normalScale = doc.normalScale,
        .occlusionStrength = doc.occlusionStrength,
        .alpha = materialAlphaFor(doc.alphaMode),
        .alphaCutoff = doc.alphaCutoff,
        .doubleSided = doc.doubleSided,
    };
}

const std::optional<MaterialTextureSlot>& documentSlotAt(const MaterialDocument& doc, std::size_t index) noexcept {
    switch (index) {
        case 1:
            return doc.metallicRoughness;
        case 2:
            return doc.normal;
        case 3:
            return doc.occlusion;
        case 4:
            return doc.emissive;
        default:
            break;
    }
    return doc.baseColor;  // index 0 and out of range -- render::materialSlotAt()'s own posture
}

std::optional<MaterialTextureSlot>& documentSlotAt(MaterialDocument& doc, std::size_t index) noexcept {
    switch (index) {
        case 1:
            return doc.metallicRoughness;
        case 2:
            return doc.normal;
        case 3:
            return doc.occlusion;
        case 4:
            return doc.emissive;
        default:
            break;
    }
    return doc.baseColor;  // index 0 and out of range -- render::materialSlotAt()'s own posture
}

std::string_view materialSlotLabel(std::size_t index) noexcept {
    switch (index) {
        case 1:
            return "metallicRoughness";
        case 2:
            return "normal";
        case 3:
            return "occlusion";
        case 4:
            return "emissive";
        default:
            break;
    }
    return "baseColor";  // index 0 and out of range -- the slot walk's own posture, one line over
}

rhi::AddressMode materialAddressModeFor(MaterialWrap wrap) noexcept {
    switch (wrap) {
        case MaterialWrap::Repeat:
            return rhi::AddressMode::Repeat;
        case MaterialWrap::Clamp:
            return rhi::AddressMode::ClampToEdge;
        case MaterialWrap::Mirror:
            return rhi::AddressMode::MirroredRepeat;
    }
    return rhi::AddressMode::Repeat;  // unreachable; enumerated so a new wrap is a -Wswitch warning
}

rhi::Filter materialFilterFor(MaterialFilter filter) noexcept {
    switch (filter) {
        case MaterialFilter::Nearest:
            return rhi::Filter::Nearest;
        case MaterialFilter::Linear:
            return rhi::Filter::Linear;
    }
    return rhi::Filter::Linear;  // unreachable; enumerated so a new filter is a -Wswitch warning
}

rhi::SamplerDesc materialSamplerDescFor(const MaterialTextureSlot& slot) noexcept {
    rhi::SamplerDesc desc{};
    desc.minFilter = materialFilterFor(slot.minFilter);
    desc.magFilter = materialFilterFor(slot.magFilter);
    desc.addressU = materialAddressModeFor(slot.wrapU);
    desc.addressV = materialAddressModeFor(slot.wrapV);
    // addressW is DELIBERATELY untouched (section 11.4's own clause): v1 materials reference 2D
    // textures, so the third axis has no token to carry and keeps the desc default.
    switch (slot.mipFilter) {
        case MaterialMipFilter::None:
            // The clamp-to-base idiom, BOTH halves: rhi::MipmapMode has no None, so "sample the top
            // level only" is Nearest plus a maxLod of zero. Dropping either half samples mips.
            desc.mipmapMode = rhi::MipmapMode::Nearest;
            desc.maxLod = 0.0F;
            break;
        case MaterialMipFilter::Nearest:
            desc.mipmapMode = rhi::MipmapMode::Nearest;
            break;
        case MaterialMipFilter::Linear:
            desc.mipmapMode = rhi::MipmapMode::Linear;
            break;
    }
    return desc;
}

bool materialSlotIsSrgb(std::size_t index) noexcept {
    return render::defaultTextureKindForSlot(index) == render::MaterialDefaultTextureKind::WhiteSrgb;
}

std::string uniqueMaterialFileName(std::string_view stem, std::span<const std::string_view> taken,
                                   std::size_t maxAttempts) {
    for (std::size_t attempt = 1; attempt <= maxAttempts; ++attempt) {
        std::string candidate(stem);
        if (attempt > 1) {
            candidate += '-';
            candidate += std::to_string(attempt);
        }
        candidate += MATERIAL_FILE_SUFFIX;
        bool collides = false;
        for (const std::string_view existing : taken) {
            if (namesEqualFolded(candidate, existing)) {
                collides = true;
                break;
            }
        }
        if (!collides) {
            return candidate;
        }
    }
    return {};  // exhausted -- the caller logs ONE WARN and writes nothing (AC-6)
}

}  // namespace engine::editor

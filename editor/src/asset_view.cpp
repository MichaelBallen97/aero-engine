// Aero Engine — the asset browser's presentation model (task 3.1.3). PURE: no ImGui, no
// <filesystem>, no GPU, no logging (INV-V8). Every rule here is provable from a std::vector or
// std::span literal with no context of any kind.
#include <aero/editor/asset_view.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

namespace {

// ASCII-only, locale-independent. NEVER std::tolower(char): UTF-8 continuation bytes are NEGATIVE as
// char, which is UB and trips bugprone-signed-char-misuse (project_files.cpp:44-46's precedent,
// copied TU-locally -- there is no shared header for a two-line function).
constexpr unsigned char foldAscii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

// The RAW bytes of the extension (no case folding), or empty when there is none, OR when the last
// '.' is the final byte ("x." has no extension -- a trailing dot is Unknown/FILE).
[[nodiscard]] std::string_view rawExtensionOf(std::string_view fileName) noexcept {
    const std::size_t dot = fileName.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 == fileName.size()) {
        return {};
    }
    // The pointer+size constructor NEVER throws, unlike substr (bugprone-exception-escape in a
    // noexcept function) -- the same idiom the header's own implementation notes require.
    return std::string_view(fileName.data() + dot + 1, fileName.size() - dot - 1);
}

// ASCII-case-insensitive compare between a raw (possibly mixed-case) extension and an all-lowercase
// literal. `noexcept` and allocation-free -- bugprone-exception-escape forbids a `substr` here.
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

[[nodiscard]] bool extensionIn(std::string_view rawExt, std::span<const std::string_view> table) noexcept {
    for (const std::string_view lit : table) {
        if (extensionEqualsFolded(rawExt, lit)) {
            return true;
        }
    }
    return false;
}

// modernize-avoid-c-arrays is live; <array> is included EXPLICITLY -- it is not transitive on
// libstdc++ or MSVC (3.1.1's BLOCKING-1).
constexpr std::array<std::string_view, 10> TEXTURE_EXTENSIONS{"png", "jpg", "jpeg", "tga",  "bmp",
                                                              "gif", "hdr", "psd",  "ktx2", "dds"};
constexpr std::array<std::string_view, 8> MODEL_EXTENSIONS{"gltf", "glb", "fbx", "obj", "blend", "dae", "ply", "stl"};
constexpr std::array<std::string_view, 4> AUDIO_EXTENSIONS{"wav", "mp3", "ogg", "flac"};
constexpr std::array<std::string_view, 7> TEXT_EXTENSIONS{"json", "txt", "md", "hlsl", "glsl", "ts", "js"};
// task 3.4.2 (D9): the FIFTH kind table. Deliberately not folded into TEXT_EXTENSIONS even though an
// .aeromat IS JSON: the browser's job here is to say what a file MEANS, and a material is an editable
// asset with its own panel, not a text file.
constexpr std::array<std::string_view, 1> MATERIAL_EXTENSIONS{"aeromat"};

// The stb-decodable SUBSET of TEXTURE_EXTENSIONS -- deliberately a SEPARATE table (D7/seed S32), not
// derived from it: .ktx2 and .dds are textures that get an icon, never a decode attempt.
constexpr std::array<std::string_view, 8> THUMBNAIL_DECODABLE_EXTENSIONS{"png", "jpg", "jpeg", "tga",
                                                                         "bmp", "gif", "hdr",  "psd"};

}  // namespace

AssetKind classifyAssetKind(std::string_view fileName, bool isDirectory) noexcept {
    if (isDirectory) {
        return AssetKind::Folder;
    }
    const std::string_view ext = rawExtensionOf(fileName);
    if (ext.empty()) {
        return AssetKind::Unknown;
    }
    if (extensionIn(ext, TEXTURE_EXTENSIONS)) {
        return AssetKind::Texture;
    }
    if (extensionIn(ext, MODEL_EXTENSIONS)) {
        return AssetKind::Model;
    }
    if (extensionIn(ext, AUDIO_EXTENSIONS)) {
        return AssetKind::Audio;
    }
    if (extensionIn(ext, TEXT_EXTENSIONS)) {
        return AssetKind::Text;
    }
    if (extensionIn(ext, MATERIAL_EXTENSIONS)) {
        return AssetKind::Material;
    }
    return AssetKind::Unknown;
}

bool isThumbnailDecodable(std::string_view fileName) noexcept {
    const std::string_view ext = rawExtensionOf(fileName);
    if (ext.empty()) {
        return false;
    }
    return extensionIn(ext, THUMBNAIL_DECODABLE_EXTENSIONS);
}

std::string_view assetKindLabel(AssetKind kind) noexcept {
    switch (kind) {
        case AssetKind::Folder:
            return "Folder";
        case AssetKind::Texture:
            return "Texture";
        case AssetKind::Model:
            return "Model";
        case AssetKind::Audio:
            return "Audio";
        case AssetKind::Text:
            return "Text";
        case AssetKind::Material:
            return "Material";
        case AssetKind::Unknown:
            return "Unknown";
    }
    return "Unknown";  // unreachable; enumerated so a new AssetKind is a -Wswitch warning, not silent
}

namespace {
constexpr std::size_t ICON_LABEL_MAX_LENGTH = 4;

[[nodiscard]] char upperAscii(unsigned char c) noexcept {
    return static_cast<char>((c >= 'a' && c <= 'z') ? static_cast<unsigned char>(c - ('a' - 'A')) : c);
}
}  // namespace

std::string iconLabelFor(std::string_view fileName) {
    const std::string_view ext = rawExtensionOf(fileName);
    if (ext.empty()) {
        return "FILE";  // no extension, or a trailing dot ("x.")
    }
    for (const unsigned char c : ext) {
        if (c > 0x7FU) {
            return "FILE";  // a non-ASCII byte ANYWHERE in the extension -- never mangle bytes (AV23)
        }
    }
    std::string label;
    if (ext.size() <= ICON_LABEL_MAX_LENGTH) {
        label.reserve(ext.size());
        for (const unsigned char c : ext) {
            label.push_back(upperAscii(c));
        }
        return label;
    }
    // Longer than 4: keep the first 3 characters plus the LAST one, so a numbered extension
    // ("model.blend1", "model.blend2", …) keeps its distinguishing digit instead of losing it to a
    // plain head-truncation -- "model.blend1" -> "BLE1" (AV21, seed S18). ASCII-only, so slicing by
    // byte count never risks cutting a multi-byte UTF-8 sequence (already refused above).
    label.reserve(ICON_LABEL_MAX_LENGTH);
    label.push_back(upperAscii(static_cast<unsigned char>(ext[0])));
    label.push_back(upperAscii(static_cast<unsigned char>(ext[1])));
    label.push_back(upperAscii(static_cast<unsigned char>(ext[2])));
    label.push_back(upperAscii(static_cast<unsigned char>(ext.back())));
    return label;
}

IconColor iconColorFor(AssetKind kind) noexcept {
    switch (kind) {
        case AssetKind::Folder:
            return IconColor{.r = 0xE0U, .g = 0xB0U, .b = 0x30U, .a = 255U};  // a warm gold
        case AssetKind::Texture:
            return IconColor{.r = 0x40U, .g = 0x90U, .b = 0xD0U, .a = 255U};  // a cool blue
        case AssetKind::Model:
            return IconColor{.r = 0x60U, .g = 0xB0U, .b = 0x60U, .a = 255U};  // a green
        case AssetKind::Audio:
            return IconColor{.r = 0xA0U, .g = 0x60U, .b = 0xC0U, .a = 255U};  // a violet
        case AssetKind::Text:
            return IconColor{.r = 0x90U, .g = 0x90U, .b = 0x90U, .a = 255U};  // a neutral grey
        case AssetKind::Material:
            return IconColor{.r = 0xE0U, .g = 0x70U, .b = 0x45U, .a = 255U};  // a warm coral
        case AssetKind::Unknown:
            return IconColor{.r = 0x50U, .g = 0x50U, .b = 0x50U, .a = 255U};  // a darker grey
    }
    return IconColor{};  // unreachable; enumerated so a new AssetKind is a -Wswitch warning
}

float tileEdgeFontMultiple(TileSize size) noexcept {
    switch (size) {
        case TileSize::Small:
            return 4.0F;
        case TileSize::Medium:
            return 6.0F;
        case TileSize::Large:
            return 8.0F;
    }
    return 6.0F;  // unreachable; enumerated so a new TileSize is a -Wswitch warning
}

int gridColumnsFor(float availWidth, float tileWidth, float spacing) noexcept {
    const float denom = tileWidth + spacing;
    if (!(availWidth > 0.0F) || !(denom > 0.0F)) {
        return 1;  // NaN-safe (every `>` on NaN is false) and division-safe -- NEVER 0
    }
    const float columnsF = (availWidth + spacing) / denom;
    if (!(columnsF >= 1.0F)) {
        return 1;
    }
    // An infinite availWidth (or a subnormal denom) produces an infinite quotient, and casting +inf
    // to int is UNDEFINED BEHAVIOUR -- an UBSan abort, not a warning (AV31). Clamp to the largest
    // value a caller's `(n + columns - 1) / columns` can use safely; still >= 1, which is all AV31
    // asserts.
    constexpr float MAX_REPRESENTABLE_COLUMNS = 1'000'000.0F;
    if (!(columnsF < MAX_REPRESENTABLE_COLUMNS)) {
        return static_cast<int>(MAX_REPRESENTABLE_COLUMNS);
    }
    return static_cast<int>(columnsF);  // floor: a positive float truncates toward zero, i.e. floors
}

bool matchesFilter(std::string_view leafName, bool isDirectory, const AssetFilter& filter) noexcept {
    if (!filter.anyKind) {
        const AssetKind kind = classifyAssetKind(leafName, isDirectory);
        if (kind != filter.kind) {
            return false;
        }
    }
    if (filter.query.empty()) {
        return true;
    }
    if (filter.query.size() > leafName.size()) {
        return false;
    }
    // ASCII-case-insensitive substring search, allocation-free (noexcept forbids substr/string).
    const std::size_t queryLen = filter.query.size();
    const std::size_t last = leafName.size() - queryLen;
    for (std::size_t start = 0; start <= last; ++start) {
        bool matched = true;
        for (std::size_t i = 0; i < queryLen; ++i) {
            const unsigned char a = foldAscii(static_cast<unsigned char>(leafName[start + i]));
            const unsigned char b = foldAscii(static_cast<unsigned char>(filter.query[i]));
            if (a != b) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return true;
        }
    }
    return false;
}

std::vector<std::size_t> filterEntriesByKind(std::span<const FileEntry> entries, const AssetFilter& filter) {
    std::vector<std::size_t> indices;
    indices.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (matchesFilter(entries[i].name, entries[i].isDirectory, filter)) {
            indices.push_back(i);
        }
    }
    return indices;
}

SearchResult searchAssets(std::span<const AssetRecord> records, const AssetFilter& filter, std::size_t cap) {
    SearchResult result;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const AssetRecord& record = records[i];
        const std::size_t lastSlash = record.relativePath.find_last_of('/');
        const std::string_view leaf = lastSlash == std::string::npos
                                          ? std::string_view(record.relativePath)
                                          : std::string_view(record.relativePath).substr(lastSlash + 1);
        // A record never carries its own isDirectory flag (AssetDatabase records only FILES) -- every
        // record here is a file, so `isDirectory` is always false at this call site.
        if (!matchesFilter(leaf, /*isDirectory=*/false, filter)) {
            continue;
        }
        ++result.total;  // UNCAPPED (seed S15) -- incremented for every match, capped or not
        if (result.hits.size() < cap) {
            result.hits.push_back(SearchHit{.relativePath = record.relativePath, .recordIndex = i});
        }
    }
    result.truncated = result.total > result.hits.size();
    return result;
}

}  // namespace engine::editor

// Aero Engine — the audio source-name predicates (task 3.7.1). See audio_cook_source.hpp for the
// contract. PURE: no disk, no ImGui, no SDL, no <filesystem>, no logging, and no third party at all —
// this TU sees neither miniaudio nor stb_vorbis, which is what lets it stay in the light include set
// the header promises.
#include <aero/editor/audio_cook_source.hpp>

#include <array>  // EXPLICIT: std::array is not transitive on libstdc++ or MSVC (3.1.1's BLOCKING-1),
                  // and modernize-avoid-c-arrays forbids the alternative. texture_cook_source.cpp:5-7
                  // carries the same note for the same reason.
#include <cstddef>
#include <string_view>
#include <utility>  // std::pair, for the extension table below

namespace engine::editor {

namespace {

// ASCII-only, locale-independent. NEVER std::tolower(char): UTF-8 continuation bytes are NEGATIVE as
// char, which is UB and trips bugprone-signed-char-misuse (asset_view.cpp:20-23's precedent, copied
// TU-locally for the third time -- there is no shared header for a two-line function, and
// texture_cook_source.cpp already made the same call).
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

}  // namespace

std::string_view audioSourceFormatLabel(AudioSourceFormat format) noexcept {
    switch (format) {
        case AudioSourceFormat::Unknown:
            return "Unknown";
        case AudioSourceFormat::Wav:
            return "WAV";
        case AudioSourceFormat::Flac:
            return "FLAC";
        case AudioSourceFormat::Mp3:
            return "MP3";
        case AudioSourceFormat::Ogg:
            return "Ogg Vorbis";
    }
    return "Unknown";  // reachable: the enum has a fixed underlying type, so every u8 is a value
}

AudioSourceFormat audioSourceFormatForName(std::string_view fileName) noexcept {
    const std::string_view ext = rawExtensionOf(fileName);
    if (ext.empty()) {
        return AudioSourceFormat::Unknown;
    }
    // THE table. Four rows, each pairing the extension with the backend that claims it, so a fifth
    // format lands in exactly one place. Deliberately NOT derived from asset_view.cpp's
    // AUDIO_EXTENSIONS -- see the header.
    constexpr std::array<std::pair<std::string_view, AudioSourceFormat>, 4> CLAIMED{
        std::pair{std::string_view("wav"), AudioSourceFormat::Wav},
        std::pair{std::string_view("flac"), AudioSourceFormat::Flac},
        std::pair{std::string_view("mp3"), AudioSourceFormat::Mp3},
        std::pair{std::string_view("ogg"), AudioSourceFormat::Ogg}};
    for (const auto& [claimed, format] : CLAIMED) {
        if (extensionEqualsFolded(ext, claimed)) {
            return format;
        }
    }
    return AudioSourceFormat::Unknown;
}

bool isCookableAudioName(std::string_view fileName) noexcept {
    return audioSourceFormatForName(fileName) != AudioSourceFormat::Unknown;
}

}  // namespace engine::editor

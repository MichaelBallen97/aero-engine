#pragma once
// Aero Engine — the audio source-name predicates (task 3.7.1). The name half of the editor's audio
// contribution: which files claim to be cookable sound, and which backend a claimed one goes to.
//
// THIS HEADER INCLUDES <cstdint> AND <string_view> AND NOTHING ELSE, FOREVER — the
// import_settings.hpp rule (editor/include/aero/editor/import_settings.hpp:12), a second application.
// AudioSourceFormat is shared by two headers with very different dependency footprints: this one,
// which every TU asking "is this file cookable?" includes, and audio_decode.hpp, which pulls <span>,
// <vector> and <string>. Declaring the enum in the heavier header would force the predicate's callers
// to take the decode's whole include set. The dependency runs PREDICATE -> DECODE, never the reverse.
// Do not add an include here.
#include <cstdint>
#include <string_view>

namespace engine::editor {

enum class AudioSourceFormat : std::uint8_t { Unknown = 0, Wav, Flac, Mp3, Ogg };

// A switch with NO `default:`, so a fifth format is a -Wswitch failure on the Linux lane. NOT named
// toString: an engine toString(SomeEnum) is found by ADL inside doctest's stringifier and breaks every
// lane (.claude/rules/ci-portability.md, hit for real at 3.6.3).
[[nodiscard]] std::string_view audioSourceFormatLabel(AudioSourceFormat format) noexcept;

// 256 MiB — the SAME number as model_import.hpp's MAX_MODEL_FILE_BYTES, and stated as such rather
// than reached independently: a 16-bit wav is a byte-for-byte 1:1 source for a 115 MB PCM region, so
// the tighter 64 MiB ceiling texture_cook_source.hpp uses would refuse a legal input here. The
// DECODED size is bounded separately by decodeAudioFile's own three caps — one per axis plus the
// PRODUCT, which is the only one of the three that bounds bytes — and that is what makes a generous
// file ceiling safe. Normative — docs/09 section 14.
inline constexpr std::uint64_t MAX_AUDIO_FILE_BYTES = 256ULL * 1024ULL * 1024ULL;

// THE SIXTH extension table in this tree, and deliberately NOT derived from asset_view.cpp's
// AUDIO_EXTENSIONS. isCookableTextureName's own comment sets the precedent verbatim: deriving one
// table from the other is exactly what would let a future edit to one silently move the other. They
// happen to agree today; nothing enforces that and nothing should.
[[nodiscard]] bool isCookableAudioName(std::string_view fileName) noexcept;

// Unknown for everything the four claimed extensions do not cover, including a name with no extension
// at all. isCookableAudioName(n) is exactly (audioSourceFormatForName(n) != Unknown) -- THIS one
// derivation is correct and wanted, because both answer the same question about the same table in the
// same file.
[[nodiscard]] AudioSourceFormat audioSourceFormatForName(std::string_view fileName) noexcept;

}  // namespace engine::editor

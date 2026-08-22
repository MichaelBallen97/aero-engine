#pragma once
// Aero Engine — the compressed-audio bytes -> interleaved s16 adapter (task 3.7.1). The editor's
// decode half: four source formats, two backends, one output shape.
//
// It lives in /editor rather than inside tools/cooker so it is exercised by aero_editor_shell_test
// against real inputs, and so the editor's own future cook path and the CLI share ONE decode rather
// than two. It CANNOT live in engine/assets: miniaudio and stb are vcpkg packages and aero_assets
// links none, which is what makes that target's PRIVATE links a real compile-time boundary (R12).
// The texture column made exactly this call at 3.3.2 and for exactly this reason.
//
// PURE: no disk (both backends are memory-only), no ImGui, no SDL, no <filesystem>, no logging —
// status is RETURNED, never printed.
#include <aero/editor/audio_cook_source.hpp>  // AudioSourceFormat -- see that header's own note

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::editor {

struct DecodedAudio {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    // Interleaved FRAME-MAJOR, EXACTLY channels * frameCount entries; EMPTY on failure. There is no
    // frameCount field, deliberately: it is samples.size() / channels and two numbers that must agree
    // are one number.
    std::vector<std::int16_t> samples;
    std::string error;  // "" IFF the decode succeeded
    // WHICH BACKEND ACTUALLY RAN, so a caller can tell "the ogg path decoded this" from "the miniaudio
    // path did". Selected by FILE NAME, never by sniffing the bytes.
    AudioSourceFormat format = AudioSourceFormat::Unknown;
};

// NEVER THROWS. Every decoder handle is released on every path, including the early returns — both
// backends hold theirs in a scope-owning guard rather than beside a naked call, which is what makes
// those early returns safe.
//
// `maxFrames` and `maxChannels` are checked TWICE: once against the source's own length query, before
// anything is reserved, and again inside the read loop. THE SECOND HALF IS THE ONE THAT MATTERS — both
// length queries are answers derived from the file's own claims (an mp3 with no Xing header reports 0,
// and a truncated or crafted ogg can report a granule position the pages that follow do not support),
// so the loop stops at the cap regardless of what the header promised. A cap checked only against a
// self-reported length is not a cap.
//
// Callers pass assets::MAX_COOKED_AUDIO_FRAMES and assets::MAX_COOKED_AUDIO_CHANNELS — the bounds the
// cook itself will apply.
[[nodiscard]] DecodedAudio decodeAudioFile(std::string_view fileName, std::span<const std::byte> fileBytes,
                                           std::uint32_t maxFrames, std::uint32_t maxChannels);

}  // namespace engine::editor

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
// THREE BOUNDS, NOT TWO, AND THE THIRD IS THE PRODUCT. `maxFrames` and `maxChannels` bound one axis
// each; `maxSamples` bounds `frames * channels`, which is the only one of the three that bounds the
// BYTES this function allocates. Without it a caller passing the cook's own per-axis caps
// (28 800 000 frames, 8 channels) would accept and buffer 230 400 000 s16 samples — ~440 MiB, four
// times assets::MAX_COOKED_AUDIO_BYTES — and cookAudio would then refuse every one of them on its
// sample cap, so the whole allocation was guaranteed waste. Reachable without crafting anything: a
// legitimate 4-channel 48 kHz ten-minute FLAC is ~115 MB on disk, under the 256 MiB read cap, and
// decodes to 230 MB.
//
// All three are checked TWICE: once against the source's own length query, before anything is
// reserved, and again inside the read loop. THE SECOND HALF IS THE ONE THAT MATTERS — a length query
// is an answer derived from the file's own claims, and some of those claims are taken unclamped (a
// Wave64 `fact` chunk's 8-byte sample count is used verbatim, miniaudio.h:81684-81685, while the
// file-size clamp at :81646-81652 applies to the data chunk instead), so the loop stops at the cap
// regardless of what the header promised. A cap checked only against a self-reported length is not a
// cap. The pre-allocation half is what keeps an HONEST over-long file from costing an allocation.
//
// Callers pass assets::MAX_COOKED_AUDIO_FRAMES, assets::MAX_COOKED_AUDIO_CHANNELS and
// assets::MAX_COOKED_AUDIO_SAMPLES — the three bounds the cook itself will apply to the same numbers.
// The cook additionally bounds the SAMPLE RATE, which is deliberately not pushed down here: a rate
// bounds nothing this function allocates, so refusing on it early would buy nothing and would put a
// second copy of that rule one layer away from the one that owns it.
[[nodiscard]] DecodedAudio decodeAudioFile(std::string_view fileName, std::span<const std::byte> fileBytes,
                                           std::uint32_t maxFrames, std::uint32_t maxChannels,
                                           std::uint64_t maxSamples);

}  // namespace engine::editor

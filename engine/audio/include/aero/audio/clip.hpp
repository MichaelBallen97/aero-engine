#pragma once
// Aero Engine — the runtime audio clip and its VFS loader (task 3.7.1). This is the file that OPENS
// engine/audio: until now this engine could open a device and write silence into it (task 0.3.3) and
// had no way at all to say what a sound IS.
//
// THE LAYER'S DEPENDENCIES ARE THE POINT. aero_audio links aero::core and aero::assets and NO vcpkg
// package of any kind -- no miniaudio, no stb, nothing. It is the SECOND target in this tree (after
// aero_assets) whose PRIVATE links are a real compile-time boundary rather than convention plus a
// grep, and engine/audio/CMakeLists.txt says so at length because task 3.7.2 is the task that can
// most easily void it. There is no decoder here and there must never be one: a runtime that can
// decode an mp3 is a runtime that can be handed one, and ADR-008 says it is handed a .pak of cooked
// artifacts. Every decode in this tree lives in /editor.
//
// NOTHING IN THIS FILE LOGS. loadAudioClip returns a status and a message and lets the caller decide
// whether that is worth an AERO_LOG_*, because the same load failure is a hard error to a runtime and
// a shrug to a tool, and this layer is not the one that knows which.
#include <aero/assets/cooked_audio.hpp>
#include <aero/core/guid.hpp>
#include <aero/core/vfs.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::audio {

// Why five statuses and not one nullopt: engine/core/vfs.hpp:87-89 records that a VFS read returning
// std::nullopt means "absent OR unreadable" and does NOT distinguish them, and that a caller which
// must tell them apart checks exists() first. loadAudioClip is that caller, so NotFound, Unreadable
// and TooLarge are three genuinely different answers here rather than three names for one nullopt.
enum class AudioClipLoadStatus : std::uint8_t {
    Ok = 0,
    NotFound,     // the VFS has no such path -- exists() said so
    Unreadable,   // exists() said yes and readFile() said no
    TooLarge,     // fileSize() exceeds assets::MAX_COOKED_AUDIO_BYTES -- REFUSED WITHOUT READING
    ParseFailed,  // parseCookedAudio refused; `message` carries its label and its message
};
// A switch with NO `default:` (the cookedAudioStatusLabel precedent) -- a future enumerator is a
// -Wswitch failure on the Linux lane rather than a silent fallthrough. NOT named toString: an engine
// toString(SomeEnum) is found by ADL inside doctest's stringifier, beats doctest's own template, and
// makes the decomposer try std::string_view + const char* -- a hard compile error on EVERY lane.
[[nodiscard]] std::string_view audioClipLoadStatusLabel(AudioClipLoadStatus status) noexcept;

// Declared here rather than below so AudioClip can befriend its one producer: loadAudioClip is the
// only thing in this tree that may fill a clip's members, and there is deliberately no public
// constructor that takes bytes.
struct AudioClipLoadResult;

// The runtime clip. It OWNS the whole file buffer and is COPY-DELETED, and the deletion is the point
// rather than an optimisation.
//
// Two shapes were rejected, and they are recorded here so neither is re-proposed:
//
//   1. Holding an assets::CookedAudio view BESIDE the buffer -- the shape CookedAnimation uses --
//      makes this type SELF-REFERENTIAL: a copy duplicates the vector and leaves the span pointing
//      into the ORIGINAL. CookedAnimation gets away with it because it never owns its bytes and its
//      header spends six lines saying so. A type that OWNS and VIEWS at once must not be copyable,
//      and the cheapest way to make that unmistakable is not to hold the view at all. So: one vector,
//      four scalars, and sampleBytes() computed from the fixed offset.
//   2. Converting to std::vector<std::int16_t> at load costs a full SECOND copy of the largest asset
//      class in the project (up to ~110 MiB) to save two byte loads per sample.
//
// THE REALTIME COST, so task 3.7.2 does not rediscover it as a surprise: getU16 is two byte loads and
// a shift, which every lane's optimiser is expected to fold into one 16-bit load; at 48 kHz stereo
// that is well under a microsecond per device callback either way. If a Tracy capture ever says
// otherwise, the answer is a mixer-side staging buffer -- NEVER a reinterpret_cast over the region,
// for the reason cooked_animation.hpp:154-161 states: a file's region carries no alignment guarantee
// this program's types satisfy.
class AudioClip {
public:
    AudioClip() = default;
    AudioClip(AudioClip&&) noexcept = default;
    AudioClip& operator=(AudioClip&&) noexcept = default;
    AudioClip(const AudioClip&) = delete;
    AudioClip& operator=(const AudioClip&) = delete;
    ~AudioClip() = default;

    // False on a default-constructed clip and on a moved-from one. Only loadAudioClip's Ok path
    // produces a valid clip; there is deliberately no public constructor that takes bytes.
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] std::uint32_t sampleRate() const noexcept { return rate; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return chans; }
    [[nodiscard]] std::uint32_t frameCount() const noexcept { return frames; }
    [[nodiscard]] Guid sourceGuid() const noexcept { return guid; }

    // channels * frameCount, in u64 -- the product is computed in 64 bits for the same reason
    // parseCookedAudio computes it there (cooked_audio.cpp step 8) and never in u32.
    [[nodiscard]] std::uint64_t sampleCount() const noexcept;

    // THE one duration spelling, forwarded to assets::cookedAudioDurationSeconds rather than
    // restated, so no two callers in this tree can disagree. 0.0F on an invalid clip.
    [[nodiscard]] float durationSeconds() const noexcept;

    // Exactly COOKED_AUDIO_SAMPLE_BYTES * sampleCount() bytes starting at COOKED_AUDIO_HEADER_BYTES,
    // and EMPTY on an invalid clip. Both bounds come from this object's own members; nothing here
    // re-parses the buffer.
    [[nodiscard]] std::span<const std::byte> sampleBytes() const noexcept;

    // TOTAL IN BOTH DIMENSIONS: an out-of-range frame or channel answers 0. Silence is the right
    // answer to a caller bug in an audio path and it must never become a read. The index is
    // FRAME-MAJOR (docs/09 section 14.2): frame f, channel c is at sample index f * channels + c.
    // There is no planar layout and no flag that could select one.
    [[nodiscard]] std::int16_t frameSample(std::uint32_t frame, std::uint32_t channel) const noexcept;

private:
    friend AudioClipLoadResult loadAudioClip(const VirtualFileSystem& vfs, std::string_view virtualPath);

    // Members are plain camelBack with NO trailing underscore, and where an accessor would collide
    // the MEMBER takes the distinct name -- rate/sampleRate(), chans/channels(), frames/frameCount(),
    // guid/sourceGuid(). That is the RenderTarget::depthFormatValue/depthFormat() precedent.
    std::vector<std::byte> storage;
    std::uint32_t rate = 0;
    std::uint32_t chans = 0;
    std::uint32_t frames = 0;
    Guid guid;
};

struct AudioClipLoadResult {
    AudioClipLoadStatus status = AudioClipLoadStatus::Ok;
    std::string message;  // "" IFF Ok; names the offending value AND its bound wherever there is one
    AudioClip clip;       // clip.valid() IFF Ok
};

// Reads one .aerowave through the VFS and returns it as an owning clip. NEVER THROWS. NEVER LOGS.
//
// THE CALL ORDER IS PART OF THE CONTRACT: exists() -> fileSize() -> readFile(), in that order.
//
//   1. !vfs.exists(path)                            -> NotFound
//   2. !vfs.fileSize(path)                          -> Unreadable
//      *size > assets::MAX_COOKED_AUDIO_BYTES       -> TooLarge, naming both numbers, WITHOUT READING
//   3. !vfs.readFile(path)                          -> Unreadable
//   4. parseCookedAudio(bytes).status != Ok         -> ParseFailed, message = label + ": " + message
//   5. otherwise the buffer is MOVED into the clip; the four scalars were copied out first
//
// exists() before readFile() is NOT redundant -- it is the ONLY thing that separates NotFound from
// Unreadable, given vfs.hpp's documented nullopt. fileSize() before readFile() means a hostile or
// corrupt 4 GB file costs a stat rather than a 4 GB allocation. And step 5 MOVES, never copies: the
// four scalars are read out of the parse result BEFORE the move, because parseCookedAudio's span
// points into the very buffer being moved from and is dead the instant it happens -- which is
// precisely the self-referential hazard AudioClip's deleted copy exists to make unspellable.
[[nodiscard]] AudioClipLoadResult loadAudioClip(const VirtualFileSystem& vfs, std::string_view virtualPath);

}  // namespace engine::audio

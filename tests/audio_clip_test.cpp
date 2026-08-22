// tests/audio_clip_test.cpp -- task 3.7.1: engine::audio::AudioClip and loadAudioClip, the pair that
// OPENS engine/audio. A TU of aero_tests, which supplies main() from test_main.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// HERMETIC BY CONSTRUCTION. Thirteen of the fourteen cases touch no disk at all: vfs.hpp's
// FileSystemBackend is a public polymorphic base with a virtual destructor and three pure virtuals,
// so this file implements two of its own and mounts them into a real VirtualFileSystem. The whole
// res:// path is therefore exercised end to end with bytes that never left memory, and the two
// statuses that only a backend can produce -- Unreadable and TooLarge -- become testable at all.
// AC13 is the one arm that reads the committed tone.aerowave through a real DirectoryBackend.
//
// AC7 AND AC8 ARE THE TWO ARMS THIS TIER EXISTS FOR. Everything else here could be inferred from the
// container's own tests; those two are the only place NotFound, Unreadable and TooLarge are proven to
// be three different answers rather than three names for one nullopt.
#include <aero/assets/audio_cook.hpp>
#include <aero/assets/cooked_audio.hpp>
#include <aero/audio/audio.hpp>
#include <aero/core/vfs.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
// <ostream> is load-bearing on MSVC (the 0.4.1 trap): doctest stringifies the std::string_view
// operands of the label CHECKs below through operator<<(std::ostream&, std::string_view), which MS
// STL defines inline in <string_view> against an INCOMPLETE std::basic_ostream -- only <ostream>
// completes it. libc++ and libstdc++ are self-sufficient, so omitting it builds clean on macOS and
// Linux and fails only on the Windows lane, inside the STL headers rather than at the CHECK. Written
// when the TU was created rather than after a Windows lane said so.
#include <ostream>
#include <utility>
#include <vector>

using engine::ByteBuffer;
using engine::Guid;
using engine::assets::AudioCookInput;
using engine::assets::AudioCookResult;
using engine::assets::AudioCookStatus;
using engine::assets::cookAudio;
using engine::assets::COOKED_AUDIO_HEADER_BYTES;
using engine::assets::CookedAudioStatus;
using engine::assets::cookedAudioStatusLabel;
using engine::assets::MAX_COOKED_AUDIO_BYTES;
using engine::audio::AudioClip;
using engine::audio::AudioClipLoadResult;
using engine::audio::AudioClipLoadStatus;
using engine::audio::audioClipLoadStatusLabel;
using engine::audio::loadAudioClip;

namespace {

// The tree's standard test GUID -- hi = 0x0123456789abcdef, lo = 0xfedcba9876543210, every byte
// non-zero, so an assertion against it is a statement about byte ORDER and not merely presence. It is
// the same value tests/fixtures/audio/tone.aerowave was cooked with.
constexpr Guid TEST_GUID{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};

constexpr std::string_view CLIP_PATH = "res://clip.aerowave";

// The committed fixture's own arithmetic, as literals: 1.0 s at 8 kHz mono is 8000 frames, whose
// artifact is 64 + 2 x 1 x 8000 = 16064 bytes. See tests/fixtures/audio/README.md.
constexpr std::uint32_t FIXTURE_FRAMES = 8000;
constexpr std::size_t FIXTURE_BYTES = 16064;

// A backend serving ONE path out of memory. Not a fixture file: the whole point is that the loader's
// res:// path is exercised with no disk at all.
class MemoryBackend final : public engine::FileSystemBackend {
public:
    MemoryBackend(std::string name, ByteBuffer bytes) : fileName(std::move(name)), content(std::move(bytes)) {}

    [[nodiscard]] bool exists(std::string_view relPath) const override { return relPath == fileName; }

    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view relPath) const override {
        if (relPath != fileName) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(content.size());
    }

    [[nodiscard]] std::optional<ByteBuffer> readFile(std::string_view relPath) const override {
        if (relPath != fileName) {
            return std::nullopt;
        }
        return content;
    }

private:
    std::string fileName;
    ByteBuffer content;
};

// The Unreadable arm's backend: exists() and fileSize() both answer, and readFile() refuses. That
// combination is unreachable through a DirectoryBackend on a healthy filesystem, which is exactly why
// the status needs a hand-written backend to witness it at all.
class UnreadableBackend final : public engine::FileSystemBackend {
public:
    explicit UnreadableBackend(bool sizeReadable) : sizeAnswerable(sizeReadable) {}

    [[nodiscard]] bool exists(std::string_view /*relPath*/) const override { return true; }

    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view /*relPath*/) const override {
        if (!sizeAnswerable) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(COOKED_AUDIO_HEADER_BYTES);
    }

    [[nodiscard]] std::optional<ByteBuffer> readFile(std::string_view /*relPath*/) const override {
        return std::nullopt;
    }

private:
    bool sizeAnswerable;
};

// The TooLarge arm's backend: exists() is true, fileSize() is over the cap, and readFile() calls
// REQUIRE(false). "REFUSED WITHOUT READING" is therefore ASSERTED rather than assumed -- swap the
// stat and the read and this case FAILS LOUDLY instead of quietly passing on a 110 MiB allocation.
class AbortOnReadBackend final : public engine::FileSystemBackend {
public:
    [[nodiscard]] bool exists(std::string_view /*relPath*/) const override { return true; }

    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view /*relPath*/) const override {
        return MAX_COOKED_AUDIO_BYTES + 1;
    }

    [[nodiscard]] std::optional<ByteBuffer> readFile(std::string_view /*relPath*/) const override {
        REQUIRE_MESSAGE(false, "loadAudioClip read a file it had already refused as TooLarge");
        return std::nullopt;
    }
};

// A backend answering exists() == false for everything, mounted so the miss is the BACKEND's answer
// rather than an empty mount table.
class AbsentBackend final : public engine::FileSystemBackend {
public:
    [[nodiscard]] bool exists(std::string_view /*relPath*/) const override { return false; }

    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view /*relPath*/) const override {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<ByteBuffer> readFile(std::string_view /*relPath*/) const override {
        return std::nullopt;
    }
};

[[nodiscard]] ByteBuffer cookedBytes(std::uint32_t rate, std::uint32_t channels, std::span<const std::int16_t> samples,
                                     Guid guid = TEST_GUID) {
    AudioCookInput input;
    input.sourceGuid = guid;
    input.sampleRate = rate;
    input.channels = channels;
    input.samples = samples;
    const AudioCookResult cooked = cookAudio(input);
    REQUIRE(cooked.status == AudioCookStatus::Ok);
    return ByteBuffer(cooked.bytes.begin(), cooked.bytes.end());
}

// Two channels x three frames, frame-major: frame f, channel c is at index f * 2 + c. Every value is
// distinct, so a channel-major or off-by-one-frame defect cannot land on the value it should have.
constexpr std::array<std::int16_t, 6> STEREO_SAMPLES{100, -100, 200, -200, 300, -300};

[[nodiscard]] AudioClipLoadResult loadFrom(std::unique_ptr<engine::FileSystemBackend> backend,
                                           std::string_view path = CLIP_PATH) {
    engine::VirtualFileSystem vfs;
    vfs.mount(std::move(backend));
    return loadAudioClip(vfs, path);
}

}  // namespace

TEST_CASE("AC1: a good .aerowave loads through a mounted backend") {
    const AudioClipLoadResult result =
        loadFrom(std::make_unique<MemoryBackend>("clip.aerowave", cookedBytes(8000, 2, STEREO_SAMPLES)));
    CHECK(result.status == AudioClipLoadStatus::Ok);
    CHECK(result.message.empty());
    CHECK(result.clip.valid());
}

TEST_CASE("AC2: every field survives the cook -> VFS -> load round trip") {
    const AudioClipLoadResult result =
        loadFrom(std::make_unique<MemoryBackend>("clip.aerowave", cookedBytes(44100, 2, STEREO_SAMPLES)));
    REQUIRE(result.status == AudioClipLoadStatus::Ok);
    CHECK(result.clip.sampleRate() == 44100U);
    CHECK(result.clip.channels() == 2U);
    CHECK(result.clip.frameCount() == 3U);
    CHECK(result.clip.sourceGuid() == TEST_GUID);
    CHECK(result.clip.sampleCount() == 6ULL);
    // 3 / 44100 is not exactly representable, so this is the ONE spelling used rather than a literal:
    // the clip forwards to assets::cookedAudioDurationSeconds and so does this assertion.
    CHECK(result.clip.durationSeconds() == engine::assets::cookedAudioDurationSeconds(44100, 3));
}

TEST_CASE("AC3: sampleBytes() is exactly the sample region and never a copy of the header") {
    const ByteBuffer source = cookedBytes(8000, 2, STEREO_SAMPLES);
    const AudioClipLoadResult result = loadFrom(std::make_unique<MemoryBackend>("clip.aerowave", source));
    REQUIRE(result.status == AudioClipLoadStatus::Ok);

    const std::span<const std::byte> region = result.clip.sampleBytes();
    REQUIRE(region.size() == 2U * 2U * 3U);
    // The first and last bytes of the region are the source file's bytes at offset 64 and at the end.
    CHECK(region.front() == source[COOKED_AUDIO_HEADER_BYTES]);
    CHECK(region.back() == source.back());
    // And the region begins exactly COOKED_AUDIO_HEADER_BYTES into the clip's own storage -- proven
    // by comparing every byte rather than by trusting the size alone.
    for (std::size_t i = 0; i < region.size(); ++i) {
        CHECK(region[i] == source[COOKED_AUDIO_HEADER_BYTES + i]);
    }
}

TEST_CASE("AC4: frameSample is frame-major, proven against literals") {
    const AudioClipLoadResult result =
        loadFrom(std::make_unique<MemoryBackend>("clip.aerowave", cookedBytes(8000, 2, STEREO_SAMPLES)));
    REQUIRE(result.status == AudioClipLoadStatus::Ok);
    const AudioClip& clip = result.clip;

    CHECK(clip.frameSample(0, 0) == 100);   // index 0
    CHECK(clip.frameSample(0, 1) == -100);  // index 1
    CHECK(clip.frameSample(1, 0) == 200);   // index 2 -- a channel-major layout would read -100 here
    CHECK(clip.frameSample(1, 1) == -200);  // index 3
    CHECK(clip.frameSample(2, 0) == 300);   // the LAST frame, index 4
    CHECK(clip.frameSample(2, 1) == -300);  // index 5
}

TEST_CASE("AC5: frameSample is TOTAL in both dimensions") {
    const AudioClipLoadResult result =
        loadFrom(std::make_unique<MemoryBackend>("clip.aerowave", cookedBytes(8000, 2, STEREO_SAMPLES)));
    REQUIRE(result.status == AudioClipLoadStatus::Ok);
    const AudioClip& clip = result.clip;

    CHECK(clip.frameSample(3, 0) == 0);  // frame == frameCount
    CHECK(clip.frameSample(UINT32_MAX, 0) == 0);
    CHECK(clip.frameSample(0, 2) == 0);  // channel == channels
    CHECK(clip.frameSample(0, UINT32_MAX) == 0);
    // The dangerous one: a channel past the count with a frame IN range would land on the next
    // frame's data if the two bounds were folded into one index comparison.
    CHECK(clip.frameSample(1, 2) == 0);
    CHECK(clip.frameSample(UINT32_MAX, UINT32_MAX) == 0);
}

TEST_CASE("AC6: NotFound, both with no mount at all and with a backend that says no") {
    const engine::VirtualFileSystem empty;
    const AudioClipLoadResult unmounted = loadAudioClip(empty, CLIP_PATH);
    CHECK(unmounted.status == AudioClipLoadStatus::NotFound);
    CHECK_FALSE(unmounted.message.empty());
    CHECK_FALSE(unmounted.clip.valid());

    const AudioClipLoadResult absent = loadFrom(std::make_unique<AbsentBackend>());
    CHECK(absent.status == AudioClipLoadStatus::NotFound);
    CHECK_FALSE(absent.clip.valid());

    // A mounted backend that serves a DIFFERENT name is a miss too -- exists() is the only thing
    // consulted, so the answer must not depend on which of the three calls noticed.
    const AudioClipLoadResult wrongName =
        loadFrom(std::make_unique<MemoryBackend>("other.aerowave", cookedBytes(8000, 1, STEREO_SAMPLES)));
    CHECK(wrongName.status == AudioClipLoadStatus::NotFound);
}

TEST_CASE("AC7: Unreadable is separable from NotFound, which is what exists() is for") {
    // THE ARM THAT PROVES Unreadable IS REACHABLE AT ALL. vfs.hpp:87-89 records that a nullopt read
    // means "absent OR unreadable" and does not distinguish them, so this status exists only because
    // loadAudioClip asks exists() first; a loader that skipped that call could not produce it.
    //
    // MEASURED, because the obvious attribution is wrong: deleting the exists() call reddens AC6 --
    // all three of its arms -- and leaves THIS case GREEN, since both shapes below still end in
    // Unreadable either way. The separation is the PAIR: AC6 pins the exists()-false side onto
    // NotFound and this case pins the exists()-true side onto Unreadable. Neither alone witnesses it.
    //
    // Both readFile-refusing shapes are driven: one whose fileSize() also refuses, and one whose
    // fileSize() answers and whose READ is what fails.
    const AudioClipLoadResult noSize = loadFrom(std::make_unique<UnreadableBackend>(false));
    CHECK(noSize.status == AudioClipLoadStatus::Unreadable);
    CHECK_FALSE(noSize.message.empty());
    CHECK_FALSE(noSize.clip.valid());

    const AudioClipLoadResult noRead = loadFrom(std::make_unique<UnreadableBackend>(true));
    CHECK(noRead.status == AudioClipLoadStatus::Unreadable);
    CHECK_FALSE(noRead.message.empty());
    CHECK_FALSE(noRead.clip.valid());
}

TEST_CASE("AC8: TooLarge is refused WITHOUT READING, asserted rather than assumed") {
    // AbortOnReadBackend::readFile calls REQUIRE(false). If the stat and the read were ever swapped,
    // this case fails loudly instead of quietly allocating 110 MiB and passing.
    const AudioClipLoadResult result = loadFrom(std::make_unique<AbortOnReadBackend>());
    CHECK(result.status == AudioClipLoadStatus::TooLarge);
    CHECK_FALSE(result.clip.valid());
    // The message names BOTH numbers: the offending size and its bound.
    CHECK(result.message.find(std::to_string(MAX_COOKED_AUDIO_BYTES + 1)) != std::string::npos);
    CHECK(result.message.find(std::to_string(MAX_COOKED_AUDIO_BYTES)) != std::string::npos);
}

TEST_CASE("AC9: ParseFailed carries the parser's own label") {
    // Bad magic: a valid artifact with one byte of the magic flipped.
    ByteBuffer corrupt = cookedBytes(8000, 1, STEREO_SAMPLES);
    corrupt[7] = static_cast<std::byte>('F');  // AEROWAVE -> AEROWAVF
    const AudioClipLoadResult badMagic = loadFrom(std::make_unique<MemoryBackend>("clip.aerowave", corrupt));
    CHECK(badMagic.status == AudioClipLoadStatus::ParseFailed);
    CHECK_FALSE(badMagic.clip.valid());
    CHECK(badMagic.message.find(std::string(cookedAudioStatusLabel(CookedAudioStatus::BadMagic))) != std::string::npos);

    // One byte short of the header.
    const ByteBuffer tiny(COOKED_AUDIO_HEADER_BYTES - 1, std::byte{0});
    const AudioClipLoadResult tooSmall = loadFrom(std::make_unique<MemoryBackend>("clip.aerowave", tiny));
    CHECK(tooSmall.status == AudioClipLoadStatus::ParseFailed);
    CHECK(tooSmall.message.find(std::string(cookedAudioStatusLabel(CookedAudioStatus::TooSmall))) != std::string::npos);
}

TEST_CASE("AC10: a default-constructed clip is inert in every dimension") {
    const AudioClip clip;
    CHECK_FALSE(clip.valid());
    CHECK(clip.sampleRate() == 0U);
    CHECK(clip.channels() == 0U);
    CHECK(clip.frameCount() == 0U);
    CHECK(clip.sampleCount() == 0ULL);
    CHECK(clip.sampleBytes().empty());
    CHECK(clip.frameSample(0, 0) == 0);
    CHECK(clip.durationSeconds() == 0.0F);
    CHECK_FALSE(clip.sourceGuid().valid());  // the nil GUID: Guid::valid() is false iff both halves are 0
}

TEST_CASE("AC11: a move takes the buffer and leaves the source invalid") {
    AudioClipLoadResult result =
        loadFrom(std::make_unique<MemoryBackend>("clip.aerowave", cookedBytes(8000, 2, STEREO_SAMPLES)));
    REQUIRE(result.status == AudioClipLoadStatus::Ok);

    const AudioClip moved = std::move(result.clip);
    CHECK(moved.valid());
    CHECK(moved.sampleRate() == 8000U);
    CHECK(moved.channels() == 2U);
    CHECK(moved.frameCount() == 3U);
    CHECK(moved.sourceGuid() == TEST_GUID);
    CHECK(moved.sampleBytes().size() == 12U);
    CHECK(moved.frameSample(2, 1) == -300);  // the last sample survived intact

    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move) -- the assertion IS that
    // a moved-from clip is inert, which is exactly what makes it safe to touch here.
    CHECK_FALSE(result.clip.valid());
    CHECK(result.clip.sampleBytes().empty());
    CHECK(result.clip.frameSample(0, 0) == 0);
}

TEST_CASE("AC12: AudioClip is move-only, and the move operations are noexcept") {
    // D18: the copy deletion is the POINT, not an optimisation -- a type that owns bytes and hands
    // out a span into them must not be copyable.
    static_assert(!std::is_copy_constructible_v<AudioClip>);
    static_assert(!std::is_copy_assignable_v<AudioClip>);
    static_assert(std::is_nothrow_move_constructible_v<AudioClip>);
    static_assert(std::is_nothrow_move_assignable_v<AudioClip>);
    CHECK_FALSE(std::is_copy_constructible_v<AudioClip>);
    CHECK_FALSE(std::is_copy_assignable_v<AudioClip>);
    CHECK(std::is_nothrow_move_constructible_v<AudioClip>);
    CHECK(std::is_nothrow_move_assignable_v<AudioClip>);
}

TEST_CASE("AC13: the committed tone.aerowave loads through a real DirectoryBackend") {
    // The one arm on disk, and the only end-to-end one: a real directory, the real committed artifact,
    // the real VFS. The fixture is 1.0 s of 8 kHz mono -- 8000 frames, 64 + 2 x 1 x 8000 = 16064
    // bytes -- cooked once by the real aero_cooker binary with the standard test GUID and FROZEN.
    // A PATH, not a flag: a missing fixture is a REQUIRE failure rather than a silent skip.
    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_AUDIO_FIXTURES_DIR));

    const AudioClipLoadResult result = loadAudioClip(vfs, "res://tone.aerowave");
    REQUIRE(result.status == AudioClipLoadStatus::Ok);
    CHECK(result.message.empty());
    REQUIRE(result.clip.valid());
    CHECK(result.clip.sampleRate() == 8000U);
    CHECK(result.clip.channels() == 1U);
    CHECK(result.clip.frameCount() == FIXTURE_FRAMES);
    CHECK(result.clip.sampleCount() == static_cast<std::uint64_t>(FIXTURE_FRAMES));
    CHECK(result.clip.sourceGuid() == TEST_GUID);
    CHECK(result.clip.durationSeconds() == 1.0F);
    CHECK(result.clip.sampleBytes().size() == FIXTURE_BYTES - COOKED_AUDIO_HEADER_BYTES);
    // The anchor's own first four samples, measured from tone.s16le.pcm and recorded in the fixture
    // README: 0, 1110, 2088, 2820.
    CHECK(result.clip.frameSample(0, 0) == 0);
    CHECK(result.clip.frameSample(1, 0) == 1110);
    CHECK(result.clip.frameSample(2, 0) == 2088);
    CHECK(result.clip.frameSample(3, 0) == 2820);
    // And its last four: -3218, -2820, -2089, -1110.
    CHECK(result.clip.frameSample(FIXTURE_FRAMES - 4, 0) == -3218);
    CHECK(result.clip.frameSample(FIXTURE_FRAMES - 3, 0) == -2820);
    CHECK(result.clip.frameSample(FIXTURE_FRAMES - 2, 0) == -2089);
    CHECK(result.clip.frameSample(FIXTURE_FRAMES - 1, 0) == -1110);
    CHECK(result.clip.frameSample(FIXTURE_FRAMES, 0) == 0);  // one past the end is silence
}

TEST_CASE("AC14: audioClipLoadStatusLabel is injective and never empty") {
    constexpr std::array<AudioClipLoadStatus, 5> ALL{
        AudioClipLoadStatus::Ok,       AudioClipLoadStatus::NotFound,    AudioClipLoadStatus::Unreadable,
        AudioClipLoadStatus::TooLarge, AudioClipLoadStatus::ParseFailed,
    };
    for (std::size_t i = 0; i < ALL.size(); ++i) {
        CHECK_FALSE(audioClipLoadStatusLabel(ALL[i]).empty());
        for (std::size_t j = i + 1; j < ALL.size(); ++j) {
            CHECK(audioClipLoadStatusLabel(ALL[i]) != audioClipLoadStatusLabel(ALL[j]));
        }
    }
}

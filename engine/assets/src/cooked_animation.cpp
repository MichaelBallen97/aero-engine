// Aero Engine — the cooked animation clip container v1: the three labels and the hostile-input
// parser (task 3.5.2). See cooked_animation.hpp for the contract and docs/09-file-formats.md
// section 13 for the normative format. NEVER THROWS. NEVER READS A FILE. NEVER LOGS. Reserves
// nothing before the header's three counts have been validated against their frozen caps.
#include <aero/assets/cooked_animation.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::assets {
namespace {

// The header's field offsets, named ONCE. docs/09 section 13.2 is the normative table; these mirror
// it and nothing else in this TU spells a header offset as a literal.
constexpr std::size_t H_MAGIC = 0;
constexpr std::size_t H_FORMAT_VERSION = 8;
constexpr std::size_t H_COOKER_VERSION = 12;
constexpr std::size_t H_GUID_HI = 16;
constexpr std::size_t H_GUID_LO = 24;
constexpr std::size_t H_RESERVED_FLAGS = 32;
constexpr std::size_t H_CHANNEL_COUNT = 36;
constexpr std::size_t H_KEY_COUNT = 40;
constexpr std::size_t H_VALUE_COUNT = 44;
constexpr std::size_t H_SOURCE_ANIMATION_INDEX = 48;
constexpr std::size_t H_DURATION_SECONDS = 52;
constexpr std::size_t H_TIMES_DATA_OFFSET = 56;
constexpr std::size_t H_VALUES_DATA_OFFSET = 64;
constexpr std::size_t H_TOTAL_BYTES = 72;
static_assert(H_TOTAL_BYTES + 8 == COOKED_ANIMATION_HEADER_BYTES);

// Channel-record field offsets (docs/09 section 13.3).
constexpr std::size_t C_TARGET_NODE_LOCAL_ID = 0;
constexpr std::size_t C_PATH = 4;
constexpr std::size_t C_INTERPOLATION = 6;
constexpr std::size_t C_KEY_COUNT = 8;
constexpr std::size_t C_FIRST_KEY = 12;
constexpr std::size_t C_FIRST_VALUE = 16;
constexpr std::size_t C_VALUE_COUNT = 20;
constexpr std::size_t C_RESERVED0 = 24;
static_assert(C_RESERVED0 + 8 == COOKED_ANIMATION_CHANNEL_BYTES);

// The two bulk strides, in bytes per element. Named so no arithmetic below carries a bare 4 or 16.
constexpr std::uint64_t TIME_STRIDE = 4;
constexpr std::uint64_t VALUE_STRIDE = 16;

[[nodiscard]] CookedAnimationParseResult refuse(CookedAnimationStatus status, std::string message) {
    CookedAnimationParseResult out;
    out.status = status;
    out.message = std::move(message);
    return out;
}

}  // namespace

std::string_view cookedAnimationPathLabel(CookedAnimationPath path) noexcept {
    switch (path) {
        case CookedAnimationPath::Translation:
            return "Translation";
        case CookedAnimationPath::Rotation:
            return "Rotation";
        case CookedAnimationPath::Scale:
            return "Scale";
    }
    return "Unknown";  // reachable: the enum has a fixed underlying type, so every u16 is a value
}

std::string_view cookedAnimationInterpolationLabel(CookedAnimationInterpolation interpolation) noexcept {
    switch (interpolation) {
        case CookedAnimationInterpolation::Linear:
            return "Linear";
        case CookedAnimationInterpolation::Step:
            return "Step";
        case CookedAnimationInterpolation::CubicSpline:
            return "Cubic spline";
    }
    return "Unknown";  // reachable, for the same reason
}

std::string_view cookedAnimationStatusLabel(CookedAnimationStatus status) noexcept {
    switch (status) {
        case CookedAnimationStatus::Ok:
            return "Ok";
        case CookedAnimationStatus::TooSmall:
            return "Too small";
        case CookedAnimationStatus::BadMagic:
            return "Bad magic";
        case CookedAnimationStatus::UnsupportedVersion:
            return "Unsupported version";
        case CookedAnimationStatus::ReservedNotZero:
            return "Reserved field not zero";
        case CookedAnimationStatus::SizeMismatch:
            return "Size mismatch";
        case CookedAnimationStatus::CapExceeded:
            return "Cap exceeded";
        case CookedAnimationStatus::BadTable:
            return "Bad table";
        case CookedAnimationStatus::BadRange:
            return "Bad range";
    }
    return "Unknown";  // unreachable; the switch has no default so a new enumerator is a -Wswitch error
}

std::span<const std::byte> channelTimeBytes(const CookedAnimation& animation, std::uint32_t channelIndex) noexcept {
    if (channelIndex >= animation.channels.size()) {
        return {};
    }
    const CookedAnimationChannel& channel = animation.channels[channelIndex];
    const std::uint64_t offset = animation.timesDataOffset + (TIME_STRIDE * channel.firstKey);
    const std::uint64_t length = TIME_STRIDE * channel.keyCount;
    // Defence in depth over an already-validated parse: a SUBTRACTION against the known-good size,
    // never an addition that can wrap.
    if (offset > animation.bytes.size() || animation.bytes.size() - offset < length) {
        return {};
    }
    return animation.bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
}

std::span<const std::byte> channelValueBytes(const CookedAnimation& animation, std::uint32_t channelIndex) noexcept {
    if (channelIndex >= animation.channels.size()) {
        return {};
    }
    const CookedAnimationChannel& channel = animation.channels[channelIndex];
    const std::uint64_t offset = animation.valuesDataOffset + (VALUE_STRIDE * channel.firstValue);
    const std::uint64_t length = VALUE_STRIDE * channel.valueCount;
    if (offset > animation.bytes.size() || animation.bytes.size() - offset < length) {
        return {};
    }
    return animation.bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
}

CookedAnimationParseResult parseCookedAnimation(std::span<const std::byte> bytes) {
    // 1. shorter than the header.
    if (bytes.size() < COOKED_ANIMATION_HEADER_BYTES) {
        return refuse(CookedAnimationStatus::TooSmall,
                      std::format("the buffer is {} bytes, shorter than the {}-byte header", bytes.size(),
                                  COOKED_ANIMATION_HEADER_BYTES));
    }

    // 2. magic. Compared BYTE BY BYTE over all eight bytes -- never a memcmp of a reinterpret_cast'd
    //    pointer, and never a prefix: the last byte is as load-bearing as the first.
    for (std::size_t i = 0; i < COOKED_ANIMATION_MAGIC.size(); ++i) {
        if (bytes[H_MAGIC + i] != static_cast<std::byte>(COOKED_ANIMATION_MAGIC[i])) {
            return refuse(CookedAnimationStatus::BadMagic, "the buffer does not begin with AEROANIM");
        }
    }

    // 3. version.
    const std::uint32_t formatVersion = getU32(bytes, H_FORMAT_VERSION);
    if (formatVersion != COOKED_ANIMATION_FORMAT_VERSION) {
        return refuse(CookedAnimationStatus::UnsupportedVersion,
                      std::format("cooked animation format version {} (this build reads version {})", formatVersion,
                                  COOKED_ANIMATION_FORMAT_VERSION));
    }

    // 4. the header's reserved space. A REFUSAL, deliberately: occupying one of these is a
    //    formatVersion bump, so a non-zero value here is a file this build cannot claim to read.
    if (getU32(bytes, H_RESERVED_FLAGS) != 0) {
        return refuse(CookedAnimationStatus::ReservedNotZero, "the header's reserved flags field is not zero");
    }

    // 5. the three counts against zero. Cheap, cap-independent, and it is what makes "a .aeroanim is
    //    never empty" a parse requirement rather than a comment.
    const std::uint32_t channelCount = getU32(bytes, H_CHANNEL_COUNT);
    const std::uint32_t keyCount = getU32(bytes, H_KEY_COUNT);
    const std::uint32_t valueCount = getU32(bytes, H_VALUE_COUNT);
    if (channelCount == 0) {
        return refuse(CookedAnimationStatus::BadTable, "the header declares zero channels");
    }
    if (keyCount == 0) {
        return refuse(CookedAnimationStatus::BadTable, "the header declares zero keys");
    }
    if (valueCount == 0) {
        return refuse(CookedAnimationStatus::BadTable, "the header declares zero values");
    }

    // 6. the caps. NOTHING IS RESERVED UNTIL THIS BLOCK HAS PASSED -- that is why it is here.
    if (channelCount > MAX_COOKED_ANIMATION_CHANNELS) {
        return refuse(CookedAnimationStatus::CapExceeded,
                      std::format("the header declares {} channels, over the cap of {}", channelCount,
                                  MAX_COOKED_ANIMATION_CHANNELS));
    }
    if (keyCount > MAX_COOKED_ANIMATION_KEYS) {
        return refuse(CookedAnimationStatus::CapExceeded, std::format("the header declares {} keys, over the cap of {}",
                                                                      keyCount, MAX_COOKED_ANIMATION_KEYS));
    }
    if (valueCount > MAX_COOKED_ANIMATION_VALUES) {
        return refuse(
            CookedAnimationStatus::CapExceeded,
            std::format("the header declares {} values, over the cap of {}", valueCount, MAX_COOKED_ANIMATION_VALUES));
    }

    // 7. totalBytes against the buffer. COMPARED, never derived: a stored field a reader recomputes
    //    and ignores is a decorative field.
    const std::uint64_t totalBytes = getU64(bytes, H_TOTAL_BYTES);
    if (totalBytes != bytes.size()) {
        return refuse(
            CookedAnimationStatus::SizeMismatch,
            std::format("the header declares {} total bytes but the buffer holds {}", totalBytes, bytes.size()));
    }

    // 8. the two region offsets, EXACTLY. The format has one legal layout and both counts are now
    //    inside their caps, so the expected offsets are computable and total -- and equality is the
    //    only check that can see a MISPOSITIONED padding site at all (docs/09 section 13.10). Four
    //    comparisons, four distinguishable messages.
    const std::uint64_t timesDataOffset = getU64(bytes, H_TIMES_DATA_OFFSET);
    const std::uint64_t valuesDataOffset = getU64(bytes, H_VALUES_DATA_OFFSET);
    const std::uint64_t expectedTimesOffset =
        COOKED_ANIMATION_HEADER_BYTES + (static_cast<std::uint64_t>(COOKED_ANIMATION_CHANNEL_BYTES) * channelCount);
    if (timesDataOffset % COOKED_ANIMATION_ALIGNMENT != 0) {
        return refuse(CookedAnimationStatus::BadRange,
                      std::format("the times region offset is not 16-aligned ({})", timesDataOffset));
    }
    if (timesDataOffset != expectedTimesOffset) {
        return refuse(CookedAnimationStatus::BadRange,
                      std::format("the times region does not begin immediately after the channel table (offset {}, "
                                  "expected {})",
                                  timesDataOffset, expectedTimesOffset));
    }
    const std::uint64_t expectedValuesOffset =
        timesDataOffset + (TIME_STRIDE * keyCount) + cookedAnimationTimesPadding(keyCount);
    if (valuesDataOffset % COOKED_ANIMATION_ALIGNMENT != 0) {
        return refuse(CookedAnimationStatus::BadRange,
                      std::format("the values region offset is not 16-aligned ({})", valuesDataOffset));
    }
    if (valuesDataOffset != expectedValuesOffset) {
        return refuse(CookedAnimationStatus::BadRange,
                      std::format("the gap between the times and values regions is not this format's single padding "
                                  "site (offset {}, expected {})",
                                  valuesDataOffset, expectedValuesOffset));
    }

    // 9. totalBytes against the format's own arithmetic. The second half of the size contract: the
    //    values region is the last thing in the file and it ends exactly at totalBytes.
    const std::uint64_t expectedTotalBytes = valuesDataOffset + (VALUE_STRIDE * valueCount);
    if (totalBytes != expectedTotalBytes) {
        return refuse(CookedAnimationStatus::SizeMismatch,
                      std::format("the header declares {} total bytes but {} values at offset {} need {}", totalBytes,
                                  valueCount, valuesDataOffset, expectedTotalBytes));
    }

    // 10. THE padding site, CHECKED rather than assumed. 0 to 12 bytes wide, so this is a bounded
    //     loop and never a scan. A non-zero byte here is a file that means something this build
    //     cannot read, which is a refusal for the same reason a reserved field is.
    const std::uint64_t paddingBegin = timesDataOffset + (TIME_STRIDE * keyCount);
    for (std::uint64_t at = paddingBegin; at < valuesDataOffset; ++at) {
        if (bytes[static_cast<std::size_t>(at)] != std::byte{0}) {
            return refuse(CookedAnimationStatus::ReservedNotZero,
                          std::format("the padding byte at offset {} is not zero", at));
        }
    }

    CookedAnimation animation;  // ONLY NOW is anything reserved.
    animation.channels.reserve(channelCount);

    // 11. the channel records. The channel table is provably inside the buffer by now:
    //     timesDataOffset == 80 + 32 * channelCount and totalBytes == bytes.size() >= timesDataOffset.
    for (std::uint32_t i = 0; i < channelCount; ++i) {
        const std::size_t o = COOKED_ANIMATION_HEADER_BYTES + (std::size_t{i} * COOKED_ANIMATION_CHANNEL_BYTES);
        if (getU64(bytes, o + C_RESERVED0) != 0) {
            return refuse(CookedAnimationStatus::ReservedNotZero,
                          std::format("channel record {} has a non-zero reserved field", i));
        }
        CookedAnimationChannel channel;
        channel.targetNodeLocalId = getU32(bytes, o + C_TARGET_NODE_LOCAL_ID);
        channel.path = static_cast<CookedAnimationPath>(getU16(bytes, o + C_PATH));
        channel.interpolation = static_cast<CookedAnimationInterpolation>(getU16(bytes, o + C_INTERPOLATION));
        channel.keyCount = getU32(bytes, o + C_KEY_COUNT);
        channel.firstKey = getU32(bytes, o + C_FIRST_KEY);
        channel.firstValue = getU32(bytes, o + C_FIRST_VALUE);
        channel.valueCount = getU32(bytes, o + C_VALUE_COUNT);

        if (!isCookedAnimationPath(channel.path)) {
            return refuse(CookedAnimationStatus::BadTable,
                          std::format("channel record {} names path code {}, which this format does not define", i,
                                      static_cast<std::uint16_t>(channel.path)));
        }
        if (!isCookedAnimationInterpolation(channel.interpolation)) {
            return refuse(
                CookedAnimationStatus::BadTable,
                std::format("channel record {} names interpolation code {}, which this format does not define", i,
                            static_cast<std::uint16_t>(channel.interpolation)));
        }
        if (channel.keyCount == 0) {
            return refuse(CookedAnimationStatus::BadTable, std::format("channel record {} declares zero keys", i));
        }
        const std::uint64_t expectedValues =
            static_cast<std::uint64_t>(channel.keyCount) * cookedAnimationValuesPerKey(channel.interpolation);
        if (channel.valueCount != expectedValues) {
            return refuse(
                CookedAnimationStatus::BadTable,
                std::format("channel record {} declares {} values over {} keys, but {} interpolation needs {}", i,
                            channel.valueCount, channel.keyCount,
                            cookedAnimationInterpolationLabel(channel.interpolation), expectedValues));
        }
        // A SUBTRACTION against the known-good count, never an addition that can wrap.
        if (channel.firstKey > keyCount || keyCount - channel.firstKey < channel.keyCount) {
            return refuse(
                CookedAnimationStatus::BadRange,
                std::format("channel record {} claims keys [{}, {}) of a {}-key times region", i, channel.firstKey,
                            static_cast<std::uint64_t>(channel.firstKey) + channel.keyCount, keyCount));
        }
        if (channel.firstValue > valueCount || valueCount - channel.firstValue < channel.valueCount) {
            return refuse(CookedAnimationStatus::BadRange,
                          std::format("channel record {} claims values [{}, {}) of a {}-value values region", i,
                                      channel.firstValue,
                                      static_cast<std::uint64_t>(channel.firstValue) + channel.valueCount, valueCount));
        }
        animation.channels.push_back(channel);
    }

    // 12. done. The header's keyCount is the REGION's size, not the channel sum: section 13.3 lets
    //     two channels share one key slice, so no cross-channel total is checked and none should be.
    animation.formatVersion = formatVersion;
    animation.cookerVersion = getU32(bytes, H_COOKER_VERSION);
    animation.sourceGuid = Guid{getU64(bytes, H_GUID_HI), getU64(bytes, H_GUID_LO)};
    animation.sourceAnimationIndex = getU32(bytes, H_SOURCE_ANIMATION_INDEX);
    animation.durationSeconds = getF32(bytes, H_DURATION_SECONDS);
    animation.keyCount = keyCount;
    animation.valueCount = valueCount;
    animation.timesDataOffset = timesDataOffset;
    animation.valuesDataOffset = valuesDataOffset;
    animation.bytes = bytes;
    return CookedAnimationParseResult{CookedAnimationStatus::Ok, std::string{}, std::move(animation)};
}

}  // namespace engine::assets

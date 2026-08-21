// Aero Engine — the animation cook (task 3.5.2): canonicalization and the byte emit. See
// animation_cook.hpp for the contract and docs/09-file-formats.md section 13 for the normative
// format. NEVER THROWS. NEVER READS OR WRITES A FILE. NEVER LOGS. ZERO floating-point arithmetic:
// every time and every value component reaches the buffer through putF32's std::bit_cast and
// nothing else -- the ONE exception is the durationSeconds fold, which is comparison-and-select.
#include <aero/assets/animation_cook.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace engine::assets {
namespace {

// The header's field offsets, named ONCE (docs/09 section 13.2). The parser spells its own copy;
// neither reads the other's, so a transposed offset is a red golden rather than a shared mistake.
constexpr std::size_t H_FORMAT_VERSION = 8;
constexpr std::size_t H_COOKER_VERSION = 12;
constexpr std::size_t H_GUID_HI = 16;
constexpr std::size_t H_GUID_LO = 24;
constexpr std::size_t H_CHANNEL_COUNT = 36;
constexpr std::size_t H_KEY_COUNT = 40;
constexpr std::size_t H_VALUE_COUNT = 44;
constexpr std::size_t H_SOURCE_ANIMATION_INDEX = 48;
constexpr std::size_t H_DURATION_SECONDS = 52;
constexpr std::size_t H_TIMES_DATA_OFFSET = 56;
constexpr std::size_t H_VALUES_DATA_OFFSET = 64;
constexpr std::size_t H_TOTAL_BYTES = 72;

// Channel-record field offsets (docs/09 section 13.3).
constexpr std::size_t C_TARGET_NODE_LOCAL_ID = 0;
constexpr std::size_t C_PATH = 4;
constexpr std::size_t C_INTERPOLATION = 6;
constexpr std::size_t C_KEY_COUNT = 8;
constexpr std::size_t C_FIRST_KEY = 12;
constexpr std::size_t C_FIRST_VALUE = 16;
constexpr std::size_t C_VALUE_COUNT = 20;

// The two bulk strides, in bytes per element. Named so no arithmetic below carries a bare 4 or 16.
constexpr std::size_t TIME_STRIDE = 4;
constexpr std::size_t VALUE_STRIDE = 16;

// How far a targetLocalId is shifted to make room for the path code in one sortable key. 16 bits is
// four orders of magnitude more than the three codes this format defines, and the key is a u64, so
// a full 32-bit localId still fits with room to spare.
constexpr unsigned int PATH_KEY_BITS = 16;

// One entry of the sorted (node, path) -> survivor-position vector. Named so the sort, the duplicate
// sweep and every emission loop spell the same type once.
using OrderEntry = std::pair<std::uint64_t, std::uint32_t>;

[[nodiscard]] AnimationCookResult refuse(std::string message) {
    AnimationCookResult out;
    out.status = AnimationCookStatus::Invalid;
    out.message = std::move(message);
    return out;
}

// The warning list is CAPPED and the total is not (the MAX_REPORTED_PER_CATEGORY shape). Every
// warning in this file goes through here so the cap cannot be forgotten at one site.
void addWarning(AnimationCookResult& out, std::string text) {
    ++out.warningTotal;
    if (out.warnings.size() < MAX_COOK_WARNINGS) {
        out.warnings.push_back(std::move(text));
    }
}

}  // namespace

AnimationCookResult cookAnimation(const AnimationCookInput& input) {
    // 1. the two whole-input refusals. An empty clip is not a degenerate animation, it is the
    //    absence of one, and the format cannot represent it (docs/09 section 13.0).
    const std::size_t channelCount = input.channels.size();
    if (channelCount == 0) {
        return refuse("the channel list is empty: a clip with no channels has no representation");
    }
    if (channelCount > MAX_COOKED_ANIMATION_CHANNELS) {
        return refuse(std::format("the channel list holds {} channels, over the cap of {}", channelCount,
                                  MAX_COOKED_ANIMATION_CHANNELS));
    }

    // 2. per-channel triage, in INPUT order and in this exact sequence, so a multiply-defective
    //    channel's outcome is stated rather than incidental: a bad CODE is a caller bug and outranks
    //    an empty key list, which is a legitimate shape the importer produces.
    AnimationCookResult out;
    std::vector<std::uint32_t> survivors;
    survivors.reserve(channelCount);
    for (std::size_t i = 0; i < channelCount; ++i) {
        const AnimationCookChannel& channel = input.channels[i];
        if (!isCookedAnimationPath(channel.path)) {
            return refuse(std::format("channel {} names path code {}, which this format does not define", i,
                                      static_cast<std::uint16_t>(channel.path)));
        }
        if (!isCookedAnimationInterpolation(channel.interpolation)) {
            return refuse(std::format("channel {} names interpolation code {}, which this format does not define", i,
                                      static_cast<std::uint16_t>(channel.interpolation)));
        }
        if (channel.times.empty()) {
            addWarning(out, std::format("channel {} (node {}, {}) carries no keys and was dropped", i,
                                        channel.targetLocalId, cookedAnimationPathLabel(channel.path)));
            continue;
        }
        const std::size_t expectedValues = channel.times.size() * cookedAnimationValuesPerKey(channel.interpolation);
        if (channel.values.size() != expectedValues) {
            return refuse(std::format("channel {} carries {} values over {} keys, but {} interpolation needs {}", i,
                                      channel.values.size(), channel.times.size(),
                                      cookedAnimationInterpolationLabel(channel.interpolation), expectedValues));
        }
        for (std::size_t k = 1; k < channel.times.size(); ++k) {
            if (!(channel.times[k] > channel.times[k - 1])) {
                return refuse(std::format("channel {}'s times are not strictly increasing at key {}", i, k));
            }
        }
        survivors.push_back(static_cast<std::uint32_t>(i));
    }

    // 3. nothing left is a REFUSAL, not an empty file.
    if (survivors.empty()) {
        return refuse("every channel was dropped: the clip carries no motion");
    }

    // 4. the normative order, as a SORTED VECTOR of packed keys -- never a hash container, so no
    //    output can depend on an iteration order. The key is total, so the same channels in any
    //    input permutation cook to identical bytes. Ties cannot survive: a duplicate
    //    (targetNodeLocalId, path) pair is a refusal, because glTF section 3.6.1 makes it a MUST NOT
    //    and the ambiguity is WHICH motion is the truth.
    std::vector<OrderEntry> order;
    order.reserve(survivors.size());
    for (const std::uint32_t position : survivors) {
        const AnimationCookChannel& channel = input.channels[position];
        const std::uint64_t key = (static_cast<std::uint64_t>(channel.targetLocalId) << PATH_KEY_BITS) |
                                  static_cast<std::uint64_t>(static_cast<std::uint16_t>(channel.path));
        order.emplace_back(key, position);
    }
    std::sort(order.begin(), order.end());
    for (std::size_t k = 1; k < order.size(); ++k) {
        if (order[k].first == order[k - 1].first) {
            const AnimationCookChannel& channel = input.channels[order[k].second];
            return refuse(std::format("two channels target node {} with path {}", channel.targetLocalId,
                                      cookedAnimationPathLabel(channel.path)));
        }
    }

    // 5. the totals, accumulated in EMISSION order, and their caps. The sort runs BEFORE the caps:
    //    the rule exists so a shuffled input can never produce a different file, and keeping the
    //    order makes that true by construction rather than by argument.
    std::uint64_t keyTotal = 0;
    std::uint64_t valueTotal = 0;
    for (const OrderEntry& entry : order) {
        const AnimationCookChannel& channel = input.channels[entry.second];
        keyTotal += channel.times.size();
        valueTotal += channel.values.size();
    }
    // The VALUE cap is tested FIRST, and the order is load-bearing rather than arbitrary: valueTotal
    // is at most 3 x keyTotal, so a key total inside its own cap can never carry a value total
    // outside its own. Testing keys first would make the value arm unreachable from any input, and
    // an unreachable cap is one nothing can prove is wired up at all.
    if (valueTotal > MAX_COOKED_ANIMATION_VALUES) {
        return refuse(
            std::format("the clip carries {} values, over the cap of {}", valueTotal, MAX_COOKED_ANIMATION_VALUES));
    }
    if (keyTotal > MAX_COOKED_ANIMATION_KEYS) {
        return refuse(std::format("the clip carries {} keys, over the cap of {}", keyTotal, MAX_COOKED_ANIMATION_KEYS));
    }

    // 6. THE duration fold, and the only floating-point operation in this file: std::max with the
    //    ACCUMULATOR FIRST, in EMISSION order, from 0.0f. Emission order rather than input order
    //    because the result is written into the header, and an input-order fold would make a
    //    shuffled input produce different header bytes -- the identical trap the mesh cook's model
    //    box already records. From 0.0f rather than -inf so a negative-times clip yields duration 0
    //    rather than a negative duration the clock would have to defend against. Bit-for-bit what
    //    gltf_import.cpp computes for ImportedAnimation::duration.
    float durationSeconds = 0.0F;
    for (const OrderEntry& entry : order) {
        durationSeconds = std::max(durationSeconds, input.channels[entry.second].times.back());
    }

    // 7. the buffer, VALUE-INITIALIZED by construction -- so reservedFlags, every channel's
    //    reserved0 and the single padding site are zero without one explicit store.
    const auto emittedChannels = static_cast<std::uint32_t>(order.size());
    const auto keyCount = static_cast<std::uint32_t>(keyTotal);
    const auto valueCount = static_cast<std::uint32_t>(valueTotal);
    const std::size_t timesDataOffset =
        COOKED_ANIMATION_HEADER_BYTES + (std::size_t{emittedChannels} * COOKED_ANIMATION_CHANNEL_BYTES);
    const std::size_t valuesDataOffset =
        timesDataOffset + (TIME_STRIDE * keyCount) + cookedAnimationTimesPadding(keyCount);
    const std::size_t totalBytes = valuesDataOffset + (VALUE_STRIDE * std::size_t{valueCount});

    std::vector<std::byte> bytes(totalBytes);
    const std::span<std::byte> buffer(bytes);
    for (std::size_t i = 0; i < COOKED_ANIMATION_MAGIC.size(); ++i) {
        buffer[i] = static_cast<std::byte>(COOKED_ANIMATION_MAGIC[i]);
    }
    putU32(buffer, H_FORMAT_VERSION, COOKED_ANIMATION_FORMAT_VERSION);
    putU32(buffer, H_COOKER_VERSION, COOKED_ANIMATION_COOKER_VERSION);
    putU64(buffer, H_GUID_HI, input.sourceGuid.hi);
    putU64(buffer, H_GUID_LO, input.sourceGuid.lo);
    putU32(buffer, H_CHANNEL_COUNT, emittedChannels);
    putU32(buffer, H_KEY_COUNT, keyCount);
    putU32(buffer, H_VALUE_COUNT, valueCount);
    putU32(buffer, H_SOURCE_ANIMATION_INDEX, input.sourceAnimationIndex);
    putF32(buffer, H_DURATION_SECONDS, durationSeconds);
    putU64(buffer, H_TIMES_DATA_OFFSET, timesDataOffset);
    putU64(buffer, H_VALUES_DATA_OFFSET, valuesDataOffset);
    putU64(buffer, H_TOTAL_BYTES, totalBytes);

    // 8. the records and both regions, in one pass over the emitted order, with the two running
    //    sums carried as the write cursors they are.
    std::uint32_t firstKey = 0;
    std::uint32_t firstValue = 0;
    for (std::size_t e = 0; e < order.size(); ++e) {
        const AnimationCookChannel& channel = input.channels[order[e].second];
        const auto channelKeys = static_cast<std::uint32_t>(channel.times.size());
        const auto channelValues = static_cast<std::uint32_t>(channel.values.size());
        const std::size_t o = COOKED_ANIMATION_HEADER_BYTES + (e * COOKED_ANIMATION_CHANNEL_BYTES);
        putU32(buffer, o + C_TARGET_NODE_LOCAL_ID, channel.targetLocalId);
        putU16(buffer, o + C_PATH, static_cast<std::uint16_t>(channel.path));
        putU16(buffer, o + C_INTERPOLATION, static_cast<std::uint16_t>(channel.interpolation));
        putU32(buffer, o + C_KEY_COUNT, channelKeys);
        putU32(buffer, o + C_FIRST_KEY, firstKey);
        putU32(buffer, o + C_FIRST_VALUE, firstValue);
        putU32(buffer, o + C_VALUE_COUNT, channelValues);

        for (std::size_t k = 0; k < channel.times.size(); ++k) {
            putF32(buffer, timesDataOffset + (TIME_STRIDE * (std::size_t{firstKey} + k)), channel.times[k]);
        }
        // w is a STORED CONSTANT for Translation and Scale, whatever the caller passed, and verbatim
        // for Rotation: the bytes are a function of the motion rather than of the caller's scratch.
        // It is a selection, not arithmetic.
        const bool carriesW = channel.path == CookedAnimationPath::Rotation;
        for (std::size_t v = 0; v < channel.values.size(); ++v) {
            const std::size_t at = valuesDataOffset + (VALUE_STRIDE * (std::size_t{firstValue} + v));
            const Vec4& value = channel.values[v];
            putF32(buffer, at + 0, value.x);
            putF32(buffer, at + 4, value.y);
            putF32(buffer, at + 8, value.z);
            putF32(buffer, at + 12, carriesW ? value.w : 0.0F);
        }
        firstKey += channelKeys;
        firstValue += channelValues;
    }

    // 9. the status. Truncated iff at least one channel was dropped and a valid file was still
    //    produced; message non-empty iff not Ok; bytes empty iff Invalid.
    const std::size_t droppedCount = channelCount - survivors.size();
    if (droppedCount > 0) {
        out.status = AnimationCookStatus::Truncated;
        out.message = std::format("{} of {} channels carried no keys and were dropped", droppedCount, channelCount);
    } else {
        out.status = AnimationCookStatus::Ok;
    }
    out.bytes = std::move(bytes);
    return out;
}

}  // namespace engine::assets

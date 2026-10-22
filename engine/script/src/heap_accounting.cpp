// Aero Engine -- the JavaScript heap's allocator (task 4.1.1). See the header for the contract.

#include <aero/script/internal/heap_accounting.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace engine::script::internal {
namespace {

// Word 0 of a live block's header is its size as a full 64-bit word, whatever the width of size_t.
[[nodiscard]] std::size_t readHeader(const void* block) noexcept {
    std::uint64_t word = 0;
    std::memcpy(&word, static_cast<const std::byte*>(block) - HEAP_HEADER_BYTES, sizeof word);
    return static_cast<std::size_t>(word);
}

void writeHeader(void* base, std::size_t size) noexcept {
    const std::uint64_t word = size;
    std::memcpy(base, &word, sizeof word);
}

// A held block's header (16 bytes, ours alone: the backend never writes before the payload) is rewritten as
//   word 0: HELD_BIT | (bytes captured, 0-7) << 56 | the payload's first bytes at release, packed low byte first
//   word 1: the next held block's base, a whole pointer
// A live block's word 0 is its size, a full 64-bit word (writeHeader), always below 2^56, so HELD_BIT tells a held
// block from a live one, and a second release of the same block is recognised by its header. No bit of any pointer is
// borrowed: Android's allocator tags the top byte of the pointers it returns. The hold needs no memory of its own.
constexpr std::uint64_t HELD_BIT = std::uint64_t{1} << 63U;
constexpr unsigned CAPTURED_SHIFT = 56U;
constexpr std::uint64_t CAPTURED_FIELD = std::uint64_t{0x7FU} << CAPTURED_SHIFT;
constexpr std::uint64_t PREFIX_MASK = (std::uint64_t{1} << CAPTURED_SHIFT) - 1U;
static_assert(HELD_SNAPSHOT_BYTES * 8U <= CAPTURED_SHIFT);
static_assert(HEAP_HEADER_BYTES >= sizeof(std::uint64_t) + sizeof(void*));

[[nodiscard]] std::uint64_t headerWord(const void* base) noexcept {
    std::uint64_t word = 0;
    std::memcpy(&word, base, sizeof word);
    return word;
}

void setHeaderWord(void* base, std::uint64_t word) noexcept { std::memcpy(base, &word, sizeof word); }

// Word 1 travels as bytes (std::bit_cast), so no pointer-to-pointer conversion is ever made.
using PointerBytes = std::array<std::byte, sizeof(void*)>;

[[nodiscard]] void* nextHeld(const void* base) noexcept {
    PointerBytes bytes{};
    std::memcpy(bytes.data(), static_cast<const std::byte*>(base) + sizeof(std::uint64_t), bytes.size());
    return std::bit_cast<void*>(bytes);
}

void setNextHeld(void* base, void* next) noexcept {
    const auto bytes = std::bit_cast<PointerBytes>(next);
    std::memcpy(static_cast<std::byte*>(base) + sizeof(std::uint64_t), bytes.data(), bytes.size());
}

[[nodiscard]] std::uint64_t packedPrefix(const void* base, std::size_t captured) noexcept {
    const auto* payload = static_cast<const unsigned char*>(base) + HEAP_HEADER_BYTES;
    std::uint64_t packed = 0;
    for (std::size_t i = 0; i < captured; ++i) {
        packed |= std::uint64_t{payload[i]} << (8U * i);
    }
    return packed;
}

}  // namespace

HeapAccounting::HeapAccounting(std::size_t limitBytes) noexcept : limit(limitBytes) {}

void HeapAccounting::openGraceWindow(std::size_t allocations, std::size_t bytes) noexcept {
    graceAllocations = allocations;
    graceCeiling = bytes > std::numeric_limits<std::size_t>::max() - inUse ? std::numeric_limits<std::size_t>::max()
                                                                           : inUse + bytes;
}

void HeapAccounting::closeGraceWindow() noexcept {
    graceAllocations = 0;
    graceCeiling = 0;
}

std::size_t HeapAccounting::admissionRoom() noexcept {
    std::size_t ceiling = limit;
    if (graceAllocations != 0) {
        ceiling = std::max(limit, graceCeiling);
        --graceAllocations;
    }
    return ceiling > inUse ? ceiling - inUse : 0;  // inUse may be past the limit: only a window admitted that
}

void* HeapAccounting::allocate(std::size_t size) noexcept {
    const std::size_t room = admissionRoom();
    if (size > room || room - size < HEAP_HEADER_BYTES) {
        ++refused;
        ++refusedGrowths;
        return nullptr;
    }
    void* base = std::malloc(size + HEAP_HEADER_BYTES);  // cannot wrap: size + header <= the ceiling
    if (base == nullptr) {
        return nullptr;  // the system failed, not the limit: not counted as a refusal
    }
    writeHeader(base, size);
    inUse += size + HEAP_HEADER_BYTES;
    peak = std::max(peak, inUse);
    ++blocks;
    return static_cast<std::byte*>(base) + HEAP_HEADER_BYTES;
}

void* HeapAccounting::allocateZeroed(std::size_t count, std::size_t size) noexcept {
    if (count != 0 && size > std::numeric_limits<std::size_t>::max() / count) {
        ++refused;  // count * size would wrap: no allocation of that size can be admitted
        return nullptr;
    }
    const std::size_t total = count * size;
    const std::size_t room = admissionRoom();
    if (total > room || room - total < HEAP_HEADER_BYTES) {
        ++refused;
        return nullptr;
    }
    void* base = std::calloc(1, total + HEAP_HEADER_BYTES);
    if (base == nullptr) {
        return nullptr;
    }
    writeHeader(base, total);  // the header sits over the zeroed prefix
    inUse += total + HEAP_HEADER_BYTES;
    peak = std::max(peak, inUse);
    ++blocks;
    return static_cast<std::byte*>(base) + HEAP_HEADER_BYTES;
}

void* HeapAccounting::reallocate(void* block, std::size_t size) noexcept {
    assert(block != nullptr && size != 0);  // never sent by the backend (quickjs.c:1681-1689)
    if (block == nullptr) {
        return allocate(size);
    }
    if (size == 0) {
        release(block);
        return nullptr;
    }
    if (holding && (headerWord(static_cast<std::byte*>(block) - HEAP_HEADER_BYTES) & HELD_BIT) != 0) {
        ++repeated;  // a held block reallocated: refused, as a failed C realloc, and the block stays held
        return nullptr;
    }
    const std::size_t old = readHeader(block);
    if (size > old && size - old > admissionRoom()) {
        ++refused;
        ++refusedGrowths;
        return nullptr;  // C realloc semantics: `block` stays valid and charged (VM37)
    }
    void* base = static_cast<std::byte*>(block) - HEAP_HEADER_BYTES;
    void* moved = std::realloc(base, size + HEAP_HEADER_BYTES);
    if (moved == nullptr) {
        return nullptr;  // ditto, for a system failure
    }
    writeHeader(moved, size);
    inUse = inUse - old + size;
    peak = std::max(peak, inUse);
    return static_cast<std::byte*>(moved) + HEAP_HEADER_BYTES;
}

void HeapAccounting::release(void* block) noexcept {
    if (block == nullptr) {
        return;
    }
    void* base = static_cast<std::byte*>(block) - HEAP_HEADER_BYTES;
    if (holding && (headerWord(base) & HELD_BIT) != 0) {
        ++repeated;  // already held: its memory is still ours, and returnHeld() frees it exactly once
        return;
    }
    const std::size_t size = readHeader(block);
    inUse -= size + HEAP_HEADER_BYTES;
    --blocks;
    if (holding) {
        assert(std::uint64_t{size} <= PREFIX_MASK);  // a live block's size is far below 2^56
        const std::size_t captured = std::min(size, HELD_SNAPSHOT_BYTES);
        setHeaderWord(base, HELD_BIT | (std::uint64_t{captured} << CAPTURED_SHIFT) | packedPrefix(base, captured));
        setNextHeld(base, heldHead);
        heldHead = base;
        ++heldCount;
        return;
    }
    std::free(base);
}

void HeapAccounting::holdReleases() noexcept {
    holding = true;
    repeated = 0;
}

HeldReturn HeapAccounting::returnHeld() noexcept {
    HeldReturn report{heldCount, 0, repeated};
    void* base = heldHead;
    while (base != nullptr) {
        const std::uint64_t word = headerWord(base);
        void* const next = nextHeld(base);
        const auto captured = static_cast<std::size_t>((word & CAPTURED_FIELD) >> CAPTURED_SHIFT);
        if (packedPrefix(base, captured) != (word & PREFIX_MASK)) {
            ++report.writtenAfterRelease;
        }
        std::free(base);
        base = next;
    }
    heldHead = nullptr;
    heldCount = 0;
    holding = false;
    repeated = 0;
    return report;
}

std::size_t HeapAccounting::requestedSize(const void* block) noexcept {
    if (block == nullptr) {
        return 0;
    }
    const std::uint64_t word = headerWord(static_cast<const std::byte*>(block) - HEAP_HEADER_BYTES);
    return (word & HELD_BIT) != 0 ? 0 : static_cast<std::size_t>(word);  // a held block reads as nothing
}

std::size_t HeapAccounting::limitBytes() const noexcept { return limit; }

std::size_t HeapAccounting::bytesInUse() const noexcept { return inUse; }

std::size_t HeapAccounting::peakBytes() const noexcept { return peak; }

std::size_t HeapAccounting::liveBlocks() const noexcept { return blocks; }

std::uint64_t HeapAccounting::refusals() const noexcept { return refused; }

std::uint64_t HeapAccounting::growthRefusals() const noexcept { return refusedGrowths; }

}  // namespace engine::script::internal

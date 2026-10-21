// Aero Engine -- the JavaScript heap's allocator (task 4.1.1). See the header for the contract.

#include <aero/script/internal/heap_accounting.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace engine::script::internal {
namespace {

[[nodiscard]] std::size_t readHeader(const void* block) noexcept {
    std::size_t size = 0;
    std::memcpy(&size, static_cast<const std::byte*>(block) - HEAP_HEADER_BYTES, sizeof size);
    return size;
}

}  // namespace

HeapAccounting::HeapAccounting(std::size_t limitBytes) noexcept : limit(limitBytes) {}

void* HeapAccounting::allocate(std::size_t size) noexcept {
    const std::size_t room = limit - inUse;  // never wraps: inUse <= limit is the class invariant
    if (size > room || room - size < HEAP_HEADER_BYTES) {
        ++refused;
        return nullptr;
    }
    void* base = std::malloc(size + HEAP_HEADER_BYTES);  // cannot wrap: size + header <= limit
    if (base == nullptr) {
        return nullptr;  // the system failed, not the limit: not counted as a refusal
    }
    std::memcpy(base, &size, sizeof size);
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
    const std::size_t room = limit - inUse;
    if (total > room || room - total < HEAP_HEADER_BYTES) {
        ++refused;
        return nullptr;
    }
    void* base = std::calloc(1, total + HEAP_HEADER_BYTES);
    if (base == nullptr) {
        return nullptr;
    }
    std::memcpy(base, &total, sizeof total);  // the header sits over the zeroed prefix
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
    const std::size_t old = readHeader(block);
    if (size > old && size - old > limit - inUse) {
        ++refused;
        return nullptr;  // C realloc semantics: `block` stays valid and charged (VM37)
    }
    void* base = static_cast<std::byte*>(block) - HEAP_HEADER_BYTES;
    void* moved = std::realloc(base, size + HEAP_HEADER_BYTES);
    if (moved == nullptr) {
        return nullptr;  // ditto, for a system failure
    }
    std::memcpy(moved, &size, sizeof size);
    inUse = inUse - old + size;
    peak = std::max(peak, inUse);
    return static_cast<std::byte*>(moved) + HEAP_HEADER_BYTES;
}

void HeapAccounting::release(void* block) noexcept {
    if (block == nullptr) {
        return;
    }
    const std::size_t size = readHeader(block);
    inUse -= size + HEAP_HEADER_BYTES;
    --blocks;
    std::free(static_cast<std::byte*>(block) - HEAP_HEADER_BYTES);
}

std::size_t HeapAccounting::requestedSize(const void* block) noexcept {
    return block == nullptr ? 0 : readHeader(block);
}

std::size_t HeapAccounting::limitBytes() const noexcept { return limit; }

std::size_t HeapAccounting::bytesInUse() const noexcept { return inUse; }

std::size_t HeapAccounting::peakBytes() const noexcept { return peak; }

std::size_t HeapAccounting::liveBlocks() const noexcept { return blocks; }

std::uint64_t HeapAccounting::refusals() const noexcept { return refused; }

}  // namespace engine::script::internal

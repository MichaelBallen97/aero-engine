#pragma once
// Aero Engine -- the JavaScript heap's allocator (task 4.1.1; internal to engine/script). Backend-free: it
// knows nothing about quickjs, so its contract is tested directly (tests/script_vm_test.cpp, VM36-VM38)
// through aero::script_internal, and the five trampolines in src/vm.cpp are one line each.
//
// It enforces the heap limit with platform-independent accounting (requested bytes plus a fixed header,
// never a platform's malloc_usable_size rounding), counts the requests the LIMIT refused -- the witness
// that tells an out-of-memory failure apart from a script's own `throw null` -- and makes "no allocation
// survived teardown" an exact count in every build configuration.
//
// Single-threaded by contract: a runtime is used by one thread at a time, and so is its allocator.

#include <cstddef>
#include <cstdint>

namespace engine::script::internal {

// Every block carries a 16-byte header holding the size requested for it: 16 keeps the payload at the
// alignment malloc returned on every 64-bit target and is >= alignof(std::max_align_t) everywhere, and it
// is what makes requestedSize() a function of the pointer alone -- the backend's usable-size hook takes no
// context argument.
inline constexpr std::size_t HEAP_HEADER_BYTES = 16;
static_assert(HEAP_HEADER_BYTES >= alignof(std::max_align_t));
static_assert(HEAP_HEADER_BYTES >= sizeof(std::size_t));

class HeapAccounting {
public:
    explicit HeapAccounting(std::size_t limitBytes) noexcept;
    HeapAccounting(const HeapAccounting&) = delete;  // pinned: the runtime holds its address
    HeapAccounting& operator=(const HeapAccounting&) = delete;
    HeapAccounting(HeapAccounting&&) = delete;
    HeapAccounting& operator=(HeapAccounting&&) = delete;
    ~HeapAccounting() = default;

    // nullptr from allocate/allocateZeroed: refused by the limit (counted) or failed in the system (not counted).
    [[nodiscard]] void* allocate(std::size_t size) noexcept;
    [[nodiscard]] void* allocateZeroed(std::size_t count, std::size_t size) noexcept;
    // nullptr from reallocate: `block` is untouched and still charged, as with C realloc.
    [[nodiscard]] void* reallocate(void* block, std::size_t size) noexcept;
    void release(void* block) noexcept;                                          // nullptr: no-op
    [[nodiscard]] static std::size_t requestedSize(const void* block) noexcept;  // nullptr: 0

    // A grace window: for the next `allocations` admission decisions (an allocate, an allocateZeroed or a GROWING
    // reallocate, refused or not) the ceiling is max(limit, inUse-at-the-opening + bytes) instead of the limit. It
    // exists for one caller: the time budget's interrupt handler, which opens it each time it returns 1, because
    // the next requests are the interrupt error's own and they must not be refused by a heap the script has just
    // filled. The ceiling is measured from the usage at the opening, so windows opened one after another stack.
    // closeGraceWindow() ends it early; a window also ends when its decisions are spent.
    void openGraceWindow(std::size_t allocations, std::size_t bytes) noexcept;
    void closeGraceWindow() noexcept;

    [[nodiscard]] std::size_t limitBytes() const noexcept;
    // Sum of (size + HEAP_HEADER_BYTES) over live blocks. It may exceed limitBytes(), by bytes a grace window admitted.
    [[nodiscard]] std::size_t bytesInUse() const noexcept;
    [[nodiscard]] std::size_t peakBytes() const noexcept;
    [[nodiscard]] std::size_t liveBlocks() const noexcept;
    [[nodiscard]] std::uint64_t refusals() const noexcept;  // requests the LIMIT refused (not system failures)

private:
    // The room the next admission decision has, and the decision itself: inside a grace window it spends one of the
    // window's decisions. Zero when inUse is already past the ceiling.
    [[nodiscard]] std::size_t admissionRoom() noexcept;

    std::size_t limit;
    std::size_t inUse = 0;  // invariant: inUse exceeds limit only by bytes a grace window admitted
    std::size_t peak = 0;
    std::size_t blocks = 0;
    std::uint64_t refused = 0;
    std::size_t graceAllocations = 0;  // admission decisions the open window has left
    std::size_t graceCeiling = 0;      // inUse at the opening + the window's bytes (saturating)
};

}  // namespace engine::script::internal

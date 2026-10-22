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

// The teardown hold (task 4.1.2, R23 in docs/08-risks.md). quickjs-ng frees every module of a context when the context
// is freed (quickjs.c:2714), while objects that outlive it -- the reaction functions of a top-level await still pending
// -- hold module values, so the collector's later release of those objects writes into a freed module (quickjs.c:6803,
// reached from :6208). From holdReleases() until returnHeld(), release() accounts a block as returned but KEEPS its
// memory, with its first payload bytes recorded in its own header: such a write lands in memory the VM still owns, and
// is counted. returnHeld() frees every held block exactly once and reports.
inline constexpr std::size_t HELD_SNAPSHOT_BYTES = 7;  // payload bytes recorded per held block, in its own header

struct HeldReturn {
    std::size_t blocks = 0;               // releases the hold accepted
    std::size_t writtenAfterRelease = 0;  // held blocks whose recorded bytes changed after their release
    std::size_t repeatedReleases = 0;     // releases of a block already held: absorbed, never freed twice
};

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
    void release(void* block) noexcept;  // nullptr: no-op; during a hold, kept and recorded (above)
    [[nodiscard]] static std::size_t requestedSize(const void* block) noexcept;  // nullptr: 0

    // A grace window: for the next `allocations` admission decisions (an allocate, an allocateZeroed or a GROWING
    // reallocate, refused or not) the ceiling is max(limit, inUse-at-the-opening + bytes) instead of the limit. It
    // exists for one caller: the time budget's interrupt handler, which opens it each time it returns 1, because
    // the next requests are the interrupt error's own and they must not be refused by a heap the script has just
    // filled. The ceiling is measured from the usage at the opening, so windows opened one after another stack.
    // closeGraceWindow() ends it early; a window also ends when its decisions are spent.
    void openGraceWindow(std::size_t allocations, std::size_t bytes) noexcept;
    void closeGraceWindow() noexcept;

    // The hold (above). holdReleases() starts it; returnHeld() frees every held block, reports, and ends it. Outside a
    // hold, release() frees at once. Nothing but Vm::Impl::shutdown starts one.
    void holdReleases() noexcept;
    [[nodiscard]] HeldReturn returnHeld() noexcept;

    [[nodiscard]] std::size_t limitBytes() const noexcept;
    // Sum of (size + HEAP_HEADER_BYTES) over live blocks. It may exceed limitBytes(), by bytes a grace window admitted.
    [[nodiscard]] std::size_t bytesInUse() const noexcept;
    [[nodiscard]] std::size_t peakBytes() const noexcept;
    [[nodiscard]] std::size_t liveBlocks() const noexcept;
    [[nodiscard]] std::uint64_t refusals() const noexcept;  // requests the LIMIT refused (not system failures)
    // The refusals of allocate and of a growing reallocate -- every one but allocateZeroed's. The backend's two
    // allocations whose refusal it tolerates are zeroed ones (its atom and shape hash growth), and a buffer that grows
    // never is (vm_errors.cpp's classify reads the difference).
    [[nodiscard]] std::uint64_t growthRefusals() const noexcept;

private:
    // The room the next admission decision has, and the decision itself: inside a grace window it spends one of the
    // window's decisions. Zero when inUse is already past the ceiling.
    [[nodiscard]] std::size_t admissionRoom() noexcept;

    std::size_t limit;
    std::size_t inUse = 0;  // invariant: inUse exceeds limit only by bytes a grace window admitted
    std::size_t peak = 0;
    std::size_t blocks = 0;
    std::uint64_t refused = 0;
    std::uint64_t refusedGrowths = 0;
    std::size_t graceAllocations = 0;  // admission decisions the open window has left
    std::size_t graceCeiling = 0;      // inUse at the opening + the window's bytes (saturating)
    bool holding = false;
    void* heldHead = nullptr;  // the held blocks, linked through their own headers
    std::size_t heldCount = 0;
    std::size_t repeated = 0;
};

}  // namespace engine::script::internal

#pragma once
// Aero Engine — SlotMap<T, Tag>: the generational slot pool behind Handle<Tag> (task 0.2.1).
// Stores T in a dense, reusable slot array (contiguous storage — docs/03) and hands out Handle<Tag>
// keys. Freeing a slot bumps its generation, so every handle taken before the free is rejected
// afterward: get() returns nullptr instead of dereferencing a stale slot (ADR-001 — use-after-free
// eliminated at the root). Tag defaults to T (SlotMap<Foo> mints Handle<Foo>); pass a distinct Tag
// when the public handle type must differ from the private stored type (e.g. an RHI Handle<BufferTag>
// over a backend struct — the boundary rule).
//
// Lifetime/validity contract:
//   * Handles are stable: an element's handle stays usable regardless of other inserts/removes.
//   * The T* returned by get() is NOT stable: an insert() that reallocates the vector invalidates it
//     (standard std::vector semantics). Hold handles across inserts, not raw pointers; re-get().
//   * T must be move-constructible and move-assignable (default-construction is not required).
//   * NOT thread-safe: synchronize externally.

#include <aero/core/handle.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace engine {

template <typename T, typename Tag = T>
class SlotMap {
public:
    using HandleType = Handle<Tag>;

    // Store a value; returns a handle valid until that value is removed. May allocate (grows the slot
    // array) — not noexcept. Takes T by value: a copy binds to lvalues, a move to rvalues.
    HandleType insert(T value) {
        if (freeHead != NO_FREE_SLOT) {
            const std::uint32_t i = freeHead;
            Slot& slot = slots[i];
            freeHead = slot.nextFree;
            slot.value = std::move(value);  // engages the optional; generation carried from last free
            ++liveCount;
            return HandleType{i, slot.generation};
        }
        assert(slots.size() < NO_FREE_SLOT && "SlotMap: u32 index space exhausted");
        const auto i = static_cast<std::uint32_t>(slots.size());
        Slot slot;
        slot.value = std::move(value);
        slot.generation = 1;  // first occupancy of a fresh slot; 0 stays reserved for null
        slots.push_back(std::move(slot));
        ++liveCount;
        return HandleType{i, 1};
    }

    // Live-value access: a pointer to the stored T, or nullptr if the handle is null, out of range, or
    // stale (its slot was freed/reused). This is the use-after-free guard.
    [[nodiscard]] T* get(HandleType handle) noexcept {
        Slot* slot = resolve(handle);
        return (slot != nullptr && slot->value.has_value()) ? &*slot->value : nullptr;
    }
    [[nodiscard]] const T* get(HandleType handle) const noexcept {
        const Slot* slot = resolve(handle);
        return (slot != nullptr && slot->value.has_value()) ? &*slot->value : nullptr;
    }

    // "Is this handle currently live in this pool?"
    [[nodiscard]] bool contains(HandleType handle) const noexcept { return resolve(handle) != nullptr; }

    // Free the handle's slot; its generation is bumped so this and every other outstanding handle to
    // the slot becomes stale. Returns false (no-op) if the handle was already null/stale/foreign —
    // double-remove is safe.
    bool remove(HandleType handle) noexcept {
        Slot* slot = resolve(handle);
        if (slot == nullptr) {
            return false;
        }
        slot->value.reset();  // destroy T now, releasing any resource it owns (RAII)
        slot->generation = bumpGeneration(slot->generation);
        slot->nextFree = freeHead;
        freeHead = handle.index;
        --liveCount;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return liveCount; }
    [[nodiscard]] bool empty() const noexcept { return liveCount == 0; }

    void reserve(std::size_t capacity) { slots.reserve(capacity); }

    // Remove every element, retaining capacity. Generations of previously-live slots are bumped, so a
    // handle obtained before clear() stays rejected even after its slot is reused.
    void clear() noexcept {
        const auto count = static_cast<std::uint32_t>(slots.size());
        for (std::uint32_t i = 0; i < count; ++i) {
            Slot& slot = slots[i];
            if (slot.value.has_value()) {
                slot.value.reset();
                slot.generation = bumpGeneration(slot.generation);
            }
            slot.nextFree = (i + 1 < count) ? (i + 1) : NO_FREE_SLOT;
        }
        freeHead = (count == 0) ? NO_FREE_SLOT : 0;
        liveCount = 0;
    }

private:
    struct Slot {
        std::optional<T> value;        // engaged iff the slot is occupied
        std::uint32_t generation = 0;  // live => >= 1; bumped on free; 0 never labels a live slot
        std::uint32_t nextFree = 0;    // intrusive free-list link, meaningful only when free
    };

    // Reserved value: "empty free list", and the one size a slot count can never reach (insert asserts
    // slots.size() < NO_FREE_SLOT), so it can never be a real index.
    static constexpr std::uint32_t NO_FREE_SLOT = std::numeric_limits<std::uint32_t>::max();

    // Bump a generation, skipping 0 on u32 wrap so 0 stays the null sentinel and a reused slot never
    // mints a null-looking handle.
    static constexpr std::uint32_t bumpGeneration(std::uint32_t generation) noexcept {
        return generation == std::numeric_limits<std::uint32_t>::max() ? 1U : generation + 1U;
    }

    // Shared validation for get()/contains()/remove(): a handle maps to its live slot, or nullptr.
    // Postcondition: a non-null return guarantees the slot is occupied (its optional is engaged), so
    // get() dereferences it via &*slot->value behind an inline has_value() guard — the access stays
    // checked for clang-tidy.
    Slot* resolve(HandleType handle) noexcept {
        if (handle.index >= slots.size()) {
            return nullptr;
        }
        Slot& slot = slots[handle.index];
        if (slot.generation != handle.generation || !slot.value.has_value()) {
            return nullptr;
        }
        return &slot;
    }
    const Slot* resolve(HandleType handle) const noexcept {
        if (handle.index >= slots.size()) {
            return nullptr;
        }
        const Slot& slot = slots[handle.index];
        if (slot.generation != handle.generation || !slot.value.has_value()) {
            return nullptr;
        }
        return &slot;
    }

    std::vector<Slot> slots;
    std::uint32_t freeHead = NO_FREE_SLOT;
    std::size_t liveCount = 0;
};

}  // namespace engine

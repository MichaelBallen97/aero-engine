// Exercises engine::SlotMap<T, Tag> (task 0.2.1): the generational slot pool behind Handle<Tag>.
// The headline case is stale-handle rejection (ADR-001, use-after-free eliminated at the root);
// the remaining cases cover occupancy/forgery, move-only T, safe clear(), and bulk consistency.
#include <aero/core/slot_map.hpp>

#include <doctest/doctest.h>

#include <memory>
#include <type_traits>
#include <vector>

namespace {
struct TagA;
struct TagB;

const int* getConst(const engine::SlotMap<int>& map, engine::SlotMap<int>::HandleType handle) {
    return map.get(handle);
}
}  // namespace

TEST_CASE("SlotMap: insert then get returns the stored value") {
    engine::SlotMap<int> map;
    const auto h = map.insert(42);
    REQUIRE(map.get(h) != nullptr);
    CHECK(*map.get(h) == 42);
    CHECK(map.size() == 1);
    CHECK_FALSE(map.empty());
}

TEST_CASE("SlotMap: get on a freed handle returns nullptr") {
    engine::SlotMap<int> map;
    const auto h = map.insert(7);
    CHECK(map.remove(h));
    CHECK(map.get(h) == nullptr);
    CHECK_FALSE(map.contains(h));
    CHECK(map.size() == 0);
}

TEST_CASE("SlotMap: a reused slot reuses the index and bumps the generation") {
    engine::SlotMap<int> map;
    const auto h1 = map.insert(1);
    map.remove(h1);
    const auto h2 = map.insert(2);
    CHECK(h2.index == h1.index);
    CHECK(h2.generation == h1.generation + 1);
    CHECK(map.get(h1) == nullptr);
    REQUIRE(map.get(h2) != nullptr);
    CHECK(*map.get(h2) == 2);
}

TEST_CASE("SlotMap: remove is idempotent and rejects stale handles") {
    engine::SlotMap<int> map;
    const auto h = map.insert(1);
    CHECK(map.remove(h));
    CHECK_FALSE(map.remove(h));
    CHECK(map.size() == 0);
}

TEST_CASE("SlotMap: get and remove reject foreign and out-of-range handles") {
    engine::SlotMap<int> map;
    const engine::SlotMap<int>::HandleType outOfRange{999, 1};
    CHECK(map.get(outOfRange) == nullptr);
    CHECK_FALSE(map.remove(outOfRange));

    const auto h1 = map.insert(1);
    const std::uint32_t i = h1.index;
    map.remove(h1);  // slot i is now free, generation bumped to 2

    // Occupancy guard: a forged handle whose generation matches the free slot's CURRENT
    // generation must still be rejected, because the slot is unoccupied (D5, edge case #4).
    const engine::SlotMap<int>::HandleType forged{i, h1.generation + 1};
    CHECK(map.get(forged) == nullptr);
    CHECK_FALSE(map.contains(forged));
}

TEST_CASE("SlotMap: the null handle is never live") {
    engine::SlotMap<int> map;
    const engine::SlotMap<int>::HandleType nullHandle{};
    CHECK(map.get(nullHandle) == nullptr);
    CHECK_FALSE(map.contains(nullHandle));
    CHECK_FALSE(map.remove(nullHandle));
}

TEST_CASE("SlotMap: size and empty track live elements") {
    engine::SlotMap<int> map;
    const auto h1 = map.insert(1);
    const auto h2 = map.insert(2);
    const auto h3 = map.insert(3);
    CHECK(map.size() == 3);
    map.remove(h2);
    CHECK(map.size() == 2);
    CHECK_FALSE(map.empty());
    map.remove(h1);
    map.remove(h3);
    CHECK(map.size() == 0);
    CHECK(map.empty());
}

TEST_CASE("SlotMap: clear invalidates all prior handles, even across reuse") {
    engine::SlotMap<int> map;
    const auto h1 = map.insert(1);
    const auto h2 = map.insert(2);
    const auto h3 = map.insert(3);

    map.clear();
    CHECK(map.empty());
    CHECK(map.get(h1) == nullptr);
    CHECK(map.get(h2) == nullptr);
    CHECK(map.get(h3) == nullptr);

    const auto h4 = map.insert(4);
    REQUIRE(map.get(h4) != nullptr);
    CHECK(*map.get(h4) == 4);
    // Old handles remain rejected even though their slots have been reused.
    CHECK(map.get(h1) == nullptr);
    CHECK(map.get(h2) == nullptr);
    CHECK(map.get(h3) == nullptr);
}

TEST_CASE("SlotMap: const access yields a const pointer") {
    engine::SlotMap<int> map;
    const auto h = map.insert(99);
    const int* value = getConst(map, h);
    REQUIRE(value != nullptr);
    CHECK(*value == 99);
}

TEST_CASE("SlotMap: stores move-only types") {
    engine::SlotMap<std::unique_ptr<int>> map;
    const auto h = map.insert(std::make_unique<int>(7));
    REQUIRE(map.get(h) != nullptr);
    REQUIRE(*map.get(h) != nullptr);
    CHECK(**map.get(h) == 7);
    CHECK(map.remove(h));
    CHECK(map.get(h) == nullptr);
}

TEST_CASE("SlotMap: interleaved insert/remove stays consistent at scale") {
    constexpr int ELEMENT_COUNT = 1000;
    engine::SlotMap<int> map;
    std::vector<engine::SlotMap<int>::HandleType> handles;
    handles.reserve(ELEMENT_COUNT);
    for (int i = 0; i < ELEMENT_COUNT; ++i) {
        handles.push_back(map.insert(i));
    }
    for (int i = 0; i < ELEMENT_COUNT; i += 2) {
        CHECK(map.remove(handles[static_cast<std::size_t>(i)]));
    }
    for (int i = 0; i < ELEMENT_COUNT; ++i) {
        const auto* value = map.get(handles[static_cast<std::size_t>(i)]);
        if (i % 2 == 0) {
            CHECK(value == nullptr);
        } else {
            REQUIRE(value != nullptr);
            CHECK(*value == i);
        }
    }
    CHECK(map.size() == static_cast<std::size_t>(ELEMENT_COUNT / 2));
}

TEST_CASE("SlotMap: reserve preserves logical contents") {
    engine::SlotMap<int> map;
    map.reserve(128);
    const auto h1 = map.insert(1);
    const auto h2 = map.insert(2);
    REQUIRE(map.get(h1) != nullptr);
    REQUIRE(map.get(h2) != nullptr);
    CHECK(*map.get(h1) == 1);
    CHECK(*map.get(h2) == 2);
}

TEST_CASE("SlotMap: a distinct Tag yields a distinct handle type") {
    static_assert(!std::is_same_v<engine::SlotMap<int, TagA>::HandleType, engine::SlotMap<int, TagB>::HandleType>);
}

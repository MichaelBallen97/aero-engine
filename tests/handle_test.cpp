// Exercises engine::Handle<Tag> (task 0.2.1): the null sentinel, valid(), equality, the
// trivially-copyable 8-byte layout docs/03 relies on for serialization, and tag-based type safety.
#include <aero/core/handle.hpp>

#include <doctest/doctest.h>

#include <type_traits>

namespace {
struct TagA;
struct TagB;
}  // namespace

TEST_CASE("Handle: a default-constructed handle is null") {
    const engine::Handle<TagA> h{};
    CHECK(h.index == 0);
    CHECK(h.generation == 0);
    CHECK_FALSE(h.valid());
}

TEST_CASE("Handle: valid() tracks a non-zero generation") {
    const engine::Handle<TagA> live{5, 1};
    const engine::Handle<TagA> nullish{5, 0};
    CHECK(live.valid());
    CHECK_FALSE(nullish.valid());
}

TEST_CASE("Handle: equality compares index and generation") {
    const engine::Handle<TagA> a{1, 1};
    const engine::Handle<TagA> b{1, 1};
    const engine::Handle<TagA> diffGeneration{1, 2};
    const engine::Handle<TagA> diffIndex{2, 1};
    CHECK(a == b);
    CHECK(a != diffGeneration);
    CHECK(a != diffIndex);
}

TEST_CASE("Handle: is a trivially-copyable 8-byte value") {
    static_assert(std::is_trivially_copyable_v<engine::Handle<TagA>>);
    static_assert(sizeof(engine::Handle<TagA>) == 8);
    CHECK(sizeof(engine::Handle<TagA>) == 8);
}

TEST_CASE("Handle: distinct tags are distinct types") {
    static_assert(!std::is_same_v<engine::Handle<TagA>, engine::Handle<TagB>>);
}

TEST_CASE("Handle: usable in constant expressions") {
    constexpr engine::Handle<TagA> NULL_HANDLE{};
    static_assert(!NULL_HANDLE.valid());
    CHECK_FALSE(NULL_HANDLE.valid());
}

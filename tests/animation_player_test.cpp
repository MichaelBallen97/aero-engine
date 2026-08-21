// tests/animation_player_test.cpp — task 3.5.2: engine::AnimationPlayer and its playback clock (PL*).
//
// Tier 0 throughout: no GPU, no reflect-gen, no generated code, no files, no randomness. Rides
// aero_tests unconditionally and passes identically with -DAERO_REFLECT_TOOLS=OFF — the same
// structural proof camera_test.cpp carries, that the component has zero codegen dependency.
//
// The battery is the clock's six steps, one arm each, plus the two properties no single step owns:
// the loop and no-loop arms are driven through the SAME inputs in one case, so a swapped pair
// reddens (a case per arm cannot see that), and `playing` is re-asserted after every branch.
//
// <ostream> is included preventively: MSVC alone needs the complete type to stringify a string_view
// inside a doctest CHECK (the four-time trap in .claude/rules/ci-portability.md).

#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <ostream>
#include <type_traits>

using engine::advanceAnimationPlayer;
using engine::AnimationPlayer;

namespace {

constexpr float DURATION = 2.0F;

}  // namespace

TEST_CASE("animation player: layout, and every default pinned (PL1)") {
    static_assert(std::is_trivially_copyable_v<AnimationPlayer>);
    static_assert(std::is_standard_layout_v<AnimationPlayer>);
    static_assert(std::is_aggregate_v<AnimationPlayer>);
    static_assert(sizeof(AnimationPlayer) == 12);
    CHECK(sizeof(AnimationPlayer) == 12);

    const AnimationPlayer p{};
    CHECK(p.time == 0.0F);
    CHECK(p.speed == 1.0F);
    CHECK(p.loop);
    CHECK(p.playing);

    CHECK(AnimationPlayer{} == AnimationPlayer{});
    AnimationPlayer other{};
    other.speed = 2.0F;
    CHECK_FALSE(AnimationPlayer{} == other);
}

TEST_CASE("animation player: a paused player's time is untouched by any delta (PL2)") {
    AnimationPlayer p{};
    p.time = 0.375F;
    p.playing = false;

    advanceAnimationPlayer(p, 1000.0F, DURATION);
    CHECK(p.time == 0.375F);  // bit-identical, so pause/resume is exact
    advanceAnimationPlayer(p, -1000.0F, DURATION);
    CHECK(p.time == 0.375F);
    CHECK_FALSE(p.playing);

    // Both deltas above are WHOLE MULTIPLES of DURATION, so a clock that advanced a paused player
    // would fmod straight back to 0.375 and this case could not see it at all. These two are not
    // multiples, and they are what makes the pause guard observable.
    advanceAnimationPlayer(p, 0.75F, DURATION);
    CHECK(p.time == 0.375F);
    advanceAnimationPlayer(p, -0.75F, DURATION);
    CHECK(p.time == 0.375F);
    CHECK_FALSE(p.playing);
}

TEST_CASE("animation player: zero, negative and NaN durations all reset time to zero (PL3)") {
    const float nan = std::numeric_limits<float>::quiet_NaN();

    for (const float duration : {0.0F, -1.0F, nan}) {
        AnimationPlayer p{};
        p.time = 1.25F;
        advanceAnimationPlayer(p, 0.5F, duration);
        CHECK(p.time == 0.0F);
        CHECK(p.playing);
    }
}

TEST_CASE("animation player: a forward step landing exactly on duration wraps to zero (PL4)") {
    AnimationPlayer p{};
    p.time = 0.5F;
    advanceAnimationPlayer(p, 1.5F, DURATION);  // 0.5 + 1.5 == DURATION exactly
    CHECK(p.time == 0.0F);

    p.time = 0.5F;
    advanceAnimationPlayer(p, 1.0F, DURATION);  // strictly inside: no wrap
    CHECK(p.time == 1.5F);
}

TEST_CASE("animation player: a delta spanning 100000 periods stays in range and finite (PL5)") {
    AnimationPlayer p{};
    p.time = 0.25F;
    advanceAnimationPlayer(p, 100000.0F * DURATION, DURATION);
    CHECK(std::isfinite(p.time));
    CHECK(p.time >= 0.0F);
    CHECK(p.time <= DURATION);
    CHECK(p.time == 0.25F);  // fmod is EXACT: a whole number of periods changes nothing

    p.time = 0.25F;
    p.speed = -1.0F;
    advanceAnimationPlayer(p, 100000.0F * DURATION, DURATION);
    CHECK(std::isfinite(p.time));
    CHECK(p.time >= 0.0F);
    CHECK(p.time <= DURATION);
    CHECK(p.time == 0.25F);
}

TEST_CASE("animation player: reverse looping wraps to the END, never sticking at zero (PL6)") {
    AnimationPlayer p{};
    p.time = 0.25F;
    p.speed = -1.0F;

    advanceAnimationPlayer(p, 0.5F, DURATION);  // 0.25 - 0.5 == -0.25 -> 1.75
    CHECK(p.time == 1.75F);
    advanceAnimationPlayer(p, 0.5F, DURATION);
    CHECK(p.time == 1.25F);
    CHECK(p.time > 0.0F);
}

TEST_CASE("animation player: a non-looping player clamps at duration and HOLDS (PL7)") {
    AnimationPlayer p{};
    p.loop = false;
    p.time = 1.5F;

    advanceAnimationPlayer(p, 1.0F, DURATION);
    CHECK(p.time == DURATION);
    for (int i = 0; i < 10; ++i) {
        advanceAnimationPlayer(p, 1.0F, DURATION);
        CHECK(p.time == DURATION);
    }
    CHECK(p.playing);
}

TEST_CASE("animation player: a non-looping reverse player clamps at zero and HOLDS (PL8)") {
    AnimationPlayer p{};
    p.loop = false;
    p.speed = -1.0F;
    p.time = 0.5F;

    advanceAnimationPlayer(p, 1.0F, DURATION);
    CHECK(p.time == 0.0F);
    for (int i = 0; i < 10; ++i) {
        advanceAnimationPlayer(p, 1.0F, DURATION);
        CHECK(p.time == 0.0F);
    }
    CHECK(p.playing);
}

TEST_CASE("animation player: the loop and no-loop arms are not swapped (PL9)") {
    // ONE case driving the identical inputs through both arms: a swapped pair leaves each arm's own
    // case green, because each is still SOME defined behaviour -- only the comparison sees it.
    AnimationPlayer looping{};
    looping.time = 1.5F;
    AnimationPlayer holding = looping;
    holding.loop = false;

    advanceAnimationPlayer(looping, 1.0F, DURATION);  // 2.5 -> wraps
    advanceAnimationPlayer(holding, 1.0F, DURATION);  // 2.5 -> clamps

    CHECK(looping.time == 0.5F);
    CHECK(holding.time == DURATION);
    CHECK(looping.time != holding.time);
}

TEST_CASE("animation player: speed zero freezes time without clearing playing (PL10)") {
    AnimationPlayer p{};
    p.time = 0.875F;
    p.speed = 0.0F;

    advanceAnimationPlayer(p, 1.0F, DURATION);
    CHECK(p.time == 0.875F);
    advanceAnimationPlayer(p, 1000.0F, DURATION);
    CHECK(p.time == 0.875F);
    CHECK(p.playing);
}

TEST_CASE("animation player: a non-finite delta or speed resets time rather than poisoning it (PL11)") {
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    for (const float delta : {inf, -inf, nan}) {
        AnimationPlayer p{};
        p.time = 0.5F;
        advanceAnimationPlayer(p, delta, DURATION);
        CHECK(p.time == 0.0F);
        CHECK(std::isfinite(p.time));
    }
    for (const float speed : {inf, -inf, nan}) {
        AnimationPlayer p{};
        p.time = 0.5F;
        p.speed = speed;
        advanceAnimationPlayer(p, 0.5F, DURATION);
        CHECK(p.time == 0.0F);
        CHECK(std::isfinite(p.time));
    }
}

TEST_CASE("animation player: playing is changed by nobody, on every branch (PL12)") {
    const float nan = std::numeric_limits<float>::quiet_NaN();

    for (const bool playing : {true, false}) {
        for (const bool loop : {true, false}) {
            AnimationPlayer p{};
            p.playing = playing;
            p.loop = loop;

            advanceAnimationPlayer(p, 0.5F, DURATION);      // the ordinary branch
            advanceAnimationPlayer(p, 1000.0F, DURATION);   // the wrap/clamp branch
            advanceAnimationPlayer(p, -1000.0F, DURATION);  // the reverse branch
            advanceAnimationPlayer(p, 0.5F, 0.0F);          // the no-duration branch
            advanceAnimationPlayer(p, nan, DURATION);       // the non-finite branch
            CHECK(p.playing == playing);
            CHECK(p.loop == loop);
        }
    }
}

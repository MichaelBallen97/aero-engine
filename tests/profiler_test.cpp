// Exercises the profiling macro wrappers (task 0.1.5): they must compile, link, and run in BOTH
// configs — no-op when profiling is OFF (Debug presets), real Tracy zones when ON (Release presets).
// No Tracy Profiler needs to be attached; with none connected the client idles and the process exits.
#include <doctest/doctest.h>
#include <aero/core/profiler.hpp>

namespace {
int instrumented_work() {
    AERO_PROFILE_ZONE_NAMED("aero::tests::instrumented_work");
    int sum = 0;
    for (int i = 0; i < 1000; ++i) sum += i;
    return sum;
}
} // namespace

TEST_CASE("profiling: macro wrappers compile, link, and run in both configs") {
    CHECK(instrumented_work() == 499500);
    AERO_PROFILE_FRAME_MARK;   // must be a valid statement in both configs
}

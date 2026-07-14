// Exercises the profiling macro wrappers (task 0.1.5): they must compile, link, and run in BOTH
// configs — no-op when profiling is OFF (Debug presets), real Tracy zones when ON (Release presets).
// No Tracy Profiler needs to be attached; with none connected the client idles and the process exits.
#include <aero/core/profiler.hpp>

#include <doctest/doctest.h>

namespace {
int instrumentedWork() {
    AERO_PROFILE_ZONE_NAMED("aero::tests::instrumentedWork");
    int sum = 0;
    for (int i = 0; i < 1000; ++i) sum += i;
    return sum;
}
}  // namespace

TEST_CASE("profiling: macro wrappers compile, link, and run in both configs") {
    CHECK(instrumentedWork() == 499500);
    AERO_PROFILE_FRAME_MARK;  // must be a valid statement in both configs
}

// Compile + link coverage for every wrapper (task 0.1.5 verify pass): each AERO_PROFILE_*
// macro must be a valid statement in BOTH configs — a no-op when profiling is OFF (Debug),
// a real Tracy call when ON (Release). This guards every wrapper's Tracy mapping: a misspelled
// expansion would fail to compile/link in the Release (Tracy-on) path. The three scoped-zone
// macros each declare the same Tracy scope object, so each lives in its own block scope
// (the header's documented "use at the top of a braced scope" contract).
TEST_CASE("profiling: every wrapper macro is a valid statement in both configs") {
    int sum = 0;
    {
        AERO_PROFILE_ZONE;  // no-arg scoped zone
        for (int i = 0; i < 10; ++i) sum += i;
    }
    { AERO_PROFILE_ZONE_NAMED("aero::tests::zone_named"); }
    { AERO_PROFILE_ZONE_NAMED_COLOR("aero::tests::zone_color", 0x00FF00); }

    AERO_PROFILE_FRAME_MARK_NAMED("aero::tests::secondary_frame");
    AERO_PROFILE_PLOT("aero::tests::plot", 42.0);  // double: unambiguous PlotData overload
    AERO_PROFILE_MESSAGE("aero::tests::message");
    AERO_PROFILE_SET_THREAD_NAME("aero::tests::thread");

    CHECK(sum == 45);
}

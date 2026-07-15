// Aero Engine — the jobs-boundary COMPILE-TIME guard (task 0.2.5).
//
// THIS FILE ASSERTS BY EXISTING. It is not a doctest suite and has no TEST_CASE: the assertion is that
// it COMPILES. Its target (aero_jobs_boundary_probe, tests/CMakeLists.txt) links ONLY aero::core, so
// vcpkg's shared per-triplet include/ directory never reaches its compile line and enkiTS is genuinely
// unreachable here. If jobs.hpp ever starts including <TaskScheduler.h>, this TU fails to compile and
// the build goes red at the moment the leak is written.
//
// aero_tests CANNOT do this: it links doctest::doctest and inherits vcpkg's whole shared
// per-triplet include/ root (risk R12, docs/08-risks.md), so <enkiTS/TaskScheduler.h> resolves
// there regardless of what aero_core links. (enkiTS's own supported spelling, <TaskScheduler.h>,
// needs include/enkiTS — which only enkiTS::enkiTS puts on a compile line — so the R12 hole is
// narrower here than it is for glm/spdlog. The `lint` job's grep covers BOTH spellings.)
//
// KEEP THIS TARGET DEPENDENCY-FREE — see tests/CMakeLists.txt.

#include <aero/core/jobs.hpp>

#include <type_traits>

// Name the public surface so the header is instantiated, not merely parsed. No NAMED entity is
// declared anywhere in this TU (docs/04's naming law has nothing to bind to).
static_assert(sizeof(engine::JobRange) >= sizeof(std::uint32_t) * 2);
static_assert(engine::JobConfig{}.threadCount == 0);
static_assert(!engine::Handle<engine::Job>{}.valid());

// JobSystem/JobGraph are asserted through their documented "neither copyable nor movable" contract
// rather than `sizeof(T) > 0`: a trait still requires the type to be COMPLETE (so the header is
// fully instantiated, which is this probe's job) while actually asserting something the API
// promises. `sizeof(T) > 0` is a tautology and trips bugprone-sizeof-expression.
static_assert(!std::is_copy_constructible_v<engine::JobSystem>);
static_assert(!std::is_move_constructible_v<engine::JobSystem>);
static_assert(!std::is_copy_constructible_v<engine::JobGraph>);
static_assert(!std::is_move_constructible_v<engine::JobGraph>);

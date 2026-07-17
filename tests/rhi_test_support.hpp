#pragma once
// Aero Engine — shared gating helpers for the GPU-dependent rhi tests (task 0.4.2). Tests-local —
// never an engine header. AERO_REQUIRE_GPU=1 (CI) turns every environment-skip into a FAILURE so
// lanes cannot silently lose GPU coverage (spec D7/D8); unset (local default), skips are loud
// MESSAGEs.
//
// Context is non-movable (one-per-process root) — there is no factory helper returning one. Every
// gated TEST_CASE constructs `engine::platform::Context ctx{{.headless = false}};` IN PLACE, gates
// on `ctx.valid()` (no real video driver -> skip/fail), then on `engine::rhi::Device::create()` (no
// GPU -> skip/fail). Windows note: headless D3D12 creation *works* (0.4.2 spec D6), but the gate
// still uses a real-video Context everywhere for uniformity and because tier 2 needs one anyway.

#include <doctest/doctest.h>

#include <cstdlib>
#include <string_view>

namespace rhi_test {

inline bool gpuRequired() {
    const char* const value = std::getenv("AERO_REQUIRE_GPU");
    return value != nullptr && std::string_view{value} != "0";
}

}  // namespace rhi_test

// Statement-macro: usable only inside a TEST_CASE body. FAILs the case (CI's ratchet) when the
// environment demands GPU coverage; otherwise MESSAGEs and returns from the enclosing function.
#define AERO_SKIP_OR_FAIL(reason)      \
    do {                               \
        if (rhi_test::gpuRequired()) { \
            FAIL(reason);              \
        } else {                       \
            MESSAGE(reason);           \
            return;                    \
        }                              \
    } while (false)

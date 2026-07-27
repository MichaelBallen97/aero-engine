// Aero Engine — render::RenderTarget tests (task 2.2.3). Tier-0 (this section, no GPU, always
// compiled): nextTargetExtent is a pure, GPU-free, total sizing policy — the framePaceSleepMs
// precedent. Tier-2 (Step 3): GPU-gated RenderTarget lifecycle/resize/draw cases, appended below.
#include <aero/render/render_target.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
// <ostream> is load-bearing on MSVC for enum/string_view CHECKs (see rhi_device_test.cpp's comment).
#include <ostream>

using engine::render::nextTargetExtent;
using engine::render::RENDER_TARGET_MAX_EXTENT;
using engine::render::RenderTargetConfig;
using namespace engine::rhi;

namespace {

struct SizingCase {
    Extent2D requested;
    Extent2D current;
    std::uint32_t quantum;
    std::uint32_t maxExtent;
    Extent2D expected;
};

}  // namespace

TEST_CASE("render target: nextTargetExtent is total, quantised and hysteretic (tier-0, no GPU)") {
    constexpr std::uint32_t MAX = RENDER_TARGET_MAX_EXTENT;
    constexpr std::uint32_t U32_MAX = 4294967295U;
    // clang-format off
    const std::array<SizingCase, 13> cases{{
        // requested       current        quantum  maxExtent  expected         // why
        {{100, 100},       {0, 0},        64,      MAX,       {128, 128}},     // 1: first allocation
        {{1280, 720},      {0, 0},        1,       MAX,       {1280, 720}},    // 2: q==1 is exact (D6)
        {{1300, 720},      {1280, 768},   64,      MAX,       {1344, 768}},    // 3: x grows, y kept
        {{1200, 700},      {1280, 768},   64,      MAX,       {1280, 768}},    // 4: keep (AC-6)
        {{640, 384},       {1280, 768},   64,      MAX,       {640, 384}},     // 5: shrink at half
        {{641, 385},       {1280, 768},   64,      MAX,       {1280, 768}},    // 6: no shrink above half
        {{200, 2000},      {1280, 768},   64,      MAX,       {256, 2048}},    // 7: shrink+grow, per axis
        {{0, 0},           {0, 0},        64,      MAX,       {64, 64}},       // 8: zero clamped to >=1
        {{9000, 9000},     {0, 0},        64,      8192,      {8192, 8192}},   // 9: clamp to max
        {{8192, 8192},     {0, 0},        100,     8192,      {8192, 8192}},   // 10: E18 clamp AFTER round-up
        {{100, 100},       {0, 0},        0,       MAX,       {100, 100}},     // 11: E17 no divide-by-zero
        {{64, 64},         {0, 0},        64,      64,        {64, 64}},       // 12: maxExtent == quantum
        {{U32_MAX, U32_MAX}, {0, 0},      64,      U32_MAX,   {U32_MAX, U32_MAX}}, // 13: C2 64-bit round-up
    }};
    // clang-format on

    for (const SizingCase& c : cases) {
        const Extent2D result = nextTargetExtent(c.requested, c.current, c.quantum, c.maxExtent);
        CHECK(result.width == c.expected.width);
        CHECK(result.height == c.expected.height);
    }
}

TEST_CASE("render target: nextTargetExtent postcondition — result >= clamp(req,1,max) (AC-14)") {
    const std::array<std::uint32_t, 5> currents{0, 64, 640, 1280, 4096};
    const std::array<std::uint32_t, 3> quanta{1, 16, 64};

    for (const std::uint32_t current : currents) {
        for (const std::uint32_t quantum : quanta) {
            for (std::uint32_t req = 0; req <= 2048; req += 7) {
                const std::uint32_t maxExtent = RENDER_TARGET_MAX_EXTENT;
                const Extent2D result = nextTargetExtent({req, req}, {current, current}, quantum, maxExtent);
                const std::uint32_t clampedReq = req < 1 ? 1 : (req > maxExtent ? maxExtent : req);
                CHECK(result.width >= clampedReq);
                CHECK(result.height >= clampedReq);
                CHECK(result.width <= maxExtent);
                CHECK(result.height <= maxExtent);
            }
        }
    }

    // C2's max-boundary sweep: maxExtent at the extreme end of uint32_t, req at/just under it.
    const std::array<std::uint32_t, 3> extremeMax{8192, 4294967294U, 4294967295U};
    const std::array<std::uint32_t, 3> extremeQuanta{1, 16, 64};
    for (const std::uint32_t maxExtent : extremeMax) {
        for (const std::uint32_t quantum : extremeQuanta) {
            for (const std::uint32_t req : {maxExtent - 1, maxExtent}) {
                const Extent2D result = nextTargetExtent({req, req}, {0, 0}, quantum, maxExtent);
                const std::uint32_t clampedReq = req < 1 ? 1 : req;
                CHECK(result.width >= clampedReq);
                CHECK(result.height >= clampedReq);
                CHECK(result.width <= maxExtent);
                CHECK(result.height <= maxExtent);
            }
        }
    }
}

TEST_CASE("render target: RenderTargetConfig defaults are load-bearing") {
    const RenderTargetConfig config{};
    CHECK(config.quantum == 1);
    CHECK(config.depth == true);
    CHECK((config.colorFormat == TextureFormat::RGBA8Unorm));
    CHECK(config.maxExtent == RENDER_TARGET_MAX_EXTENT);
    CHECK(RENDER_TARGET_MAX_EXTENT == 8192);
}

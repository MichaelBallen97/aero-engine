#pragma once
// Task 2.2.2 fixture — every field-annotation path in one `Annotated` struct: a valid range, a valid
// colour, two malformed ranges (non-literal bounds; inverted bounds), two misapplied ranges (on a
// Vec3, on a bool), a misapplied colour (on a float), an unknown engine:: annotation (a raw
// misspelling AERO_RANGE itself cannot produce), and a foreign (non-engine::) annotation that must be
// ignored SILENTLY (E7). Process-boundary only (deliberately on no HEADERS list): its misapplied
// annotations exist to produce warnings, not to compile.
#include <aero/core/math.hpp>

#include "aero_reflect.hpp"

struct AERO_COMPONENT Annotated {
    float speed AERO_RANGE(0.0f, 10.0f) = 1.0f;
    std::uint32_t level AERO_RANGE(0, 2) = 0;
    engine::Vec3 tint AERO_COLOR = engine::Vec3::one();

    float badRange AERO_RANGE(lo, hi) = 0.0f;  // non-literal bounds -- malformed, dropped
    float inverted AERO_RANGE(5, 1) = 0.0f;    // min > max -- malformed, dropped

    engine::Vec3 rangedVec AERO_RANGE(0, 1);   // range on a non-scalar -- misapplied, dropped
    bool rangedBool AERO_RANGE(0, 1) = false;  // range on a bool -- misapplied, dropped
    float coloredFloat AERO_COLOR = 0.0f;      // colour on a non-Vec3 -- misapplied, dropped

    int typo [[clang::annotate("engine::rnage:0:1")]] = 0;  // raw: unknown engine:: annotation
    int foreign [[clang::annotate("other::thing")]] = 0;    // raw: non-engine:: -- ignored silently
};

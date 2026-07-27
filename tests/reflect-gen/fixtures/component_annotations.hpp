#pragma once
// Task 2.2.2 fixture — every field-annotation path in one `Annotated` struct: a valid range, a valid
// colour, malformed ranges (non-literal bounds; inverted bounds; non-finite bounds; an
// out-of-range/overflowing literal), misapplied ranges (on a Vec3, on a bool), a misapplied colour
// (on a float), an unknown engine:: annotation (a raw misspelling AERO_RANGE itself cannot produce),
// a foreign (non-engine::) annotation that must be ignored SILENTLY (E7), and a legitimately negative
// bound on an UNSIGNED destination (syntactically valid -- the runtime seam's own clamp, not the
// tool's parse, is what handles it; this must NOT be rejected). Process-boundary only (deliberately
// on no HEADERS list): its misapplied/malformed annotations exist to produce warnings, not to
// compile.
#include <aero/core/math.hpp>

#include "aero_reflect.hpp"

#include <cstdint>

struct AERO_COMPONENT Annotated {
    float speed AERO_RANGE(0.0f, 10.0f) = 1.0f;
    std::uint32_t level AERO_RANGE(0, 2) = 0;
    engine::Vec3 tint AERO_COLOR = engine::Vec3::one();
    // The negative-into-unsigned case: -1 is a syntactically VALID numeric literal (parseRangeToken
    // has, and needs, no notion of the destination field's own signedness) -- this must be ACCEPTED,
    // never rejected, so a future grammar tightening does not start over-rejecting legitimate bounds.
    // component_ops.cpp's clampRangeUint64 is what turns this into a sane runtime clamp (D8).
    std::uint32_t negativeUnsigned AERO_RANGE(-1, 5) = 0;

    float badRange AERO_RANGE(lo, hi) = 0.0f;  // non-literal bounds -- malformed, dropped
    float inverted AERO_RANGE(5, 1) = 0.0f;    // min > max -- malformed, dropped
    // Task 2.2.2 review finding 1: strtod is a MAGNITUDE oracle, not a C++-literal-grammar oracle --
    // it happily accepts "inf"/"infinity"/"nan" and silently overflows an out-of-range literal to
    // +-HUGE_VAL. All three below must be rejected the SAME way as badRange/inverted: warn, drop,
    // exit 0 -- never emitted verbatim into generated code.
    float nonFiniteHigh AERO_RANGE(0, INFINITY) = 0.0f;  // non-finite bound -- malformed, dropped
    float nonFiniteLow AERO_RANGE(NAN, 10) = 0.0f;       // non-finite bound -- malformed, dropped
    float outOfRange AERO_RANGE(0, 1e400) = 0.0f;        // ERANGE-overflowing literal -- malformed, dropped

    engine::Vec3 rangedVec AERO_RANGE(0, 1);   // range on a non-scalar -- misapplied, dropped
    bool rangedBool AERO_RANGE(0, 1) = false;  // range on a bool -- misapplied, dropped
    float coloredFloat AERO_COLOR = 0.0f;      // colour on a non-Vec3 -- misapplied, dropped

    int typo [[clang::annotate("engine::rnage:0:1")]] = 0;  // raw: unknown engine:: annotation
    int foreign [[clang::annotate("other::thing")]] = 0;    // raw: non-engine:: -- ignored silently
};

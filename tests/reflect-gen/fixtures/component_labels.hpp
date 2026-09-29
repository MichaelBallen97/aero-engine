#pragma once
// Task E.5.2 fixture -- the AERO_LABELS annotation, every arm. Modelled on component_asset.hpp: the REAL
// engine vocabulary header, namespaced, so the emitted TU compiles into aero_reflect_meta_test. Every
// dropped arm is an ORDINARY field whose labels -- and nothing else -- are dropped, so this header compiles
// and registers cleanly. The 64- and 65-label cap arms are written by labels_malformed into its own
// WORK_DIR: sixty-five identifiers here would drown the fixture.
//
// hexRange's bounds end in no hex digit F: parseRangeToken strips one trailing f/F before it recognises
// a hex literal (a latent, pre-existing quirk recorded as unowned in the E.5.2 handoffs), and this arm
// exists to prove the NUMERIC comparison, not to trip that.
#include <aero/reflect/annotations.hpp>

#include <cstdint>

namespace engine::demo {
struct AERO_COMPONENT Labelled {
    std::uint32_t tier AERO_RANGE(0, 2) AERO_LABELS(Low, Mid, High) = 0;  // valid, unsigned
    std::int16_t gear AERO_RANGE(0, 1) AERO_LABELS(Park, Drive) = 0;      // valid, SIGNED
    std::uint8_t hexRange AERO_RANGE(0x0, 0x2) AERO_LABELS(A, B, C) = 0;  // valid: compared NUMERICALLY
    std::uint32_t plain AERO_RANGE(0, 3) = 0;                             // a range custom, no labels
    float ratio AERO_RANGE(0.0f, 1.0f) AERO_LABELS(Off, On) = 0.0F;       // MISAPPLIED (float): range kept
    bool flag AERO_LABELS(No, Yes) = false;                               // MISAPPLIED (bool): no custom
    std::uint32_t numeric AERO_RANGE(0, 1) AERO_LABELS(1st, 2nd) = 0;     // MALFORMED: range kept
    std::uint32_t empty AERO_RANGE(0, 0) AERO_LABELS() = 0;               // MALFORMED (empty list)
    std::uint32_t twice AERO_RANGE(0, 1) AERO_LABELS(Same, Same) = 0;     // MALFORMED (duplicate)
    std::uint32_t shortList AERO_RANGE(0, 3) AERO_LABELS(One, Two) = 0;   // MISMATCH (max 3 != 1)
    std::uint32_t offset AERO_RANGE(1, 2) AERO_LABELS(One, Two) = 0;      // MISMATCH (min 1 != 0)
    std::uint32_t unranged AERO_LABELS(Up, Down) = 0;                     // MISMATCH (no range): no custom
};
}  // namespace engine::demo

#pragma once
// Task 2.2.2 (D18) -- the zero-per-component-editor-code proof. `git grep InspectorProbe --
// editor/` is empty: no editor source names this type, so a passing model case over it proves the
// inspector's walk is generic, not a hardcoded list (AC-11). The last two fields are O2's cheap
// coverage pins -- both already inside classifyField's whitelist, so they need no tool change and
// produce no warning, yet the SPEC's 15-type list would have rendered neither.
#include <aero/core/math.hpp>
#include <aero/reflect/annotations.hpp>

#include <cstdint>
#include <string>

struct AERO_COMPONENT InspectorProbe {
    float speed AERO_RANGE(0.0f, 10.0f) = 1.0f;          // ranged float
    engine::Vec3 tint AERO_COLOR = engine::Vec3::one();  // colour Vec3
    std::string label;                                   // the D3 string category
    std::int16_t gear = 0;                               // narrow signed
    std::uint8_t tiny = 0;                               // narrow unsigned -- the E9/S5 clamp target
    bool enabled = false;
    engine::Quat aim;
    double mass = 0.0;
    long tick = 0;       // O2: CXType_Long. The spec's 15-type list omits it -- WITHOUT O2 this
                         // field is accepted by the generator and INVISIBLE in the inspector.
    char16_t glyph = 0;  // O2: CXType_Char16, always unsigned -- the exotic end of the same hole.
};

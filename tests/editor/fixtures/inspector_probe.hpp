#pragma once
// Task 2.2.2 (D18) -- the zero-per-component-editor-code proof. `git grep InspectorProbe --
// editor/` is empty: no editor source names this type, so a passing model case over it proves the
// inspector's walk is generic, not a hardcoded list (AC-11). `tick`/`glyph` are O2's cheap coverage
// pins -- both already inside classifyField's whitelist, so they need no tool change and produce no
// warning, yet the SPEC's 15-type list would have rendered neither. `clampedRange`/`hugeRange` are
// review finding 2's coverage pins, chosen so an ORDINARY write ALWAYS crosses the bound that used to
// be cast unconditionally: `clampedRange` (unsigned) carries a wholly-negative range, so every
// non-negative write exceeds `rangeMax` -- the branch clampRangeUint64 did NOT guard (only its
// `rangeMin <= 0.0` branch was guarded, the asymmetry the review caught); `hugeRange` (signed)
// carries a range whose bounds exceed int64_t's own domain, so every write is below `rangeMin` --
// both used to reach an UNDEFINED-BEHAVIOUR double->integer cast in the seam before doubleToClamped
// closed it.
// task 3.1.5 appends ONE field, `asset`: the reflectable subset's new engine::Guid category. It is
// APPENDED, never inserted, so every positional assertion below index 11 keeps its meaning -- the
// field-count REQUIREs move 12 -> 13 and nothing else does. It is what gives this binary a reflected
// Guid field to read, write and refuse a wrong type on (IR6/IR7), without borrowing a real component
// whose entt::meta registration is process-lifetime and shared with the AC-12 drift pin.
#include <aero/core/guid.hpp>
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
    // A wholly-negative range on an UNSIGNED field: min<=max holds (-10<=-5), so it parses; every
    // non-negative write exceeds rangeMax, which used to be cast unguarded (finding 2).
    std::uint16_t clampedRange AERO_RANGE(-10, -5) = 0;
    // A range whose bounds exceed int64_t's own domain: every write's widened int64 falls below
    // rangeMin, which used to be cast unguarded regardless of magnitude (finding 2).
    std::int16_t hugeRange AERO_RANGE(1e300, 2e300) = 0;
    engine::Guid asset;  // task 3.1.5's category; nil by default, which is a VALUE and not an absence
};

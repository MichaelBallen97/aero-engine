#pragma once
// Task 1.2.2 fixture — the numeric-edge component: 64-bit integers beyond double's 2^53 (D5's raison
// d'etre), narrow integrals for range checking, and a `long double` pinning the D11 whitelist skip in
// BOTH generated functions. Self-contained (no math.hpp) — distinct stem and distinct type name.
#include "aero_reflect.hpp"

#include <cstdint>

struct AERO_COMPONENT ReflectLimits {
    std::int64_t big;
    std::uint64_t huge;
    std::int16_t small;
    std::uint8_t tiny;
    double precise;
    long double weird;  // D11: unsupported -> skipped by both generated functions (still compiles)
};

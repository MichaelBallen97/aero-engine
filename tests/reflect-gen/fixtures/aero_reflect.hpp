#pragma once
// Task 1.1.2 — the engine's component-annotation vocabulary (tests-local home; promoted to an
// engine/reflect header when the first REAL component is authored in engine code, task 1.3.2).
//
// AERO_COMPONENT marks a struct/class for reflection. Under aero_reflect_gen (which defines
// AERO_REFLECT_PARSE) it expands to a clang::annotate the tool detects as a first-class AST node;
// under the real compiler it expands to NOTHING — zero attribute, zero warning, zero runtime cost.
#if defined(AERO_REFLECT_PARSE)
    #define AERO_COMPONENT [[clang::annotate("engine::component")]]
#else
    #define AERO_COMPONENT
#endif

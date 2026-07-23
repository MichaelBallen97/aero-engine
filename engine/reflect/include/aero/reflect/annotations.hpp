#pragma once
// Aero Engine — the reflection annotation vocabulary (ADR-004; promoted from
// tests/reflect-gen/fixtures/aero_reflect.hpp at task 1.3.2, per the 1.1.2 record).
//
// AERO_COMPONENT marks a struct/class definition, at namespace scope, for reflection:
//
//     struct AERO_COMPONENT Transform { ... };
//
// Under aero_reflect_gen — which auto-injects -DAERO_REFLECT_PARSE=1 into every parse — it
// expands to [[clang::annotate("engine::component")]], a first-class AST node the tool detects
// (a bare [[engine::component]] would be DISCARDED by Clang: no attribute cursor survives).
// Under the real compiler it expands to NOTHING: zero attribute, zero warning, zero cost.
//
// THE STRING "engine::component" IS A FROZEN CONTRACT with tools/reflect-gen's detector; the
// process-boundary case reflect-gen.components_engine_transform runs the real tool over a real
// engine header, so a drift on either side turns CI red. Fields must stay inside reflect-gen's
// supported subset (primitives + engine::Vec3/Quat) to serialize; anything else is collected,
// tagged unsupported, and skipped with a warning — never an error (the 1.1.2 leniency).
#if defined(AERO_REFLECT_PARSE)
    #define AERO_COMPONENT [[clang::annotate("engine::component")]]
#else
    #define AERO_COMPONENT
#endif

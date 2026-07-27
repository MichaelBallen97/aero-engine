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
// supported subset (primitives + engine::Vec3/Quat/std::string) to serialize; anything else is
// collected, tagged unsupported, and skipped with a warning — never an error (the 1.1.2 leniency).
#if defined(AERO_REFLECT_PARSE)
    #define AERO_COMPONENT [[clang::annotate("engine::component")]]
    // FIELD annotations (task 2.2.2). Canonical position: AFTER the field name, BEFORE any
    // initializer -- ADR-004's own example position, verified to land the AnnotateAttr as a direct
    // child of the FieldDecl with AND without an NSDMI:
    //     float fovYRadians AERO_RANGE(0.0175f, 3.1241f) = radians(60.0f);
    //     Vec3  color       AERO_COLOR                   = Vec3::one();
    // AERO_RANGE's arguments must be numeric LITERALS (optional sign, optional single trailing
    // f/F/l/L). Stringization passes them through verbatim; the tool validates and warns + ignores
    // anything it cannot parse -- never an error, never a build break.
    #define AERO_RANGE(minLiteral, maxLiteral) [[clang::annotate("engine::range:" #minLiteral ":" #maxLiteral)]]
    #define AERO_COLOR [[clang::annotate("engine::color")]]
#else
    #define AERO_COMPONENT
    #define AERO_RANGE(minLiteral, maxLiteral)
    #define AERO_COLOR
#endif

namespace engine::reflect {

// The RUNTIME MIRROR of the field annotations above: attached per data member by the GENERATED
// entt::meta registration (entt custom data) and read back by the editor's inspector. It lives beside
// the macros deliberately -- vocabulary and mirror must never version-skew, and ONE ODR-single
// definition is what makes `.custom<engine::reflect::FieldUiMeta>` resolvable from both the generated
// TU and the editor. Plain aggregate of bool/double: third-party-free, so the boundary rule holds by
// construction. Doubles, not floats: the same struct describes a range on a uint64 and on a float.
//
// SPARSE BY DESIGN: --emit-meta attaches one ONLY to a field carrying at least one annotation. An
// unannotated field has no custom, and entt's typed retrieval yields nullptr == "all defaults". Never
// emit a default-valued custom "to be uniform".
//
// FIELD ORDER IS A FROZEN CONTRACT with the emitter's designated-initializer output (C++20 requires
// designated initializers in declaration order).
struct FieldUiMeta {
    bool hasRange = false;
    double rangeMin = 0.0;
    double rangeMax = 0.0;
    bool color = false;
};

}  // namespace engine::reflect

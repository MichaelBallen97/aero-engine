#pragma once
// Task E.3.3 fixture -- the AERO_ASSET annotation, every arm. Modelled on component_guid.hpp: the
// REAL engine vocabulary header, namespaced, so the emitted TU compiles into aero_reflect_meta_test.
// The misapplied and malformed members are ORDINARY fields whose annotation is dropped, so this
// header compiles and registers cleanly -- unlike component_annotations.hpp, which is on no HEADERS
// list at all.
//
// `dashed`'s payload is spaced `tex - ture` because clang-format reads the `-` as a binary operator
// inside a macro argument and will re-space it on every run. The `#` operator folds that whitespace
// to single spaces, so the stringized payload is "tex - ture" -- still not an identifier, which is
// the arm this member exists for. Do not close the spaces up; clang-format will just reopen them.
#include <aero/core/guid.hpp>
#include <aero/reflect/annotations.hpp>

namespace engine::demo {
struct AERO_COMPONENT Referenced {
    engine::Guid texture AERO_ASSET(texture);    // valid: emits .assetKind = "texture"
    engine::Guid anything;                       // unannotated: NO custom at all
    engine::Guid shader AERO_ASSET(shader);      // grammar-valid, vocabulary-UNKNOWN: emitted verbatim;
                                                 // the EDITOR refuses it (assetReferenceKindFromToken)
    float scale AERO_ASSET(model) = 1.0F;        // MISAPPLIED (not a Guid): warn, drop
    engine::Guid dashed AERO_ASSET(tex - ture);  // malformed token: warn, drop
    engine::Guid empty AERO_ASSET();             // an EMPTY token stringizes to "": malformed, drop
};
}  // namespace engine::demo

#pragma once
// 1.2 audit fixture — a component nested inside a class: still DETECTED (1.1.2 E4), but --emit-json
// must SKIP it (a class-scope component cannot be wrapped in a namespace block; the generated TU
// would not compile) while the namespace-scope sibling is emitted normally.
#include "aero_reflect.hpp"

namespace engine {

class NestedOuter {
public:
    struct AERO_COMPONENT NestedInner {
        int hp;
        float speed;
    };
};

struct AERO_COMPONENT NestedSibling {
    float mass;
};

}  // namespace engine

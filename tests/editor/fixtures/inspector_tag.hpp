#pragma once
// Task 2.2.2 review finding 6 -- a zero-field "tag" component, modeled on
// tests/reflect-gen/fixtures/component_tag.hpp's shape: detected, meta-registered, with NO fields.
// InspectorProbe alone (10-12 fields) and the five built-ins (all field-bearing) leave
// addComponent's documented raison d'etre -- a tag's addRaw returns nullptr ON SUCCESS too (E13), so
// hasRaw AFTERWARDS is the only correct success signal -- and the model's "(no fields)" branch
// (hasFields == true, fields.empty(), distinct from the E4 case where hasFields == false because the
// type carries no entt::meta at all) both unexercised.
#include <aero/reflect/annotations.hpp>

struct AERO_COMPONENT InspectorTag {};

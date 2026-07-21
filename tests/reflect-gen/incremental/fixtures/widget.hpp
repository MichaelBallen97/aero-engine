#pragma once
#include "aero_reflect.hpp"
#include "widget_types.hpp"

struct AERO_COMPONENT Widget {
    WidgetScalar speed;  // canonicalizes to float THROUGH the transitive header
    int gear;
    bool engaged;
};

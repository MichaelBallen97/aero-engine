# engine/core/ — the zero-dependency foundation layer (docs/03). First engine library.
# Header-only for now (Handle and SlotMap are templates); becomes STATIC when the first core .cpp
# lands (math 0.2.2 / logging 0.2.4). core depends on NOTHING — it is the base of the layer stack.
add_library(aero_core INTERFACE)
add_library(aero::core ALIAS aero_core)

# Public include root: consumers see <aero/core/handle.hpp>, <aero/core/slot_map.hpp> (and the
# existing <aero/core/profiler.hpp>). aero::profiling also exposes this same root; both are INTERFACE
# and CMake de-duplicates the include path, so linking both is harmless.
target_include_directories(aero_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

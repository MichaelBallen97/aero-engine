# engine/reflect — the reflection/serialization runtime (task 1.2.1, ADR-004). First population of the
# docs/03 reflect layer (was a .gitkeep placeholder). Hand-rolled JSON writer + leaf serializers; wraps NO
# third-party type (std-only + engine::Vec3/Quat), so — like engine/render at 0.5.1 — no find_package and no
# boundary guard: the boundary rule is satisfied by construction.
add_library(aero_reflect STATIC
    src/json_writer.cpp
    src/json_value.cpp    # task 1.2.2 — the JsonValue DOM + number conversion
    src/serialize.cpp
)
add_library(aero::reflect ALIAS aero_reflect)

target_include_directories(aero_reflect PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# PUBLIC aero::core: serialize.hpp (a public header) includes <aero/core/math.hpp> for engine::Vec3/Quat.
target_link_libraries(aero_reflect PUBLIC aero::core)

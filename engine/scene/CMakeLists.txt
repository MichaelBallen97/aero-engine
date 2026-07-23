# engine/scene/ — the EnTT-backed world (Epic 1.3, docs/03). The fifth engine layer; depends only on
# core. EnTT is PRIVATE: no entt type appears in a public header (project rule #3), enforced by
# .github/scripts/check-scene-boundary.sh + tests/scene_boundary_probe.cpp (task 1.3.1).
find_package(EnTT CONFIG REQUIRED)   # package EnTT, target EnTT::EnTT (header-only)

add_library(aero_scene STATIC
    src/world.cpp      # the ONE TU besides internal/.../world_access.hpp where EnTT is visible
    src/transform.cpp  # task 1.3.2 — engine::Transform's matrices + the builtin-registration hook
)
add_library(aero::scene ALIAS aero_scene)

# PUBLIC: consumers see <aero/scene/{entity,component,world,scene}.hpp>.
target_include_directories(aero_scene PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# PRIVATE: the internal seam is for aero_scene's own TU (and, via aero::scene_internal, for the few
# targets that author or register component types) — never for ordinary consumers.
target_include_directories(aero_scene PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/internal)

# aero::core PUBLIC — entity.hpp surfaces Handle<Tag>, and world.cpp logs through <aero/core/log.hpp>.
# aero::reflect PUBLIC (task 1.3.2) — transform.hpp, a PUBLIC header, includes
# <aero/reflect/annotations.hpp> for AERO_COMPONENT, so consumers need that include root propagated.
# The edge is downward-legal (docs/03 draws scene ABOVE reflect) and costs the boundary guard nothing:
# aero_reflect wraps NO third-party type (std + engine::Vec3/Quat only) and links no vcpkg package, so
# it propagates ZERO vcpkg include roots and tests/scene_boundary_probe.cpp stays a real guard.
#
# core's own third-party backends are PRIVATE-to-core, so this propagates NO vcpkg header.
#
# EnTT::EnTT and aero::profiling are PRIVATE forever. For a STATIC library CMake propagates a PRIVATE
# link as $<LINK_ONLY:...>: the archive reaches the final binary, but the usage requirements — entt's
# headers AND vcpkg's shared per-triplet include/ root; Tracy's headers — do NOT reach consumers.
# That is exactly what makes tests/scene_boundary_probe.cpp a real compile-time guard (risk R12,
# docs/08-risks.md). Flipping EnTT to PUBLIC would reopen the boundary engine-wide — do not
# "simplify" it. (Same reasoning as engine/platform/ and engine/rhi/.)
target_link_libraries(aero_scene
    PUBLIC  aero::core aero::reflect
    PRIVATE EnTT::EnTT aero::profiling
)

# The INTERNAL registration seam (task 0.4.2's aero_platform_internal precedent). UNLIKE that one,
# this target carries EnTT::EnTT INTERFACE: world_access.hpp genuinely includes <entt/entt.hpp>, so a
# consumer must get entt's include path with it. It is not installed and is outside the
# engine/*/include/* glob the boundary script scans.
# NEVER LINK THIS INTO A BOUNDARY PROBE — it would put entt's include root (and vcpkg's shared root)
# on the probe's compile line and silently reduce tests/scene_boundary_probe.cpp to a no-op.
add_library(aero_scene_internal INTERFACE)
add_library(aero::scene_internal ALIAS aero_scene_internal)
target_include_directories(aero_scene_internal INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/internal)
target_link_libraries(aero_scene_internal INTERFACE aero::scene EnTT::EnTT)

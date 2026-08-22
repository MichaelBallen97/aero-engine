# 3.6.2 deliverable: directional shadows made visible and measurable — a ground plane with a static
# caster group, a MIRRORED cube beside its unmirrored twin, and a SKINNED instance, all under one
# directional light whose ELEVATION sweeps 10-80 degrees on a 20-second triangle wave. Shadow length
# and direction are therefore continuously checkable against the sun. `--no-shadows` runs the
# identical scene with RenderView::shadowsEnabled = false, which is what isolates the depth pass's
# cost: nothing exposes it separately, because it runs inside renderShadowMap. `--resolution N`
# overrides the map size (validation row 8), `--distance D` the light's shadowDistance (row 4's
# region edge), and `--elevation DEG` freezes the sun for a reproducible picture (rows 6 and 9).
# CI builds this (compile-proof, 3 OSes); run it locally for the visual pass
# (editor/validation/3.6.2-directional-shadow-map.md).
# Needs the cooked scene.* AND shadow.* shaders (AERO_SHADER_TOOLS=ON) at runtime — without them this
# compiles a stub main that logs and returns 1 (the phase-0-cube precedent).
#
# aero::assets IS on the link line, unlike phase-3-culling: the skinned instance is the ONLY path into
# the skinned depth pipeline (draw()'s built-in primitive arm ignores a palette entirely), and that
# needs a registered cooked mesh. It is cooked IN MEMORY at startup — no artifact is read and no
# fixture is committed. aero_render has linked PUBLIC aero::assets since 3.4.1, so naming it here
# moves nothing; it is named because this TU includes its headers.
add_executable(aero_sample_phase3_shadows main.cpp)
target_link_libraries(aero_sample_phase3_shadows
    PRIVATE aero::render aero::assets aero::rhi aero::platform aero::core aero::profiling
)
if(AERO_SHADER_TOOLS)
    add_dependencies(aero_sample_phase3_shadows aero_shaders)
    target_compile_definitions(aero_sample_phase3_shadows
        PRIVATE AERO_PHASE3_SHADOWS_ENABLED AERO_SHADERS_DIR="${CMAKE_BINARY_DIR}/shaders")
endif()

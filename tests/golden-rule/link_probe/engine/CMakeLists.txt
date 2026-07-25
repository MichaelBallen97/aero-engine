# Three INTERFACE libs + probe:: aliases, chained in the REAL tree's direction (probe_rhi ->
# probe_platform -> probe_core) so the reverse canary probe_editor_core -> probe_core is meaningful.
if(NOT PROBE_SEED_NO_CONSUMERS)
    # NOTE (plan deviation, minor): CMake script syntax does not support ';' as a statement separator
    # between two command invocations on one physical line (it is a LIST separator inside a single
    # command's arguments, not a command terminator) -- one add_library() call per line.
    add_library(probe_core      INTERFACE)
    add_library(probe::core     ALIAS probe_core)
    add_library(probe_platform  INTERFACE)
    add_library(probe::platform ALIAS probe_platform)
    add_library(probe_rhi       INTERFACE)
    add_library(probe::rhi      ALIAS probe_rhi)
    target_link_libraries(probe_platform INTERFACE probe::core)
    target_link_libraries(probe_rhi      INTERFACE probe::platform)

    # Spec correction C2: seed the DEEPEST target, so probe_platform reaches the editor at distance 2
    # and probe_rhi at distance 3 -- the driver asserts on probe_rhi, a finding only a real traversal
    # can produce. Seeding probe_rhi (the spec's own sketch) would be a one-hop DIRECT edge and would
    # pass even if the walk did nothing.
    if(PROBE_SEED_LINK)
        target_link_libraries(probe_core INTERFACE probe::editor_core)
    endif()
    if(PROBE_SEED_INCLUDE_DIR)
        target_include_directories(probe_core INTERFACE ${CMAKE_SOURCE_DIR}/editor/include)
    endif()
endif()

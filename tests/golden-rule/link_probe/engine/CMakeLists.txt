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

    # (fix, post-review Gap 1) A real, UNSEEDED include directory on the clean fixture, so step 5's
    # resolved-path comparison actually EXECUTES in the negative direction on every configure --
    # including case 1 (clean), which is registered from a WORK_DIR containing a literal `editor` path
    # segment (tests/CMakeLists.txt). Before this line the clean case's three consumers were bare
    # INTERFACE libraries with ZERO include directories, so step 5's foreach(_gr_entry IN LISTS
    # _gr_dirs) loop body never ran at all for the clean case -- a naive substring match on the word
    # "editor" (which step 5 does NOT do; see cmake/golden_rule.cmake) would have passed this fixture
    # not because the comparison is correct but because the comparison never RAN. This is what makes
    # the D10 "not a substring match" claim (docs/10-engineering-log.md, task 2.1.2) a proven fact rather than an
    # untested aspiration. The directory need not exist on disk -- step 5 uses
    # get_filename_component(... ABSOLUTE), never REALPATH, exactly so a seeded/probe path need not
    # resolve to a real file.
    target_include_directories(probe_core INTERFACE "${CMAKE_SOURCE_DIR}/engine/include")

    # Spec correction C2: seed the DEEPEST target, so probe_platform reaches the editor at distance 2
    # and probe_rhi at distance 3 -- the driver asserts on probe_rhi, a finding only a real traversal
    # can produce. Seeding probe_rhi (the spec's own sketch) would be a one-hop DIRECT edge and would
    # pass even if the walk did nothing.
    #
    # (fix, post-review Gap 2) Seeded through a NESTED generator expression,
    # $<$<CONFIG:Debug>:probe::editor_core>, rather than the plain target name -- the exact shape
    # target_link_libraries(aero_core PRIVATE $<$<CONFIG:Debug>:aero::editor_core>) would produce on a
    # real target, and the shape _aero_gr_resolve_entry's old plain-token extraction silently dropped
    # (see cmake/golden_rule.cmake). This also still proves case 2 needs a real traversal (the genex
    # still resolves through probe_core -> probe_platform -> probe_rhi identically to the plain form).
    if(PROBE_SEED_LINK)
        target_link_libraries(probe_core INTERFACE "$<$<CONFIG:Debug>:probe::editor_core>")
    endif()
    if(PROBE_SEED_INCLUDE_DIR)
        target_include_directories(probe_core INTERFACE ${CMAKE_SOURCE_DIR}/editor/include)
    endif()
endif()

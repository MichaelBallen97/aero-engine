if(NOT PROBE_SEED_NO_FORBIDDEN)
    add_library(probe_editor_core INTERFACE)
    add_library(probe::editor_core ALIAS probe_editor_core)
    if(NOT PROBE_SEED_BREAK_CANARY)
        target_link_libraries(probe_editor_core INTERFACE probe::core)   # satisfies the reverse canary
    endif()
endif()

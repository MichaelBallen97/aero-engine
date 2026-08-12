# tools/cooker — task 3.3.1. The third first-party CLI: a source model in, a cooked .aeromesh out.
# See README.md for the frozen contract (argv grammar, exit codes, the artifact rule, and the
# flag-effect asymmetry).
#
# THIS IS THE FIRST /tools BINARY THAT LINKS AN ENGINE TARGET, and it links an EDITOR one too. That is
# legal and is not an exception carved for this task: tools/ is outside the golden rule on BOTH halves
# -- .github/scripts/check-golden-rule.sh's SCAN_ROOTS is ('engine' 'runtime') with its own comment
# "tools/ lives on the far side of the dependency-placement invariant", and aero_assert_golden_rule's
# CONSUMER_DIRS in the root CMakeLists.txt is engine + runtime. aero_cooker is enumerated by neither.
#
# WHY aero::editor_core: the tool needs an ImportedModel, which means it needs importModel(), which
# lives there. Re-implementing a glTF parser inside this tool was rejected outright -- it would be a
# second parser for the format ADR-003 designates canonical, and wrong for the other seven claimed
# extensions on the day it shipped.
#
# ACCEPTED, STATED COST: this drags ImGui, SDL3, fastgltf, ufbx, tinyobjloader and assimp onto the
# link line through aero_editor_core's PRIVATE links (which become $<LINK_ONLY:> on a STATIC library,
# so the archives reach us while their include directories do not). The tool initializes none of them.
# The fix, if it ever bites, is splitting the importer TUs into their own ImGui-free target -- a
# self-contained refactor that changes no consumer. It is not this task's change.
#
# ORDERING: both aero::assets and aero::editor_core are ALIAS targets defined in LATER
# add_subdirectory() calls (the root CMakeLists.txt adds tools/ before engine/, runtime/ and editor/).
# CMake resolves a `::` link item at GENERATE time, not at call time, so this configures clean. If a
# future CMake ever changes that, the fix is one line -- move add_subdirectory(tools/cooker) to the
# root file after add_subdirectory(editor) -- and NOT a reorder of tools/ as a whole, which would
# break cmake/reflect.cmake's configure-time `if(TARGET aero_reflect_gen)` check.
#
# NO GATE, unlike aero_shaderc (AERO_SHADER_TOOLS) and aero_reflect_gen (AERO_REFLECT_TOOLS): this
# tool needs neither libclang nor a shader toolchain, and aero_editor_core builds in both reduced
# configurations. Its ctest cases are therefore registered UNCONDITIONALLY and ctest -N moves in ALL
# THREE configurations -- deliberate, and the first time in this epic that number has moved.
add_executable(aero_cooker src/main.cpp)
target_compile_features(aero_cooker PRIVATE cxx_std_20)
target_link_libraries(aero_cooker PRIVATE aero::assets aero::editor_core)

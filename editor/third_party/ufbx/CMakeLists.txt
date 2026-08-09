# editor/third_party/ufbx/CMakeLists.txt -- task 3.2.2 (D3).
# VENDORED because ufbx is in NO vcpkg registry (measured twice: the pinned baseline has 2851 ports
# and no ufbx; GitHub returns 404 for ports/ufbx and versions/u-/ufbx.json on main). Not a preference.
#
# Deliberately a .c compiled by US, not a vcpkg package: this target inherits the project's
# directory-scope compile options, so ufbx.c IS ASan/UBSan-instrumented in the Debug presets -- the
# single most valuable place in this tree for a sanitizer to be watching, since ufbx parses untrusted
# binary files. A vcpkg build would use vcpkg's own triplet flags and be a blind spot.
# (engine/platform/src/miniaudio_impl.c records the same property for miniaudio.)
#
# The CI lint globs are  clang-tidy: '*.cpp'  /  clang-format: '*.cpp' '*.hpp' '*.h' ...
# -- clang-tidy skips .c automatically; clang-format needs the ':!:editor/third_party/**' pathspec
# added in .github/workflows/ci.yml. Do not lint code we do not own, and do not reformat it: the
# vendored files are byte-identical to upstream v0.23.0 and stay that way (see README.md).
add_library(aero_ufbx STATIC ufbx.c)
add_library(aero::ufbx ALIAS aero_ufbx)

# SYSTEM: ufbx.h is third-party. Its warnings are not ours to fix, and this project builds with
# warnings-as-errors on the lint lane. PUBLIC so the one consumer (aero_editor_core, PRIVATE-linked)
# can #include <ufbx.h>; because the consumer links this PRIVATE, the path reaches nothing further.
target_include_directories(aero_ufbx SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ufbx.c is C11-friendly; ufbx_is_thread_safe() wants <stdatomic.h>, which C11 provides.
set_target_properties(aero_ufbx PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

# NOT defined, deliberately, each with its reason:
#   UFBX_REAL_IS_FLOAT       -- double is ufbx's default and best-tested configuration, and the
#                               axis/unit conversion is exactly where precision is worth having.
#                               Narrowing to float happens once per type in fbx_import.cpp, explicitly.
#   UFBX_ENABLE_ERROR_STACK  -- the panel shows ufbx_error::description; a stack is a debugging aid and
#                               changes ufbx_error's cost on every failure path.
#   UFBX_NO_*                -- no feature trims. A trim that removes something the importer later
#                               needs fails at link time in one lane and not another.

# engine/assets/ — the cooked-asset formats (task 3.3.1). The NINTH engine target and the first new
# subsystem since 1.4.2 (scene_serialize). It holds the versioned .aeromesh container (parse) and the
# mesh cook (produce), both PURE: no disk, no <fstream>, no <filesystem>, no logging, no third party,
# no GPU, no per-OS branch.
#
# WHY HERE AND NOT engine/core: core is dependency-free PRIMITIVES (Handle, Guid, ContentHash, math);
# a versioned container with a validating parser is not a primitive. WHY HERE AND NOT /editor: the
# container's READER is a runtime component by definition -- the task's own goal is "runtime never
# parses glTF, it memory-maps cooked buffers" -- and the golden rule forbids /runtime from ever
# including an editor header, so putting it in /editor buys a guaranteed relocation across the rule's
# own line at Phase 5.
#
# aero::rhi is DELIBERATELY ABSENT and must stay absent. Two reasons, the second fatal:
#   1. it would drag the GPU layer (and SDL_GPU's archive, through $<LINK_ONLY:>) onto the link line
#      of a pure byte-in/byte-out library and of a build-time CLI that never opens a device;
#   2. rhi::VertexFormat's numeric values are NOT a contract -- types.hpp declares fifteen
#      enumerators with implicit values, so a reorder is legal and invisible today. Writing those
#      values into a file makes every previously-cooked artifact silently decode to the wrong formats
#      after such a reorder. assets::CookedVertexFormat is explicitly numbered and frozen forever.
#
# NO find_package. NOT ONE, EVER. aero_assets links no vcpkg package at all, which is what makes its
# PRIVATE links a REAL compile-time boundary here rather than the usual convention-plus-grep (R12:
# vcpkg's shared per-triplet include/ root reaches any target linking any vcpkg package). Adding a
# find_package to this file voids that silently while CI stays green. Do not "simplify" it.
add_library(aero_assets STATIC
    src/cooked_mesh.cpp
    src/mesh_cook.cpp
    src/cooked_texture.cpp   # task 3.3.2 -- the KTX2 subset v1: the writer and the hostile-input reader
    src/bc_block.cpp         # task 3.3.2 -- the two INTEGER block encoders; BC3/BC5 compose them
    src/texture_cook.cpp     # task 3.3.2 -- cookTexture: mip chain, gamma tables, block loop, assembly
    src/cooked_skeleton.cpp  # task 3.5.1 -- the .aeroskel container v1: the hostile-input parser
    src/skeleton_cook.cpp    # task 3.5.1 -- cookSkeleton: closure canonicalization, ordering, emit
    src/cooked_animation.cpp # task 3.5.2 -- the .aeroanim container v1: the hostile-input parser
    src/animation_cook.cpp   # task 3.5.2 -- cookAnimation: triage, canonical order, the one padding site
    src/cooked_audio.cpp     # task 3.7.1 -- the .aerowave container v1: the hostile-input parser
    src/audio_cook.cpp       # task 3.7.1 -- cookAudio: integer validation, derived frameCount, emit
)
add_library(aero::assets ALIAS aero_assets)

target_include_directories(aero_assets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# The engine/render/CMakeLists.txt shape verbatim. aero::core PUBLIC because Vec2/Vec3/Vec4 and Guid
# appear in this target's public headers; aero::profiling PRIVATE so Tracy never reaches a consumer
# ($<LINK_ONLY:> on a STATIC library carries the archive and discards usage requirements).
target_link_libraries(aero_assets
    PUBLIC aero::core
    PRIVATE aero::profiling
)

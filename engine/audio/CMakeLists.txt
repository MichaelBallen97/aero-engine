# engine/audio/ — the audio layer (ADR-006, docs/03). Task 3.7.1 OPENS it with the runtime clip
# resource and its VFS loader; task 3.7.2 adds playback, sources and the listener. The directory has
# existed since 13 July 2026 holding nothing but a .gitkeep, exactly as engine/assets/ did until
# 3.3.1, and that .gitkeep goes in the same commit this file arrives in.
#
# NO find_package. NOT ONE, EVER. aero_audio links no vcpkg package at all, which makes this the
# SECOND target in this tree (after aero_assets) whose PRIVATE links are a REAL compile-time boundary
# rather than the usual convention-plus-grep: R12 says a PRIVATE vcpkg link cannot enforce a header
# boundary, because vcpkg installs every port into ONE shared per-triplet include/ root that lands on
# the compile line of any target linking any vcpkg package. aero_assets links none; this target links
# none either, so a stray `#include <miniaudio.h>` here is a HARD COMPILE ERROR rather than a guard
# finding. ADDING A find_package TO THIS FILE VOIDS THAT SILENTLY WHILE CI STAYS GREEN.
#
# THIS IS A PERMANENT PROPERTY, not an accident of what 3.7.1 needed. Task 3.7.3 is going to be asked
# to guard exactly this, and task 3.7.2 is the task that can most easily void it. The recommended
# shape that keeps it intact: 3.7.2 extends engine/platform's AudioDevice with a callback/mix seam
# expressed in ENGINE types, and engine/audio stays vcpkg-free, holding the graph, the sources, the
# listener and the mixer that fills a buffer of engine-typed samples. That is a RECOMMENDATION, not a
# constraint 3.7.1 imposes -- what 3.7.1 owes 3.7.2 is the OPTION.
#
# The decoders are deliberately absent too: a runtime that can decode an mp3 is a runtime that can be
# handed one, and ADR-008 says it is handed a .pak of cooked artifacts. Every decode in this tree
# lives in /editor.
add_library(aero_audio STATIC
    src/clip.cpp     # task 3.7.1 -- the .aerowave -> AudioClip load, through the VFS
    src/spatial.cpp  # task 3.7.2 -- the pure rolloff + constant-power pan
)
add_library(aero::audio ALIAS aero_audio)

target_include_directories(aero_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# aero::core PUBLIC: clip.hpp names engine::VirtualFileSystem and engine::Guid in its own signatures.
# aero::assets PUBLIC: it names assets::MAX_COOKED_AUDIO_BYTES and its statuses derive from
# parseCookedAudio's. Both edges are DOWNWARD and cycle-free (core -> assets -> audio); engine/assets
# links aero::core and nothing else.
# aero::profiling PRIVATE: $<LINK_ONLY:> on a STATIC library carries the archive and discards usage
# requirements, so Tracy never reaches a consumer.
target_link_libraries(aero_audio
    PUBLIC aero::core aero::assets
    PRIVATE aero::profiling
)

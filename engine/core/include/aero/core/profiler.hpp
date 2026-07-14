#pragma once
// Aero Engine — profiling macro wrappers (task 0.1.5).
// Engine's own profiling API; the Tracy backend is an implementation detail that never crosses this
// boundary (project boundary rule). All macros compile to nothing unless the build enables profiling
// (AERO_ENABLE_PROFILING=ON → aero::profiling defines AERO_PROFILING_ENABLED). Tracy is dev-builds-only.
// Macros are statement-level: use at the top of a braced scope, not after a braceless if/for.

#if defined(AERO_PROFILING_ENABLED)
    #include <tracy/Tracy.hpp>
    #define AERO_PROFILE_ZONE                          ZoneScoped                 // scope, auto-named by function
    #define AERO_PROFILE_ZONE_NAMED(name)              ZoneScopedN(name)          // scope, literal name
    #define AERO_PROFILE_ZONE_NAMED_COLOR(name, color) ZoneScopedNC(name, color)  // name + 0xRRGGBB
    #define AERO_PROFILE_FRAME_MARK                    FrameMark                  // once per frame
    #define AERO_PROFILE_FRAME_MARK_NAMED(name)        FrameMarkNamed(name)
    #define AERO_PROFILE_PLOT(name, value)             TracyPlot(name, value)     // numeric graph
    #define AERO_PROFILE_MESSAGE(text)                 TracyMessageL(text)        // literal message
    #define AERO_PROFILE_SET_THREAD_NAME(name)         tracy::SetThreadName(name)
#else
    #define AERO_PROFILE_ZONE                          ((void)0)
    #define AERO_PROFILE_ZONE_NAMED(name)              ((void)0)
    #define AERO_PROFILE_ZONE_NAMED_COLOR(name, color) ((void)0)
    #define AERO_PROFILE_FRAME_MARK                    ((void)0)
    #define AERO_PROFILE_FRAME_MARK_NAMED(name)        ((void)0)
    #define AERO_PROFILE_PLOT(name, value)             ((void)0)
    #define AERO_PROFILE_MESSAGE(text)                 ((void)0)
    #define AERO_PROFILE_SET_THREAD_NAME(name)         ((void)0)
#endif

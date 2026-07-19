# samples/ — validation apps for each phase (docs/03). Each subdirectory is its owning task's
# deliverable, built by CI as a compile-proof. Task 0.5.2 adds phase-0-cube (the static textured
# cube); 0.5.3 finalizes the phase-0 gate artifact (spin + fps HUD + the 3-OS 60fps validation).
add_subdirectory(phase-0-window)
add_subdirectory(phase-0-clear)  # task 0.5.1 — first pixels
add_subdirectory(phase-0-cube)   # task 0.5.2 — textured cube (phase-0 gate artifact)

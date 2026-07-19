# samples/ — validation apps for each phase (docs/03). Each subdirectory is its owning task's
# deliverable, built by CI as a compile-proof. Task 0.5.2 added phase-0-cube (the static textured
# cube); 0.5.3 finalized the phase-0 gate artifact (spin + fps readout + the macOS 60fps sign-off;
# Windows/Linux pending, see samples/phase-0-cube/VALIDATION.md).
add_subdirectory(phase-0-window)
add_subdirectory(phase-0-clear)  # task 0.5.1 — first pixels
add_subdirectory(phase-0-cube)   # task 0.5.2 — textured cube (phase-0 gate artifact)

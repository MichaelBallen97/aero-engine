# samples/ — validation apps for each phase (docs/03). Each subdirectory is its owning task's
# deliverable, built by CI as a compile-proof. Task 0.5.2 added phase-0-cube (the static textured
# cube); 0.5.3 finalized the phase-0 gate artifact (spin + fps readout + the macOS 60fps sign-off;
# Windows/Linux pending, see samples/phase-0-cube/VALIDATION.md).
add_subdirectory(phase-0-window)
add_subdirectory(phase-0-clear)  # task 0.5.1 — first pixels
add_subdirectory(phase-0-cube)   # task 0.5.2 — textured cube (phase-0 gate artifact)
add_subdirectory(phase-1-scene)  # task 1.4.2 — load & draw a JSON scene (phase-1 gate artifact)
add_subdirectory(phase-3-materials)  # task 3.4.1 — materials + cooked textures
add_subdirectory(phase-3-skinning)   # task 3.5.1 — a bind-pose twin beside a waving twin
add_subdirectory(phase-3-animation)  # task 3.5.2 — a bind-pose reference, a playing copy, a frozen one
add_subdirectory(phase-3-culling)    # task 3.6.1 — the culling A/B rig
add_subdirectory(phase-3-shadows)    # task 3.6.2 — the directional shadow A/B rig
add_subdirectory(phase-3-tonemap)    # task 3.6.3 — the tonemap/gamma A/B rig
# phase-2-editor-scene is DATA, not a sample app, and is deliberately NOT add_subdirectory'd: it is an
# Aero project (project.json + assets/ + scenes/) authored through the editor, with no main.cpp and no
# CMakeLists.txt of its own. The other three samples are compile-proofs of engine code; the Phase 2
# gate artifact is a provenance proof of the EDITOR, and nothing CMake can do would strengthen it —
# the scene's format is already pinned by scene_golden_test.cpp (EG1-EG8) and scene_serialize_test.cpp
# (G1-G10). See samples/phase-2-editor-scene/README.md.

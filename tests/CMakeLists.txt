# tests/ — doctest unit tests (docs/04). Harness created by task 0.1.4.

find_package(doctest CONFIG REQUIRED)

# aero_tests links SDL3::SDL3 EXPLICITLY (not merely transitively via aero::platform) so the
# white-box pump test (platform_event_test.cpp) can #include <SDL3/SDL.h> and push synthetic SDL
# events through the real Context::pollEvent. Tests are OUTSIDE the boundary rule — docs/04 governs
# engine PUBLIC headers, not test TUs. This does not touch the boundary probes below (separate
# targets, each linking only its own engine library).
find_package(SDL3 CONFIG REQUIRED)

# One runner for the whole project: test TUs register TEST_CASEs, test_main.cpp
# only generates main(). This is the repo's first compiled+linked target.
add_executable(aero_tests
    test_main.cpp
    vulkan_stack_pin.cpp  # task 0.5.2 — Linux/LSan-only: pin the Vulkan loader stack mapped (no-op elsewhere)
    sanity_test.cpp
    profiler_test.cpp
    handle_test.cpp
    slot_map_test.cpp
    log_test.cpp
    math_test.cpp
    jobs_test.cpp
    time_test.cpp
    vfs_test.cpp
    guid_test.cpp          # task 3.1.1 — engine::Guid: text codec, ordering, hash, seeded generator
    content_hash_test.cpp  # task 3.1.2 — engine::ContentHash: MurmurHash3 x64_128, text codec, ordering, incremental hasher
    platform_test.cpp
    platform_event_test.cpp
    input_test.cpp        # task 0.3.2 — black-box InputState (no SDL)
    audio_device_test.cpp # task 0.3.3 — black-box AudioDevice, null backend (no hardware)
    rhi_types_test.cpp    # task 0.4.1 — handle/desc/flag vocabulary (compile-time heavy, no GPU)
    rhi_format_test.cpp   # task 0.4.1 — format classification/size/toString (no GPU)
    rhi_device_test.cpp   # task 0.4.2 — tiers 0-1: lifecycle/resources/uploads/frame-flow/render-pass
    rhi_swapchain_test.cpp # task 0.4.2 — tier 2: real visible window, claim/acquire/present
    shader_loader_test.cpp       # task 0.4.4 — tier-0: parseShaderMetadata + selectArtifact (no GPU)
    rhi_shader_pipeline_test.cpp # task 0.4.4 — tier-1 (gated): load → valid pipeline on real GPU
    render_clear_test.cpp # task 0.5.1 — tier-0 config default + tier-2 clear/resize/move/leak
    render_cube_test.cpp   # task 0.5.2 — tier-0 mesh invariants + tier-2 textured-cube draw pipeline
    scene_format_test.cpp # task 1.2.3 — scene format v1: envelope/version/hierarchy + canonical round-trip
    scene_test.cpp        # task 1.3.1 — engine::World over EnTT: entities, components, queries, registration
    transform_test.cpp    # task 1.3.2 — engine::Transform, the World-owned forest, world matrices
    camera_test.cpp       # task 1.3.3 — engine::Camera: defaults/layout/projection/view/registration
    light_test.cpp        # task 1.3.3 — engine::DirectionalLight + PointLight
    scene_render_test.cpp # task 1.4.1 — buildRenderView (tier-0) + gated SceneRenderer GPU draw-smoke
    render_target_test.cpp # task 2.2.3 — tier-0 sizing policy + GPU-gated RenderTarget lifecycle
)

# AC-9(i) (task 0.2.2): aero::core is linked; glm::glm deliberately IS NOT — that is intentional
# style, not a compile-time guarantee. Do not "fix" it by adding glm::glm here. (glm's ARCHIVE
# still reaches the link line via $<LINK_ONLY:> from aero_core's PRIVATE link — that is correct
# and carries no headers.)
#
# This target ALSO links doctest::doctest, which — via vcpkg's shared per-triplet include/
# directory — exports GLM's headers to every TU here regardless of what aero_core links (verified:
# a probe TU with this target's exact flags plus `#include <glm/vec3.hpp>` compiles clean). So a
# public math header pulling GLM in would NOT necessarily fail to compile here — this target does
# NOT prove the compile-time boundary the way an engine-layer target linking only aero::core does
# (verified with exactly that probe). See docs/02-adrs.md's ADR-005 implementation note and risk
# R12 in docs/08-risks.md; .github/scripts/check-math-boundary.sh (task 0.2.3, run by the `lint`
# job) is the real enforcement for tests/. The sibling aero_math_boundary_probe target below (also
# 0.2.3) covers the public math headers at compile time instead.
# ${CMAKE_DL_LIBS} (libdl on Linux, empty elsewhere): vulkan_stack_pin.cpp calls dlopen/dlerror
# directly to keep the Vulkan driver stack mapped for LSan (task 0.5.2).
#
# aero::scene_internal is linked here (and ONLY here, besides its own target) because scene_test.cpp
# calls registerComponent<T>() through the internal seam — tests sit OUTSIDE the boundary rule (the
# note above, :5-9). The compile-time guard for the scene/EnTT boundary lives in the separate
# aero_scene_boundary_probe target below, which links aero::scene ALONE.
#
# task 2.2.3: aero::rhi_internal joins this line for the SAME reason aero::scene_internal is here
# (above) -- rhi_swapchain_test.cpp exercises the internal native-handle seam
# (rhi::internal::NativeDeviceAccessor::texture, incl. its refusal to hand out a WRITE-ONLY
# swapchain-acquired texture), and tests sit OUTSIDE the boundary rule. It is a header-only
# INTERFACE target carrying ONE include directory and no library, no vcpkg root and no SDL, so it
# cannot weaken any guard. The compile-time guard for rhi/SDL_GPU is aero_rhi_boundary_probe, which
# links aero::rhi ALONE and is UNTOUCHED by this line -- and engine/rhi/CMakeLists.txt:43's "NEVER
# link it into a boundary probe" is about exactly those probe targets, not this runner (§O-1).
target_link_libraries(aero_tests PRIVATE doctest::doctest aero::profiling aero::core aero::platform aero::reflect aero::render aero::rhi aero::rhi_internal aero::scene aero::scene_internal aero::scene_render SDL3::SDL3 ${CMAKE_DL_LIBS})

# Task 0.4.4: the shader-loading tests need the compiled artifacts (aero_add_shaders -> aero_shaders,
# in build/<preset>/shaders). aero_shaders exists ONLY when AERO_SHADER_TOOLS is ON (cmake/shaders.cmake),
# and tests/ is add_subdirectory'd AFTER shaders/ (root CMakeLists.txt), so the target is in scope here.
# INVARIANT: AERO_SHADERS_DIR must always equal aero_add_shaders' OUTPUT_DIR (default
# ${CMAKE_BINARY_DIR}/shaders, used unchanged by shaders/CMakeLists.txt) -- move both together.
if(AERO_SHADER_TOOLS)
    add_dependencies(aero_tests aero_shaders)  # artifacts built before ctest runs
    target_compile_definitions(aero_tests PRIVATE
        AERO_SHADER_TOOLS_ENABLED=1
        AERO_SHADERS_DIR="${CMAKE_BINARY_DIR}/shaders")  # MUST match aero_add_shaders' default OUTPUT_DIR (F9)
endif()

# Single ctest entry; doctest prints per-TEST_CASE detail on failure. Per-case
# registration (doctest_discover_tests) can be adopted later without churn.
add_test(NAME aero_tests COMMAND aero_tests)

# ---------------------------------------------------------------------------------------------
# task 2.1.1 — the editor's ImGui host smoke test. Standalone, gated at RUNTIME by AERO_REQUIRE_GPU
# (rhi_test_support.hpp). NOT inside if(AERO_REFLECT_TOOLS)/if(AERO_SHADER_TOOLS): the editor depends
# on neither (AC-9). ImGui-free at source — drives editor_core's engine-typed API; imgui/SDL3 reach it
# only transitively through editor_core's PRIVATE static archive. vulkan_stack_pin.cpp + ${CMAKE_DL_LIBS}
# give the lavapipe LSan pin on the Linux Debug lane (F9), like every GPU-drawing test.
add_executable(aero_editor_imgui_test
    editor/imgui_layer_test.cpp
    vulkan_stack_pin.cpp)
target_link_libraries(aero_editor_imgui_test PRIVATE
    doctest::doctest aero::editor_core aero::rhi aero::platform aero::core ${CMAKE_DL_LIBS})
target_include_directories(aero_editor_imgui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})  # tests/ for rhi_test_support.hpp
# code-review finding 4 (I30): the panel's own Refresh-button flag cannot be driven through a real
# frame here (this target is ImGui-free at source and cannot synthesize a widget click -- the
# AC-27/FileDialogHost precedent, the same reason PU1 in project_test.cpp reads project_ui.cpp's own
# source text). I30 reads editor_app.cpp's own source text the identical way -- the
# AERO_PROJECT_FIXTURES_DIR/AERO_EDITOR_SRC_DIR precedent, extended to this target for the first time.
target_compile_definitions(aero_editor_imgui_test PRIVATE
    AERO_EDITOR_SRC_DIR="${CMAKE_SOURCE_DIR}/editor/src")
add_test(NAME aero_editor_imgui_test COMMAND aero_editor_imgui_test)

# ---------------------------------------------------------------------------------------------
# task 2.1.3 — the editor shell's tier-0 tests: panel registry, pacing policy, panel defaults. NO
# GPU, no window, no ImGui context — so it is NOT gated by AERO_REQUIRE_GPU and needs neither
# vulkan_stack_pin.cpp nor ${CMAKE_DL_LIBS} (nothing here loads Vulkan). Like aero_editor_imgui_test
# above, it lives OUTSIDE every AERO_*_TOOLS gate: the editor depends on neither codegen nor shaders.
#
# THIS IS NOT A BOUNDARY PROBE, AND MUST NEVER BE MISTAKEN FOR ONE. The TU never includes an ImGui
# header — that is hygiene (2.1.3 D9), not enforcement: this target links doctest::doctest, which
# puts vcpkg's shared per-triplet include/ root on the compile line, and imgui.h lives FLAT in it,
# so a leaked <imgui.h> in a public editor header WOULD still compile here (risk R12, docs/08 — the
# same limitation documented for GLM and aero_tests at :51-59 above; verified by seeding exactly
# that during 2.1.3 and reverting). The "public editor headers stay ImGui-free" rule is held by
# review and by FILE PLACEMENT (every ImGui entry point lives in editor/src/, not editor/include/).
# 2.1.3's D12 was re-examined against this fact and DELIBERATELY RE-AFFIRMED: no editor-side probe
# target ships, because project rule #3 governs the ENGINE's public API and the editor is on its far
# side by design. Do not "fix" this by adding link libraries here or a probe target elsewhere.
#
# task 2.2.1: this target gains editor/hierarchy_test.cpp, plus aero::scene and aero::scene_internal
# on its link line. aero::scene_internal is here for the SAME reason aero_tests links it (:63-66):
# hierarchy_test.cpp registers a TEST-LOCAL component through
# engine::scene::internal::registerComponent<T> to prove duplicate is generic over the registration
# table (AC-13/E12) -- a component no line of editor code has ever heard of. Tests sit OUTSIDE the
# boundary rule; the compile-time guard for scene/EnTT is aero_scene_boundary_probe, which links
# aero::scene ALONE and is untouched by this line. The target stays TIER-0: aero_scene_internal is a
# header-only INTERFACE target and nothing here builds a GPU, a window or an ImGui context (AC-19).
#
# task 2.2.4: this target gains editor/project_files_test.cpp -- tier-0 still holds, and
# std::filesystem needs no link library (the engine/core precedent), so the link line is unchanged.
#
# task 2.2.5: console_model_test.cpp runs std::thread (the LogSink concurrency and R14 stress arms).
# This target links no threading library today, and the tree's ONLY other find_package(Threads) sits
# inside engine/platform/CMakeLists.txt's elseif(UNIX) branch -- so on macOS and Windows there is no
# Threads::Threads anywhere to propagate. Relying on $<LINK_ONLY:> through two static archives is the
# kind of thing that is green on macOS and red on one CI lane. One explicit link removes the question.
# This target is NOT a boundary probe, so R12's "exactly one aero:: entry" rule does not apply to it.
#
# task 2.3.1 joins this target with editor/editor_camera_test.cpp (the gesture matrix in step 2, then
# the EditorCamera model in step 4) and, in step 3, editor/scene_bounds_test.cpp. The target stays
# TIER-0: no GPU, no window, no ImGui context, so it must pass identically with AERO_REQUIRE_GPU unset
# and set. target_link_libraries below is UNCHANGED (AC-22) -- the new TUs need only
# aero::core/aero::scene/aero::scene_internal, all already on this line.
#
# task 2.3.2 joins this target with editor/picking_test.cpp (step 1: the screen mapping, the basis ray
# and the local-box slab test; step 2: the world pick walk and the click-decision table) and, in
# step 3, editor/selection_overlay_test.cpp. The target stays TIER-0: no GPU, no window, no ImGui
# context, so it must pass identically with AERO_REQUIRE_GPU unset and set. target_link_libraries
# below is UNCHANGED (AC-21) and no add_test is registered (ctest -N stays 94/5, AC-22).
#
# task 2.3.3 joins this target with editor/gizmo_test.cpp (step 2: the tool-state model; step 3: the
# geometry and the write pipeline) and, in step 4, editor/transform_ops_test.cpp. The target stays
# TIER-0: no GPU, no window, no ImGui context, so it must pass identically with AERO_REQUIRE_GPU
# unset and set. NEITHER new TU includes <ImGuizmo.h> -- the pure layer names no ImGuizmo type, which
# is exactly what makes this whole task tier-0 testable. target_link_libraries below is UNCHANGED
# (AC-20) and no add_test is registered (ctest -N stays 94/5).
#
# task 2.4.1 joins this target with editor/command_stack_test.cpp (step 1: the Command interface and the
# stack's five algorithms) and, in step 2, editor/transform_command_test.cpp. The target stays TIER-0: no
# GPU, no window, no ImGui context, so it must pass identically with AERO_REQUIRE_GPU unset and set.
# Neither new TU names an ImGui, ImGuizmo or entt type -- the whole command model is pure, which is what
# makes it tier-0 testable at all. The link line below is UNCHANGED (AC-23) and no new ctest entry is
# registered (ctest -N stays 94/5/18, AC-25).
#
# task 2.4.2 joins this target with editor/scene_snapshot_test.cpp (step 4: the detached-subtree payload
# and its two-phase restore) and, in steps 5-6, editor/structural_commands_test.cpp. Both stay TIER-0: no
# GPU, no window, no ImGui context, so they must pass identically with AERO_REQUIRE_GPU unset and set --
# and, crucially, they need NO REFLECTION (component_ops' addComponent/removeComponent and the whole
# snapshot facility are meta-free), which is what makes AC-8's "structural undo works with
# -DAERO_REFLECT_TOOLS=OFF" a tested fact rather than a claim. No new ctest entry is registered (the
# link line below is unchanged; ctest -N stays 94/5/18).
#
# task 2.5.1 joins this target with editor/scene_session_test.cpp (steps 1-2: the document model, the
# pure guard state machine, THE scene swap and the atomic file I/O; step 5: the whole file-flow
# transition table, driven with a NULL dialog channel) and, in steps 3 and 5, the CONDITIONAL
# editor/scene_io_test.cpp below. scene_session_test.cpp is UNGATED and must build and pass in both
# tools-OFF configurations -- that is AC-6/E21's proof and the reason the two TUs are split. Both stay
# TIER-0: no GPU, no window, no ImGui context, so they must pass identically with AERO_REQUIRE_GPU
# unset and set. The link line below is unchanged and no new ctest entry is registered (ctest -N stays
# 94/5/18, AC-35).
#
# task 2.6.1 joins this target with editor/project_test.cpp -- the project format, its validators,
# the filesystem half, the flow, and the three golden fixtures. UNCONDITIONAL, unlike
# scene_io_test.cpp and scene_golden_test.cpp below: the project flow reads and writes no scene, so
# it needs no serialization at all (spec D4/AC-35). It must be PRESENT in the reflect-OFF build, not
# skipped -- prove it with --list-test-cases. Tier-0: no GPU, no window, no ImGui context. The link
# line is UNCHANGED and no new ctest entry is registered (ctest -N stays 94/5/18, AC-39).
#
# task 2.6.2 joins this target with editor/project_settings_test.cpp -- the settings panel's pure
# model. UNCONDITIONAL, for the same reason project_test.cpp is: project_settings.hpp depends on
# project.hpp and nothing else, so it inherits that header's gate-free property (spec D4/AC-14). It
# must be PRESENT in the reflect-OFF build, not skipped -- prove it with --list-test-cases. Tier-0:
# no GPU, no window, no ImGui context. The link line is UNCHANGED and no new ctest entry is
# registered (ctest -N stays 95/6/19, AC-33).
find_package(Threads REQUIRED)
# task 3.1.1 joins this target with editor/asset_meta_test.cpp (the .meta v1 format + the pure
# planner, cases AM1-AM27/AP1-AP14/AG1-AG5) and, in Step 3, editor/asset_database_test.cpp (the walk,
# cases AD1-AD30 + AG6). UNCONDITIONAL, for the same reason project_test.cpp/project_settings_test.cpp
# are: neither TU depends on reflection, so both inherit that gate-free property (D4/AC-17/INV-P5).
# Both must be PRESENT in the reflect-OFF build, not skipped -- prove it with --list-test-cases.
# Tier-0: no GPU, no window, no ImGui context. The link line is UNCHANGED and no new ctest entry is
# registered (ctest -N stays 95/6/19, AC-46).
#
# task 3.1.2 joins this target with editor/text_file_test.cpp -- text_file.cpp's three PRE-EXISTING
# primitives (readTextFile/writeTextFileAtomic/fileExists) have never had a TU of their own (plan A1);
# this is the honest home for them AND for the two new streaming primitives this task ships
# (hashFileContents/ensureDirectory). UNCONDITIONAL, for the same reason project_test.cpp is:
# text_file.hpp depends on neither reflection nor entt, so it must be PRESENT in the reflect-OFF
# build, not skipped -- prove it with --list-test-cases. Tier-0: no GPU, no window, no ImGui context.
# The link line is UNCHANGED and no new ctest entry is registered.
#
# task 3.1.2, Step 4 joins this target with editor/asset_cache_test.cpp -- the import cache index v1
# format (parseAssetCache/writeAssetCacheText/importChangeLabel/AssetCacheIndex::find, cases
# IC1-IC34/IG1-IG5). UNCONDITIONAL, for the same reason asset_meta_test.cpp is (D4/AC-17/INV-P5):
# asset_cache.hpp depends on nothing but content_hash.hpp/guid.hpp, so it must be PRESENT in the
# reflect-OFF build, not skipped -- prove it with --list-test-cases, which must show a DIFFERENT
# total in the two reduced configurations than it did before this step, not the same one twice.
# Tier-0. The link line is UNCHANGED and no new ctest entry is registered (ctest -N stays 95/6/19).
#
# task 3.1.3 joins this target with editor/asset_view_test.cpp (Step 1: kinds/icons/filter/search/grid
# math, cases AV1-AV42), editor/thumbnail_cache_test.cpp (Step 4: the ledger/budget/LRU/resampler,
# cases TC1-TC38) and editor/asset_actions_test.cpp (Step 9: the orphan-delete planner + action,
# cases AA1-AA22) -- all three UNCONDITIONAL, for the identical reason asset_meta_test.cpp is: none
# depends on reflection or entt, so all three must be PRESENT in both reduced configurations, not
# skipped -- prove it with --list-test-cases. Tier-0. The link line is UNCHANGED and no new ctest
# entry is registered (ctest -N stays 95/6/19, §V1).
#
# task 3.1.4 joins this target with editor/asset_watcher_test.cpp (Step 2: the pure snapshot algebra
# -- find/isSettled/diffSnapshots/applyChanges; Step 3: the AssetWatcher class itself, against a real
# scratch TempDir with an INJECTED clock -- cases AW1-AW46). UNCONDITIONAL, for the identical reason
# asset_meta_test.cpp is (D4/AC-17/INV-P5): asset_watcher.hpp depends only on asset_meta.hpp and
# project_files.hpp, neither of which needs reflection, so it must be PRESENT in both reduced
# configurations, not skipped -- prove it with --list-test-cases, which must show a DIFFERENT total in
# the two reduced configurations than it did before this step, not the same one twice. Tier-0: no GPU,
# no window, no ImGui context, and NO TEST SLEEPS. The link line is UNCHANGED and no new ctest entry
# is registered (ctest -N stays 95/6/19).
#
# task 3.2.1 joins this target with editor/model_import_test.cpp (Step 2 onward: the canonical model,
# the pure URI policy -- classifyUri/normalizeRelativePath/isImportableModelName -- and the glTF
# backend, cases MI1-MI93 plus MI40b) and, from Step 9, editor/model_import_session_test.cpp.
# UNCONDITIONAL, for the identical reason asset_meta_test.cpp is: model_import.hpp depends only on
# aero/core/{guid,math}.hpp, aero/editor/import_settings.hpp and aero/editor/scene_bounds.hpp -- the
# last of those reaches aero::scene, which is a PUBLIC, UNGATED dependency of aero_editor_core (only
# engine/scene_serialize is gated on AERO_REFLECT_TOOLS) -- so it must be PRESENT in both reduced
# configurations, not skipped -- prove it with --list-test-cases. Tier-0: no GPU, no window, no ImGui
# context, no sleeps, and no disk on the critical path -- every importer case is driven from a string
# literal or a committed text fixture reached through AERO_ASSET_FIXTURES_DIR, which is ALREADY
# defined on this target. The link line is UNCHANGED and no new ctest entry is registered (ctest -N
# stays 95/6/19), and NEITHER TU NAMES fastgltf.
#
# task 3.2.2 joins this target with ONE new TU. UNCONDITIONAL, for the identical reason
# model_import_test.cpp is: model_import.hpp depends only on aero/core/{guid,math}.hpp,
# aero/editor/import_settings.hpp and aero/editor/scene_bounds.hpp; the last reaches aero::scene, a
# PUBLIC, UNGATED dependency of aero_editor_core (only engine/scene_serialize is gated on
# AERO_REFLECT_TOOLS). This TU must therefore be PRESENT in BOTH reduced configurations, not skipped --
# prove it with --list-test-cases, which must show the SAME total in the two and FI1 present in both
# (AC-68). Tier-0: no GPU, no window, no ImGui context, no sleeps, and no disk on the critical path --
# every case is driven from an ASCII-FBX STRING LITERAL. The one binary fixture is reached through
# AERO_ASSET_FIXTURES_DIR, ALREADY defined on this target. The link line is UNCHANGED, no new ctest
# entry is registered (ctest -N stays 95/6/19), and THIS TU NAMES NO ufbx TYPE -- it drives importFbx
# through the public importModel() only.
add_executable(aero_editor_shell_test editor/shell_test.cpp editor/hierarchy_test.cpp
               editor/project_files_test.cpp editor/console_model_test.cpp
               editor/editor_camera_test.cpp editor/scene_bounds_test.cpp
               editor/picking_test.cpp editor/selection_overlay_test.cpp
               editor/gizmo_test.cpp editor/transform_ops_test.cpp
               editor/command_stack_test.cpp editor/transform_command_test.cpp
               editor/scene_snapshot_test.cpp editor/structural_commands_test.cpp
               editor/scene_session_test.cpp editor/project_test.cpp
               editor/project_settings_test.cpp
               editor/asset_meta_test.cpp editor/asset_database_test.cpp
               editor/text_file_test.cpp editor/asset_cache_test.cpp
               editor/asset_view_test.cpp editor/thumbnail_cache_test.cpp
               editor/asset_actions_test.cpp editor/asset_watcher_test.cpp
               editor/model_import_test.cpp editor/model_import_session_test.cpp
               editor/fbx_import_test.cpp)
target_link_libraries(aero_editor_shell_test PRIVATE
    doctest::doctest aero::editor_core aero::scene aero::scene_internal aero::core Threads::Threads)
add_test(NAME aero_editor_shell_test COMMAND aero_editor_shell_test)

# task 2.6.1: the three committed project fixtures reach the UNGATED project_test.cpp. Not optional
# and not #ifdef-guarded (2.5.2's D8 rule, reused): a missing definition is a COMPILE error and a
# missing fixture file is a REQUIRE failure. Neither can silently skip, which is the same failure
# mode as a regeneration flag. NO AERO_PROJECT_*_OUT_DIR is defined -- the project battery uses
# describeMismatch's context window (which, for a 152-byte manifest, IS the whole file) and never
# dumpActual, so nothing here writes anywhere and `ctest -j` has no collision surface at all.
target_compile_definitions(aero_editor_shell_test PRIVATE
    AERO_PROJECT_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/projects"
    # code review (BLOCKING-1): project_ui.cpp's ImGui::CloseCurrentPopup() bookkeeping cannot be
    # driven from any test tier -- it needs a live ImGui context and a synthesized widget click, which
    # this codebase has never had (the AC-27/FileDialogHost precedent). project_test.cpp's PU1 case
    # reads the file's OWN SOURCE TEXT as a mechanical proof instead -- the same textual-grep principle
    # the five CI architecture guards already use, just run as a doctest case rather than a lint-job
    # script. A path, not a flag: a missing file is a REQUIRE failure, never a silent skip.
    AERO_EDITOR_SRC_DIR="${CMAKE_SOURCE_DIR}/editor/src"
    # task 3.1.1: the two committed asset-meta fixtures reach the UNGATED asset_meta_test.cpp. Same
    # rule as AERO_PROJECT_FIXTURES_DIR above (2.5.2's D8, third application): not optional, not
    # #ifdef-guarded. AG1-AG5 use describeMismatch's context window and never dumpActual, so nothing
    # here writes anywhere and `ctest -j` has no collision surface at all.
    AERO_ASSET_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/assets")
# MOVED OUT of the if(AERO_REFLECT_TOOLS) block below, where task 2.5.2 put it: project_test.cpp is
# UNGATED and includes tests/scene_golden_support.hpp, so this include root must exist in EVERY
# configuration. The aero_editor_imgui_test precedent is at :106.
target_include_directories(aero_editor_shell_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# task 2.5.1, step 3: the round-trip cases need the engine's generated component serializers, which
# do not EXIST with the tool off (F9). The TARGET stays ungated -- only this ONE TU is conditional --
# so the reflect-OFF configuration still builds and runs every serialization-free case in
# scene_session_test.cpp above. This TU names no serialization type directly: everything it calls
# lives in aero_editor_core, so the link line and the include directories above are both unchanged,
# and no new ctest entry is registered (ctest -N stays 94/5/18, AC-35).
if(AERO_REFLECT_TOOLS)
    target_sources(aero_editor_shell_test PRIVATE editor/scene_io_test.cpp)
    # task 2.5.2: the EDITOR half of the golden battery -- the same three fixtures driven through
    # 2.5.1's real openSceneFile/saveSceneFile disk path. Same gate as scene_io_test.cpp above and for
    # the same reason (2.5.1 F9): the generated component serializers do not EXIST with the tool off,
    # so this TU is ABSENT from that build rather than skipped -- prove it with --list-test-cases,
    # which must read 280 there and 304 here, not 304 in both.
    #
    # Like scene_io_test.cpp this TU names no engine serialization type directly: every symbol it
    # calls lives in aero_editor_core. So the LINK LINE above is unchanged; the only additions are the
    # two fixture-locating definitions and the tests/ include root for scene_golden_support.hpp (the
    # aero_editor_imgui_test precedent at :106). AERO_GOLDEN_OUT_DIR differs from the engine
    # battery's on purpose -- `ctest -j` runs both targets concurrently (spec D13). No new ctest
    # entry is registered here: ctest -N stays 94/5/18.
    target_sources(aero_editor_shell_test PRIVATE editor/scene_golden_test.cpp)
    target_compile_definitions(aero_editor_shell_test PRIVATE
        AERO_GOLDEN_SCENES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/scenes"
        AERO_GOLDEN_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/golden/editor")
endif()

# ---------------------------------------------------------------------------------------------
# The math-boundary COMPILE-TIME guard (task 0.2.3; docs/02-adrs.md:211).
#
# OBJECT, not an executable: compiling is the whole assertion, so there is nothing to link or run
# and no ctest entry to register. It builds as part of `all`, so every preset on every CI lane
# enforces it for free.
#
# THE LINK LINE IS THE TEST. aero::core links glm::glm PRIVATE, so a target that links ONLY
# aero::core never sees vcpkg's shared per-triplet include/ directory and cannot resolve
# <glm/...>. That is exactly what makes a leaked GLM include in a PUBLIC math header a compile
# error here (verified 2026-07-15: with GLM installed in the shared root, this flag set still
# reports `fatal error: 'glm/vec3.hpp' file not found`, while aero_tests' flag set compiles it
# clean — risk R12, docs/08-risks.md).
#
# DO NOT ADD ANY OTHER LINK LIBRARY. Not doctest::doctest, not aero::profiling (it links Tracy —
# a vcpkg package — in Release), not glm::glm. Any one of them puts the shared vcpkg include root
# back on this compile line and silently reduces this guard to a no-op WITHOUT turning CI red.
# That is the only way this guard can rot, so it is the one thing to never "simplify".
add_library(aero_math_boundary_probe OBJECT math_boundary_probe.cpp)
target_link_libraries(aero_math_boundary_probe PRIVATE aero::core)

# ---------------------------------------------------------------------------------------------
# The jobs-boundary COMPILE-TIME guard (task 0.2.5; docs/04 boundary rule).
#
# Same mechanism as aero_math_boundary_probe above, for a different backend: aero::core links
# enkiTS::enkiTS PRIVATE, so a target linking ONLY aero::core never sees vcpkg's shared per-triplet
# include/ directory and cannot resolve <TaskScheduler.h>. A leaked enkiTS include in a public header
# is therefore a compile error the moment it is written.
#
# A separate target from the math probe on purpose: each guard names its own backend, so a failure
# says which boundary broke. A future task may consolidate both into one aero_core_boundary_probe as
# 0.2.6 adds time/VFS headers — do NOT do that here; it is churn on a guard that just landed.
#
# DO NOT ADD ANY OTHER LINK LIBRARY — not doctest, not aero::profiling, not enkiTS::enkiTS. Any of
# them restores the shared vcpkg include root and silently reduces this guard to a no-op while CI
# stays green.
add_library(aero_jobs_boundary_probe OBJECT jobs_boundary_probe.cpp)
target_link_libraries(aero_jobs_boundary_probe PRIVATE aero::core)

# ---------------------------------------------------------------------------------------------
# The platform SDL-boundary COMPILE-TIME guard (task 0.3.1; docs/04 boundary rule).
#
# Same mechanism as the math/jobs probes, for SDL: aero::platform links SDL3 PRIVATE, so a target
# linking ONLY aero::platform never sees vcpkg's shared per-triplet include/ directory and cannot
# resolve <SDL3/...>. A leaked SDL include in a public platform header is therefore a compile error
# the moment it is written. (This CORRECTS the 0.2.5 handoff note that platform "must be greps":
# the probe works here exactly as for glm — spec D8.)
#
# DO NOT ADD ANY OTHER LINK LIBRARY — not doctest, not SDL3::SDL3, not aero::profiling. Any of them
# restores the shared vcpkg include root and silently reduces this guard to a no-op while CI stays
# green. That is the only way this guard can rot.
add_library(aero_platform_boundary_probe OBJECT platform_boundary_probe.cpp)
target_link_libraries(aero_platform_boundary_probe PRIVATE aero::platform)

# ---------------------------------------------------------------------------------------------
# The RHI SDL_GPU-boundary COMPILE-TIME guard (task 0.4.5; ADR-002 "the sacred wrapper").
#
# Same mechanism as the math/jobs/platform probes, for SDL_GPU: aero::rhi links SDL3 PRIVATE (and its
# PUBLIC deps aero::core/aero::platform propagate no vcpkg header), so a target linking ONLY aero::rhi
# never sees vcpkg's shared per-triplet include/ directory and cannot resolve <SDL3/SDL_gpu.h>. A
# leaked SDL include in a public rhi header is therefore a compile error the moment it is written
# (verified 2026-07-19: rhi.hpp's whole surface compiles with only aero::rhi/aero::core includes,
# while a seeded <SDL3/SDL_gpu.h> reports 'file not found' -- risk R12, docs/08-risks.md).
#
# The textual grep guard .github/scripts/check-rhi-boundary.sh (also 0.4.5, run by the `lint` job) is
# the enforcement that reaches tests/ and every engine/runtime .cpp, where R12 breaks this compile
# boundary. This probe holds the public-header half.
#
# DO NOT ADD ANY OTHER LINK LIBRARY -- not doctest, not SDL3::SDL3, not aero::profiling (Tracy, a
# vcpkg package, in Release). Any of them restores the shared vcpkg include root and silently reduces
# this guard to a no-op while CI stays green. That is the only way this guard can rot.
add_library(aero_rhi_boundary_probe OBJECT rhi_boundary_probe.cpp)
target_link_libraries(aero_rhi_boundary_probe PRIVATE aero::rhi)

# ---------------------------------------------------------------------------------------------
# The scene EnTT-boundary COMPILE-TIME guard (task 1.3.1; project rule #3).
#
# Same mechanism as the math/jobs/platform/rhi probes, for EnTT: aero::scene links EnTT::EnTT PRIVATE
# (and its only PUBLIC dep, aero::core, propagates no vcpkg header), so a target that links ONLY
# aero::scene never sees vcpkg's shared per-triplet include/ directory and cannot resolve
# <entt/entt.hpp>. A leaked entt include in a public scene header is therefore a compile error the
# moment it is written. EnTT is the FIFTH private flat-root backend this mechanism bites for (after
# glm, SDL3, miniaudio, SDL_GPU) -- risk R12, docs/08-risks.md.
#
# The textual guard .github/scripts/check-scene-boundary.sh (also 1.3.1, run by the `lint` job) is the
# enforcement that reaches every public engine header; this probe holds the compile-time half.
#
# DO NOT ADD ANY OTHER LINK LIBRARY -- not doctest, not aero::profiling (Tracy, a vcpkg package, in
# Release), not EnTT::EnTT, and ABOVE ALL not aero::scene_internal, which carries EnTT::EnTT INTERFACE
# BY DESIGN. Any of them restores the shared vcpkg include root and silently reduces this guard to a
# no-op while CI stays green. That is the only way this guard can rot.
add_library(aero_scene_boundary_probe OBJECT scene_boundary_probe.cpp)
target_link_libraries(aero_scene_boundary_probe PRIVATE aero::scene)

# ---------------------------------------------------------------------------------------------
# The shaderc end-to-end CTest suite (task 0.4.3; spec D9/§3.6) -- a net-new harness (no prior
# cmake -P / external-binary add_test precedent in this repo). Each case runs the REAL aero_shaderc
# binary through tests/shaderc/run_case.cmake, which is what actually asserts exit codes -- several
# cases are NEGATIVE (the tool is expected to exit non-zero), and a raw `add_test(COMMAND
# aero_shaderc ...)` would treat any non-zero exit as a ctest FAILURE regardless of whether that was
# the expected behavior (see run_case.cmake's own header comment).
#
# No doctest TU for the tool, and no tool code links into aero_tests (spec D9) -- a CLI's honest test
# is its process boundary: argv in, exit code + files out.
#
# Gated on AERO_SHADER_TOOLS: when OFF, aero_shaderc is never built and these tests must not be
# registered (spec E18) -- the ctest count visibly shrinking is deliberate, not a bug.
if(AERO_SHADER_TOOLS)
    set(_aero_shaderc_cases
        help vert_happy frag_happy reflect_ubo subset_msl include_dir defines
        bad_syntax missing_input bad_stage unknown_flag double_include determinism
    )
    foreach(_aero_case IN LISTS _aero_shaderc_cases)
        add_test(
            NAME shaderc.${_aero_case}
            COMMAND "${CMAKE_COMMAND}"
                    -DTOOL=$<TARGET_FILE:aero_shaderc>
                    -DCASE=${_aero_case}
                    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/shaderc/${_aero_case}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/shaderc/run_case.cmake
        )
    endforeach()
    unset(_aero_shaderc_cases)
endif()

# ---------------------------------------------------------------------------------------------
# The reflect-gen end-to-end CTest suite (task 1.1.1; plan/spec D9/§3.6) -- mirrors the shaderc
# harness immediately above: each case runs the REAL aero_reflect_gen binary through
# tests/reflect-gen/run_case.cmake, which is what actually asserts exit codes -- several cases are
# NEGATIVE (bad_syntax=2, missing_input=3, unknown_flag/double_input=1), and a raw
# `add_test(COMMAND aero_reflect_gen ...)` would treat any non-zero exit as a ctest FAILURE
# regardless of whether that was the expected behavior (see run_case.cmake's own header comment).
#
# No doctest TU for the tool, and no tool code links into aero_tests (spec D9) -- a CLI's honest test
# is its process boundary: argv in, exit code + stdout/stderr out.
#
# Gated on AERO_REFLECT_TOOLS: when OFF, aero_reflect_gen is never built and these tests must not be
# registered -- the ctest count visibly shrinking is deliberate (AC-10), not a bug.
if(AERO_REFLECT_TOOLS)
    # AERO_REFLECT_CLANG_ARGS (the per-OS libclang parse flags) and AERO_LLVM_ROOT are now owned by
    # cmake/reflect.cmake (task 1.1.4, D2), included at ROOT scope before add_subdirectory(tests) runs
    # — both are already set here with no local computation needed.

    set(_aero_reflect_cases
        help version walk_plain parse_engine_header engine_types all_flag
        annotation_visible bad_syntax missing_input no_input unknown_flag double_input determinism
        components_basic components_categories components_unsupported components_multi
        components_none components_tag components_determinism
        components_bad_syntax components_missing_input
        emit_basic emit_output_file emit_unsupported emit_multi emit_tag
        emit_none emit_determinism emit_bad_syntax emit_missing_input
        depfile_basic depfile_requires_output depfile_requires_emit
        depfile_parse_fail depfile_space_escape depfile_determinism
        depfile_unwritable
        json_basic json_output_file json_unsupported json_multi json_tag
        json_none json_determinism json_depfile json_bad_syntax json_missing_input
        json_mutually_exclusive
        json_reader_basic json_reader_unsupported json_reader_multi json_reader_tag
        json_reader_none json_reader_limits components_longdouble
        json_nested_skip
        components_engine_transform   # task 1.3.2 — the REAL tool over the REAL engine header
        components_engine_camera      # task 1.3.3
        components_engine_light       # task 1.3.3
        components_engine_mesh_renderer   # task 1.4.1
        # task 2.2.2: field annotations (AERO_RANGE/AERO_COLOR) and the std::string category
        annotations_components annotations_meta annotations_malformed annotations_nonfinite
        annotations_misapplied annotations_unknown annotations_determinism
        string_components string_meta string_json)
    foreach(_aero_reflect_case IN LISTS _aero_reflect_cases)
        add_test(NAME reflect-gen.${_aero_reflect_case}
            COMMAND "${CMAKE_COMMAND}"
                    -DTOOL=$<TARGET_FILE:aero_reflect_gen>
                    -DCASE=${_aero_reflect_case}
                    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/reflect-gen/${_aero_reflect_case}
                    "-DCLANG_ARGS=${AERO_REFLECT_CLANG_ARGS}"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/reflect-gen/run_case.cmake)
    endforeach()
    unset(_aero_reflect_cases)

    # --- task 1.1.4: the module's first real consumer (replaces the 1.1.3 hand-wired block, D12) ---
    find_package(EnTT CONFIG REQUIRED)   # EnTT::EnTT (header-only) — unchanged from 1.1.3
    add_executable(aero_reflect_meta_test reflect-gen/meta_test.cpp)
    # aero::scene (task 1.3.2) supplies the COMPILE-side include roots for the generated
    # transform.meta.gen.cpp: scene (its own header), reflect (annotations.hpp) and core (math.hpp),
    # all via aero_scene's PUBLIC chain. The INCLUDE_DIRS below supply the PARSE side, which is a
    # separate problem — a linked library's interface include dirs are not target-derivable.
    target_link_libraries(aero_reflect_meta_test PRIVATE doctest::doctest EnTT::EnTT aero::scene aero::core)
    target_include_directories(aero_reflect_meta_test PRIVATE reflect-gen/fixtures)  # meta_test.cpp's own #includes
    aero_reflect_generate(aero_reflect_meta_test
        HEADERS reflect-gen/fixtures/component_codegen.hpp
                reflect-gen/fixtures/component_wiring.hpp
                ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/transform.hpp  # 1.3.2 — the REAL header
                ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/camera.hpp     # 1.3.3
                ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/light.hpp      # 1.3.3
                ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/mesh_renderer.hpp  # 1.4.1
        INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/engine/core/include"      # aero::core PUBLIC dir (D9)
                     "${CMAKE_SOURCE_DIR}/engine/scene/include"     # 1.3.2 — parse: entity.hpp
                     "${CMAKE_SOURCE_DIR}/engine/reflect/include")  # 1.3.2 — parse: annotations.hpp
    add_test(NAME aero_reflect_meta_test COMMAND aero_reflect_meta_test)

    # --- task 1.1.4: the end-to-end incremental-build proof (spec D13) ---
    get_target_property(_aero_entt_include EnTT::EnTT INTERFACE_INCLUDE_DIRECTORIES)   # V4
    add_test(NAME reflect-gen.incremental_e2e
        COMMAND "${CMAKE_COMMAND}"
                -DTOOL=$<TARGET_FILE:aero_reflect_gen>
                -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
                -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/reflect-gen/incremental_e2e
                -DAERO_LLVM_ROOT=${AERO_LLVM_ROOT}
                "-DENTT_INCLUDE=${_aero_entt_include}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/reflect-gen/incremental_e2e.cmake)

    # --- task 1.2.1: the JSON writer runtime proof (sibling of aero_reflect_meta_test; NO EnTT — D4/D11) ---
    add_executable(aero_reflect_json_test reflect-gen/json_test.cpp)
    target_link_libraries(aero_reflect_json_test PRIVATE doctest::doctest aero::reflect aero::scene aero::core)
    target_include_directories(aero_reflect_json_test PRIVATE reflect-gen/fixtures)   # component_*.hpp + aero_reflect.hpp
    aero_reflect_generate_json(aero_reflect_json_test
        HEADERS reflect-gen/fixtures/component_codegen.hpp
                reflect-gen/fixtures/component_wiring.hpp
                reflect-gen/fixtures/component_multi.hpp
                reflect-gen/fixtures/component_limits.hpp   # task 1.2.2 — the D12 numeric-edge fixture
                reflect-gen/fixtures/component_tag.hpp      # task 1.2.2 review fix — a REAL generated
                                                             # zero-field aeroReadJson, proven end-to-end
                ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/transform.hpp  # 1.3.2 — the REAL header
                ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/camera.hpp     # 1.3.3
                ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/light.hpp      # 1.3.3
                ${CMAKE_SOURCE_DIR}/engine/scene/include/aero/scene/mesh_renderer.hpp  # 1.4.1
                reflect-gen/fixtures/component_text.hpp     # task 2.2.2 -- the std::string category
        INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/engine/core/include"
                     "${CMAKE_SOURCE_DIR}/engine/scene/include"     # 1.3.2
                     "${CMAKE_SOURCE_DIR}/engine/reflect/include")  # 1.3.2
    add_test(NAME aero_reflect_json_test COMMAND aero_reflect_json_test)

    # --- task 1.4.2: the World <-> SceneDocument bridge's own tier-0 battery (Correction 2, plan) ---
    # This target LINKS aero::scene_serialize (the generated serializers already compiled INTO that
    # library by engine/scene_serialize/CMakeLists.txt) — it does NOT call aero_reflect_generate_json
    # itself and does NOT forward-declare aeroReadJson/aeroWriteJson (contrast aero_reflect_json_test,
    # which generates into itself because before 1.4.2 there was no shipping consumer).
    add_executable(aero_scene_serialize_test scene_serialize_test.cpp)
    target_link_libraries(aero_scene_serialize_test PRIVATE doctest::doctest aero::scene_serialize aero::scene
                                                             aero::reflect aero::core)
    target_compile_definitions(aero_scene_serialize_test PRIVATE
        AERO_PHASE1_SCENE_DIR="${CMAKE_SOURCE_DIR}/samples/phase-1-scene")  # locates the committed file (test 9)

    # task 2.5.2: the golden fixtures reach the ENGINE battery. AERO_GOLDEN_SCENES_DIR is NOT
    # optional -- scene_serialize_test.cpp's new G1-G10 use it with no #ifdef guard (spec D8), so a
    # missing definition is a COMPILE error and a missing fixture file is a REQUIRE failure. Neither
    # can silently skip, which is the same failure mode as a regeneration flag. The pre-existing
    # AERO_PHASE1_SCENE_DIR line above and its #ifdef consumer at scene_serialize_test.cpp:314 are
    # left ALONE: that guard is acceptable for an OPTIONAL sample and this task does not touch it.
    # AERO_GOLDEN_OUT_DIR is a per-battery failure-dump root -- `ctest -j` runs this target and
    # aero_editor_shell_test concurrently, and neither may write where the other reads (D13).
    # The include dir is for tests/scene_golden_support.hpp; the aero_editor_imgui_test precedent is
    # at :106. This target is NOT a boundary probe, so R12's "exactly one aero:: entry" rule does not
    # apply to it and NO link line changes -- <filesystem> needs no link library, and aero::core
    # (already on the line) has used it since engine/core/src/vfs.cpp.
    target_compile_definitions(aero_scene_serialize_test PRIVATE
        AERO_GOLDEN_SCENES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/scenes"
        AERO_GOLDEN_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/golden/engine")
    target_include_directories(aero_scene_serialize_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME aero_scene_serialize_test COMMAND aero_scene_serialize_test)

    # --- task 2.2.2: the inspector's tier-0 battery + the zero-per-component-code proof (D18) ---
    # GATED: unlike aero_editor_shell_test, this target compiles GENERATED meta for its own fixture.
    # aero::scene_internal is here for the SAME reason aero_tests and aero_editor_shell_test link it:
    # the fixture is registered through engine::scene::internal::registerComponent<T>. Tier-0: no GPU,
    # no window, no ImGui context, no AERO_REQUIRE_GPU, no vulkan_stack_pin.cpp, no ${CMAKE_DL_LIBS}.
    #
    # task 2.4.2 joins this target with editor/field_command_test.cpp (step 5: SetFieldCommand driven
    # against the real generated InspectorProbe meta, cases P1-P7, and step 7's P8) -- a SECOND TU on
    # this already-two-TU-capable target (inspector_test.cpp defines DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN;
    # the new TU must not). The link line below is unchanged.
    add_executable(aero_editor_inspector_test editor/inspector_test.cpp editor/field_command_test.cpp)
    target_link_libraries(aero_editor_inspector_test PRIVATE
        doctest::doctest aero::editor_core aero::scene aero::scene_internal aero::core)
    target_include_directories(aero_editor_inspector_test PRIVATE editor/fixtures)
    aero_reflect_generate(aero_editor_inspector_test
        HEADERS editor/fixtures/inspector_probe.hpp
                editor/fixtures/inspector_tag.hpp  # review finding 6 -- the zero-field tag proof
        INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/engine/core/include"
                     "${CMAKE_SOURCE_DIR}/engine/scene/include"
                     "${CMAKE_SOURCE_DIR}/engine/reflect/include")
    add_test(NAME aero_editor_inspector_test COMMAND aero_editor_inspector_test)
endif()

# ---------------------------------------------------------------------------------------------
# task 2.1.2 -- hermetic proofs that the golden-rule guards actually go RED. Ungated (D15): the
# guards depend on neither codegen nor shaders (the aero_editor_imgui_test precedent above). Neither
# case writes inside the repo tree; both use distinct scratch roots, so ctest -j is safe.
find_program(AERO_BASH_EXECUTABLE bash)
find_package(Git QUIET)      # NOT previously in scope anywhere -- verified; this is its first use

# NOT WIN32 (D16): the script never runs on Windows in CI at all (the lint job is ubuntu-24.04-only),
# and this case's real value-add -- proving the regexes + `nl | sed` comment-stripping behave under
# BSD userland -- comes from the macOS lane. Git Bash is an MSYS2/GNU userland, so Windows would
# re-prove what Linux already proves, against three failure modes that cannot be verified from this
# dev machine (see spec D16). No coverage of the invariant is lost.
if(NOT WIN32 AND AERO_BASH_EXECUTABLE AND GIT_EXECUTABLE)
    add_test(NAME golden-rule.include_scan_e2e COMMAND ${CMAKE_COMMAND}
        -DSCRIPT=${CMAKE_SOURCE_DIR}/.github/scripts/check-golden-rule.sh
        -DBASH=${AERO_BASH_EXECUTABLE}
        -DGIT=${GIT_EXECUTABLE}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/golden-rule-scratch/include-scan
        -P ${CMAKE_CURRENT_SOURCE_DIR}/golden-rule/include_scan_e2e.cmake)
else()
    message(STATUS "golden-rule: include_scan_e2e SKIPPED (Windows, or bash/git not found) -- the "
                   "guard itself still runs in CI's lint job on Linux")
endif()

# All three lanes: pure CMake, spawns no shell, creates no git repo. Six nested configures (clean + 5
# seams) prove both violation arms and all three self-verify arms of aero_assert_golden_rule.
# The `editor` segment in WORK_DIR is LOAD-BEARING -- it is the hermetic proof of D10 (a repo checked
# out under a directory literally named `editor` must still configure clean). Do not "tidy" it away.
add_test(NAME golden-rule.link_graph_e2e COMMAND ${CMAKE_COMMAND}
    -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/golden-rule/link_probe
    -DAERO_REPO_ROOT=${CMAKE_SOURCE_DIR}
    -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/golden-rule-scratch/editor/link-graph
    -P ${CMAKE_CURRENT_SOURCE_DIR}/golden-rule/link_graph_e2e.cmake)

# ---------------------------------------------------------------------------------------------
# Sixth architecture guard: project-no-delete (task 2.6.1, code-review round; D7/INV-P4). A hermetic
# proof that .github/scripts/check-project-no-delete.sh actually goes RED, mirroring
# golden-rule.include_scan_e2e's shape exactly -- the reviewer's condition for accepting sabotage seed
# S11 as documented debt was that the ONLY artefact proving a future `remove_all` would be caught
# (the plan's own §V7 grep) lives in the gitignored docs/plans/ tree and ceases to exist at merge; this
# ctest entry is what survives it. Same NOT WIN32 gate as golden-rule's own e2e case, and for the same
# reason (D16): the script never runs on Windows in CI (the lint job is ubuntu-24.04-only), and this
# case's value -- proving the regex + `nl | sed` comment-stripping pipeline behaves under BSD userland
# -- comes from the macOS lane; Linux would only re-prove what Linux already proves.
if(NOT WIN32 AND AERO_BASH_EXECUTABLE AND GIT_EXECUTABLE)
    add_test(NAME project-no-delete.no_delete_e2e COMMAND ${CMAKE_COMMAND}
        -DSCRIPT=${CMAKE_SOURCE_DIR}/.github/scripts/check-project-no-delete.sh
        -DBASH=${AERO_BASH_EXECUTABLE}
        -DGIT=${GIT_EXECUTABLE}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/project-no-delete-scratch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/project-no-delete/no_delete_e2e.cmake)
else()
    message(STATUS "project-no-delete: no_delete_e2e SKIPPED (Windows, or bash/git not found) -- the "
                   "guard itself still runs in CI's lint job on Linux")
endif()

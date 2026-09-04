# editor/ — the editor layer (docs/03; Epic 2.1 opens it). ImGui/panels/importers/exporter, 3
# desktop platforms. Depends on engine/; NEVER the reverse (golden rule — 2.1.2 makes it a CI
# failure). ImGui + SDL3 + the two internal seams are PRIVATE to aero_editor_core, so ImGui is
# confined to this layer's .cpp TUs and the aero_editor exe / the smoke test stay ImGui-free.

find_package(imgui CONFIG REQUIRED)    # 1.92.8 docking + sdl3 + sdlgpu3 bindings (vcpkg.json, D1)
find_package(imguizmo CONFIG REQUIRED)  # 1.10, MIT, static-only (vcpkg.json, task 2.3.3 D1).
                                        # Editor-ONLY by docs/03:87. Its header installs FLAT into
                                        # include/, so the spelling is #include <ImGuizmo.h> and the
                                        # imported target is imguizmo::imguizmo. The port's exported
                                        # target carries NO interface link libraries (imgui reaches
                                        # it as PRIVATE include dirs), so this adds nothing else.
find_package(SDL3  CONFIG REQUIRED)    # editor speaks SDL/SDL_GPU directly (far side of the boundary)
find_package(Stb REQUIRED)   # task 3.1.3 (D1): header-only. The port ships FindStb.cmake + a
                             # vcpkg-cmake-wrapper (§G-6); there is NO imported target, so the consumer
                             # spelling is ${Stb_INCLUDE_DIR}. Editor-ONLY by docs/03:91.
find_package(fastgltf CONFIG REQUIRED)  # task 3.2.1 (D1): 0.9.0, MIT. EDITOR-ONLY by docs/03's
                                        # dependency-placement invariant. The port's own config
                                        # prepends find_dependency(simdjson) (Apache-2.0 OR MIT -- the
                                        # MIT arm satisfies docs/01's hard requirement outright), so
                                        # simdjson arrives transitively with NO second find_package.
                                        # builtin-baseline is untouched; only the dependencies array grows.
find_package(tinyobjloader CONFIG REQUIRED)  # task 3.2.3 (D1): 2.0.0rc13, MIT, static-only.
                                             # EDITOR-ONLY by docs/03's dependency-placement invariant.
                                             # The port has NO transitive dependency beyond two host
                                             # tools, so nothing arrives with it. The `double` feature
                                             # is NEVER requested: the portfile rewrites
                                             # `#ifdef TINYOBJLOADER_USE_DOUBLE` to `#if 0` in the
                                             # installed header when it is absent, which is what keeps
                                             # `real_t == float` in the header and in the archive.
                                             # builtin-baseline is untouched; only `dependencies` grows.
find_package(assimp CONFIG REQUIRED)  # task 3.2.5 (a settled ADR -- docs/01:30, docs/02 ADR-002/003,
                                      # docs/03:91 and docs/06:28 all already name Assimp for DAE/PLY/STL
                                      # and all already mark it editor-only): 6.0.4#3, BSD-3-Clause.
                                      # EDITOR-ONLY by docs/03's dependency-placement invariant, which
                                      # runtime/CMakeLists.txt already states by hand and phase 5.2.2 is
                                      # specified to assert.
                                      #
                                      # The port declares NO vcpkg_check_linkage, so this is a STATIC
                                      # library on arm64-osx/x64-linux and a DLL on x64-windows -- which
                                      # is why the second stb_image implementation it carries is a
                                      # macOS/Linux concern (see thumbnail_store.cpp's STB_IMAGE_STATIC)
                                      # and why the applocal DLL copy is a Windows one.
                                      #
                                      # Nine transitive ports arrive with it -- zlib, minizip, pugixml,
                                      # rapidjson, poly2tri, polyclipping, utfcpp, kubazip, stb -- every
                                      # one MIT-compatible, which docs/01 requires. The patched config
                                      # find_dependency's `Stb MODULE` on the static triplets, which the
                                      # find_package(Stb REQUIRED) above already satisfies.
                                      #
                                      # builtin-baseline is untouched; only the dependencies array grows.
                                      # The port's ONE optional feature -- compressed-mesh support --
                                      # is NEVER requested, so its extra dependency never arrives. Its
                                      # name is deliberately NOT written here: AC-69's gate greps for
                                      # that token over vcpkg.json and editor/, and a prose mention
                                      # would turn it red for a reason that is not a violation.

# task 3.7.1: the vcpkg miniaudio port is HEADER-ONLY (no CMake config, no prebuilt lib), so the
# header is located explicitly, exactly as engine/platform/CMakeLists.txt does. SYSTEM so ~90k lines
# of code we neither own nor may patch stay out of the warning and clang-tidy surface in a build where
# --warnings-as-errors='*' is live; PRIVATE because no public editor header names a ma_ type.
# THIS IS AN INCLUDE DIRECTORY, NOT A LINK: aero_editor_core already links aero::platform PUBLIC
# (below), and that archive already carries the ONE MINIAUDIO_IMPLEMENTATION unit. A second
# implementation TU anywhere in this tree is a duplicate-symbol link error.
find_path(MINIAUDIO_INCLUDE_DIR NAMES miniaudio.h REQUIRED)

# task 3.2.2 (D3): ufbx is in NO vcpkg registry, so it is VENDORED rather than found. aero_ufbx is a
# STATIC target over one .c compiled with THIS project's flags -- which is what makes it
# ASan/UBSan-instrumented in the Debug lanes. EDITOR-ONLY by docs/03's dependency-placement invariant;
# PRIVATE below (like fastgltf::fastgltf beside it, and aero::scene_serialize further down): on a
# STATIC library that becomes $<LINK_ONLY:>, so the archive DOES reach consumers' link lines (aero_editor,
# every test target linking aero_editor_core) -- that is correct and unavoidable, not a leak -- while
# its SYSTEM include directory (editor/third_party/ufbx/CMakeLists.txt) reaches nobody's compile line.
# No target other than aero_editor_core ever declares `aero_ufbx`/`aero::ufbx` by name.
add_subdirectory(third_party/ufbx)

add_library(aero_editor_core STATIC
    src/imgui_layer.cpp
    src/panel_registry.cpp
    src/editor_app.cpp
    src/shell_ui.cpp
    src/selection.cpp           # task 2.2.1
    src/entity_ops.cpp          # task 2.2.1
    src/tree_walk.cpp           # task 2.2.1, review round 2 (Gap 1)
    src/hierarchy_panel.cpp     # task 2.2.1
    src/component_ops.cpp       # task 2.2.2 -- the D2 seam (entt-using, ImGui-free)
    src/inspector_model.cpp     # task 2.2.2 -- the meta walk (entt-using, ImGui-free, const World&)
    src/meta_utils.cpp          # task 2.2.2 -- the ONE 17-type map, shared by the two above
    src/editor_reflection.cpp   # task 2.2.2 -- the D16 bootstrap (tools-ON/OFF via one #if)
    src/text_input.cpp          # task 2.2.2 -- inputTextString, promoted verbatim from hierarchy_panel.cpp
    src/inspector_panel.cpp     # task 2.2.2 -- the reflection-driven Inspector panel
    src/viewport_panel.cpp      # task 2.2.3 -- the scene rendered to texture inside a panel
    src/project_files.cpp       # task 2.2.4 -- the asset browser's filesystem model (ImGui-free)
    src/asset_browser_panel.cpp # task 2.2.4 -- the read-only project file browser
    src/console_model.cpp       # task 2.2.5 -- the console panel's log model (ImGui-free)
    src/console_panel.cpp       # task 2.2.5 -- the log/console panel
    src/editor_camera.cpp       # task 2.3.1 -- the pure gesture + orbit/pan/dolly/fly model (ImGui-free)
    src/scene_bounds.cpp        # task 2.3.1 -- Aabb + the world-bounds walk (ImGui-free; 2.3.2 reuses)
    src/picking.cpp             # task 2.3.2 -- screen ray + ray-vs-box + the world pick (ImGui-free)
    src/selection_overlay.cpp   # task 2.3.2 -- selection -> screen-space segments (ImGui-free)
    src/gizmo.cpp                # task 2.3.3 -- the pure gizmo model (ImGui-free AND ImGuizmo-free)
    src/view_axis_gizmo.cpp     # task E.1.3 -- the corner widget's layout, hit test, poses and snap
    src/transform_ops.cpp       # task 2.3.3 -- the Transform write seam 2.4.2 wraps
    src/command_stack.cpp       # task 2.4.1 -- Command + the bounded CommandStack (ImGui-free, entt-free)
    src/transform_command.cpp   # task 2.4.1 -- the undoable Transform write wrapping transform_ops
    src/scene_snapshot.cpp      # task 2.4.2 -- SubtreeSnapshot/ComponentSnapshot (entt-free, ImGui-free)
    src/component_commands.cpp  # task 2.4.2 -- SetField/Add/RemoveComponent, wrapping component_ops
    src/entity_commands.cpp     # task 2.4.2 -- the five structural commands, wrapping entity_ops
    src/scene_session.cpp       # task 2.5.1 -- the document model, the pure guard and THE scene swap
    src/text_file.cpp           # task 2.5.1 / 2.6.1 D12 -- ALL <filesystem>/<fstream> for the editor's TEXT files
    src/project.cpp             # task 2.6.1 -- the project format's pure half (ImGui/SDL/<filesystem>-free)
    src/project_file.cpp        # task 2.6.1 -- ALL <filesystem> and ALL SDL for projects; never deletes anything (D7)
    src/scene_io.cpp            # task 2.5.1 -- the scene_serialize bridge; the ONE #if (F10/AC-33)
    src/file_dialog.cpp         # task 2.5.1 -- SDL native dialogs + the cross-thread result channel
    src/project_ui.cpp          # task 2.6.1 -- the Welcome window and the New Project modal
    src/project_settings.cpp    # task 2.6.2 -- the settings panel's pure model (ImGui/SDL/<filesystem>-free)
    src/project_settings_panel.cpp # task 2.6.2 -- the read-only Project Settings panel (the only ImGui in it)
    src/asset_meta.cpp          # task 3.1.1 -- the .meta v1 format + the pure lifecycle planner
    src/asset_database.cpp      # task 3.1.1 -- the scan; <filesystem>-FREE and <fstream>-FREE by rule (D4)
    src/asset_cache.cpp         # task 3.1.2 -- the import-cache index format + the three pure planners
    src/asset_view.cpp          # task 3.1.3 -- kinds, icons, filter/search, grid math (PURE)
    src/thumbnail_cache.cpp     # task 3.1.3 -- key/state/LRU/budget + the box resampler (PURE, no GPU)
    src/thumbnail_store.cpp     # task 3.1.3 -- the ONLY stb_image TU; RHI texture lifetime
    src/asset_actions.cpp       # task 3.1.3 -- the ONE sanctioned deletion (D12/D13)
    src/asset_watcher.cpp       # task 3.1.4 -- the budgeted assets-tree change probe (PURE + one walk)
    src/model_import.cpp        # task 3.2.1 -- the canonical model + every pure helper (NO fastgltf)
    src/gltf_import.cpp         # task 3.2.1 -- the ONLY fastgltf TU; NO file operation at all
    src/model_import_session.cpp # task 3.2.1 -- the on-demand two-pass driver; the ONE write
    src/import_details_panel.cpp # task 3.2.1 -- the ImGui half of the import result
    src/fbx_import.cpp           # task 3.2.2 -- the ONLY ufbx TU; NO file operation at all
    src/obj_import.cpp           # task 3.2.3 -- the ONLY tinyobjloader TU; NO file operation at all
    src/blender_tool.cpp         # task 3.2.4 -- PURE: no disk, no SDL, no <filesystem>, no logging
    src/blender_process.cpp      # task 3.2.4 -- the ONLY SDL_Process TU in the tree
    src/blender_service.cpp      # task 3.2.4 -- the state machine; SDL_getenv only, never SDL_Process
    src/assimp_import.cpp        # task 3.2.5 -- the ONLY assimp TU; NO file operation at all
    src/mesh_cook_source.cpp     # task 3.3.1 -- the ONLY ImportedModel -> assets:: adapter; PURE
    src/texture_cook_source.cpp  # task 3.3.2 -- the SECOND stb_image TU; PURE, no disk, no UI
    src/material_edit.cpp        # task 3.4.2 -- the PURE document->render bridge (D8); no ImGui, no GPU
    src/material_session.cpp     # task 3.4.2 -- the target/dirty/apply machine; the ONE .aeromat write
    src/material_panel.cpp       # task 3.4.2 -- the only new ImGui TU
    src/material_preview.cpp     # task 3.4.2 -- the only new GPU TU: target, renderer, texture cache
    src/skeleton_cook_source.cpp # task 3.5.1 -- the ImportedModel -> skeleton cook adapter; PURE
    src/animation_cook_source.cpp # task 3.5.2 -- the ImportedModel -> animation cook adapter; PURE
    src/audio_cook_source.cpp    # task 3.7.1 -- the audio source-name predicates; PURE, no third party
    src/audio_decode.cpp         # task 3.7.1 -- the ONLY stb_vorbis TU; the ONLY miniaudio TU outside engine/platform/src
    src/asset_drag.cpp           # task 3.1.5 -- the drag payload + the drop routing matrix; PURE, no ImGui
    src/instantiate_plan.cpp     # task 3.1.5 -- the ImportedModel -> entity-subtree planner; PURE
    src/asset_commands.cpp       # task 3.1.5 -- the sixth structural command: one undoable drop
    src/material_from_import.cpp # task 3.1.5 -- ImportedMaterial -> MaterialDocument; PURE
    src/texture_load.cpp         # task 3.1.5 -- the SHARED decode->cook->parse->upload chain (src-private)
    src/scene_asset_ledger.cpp   # task 3.1.5 -- the decision core; PURE, no GPU, no disk, no UI
    src/scene_asset_loader.cpp   # task 3.1.5 -- the only new GPU TU: executes ONE ledger directive
)
add_library(aero::editor_core ALIAS aero_editor_core)
target_include_directories(aero_editor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# SYSTEM so nothing lints or warns on a vendored header we neither own nor may patch. PRIVATE for the
# same reason imgui is. NOTE (A13): this does NOT confine stb_image.h to one TU -- FindStb resolves to
# vcpkg's SHARED per-triplet include root, which is already on every doctest-linking target's compile
# line (R12). INV-V10 is held by the §V6 grep and by review, exactly like the ImGui-free-public-header
# rule. Do not claim enforcement that does not exist.
target_include_directories(aero_editor_core SYSTEM PRIVATE ${Stb_INCLUDE_DIR})
# task 3.7.1: miniaudio's DECLARATIONS, for the editor-side wav/flac/mp3 decode. Same SYSTEM/PRIVATE
# discipline as ${Stb_INCLUDE_DIR} above, and the same non-enforcement caveat (R12): FindStb and this
# find_path both resolve inside vcpkg's SHARED per-triplet include root, so neither confines its header
# to one TU. Confining <miniaudio.h> to the ONE editor decode TU is held by review and by an include
# grep, exactly like the ImGui-free-public-header rule -- and NOT by this line. Do not claim
# enforcement that does not exist. check-platform-boundary.sh is a different guarantee again: its glob
# is engine/*/include/*, so it polices the ENGINE's public headers and never sees editor/src at all.
#
# THAT TU IS DELIBERATELY NOT NAMED HERE, and the reason is a test rather than taste: AI2/AI3
# (tests/editor/assimp_import_test.cpp) derive the editor's TU list by scanning THIS FILE's raw text
# for `src/*.cpp` tokens -- comments included -- and then REQUIRE every name they find to be a
# readable file. A prose mention of a source that does not exist yet turns them red for a reason that
# is not a violation, which is the same trap the assimp block above records for its feature token.
target_include_directories(aero_editor_core SYSTEM PRIVATE ${MINIAUDIO_INCLUDE_DIR})
find_package(EnTT CONFIG REQUIRED)   # header-only; a manifest dependency since 1.1.3 -- no vcpkg change
target_link_libraries(aero_editor_core
    # aero::scene is PUBLIC because panel_context.hpp / editor_app.hpp surface engine::World and
    # engine::Entity in PUBLIC editor headers (task 2.2.1). It propagates NO vcpkg include path:
    # aero_scene links EnTT::EnTT PRIVATE (task 1.3.1) and its PUBLIC deps (aero::core, aero::reflect)
    # link no vcpkg package either -- the same property tests/scene_boundary_probe.cpp rests on. It is
    # also NOT gated on AERO_REFLECT_TOOLS (only engine/scene_serialize is), so the tools-OFF build and
    # the ungated tier-0 shell test both survive (2.1.3 AC-9 preserved).
    # aero::assets is PUBLIC because mesh_cook_source.hpp names assets::MeshCookPrimitive and
    # assets::MeshCookResult in its own signatures (task 3.3.1). It propagates NO vcpkg include path:
    # aero_assets links NO vcpkg package at all and its only PUBLIC dependency is aero::core, which
    # links spdlog/glm/enkiTS PRIVATE -- the same property tests/math_boundary_probe.cpp rests on. It
    # is NOT gated on AERO_REFLECT_TOOLS, so both reduced configurations and the ungated tier-0 shell
    # test survive unchanged.
    # aero::render is PUBLIC because material_edit.hpp names render::MaterialParams,
    # render::MaterialAlpha and render::defaultTextureKindForSlot in its own signatures (task 3.4.2).
    # A RECORDED DEVIATION from the spec's "no link-line change at all", taken because that clause and
    # AC-25 cannot both hold: aero::scene_render is PRIVATE below, which on a STATIC library propagates
    # as $<LINK_ONLY:...> -- the archive reaches consumers, engine/render/include does NOT -- so a
    # public editor header naming a render type does not compile without this edge, and
    # aero_editor_shell_test cannot see the header at all. It is the aero::assets precedent one epic
    # old, criterion for criterion: engine/render/CMakeLists.txt calls NO find_package, its PUBLIC
    # dependencies are aero::rhi and aero::assets -- BOTH ALREADY PUBLIC on this line, so the edge adds
    # nothing to a consumer's compile line but engine/render/include -- it carries no build gate of any
    # kind, and no tests/*_boundary_probe.cpp links aero::editor_core, so no probe can be weakened by
    # it. The golden rule is untouched: editor -> render is the legal direction, and
    # check-golden-rule.sh polices the reverse.
    PUBLIC  aero::rhi aero::platform aero::scene aero::core aero::assets aero::render
    # EnTT is PRIVATE for the SAME reason imgui is (D1/2.1.3): the editor's PUBLIC headers stay
    # entt-free BY FILE PLACEMENT -- every entt entry point lives in editor/src/. That is convention +
    # review, NOT enforcement: this target's consumers link doctest/vcpkg roots, so a leaked
    # <entt/entt.hpp> in a public editor header would still compile (risk R12, docs/08 -- the same
    # limitation 2.1.3 documented for imgui.h and proved by seeding it). Do not "fix" it with a probe.
    # aero::scene_render is PRIVATE (D17): no PUBLIC editor header names a render/scene_render type --
    # viewport_panel.hpp is src-private. It brings aero::render transitively (PUBLIC on
    # aero_scene_render) and, like aero::scene, propagates no vcpkg include path. It is NOT gated on
    # AERO_REFLECT_TOOLS (engine/scene_render/CMakeLists.txt has no gate), so the tools-OFF build and
    # the ungated tier-0 shell test both survive.
    # imguizmo::imguizmo is PRIVATE for the SAME reason imgui::imgui is (D1/2.1.3): it confines
    # ImGuizmo to this layer's .cpp TUs, so aero_editor and aero_editor_shell_test stay ImGuizmo-free
    # at source. Like imgui, that is convention + review, NOT enforcement (R12).
    # tinyobjloader::tinyobjloader is PRIVATE, exactly as fastgltf::fastgltf and aero_ufbx are (task
    # 3.2.3, D1): on a STATIC library a PRIVATE dependency propagates as $<LINK_ONLY:...>, so the
    # archive DOES reach aero_editor's and every test target's link line -- correct and unavoidable,
    # not a leak. The checkable invariant is the INCLUDE boundary: tiny_obj_loader.h reaches exactly
    # one TU (obj_import.cpp), never a test TU and never an engine/ TU.
    # assimp::assimp is PRIVATE, exactly as fastgltf::fastgltf, aero_ufbx and
    # tinyobjloader::tinyobjloader are (task 3.2.5): on a STATIC library a PRIVATE dependency propagates
    # as $<LINK_ONLY:...>, so the archive DOES reach aero_editor's and every test target's link line --
    # correct and unavoidable, not a leak. The checkable invariant is the INCLUDE boundary: no Assimp
    # header reaches any TU but editor/src/assimp_import.cpp, which AC-11's grep and
    # assimp_import_test.cpp's own AI2 source assertion both check. NEVER write the eight-character
    # token "assimp" immediately followed by a forward slash in a comment anywhere under editor/,
    # tests/, engine/, runtime/ or tools/ -- AC-11's grep scans those roots and a prose mention would
    # turn it red for a reason that is not a violation.
    PRIVATE aero::profiling imgui::imgui imguizmo::imguizmo SDL3::SDL3 EnTT::EnTT aero::scene_render
            aero::platform_internal aero::rhi_internal fastgltf::fastgltf aero_ufbx
            tinyobjloader::tinyobjloader assimp::assimp
)

# task 2.6.1 (D14): the ONLY consumer is editor_app.cpp, and `engineVersion` is INFORMATIONAL in v0
# -- never compared for ordering, never a gate. This is what keeps engine/ untouched for a THIRD
# consecutive task: engine/core/include/aero/core/version.hpp would be a defensible change, but the
# runtime is the task that should decide that header's shape (Phase 5), not this one.
target_compile_definitions(aero_editor_core PRIVATE AERO_ENGINE_VERSION="${PROJECT_VERSION}")

if(AERO_REFLECT_TOOLS)
    # Task 2.2.2 (D16/§3.12): the four built-in component headers, compiled INTO the editor as
    # generated entt::meta registration + one aggregator, aero_reflect_register_all_aero_editor_core
    # (cmake/reflect.cmake's MAKE_C_IDENTIFIER rule). The INCLUDE_DIRS trio is the F19/scene_serialize
    # set. The entt-using TUs compile UNCONDITIONALLY: with tools OFF nothing is registered, resolve
    # misses, and D12's degradation falls out with zero #if outside editor_reflection.cpp.
    aero_reflect_generate(aero_editor_core
        HEADERS ${AERO_BUILTIN_COMPONENT_HEADERS}   # task 3.5.2 (D7): named once, at root scope
        INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/engine/core/include"
                     "${CMAKE_SOURCE_DIR}/engine/scene/include"
                     "${CMAKE_SOURCE_DIR}/engine/reflect/include")
    target_compile_definitions(aero_editor_core PRIVATE AERO_EDITOR_REFLECTION=1)

    # Task 2.5.1: the editor's scene I/O bridge. PRIVATE, and INSIDE this gate, because
    # engine/scene_serialize/CMakeLists.txt:7-10 return()s when AERO_REFLECT_TOOLS is OFF -- the target
    # does not exist there, so neither does its include directory. PRIVATE (not PUBLIC) because no
    # PUBLIC editor header names a scene_serialize type (AC-32); on a STATIC library that becomes
    # $<LINK_ONLY:>, so the archive reaches consumers' link lines while its include directory reaches
    # nobody's compile line. It brings aero::reflect, which aero::scene already carries PUBLIC, so no
    # new vcpkg include path reaches any consumer and no boundary probe is affected.
    target_link_libraries(aero_editor_core PRIVATE aero::scene_serialize)
endif()

if(AERO_SHADER_TOOLS)
    # Task 2.2.3: the viewport needs the cooked scene.{vert,frag}. Same shape as
    # samples/phase-1-scene (F22). INVARIANT: AERO_SHADERS_DIR must equal aero_add_shaders'
    # OUTPUT_DIR -- move both together (tests/CMakeLists.txt records the same rule).
    add_dependencies(aero_editor_core aero_shaders)
    target_compile_definitions(aero_editor_core PRIVATE
        AERO_EDITOR_SHADERS=1 AERO_SHADERS_DIR="${CMAKE_BINARY_DIR}/shaders")
endif()

add_executable(aero_editor src/main.cpp)
target_link_libraries(aero_editor PRIVATE aero::editor_core aero::core)

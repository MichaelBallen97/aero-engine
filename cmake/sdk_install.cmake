# task 4.5.1: the installable C++ SDK (docs/04). Included by the root CMakeLists when AERO_SDK is ON, AFTER every
# add_subdirectory(): root scope reaches no guarded target, and the layers' aliases exist by now.
#
# `aero_sdk` is what the sdk build presets build: the eleven layers and the compiled shaders. add_dependencies takes an
# alias, and spelling the raw aero_<layer> names here would redden audio-boundary's Part 1d.
add_custom_target(aero_sdk)
foreach(_sdk_layer IN ITEMS core assets audio platform rhi render reflect scene scene_render scene_audio
                            scene_serialize)
    add_dependencies(aero_sdk aero::${_sdk_layer})
endforeach()
unset(_sdk_layer)
add_dependencies(aero_sdk aero_shaders)

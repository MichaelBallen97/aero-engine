#pragma once
// engine/scene_serialize/src/builtin_serializers.hpp — task 1.4.2: internal (not installed) forward
// declarations of the 10 functions aero_reflect_generate_json generates for the 5 built-in components
// (into THIS library, via CMakeLists.txt's aero_reflect_generate_json call — Correction 2). Mirrors
// tests/reflect-gen/json_test.cpp's forward-decl pattern exactly: unqualified aeroWriteJson/
// aeroReadJson inside `namespace engine { }`, DOM parameter types fully-qualified so ADL finds them
// from scene_serialize.cpp regardless of which `using`/namespace context calls them.
#include <aero/reflect/json_value.hpp>
#include <aero/reflect/json_writer.hpp>
#include <aero/scene/animation_player.hpp>
#include <aero/scene/audio_listener.hpp>
#include <aero/scene/audio_source.hpp>
#include <aero/scene/camera.hpp>
#include <aero/scene/environment.hpp>  // task E.2.1
#include <aero/scene/light.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/transform.hpp>

namespace engine {
void aeroWriteJson(engine::JsonWriter&, const Transform&);
bool aeroReadJson(const engine::JsonValue&, Transform&);
void aeroWriteJson(engine::JsonWriter&, const Camera&);
bool aeroReadJson(const engine::JsonValue&, Camera&);
void aeroWriteJson(engine::JsonWriter&, const DirectionalLight&);
bool aeroReadJson(const engine::JsonValue&, DirectionalLight&);
void aeroWriteJson(engine::JsonWriter&, const PointLight&);
bool aeroReadJson(const engine::JsonValue&, PointLight&);
void aeroWriteJson(engine::JsonWriter&, const MeshRenderer&);
bool aeroReadJson(const engine::JsonValue&, MeshRenderer&);
void aeroWriteJson(engine::JsonWriter&, const AnimationPlayer&);  // task 3.5.2
bool aeroReadJson(const engine::JsonValue&, AnimationPlayer&);
void aeroWriteJson(engine::JsonWriter&, const AudioSource&);  // task 3.7.2
bool aeroReadJson(const engine::JsonValue&, AudioSource&);
void aeroWriteJson(engine::JsonWriter&, const AudioListener&);  // task 3.7.2
bool aeroReadJson(const engine::JsonValue&, AudioListener&);
void aeroWriteJson(engine::JsonWriter&, const Environment&);  // task E.2.1
bool aeroReadJson(const engine::JsonValue&, Environment&);
}  // namespace engine

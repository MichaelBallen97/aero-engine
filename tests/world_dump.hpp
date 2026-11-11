#pragma once
// Aero Engine -- two World oracles shared by the World::clone test (task 4.7.2, scene_clone_test.cpp) and the park's
// tests (parked_scene_test.cpp, imgui_layer_test.cpp). Neither reads reflection, so both run identically with
// AERO_REFLECT_TOOLS=OFF. Everything is inline in namespace world_test, so including this header from several
// translation units defines nothing twice.
//
// dumpWorld() is a text of everything the public World API shows: identities, names, the forest in attach order, which
// registered types each entity holds, the Transform's bits, and each registration's count. firstBuiltinDifference()
// compares the eleven built-in components value by value through their defaulted operator==.

#include <aero/scene/scene.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace world_test {

namespace detail {

inline void appendHex(std::string& out, float value) {
    static constexpr const char* DIGITS = "0123456789abcdef";
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (int shift = 28; shift >= 0; shift -= 4) {
        out.push_back(DIGITS[(bits >> static_cast<unsigned>(shift)) & 0xFU]);
    }
}

inline void appendEntity(std::string& out, engine::Entity entity) {
    out += std::to_string(entity.index);
    out.push_back(':');
    out += std::to_string(entity.generation);
}

// Compares component T of `a` and `b` on `entity`: presence first, then the value. "" when equal.
template <typename T>
std::string compareOne(engine::World& a, engine::World& b, engine::Entity entity, const char* typeName) {
    const T* left = a.get<T>(entity);
    const T* right = b.get<T>(entity);
    if ((left == nullptr) != (right == nullptr)) {
        return std::string{typeName} + " presence differs on " + std::to_string(entity.index) + ":" +
               std::to_string(entity.generation);
    }
    if (left != nullptr && !(*left == *right)) {
        return std::string{typeName} + " value differs on " + std::to_string(entity.index) + ":" +
               std::to_string(entity.generation);
    }
    return {};
}

}  // namespace detail

inline std::string dumpWorld(const engine::World& world) {
    std::string out;
    const std::size_t types = world.componentTypeCount();
    world.eachEntity([&](engine::Entity entity) {
        detail::appendEntity(out, entity);
        out += " name=[";
        out += std::string{world.name(entity)};
        out += "] parent=";
        if (world.parent(entity).valid()) {
            detail::appendEntity(out, world.parent(entity));
        } else {
            out.push_back('-');
        }
        out += " children=";
        world.eachChild(entity, [&](engine::Entity child) {
            detail::appendEntity(out, child);
            out.push_back(',');
        });
        out += " types=";
        for (std::size_t i = 0; i < types; ++i) {
            if (world.hasRaw(world.componentTypeAt(i), entity)) {
                out += std::to_string(i);
                out.push_back(',');
            }
        }
        if (const auto* transform = world.get<engine::Transform>(entity); transform != nullptr) {
            out += " xf=";
            for (const float value :
                 {transform->position.x, transform->position.y, transform->position.z, transform->rotation.x,
                  transform->rotation.y, transform->rotation.z, transform->rotation.w, transform->scale.x,
                  transform->scale.y, transform->scale.z}) {
                detail::appendHex(out, value);
                out.push_back('.');
            }
        }
        out.push_back('\n');
    });
    for (std::size_t i = 0; i < types; ++i) {
        const engine::ComponentTypeId id = world.componentTypeAt(i);
        out += "type ";
        out += std::to_string(i);
        out.push_back(' ');
        out += std::string{world.componentTypeName(id)};
        out.push_back(' ');
        out += std::to_string(world.countRaw(id));
        out.push_back('\n');
    }
    return out;
}

// The first built-in component that differs between `a` and `b` on a live entity of `a`, as a line naming the entity
// and the type; "" when none does. `b` must answer alive() for every live entity of `a`.
inline std::string firstBuiltinDifference(engine::World& a, engine::World& b) {
    std::vector<engine::Entity> entities;
    a.eachEntity([&](engine::Entity entity) { entities.push_back(entity); });
    for (const engine::Entity entity : entities) {
        if (!b.alive(entity)) {
            return "not alive on the other World: " + std::to_string(entity.index) + ":" +
                   std::to_string(entity.generation);
        }
        const std::string found = [&]() -> std::string {
            for (const std::string& line : {
                     detail::compareOne<engine::Transform>(a, b, entity, "Transform"),
                     detail::compareOne<engine::Camera>(a, b, entity, "Camera"),
                     detail::compareOne<engine::DirectionalLight>(a, b, entity, "DirectionalLight"),
                     detail::compareOne<engine::PointLight>(a, b, entity, "PointLight"),
                     detail::compareOne<engine::MeshRenderer>(a, b, entity, "MeshRenderer"),
                     detail::compareOne<engine::AnimationPlayer>(a, b, entity, "AnimationPlayer"),
                     detail::compareOne<engine::AudioSource>(a, b, entity, "AudioSource"),
                     detail::compareOne<engine::AudioListener>(a, b, entity, "AudioListener"),
                     detail::compareOne<engine::Environment>(a, b, entity, "Environment"),
                     detail::compareOne<engine::SpotLight>(a, b, entity, "SpotLight"),
                     detail::compareOne<engine::ScriptComponent>(a, b, entity, "ScriptComponent"),
                 }) {
                if (!line.empty()) {
                    return line;
                }
            }
            return {};
        }();
        if (!found.empty()) {
            return found;
        }
    }
    return {};
}

}  // namespace world_test

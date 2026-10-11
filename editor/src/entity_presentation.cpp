// Aero Engine -- what an entity looks like in the panels (task E.6.3). PURE: see entity_presentation.hpp.
#include <aero/editor/editor_icons.hpp>
#include <aero/editor/entity_presentation.hpp>
#include <aero/scene/audio_listener.hpp>
#include <aero/scene/audio_source.hpp>
#include <aero/scene/camera.hpp>
#include <aero/scene/environment.hpp>
#include <aero/scene/light.hpp>
#include <aero/scene/mesh_renderer.hpp>
#include <aero/scene/spot_light.hpp>
#include <aero/scene/world.hpp>

#include <algorithm>
#include <array>

namespace engine::editor {

namespace {

// THE ten built-ins (CMakeLists.txt's AERO_BUILTIN_COMPONENT_HEADERS), keyed by registration name.
constexpr std::array<ComponentIconRow, COMPONENT_ICON_COUNT> COMPONENT_ICONS{{
    {"engine::Transform", AERO_ICON_AXIS_3D},
    {"engine::Camera", AERO_ICON_CAMERA},
    {"engine::DirectionalLight", AERO_ICON_SUN},
    {"engine::PointLight", AERO_ICON_LIGHTBULB},
    {"engine::SpotLight", AERO_ICON_LAMP_CEILING},
    {"engine::MeshRenderer", AERO_ICON_BOX},
    {"engine::AnimationPlayer", AERO_ICON_FILM},
    {"engine::AudioSource", AERO_ICON_VOLUME_2},
    {"engine::AudioListener", AERO_ICON_EAR},
    {"engine::Environment", AERO_ICON_CLOUD_SUN},
}};

[[nodiscard]] bool isAsciiUpper(char c) noexcept { return c >= 'A' && c <= 'Z'; }
[[nodiscard]] bool isAsciiLower(char c) noexcept { return c >= 'a' && c <= 'z'; }

}  // namespace

EntityKind entityKindFor(const World& world, Entity entity) noexcept {
    if (!world.alive(entity)) {
        return EntityKind::Empty;  // a dead OR null handle -- alive() answers both, silently
    }
    if (world.has<Camera>(entity)) {
        return EntityKind::Camera;
    }
    if (world.has<DirectionalLight>(entity) || world.has<PointLight>(entity) || world.has<SpotLight>(entity)) {
        return EntityKind::Light;
    }
    if (world.has<MeshRenderer>(entity)) {
        return EntityKind::Mesh;
    }
    if (world.has<AudioSource>(entity) || world.has<AudioListener>(entity)) {
        return EntityKind::Audio;
    }
    if (world.has<Environment>(entity)) {
        return EntityKind::Environment;
    }
    return EntityKind::Empty;
}

ChromeRole entityKindRole(EntityKind kind, bool selected) noexcept {
    if (selected) {
        return ChromeRole::Accent;
    }
    switch (kind) {  // identical arms share ONE label (bugprone-branch-clone)
        case EntityKind::Camera:
            return ChromeRole::TextSecondary;
        case EntityKind::Light:
            return ChromeRole::Warning;
        case EntityKind::Mesh:
            return ChromeRole::Info;
        case EntityKind::Audio:
            return ChromeRole::Success;
        case EntityKind::Environment:
        case EntityKind::Empty:
            return ChromeRole::TextFaint;
    }
    return ChromeRole::TextFaint;
}

std::span<const ComponentIconRow> componentIconTable() noexcept { return COMPONENT_ICONS; }

std::string_view componentIconFor(std::string_view registrationName) noexcept {
    const auto row = std::ranges::find(COMPONENT_ICONS, registrationName, &ComponentIconRow::registrationName);
    return row != COMPONENT_ICONS.end() ? row->icon : std::string_view(AERO_ICON_SHAPES);
}

void humanizeIdentifier(std::string_view identifier, std::string& out) {
    out.clear();
    bool inWord = false;
    for (std::size_t i = 0; i < identifier.size(); ++i) {
        const char c = identifier[i];
        if (c == '_' || c == ' ') {
            inWord = false;  // a separator: dropped, and the next byte starts a word
            continue;
        }
        bool startsWord = !inWord;
        if (inWord && isAsciiUpper(c)) {
            // inWord means i > 0 and the previous byte was no separator.
            const bool afterNonCapital = !isAsciiUpper(identifier[i - 1U]);
            const bool lastOfCapitalRun = i + 1U < identifier.size() && isAsciiLower(identifier[i + 1U]);
            startsWord = afterNonCapital || lastOfCapitalRun;
        }
        if (startsWord) {
            if (!out.empty()) {
                out.push_back(' ');
            }
            out.push_back(isAsciiLower(c) ? static_cast<char>(c - 'a' + 'A') : c);
            inWord = true;
        } else {
            out.push_back(c);
        }
    }
}

void entityIdText(Entity entity, std::string& out) {
    out = "e#";
    out += std::to_string(entity.index);
}

void entityIdTooltip(Entity entity, std::string& out) {
    out = "Entity ";
    out += std::to_string(entity.index);
    out += ", generation ";
    out += std::to_string(entity.generation);
}

}  // namespace engine::editor

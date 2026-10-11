#pragma once
// Aero Engine -- what an entity LOOKS LIKE in the panels (task E.6.3, spec D5, D6): its kind (the Hierarchy's dot, the
// Inspector's swatch), its components' icons, a field name humanised for a label, and its id text. PUBLIC and PURE:
// it asks World::has<T> only, never each<T>, which logs per call for an unregistered type (viewport_icons.hpp:110-113).
//
// THE COMPONENT ICON TABLE IS A COMPONENT-COUNT SITE: it holds exactly the ten built-ins (EK5 pins ten rows), so the
// next built-in adds a row here, beside RF7's and IR21's literals. A missing row is the fallback glyph, never an error.
#include <aero/editor/panel_chrome_model.hpp>  // ChromeRole
#include <aero/scene/entity.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace engine {
class World;
}  // namespace engine

namespace engine::editor {

// D6's precedence: Camera > Light (directional / point / spot) > Mesh > Audio (source / listener) > Environment >
// Empty. It differs from viewportIconFor's (light before camera) ON PURPOSE: the dot names what an entity IS for, the
// viewport icon what it emits (spec section 10, risk 7).
enum class EntityKind : std::uint8_t { Camera, Light, Mesh, Audio, Environment, Empty };
[[nodiscard]] EntityKind entityKindFor(const World& world, Entity entity) noexcept;  // dead / null -> Empty, silent
// Light warning, mesh info, camera textSecondary, audio success, environment and empty textFaint; selected -> accent.
[[nodiscard]] ChromeRole entityKindRole(EntityKind kind, bool selected) noexcept;

struct ComponentIconRow {
    std::string_view registrationName;  // World::componentTypeName, "engine::Transform"
    std::string_view icon;              // an AERO_ICON_* view
};
inline constexpr std::size_t COMPONENT_ICON_COUNT = 10;  // the built-ins: the next one moves this and EK5
[[nodiscard]] std::span<const ComponentIconRow> componentIconTable() noexcept;
// By REGISTRATION name (never the short name: "game::Camera" is not engine::Camera); AERO_ICON_SHAPES otherwise.
[[nodiscard]] std::string_view componentIconFor(std::string_view registrationName) noexcept;

// camelCase / snake_case -> "Title Case". `_` and ` ` separate words (dropped; ONE space joins words; an empty word
// is skipped). Inside a run a word starts before an ASCII capital whose previous byte is not an ASCII capital, and
// before the last capital of a capital run followed by an ASCII lower-case letter (HDRExposure -> HDR Exposure). A
// digit never starts a word. A word's first byte is upper-cased only if it is ASCII lower case; every other byte,
// and every byte >= 0x80, is unchanged. Idempotent on its own output. CLEARS `out`, then appends -- a caller-owned
// slot keeps its capacity (inspector_model.hpp's KP3 rule).
void humanizeIdentifier(std::string_view identifier, std::string& out);
void entityIdText(Entity entity, std::string& out);     // "e#<index>", decimal (D5); clears `out`
void entityIdTooltip(Entity entity, std::string& out);  // "Entity <index>, generation <generation>"; clears `out`

}  // namespace engine::editor

// Aero Engine -- the component binding table (task 4.2.1): the add rules, and the built-in table. Backend-free.

#include <aero/core/log.hpp>
#include <aero/script/internal/component_binding.hpp>

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

#if defined(AERO_SCRIPT_COMPONENT_BINDINGS)
// NOLINTNEXTLINE(readability-identifier-naming) -- the generated aggregator's frozen snake_case name
void aero_script_bind_all_aero_script(engine::script::internal::ComponentBindingTable& table);
#endif

namespace engine::script::internal {

namespace {

[[nodiscard]] bool isIdentifierStart(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

[[nodiscard]] bool isIdentifierPart(char c) noexcept { return isIdentifierStart(c) || (c >= '0' && c <= '9'); }

[[nodiscard]] bool isIdentifier(std::string_view name) noexcept {
    return !name.empty() && isIdentifierStart(name.front()) &&
           std::all_of(name.begin() + 1, name.end(), [](char c) { return isIdentifierPart(c); });
}

// The rule a binding breaks, or an empty view when it breaks none.
[[nodiscard]] std::string_view brokenRule(const ComponentBinding& binding, std::span<const ComponentBinding> bound) {
    if (!isIdentifier(binding.scriptName)) {
        return "the script name must be a non-empty identifier";
    }
    if (binding.scriptName == "Vec3" || binding.scriptName == "Quat" || binding.scriptName == "Entity") {
        return "the script name is reserved for a class the namespace already holds";
    }
    if (!binding.type.valid()) {
        return "the component type is invalid";
    }
    for (const ComponentBinding& other : bound) {
        if (other.scriptName == binding.scriptName) {
            return "the script name is already bound";
        }
        if (other.type == binding.type) {
            return "the component type is already bound";
        }
    }
    if (bound.size() >= MAX_BOUND_COMPONENTS) {
        return "at most 127 components can be bound";
    }
    if (binding.fields.size() > MAX_BOUND_FIELDS) {
        return "at most 255 fields can be bound per component";
    }
    for (std::size_t i = 0; i < binding.fields.size(); ++i) {
        const FieldBinding& field = binding.fields[i];
        if (field.name.empty()) {
            return "a field name is empty";
        }
        if (field.name == "constructor") {
            return "a field cannot be named constructor";
        }
        if (field.address == nullptr) {
            return "a field has no address";
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (binding.fields[j].name == field.name) {
                return "a field name is repeated";
            }
        }
    }
    return {};
}

}  // namespace

bool ComponentBindingTable::add(ComponentBinding binding) {
    const std::string_view rule = brokenRule(binding, entries);
    if (!rule.empty()) {
        AERO_LOG_ERROR("script: component binding '{}' refused: {}", binding.scriptName, rule);
        return false;
    }
    entries.push_back(std::move(binding));
    return true;
}

std::span<const ComponentBinding> ComponentBindingTable::components() const noexcept { return entries; }

const ComponentBindingTable& builtinComponentBindings() {
    static const ComponentBindingTable table = [] {
        ComponentBindingTable built;
#if defined(AERO_SCRIPT_COMPONENT_BINDINGS)
        aero_script_bind_all_aero_script(built);
#endif
        return built;
    }();
    return table;
}

}  // namespace engine::script::internal

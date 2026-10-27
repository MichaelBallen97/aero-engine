#pragma once
// Aero Engine -- the component binding descriptors (task 4.2.1; internal to engine/script). Backend-free: nothing
// here names the JavaScript engine, so reflect-gen's generated tables compile against it and the tests drive it
// directly through aero::script_internal. The quickjs runtime (src/) reads these descriptors; it never sees a C++
// component type.
//
// One ComponentBinding says "the component C is the script class `scriptName`, with these fields": for each field a
// name, a FieldKind and the address of the member inside one stored component. The generated code builds them with
// bindComponent / bindField, so a line that names the wrong class is a build error, and a field type outside the
// seven bindable ones is a static_assert naming those seven.

#include <aero/core/guid.hpp>
#include <aero/core/math.hpp>
#include <aero/scene/component.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace engine::script::internal {

enum class FieldKind : std::uint8_t { Float = 0, Bool, UInt32, Vec3, Quat, Guid, String };

// A native function's magic is a 16-bit integer, and an accessor's is component << 8 | field: so at most 127
// components (the high byte stays below the sign bit) and 255 fields each.
inline constexpr std::size_t MAX_BOUND_COMPONENTS = 127;
inline constexpr std::size_t MAX_BOUND_FIELDS = 255;

namespace detail {
template <typename T>
inline constexpr bool ALWAYS_FALSE = false;
}  // namespace detail

// float, bool, std::uint32_t, Vec3, Quat, Guid, std::string; any other type is a static_assert naming those seven.
// This is the one place a C++ type maps to a script kind.
template <typename T>
[[nodiscard]] consteval FieldKind fieldKindOf() {
    if constexpr (std::is_same_v<T, float>) {
        return FieldKind::Float;
    } else if constexpr (std::is_same_v<T, bool>) {
        return FieldKind::Bool;
    } else if constexpr (std::is_same_v<T, std::uint32_t>) {
        return FieldKind::UInt32;
    } else if constexpr (std::is_same_v<T, Vec3>) {
        return FieldKind::Vec3;
    } else if constexpr (std::is_same_v<T, Quat>) {
        return FieldKind::Quat;
    } else if constexpr (std::is_same_v<T, Guid>) {
        return FieldKind::Guid;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return FieldKind::String;
    } else {
        static_assert(detail::ALWAYS_FALSE<T>,
                      "a script-bound field must be float, bool, std::uint32_t, Vec3, Quat, Guid or std::string");
        return FieldKind::Float;
    }
}

struct FieldBinding {
    std::string_view name;                                 // the member's name, and the script property's
    FieldKind kind = FieldKind::Float;                     // how a script value converts to and from the member
    void* (*address)(void* component) noexcept = nullptr;  // the member inside one stored component
};

struct ComponentBinding {
    std::string_view scriptName;       // the class name under `engine` -- the type's unqualified name
    ComponentTypeId type;              // componentTypeId<C>()
    std::vector<FieldBinding> fields;  // declaration order
};

// A set of bindings in which every entry is valid, so the runtime that reads it trusts it. Every name is a view into
// static storage (the generated literals, or a test's), so the table never owns text.
class ComponentBindingTable {
public:
    // False, with one ERROR naming the script name and the rule and nothing appended, when the entry breaks a rule:
    // a script name that is empty or not an identifier ([A-Za-z_$][A-Za-z0-9_$]*), or that is Vec3, Quat or Entity
    // (the classes the namespace already holds); a name or a type already bound; an invalid type; the 128th
    // component; more than MAX_BOUND_FIELDS fields; a field name that is empty, repeated or `constructor`; a field
    // with no address.
    bool add(ComponentBinding binding);

    [[nodiscard]] std::span<const ComponentBinding> components() const noexcept;

private:
    std::vector<ComponentBinding> entries;
};

// The member pointer's class and value type.
template <typename M>
struct MemberOf;

template <typename C, typename T>
struct MemberOf<T C::*> {
    using Class = C;
    using Type = T;
};

template <auto Member>
[[nodiscard]] void* memberAddress(void* component) noexcept {
    using Class = typename MemberOf<decltype(Member)>::Class;
    return std::addressof(static_cast<Class*>(component)->*Member);
}

// A field known, at compile time, to belong to C.
template <typename C>
struct FieldOf {
    FieldBinding binding;
};

// Member is a pointer to a data member, &C::m.
template <auto Member>
[[nodiscard]] FieldOf<typename MemberOf<decltype(Member)>::Class> bindField(std::string_view name) {
    using Type = typename MemberOf<decltype(Member)>::Type;
    return {FieldBinding{name, fieldKindOf<Type>(), &memberAddress<Member>}};
}

template <typename C, typename... Fields>
    requires(std::same_as<Fields, FieldOf<C>> && ...)
[[nodiscard]] ComponentBinding bindComponent(std::string_view scriptName, Fields... fields) {
    static_assert(sizeof...(Fields) <= MAX_BOUND_FIELDS, "a bound component holds at most MAX_BOUND_FIELDS fields");
    return ComponentBinding{scriptName, componentTypeId<C>(), std::vector<FieldBinding>{fields.binding...}};
}

// The generated ten built-ins, or an empty table when reflect-gen is not built.
[[nodiscard]] const ComponentBindingTable& builtinComponentBindings();

}  // namespace engine::script::internal

#pragma once
// Aero Engine -- the ONE place the entt::meta arithmetic-type map lives (task 2.2.2), shared by
// component_ops.cpp and inspector_model.cpp so a subset change is a one-line edit here instead of a
// hunt through two switches. SRC-PRIVATE, entt-using -- included only by the two .cpp files above.

#include <aero/editor/component_ops.hpp>  // FieldKind

#include <entt/entt.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <type_traits>

namespace engine::editor {

// The 17 arithmetic types tools/reflect-gen's 18-CXTypeKind whitelist can produce (Char_U and Char_S
// both map to plain `char`, so 18 kinds => 17 C++ types), plus the three class types. ONE list, so a
// subset change is a one-line edit here instead of a hunt through two switches.
//
// WHY 17 AND NOT THE SPEC'S 15 (plan decision O2): omitting char / signed char / char16_t / char32_t /
// wchar_t / long / unsigned long would leave a SILENT hole -- reflect-gen accepts such a field,
// registers it in entt::meta, and the inspector then renders NOTHING for it, with no warning anywhere.
// "Accepted by the generator but invisible in the UI" is precisely the drift AC-12 exists to catch,
// and it is cheaper to close than to detect. THIS LIST AND classifyField's WHITELIST ARE A PAIR --
// change one, change the other (the defensive skip in inspector_model.cpp makes drift audible).
template <typename... Ts>
struct TypeList {};
using ArithmeticTypes =
    TypeList<bool, char, signed char, unsigned char, char16_t, char32_t, wchar_t, short, unsigned short, int,
             unsigned int, long, unsigned long, long long, unsigned long long, float, double>;

// A fold, not recursion -- misc-no-recursion has nothing to say about a pack expansion (world.hpp's
// applySlots makes the same point). Calls fn.template operator()<T>() for the ONE T whose entt type_id
// matches `info`; returns false when none does.
template <typename... Ts, typename Fn>
bool dispatchArithmetic(const entt::type_info& info, TypeList<Ts...> /*list*/, Fn&& fn) {
    return ((info == entt::type_id<Ts>() ? (fn.template operator()<Ts>(), true) : false) || ...);
}

// SIGNEDNESS IS ASKED, NEVER ASSUMED (O2). `char` is a DISTINCT type from both `signed char` and
// `unsigned char`, and whether it is signed is IMPLEMENTATION-DEFINED (unsigned on arm64 macOS/Linux,
// signed on x86-64). `wchar_t` is likewise implementation-signed (signed on macOS/Linux, unsigned
// 16-bit on Windows). char16_t/char32_t are always unsigned. std::is_signed_v<T> answers all of them
// at compile time on the host actually doing the build, so the Int-vs-UInt transport half is correct
// per-platform with no #ifdef and no table to keep in sync. Never hardcode a kind for these four.
template <typename T>
constexpr FieldKind kindOfArithmetic() {
    if constexpr (std::is_same_v<T, bool>) {
        return FieldKind::Bool;
    } else if constexpr (std::is_floating_point_v<T>) {
        return FieldKind::Float;
    } else if constexpr (std::is_signed_v<T>) {
        return FieldKind::Int;
    } else {
        return FieldKind::UInt;
    }
}

// name -> meta_type. The join key between the per-World registration table and the process-global meta
// context is the component NAME (D16) -- the same durable identity docs/09 uses. componentTypeName
// returns a string_view into World-owned storage; hashed_string::value takes (ptr, size), so no
// null-termination is assumed.
entt::meta_type resolveComponentMeta(std::string_view registrationName);

// ---- the seam's numeric clamp helpers (D8) -----------------------------------------------------
//
// Range-clamp happens in the WIDE domain (int64/uint64/double), preserving full precision for an
// IN-RANGE value; only the OUT-OF-RANGE bound round-trips through double, which is unavoidable since
// FieldUiMeta's rangeMin/rangeMax are doubles by design (one struct describes a range on a uint64 and
// on a float alike). ceil/floor on the bound is what keeps the clamp from ever WIDENING the admissible
// set (a range of [0.2, 9.8] must clamp an out-of-range integer to 1 or 9, never 0 or 10).
[[nodiscard]] std::int64_t clampRangeInt64(std::int64_t v, bool hasRange, double rangeMin, double rangeMax);
[[nodiscard]] std::uint64_t clampRangeUint64(std::uint64_t v, bool hasRange, double rangeMin, double rangeMax);
// NaN passes through unchanged (std::clamp's comparisons are both false for NaN -- E10).
[[nodiscard]] double clampRangeDouble(double v, bool hasRange, double rangeMin, double rangeMax);

// Narrows an ALREADY range-clamped wide value to T's own numeric domain, SATURATING rather than
// wrapping -- the whole reason the seam never lets EnTT do the conversion (C6: EnTT's own set() wraps
// 300 into a uint8_t as 44). Never routes an integral source through `double` (which would lose
// precision near the 64-bit extremes) except for the destination-is-floating-point case, where that
// loss is inherent to the destination type anyway.
template <typename T>
T narrowFromInt64(std::int64_t v) {
    if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(v);
    } else if constexpr (std::is_same_v<T, bool>) {
        return v != 0;
    } else if constexpr (std::is_signed_v<T>) {
        const auto lo = static_cast<std::int64_t>(std::numeric_limits<T>::lowest());
        const auto hi = static_cast<std::int64_t>(std::numeric_limits<T>::max());
        return static_cast<T>(std::clamp<std::int64_t>(v, lo, hi));
    } else {
        if (v < 0) {
            return T{0};
        }
        const auto hi = static_cast<std::uint64_t>(std::numeric_limits<T>::max());
        return static_cast<T>(std::min<std::uint64_t>(static_cast<std::uint64_t>(v), hi));
    }
}

template <typename T>
T narrowFromUint64(std::uint64_t v) {
    if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(v);
    } else if constexpr (std::is_same_v<T, bool>) {
        return v != 0;
    } else {
        // Both the signed and unsigned destination branches saturate the SAME way from an
        // unsigned source: min(v, destination max), since v is never negative here.
        const auto hi = static_cast<std::uint64_t>(std::numeric_limits<T>::max());
        return static_cast<T>(std::min<std::uint64_t>(v, hi));
    }
}

// Only used for T in {float, double} (the Float kind's own destination types).
template <typename T>
T narrowFromDouble(double v) {
    const double lo = static_cast<double>(std::numeric_limits<T>::lowest());
    const double hi = static_cast<double>(std::numeric_limits<T>::max());
    return static_cast<T>(std::clamp(v, lo, hi));  // NaN passes through -- both comparisons false
}

}  // namespace engine::editor

#include "meta_utils.hpp"

#include <cmath>
#include <limits>

namespace engine::editor {

namespace {

// Review finding 2: clamp `v` into T's representable domain BEFORE any cast to T --
// static_cast<uint64_t>(-1.0) and static_cast<int64_t>(1e300) are both UNDEFINED BEHAVIOUR
// ([conv.fpint]: the truncated value must be representable in the destination type), and this
// function is where the destination-domain bound ITSELF comes from a caller-supplied double
// (rangeMin/rangeMax), not from `v` -- so both the "negative bound reaches an unsigned width" case
// and the "magnitude too large for the destination" case must be handled, not just the one this
// task's own fixture happened to exercise. NaN has no ordering to clamp against, so it is handled
// first and separately, matching component_ops.hpp's "values are never validated" stance (E10).
template <typename T>
T doubleToClamped(double v) {
    if (v != v) {  // NaN
        return T{0};
    }
    const auto lo = static_cast<double>(std::numeric_limits<T>::lowest());
    const auto hi = static_cast<double>(std::numeric_limits<T>::max());
    if (v <= lo) {
        return std::numeric_limits<T>::lowest();
    }
    if (v >= hi) {
        return std::numeric_limits<T>::max();
    }
    return static_cast<T>(v);
}

}  // namespace

entt::meta_type resolveComponentMeta(std::string_view registrationName) {
    const entt::id_type id = entt::hashed_string::value(registrationName.data(), registrationName.size());
    return entt::resolve(id);
}

std::int64_t clampRangeInt64(std::int64_t v, bool hasRange, double rangeMin, double rangeMax) {
    if (!hasRange) {
        return v;
    }
    const auto d = static_cast<double>(v);
    if (d < rangeMin) {
        return doubleToClamped<std::int64_t>(std::ceil(rangeMin));
    }
    if (d > rangeMax) {
        return doubleToClamped<std::int64_t>(std::floor(rangeMax));
    }
    return v;
}

std::uint64_t clampRangeUint64(std::uint64_t v, bool hasRange, double rangeMin, double rangeMax) {
    if (!hasRange) {
        return v;
    }
    const auto d = static_cast<double>(v);
    if (d < rangeMin) {
        return doubleToClamped<std::uint64_t>(std::ceil(rangeMin));
    }
    if (d > rangeMax) {
        return doubleToClamped<std::uint64_t>(std::floor(rangeMax));
    }
    return v;
}

double clampRangeDouble(double v, bool hasRange, double rangeMin, double rangeMax) {
    if (!hasRange) {
        return v;
    }
    return std::clamp(v, rangeMin, rangeMax);  // NaN passes through -- both comparisons false (E10)
}

}  // namespace engine::editor

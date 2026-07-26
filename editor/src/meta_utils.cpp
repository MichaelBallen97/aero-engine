#include "meta_utils.hpp"

#include <cmath>

namespace engine::editor {

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
        return static_cast<std::int64_t>(std::ceil(rangeMin));
    }
    if (d > rangeMax) {
        return static_cast<std::int64_t>(std::floor(rangeMax));
    }
    return v;
}

std::uint64_t clampRangeUint64(std::uint64_t v, bool hasRange, double rangeMin, double rangeMax) {
    if (!hasRange) {
        return v;
    }
    const auto d = static_cast<double>(v);
    if (d < rangeMin) {
        return rangeMin <= 0.0 ? std::uint64_t{0} : static_cast<std::uint64_t>(std::ceil(rangeMin));
    }
    if (d > rangeMax) {
        return static_cast<std::uint64_t>(std::floor(rangeMax));
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

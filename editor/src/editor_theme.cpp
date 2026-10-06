// Aero Engine — resolveUiScale (task E.6.1, spec D8); the theme itself is editor_theme.hpp's one
// constexpr value.
#include <aero/editor/editor_theme.hpp>

#include <algorithm>
#include <cmath>

namespace engine::editor {

float resolveUiScale(float displayScale, float pixelDensity, float previous) noexcept {
    // 1. A previous that cannot be kept is replaced -- and a kept one is clamped like a computed one.
    const bool previousUsable = std::isfinite(previous) && previous > 0.0F;
    const float kept = previousUsable ? std::clamp(previous, UI_SCALE_MIN, UI_SCALE_MAX) : 1.0F;
    // 2. SDL_GetWindowPixelDensity divides pixel width by window width (SDL_video.c:1877-1889) and reads
    //    0/0 or x/0 for a minimised window on some backends; SDL_GetWindowDisplayScale returns 0 on error.
    //    The positive tests are written so a NaN fails them (`!(x > 0)`), never as `x <= 0`.
    if (!std::isfinite(displayScale) || !(displayScale > 0.0F) || !std::isfinite(pixelDensity) ||
        !(pixelDensity > 0.0F)) {
        return kept;
    }
    const float raw = displayScale / pixelDensity;
    if (!std::isfinite(raw)) {  // 3. finite / tiny overflows
        return kept;
    }
    // 4. Quantised by DIVISION: k * fl(0.05) is bit-unequal to fl(k / 20) for 14 of k in [10, 80] (US2).
    const float quantised = std::round(raw * UI_SCALE_STEPS_PER_UNIT) / UI_SCALE_STEPS_PER_UNIT;
    // 5. Clamped. `quantised` is finite or +inf here (raw * 20 can overflow), and std::clamp maps +inf to
    //    MAX.
    return std::clamp(quantised, UI_SCALE_MIN, UI_SCALE_MAX);
}

}  // namespace engine::editor

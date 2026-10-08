// Aero Engine — the status bar's pure half (task E.6.2, D12-D17). No ImGui: see status_bar.hpp.
#include <aero/editor/editor_glyphs.hpp>
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/status_bar.hpp>

#include <array>
#include <cmath>
#include <cstdio>

namespace engine::editor {

namespace {

constexpr std::string_view DOT = " " AERO_GLYPH_MIDDLE_DOT " ";

[[nodiscard]] float layoutScale(float uiScale) noexcept {  // copied, not shared (toolbar_model.cpp's note)
    return (std::isfinite(uiScale) && uiScale > 0.0F) ? uiScale : 1.0F;
}

[[nodiscard]] bool isSeparator(char c) noexcept { return c == '/' || c == '\\'; }

[[nodiscard]] std::string_view lastSegment(std::string_view path) noexcept {
    const std::size_t cut = path.find_last_of("/\\");
    // The pointer + size constructor, never substr (which may throw and trips bugprone-exception-escape in a
    // noexcept function -- the leafOf / assetNameForMeta precedent). cut + 1 <= size: find_last_of found a byte.
    const std::string_view leaf =
        cut == std::string_view::npos ? path : std::string_view(path.data() + cut + 1U, path.size() - cut - 1U);
    return leaf.empty() ? path : leaf;
}

}  // namespace

WatchState classifyWatchState(bool projectOpen, const WatchStatus& status) noexcept {
    if (!projectOpen) {
        return WatchState::NoProject;
    }
    if (!status.enabled) {
        return WatchState::Off;
    }
    if (status.rootUnreadable) {
        return WatchState::Unreadable;
    }
    if (status.truncated) {
        return WatchState::Partial;
    }
    if (status.deferredSweeps > 0) {
        return WatchState::Settling;
    }
    if (status.unreadableDirs > 0) {
        return WatchState::SomeUnreadable;
    }
    return WatchState::Watching;
}

std::string watchStateText(WatchState state, std::size_t unreadableDirs, std::size_t assetCount) {
    std::string text;
    switch (state) {  // NO default:
        case WatchState::NoProject:
            return {};
        case WatchState::Off:
            text = "auto-refresh off";
            break;
        case WatchState::Unreadable:
            text = "watch paused";
            break;
        case WatchState::Partial:
            text = "watching (partial)";
            break;
        case WatchState::Settling:
            text = "settling";
            break;
        case WatchState::SomeUnreadable:
            text = "watching (" + std::to_string(unreadableDirs) + " unreadable)";
            break;
        case WatchState::Watching:
            text = "watching";
            break;
    }
    text += DOT;
    text += assetCountText(assetCount);
    return text;
}

std::string assetFooterWatchText(WatchState state, std::uint32_t deferredSweeps, std::size_t unreadableDirs) {
    switch (state) {  // NO default: -- the six strings below are 3.1.4's, byte for byte (FT13)
        case WatchState::NoProject:
            return {};
        case WatchState::Off:
            return "Auto-refresh off";
        case WatchState::Unreadable:
            return "Watch paused -- assets folder unreadable";
        case WatchState::Partial:
            return "Watching (partial -- tree exceeds the scan limit)";
        case WatchState::Settling:
            return "Watching -- settling (" + std::to_string(deferredSweeps) + ")";
        case WatchState::SomeUnreadable:
            return "Watching (" + std::to_string(unreadableDirs) + " folder(s) unreadable)";
        case WatchState::Watching:
            return "Watching";
    }
    return {};
}

std::string abbreviateHome(std::string_view root, std::string_view home) {
    while (!home.empty() && isSeparator(home.back())) {
        home.remove_suffix(1);
    }
    if (home.empty()) {
        return std::string(root);
    }
    if (root == home) {
        return "~";
    }
    if (root.size() > home.size() && root.starts_with(home) && isSeparator(root[home.size()])) {
        return "~" + std::string(root.substr(home.size()));  // segment-wise: the separator follows
    }
    return std::string(root);
}

std::string elidePathLeft(std::string_view path, const CaptionLineFits& fits) {
    return subtitledTileCaptionLine(path, lastSegment(path), fits);
}

std::string groupThousands(std::uint64_t n) {
    const std::string digits = std::to_string(n);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0U && (digits.size() - i) % 3U == 0U) {
            out += ' ';  // counted from the RIGHT (seed S12 counts from the left)
        }
        out += digits[i];
    }
    return out;
}

std::string assetCountText(std::size_t n) {
    if (n == 1U) {
        return "1 asset";
    }
    return groupThousands(n) + " assets";
}

std::string backendDisplayName(std::string_view backendName) {
    if (backendName == "metal") {
        return "Metal";
    }
    if (backendName == "vulkan") {
        return "Vulkan";
    }
    if (backendName == "direct3d12") {
        return "Direct3D 12";
    }
    return std::string(backendName);
}

void FrameTimeReadout::add(float rawDeltaSeconds) noexcept {
    if (!std::isfinite(rawDeltaSeconds) || rawDeltaSeconds < 0.0F) {
        return;  // ignored, never counted as a frame
    }
    windowSeconds += static_cast<double>(rawDeltaSeconds);
    ++windowFrames;
    if (windowSeconds >= static_cast<double>(FRAME_READOUT_WINDOW_SECONDS)) {
        fpsValue = static_cast<float>(static_cast<double>(windowFrames) / windowSeconds);
        millisecondsValue = static_cast<float>(1000.0 * windowSeconds / static_cast<double>(windowFrames));
        publishedValue = true;
        windowSeconds = 0.0;
        windowFrames = 0;
    }
}

std::string frameReadoutText(const FrameTimeReadout& readout) {
    if (!readout.published()) {
        return std::string("-- fps") + std::string(DOT) + "-- ms";
    }
    std::array<char, 32> ms{};
    const int written = std::snprintf(ms.data(), ms.size(), "%.1f", static_cast<double>(readout.milliseconds()));
    return std::to_string(std::lround(readout.fps())) + " fps" + std::string(DOT) +
           (written > 0 ? std::string(ms.data()) : std::string("--")) + " ms";
}

StatusBarText statusBarText(bool projectOpen, std::string_view rootDisplay, std::string_view rootFull,
                            WatchState watchState, std::size_t unreadableDirs, std::size_t assetCount,
                            const FrameTimeReadout& readout, std::string_view backendLabel) {
    StatusBarText text;
    text.projectOpen = projectOpen;
    if (projectOpen) {
        text.root = std::string(rootDisplay);
        text.rootTooltip = std::string(rootFull);
        text.watch = watchStateText(watchState, unreadableDirs, assetCount);
    } else {
        text.watch = std::string(NO_PROJECT_STATUS_TEXT);
    }
    text.frame = frameReadoutText(readout);
    text.backend = std::string(backendLabel);
    return text;
}

StatusBarLayout statusBarLayout(float barWidth, const StatusBarText& text, const TextWidth& measure, float uiScale) {
    StatusBarLayout out;
    const float s = layoutScale(uiScale);
    const float width = (std::isfinite(barWidth) && barWidth > 0.0F) ? barWidth : 0.0F;
    const float pad = EDITOR_THEME.shell.statusBarPaddingX * s;
    const float gap = EDITOR_THEME.shell.statusBarZoneGap * s;
    const auto w = [&measure](std::string_view t) { return t.empty() ? 0.0F : measure(t); };
    const auto fitsWithin = [&measure](float budget) {
        return [&measure, budget](std::string_view line) { return measure(line) <= budget; };
    };
    const float ellipsis = w(AERO_GLYPH_ELLIPSIS);

    // (1) The right zone: whole, right-aligned. It never yields.
    out.frame = text.frame;
    out.backend = text.backend;
    const float rightWidth = w(text.frame) + (text.backend.empty() ? 0.0F : gap + w(text.backend));
    out.rightStart = width - pad - rightWidth;
    out.frameX = out.rightStart;
    out.backendX = out.frameX + w(text.frame) + (text.frame.empty() ? 0.0F : gap);

    const float budget = out.rightStart - gap - pad;  // what the left zone may use
    if (width <= 0.0F || !(budget > 0.0F)) {
        return out;  // (4) narrower than the right zone: no left zone at all
    }
    std::string root;
    std::string watch;
    if (text.root.empty()) {  // no project: "No project open" alone
        if (w(text.watch) <= budget) {
            watch = text.watch;
        } else if (budget >= ellipsis) {
            watch = elideCaptionRight(text.watch, fitsWithin(budget));
        }
    } else if (w(text.root) + gap + w(text.watch) <= budget) {
        root = text.root;
        watch = text.watch;
    } else {
        const std::string_view leaf = lastSegment(text.root);
        const std::string rootFloor =
            leaf == text.root ? std::string(leaf) : std::string(AERO_GLYPH_ELLIPSIS) + std::string(leaf);
        if (w(rootFloor) + gap + w(text.watch) <= budget) {
            root = elidePathLeft(text.root, fitsWithin(budget - gap - w(text.watch)));  // (2)
            watch = text.watch;
        } else if (budget - w(rootFloor) - gap >= ellipsis) {
            root = rootFloor;
            watch = elideCaptionRight(text.watch, fitsWithin(budget - w(rootFloor) - gap));  // (3)
        } else if (w(rootFloor) <= budget) {
            root = rootFloor;  // (4) the watcher goes first
        } else if (budget >= ellipsis) {
            root = elidePathLeft(text.root, fitsWithin(budget));  // (4) the last segment right-elided
        }
    }
    out.root = root;
    out.watch = watch;
    out.rootX = pad;
    out.watchX = root.empty() ? pad : pad + w(root) + gap;
    out.leftDrawn = !root.empty() || !watch.empty();
    out.leftEnd = watch.empty() ? (root.empty() ? pad : pad + w(root)) : out.watchX + w(watch);
    return out;
}

}  // namespace engine::editor

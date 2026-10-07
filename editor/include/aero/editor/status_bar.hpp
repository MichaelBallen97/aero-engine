#pragma once
// Aero Engine — the status bar's PURE half (task E.6.2, D12-D17): the watcher's one classification and its two
// vocabularies, the home-abbreviated root, the asset count, the backend's name, the frame readout and the bar's
// layout. The ImGui TU measures and draws; everything decided is here, at tier 0 (status_bar_test.cpp).
#include <aero/editor/asset_view.hpp>     // TextWidth, elideCaptionRight, subtitledTileCaptionLine
#include <aero/editor/asset_watcher.hpp>  // WatchStatus

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace engine::editor {

// ---- the watcher (D13) ---------------------------------------------------------------------------------------
// ONE classification, the Assets footer's exact precedence with one arm in front: `enabled` stays true with no
// project and `phase` reads Disabled for both "no project" and "auto-refresh off", so only projectOpen tells the
// first two apart. The enumerator order IS the precedence.
enum class WatchState : std::uint8_t { NoProject = 0, Off, Unreadable, Partial, Settling, SomeUnreadable, Watching };
[[nodiscard]] WatchState classifyWatchState(bool projectOpen, const WatchStatus& status) noexcept;

// The status bar's words: "watching", "settling", ... + " · " + assetCountText(assetCount). NoProject -> "" (the
// bar shows NO_PROJECT_STATUS_TEXT instead). `unreadableDirs` is printed for SomeUnreadable only.
[[nodiscard]] std::string watchStateText(WatchState state, std::size_t unreadableDirs, std::size_t assetCount);

// The Assets footer's words, MOVED here byte-identical from asset_browser_panel.cpp (3.1.4's chain, AC-36).
// NoProject -> "" (the footer never draws without a project; defined so the switch stays total).
[[nodiscard]] std::string assetFooterWatchText(WatchState state, std::uint32_t deferredSweeps,
                                               std::size_t unreadableDirs);

inline constexpr std::string_view NO_PROJECT_STATUS_TEXT = "No project open";

// ---- the root (D16) ------------------------------------------------------------------------------------------
// `home` replaced by "~" when it is a SEGMENT-WISE prefix of `root` -- equal, or followed by '/' or '\' -- else
// `root` unchanged. A trailing separator on `home` is ignored; an empty `home` disables the rule. Case-SENSITIVE
// and lexical, so a drive-letter case mismatch shows the full path (the safe direction). The SIXTH home of the
// segment-wise prefix rule: a raw starts_with would turn /Users/michael into ~chael under home /Users/mi.
[[nodiscard]] std::string abbreviateHome(std::string_view root, std::string_view home);

// `path` when it fits; else an ellipsis + the longest suffix that fits and still holds the whole LAST segment;
// else that segment right-elided -- subtitledTileCaptionLine with the last segment as its leaf, so no cut ever
// lands inside a UTF-8 sequence. A budget below the ellipsis itself returns the ellipsis alone, which does NOT
// fit: a caller gates on `budget >= w(ellipsis)` first, as statusBarLayout does.
[[nodiscard]] std::string elidePathLeft(std::string_view path, const CaptionLineFits& fits);

// ---- the count (D17) and the backend (D15) -------------------------------------------------------------------
[[nodiscard]] std::string groupThousands(std::uint64_t n);  // "1 284", "50 000": a plain ASCII space per three digits
[[nodiscard]] std::string assetCountText(std::size_t n);    // "1 asset", every other count "<n> assets"
// "metal" -> "Metal", "vulkan" -> "Vulkan", "direct3d12" -> "Direct3D 12"; an unknown non-empty name verbatim; "" ->
// "".
[[nodiscard]] std::string backendDisplayName(std::string_view backendName);

// ---- the frame readout (D14) ---------------------------------------------------------------------------------
inline constexpr float FRAME_READOUT_WINDOW_SECONDS = 0.5F;

// A windowed average of the RAW delta, published once the window holds FRAME_READOUT_WINDOW_SECONDS: fps =
// frames / seconds and ms = 1000 x seconds / frames, a consistent pair (fps x ms == 1000), which two EMAs are not.
// A non-finite or negative delta is ignored; one delta longer than the window publishes at once.
class FrameTimeReadout {
public:
    void add(float rawDeltaSeconds) noexcept;
    [[nodiscard]] bool published() const noexcept { return publishedValue; }
    [[nodiscard]] float fps() const noexcept { return fpsValue; }
    [[nodiscard]] float milliseconds() const noexcept { return millisecondsValue; }

private:
    double windowSeconds = 0.0;
    std::uint32_t windowFrames = 0;
    bool publishedValue = false;
    float fpsValue = 0.0F;
    float millisecondsValue = 0.0F;
};
// "-- fps · -- ms" before the first publication; then "<fps rounded> fps · <ms to one decimal> ms".
[[nodiscard]] std::string frameReadoutText(const FrameTimeReadout& readout);

// ---- the bar (D12) --------------------------------------------------------------------------------------------
struct StatusBarText {
    bool projectOpen = false;
    std::string root;         // home-abbreviated; "" with no project
    std::string rootTooltip;  // the full root
    std::string watch;        // watchStateText, or NO_PROJECT_STATUS_TEXT
    std::string frame;        // frameReadoutText
    std::string backend;      // backendDisplayName
    bool operator==(const StatusBarText&) const = default;
};
[[nodiscard]] StatusBarText statusBarText(bool projectOpen, std::string_view rootDisplay, std::string_view rootFull,
                                          WatchState watchState, std::size_t unreadableDirs, std::size_t assetCount,
                                          const FrameTimeReadout& readout, std::string_view backendLabel);

// Every x is BAR-LOCAL (from the status bar window's left edge).
struct StatusBarLayout {
    std::string root;  // each as drawn; "" when not drawn
    float rootX = 0.0F;
    std::string watch;
    float watchX = 0.0F;
    bool leftDrawn = false;
    float leftEnd = 0.0F;
    std::string frame;
    float frameX = 0.0F;
    std::string backend;
    float backendX = 0.0F;
    float rightStart = 0.0F;
    bool operator==(const StatusBarLayout&) const = default;
};
// THE ORDER IS FIXED: (1) the right zone takes its full width, right-aligned at barWidth - statusBarPaddingX;
// (2) the root shrinks through elidePathLeft to its floor (ellipsis + its last segment); (3) only then is the
// watcher right-elided; (4) last, the watcher goes and the root's last segment is right-elided. A barWidth
// narrower than the right zone alone -- or NaN, negative or zero, sanitised to 0 -- draws no left zone, and the
// right zone is clipped by the window, never overlapped. `uiScale` is NON-DEFAULTED and multiplies
// statusBarPaddingX and statusBarZoneGap exactly once (US9); a bad scale is 1.
[[nodiscard]] StatusBarLayout statusBarLayout(float barWidth, const StatusBarText& text, const TextWidth& measure,
                                              float uiScale);

}  // namespace engine::editor

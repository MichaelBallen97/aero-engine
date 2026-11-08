#pragma once
// Aero Engine -- the play session (task 4.7.1, D1-D11). PURE: no ImGui, no SDL, no GPU, no logging -- so every rule
// is a tier-0 table (play_session_test.cpp). Tier 0.
//
// The mode (Edit, Playing, Paused) has ONE writer, PlaySession::apply. The functions below are the rules the chrome
// and the panels read: which control is enabled, which draws active, what a chord asks for, what a tooltip says,
// whether the scene is locked. PlaySession also owns the game's clock (D6) and the game's own platform::InputState
// (D5), fed from the editor's event stream and made empty at Play, so the editor's input is never the game's.
#include <aero/core/math.hpp>
#include <aero/editor/project.hpp>
#include <aero/editor/shortcut_hint.hpp>
#include <aero/platform/event.hpp>
#include <aero/platform/input.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace engine::editor {

enum class PlayMode : std::uint8_t { Edit = 0, Playing, Paused };
enum class PlayControl : std::uint8_t { Play = 0, Pause, Step, Stop };  // TOOLBAR_PLAY_CONTROLS' order
inline constexpr std::size_t PLAY_CONTROL_COUNT = 4;
enum class PlayAvailability : std::uint8_t { Available = 0, NoProject, CppProject };
enum class PlayChord : std::uint8_t { PlayOrStop = 0, PauseOrResume, Step };  // Cmd/Ctrl+P, +Shift+P, +Alt+P
enum class PlayTransition : std::uint8_t { None = 0, Started, Paused, Resumed, StepQueued, Stopped };
inline constexpr float PLAY_STEP_SECONDS = 1.0F / 60.0F;  // D6; the C++ mini-game's FIXED_DT

// Play is available only in an open TypeScript project (D3): a C++ game runs as its own build.
[[nodiscard]] PlayAvailability playAvailability(bool projectOpen, ProjectLanguage language) noexcept;
[[nodiscard]] constexpr bool sceneLocked(PlayMode mode) noexcept { return mode != PlayMode::Edit; }
// `inputFree` is false behind a modal or a native dialog. Availability gates Play alone: Play is the only way out of
// Edit, so the other three are already off by mode, and a session is never stranded without a Stop.
[[nodiscard]] bool playControlEnabled(PlayControl control, PlayMode mode, PlayAvailability availability,
                                      bool inputFree) noexcept;
[[nodiscard]] bool playControlDrawnActive(PlayControl control, PlayMode mode) noexcept;  // Play / Pause only (D10)
[[nodiscard]] PlayMode nextPlayMode(PlayMode mode, PlayControl control) noexcept;  // an ineligible pair returns `mode`
// nullopt when the chord asks for nothing in `mode`; the chrome then applies playControlEnabled.
[[nodiscard]] std::optional<PlayControl> playControlForChord(PlayChord chord, PlayMode mode) noexcept;
[[nodiscard]] std::string playControlTooltip(PlayControl control, PlayAvailability availability, HostOs os);
[[nodiscard]] std::string playStatusText(PlayMode mode, std::uint64_t frame);  // "" in Edit
[[nodiscard]] bool gameHasFocus(PlayMode mode, bool viewportHoldsKeyboard, bool windowFocused) noexcept;
// A key-down of P carrying the host's command modifier: a play chord, never the game's (D8).
[[nodiscard]] bool isPlayChordKey(const platform::KeyData& key, HostOs os) noexcept;

struct GameFrame {
    bool runs = false;
    float deltaSeconds = 0.0F;
};

// The viewport image's rectangle in window points, [min, max).
struct GameImageRect {
    Vec2 min{};
    Vec2 max{};
    bool valid = false;
};

class PlaySession {
public:
    [[nodiscard]] PlayMode mode() const noexcept { return modeValue; }
    [[nodiscard]] bool active() const noexcept { return sceneLocked(modeValue); }
    [[nodiscard]] std::uint64_t frame() const noexcept { return frameValue; }
    [[nodiscard]] bool stepQueued() const noexcept { return stepQueuedValue; }
    [[nodiscard]] bool gameFocused() const noexcept { return focusValue; }
    // The game's state: made empty at Play, dropped at Stop. 4.7.4 binds it with Vm::bindInput.
    [[nodiscard]] const platform::InputState& input() const noexcept { return game; }

    // The ONE writer of the mode.
    PlayTransition apply(PlayControl control) noexcept;
    // D6: Playing runs a frame with `clampedDeltaSeconds`; Paused with a step queued runs one of PLAY_STEP_SECONDS.
    [[nodiscard]] GameFrame advance(float clampedDeltaSeconds) noexcept;
    // D11: the caller calls this only after a game frame ran, so an edge made while paused reaches the next frame.
    void endGameFrame() noexcept;

    // The game's input feed (D5, D8, D9).
    void setGameFocus(bool focused) noexcept;
    void feed(const platform::Event& event, const GameImageRect& image, HostOs os) noexcept;
    void adoptConnectedPads(const platform::InputState& editorInput) noexcept;

private:
    PlayMode modeValue = PlayMode::Edit;
    std::uint64_t frameValue = 0;
    bool stepQueuedValue = false;
    bool focusValue = false;
    platform::InputState game{};
    Vec2 pointer{};  // the last absolute pointer position fed (window points), for the wheel's over-image test
};

}  // namespace engine::editor

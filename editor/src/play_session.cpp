// Aero Engine -- the play session's rules and clock (task 4.7.1). PURE: no ImGui, no SDL, no GPU, no logging. The
// game's input feed (feed, setGameFocus, adoptConnectedPads) is filled by the next step.
#include <aero/editor/editor_glyphs.hpp>  // AERO_GLYPH_MIDDLE_DOT
#include <aero/editor/play_session.hpp>

namespace engine::editor {

PlayAvailability playAvailability(bool projectOpen, ProjectLanguage language) noexcept {
    if (!projectOpen) {
        return PlayAvailability::NoProject;
    }
    return language == ProjectLanguage::Cpp ? PlayAvailability::CppProject : PlayAvailability::Available;
}

bool playControlEnabled(PlayControl control, PlayMode mode, PlayAvailability availability, bool inputFree) noexcept {
    if (!inputFree) {
        return false;
    }
    switch (control) {  // NO default: -- a fifth control is a clang-diagnostic-switch failure on the lint lane
        case PlayControl::Play:
            return mode == PlayMode::Edit && availability == PlayAvailability::Available;
        case PlayControl::Pause:
        case PlayControl::Stop:
            return mode != PlayMode::Edit;
        case PlayControl::Step:
            return mode == PlayMode::Paused;
    }
    return false;
}

bool playControlDrawnActive(PlayControl control, PlayMode mode) noexcept {
    return (control == PlayControl::Play && mode == PlayMode::Playing) ||
           (control == PlayControl::Pause && mode == PlayMode::Paused);
}

PlayMode nextPlayMode(PlayMode mode, PlayControl control) noexcept {
    switch (mode) {
        case PlayMode::Edit:
            return control == PlayControl::Play ? PlayMode::Playing : PlayMode::Edit;
        case PlayMode::Playing:
            if (control == PlayControl::Pause) {
                return PlayMode::Paused;
            }
            return control == PlayControl::Stop ? PlayMode::Edit : PlayMode::Playing;
        case PlayMode::Paused:
            if (control == PlayControl::Pause) {
                return PlayMode::Playing;
            }
            return control == PlayControl::Stop ? PlayMode::Edit : PlayMode::Paused;
    }
    return mode;
}

std::optional<PlayControl> playControlForChord(PlayChord chord, PlayMode mode) noexcept {
    switch (chord) {
        case PlayChord::PlayOrStop:
            return mode == PlayMode::Edit ? PlayControl::Play : PlayControl::Stop;
        case PlayChord::PauseOrResume:
            if (mode == PlayMode::Edit) {
                return std::nullopt;
            }
            return PlayControl::Pause;
        case PlayChord::Step:
            if (mode == PlayMode::Paused) {
                return PlayControl::Step;
            }
            return std::nullopt;
    }
    return std::nullopt;
}

std::string playControlTooltip(PlayControl control, PlayAvailability availability, HostOs os) {
    switch (availability) {
        case PlayAvailability::NoProject:
            return "No project is open";
        case PlayAvailability::CppProject:
            return "A C++ game runs as its own build";
        case PlayAvailability::Available:
            break;
    }
    switch (control) {
        case PlayControl::Play:
            return "Play (" + chordHint(os, {.ctrl = true}, "P") + ")";
        case PlayControl::Pause:
            return "Pause or resume (" + chordHint(os, {.ctrl = true, .shift = true}, "P") + ")";
        case PlayControl::Step:
            return "Step one frame while paused (" + chordHint(os, {.ctrl = true, .alt = true}, "P") + ")";
        case PlayControl::Stop:
            return "Stop (" + chordHint(os, {.ctrl = true}, "P") + ")";
    }
    return {};
}

std::string playStatusText(PlayMode mode, std::uint64_t frame) {
    switch (mode) {
        case PlayMode::Edit:
            return {};
        case PlayMode::Playing:
            return "Playing " AERO_GLYPH_MIDDLE_DOT " frame " + std::to_string(frame);
        case PlayMode::Paused:
            return "Paused " AERO_GLYPH_MIDDLE_DOT " frame " + std::to_string(frame);
    }
    return {};
}

bool gameHasFocus(PlayMode mode, bool viewportHoldsKeyboard, bool windowFocused) noexcept {
    return mode != PlayMode::Edit && viewportHoldsKeyboard && windowFocused;
}

bool isPlayChordKey(const platform::KeyData& /*key*/, HostOs /*os*/) noexcept {
    return false;  // the next step
}

PlayTransition PlaySession::apply(PlayControl control) noexcept {
    const PlayMode before = modeValue;
    const PlayMode after = nextPlayMode(before, control);
    PlayTransition transition = PlayTransition::None;
    if (before == PlayMode::Edit) {
        if (after == PlayMode::Playing) {
            transition = PlayTransition::Started;
        }
    } else if (after == PlayMode::Edit) {
        transition = PlayTransition::Stopped;
    } else if (before == PlayMode::Playing && after == PlayMode::Paused) {
        transition = PlayTransition::Paused;
    } else if (before == PlayMode::Paused && after == PlayMode::Playing) {
        transition = PlayTransition::Resumed;
    } else if (before == PlayMode::Paused && control == PlayControl::Step) {
        transition = PlayTransition::StepQueued;
    }
    modeValue = after;
    switch (transition) {
        case PlayTransition::Started:
            frameValue = 0;
            game = {};
            stepQueuedValue = false;
            focusValue = false;
            pointer = {};
            break;
        case PlayTransition::StepQueued:
            stepQueuedValue = true;
            break;
        case PlayTransition::Stopped:
            game = {};
            stepQueuedValue = false;
            focusValue = false;
            break;
        case PlayTransition::None:
        case PlayTransition::Paused:
        case PlayTransition::Resumed:
            break;
    }
    return transition;
}

GameFrame PlaySession::advance(float clampedDeltaSeconds) noexcept {
    if (modeValue == PlayMode::Playing) {
        ++frameValue;
        return {.runs = true, .deltaSeconds = clampedDeltaSeconds};
    }
    if (modeValue == PlayMode::Paused && stepQueuedValue) {
        stepQueuedValue = false;
        ++frameValue;
        return {.runs = true, .deltaSeconds = PLAY_STEP_SECONDS};
    }
    return {};
}

void PlaySession::endGameFrame() noexcept { game.newFrame(); }

void PlaySession::setGameFocus(bool /*focused*/) noexcept {}

void PlaySession::feed(const platform::Event& /*event*/, const GameImageRect& /*image*/, HostOs /*os*/) noexcept {}

void PlaySession::adoptConnectedPads(const platform::InputState& /*editorInput*/) noexcept {}

}  // namespace engine::editor

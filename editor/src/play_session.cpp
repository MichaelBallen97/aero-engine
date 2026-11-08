// Aero Engine -- the play session's rules and clock (task 4.7.1). PURE: no ImGui, no SDL, no GPU, no logging. The
// game's input feed (D5, D8, D9) is the last third of the file.
#include <aero/editor/editor_glyphs.hpp>  // AERO_GLYPH_MIDDLE_DOT
#include <aero/editor/play_session.hpp>

#include <cstddef>

namespace engine::editor {

namespace {

[[nodiscard]] bool insideImage(const GameImageRect& image, float x, float y) noexcept {
    return image.valid && x >= image.min.x && x < image.max.x && y >= image.min.y && y < image.max.y;
}

}  // namespace

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

bool isPlayChordKey(const platform::KeyData& key, HostOs os) noexcept {
    // ImGuiMod_Ctrl is the Command key on macOS, so the chord's modifier is GUI there and Control elsewhere.
    const bool command = os == HostOs::MacOs ? key.mods.gui() : key.mods.ctrl();
    return key.code == platform::Key::P && command;
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

void PlaySession::setGameFocus(bool focused) noexcept {
    const bool next = focused && modeValue != PlayMode::Edit;
    if (focusValue && !next) {
        // A focus loss sends no release: the game gets one for everything it holds (A5), buttons only (D9).
        for (auto k = static_cast<std::size_t>(platform::Key::A); k < static_cast<std::size_t>(platform::Key::Count);
             ++k) {
            const auto code = static_cast<platform::Key>(k);
            if (game.keyDown(code)) {
                platform::Event ev;
                ev.type = platform::EventType::KeyUp;
                ev.key.code = code;
                game.process(ev);
            }
        }
        for (auto b = static_cast<std::size_t>(platform::MouseButton::Left);
             b < static_cast<std::size_t>(platform::MouseButton::Count); ++b) {
            const auto button = static_cast<platform::MouseButton>(b);
            if (game.mouseButtonDown(button)) {
                platform::Event ev;
                ev.type = platform::EventType::MouseButtonUp;
                ev.mouseButton.button = button;
                ev.mouseButton.x = game.mousePosition().x;
                ev.mouseButton.y = game.mousePosition().y;
                game.process(ev);
            }
        }
        for (std::size_t slot = 0; slot < platform::MAX_GAMEPADS; ++slot) {
            if (!game.gamepadConnected(slot)) {
                continue;
            }
            for (auto b = static_cast<std::size_t>(platform::GamepadButton::South);
                 b < static_cast<std::size_t>(platform::GamepadButton::Count); ++b) {
                const auto button = static_cast<platform::GamepadButton>(b);
                if (game.gamepadButtonDown(button, slot)) {
                    platform::Event ev;
                    ev.type = platform::EventType::GamepadButtonUp;
                    ev.gamepadButton.slot = static_cast<std::uint8_t>(slot);
                    ev.gamepadButton.button = button;
                    game.process(ev);
                }
            }
        }
    }
    focusValue = next;
}

void PlaySession::feed(const platform::Event& event, const GameImageRect& image, HostOs os) noexcept {
    using platform::EventType;
    if (modeValue == PlayMode::Edit) {
        return;
    }
    // The pointer is tracked before the focus check, so the wheel's over-image test knows where it is on return.
    if (event.type == EventType::MouseMoved) {
        pointer = {event.mouseMotion.x, event.mouseMotion.y};
    } else if (event.type == EventType::MouseButtonDown || event.type == EventType::MouseButtonUp) {
        pointer = {event.mouseButton.x, event.mouseButton.y};
    }
    if (event.type == EventType::GamepadConnected || event.type == EventType::GamepadDisconnected) {
        game.process(event);  // a pad is the game's whether or not the Viewport holds the keyboard
        return;
    }
    if (!focusValue) {
        return;
    }
    platform::Event moved = event;
    switch (event.type) {
        case EventType::KeyDown:
            if (!isPlayChordKey(event.key, os)) {
                game.process(event);
            }
            break;
        case EventType::KeyUp:
            if (game.keyDown(event.key.code)) {
                game.process(event);
            }
            break;
        case EventType::MouseButtonDown:
            if (insideImage(image, event.mouseButton.x, event.mouseButton.y)) {
                moved.mouseButton.x -= image.min.x;
                moved.mouseButton.y -= image.min.y;
                game.process(moved);
            }
            break;
        case EventType::MouseButtonUp:
            if (game.mouseButtonDown(event.mouseButton.button)) {
                moved.mouseButton.x -= image.min.x;
                moved.mouseButton.y -= image.min.y;
                game.process(moved);
            }
            break;
        case EventType::MouseMoved:
            if (image.valid) {
                moved.mouseMotion.x -= image.min.x;
                moved.mouseMotion.y -= image.min.y;
                game.process(moved);
            }
            break;
        case EventType::MouseWheel:
            if (insideImage(image, pointer.x, pointer.y)) {
                game.process(event);
            }
            break;
        case EventType::GamepadButtonDown:
        case EventType::GamepadAxisMoved:
            game.process(event);
            break;
        case EventType::GamepadButtonUp:
            if (game.gamepadButtonDown(event.gamepadButton.button, event.gamepadButton.slot)) {
                game.process(event);
            }
            break;
        case EventType::None:
        case EventType::Quit:
        case EventType::WindowClose:
        case EventType::WindowResized:
        case EventType::WindowPixelSizeChanged:
        case EventType::WindowFocusGained:
        case EventType::WindowFocusLost:
        case EventType::WindowMinimized:
        case EventType::WindowRestored:
        case EventType::GamepadConnected:
        case EventType::GamepadDisconnected:
            break;  // a window event would clear held state with no edge (A5); the pads were handled above
    }
}

void PlaySession::adoptConnectedPads(const platform::InputState& editorInput) noexcept {
    for (std::size_t slot = 0; slot < platform::MAX_GAMEPADS; ++slot) {
        if (editorInput.gamepadConnected(slot)) {
            platform::Event ev;
            ev.type = platform::EventType::GamepadConnected;
            ev.gamepad.slot = static_cast<std::uint8_t>(slot);
            game.process(ev);
        }
    }
}

}  // namespace engine::editor

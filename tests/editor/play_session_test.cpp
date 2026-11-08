// tests/editor/play_session_test.cpp -- task 4.7.1: the play session's rules (PY) and, from Step 7, the game's input
// feed (GI). A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp. Tier 0: no window, no GPU,
// no ImGui. No #if of any kind.
#include <aero/editor/play_session.hpp>
#include <aero/platform/event.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>  // MSVC: a CHECK over a std::string_view needs the complete std::ostream (the 0.4.1 trap)
#include <string>
#include <string_view>

using engine::editor::gameHasFocus;
using engine::editor::GameImageRect;
using engine::editor::HostOs;
using engine::editor::nextPlayMode;
using engine::editor::PLAY_CONTROL_COUNT;
using engine::editor::PLAY_STEP_SECONDS;
using engine::editor::playAvailability;
using engine::editor::PlayAvailability;
using engine::editor::PlayChord;
using engine::editor::PlayControl;
using engine::editor::playControlDrawnActive;
using engine::editor::playControlEnabled;
using engine::editor::playControlForChord;
using engine::editor::playControlTooltip;
using engine::editor::PlayMode;
using engine::editor::PlaySession;
using engine::editor::playStatusText;
using engine::editor::PlayTransition;
using engine::editor::ProjectLanguage;
using engine::editor::sceneLocked;
using engine::platform::Event;
using engine::platform::EventType;
using engine::platform::GamepadAxis;
using engine::platform::GamepadButton;
using engine::platform::InputState;
using engine::platform::Key;
using engine::platform::KeyMod;
using engine::platform::MouseButton;

namespace {

constexpr std::array<PlayMode, 3> MODES{PlayMode::Edit, PlayMode::Playing, PlayMode::Paused};
constexpr std::array<PlayControl, PLAY_CONTROL_COUNT> CONTROLS{PlayControl::Play, PlayControl::Pause, PlayControl::Step,
                                                               PlayControl::Stop};
constexpr std::array<PlayAvailability, 3> AVAILABILITIES{PlayAvailability::Available, PlayAvailability::NoProject,
                                                         PlayAvailability::CppProject};

// Reaches `mode` from a fresh session through apply(), the one writer.
PlaySession sessionIn(PlayMode mode) {
    PlaySession session;
    if (mode == PlayMode::Edit) {
        return session;
    }
    session.apply(PlayControl::Play);
    if (mode == PlayMode::Paused) {
        session.apply(PlayControl::Pause);
    }
    return session;
}

// The table of nextPlayMode, spelt row by row (not derived from the code under test).
PlayMode expectedNext(PlayMode mode, PlayControl control) {
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

PlayTransition expectedTransition(PlayMode mode, PlayControl control) {
    if (mode == PlayMode::Edit) {
        return control == PlayControl::Play ? PlayTransition::Started : PlayTransition::None;
    }
    if (control == PlayControl::Stop) {
        return PlayTransition::Stopped;
    }
    if (mode == PlayMode::Playing) {
        return control == PlayControl::Pause ? PlayTransition::Paused : PlayTransition::None;
    }
    if (control == PlayControl::Pause) {
        return PlayTransition::Resumed;
    }
    return control == PlayControl::Step ? PlayTransition::StepQueued : PlayTransition::None;
}

// ---- the game's input feed (GI): events built field by field, the union's rule (event.hpp) ----

Event keyEvent(EventType type, Key code, KeyMod mod = KeyMod::None, bool repeat = false) {
    Event ev;
    ev.type = type;
    ev.key.code = code;
    ev.key.mods.bits = static_cast<std::uint16_t>(mod);
    ev.key.repeat = repeat;
    return ev;
}

Event buttonEvent(EventType type, MouseButton button, float x, float y) {
    Event ev;
    ev.type = type;
    ev.mouseButton.button = button;
    ev.mouseButton.x = x;
    ev.mouseButton.y = y;
    ev.mouseButton.clicks = 1;
    return ev;
}

Event moveEvent(float x, float y, float dx, float dy) {
    Event ev;
    ev.type = EventType::MouseMoved;
    ev.mouseMotion.x = x;
    ev.mouseMotion.y = y;
    ev.mouseMotion.dx = dx;
    ev.mouseMotion.dy = dy;
    return ev;
}

Event wheelEvent(float y) {
    Event ev;
    ev.type = EventType::MouseWheel;
    ev.mouseWheel.x = 0.0F;
    ev.mouseWheel.y = y;
    return ev;
}

Event padEvent(EventType type, std::uint8_t slot) {
    Event ev;
    ev.type = type;
    ev.gamepad.slot = slot;
    return ev;
}

Event padButtonEvent(EventType type, GamepadButton button, std::uint8_t slot) {
    Event ev;
    ev.type = type;
    ev.gamepadButton.slot = slot;
    ev.gamepadButton.button = button;
    return ev;
}

Event padAxisEvent(GamepadAxis axis, std::int16_t value, std::uint8_t slot) {
    Event ev;
    ev.type = EventType::GamepadAxisMoved;
    ev.gamepadAxis.slot = slot;
    ev.gamepadAxis.axis = axis;
    ev.gamepadAxis.value = value;
    return ev;
}

GameImageRect imageRect() {
    GameImageRect rect;
    rect.min = {100.0F, 40.0F};
    rect.max = {420.0F, 280.0F};
    rect.valid = true;
    return rect;
}

// A Playing session whose game has the keyboard.
PlaySession focusedSession() {
    PlaySession session = sessionIn(PlayMode::Playing);
    session.setGameFocus(true);
    return session;
}

}  // namespace

TEST_CASE("play session: every (mode, control) pair moves the mode and reports its transition (task 4.7.1, PY1)") {
    for (const PlayMode mode : MODES) {
        for (const PlayControl control : CONTROLS) {
            CAPTURE(static_cast<int>(mode));
            CAPTURE(static_cast<int>(control));
            CHECK((nextPlayMode(mode, control) == expectedNext(mode, control)));
            PlaySession session = sessionIn(mode);
            REQUIRE((session.mode() == mode));
            CHECK((session.apply(control) == expectedTransition(mode, control)));
            CHECK((session.mode() == expectedNext(mode, control)));
        }
    }
}

TEST_CASE("play session: a control is enabled by mode, availability and a free input (task 4.7.1, PY2)") {
    for (const PlayControl control : CONTROLS) {
        for (const PlayMode mode : MODES) {
            for (const PlayAvailability availability : AVAILABILITIES) {
                for (const bool inputFree : {false, true}) {
                    CAPTURE(static_cast<int>(control));
                    CAPTURE(static_cast<int>(mode));
                    CAPTURE(static_cast<int>(availability));
                    CAPTURE(inputFree);
                    bool expected = false;
                    if (inputFree) {
                        switch (control) {
                            case PlayControl::Play:
                                expected = mode == PlayMode::Edit && availability == PlayAvailability::Available;
                                break;
                            case PlayControl::Pause:
                            case PlayControl::Stop:
                                expected = mode != PlayMode::Edit;
                                break;
                            case PlayControl::Step:
                                expected = mode == PlayMode::Paused;
                                break;
                        }
                    }
                    CHECK((playControlEnabled(control, mode, availability, inputFree) == expected));
                }
            }
        }
    }
}

TEST_CASE("play session: only Play while playing and Pause while paused draw active (task 4.7.1, PY3)") {
    for (const PlayControl control : CONTROLS) {
        for (const PlayMode mode : MODES) {
            CAPTURE(static_cast<int>(control));
            CAPTURE(static_cast<int>(mode));
            const bool expected = (control == PlayControl::Play && mode == PlayMode::Playing) ||
                                  (control == PlayControl::Pause && mode == PlayMode::Paused);
            CHECK((playControlDrawnActive(control, mode) == expected));
        }
    }
}

TEST_CASE("play session: a chord names its control by the mode it is pressed in (task 4.7.1, PY4)") {
    using Opt = std::optional<PlayControl>;
    // PlayOrStop: Play in Edit, Stop otherwise.
    CHECK((playControlForChord(PlayChord::PlayOrStop, PlayMode::Edit) == Opt{PlayControl::Play}));
    CHECK((playControlForChord(PlayChord::PlayOrStop, PlayMode::Playing) == Opt{PlayControl::Stop}));
    CHECK((playControlForChord(PlayChord::PlayOrStop, PlayMode::Paused) == Opt{PlayControl::Stop}));
    // PauseOrResume: Pause unless Edit (apply turns Pause into Resume while paused).
    CHECK((playControlForChord(PlayChord::PauseOrResume, PlayMode::Edit) == Opt{}));
    CHECK((playControlForChord(PlayChord::PauseOrResume, PlayMode::Playing) == Opt{PlayControl::Pause}));
    CHECK((playControlForChord(PlayChord::PauseOrResume, PlayMode::Paused) == Opt{PlayControl::Pause}));
    // Step: only while paused.
    CHECK((playControlForChord(PlayChord::Step, PlayMode::Edit) == Opt{}));
    CHECK((playControlForChord(PlayChord::Step, PlayMode::Playing) == Opt{}));
    CHECK((playControlForChord(PlayChord::Step, PlayMode::Paused) == Opt{PlayControl::Step}));
}

TEST_CASE("play session: a project decides whether Play is available (task 4.7.1, PY5)") {
    for (const ProjectLanguage language : {ProjectLanguage::Ts, ProjectLanguage::Cpp}) {
        CAPTURE(static_cast<int>(language));
        CHECK((playAvailability(false, language) == PlayAvailability::NoProject));
    }
    CHECK((playAvailability(true, ProjectLanguage::Ts) == PlayAvailability::Available));
    CHECK((playAvailability(true, ProjectLanguage::Cpp) == PlayAvailability::CppProject));
}

TEST_CASE("play session: a tooltip names its chord, or why the control is off (task 4.7.1, PY6)") {
    CHECK(playControlTooltip(PlayControl::Play, PlayAvailability::Available, HostOs::MacOs) == "Play (Cmd+P)");
    CHECK(playControlTooltip(PlayControl::Pause, PlayAvailability::Available, HostOs::MacOs) ==
          "Pause or resume (Cmd+Shift+P)");
    CHECK(playControlTooltip(PlayControl::Step, PlayAvailability::Available, HostOs::MacOs) ==
          "Step one frame while paused (Cmd+Option+P)");
    CHECK(playControlTooltip(PlayControl::Stop, PlayAvailability::Available, HostOs::MacOs) == "Stop (Cmd+P)");
    for (const HostOs os : {HostOs::Windows, HostOs::Linux}) {
        CAPTURE(static_cast<int>(os));
        CHECK(playControlTooltip(PlayControl::Play, PlayAvailability::Available, os) == "Play (Ctrl+P)");
        CHECK(playControlTooltip(PlayControl::Pause, PlayAvailability::Available, os) ==
              "Pause or resume (Ctrl+Shift+P)");
        CHECK(playControlTooltip(PlayControl::Step, PlayAvailability::Available, os) ==
              "Step one frame while paused (Ctrl+Alt+P)");
        CHECK(playControlTooltip(PlayControl::Stop, PlayAvailability::Available, os) == "Stop (Ctrl+P)");
    }
    for (const HostOs os : {HostOs::Windows, HostOs::MacOs, HostOs::Linux}) {
        for (const PlayControl control : CONTROLS) {
            CAPTURE(static_cast<int>(os));
            CAPTURE(static_cast<int>(control));
            CHECK(playControlTooltip(control, PlayAvailability::NoProject, os) == "No project is open");
            CHECK(playControlTooltip(control, PlayAvailability::CppProject, os) == "A C++ game runs as its own build");
            for (const PlayAvailability availability : AVAILABILITIES) {
                for (const char c : playControlTooltip(control, availability, os)) {
                    CHECK(static_cast<unsigned char>(c) < 0x80U);
                }
            }
        }
    }
}

TEST_CASE("play session: the clock runs a frame per tick while playing and per Step while paused (task 4.7.1, PY7)") {
    PlaySession session;
    CHECK((session.apply(PlayControl::Play) == PlayTransition::Started));
    CHECK(session.frame() == 0);
    for (std::uint64_t i = 1; i <= 3; ++i) {
        const auto frame = session.advance(0.02F);
        CHECK(frame.runs);
        CHECK(frame.deltaSeconds == 0.02F);
        CHECK(session.frame() == i);
    }
    CHECK((session.apply(PlayControl::Pause) == PlayTransition::Paused));
    CHECK_FALSE(session.advance(0.02F).runs);
    CHECK(session.frame() == 3);

    CHECK((session.apply(PlayControl::Step) == PlayTransition::StepQueued));
    CHECK(session.stepQueued());
    const auto stepped = session.advance(0.25F);  // a step never uses the editor's delta
    CHECK(stepped.runs);
    CHECK(stepped.deltaSeconds == PLAY_STEP_SECONDS);
    CHECK(session.frame() == 4);
    CHECK_FALSE(session.stepQueued());
    CHECK_FALSE(session.advance(0.02F).runs);

    // Two presses before one advance still run one frame.
    session.apply(PlayControl::Step);
    session.apply(PlayControl::Step);
    CHECK(session.advance(0.02F).runs);
    CHECK_FALSE(session.advance(0.02F).runs);
    CHECK(session.frame() == 5);

    CHECK((session.apply(PlayControl::Stop) == PlayTransition::Stopped));
    CHECK((session.mode() == PlayMode::Edit));
    CHECK_FALSE(session.advance(0.02F).runs);
    CHECK(session.frame() == 5);  // Stop keeps the last count
    CHECK((session.apply(PlayControl::Play) == PlayTransition::Started));
    CHECK(session.frame() == 0);
}

TEST_CASE("play session: the status segment reads empty in Edit and names the frame otherwise (task 4.7.1, PY8)") {
    CHECK(playStatusText(PlayMode::Edit, 7).empty());
    CHECK(playStatusText(PlayMode::Playing, 12) == "Playing \xC2\xB7 frame 12");
    CHECK(playStatusText(PlayMode::Paused, 0) == "Paused \xC2\xB7 frame 0");
}

TEST_CASE("play session: the game has focus only in play, with the Viewport holding the keyboard (task 4.7.1, PY9)") {
    for (const PlayMode mode : MODES) {
        for (const bool viewport : {false, true}) {
            for (const bool window : {false, true}) {
                CAPTURE(static_cast<int>(mode));
                CAPTURE(viewport);
                CAPTURE(window);
                CHECK((gameHasFocus(mode, viewport, window) == (mode != PlayMode::Edit && viewport && window)));
            }
        }
    }
}

TEST_CASE("play session: the scene is locked in play, and a fresh session starts in Edit (task 4.7.1, PY10)") {
    CHECK_FALSE(sceneLocked(PlayMode::Edit));
    CHECK(sceneLocked(PlayMode::Playing));
    CHECK(sceneLocked(PlayMode::Paused));

    const PlaySession session;
    CHECK((session.mode() == PlayMode::Edit));
    CHECK_FALSE(session.active());
    CHECK(session.frame() == 0);
    CHECK_FALSE(session.gameFocused());
    CHECK_FALSE(session.stepQueued());
    for (std::size_t k = 1; k < static_cast<std::size_t>(engine::platform::Key::Count); ++k) {
        CHECK_FALSE(session.input().keyDown(static_cast<engine::platform::Key>(k)));
    }
    CHECK(sessionIn(PlayMode::Playing).active());
    CHECK(sessionIn(PlayMode::Paused).active());
}

TEST_CASE("play feed: Edit feeds nothing, pads reach the game unfocused, keys only focused (task 4.7.1, GI1)") {
    const GameImageRect image = imageRect();
    PlaySession edit;
    edit.setGameFocus(true);
    CHECK_FALSE(edit.gameFocused());  // Edit never takes focus
    edit.feed(keyEvent(EventType::KeyDown, Key::Space), image, HostOs::Windows);
    edit.feed(padEvent(EventType::GamepadConnected, 0), image, HostOs::Windows);
    CHECK_FALSE(edit.input().keyDown(Key::Space));
    CHECK_FALSE(edit.input().gamepadConnected(0));

    PlaySession playing = sessionIn(PlayMode::Playing);
    playing.feed(padEvent(EventType::GamepadConnected, 0), image, HostOs::Windows);
    playing.feed(keyEvent(EventType::KeyDown, Key::Space), image, HostOs::Windows);
    CHECK(playing.input().gamepadConnected(0));
    CHECK_FALSE(playing.input().keyDown(Key::Space));

    playing.setGameFocus(true);
    playing.feed(keyEvent(EventType::KeyDown, Key::Space), image, HostOs::Windows);
    CHECK(playing.input().keyDown(Key::Space));
    CHECK(playing.input().keyPressed(Key::Space));
    playing.feed(keyEvent(EventType::KeyUp, Key::Space), image, HostOs::Windows);
    CHECK_FALSE(playing.input().keyDown(Key::Space));
    CHECK(playing.input().keyReleased(Key::Space));
}

TEST_CASE("play feed: the host's command-modifier P is a play chord and never the game's (task 4.7.1, GI2)") {
    const GameImageRect image = imageRect();
    struct Host {
        HostOs os;
        KeyMod chordMod;
        KeyMod otherMod;
    };
    const std::array<Host, 3> hosts{Host{HostOs::MacOs, KeyMod::LeftGui, KeyMod::LeftCtrl},
                                    Host{HostOs::Windows, KeyMod::LeftCtrl, KeyMod::LeftGui},
                                    Host{HostOs::Linux, KeyMod::RightCtrl, KeyMod::LeftGui}};
    for (const Host& host : hosts) {
        CAPTURE(static_cast<int>(host.os));
        {
            PlaySession session = focusedSession();
            session.feed(keyEvent(EventType::KeyDown, Key::P, host.chordMod), image, host.os);
            CHECK_FALSE(session.input().keyDown(Key::P));  // the chord's own key-down is dropped
            session.feed(keyEvent(EventType::KeyUp, Key::P, host.chordMod), image, host.os);
            CHECK_FALSE(session.input().keyReleased(Key::P));  // and its release is no phantom edge
        }
        {
            PlaySession session = focusedSession();
            session.feed(keyEvent(EventType::KeyDown, Key::P, host.otherMod), image, host.os);
            CHECK(session.input().keyDown(Key::P));  // the other host's modifier is an ordinary P
            // An auto-repeat of the chord while P is held is dropped and does not unhold it.
            session.feed(keyEvent(EventType::KeyDown, Key::P, host.chordMod, true), image, host.os);
            CHECK(session.input().keyDown(Key::P));
            session.feed(keyEvent(EventType::KeyUp, Key::P, host.otherMod), image, host.os);
            CHECK_FALSE(session.input().keyDown(Key::P));
            CHECK(session.input().keyReleased(Key::P));
        }
        {
            PlaySession session = focusedSession();
            session.feed(keyEvent(EventType::KeyDown, Key::LeftGui, host.chordMod), image, host.os);
            CHECK(session.input().keyDown(Key::LeftGui));  // a modifier alone is not the chord
            session.feed(keyEvent(EventType::KeyDown, Key::Space, host.chordMod), image, host.os);
            CHECK(session.input().keyDown(Key::Space));  // nor is another key with it
        }
    }
    PlaySession plain = focusedSession();
    plain.feed(keyEvent(EventType::KeyDown, Key::P), image, HostOs::MacOs);
    CHECK(plain.input().keyDown(Key::P));  // P with no modifier is the game's
}

TEST_CASE("play feed: the pointer is translated into the image and a press outside never starts (task 4.7.1, GI3)") {
    const GameImageRect image = imageRect();
    constexpr HostOs OS = HostOs::Windows;
    PlaySession session = focusedSession();

    session.feed(buttonEvent(EventType::MouseButtonDown, MouseButton::Left, 50.0F, 20.0F), image, OS);
    CHECK_FALSE(session.input().mouseButtonDown(MouseButton::Left));  // outside the image
    session.feed(buttonEvent(EventType::MouseButtonDown, MouseButton::Left, 150.0F, 60.0F), image, OS);
    CHECK(session.input().mouseButtonDown(MouseButton::Left));
    CHECK(session.input().mousePosition().x == doctest::Approx(50.0F));
    CHECK(session.input().mousePosition().y == doctest::Approx(20.0F));
    // The release of a held button reaches even outside the image.
    session.feed(buttonEvent(EventType::MouseButtonUp, MouseButton::Left, 600.0F, 500.0F), image, OS);
    CHECK_FALSE(session.input().mouseButtonDown(MouseButton::Left));
    CHECK(session.input().mouseButtonReleased(MouseButton::Left));
    CHECK(session.input().mousePosition().x == doctest::Approx(500.0F));
    CHECK(session.input().mousePosition().y == doctest::Approx(460.0F));
    // A release of a button the game never saw pressed is dropped.
    session.endGameFrame();
    session.feed(buttonEvent(EventType::MouseButtonUp, MouseButton::Right, 150.0F, 60.0F), image, OS);
    CHECK_FALSE(session.input().mouseButtonReleased(MouseButton::Right));

    session.feed(moveEvent(110.0F, 50.0F, 3.0F, -2.0F), image, OS);
    CHECK(session.input().mousePosition().x == doctest::Approx(10.0F));
    CHECK(session.input().mousePosition().y == doctest::Approx(10.0F));
    CHECK(session.input().mouseDelta().x == doctest::Approx(3.0F));
    CHECK(session.input().mouseDelta().y == doctest::Approx(-2.0F));
    session.feed(wheelEvent(1.0F), image, OS);  // the pointer is over the image
    CHECK(session.input().mouseWheel().y == doctest::Approx(1.0F));

    session.endGameFrame();
    session.feed(moveEvent(5.0F, 5.0F, 0.0F, 0.0F), image, OS);  // the game's pointer follows, the wheel does not
    session.feed(wheelEvent(1.0F), image, OS);
    CHECK(session.input().mouseWheel().y == doctest::Approx(0.0F));

    GameImageRect hidden = image;
    hidden.valid = false;
    PlaySession blind = focusedSession();
    blind.feed(buttonEvent(EventType::MouseButtonDown, MouseButton::Left, 150.0F, 60.0F), hidden, OS);
    blind.feed(moveEvent(150.0F, 60.0F, 1.0F, 1.0F), hidden, OS);
    blind.feed(wheelEvent(1.0F), hidden, OS);
    CHECK_FALSE(blind.input().mouseButtonDown(MouseButton::Left));
    CHECK(blind.input().mouseDelta().x == doctest::Approx(0.0F));
    CHECK(blind.input().mouseWheel().y == doctest::Approx(0.0F));
}

TEST_CASE("play feed: losing focus releases everything the game holds, with edges (task 4.7.1, GI4)") {
    const GameImageRect image = imageRect();
    constexpr HostOs OS = HostOs::Linux;
    PlaySession session = focusedSession();
    session.feed(padEvent(EventType::GamepadConnected, 0), image, OS);
    session.feed(keyEvent(EventType::KeyDown, Key::W), image, OS);
    session.feed(buttonEvent(EventType::MouseButtonDown, MouseButton::Left, 150.0F, 60.0F), image, OS);
    session.feed(padButtonEvent(EventType::GamepadButtonDown, GamepadButton::South, 0), image, OS);
    REQUIRE(session.input().keyDown(Key::W));
    REQUIRE(session.input().mouseButtonDown(MouseButton::Left));
    REQUIRE(session.input().gamepadButtonDown(GamepadButton::South, 0));
    session.endGameFrame();

    session.setGameFocus(false);
    CHECK_FALSE(session.gameFocused());
    CHECK(session.input().keyReleased(Key::W));
    CHECK(session.input().mouseButtonReleased(MouseButton::Left));
    CHECK(session.input().gamepadButtonReleased(GamepadButton::South, 0));
    CHECK_FALSE(session.input().keyDown(Key::W));
    CHECK_FALSE(session.input().mouseButtonDown(MouseButton::Left));
    CHECK_FALSE(session.input().gamepadButtonDown(GamepadButton::South, 0));
    CHECK(session.input().gamepadConnected(0));  // the pad stays connected

    session.feed(keyEvent(EventType::KeyDown, Key::W), image, OS);
    CHECK_FALSE(session.input().keyDown(Key::W));  // dropped until focus returns
    session.setGameFocus(true);
    CHECK(session.gameFocused());
    session.feed(keyEvent(EventType::KeyDown, Key::W), image, OS);
    CHECK(session.input().keyDown(Key::W));

    // A window-level focus loss is never forwarded: it would clear held state with no edge.
    Event lost;
    lost.type = EventType::WindowFocusLost;
    session.feed(lost, image, OS);
    CHECK(session.input().keyDown(Key::W));
}

TEST_CASE("play feed: an edge made while paused survives until a game frame ends (task 4.7.1, GI5)") {
    const GameImageRect image = imageRect();
    constexpr HostOs OS = HostOs::Windows;
    PlaySession session = focusedSession();
    session.apply(PlayControl::Pause);
    session.feed(keyEvent(EventType::KeyDown, Key::Space), image, OS);
    CHECK(session.input().keyPressed(Key::Space));
    for (int tick = 0; tick < 2; ++tick) {
        CHECK_FALSE(session.advance(0.016F).runs);  // no frame ran, so the caller does not end one
        CHECK(session.input().keyPressed(Key::Space));
    }
    session.apply(PlayControl::Step);
    const engine::editor::GameFrame frame = session.advance(0.016F);
    REQUIRE(frame.runs);
    CHECK(session.input().keyPressed(Key::Space));  // the script's frame sees the edge
    session.endGameFrame();
    CHECK_FALSE(session.input().keyPressed(Key::Space));
    CHECK(session.input().keyDown(Key::Space));
}

TEST_CASE("play feed: Play adopts the editor's connected pads, Stop forgets them (task 4.7.1, GI6)") {
    InputState editor;
    editor.process(padEvent(EventType::GamepadConnected, 0));
    editor.process(padEvent(EventType::GamepadConnected, 2));
    editor.process(padButtonEvent(EventType::GamepadButtonDown, GamepadButton::South, 0));

    PlaySession session = sessionIn(PlayMode::Playing);
    CHECK_FALSE(session.input().gamepadConnected(0));  // a fresh game state knows no pad
    session.adoptConnectedPads(editor);
    CHECK(session.input().gamepadConnected(0));
    CHECK_FALSE(session.input().gamepadConnected(1));
    CHECK(session.input().gamepadConnected(2));
    CHECK_FALSE(session.input().gamepadConnected(3));
    CHECK_FALSE(session.input().gamepadButtonDown(GamepadButton::South, 0));  // a held press is not the game's

    session.apply(PlayControl::Stop);
    session.apply(PlayControl::Play);
    for (std::size_t slot = 0; slot < engine::platform::MAX_GAMEPADS; ++slot) {
        CHECK_FALSE(session.input().gamepadConnected(slot));
    }
    session.adoptConnectedPads(editor);
    CHECK(session.input().gamepadConnected(0));
}

TEST_CASE("play feed: a pad release the game never saw pressed makes no edge (task 4.7.1, GI7)") {
    const GameImageRect image = imageRect();
    constexpr HostOs OS = HostOs::Windows;
    PlaySession session = sessionIn(PlayMode::Playing);
    session.feed(padEvent(EventType::GamepadConnected, 1), image, OS);

    session.feed(padButtonEvent(EventType::GamepadButtonDown, GamepadButton::South, 1), image, OS);
    CHECK_FALSE(session.input().gamepadButtonDown(GamepadButton::South, 1));  // unfocused: dropped
    session.setGameFocus(true);
    session.feed(padButtonEvent(EventType::GamepadButtonUp, GamepadButton::South, 1), image, OS);
    CHECK_FALSE(session.input().gamepadButtonReleased(GamepadButton::South, 1));  // its Up is dropped too

    session.feed(padButtonEvent(EventType::GamepadButtonDown, GamepadButton::East, 1), image, OS);
    CHECK(session.input().gamepadButtonPressed(GamepadButton::East, 1));
    session.feed(padButtonEvent(EventType::GamepadButtonUp, GamepadButton::East, 1), image, OS);
    CHECK(session.input().gamepadButtonReleased(GamepadButton::East, 1));
}

TEST_CASE("play feed: losing focus centres every stick and trigger the game holds (task 4.7.1, GI8)") {
    const GameImageRect image = imageRect();
    constexpr HostOs OS = HostOs::Windows;
    PlaySession session = sessionIn(PlayMode::Playing);
    session.feed(padEvent(EventType::GamepadConnected, 1), image, OS);
    session.setGameFocus(true);

    session.feed(padAxisEvent(GamepadAxis::LeftX, 32767, 1), image, OS);
    session.feed(padAxisEvent(GamepadAxis::RightTrigger, 32767, 1), image, OS);
    CHECK(session.input().gamepadAxis(GamepadAxis::LeftX, 1) == doctest::Approx(1.0F));
    CHECK(session.input().gamepadAxis(GamepadAxis::RightTrigger, 1) == doctest::Approx(1.0F));

    session.setGameFocus(false);
    CHECK(session.input().gamepadAxis(GamepadAxis::LeftX, 1) == 0.0F);
    CHECK(session.input().gamepadAxis(GamepadAxis::RightTrigger, 1) == 0.0F);
    CHECK(session.input().gamepadConnected(1));

    session.feed(padAxisEvent(GamepadAxis::LeftX, -32768, 1), image, OS);
    CHECK(session.input().gamepadAxis(GamepadAxis::LeftX, 1) == 0.0F);  // unfocused: dropped
    session.setGameFocus(true);
    session.feed(padAxisEvent(GamepadAxis::LeftX, -32768, 1), image, OS);
    CHECK(session.input().gamepadAxis(GamepadAxis::LeftX, 1) == doctest::Approx(-1.0F));
}

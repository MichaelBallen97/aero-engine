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

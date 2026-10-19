// Aero Engine -- the Welcome window's and the New Project dialog's pure model (task E.6.4): WL1-WL6, NP1-NP13. Tier 0:
// no window, no GPU, no ImGui -- every state, string, role, fit and layout the two windows draw.
#include <aero/core/math.hpp>
#include <aero/editor/editor_glyphs.hpp>
#include <aero/editor/editor_theme.hpp>
#include <aero/editor/project.hpp>
#include <aero/editor/project_ui_view.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <ostream>  // CHECKs a std::string_view (the 0.4.1 MSVC trap)
#include <string>
#include <string_view>
#include <vector>

namespace ed = engine::editor;

namespace {

using State = ed::NewProjectTargetState;

// One unit per BYTE: an ellipsis (3 bytes of UTF-8) is 3 wide -- arithmetic, never a face.
const ed::TextWidth byteWidth = [](std::string_view text) { return static_cast<float>(text.size()); };
// The blurb on one Body line at scale 1 (16 dp), whatever the width -- WL2, WL3 and WL6's control.
const ed::WrappedTextHeight oneLineBlurb = [](float /*wrapWidth*/) { return 16.0F; };

[[nodiscard]] ed::NewProjectTargetFacts factsFor(std::string_view location, std::string_view name,
                                                 ed::NewProjectTargetKind target, bool locationIsDirectory) {
    ed::NewProjectTargetFacts facts;
    facts.location = std::string(location);
    facts.name = std::string(name);
    facts.touched = true;
    facts.locationIsDirectory = locationIsDirectory;
    facts.target = target;
    facts.targetPath = std::string(location) + "/" + std::string(name);
    return facts;
}

// True iff `text` is whole UTF-8: no sequence cut at either end or in the middle.
[[nodiscard]] bool wholeUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const std::size_t length = lead < 0x80U            ? 1U
                                   : (lead >> 5U) == 0x6U  ? 2U
                                   : (lead >> 4U) == 0xEU  ? 3U
                                   : (lead >> 3U) == 0x1EU ? 4U
                                                           : 0U;
        if (length == 0U || i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) >> 6U) != 0x2U) {
                return false;
            }
        }
        i += length;
    }
    return true;
}

constexpr std::array<State, ed::NEW_PROJECT_TARGET_STATE_COUNT> ALL_STATES{
    State::NoName,       State::BadName,        State::NoLocation,    State::Checking,          State::LocationMissing,
    State::TargetIsFile, State::TargetNotEmpty, State::CreatesFolder, State::AdoptsEmptyFolder,
};

}  // namespace

TEST_CASE("project ui: the lexical states never read the facts (task E.6.4, NP1)") {
    const ed::NewProjectTargetFacts creatable = factsFor("/work", "Kart", ed::NewProjectTargetKind::Absent, true);
    CHECK((ed::classifyNewProjectTarget("", "/work", creatable) == State::NoName));
    CHECK((ed::classifyNewProjectTarget("   ", "/work", creatable) == State::NoName));
    const std::array<std::string, 6> badNames{std::string(65, 'x'), "a/b", "..", "a<b", "Foo.", "CON"};
    for (const std::string& name : badNames) {
        CAPTURE(name);
        REQUIRE(ed::validateProjectName(name) != ed::NameProblem::Ok);  // ANTI-VACUITY: each really is refused
        REQUIRE(ed::validateProjectName(name) != ed::NameProblem::Empty);
        CHECK((ed::classifyNewProjectTarget(name, "/work", creatable) == State::BadName));
    }
    CHECK((ed::classifyNewProjectTarget("Kart", "", creatable) == State::NoLocation));
    // The NAME first: createProject's order.
    CHECK((ed::classifyNewProjectTarget("", "", creatable) == State::NoName));
    CHECK((ed::classifyNewProjectTarget("Kart", "/work", creatable) == State::CreatesFolder));  // the control
}

TEST_CASE("project ui: Checking waits for a probe of THIS location, never of this name (task E.6.4, NP2)") {
    ed::NewProjectTargetFacts untouched;
    untouched.location = "/work";
    untouched.name = "Kart";
    CHECK((ed::classifyNewProjectTarget("Kart", "/work", untouched) == State::Checking));
    const ed::NewProjectTargetFacts elsewhere = factsFor("/other", "Kart", ed::NewProjectTargetKind::Absent, true);
    CHECK((ed::classifyNewProjectTarget("Kart", "/work", elsewhere) == State::Checking));
    // The one-frame lag: facts for the previous name stand, so Create does not grey on every keystroke (D12).
    const ed::NewProjectTargetFacts lagging = factsFor("/work", "Kar", ed::NewProjectTargetKind::Absent, true);
    CHECK((ed::classifyNewProjectTarget("Kart", "/work", lagging) == State::CreatesFolder));
}

TEST_CASE("project ui: the filesystem states follow the probe's facts (task E.6.4, NP3)") {
    using Kind = ed::NewProjectTargetKind;
    const ed::NewProjectTargetFacts fileFacts = factsFor("/w", "Kart", Kind::File, true);
    CHECK((ed::classifyNewProjectTarget("Kart", "/w", fileFacts) == State::TargetIsFile));
    CHECK((ed::classifyNewProjectTarget("Kart", "/w", factsFor("/w", "Kart", Kind::NonEmptyDirectory, true)) ==
           State::TargetNotEmpty));
    CHECK((ed::classifyNewProjectTarget("Kart", "/w", factsFor("/w", "Kart", Kind::Absent, true)) ==
           State::CreatesFolder));
    CHECK((ed::classifyNewProjectTarget("Kart", "/w", factsFor("/w", "Kart", Kind::EmptyDirectory, true)) ==
           State::AdoptsEmptyFolder));
    for (const Kind kind : {Kind::Absent, Kind::EmptyDirectory, Kind::NonEmptyDirectory, Kind::File}) {
        CHECK((ed::classifyNewProjectTarget("Kart", "/w", factsFor("/w", "Kart", kind, false)) ==
               State::LocationMissing));  // a missing location wins over any target
    }
}

TEST_CASE("project ui: Create is enabled for exactly the two creatable states (task E.6.4, NP4)") {
    std::vector<State> enabled;
    for (const State state : ALL_STATES) {
        if (ed::newProjectCreateEnabled(state)) {
            enabled.push_back(state);
        }
    }
    REQUIRE(enabled.size() == 2U);
    CHECK((enabled[0] == State::CreatesFolder));
    CHECK((enabled[1] == State::AdoptsEmptyFolder));
}

TEST_CASE("project ui: every state's line, word for word and role for role (task E.6.4, NP5)") {
    using Kind = ed::NewProjectTargetKind;
    using Role = ed::ChromeRole;
    const std::string home = "/Users/me";
    const std::string loc = "/Users/me/dev/";  // a trailing separator: a naive re-join would read ".../dev//Kart"
    ed::NewProjectTargetFacts facts = factsFor(loc, "Kart", Kind::Absent, true);
    facts.targetPath = "/Users/me/dev/Kart";  // what newProjectTargetPath answers: lexically normal
    const auto line = [&](State state, std::string_view name, std::string_view error) {
        return ed::newProjectLine(state, name, loc, facts, error, home);
    };
    const auto checkTailOnly = [](const ed::NewProjectLine& l, std::string_view tail, Role role) {
        CHECK(l.lead.text.empty());
        CHECK(l.path.text.empty());
        CHECK(l.tail.text == tail);
        CHECK((l.tail.role == role));
    };
    checkTailOnly(line(State::NoName, "", ""), ed::NEW_PROJECT_HINT_NO_NAME, Role::TextMuted);
    checkTailOnly(line(State::BadName, "a/b", ""), ed::nameProblemMessage(ed::NameProblem::Separator), Role::Error);
    checkTailOnly(line(State::NoLocation, "Kart", ""), ed::NEW_PROJECT_HINT_NO_LOCATION, Role::TextMuted);
    checkTailOnly(line(State::Checking, "Kart", ""), ed::NEW_PROJECT_HINT_CHECKING, Role::TextFaint);

    const ed::NewProjectLine missing =
        ed::newProjectLine(State::LocationMissing, "Kart", "/Users/me/gone", facts, "", home);
    CHECK(missing.path.text == "~/gone");  // the LOCATION, home-abbreviated
    CHECK((missing.path.role == Role::TextSecondary));
    CHECK(missing.tail.text == ed::NEW_PROJECT_TAIL_LOCATION_MISSING);
    CHECK((missing.tail.role == Role::Error));
    const ed::NewProjectLine isFile = line(State::TargetIsFile, "Kart", "");
    CHECK(isFile.path.text == "~/dev/Kart");  // facts.targetPath, never re-joined from the inputs
    CHECK(isFile.tail.text == ed::NEW_PROJECT_TAIL_TARGET_IS_FILE);
    CHECK((isFile.tail.role == Role::Error));
    CHECK(line(State::TargetNotEmpty, "Kart", "").tail.text == ed::NEW_PROJECT_TAIL_TARGET_NOT_EMPTY);

    const ed::NewProjectLine creates = line(State::CreatesFolder, "Kart", "");
    CHECK(creates.lead.text == "creates ");
    CHECK((creates.lead.role == Role::TextMuted));
    CHECK(creates.path.text == "~/dev/Kart");
    CHECK((creates.path.role == Role::Success));
    CHECK(creates.tail.text == " " AERO_GLYPH_MIDDLE_DOT " new folder");
    CHECK((creates.tail.role == Role::TextFaint));
    CHECK(line(State::AdoptsEmptyFolder, "Kart", "").tail.text == " " AERO_GLYPH_MIDDLE_DOT " folder is empty");

    // D11: a failed Create's message replaces a CREATABLE line only.
    checkTailOnly(line(State::CreatesFolder, "Kart", "could not create"), "could not create", Role::Error);
    checkTailOnly(line(State::AdoptsEmptyFolder, "Kart", "could not create"), "could not create", Role::Error);
    const ed::NewProjectLine refused = line(State::TargetNotEmpty, "Kart", "could not create");
    CHECK(refused.tail.text == ed::NEW_PROJECT_TAIL_TARGET_NOT_EMPTY);
}

TEST_CASE("project ui: the line fits path first, then the tail, then the lead (task E.6.4, NP6)") {
    using Role = ed::ChromeRole;
    ed::NewProjectLine line;
    line.lead = {"creates ", Role::TextMuted};      // 8
    line.path = {"/a/bb/ccc/Kart", Role::Success};  // 14
    line.tail = {" x", Role::TextFaint};            // 2
    const ed::NewProjectLine whole = ed::fitNewProjectLine(line, 100.0F, byteWidth);
    CHECK(whole.path.text == line.path.text);

    const ed::NewProjectLine pathCut = ed::fitNewProjectLine(line, 20.0F, byteWidth);  // the path's budget is 10
    CHECK(pathCut.lead.text == "creates ");
    CHECK(pathCut.tail.text == " x");
    CHECK(pathCut.path.text.rfind(AERO_GLYPH_ELLIPSIS, 0) == 0U);  // it opens with the ellipsis...
    CHECK(pathCut.path.text.size() >= 4U);
    CHECK(pathCut.path.text.compare(pathCut.path.text.size() - 4U, 4U, "Kart") == 0);  // ...and keeps the folder
    CHECK(byteWidth(pathCut.path.text) <= 10.0F);

    const ed::NewProjectLine pathGone = ed::fitNewProjectLine(line, 12.0F, byteWidth);  // the path's budget is 2 < 3
    CHECK(pathGone.path.text.empty());
    CHECK(pathGone.tail.text == " x");
    CHECK(pathGone.lead.text == "creates ");

    const ed::NewProjectLine leadCut = ed::fitNewProjectLine(line, 5.0F, byteWidth);
    CHECK(leadCut.path.text.empty());
    CHECK(leadCut.tail.text.empty());
    CHECK_FALSE(leadCut.lead.text.empty());
    CHECK(byteWidth(leadCut.lead.text) <= 5.0F);

    const ed::NewProjectLine nothing = ed::fitNewProjectLine(line, 2.0F, byteWidth);
    CHECK(nothing.lead.text.empty());
    CHECK(nothing.path.text.empty());
    CHECK(nothing.tail.text.empty());

    // A cut never lands inside a UTF-8 sequence -- at any budget.
    ed::NewProjectLine accented = line;
    accented.path.text = "/a/Caf\xC3\xA9 \xC3\xA9t\xC3\xA9";  // "/a/Café été": the leaf itself must be cut
    for (int units = 0; units < 40; ++units) {                // an INTEGER counter (E.1.2's float-loop lesson)
        const auto budget = static_cast<float>(units);        // auto: a bare cast (modernize-use-auto)
        CAPTURE(budget);
        const ed::NewProjectLine fitted = ed::fitNewProjectLine(accented, budget, byteWidth);
        CHECK(wholeUtf8(fitted.lead.text));
        CHECK(wholeUtf8(fitted.path.text));
        CHECK(wholeUtf8(fitted.tail.text));
        CHECK(byteWidth(fitted.lead.text) + byteWidth(fitted.path.text) + byteWidth(fitted.tail.text) <= budget);
    }
    CHECK(ed::fitPathLeft("/a/b", 2.0F, byteWidth).empty());  // below the ellipsis
    CHECK(ed::fitTextRight("abcdef", 2.0F, byteWidth).empty());
    CHECK(ed::fitTextRight("abc", 3.0F, byteWidth) == "abc");  // a text that fits is returned whole
}

TEST_CASE("project ui: the footer summarises today's fields (task E.6.4, NP7)") {
    CHECK(ed::newProjectFooterSummary("") == "TypeScript");
    CHECK(ed::newProjectFooterSummary("Kart") == "Kart " AERO_GLYPH_MIDDLE_DOT " TypeScript");
    CHECK(ed::newProjectFooterSummary("50%d ##x") == "50%d ##x " AERO_GLYPH_MIDDLE_DOT " TypeScript");  // verbatim
    CHECK(ed::languageCardTitle(ed::NEW_PROJECT_LANGUAGE) == "TypeScript");
}

TEST_CASE("project ui: the language control shows what Create writes, and names 4.6.1 (task E.6.4, NP8)") {
    REQUIRE(ed::NEW_PROJECT_LANGUAGE_CARDS.size() == 2U);
    CHECK((ed::NEW_PROJECT_LANGUAGE_CARDS[0].language == ed::ProjectLanguage::Cpp));
    CHECK(ed::NEW_PROJECT_LANGUAGE_CARDS[0].title == "C++");
    CHECK(ed::NEW_PROJECT_LANGUAGE_CARDS[0].subtitle == "Native code, built for each platform.");
    CHECK((ed::NEW_PROJECT_LANGUAGE_CARDS[1].language == ed::ProjectLanguage::Ts));
    CHECK(ed::NEW_PROJECT_LANGUAGE_CARDS[1].title == "TypeScript");
    CHECK(ed::NEW_PROJECT_LANGUAGE_CARDS[1].subtitle == "Embedded scripts, no toolchain to export.");
    CHECK_FALSE(ed::languageCardSelected(ed::ProjectLanguage::Cpp));
    CHECK(ed::languageCardSelected(ed::ProjectLanguage::Ts));
    CHECK(ed::LANGUAGE_CONTROL_TOOLTIP == "Not implemented yet -- task 4.6.1");
}

TEST_CASE("project ui: the dialog's copy is true for this build (task E.6.4, NP9)") {
    std::string all;
    std::vector<std::string> code;
    for (const ed::InfoSpan& span : ed::NEW_PROJECT_INFO_SPANS) {
        all += span.text;
        if (span.code) {
            code.emplace_back(span.text);
        }
    }
    CHECK(all == "Aero writes project.json and creates the assets/ and scenes/ folders.");
    // The COPY: its code spans name the manifest file and the two folders by the format's own constants. Whether the
    // writer creates exactly those is P95's (createProject spells its folder names as literals, not these constants).
    REQUIRE(code.size() == 3U);
    CHECK(code[0] == std::string(ed::PROJECT_FILE_NAME));
    CHECK(code[1] == ed::ProjectManifest{}.assetsPath + "/");
    CHECK(code[2] == ed::ProjectManifest{}.scenesPath + "/");
    CHECK(all.find("scripts") == std::string::npos);  // mock 1e's copy: false for this build (G1)
    CHECK(all.find("scene ") == std::string::npos);
    const std::string hint(ed::NEW_PROJECT_LANGUAGE_HINT);
    CHECK(hint.find("fixed at creation") != std::string::npos);  // ADR-008
    CHECK(hint.find("coexist") == std::string::npos);            // mock 1e's hint contradicts ADR-008 (G2)
    CHECK(hint.find("later") == std::string::npos);
}

TEST_CASE("project ui: the dialog's layout is the theme's, scaled once and clamped (task E.6.4, NP10)") {
    const ed::NewProjectDialogLayout one = ed::newProjectDialogLayout(2000.0F, 1.0F);
    CHECK(one.width == 667.0F);
    CHECK(one.paddingX == 26.0F);
    CHECK(one.contentWidth == 615.0F);
    CHECK(one.headerHeight == 54.0F);  // 1 + 52 + 1: the border, mock 1e's strip, its rule
    CHECK(one.footerHeight == 66.0F);  // 1 + 64 + 1: the rule, mock 1e's strip, the border
    CHECK(one.windowRounding == 9.0F);
    CHECK(one.controlRounding == 7.0F);
    CHECK(one.fieldPadding.x == 12.0F);
    CHECK(one.fieldPadding.y == 11.0F);  // 16 + 2 x 11 = the 38-dp field
    CHECK(one.buttonPadding.x == 18.0F);
    CHECK(one.buttonPadding.y == 10.0F);  // 16 + 2 x 10 = the 36-dp button
    CHECK(one.fieldHeight == 38.0F);
    CHECK(one.buttonHeight == 36.0F);
    CHECK(one.cardWidth == 303.0F);  // (615 - 9) / 2: mock 1e's measured card
    CHECK(one.cardHeight == 54.0F);
    CHECK(one.bodyTop == 18.0F);
    CHECK(one.infoHeight == 40.0F);
    CHECK(one.footerButtonGap == 12.0F);
    const ed::NewProjectDialogLayout two = ed::newProjectDialogLayout(4000.0F, 2.0F);
    CHECK(two.width == 1334.0F);
    CHECK(two.contentWidth == 1230.0F);
    CHECK(two.fieldPadding.y == 22.0F);  // body 32 points: 32 + 44 = 76 = 2 x 38
    CHECK(two.buttonPadding.y == 20.0F);
    CHECK(two.cardWidth == 606.0F);
    CHECK(two.radioDiameter == 30.0F);
    CHECK(ed::newProjectDialogLayout(640.0F, 1.0F).width == 592.0F);  // the work area less 24 a side
    CHECK(ed::newProjectDialogLayout(200.0F, 1.0F).width == 280.0F);  // the floor
    CHECK(ed::newProjectDialogLayout(std::numeric_limits<float>::quiet_NaN(), 1.0F).width == 667.0F);
    CHECK(ed::newProjectDialogLayout(std::numeric_limits<float>::infinity(), 1.0F).width == 667.0F);
}

TEST_CASE("project ui: the footer puts Create at the right edge and the summary before Cancel (task E.6.4, NP11)") {
    const ed::NewProjectDialogLayout layout = ed::newProjectDialogLayout(2000.0F, 1.0F);
    const ed::NewProjectFooterLayout footer = ed::newProjectFooterLayout(layout, 80.0F, 129.0F);
    CHECK(footer.createX == 512.0F);  // 667 - 26 - 129
    CHECK(footer.cancelX == 420.0F);  // 512 - 12 - 80
    CHECK(footer.buttonY == 15.0F);   // (66 - 36) / 2: mock 1e's 14 under its 1-dp rule
    CHECK(footer.summaryX == 26.0F);
    CHECK(footer.summaryMaxWidth == 382.0F);                                            // 420 - 12 - 26
    CHECK(ed::newProjectFooterLayout(layout, 400.0F, 300.0F).summaryMaxWidth == 0.0F);  // floored, never negative
}

TEST_CASE("project ui: the header strip places the mark, the title, the version and the close box (task E.6.4, NP12)") {
    const ed::DialogHeaderLayout modal = ed::dialogHeaderLayout(667.0F, true, 1.0F);
    CHECK(modal.height == 54.0F);
    CHECK(modal.centerY == 27.0F);  // 1 + 52 / 2: the mock's mid-line, 27 dp from the outer top
    CHECK(modal.markCenter.x == 34.0F);
    CHECK(modal.markCenter.y == 27.0F);
    CHECK(modal.titleX == 55.0F);  // 34 + 24 / 2 + 9; the mock's pen is 54.85
    CHECK(modal.closeDrawn);
    CHECK(modal.closeMin.x == 626.0F);  // centre 667 - 29 = 638, half 12
    CHECK(modal.closeMin.y == 15.0F);
    CHECK(modal.closeMax.x == 650.0F);
    CHECK(modal.closeMax.y == 39.0F);
    CHECK(modal.versionRight == 615.0F);  // 626 - 11
    const ed::DialogHeaderLayout welcome = ed::dialogHeaderLayout(560.0F, false, 1.0F);
    CHECK_FALSE(welcome.closeDrawn);
    CHECK(welcome.versionRight == 534.0F);  // 560 - 26
    const ed::DialogHeaderLayout twice = ed::dialogHeaderLayout(1334.0F, true, 2.0F);
    CHECK(twice.height == 108.0F);
    CHECK(twice.titleX == 110.0F);
    CHECK(twice.closeMin.x == 1252.0F);  // 1334 - 58 - 24
}

TEST_CASE("project ui: the mark is a square turned 45 degrees (task E.6.4, NP13)") {
    for (const float scale : {1.0F, 2.0F}) {
        CAPTURE(scale);
        const engine::Vec2 c{100.0F, 50.0F};
        const std::array<engine::Vec2, 4> p = ed::aeroMarkPoints(c, scale);
        const float half = (16.0F * scale) / std::numbers::sqrt2_v<float>;
        CHECK(p[0].x == c.x);  // top and bottom on the centre's vertical, EXACTLY
        CHECK(p[2].x == c.x);
        CHECK(p[1].y == c.y);  // right and left on its horizontal, EXACTLY
        CHECK(p[3].y == c.y);
        CHECK(c.y - p[0].y == doctest::Approx(half).epsilon(1e-6));
        CHECK(p[1].x - c.x == doctest::Approx(half).epsilon(1e-6));
        CHECK(p[2].y - c.y == doctest::Approx(half).epsilon(1e-6));
        CHECK(c.x - p[3].x == doctest::Approx(half).epsilon(1e-6));
        const float side = std::hypot(p[1].x - p[0].x, p[1].y - p[0].y);
        CHECK(side == doctest::Approx(16.0F * scale).epsilon(1e-6));  // the markSide square
    }
}

TEST_CASE("project ui: a recent card shows the folder name and the home-abbreviated path (task E.6.4, WL1)") {
    const ed::RecentCardText plain = ed::recentCardText("/Users/me/dev/MyGame", "/Users/me");
    CHECK(plain.title == "MyGame");
    CHECK(plain.path == "~/dev/MyGame");
    const ed::RecentCardText trailing = ed::recentCardText("/Users/me/dev/MyGame/", "/Users/me");
    CHECK(trailing.title == "MyGame");
    // A raw literal outside a macro (modernize-raw-string-literal); the shorter home's raw form would be longer.
    const ed::RecentCardText windows = ed::recentCardText(R"(C:\Users\me\Game)", "C:\\Users\\me");
    CHECK(windows.title == "Game");
    CHECK(windows.path == "~\\Game");
    const ed::RecentCardText hashes = ed::recentCardText("/Users/me/a##b/Game##2", "/Users/me");  // G6
    CHECK(hashes.title == "Game##2");
    CHECK(hashes.path == "~/a##b/Game##2");
    const ed::RecentCardText rawPrefix = ed::recentCardText("/Users/michael/Game", "/Users/mi");
    CHECK(rawPrefix.path == "/Users/michael/Game");  // a raw prefix is not a segment: no "~chael"
}

TEST_CASE("project ui: the Welcome list folds before the window overflows (task E.6.4, WL2)") {
    const engine::Vec2 tall{2000.0F, 2000.0F};
    const ed::WelcomeLayout none = ed::welcomeLayout(0U, tall, oneLineBlurb, 1.0F);
    CHECK(none.visibleCards == 0U);
    CHECK(none.listHeight == 0.0F);
    CHECK(none.emptyStateHeight == 96.0F);
    const ed::WelcomeLayout three = ed::welcomeLayout(3U, tall, oneLineBlurb, 1.0F);
    CHECK(three.visibleCards == 3U);
    CHECK_FALSE(three.listScrolls);
    CHECK(three.listHeight == 178.0F);  // 3 x 54 + 2 x 8
    const ed::WelcomeLayout twelve = ed::welcomeLayout(12U, tall, oneLineBlurb, 1.0F);
    CHECK(twelve.visibleCards == 5U);
    CHECK(twelve.listScrolls);
    CHECK(twelve.listHeight == 302.0F);  // 5 x 54 + 4 x 8
    // A short work area: as many cards as fit under the fixed rows -- 222 dp at scale 1 (54 + 18 + 16 + 16 + 18 + 36
    // + 18 + 14 + 6 + 26), an INDEPENDENT sum -- and not one more.
    const engine::Vec2 shortArea{2000.0F, 400.0F};
    const ed::WelcomeLayout folded = ed::welcomeLayout(12U, shortArea, oneLineBlurb, 1.0F);
    const float room = shortArea.y - (2.0F * 24.0F);
    CHECK(folded.visibleCards < 5U);  // ANTI-VACUITY: this area really folds
    CHECK(folded.visibleCards >= 1U);
    CHECK(222.0F + folded.listHeight <= room);
    CHECK(222.0F + folded.listHeight + 54.0F + 8.0F > room);
    // The floor: never fewer than one card.
    CHECK(ed::welcomeLayout(12U, engine::Vec2{2000.0F, 100.0F}, oneLineBlurb, 1.0F).visibleCards == 1U);
}

TEST_CASE("project ui: the Welcome width is the theme's, clamped to the work area (task E.6.4, WL3)") {
    CHECK(ed::welcomeLayout(0U, engine::Vec2{2000.0F, 2000.0F}, oneLineBlurb, 1.0F).width == 560.0F);
    CHECK(ed::welcomeLayout(0U, engine::Vec2{4000.0F, 2000.0F}, oneLineBlurb, 2.0F).width == 1120.0F);
    CHECK(ed::welcomeLayout(0U, engine::Vec2{500.0F, 2000.0F}, oneLineBlurb, 1.0F).width == 452.0F);
    CHECK(ed::welcomeLayout(0U, engine::Vec2{200.0F, 2000.0F}, oneLineBlurb, 1.0F).width == 280.0F);
    CHECK(ed::welcomeLayout(0U, engine::Vec2{2000.0F, 2000.0F}, oneLineBlurb, 1.0F).contentWidth == 508.0F);
}

TEST_CASE("project ui: the Welcome window's words (task E.6.4, WL4)") {
    CHECK(ed::WELCOME_TITLE == "Aero Editor");
    CHECK(ed::WELCOME_HEADLINE == "No project is open.");
    CHECK(ed::WELCOME_BLURB == "Create a new project or open an existing one to get started.");
    CHECK(ed::WELCOME_NEW_PROJECT_LABEL == "New Project...");    // the File menu's item, byte for byte
    CHECK(ed::WELCOME_OPEN_PROJECT_LABEL == "Open Project...");  // likewise
    CHECK(ed::WELCOME_RECENTS_HEADING == "RECENT PROJECTS");
    CHECK(ed::WELCOME_EMPTY_TITLE == "No recent projects");
    CHECK(ed::WELCOME_EMPTY_BODY == "Projects you create or open are listed here.");
    CHECK(ed::WELCOME_MAX_VISIBLE_CARDS == 5U);
    CHECK(ed::NEW_PROJECT_TITLE == "New project");
}

TEST_CASE("project ui: a record frame resets what a frame draws and keeps the counters (task E.6.4, WL5)") {
    ed::WelcomeRecord welcome;
    welcome.framesDrawn = 7U;
    welcome.drawnThisFrame = true;
    welcome.emptyStateDrawn = true;
    welcome.cards.push_back(ed::RecentCardRecord{"a", "b", engine::Vec2{1.0F, 1.0F}, engine::Vec2{2.0F, 2.0F}});
    welcome.newProject.enabled = true;
    welcome.header.title = "x";
    welcome.header.version = "y";
    ed::beginWelcomeRecordFrame(welcome);
    CHECK(welcome.framesDrawn == 7U);
    CHECK_FALSE(welcome.drawnThisFrame);
    CHECK_FALSE(welcome.emptyStateDrawn);
    CHECK(welcome.cards.empty());
    CHECK_FALSE(welcome.newProject.enabled);
    CHECK(welcome.header.title.empty());
    CHECK(welcome.header.version.empty());

    ed::NewProjectRecord modal;
    modal.framesDrawn = 3U;
    modal.popupOpenAtEntry = true;
    modal.state = State::CreatesFolder;
    modal.line.tail.text = "x";
    modal.create.enabled = true;
    modal.language[1].drawnActive = true;
    modal.header.close.enabled = true;
    ed::beginNewProjectRecordFrame(modal);
    CHECK(modal.framesDrawn == 3U);
    CHECK_FALSE(modal.popupOpenAtEntry);
    CHECK((modal.state == State::NoName));
    CHECK(modal.line.tail.text.empty());
    CHECK_FALSE(modal.create.enabled);
    CHECK_FALSE(modal.language[1].drawnActive);
    CHECK_FALSE(modal.header.close.enabled);  // a frame that draws no close box reads "not drawn"
}

TEST_CASE("project ui: the Welcome fold counts a wrapped blurb, and the empty well folds too (task E.6.4, WL6)") {
    // A stand-in for the face's measure: the blurb takes a second Body line below a 300-dp wrap width. It is asked at
    // the layout's OWN content width: a 400-wide work area gives width 352, content 300 (one line); 380 gives width
    // 332, content 280 (two lines).
    const ed::WrappedTextHeight wrapsBelow300 = [](float wrapWidth) { return wrapWidth < 300.0F ? 32.0F : 16.0F; };
    const ed::WelcomeLayout wide = ed::welcomeLayout(12U, engine::Vec2{400.0F, 390.0F}, wrapsBelow300, 1.0F);
    const ed::WelcomeLayout narrow = ed::welcomeLayout(12U, engine::Vec2{380.0F, 390.0F}, wrapsBelow300, 1.0F);
    CHECK(wide.contentWidth == 300.0F);
    CHECK(narrow.contentWidth == 280.0F);
    CHECK(wide.blurbHeight == 16.0F);
    CHECK(narrow.blurbHeight == 32.0F);  // ANTI-VACUITY: the narrow window really wraps
    // 390 - 48 = 342 of room: 222 + 116 <= 342 < 222 + 178 (two cards); 238 + 54 <= 342 < 238 + 116 (one card).
    CHECK(wide.visibleCards == 2U);
    CHECK(narrow.visibleCards == 1U);  // the second blurb line cost a card

    // No recents: the well folds to the room under 222 dp of fixed rows, between its 54-dp block (16 + 6 + 16 + 2 +
    // 14) and its full 96.
    const ed::WelcomeLayout roomy = ed::welcomeLayout(0U, engine::Vec2{2000.0F, 2000.0F}, oneLineBlurb, 1.0F);
    CHECK(roomy.emptyStateHeight == 96.0F);
    CHECK(roomy.emptyStateBlock == 54.0F);
    CHECK(ed::welcomeLayout(0U, engine::Vec2{2000.0F, 340.0F}, oneLineBlurb, 1.0F).emptyStateHeight == 70.0F);
    CHECK(ed::welcomeLayout(0U, engine::Vec2{2000.0F, 200.0F}, oneLineBlurb, 1.0F).emptyStateHeight == 54.0F);
    const float nanHeight = std::numeric_limits<float>::quiet_NaN();
    CHECK(ed::welcomeLayout(0U, engine::Vec2{2000.0F, nanHeight}, oneLineBlurb, 1.0F).emptyStateHeight == 96.0F);

    // A measure the fold cannot use counts one Body line, never less.
    const ed::WrappedTextHeight broken = [](float /*wrapWidth*/) { return std::numeric_limits<float>::quiet_NaN(); };
    const ed::WrappedTextHeight tooShort = [](float /*wrapWidth*/) { return 3.0F; };
    const engine::Vec2 tall{2000.0F, 2000.0F};
    CHECK(ed::welcomeLayout(3U, tall, broken, 1.0F).blurbHeight == 16.0F);
    CHECK(ed::welcomeLayout(3U, tall, tooShort, 1.0F).blurbHeight == 16.0F);
    CHECK(ed::welcomeLayout(3U, tall, ed::WrappedTextHeight{}, 1.0F).blurbHeight == 16.0F);  // an empty measurer
}

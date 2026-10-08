// tests/editor/breadcrumb_test.cpp -- task E.6.2: the breadcrumb's pure layout (BD1-BD7). A TU of
// aero_editor_shell_test. Tier 0, every configuration, no #if. The measurers are FIXED-ADVANCE over BYTES -- the
// AV63/AV64 shape, under which a byte search would prefer an invalid UTF-8 split -- so exact results are
// arithmetic, and the shell metrics (minGap 24, gap 8, dot 6) are restated literals.
#include <aero/editor/breadcrumb.hpp>
#include <aero/editor/editor_glyphs.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ostream>  // the 0.4.1 MSVC trap
#include <string>
#include <string_view>

namespace {

namespace ed = engine::editor;

[[nodiscard]] float body10(std::string_view t) { return 10.0F * static_cast<float>(t.size()); }
[[nodiscard]] float strong12(std::string_view t) { return 12.0F * static_cast<float>(t.size()); }
[[nodiscard]] float body20(std::string_view t) { return 20.0F * static_cast<float>(t.size()); }
[[nodiscard]] float strong24(std::string_view t) { return 24.0F * static_cast<float>(t.size()); }

// camelBack, not UPPER_CASE: a std::function cannot be constexpr, so readability-identifier-naming classes these
// as plain variables.
const ed::BreadcrumbMeasure atOne{.body = body10, .strong = strong12};
const ed::BreadcrumbMeasure atTwo{.body = body20, .strong = strong24};

// UTF-8 well-formedness, and every non-ASCII code point either the ellipsis or present in `source`.
[[nodiscard]] bool onlyWholeCodePointsOf(std::string_view drawn, std::string_view source) {
    std::size_t i = 0;
    while (i < drawn.size()) {
        const auto lead = static_cast<unsigned char>(drawn[i]);
        const std::size_t length =
            lead < 0x80U ? 1U : (lead >= 0xF0U ? 4U : (lead >= 0xE0U ? 3U : (lead >= 0xC0U ? 2U : 0U)));
        if (length == 0U || i + length > drawn.size()) {
            return false;  // a continuation byte where a lead belongs, or a truncated sequence
        }
        for (std::size_t k = 1; k < length; ++k) {
            if ((static_cast<unsigned char>(drawn[i + k]) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        const std::string_view codePoint = drawn.substr(i, length);
        if (length > 1U && codePoint != std::string_view(AERO_GLYPH_ELLIPSIS) &&
            source.find(codePoint) == std::string_view::npos) {
            return false;
        }
        i += length;
    }
    return true;
}

}  // namespace

TEST_CASE("breadcrumb: the segments (task E.6.2, BD1)") {
    const ed::BreadcrumbText withProject = ed::breadcrumbText("Game", "level1.scene.json", false);
    CHECK(withProject.project == "Game");
    CHECK(withProject.separator == "/");
    CHECK(withProject.scene == "level1.scene.json");  // the leaf verbatim -- no stem is invented
    CHECK_FALSE(withProject.dirty);
    CHECK(ed::breadcrumbText("Game", "level1.scene.json", true).dirty);
    const ed::BreadcrumbText none = ed::breadcrumbText("", "Untitled", true);
    CHECK(none.project.empty());
    CHECK(none.separator.empty());  // no project: the scene alone, no dangling "/"
    CHECK(none.scene == "Untitled");
}

TEST_CASE("breadcrumb: centred in the free span, in SCREEN x (task E.6.2, BD2)") {
    // A bar whose left edge is at 300: a layout that mixed in a local x would land 300 off (seed S19).
    const ed::BreadcrumbText text = ed::breadcrumbText("Game", "a.scene.json", true);
    const ed::BreadcrumbLayout l = ed::breadcrumbLayout(300.0F, 1300.0F, 500.0F, text, atOne, 1.0F);
    // span [524, 1300] = 776; run = 40 + 8 + 10 + 8 + 144 + 8 + 6 = 224; x0 = 524 + (776 - 224) / 2 = 800.
    CHECK(l.project == "Game");
    CHECK(l.projectX == 800.0F);
    CHECK(l.separatorDrawn);
    CHECK(l.separatorX == 848.0F);
    CHECK(l.scene == "a.scene.json");
    CHECK(l.sceneX == 866.0F);
    CHECK(l.dotDrawn);
    CHECK(l.dotDiameter == 6.0F);
    CHECK(l.dotCenterX == 1021.0F);
    CHECK(l.runStart == 800.0F);
    CHECK(l.runEnd == 1024.0F);
}

TEST_CASE("breadcrumb: the left fallback is the menus' end plus the minimum gap (task E.6.2, BD3)") {
    const ed::BreadcrumbText text = ed::breadcrumbText("Game", "a.scene.json", true);
    // A span exactly the run's width (224): the run starts at menusEnd + 24, never closer (seed S18).
    const ed::BreadcrumbLayout tight = ed::breadcrumbLayout(0.0F, 748.0F, 500.0F, text, atOne, 1.0F);
    CHECK(tight.runStart == 524.0F);
    CHECK(tight.project == "Game");  // nothing elided at the exact fit
    for (float menusEnd = 0.0F; menusEnd <= 1300.0F; menusEnd += 7.0F) {
        CAPTURE(menusEnd);
        const ed::BreadcrumbLayout l = ed::breadcrumbLayout(0.0F, 1300.0F, menusEnd, text, atOne, 1.0F);
        if (l.runEnd > l.runStart) {
            CHECK(l.runStart >= menusEnd + 24.0F);
            CHECK(l.runEnd <= 1300.0F);
        }
    }
}

TEST_CASE("breadcrumb: project first, then the scene, on code-point boundaries (task E.6.2, BD4)") {
    // project 8 bytes ("Caf" + U+00E9 + "XYZ"), scene 6 bytes, clean: rest = 8 + 10 + 8 + 72 = 98.
    const ed::BreadcrumbText text = ed::breadcrumbText("Caf\xC3\xA9XYZ", "s.json", false);
    // A 7-byte project budget (70): "Caf\xC3" + ellipsis would be 7 bytes but splits U+00E9 -> "Caf" + ellipsis.
    const ed::BreadcrumbLayout cut = ed::breadcrumbLayout(0.0F, 192.0F, 0.0F, text, atOne, 1.0F);  // span 168
    CHECK(cut.project == "Caf" AERO_GLYPH_ELLIPSIS);
    CHECK(cut.scene == "s.json");  // the scene is whole while the project can still yield
    // The project DROPPED (budget under the ellipsis's 30) with its separator; the scene still whole.
    const ed::BreadcrumbLayout dropped = ed::breadcrumbLayout(0.0F, 150.0F, 0.0F, text, atOne, 1.0F);  // span 126
    CHECK(dropped.project.empty());
    CHECK_FALSE(dropped.separatorDrawn);
    CHECK(dropped.scene == "s.json");
    // A Cyrillic scene leaf right-elided only after the project is gone.
    const ed::BreadcrumbText cyrillic =
        ed::breadcrumbText("Caf\xC3\xA9XYZ", "\xD0\x9C\xD0\xB8\xD1\x80.json", true);  // U+041C U+0438 U+0440
    bool sawSceneElided = false;
    for (int barRight = 1300; barRight >= 0; --barRight) {
        CAPTURE(barRight);
        const ed::BreadcrumbLayout l =
            ed::breadcrumbLayout(0.0F, static_cast<float>(barRight), 100.0F, cyrillic, atOne, 1.0F);
        if (l.scene != cyrillic.scene) {
            sawSceneElided = sawSceneElided || !l.scene.empty();
            CHECK(l.project.empty());  // the scene yields only after the project is gone
        }
        if (cyrillic.dirty && !l.dotDrawn && l.runEnd > l.runStart) {
            CHECK(l.scene.empty());  // the dot is the LAST thing to go while anything draws
        }
        CHECK(onlyWholeCodePointsOf(l.project, cyrillic.project));
        CHECK(onlyWholeCodePointsOf(l.scene, cyrillic.scene));
        if (l.runEnd > l.runStart) {
            CHECK(l.runStart >= 124.0F);  // never over the menus
            CHECK(l.runEnd <= static_cast<float>(barRight));
        }
    }
    CHECK(sawSceneElided);  // ANTI-VACUITY: the sweep reached the scene's elision
}

TEST_CASE("breadcrumb: the only non-ASCII bytes are the ellipsis and whole input code points (task E.6.2, BD5)") {
    const ed::BreadcrumbText text = ed::breadcrumbText("\xCE\x91\xCE\xB5\xCF\x81\xCF\x8C", "Caf\xC3\xA9.json", true);
    for (int barRight = 0; barRight <= 600; ++barRight) {
        const ed::BreadcrumbLayout l =
            ed::breadcrumbLayout(0.0F, static_cast<float>(barRight), 0.0F, text, atOne, 1.0F);
        CAPTURE(barRight);
        CHECK(onlyWholeCodePointsOf(l.project, text.project));
        CHECK(onlyWholeCodePointsOf(l.scene, text.scene));
    }
    CHECK_FALSE(onlyWholeCodePointsOf("Caf\xC3", "Caf\xC3\xA9"));  // ANTI-VACUITY: a split sequence is refused
}

TEST_CASE("breadcrumb: degenerate widths draw nothing and overlap nothing (task E.6.2, BD6)") {
    const ed::BreadcrumbText text = ed::breadcrumbText("Game", "a.scene.json", true);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const ed::BreadcrumbLayout& l : {ed::breadcrumbLayout(0.0F, nan, 100.0F, text, atOne, 1.0F),
                                          ed::breadcrumbLayout(0.0F, 400.0F, 500.0F, text, atOne, 1.0F),
                                          ed::breadcrumbLayout(0.0F, 524.0F, 500.0F, text, atOne, 1.0F),
                                          ed::breadcrumbLayout(nan, 1300.0F, 100.0F, text, atOne, 1.0F)}) {
        CHECK(l.project.empty());
        CHECK(l.scene.empty());
        CHECK_FALSE(l.dotDrawn);
        CHECK_FALSE(l.separatorDrawn);
    }
}

TEST_CASE("breadcrumb: a scale of 2 doubles the minimum gap, the gaps and the dot (task E.6.2, BD7)") {
    const ed::BreadcrumbText text = ed::breadcrumbText("Game", "a.scene.json", true);
    const ed::BreadcrumbLayout one = ed::breadcrumbLayout(300.0F, 1300.0F, 500.0F, text, atOne, 1.0F);
    const ed::BreadcrumbLayout two = ed::breadcrumbLayout(600.0F, 2600.0F, 1000.0F, text, atTwo, 2.0F);
    CHECK(two.projectX == 2.0F * one.projectX);
    CHECK(two.separatorX == 2.0F * one.separatorX);
    CHECK(two.sceneX == 2.0F * one.sceneX);
    CHECK(two.dotCenterX == 2.0F * one.dotCenterX);
    CHECK(two.dotDiameter == 12.0F);
    // The left fallback at scale 2 is menusEnd + 48 (seed S47).
    const ed::BreadcrumbLayout tight = ed::breadcrumbLayout(0.0F, 1496.0F, 1000.0F, text, atTwo, 2.0F);  // span 448
    CHECK(tight.runStart == 1048.0F);
}

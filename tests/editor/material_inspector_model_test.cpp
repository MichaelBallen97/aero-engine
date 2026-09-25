// tests/editor/material_inspector_model_test.cpp
// task E.3.4: the Material panel's PURE model -- the section table, the labels, the slot row's
// decisions, the status line and the panel geometry. Tier 0: no ImGui, no GPU, no window.
#include <aero/core/guid.hpp>
#include <aero/editor/material_edit.hpp>  // materialSlotIsSrgb -- MR6 reads it DIRECTLY
#include <aero/editor/material_inspector_model.hpp>
#include <aero/reflect/material_format.hpp>
#include <aero/render/material.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <ostream>  // the 0.4.1 MSVC trap: this TU CHECKs string_views
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {
// The printers go in the TYPE's namespace, not in this file's anonymous one: doctest calls
// operator<< UNQUALIFIED from inside doctest::detail, so only namespaces ASSOCIATED WITH THE ARGUMENT
// are searched (E.2.2's finding 2 -- an anonymous-namespace printer for an engine type is invisible
// and nothing warns).
inline std::ostream& operator<<(std::ostream& os, MaterialSlotNotice n) { return os << static_cast<int>(n); }
inline std::ostream& operator<<(std::ostream& os, MaterialStatusSeverity s) { return os << static_cast<int>(s); }
inline std::ostream& operator<<(std::ostream& os, MaterialPanelMode m) { return os << static_cast<int>(m); }
}  // namespace engine::editor

using engine::MaterialTextureSlot;  // the FORMAT's, not render::'s -- material_edit.hpp's convention
using engine::editor::MATERIAL_BODY_FLOOR_POINTS;
using engine::editor::MATERIAL_BODY_MIN_FONT;
using engine::editor::MATERIAL_FIELD_COUNT;
using engine::editor::MATERIAL_PREVIEW_FRACTION;
using engine::editor::MATERIAL_PREVIEW_MAX_FONT;
using engine::editor::MATERIAL_PREVIEW_MIN_FONT;
using engine::editor::MATERIAL_SECTION_COUNT;
using engine::editor::MATERIAL_SLOT_THUMB_FONT_MULTIPLE;
using engine::editor::materialColorSpaceNote;
using engine::editor::materialDirtyWord;
using engine::editor::MaterialFieldId;
using engine::editor::materialFieldLabel;
using engine::editor::MaterialPanelLayout;
using engine::editor::materialPanelLayout;
using engine::editor::MaterialPanelMetrics;
using engine::editor::MaterialPanelMode;
using engine::editor::materialSamplerNodeLabel;
using engine::editor::MaterialSection;
using engine::editor::MaterialSectionId;
using engine::editor::materialSections;
using engine::editor::materialSlotIsSrgb;
using engine::editor::MaterialSlotNotice;
using engine::editor::materialSlotNoticeText;
using engine::editor::MaterialSlotRow;
using engine::editor::materialSlotRow;
using engine::editor::materialSlotSamplerIsDefault;
using engine::editor::MaterialStatusLine;
using engine::editor::materialStatusLine;
using engine::editor::MaterialStatusSeverity;
using engine::editor::materialUnknownKeySummary;

namespace {

// Every string this model returns must be ASCII: the one UI font draws '?' past a small fixed set
// (.claude/rules/editor.md, "The UI font"; 3.1.3's post-merge lesson). One helper, six callers.
[[nodiscard]] bool isAscii(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80U; });
}

// A metric set at this tree's unscaled desktop defaults. `availHeight` is the only parameter the
// sweep cases vary.
[[nodiscard]] MaterialPanelMetrics desktopMetrics(float availHeight) {
    return MaterialPanelMetrics{.availHeight = availHeight,
                                .fontSize = 13.0F,
                                .frameHeight = 19.0F,
                                .textLineHeight = 13.0F,
                                .itemSpacingY = 4.0F,
                                .separatorHeight = 1.0F};
}

// THE MEASURED 2x METRIC SET, not a scaled-up guess: read off the live style in a 320x180 and an
// 800x500 editor window on a 3024x1964 built-in Retina panel at a display scale of 2.0. The style is
// doubled by ScaleAllSizes(SDL_GetWindowDisplayScale(win)) (imgui_layer.cpp:87-89) and the font is
// NOT, which is exactly why frame is 25 while fontSize stays 13.
[[nodiscard]] MaterialPanelMetrics retinaMetrics(float availHeight) {
    return MaterialPanelMetrics{.availHeight = availHeight,
                                .fontSize = 13.0F,
                                .frameHeight = 25.0F,
                                .textLineHeight = 13.0F,
                                .itemSpacingY = 8.0F,
                                .separatorHeight = 2.0F};
}

// `fixed` is not returned directly; it is RECOVERABLE from the struct, which is what lets MR15 assert
// a BOUND rather than a magic number. Stated once, here, so four cases share one derivation.
[[nodiscard]] float fixedChrome(const MaterialPanelLayout& l) {
    return (l.headerHeight - l.previewHeight) + l.footerHeight;
}

[[nodiscard]] bool allFinite(const MaterialPanelLayout& l) {
    return std::isfinite(l.headerHeight) && std::isfinite(l.previewHeight) && std::isfinite(l.footerHeight) &&
           std::isfinite(l.bodyHeight) && std::isfinite(l.thumbEdge);
}

// A bound slot with a valid, non-nil guid. The generator is not used: nothing here depends on the
// VALUE, only on valid() being true, and a literal keeps every case deterministic by construction.
[[nodiscard]] MaterialTextureSlot boundSlot() {
    MaterialTextureSlot slot{};
    slot.guid = engine::Guid{.hi = 0U, .lo = 7U};
    return slot;
}

}  // namespace

TEST_CASE("material inspector: the section table is the enum, in order, ASCII and distinct (MR1)") {
    const std::span<const MaterialSection> sections = materialSections();
    REQUIRE(sections.size() == MATERIAL_SECTION_COUNT);
    std::vector<std::string_view> titles;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        CAPTURE(i);
        CHECK((sections[i].id == static_cast<MaterialSectionId>(i)));  // double parens: scoped enum
        CHECK_FALSE(sections[i].title.empty());
        CHECK(isAscii(sections[i].title));
        titles.push_back(sections[i].title);
    }
    std::sort(titles.begin(), titles.end());
    CHECK(std::adjacent_find(titles.begin(), titles.end()) == titles.end());  // distinct
}

TEST_CASE("material inspector: every one of docs/09 11.1's ten scalar keys appears exactly once (MR2)") {
    // A redesign that drops occlusionStrength is invisible to every other tier in this tree: I86
    // asserts document VALUES through seams and never which rows were DRAWN. This is the only place
    // it reddens.
    std::array<int, MATERIAL_FIELD_COUNT> tally{};
    std::size_t total = 0;
    for (const MaterialSection& section : materialSections()) {
        for (const MaterialFieldId field : section.fields) {
            const auto index = static_cast<std::size_t>(field);
            REQUIRE(index < MATERIAL_FIELD_COUNT);
            ++tally.at(index);
            ++total;
        }
    }
    for (std::size_t i = 0; i < MATERIAL_FIELD_COUNT; ++i) {
        CAPTURE(i);
        CAPTURE(materialFieldLabel(static_cast<MaterialFieldId>(i)));
        CHECK(tally.at(i) == 1);
    }
    // ANTI-VACUITY: a table with no fields at all would satisfy nothing above if the loop never ran.
    CHECK(total == MATERIAL_FIELD_COUNT);
}

TEST_CASE("material inspector: the five slot sections are in materialSlotAt's binding order (MR3)") {
    // A TRIPLE identity, and that is why this is an assertion rather than a comment: the section
    // order, render::materialSlotAt's BINDING order, and docs/09 11.1's canonical order for
    // `textures` are ONE fact. A format change that reorders `textures` reddens HERE instead of
    // silently desynchronising the panel from the file.
    std::vector<std::size_t> slots;
    std::size_t withoutSlot = 0;
    for (const MaterialSection& section : materialSections()) {
        if (section.slot.has_value()) {
            slots.push_back(*section.slot);
        } else {
            ++withoutSlot;
        }
    }
    REQUIRE(slots.size() == engine::render::MATERIAL_TEXTURE_SLOT_COUNT);
    CHECK(withoutSlot == 3);  // Material, Rendering, File
    for (std::size_t i = 0; i < slots.size(); ++i) {
        CAPTURE(i);
        CHECK(slots[i] == i);  // ASCENDING, which IS materialSlotAt's order
    }
}

TEST_CASE("material inspector: materialFieldLabel is total, distinct, ASCII, six unchanged (MR4)") {
    std::vector<std::string_view> labels;
    for (std::size_t i = 0; i < MATERIAL_FIELD_COUNT; ++i) {
        const std::string_view label = materialFieldLabel(static_cast<MaterialFieldId>(i));
        CAPTURE(i);
        CHECK_FALSE(label.empty());
        CHECK(isAscii(label));
        labels.push_back(label);
    }
    std::sort(labels.begin(), labels.end());
    CHECK(std::adjacent_find(labels.begin(), labels.end()) == labels.end());

    SUBCASE("the six unrenamed labels are byte-identical to what 3.4.2 ships") {
        // Four labels ARE renamed on purpose (Base color -> Tint, Emissive -> Color, Normal scale ->
        // Scale, Occlusion strength -> Strength) because the section title already carries the word.
        // These six are not, and pinning them is what makes the diff read as FOUR renames.
        CHECK(materialFieldLabel(MaterialFieldId::Name) == std::string_view("Name"));
        CHECK(materialFieldLabel(MaterialFieldId::MetallicFactor) == std::string_view("Metallic"));
        CHECK(materialFieldLabel(MaterialFieldId::RoughnessFactor) == std::string_view("Roughness"));
        CHECK(materialFieldLabel(MaterialFieldId::AlphaMode) == std::string_view("Alpha mode"));
        CHECK(materialFieldLabel(MaterialFieldId::AlphaCutoff) == std::string_view("Alpha cutoff"));
        CHECK(materialFieldLabel(MaterialFieldId::DoubleSided) == std::string_view("Double sided"));
    }
    SUBCASE("the four renamed ones are the new strings, stated so a silent revert reddens") {
        CHECK(materialFieldLabel(MaterialFieldId::BaseColorFactor) == std::string_view("Tint"));
        CHECK(materialFieldLabel(MaterialFieldId::EmissiveFactor) == std::string_view("Color"));
        CHECK(materialFieldLabel(MaterialFieldId::NormalScale) == std::string_view("Scale"));
        CHECK(materialFieldLabel(MaterialFieldId::OcclusionStrength) == std::string_view("Strength"));
    }
}

TEST_CASE("material inspector: materialSlotSamplerIsDefault is the six tokens and NOT the guid (MR5)") {
    const MaterialTextureSlot d{};
    REQUIRE(materialSlotSamplerIsDefault(d));  // the format's own defaults: uvSet 0, repeat/repeat,
                                               // linear/linear/linear (docs/09 11.1)
    SUBCASE("each of the six tokens, flipped ALONE, makes it false") {
        {
            MaterialTextureSlot s = d;
            s.uvSet = 1;
            CHECK_FALSE(materialSlotSamplerIsDefault(s));
        }
        {
            MaterialTextureSlot s = d;
            s.wrapU = engine::MaterialWrap::Clamp;
            CHECK_FALSE(materialSlotSamplerIsDefault(s));
        }
        {
            MaterialTextureSlot s = d;
            s.wrapV = engine::MaterialWrap::Mirror;
            CHECK_FALSE(materialSlotSamplerIsDefault(s));
        }
        {
            MaterialTextureSlot s = d;
            s.minFilter = engine::MaterialFilter::Nearest;
            CHECK_FALSE(materialSlotSamplerIsDefault(s));
        }
        {
            MaterialTextureSlot s = d;
            s.magFilter = engine::MaterialFilter::Nearest;
            CHECK_FALSE(materialSlotSamplerIsDefault(s));
        }
        // NOTE THE TYPE: mipFilter is `MaterialMipFilter`, which has a `None` enumerator;
        // minFilter/magFilter are `MaterialFilter`, which has only Nearest/Linear. Three enums, and
        // the third is easy to write as the second.
        {
            MaterialTextureSlot s = d;
            s.mipFilter = engine::MaterialMipFilter::None;
            CHECK_FALSE(materialSlotSamplerIsDefault(s));
        }
    }
    SUBCASE("the GUID is the seventh member and is deliberately NOT compared") {
        // A slot is "custom" because of how it is SAMPLED, not because of what it points at. Without
        // this clause a predicate that compared the whole struct would pass every arm above.
        const MaterialTextureSlot s = boundSlot();
        REQUIRE(s.guid.valid());  // anti-vacuity: the flip really happened
        CHECK(materialSlotSamplerIsDefault(s));
    }
}

TEST_CASE("material inspector: the colour-space note agrees with materialSlotIsSrgb, all five (MR6)") {
    // The case reads materialSlotIsSrgb ITSELF rather than a literal index list -- which is what makes
    // this a statement about COMPOSITION. A hand-written index list in the implementation would agree
    // with a hand-written index list here and neither would be tested.
    const std::string_view srgb = materialColorSpaceNote(0);
    const std::string_view linear = materialColorSpaceNote(1);
    REQUIRE(srgb != linear);  // anti-vacuity: there really are two sentences
    CHECK(isAscii(srgb));
    CHECK(isAscii(linear));
    for (std::size_t i = 0; i < engine::render::MATERIAL_TEXTURE_SLOT_COUNT; ++i) {
        CAPTURE(i);
        CHECK(materialColorSpaceNote(i) == (materialSlotIsSrgb(i) ? srgb : linear));
    }
    SUBCASE("and the split is the one docs/09 11.1 describes -- 0 and 4 sRGB, 1/2/3 linear") {
        // Not redundant with the loop: the loop proves AGREEMENT and this proves the predicate has
        // not itself been inverted underneath both.
        CHECK(materialColorSpaceNote(0) == srgb);
        CHECK(materialColorSpaceNote(4) == srgb);
        CHECK(materialColorSpaceNote(1) == linear);
        CHECK(materialColorSpaceNote(2) == linear);
        CHECK(materialColorSpaceNote(3) == linear);
    }
}

TEST_CASE("material inspector: the sampler node says Sampler, or Sampler (custom) (MR7)") {
    const std::string_view plain = materialSamplerNodeLabel(true);
    const std::string_view custom = materialSamplerNodeLabel(false);
    CHECK(plain == std::string_view("Sampler"));
    CHECK(custom == std::string_view("Sampler (custom)"));
    // The false form CONTAINS the true form, so a reader sees one node with a qualifier rather than
    // two unrelated nodes.
    CHECK(custom.find(plain) == 0U);
    CHECK(isAscii(custom));
}

TEST_CASE("material inspector: an unbound slot is None, with nothing to clear (MR8)") {
    const MaterialSlotRow row =
        materialSlotRow(std::nullopt, {}, false, /*databaseAvailable=*/true, /*recordFound=*/false);
    CHECK(row.valueText == "None");
    CHECK_FALSE(row.clearEnabled);
    CHECK((row.notice == MaterialSlotNotice::None));
    // TRUE, deliberately: an absent slot has nothing custom about it, so the disclosure that is not
    // drawn would not have said "(custom)" either.
    CHECK(row.samplerIsDefault);
}

TEST_CASE("material inspector: a bound, resolved texture reads its FULL relative path (MR9)") {
    const MaterialTextureSlot slot = boundSlot();
    const MaterialSlotRow row = materialSlotRow(slot, "textures/copper_albedo.png", /*recordIsTexture=*/true,
                                                /*databaseAvailable=*/true, /*recordFound=*/true);
    // The FULL path, not the leaf. The Inspector's guidFieldRow says "<leaf> (<kind>)"
    // (inspector_model.hpp:84-86) and that difference is DELIBERATE: this row is the only place the
    // user sees which file in which FOLDER a material references. Do not unify them.
    CHECK(row.valueText == "textures/copper_albedo.png");
    CHECK(row.clearEnabled);
    CHECK((row.notice == MaterialSlotNotice::None));
}

TEST_CASE("material inspector: the two notices, and the no-database state that is NEITHER (MR10)") {
    const MaterialTextureSlot slot = boundSlot();
    SUBCASE("(a) resolved but not a texture -> NotATexture, and Clear stays live") {
        const MaterialSlotRow row = materialSlotRow(slot, "meshes/hero.obj", /*recordIsTexture=*/false, true, true);
        CHECK((row.notice == MaterialSlotNotice::NotATexture));
        CHECK(row.valueText == "meshes/hero.obj");  // the path still shows: AC-21 NAMES it
        CHECK(row.clearEnabled);
    }
    SUBCASE("(b) a database that does not know the guid -> Unresolvable") {
        const MaterialSlotRow row = materialSlotRow(slot, {}, false, /*databaseAvailable=*/true, /*recordFound=*/false);
        CHECK((row.notice == MaterialSlotNotice::Unresolvable));
        CHECK(row.valueText == "(unresolved)");
        CHECK(row.clearEnabled);
    }
    SUBCASE("(c) NO DATABASE AT ALL -> unresolved, and NO NOTICE. AC-17.") {
        // The panel is not entitled to say "this GUID is not in this project" when it has not read
        // one. This is TODAY's behaviour (the panel gates that notice on `database != nullptr`) and
        // NOTHING that existed before E.3.4 could see it: I86 binds a dead GUID with a LIVE database,
        // and no case anywhere drives the panel with a null database and inspects what it drew.
        const MaterialSlotRow row =
            materialSlotRow(slot, {}, false, /*databaseAvailable=*/false, /*recordFound=*/false);
        CHECK((row.notice == MaterialSlotNotice::None));
        CHECK(row.valueText == "(unresolved)");
        CHECK(row.clearEnabled);
    }
    SUBCASE("(d) samplerIsDefault tracks the slot, not the record") {
        MaterialTextureSlot custom = slot;
        custom.wrapU = engine::MaterialWrap::Clamp;
        CHECK_FALSE(materialSlotRow(custom, "t.png", true, true, true).samplerIsDefault);
        CHECK(materialSlotRow(slot, "t.png", true, true, true).samplerIsDefault);
    }
}

TEST_CASE("material inspector: the notice sentences are the panel's own, verbatim (MR11)") {
    // VERBATIM the two slot notices 3.4.2 ships -- NOT the PREVIEW-TEXTURE-STATE switch's
    // "Loading the preview texture..." and Failed arms, which are a different pair of sentences and
    // stay where they are. Pinning them is what makes the diff a MOVE rather than a re-wording.
    CHECK(materialSlotNoticeText(MaterialSlotNotice::None).empty());
    CHECK(materialSlotNoticeText(MaterialSlotNotice::NotATexture) ==
          std::string_view("This asset is not a texture; the slot will use its default."));
    CHECK(materialSlotNoticeText(MaterialSlotNotice::Unresolvable) ==
          std::string_view("This GUID is not in this project; the slot will use its default."));
    CHECK(isAscii(materialSlotNoticeText(MaterialSlotNotice::NotATexture)));
    CHECK(isAscii(materialSlotNoticeText(MaterialSlotNotice::Unresolvable)));
}

TEST_CASE("material inspector: the status line resolves by priority, pairwise (MR12)") {
    constexpr std::string_view INVALID = "alphaCutoff is out of range";
    constexpr std::string_view LAST = "Saved.";
    const std::string_view none{};

    SUBCASE("all four present -> the invalid message wins, at Warning") {
        const MaterialStatusLine s = materialStatusLine(INVALID, true, 3, LAST);
        CHECK(s.text == INVALID);
        CHECK((s.severity == MaterialStatusSeverity::Warning));
    }
    SUBCASE("invalid absent -> the external change wins, at Notice") {
        const MaterialStatusLine s = materialStatusLine(none, true, 3, LAST);
        CHECK(s.text == "This file changed on disk; Apply will overwrite it.");
        CHECK((s.severity == MaterialStatusSeverity::Notice));
    }
    SUBCASE("invalid and external absent -> the unknown-key summary wins, at Notice") {
        const MaterialStatusLine s = materialStatusLine(none, false, 3, LAST);
        CHECK(s.text == materialUnknownKeySummary(3));
        CHECK((s.severity == MaterialStatusSeverity::Notice));
    }
    SUBCASE("only lastMessage -> the message, at Disabled") {
        const MaterialStatusLine s = materialStatusLine(none, false, 0, LAST);
        CHECK(s.text == LAST);
        CHECK((s.severity == MaterialStatusSeverity::Disabled));
    }
    SUBCASE("nothing at all -> EMPTY, at Disabled -- and the panel draws it ANYWAY") {
        const MaterialStatusLine s = materialStatusLine(none, false, 0, none);
        CHECK(s.text.empty());
        CHECK((s.severity == MaterialStatusSeverity::Disabled));
    }
    SUBCASE("ANTI-VACUITY: each lower arm is REACHABLE on its own") {
        // Without this, an implementation that returned the invalid message unconditionally would
        // satisfy every "wins" subcase above by accident of their fixtures.
        CHECK((materialStatusLine(none, true, 0, none).severity == MaterialStatusSeverity::Notice));
        CHECK(materialStatusLine(none, false, 1, none).text == materialUnknownKeySummary(1));
        CHECK(materialStatusLine(none, false, 0, LAST).text == LAST);
    }
}

TEST_CASE("material inspector: the unknown-key summary pluralises, and the dirty word (MR13)") {
    CHECK(materialUnknownKeySummary(0).empty());
    // The SINGULAR arm is its own assertion because "always pluralises" is the kind of thing that
    // ships wrong and never reddens (S15's only witness anywhere).
    CHECK(materialUnknownKeySummary(1) == "1 unknown key will be removed on Apply.");
    CHECK(materialUnknownKeySummary(2) == "2 unknown keys will be removed on Apply.");
    CHECK(materialUnknownKeySummary(17) == "17 unknown keys will be removed on Apply.");
    CHECK(isAscii(materialUnknownKeySummary(2)));

    CHECK(materialDirtyWord(true) == std::string_view("Unsaved changes"));
    CHECK(materialDirtyWord(false).empty());
    CHECK(isAscii(materialDirtyWord(true)));
}

TEST_CASE("material inspector: a desktop panel gets a preview LARGER than the 180 it replaces (MR14)") {
    const MaterialPanelLayout l = materialPanelLayout(desktopMetrics(700.0F));
    CHECK((l.mode == MaterialPanelMode::FixedRegions));  // double parens: scoped enum
    CHECK(l.previewShown);
    // STRICTLY larger than the constant it replaces (PREVIEW_HEIGHT_POINTS was 180.0F) -- which is
    // the deliverable's "a larger live preview", stated as arithmetic rather than as an impression.
    CHECK(l.previewHeight > 180.0F);
    CHECK(l.bodyHeight >= MATERIAL_BODY_MIN_FONT * 13.0F);
    CHECK(l.thumbEdge == doctest::Approx(MATERIAL_SLOT_THUMB_FONT_MULTIPLE * 13.0F));

    // THE FOOTER, PINNED EXACTLY, AND THE LITERAL IS INDEPENDENTLY DERIVED rather than recomputed
    // from the metrics: spacing + separator + spacing + frame + spacing + textLine
    // == 4 + 1 + 4 + 19 + 4 + 13 == 45. Every term is a small integer, so binary32 is exact and an
    // Approx would only hide a real error. THREE spacings -- EndChild calls ItemSize(child_size)
    // (imgui.cpp:6860) and ItemSize advances by line_height + ItemSpacing.y (:12129), which is the
    // leading spacing this plan first dropped; ImGui's own changelog calls the two-term form a bug
    // (imgui.cpp:437). This assertion is the ONLY thing that reddens seed S53.
    CHECK(l.footerHeight == 45.0F);
    // And the chrome that follows from it, also exact at these metrics: 17 + 9 + 45.
    CHECK(fixedChrome(l) == 71.0F);
}

TEST_CASE("material inspector: the preview is shown whenever the chrome leaves four points (MR15)") {
    constexpr float FONT = 13.0F;
    // THE SWEEP RUNS TO 80 FONT UNITS, NOT 40. The saturation knee is at
    // MATERIAL_PREVIEW_MAX_FONT / MATERIAL_PREVIEW_FRACTION == 63.16 font units, so a 40-unit sweep
    // NEVER reaches it and an "at least one sample saturates" arm would fail on a CORRECT tree. A
    // retune that pushes the knee past 80 reddens the saturation arm visibly rather than quietly
    // un-covering it.
    constexpr float MAX_UNITS = 80.0F;
    // TWO SWEEPS, NOT ONE, AND THE SECOND IS NOT SYMMETRY. The body-minimum clamp binds NOWHERE at 1x
    // with these constants -- `avail - 188 > 130` and `avail - 188 < 0.38*avail` have no common
    // solution -- and binds only over a handful of integer samples at the MEASURED 2x metrics. A
    // 1x-only sweep therefore leaves seed S17 uncovered and makes the `sawBodyClamped` arm below
    // unsatisfiable. The 2x set is the one step 0 actually measured.
    const std::array<MaterialPanelMetrics, 2> bases{desktopMetrics(0.0F), retinaMetrics(0.0F)};

    bool sawScrolling = false;
    bool sawFixedRegions = false;
    bool sawBodyClamped = false;
    bool sawSaturated = false;

    for (const MaterialPanelMetrics& base : bases) {
        CAPTURE(base.frameHeight);  // 19 == the 1x sweep, 25 == the measured 2x sweep
        for (int step = 0; step <= static_cast<int>(MAX_UNITS * FONT); ++step) {
            const auto avail = static_cast<float>(step);
            CAPTURE(avail);
            MaterialPanelMetrics m = base;
            m.availHeight = avail;
            const MaterialPanelLayout l = materialPanelLayout(m);
            REQUIRE(allFinite(l));
            CHECK(l.footerHeight > 0.0F);
            CHECK(l.previewShown == (l.previewHeight >= 1.0F));

            // AC-3, UNCONDITIONAL. There is no antecedent left to falsify: both modes floor the
            // preview at minPreview, so EVERY sample shows one. This single clause is what the whole
            // revision exists for -- before the fallback, a 98-point content region against a
            // 103-point chrome produced ZERO here and I99 failed on every Retina Mac.
            CHECK(l.previewShown);
            CHECK(l.previewHeight >= MATERIAL_PREVIEW_MIN_FONT * FONT);

            // THE MODE BOUNDARY, ASSERTED FROM BOTH SIDES, against the IMPLEMENTATION's own chrome
            // recovered from the struct rather than against a number written here. A retune of any of
            // the five constants therefore cannot move the boundary without reddening this.
            const float usable = std::max(0.0F, avail - fixedChrome(l));
            const bool fixedViable = usable >= (MATERIAL_PREVIEW_MIN_FONT * FONT) + MATERIAL_BODY_FLOOR_POINTS;
            CHECK((l.mode == (fixedViable ? MaterialPanelMode::FixedRegions : MaterialPanelMode::Scrolling)));

            if (l.mode == MaterialPanelMode::FixedRegions) {
                sawFixedRegions = true;
                CHECK(l.bodyHeight >= MATERIAL_BODY_FLOOR_POINTS);
            } else {
                sawScrolling = true;
                // ZERO, not the floor: nothing reserves a body in this mode, and a reader that sees
                // MATERIAL_BODY_FLOOR_POINTS here would think something did.
                CHECK(l.bodyHeight == 0.0F);
                CHECK(l.previewHeight == MATERIAL_PREVIEW_MIN_FONT * FONT);  // exact: the floor IS the value
            }

            const float unclamped = std::min(avail * MATERIAL_PREVIEW_FRACTION, MATERIAL_PREVIEW_MAX_FONT * FONT);
            if (l.mode == MaterialPanelMode::FixedRegions && l.previewHeight < unclamped - 0.5F) {
                sawBodyClamped = true;  // the body's minimum actually bound -- 2x only, see above
            }
            if (l.previewHeight >= (MATERIAL_PREVIEW_MAX_FONT * FONT) - 0.001F) {
                sawSaturated = true;
            }
        }
    }
    // ANTI-VACUITY, four arms, and every one of them fires on a correct tree -- which is the property
    // the arm this replaced did NOT have (the old `sawHidden` is now unsatisfiable by design, because
    // previewShown is unconditionally true).
    CHECK(sawScrolling);     // the fallback is REACHABLE, which is the whole revision
    CHECK(sawFixedRegions);  // and so is the fixed-region walk
    CHECK(sawBodyClamped);   // the body-minimum clamp binds somewhere in the 2x sweep
    CHECK(sawSaturated);     // and the sweep is wide enough to reach MAX (the knee at 63.16 units)
}

TEST_CASE("material inspector: previewHeight never shrinks, and does not JUMP at the mode boundary (MR16)") {
    // A layout whose preview SHRINKS as the panel grows is a bug no single-sample case can see. With
    // the fallback there is a SECOND bug of the same shape and it is worse: a preview that STEPS at
    // the mode boundary means dragging the dock divider one point moves the picture 130 points. The
    // threshold was chosen as `minPreview + FLOOR` precisely so the two modes agree EXACTLY there, so
    // this case asserts continuity as well as monotonicity.
    constexpr float FONT = 13.0F;
    const std::array<MaterialPanelMetrics, 2> bases{desktopMetrics(0.0F), retinaMetrics(0.0F)};

    for (const MaterialPanelMetrics& base : bases) {
        CAPTURE(base.frameHeight);
        float previous = -1.0F;
        float largestStep = 0.0F;
        bool crossedBoundary = false;
        MaterialPanelMode previousMode = MaterialPanelMode::Scrolling;

        for (int step = 0; step <= static_cast<int>(80.0F * FONT); ++step) {
            const auto avail = static_cast<float>(step);
            CAPTURE(avail);
            MaterialPanelMetrics m = base;
            m.availHeight = avail;
            const MaterialPanelLayout l = materialPanelLayout(m);

            CHECK(l.previewHeight >= previous);  // MONOTONE, seed S22's guard
            if (previous >= 0.0F) {
                largestStep = std::max(largestStep, l.previewHeight - previous);
                if (l.mode != previousMode) {
                    crossedBoundary = true;
                    // CONTINUITY AT THE BOUNDARY, asserted where it happens. A STEP here is seed S51
                    // (a threshold that is not the continuous one) and it is plainly visible in the
                    // product as a jumping picture.
                    CHECK(l.previewHeight - previous < 1.0F);
                }
            }
            previous = l.previewHeight;
            previousMode = l.mode;
        }
        // ANTI-VACUITY, both arms. Without the first, a sweep that never left one mode would satisfy
        // the continuity clause by never evaluating it.
        CHECK(crossedBoundary);
        CHECK(previous > 0.0F);
        // The whole sweep's largest single step, stated globally so a discontinuity ANYWHERE reddens
        // and not only at the mode change. ONE POINT of availHeight may move the preview by AT MOST
        // one point: the fraction arm's slope is 0.38 and the fallback's is 0, but the body-minimum
        // clamp's arm is `usable - minBody`, whose slope is exactly 1 -- so the bound is `<=` and not
        // `<`, and the 2x sweep really does attain it (measured: exactly 1.0 over avail 350..354,
        // where max(minPreview, usable - minBody) crosses minPreview and begins to govern).
        CHECK(largestStep <= 1.0F);
    }
}

TEST_CASE("material inspector: a huge panel saturates the preview exactly at MAX (MR17)") {
    const MaterialPanelLayout l = materialPanelLayout(desktopMetrics(100000.0F));
    // Saturation is a FIXED-REGIONS claim: the fallback hands back minPreview and never reaches MAX,
    // so asserting the mode is what stops this case quietly becoming a statement about the wrong arm.
    CHECK((l.mode == MaterialPanelMode::FixedRegions));
    // EXACT, because the arithmetic is exact: the saturating clamp is `std::min(preview, maxPreview)`
    // and maxPreview is `MATERIAL_PREVIEW_MAX_FONT * fontSize` computed the same way on both sides.
    CHECK(l.previewHeight == MATERIAL_PREVIEW_MAX_FONT * 13.0F);
    CHECK(l.bodyHeight > 90000.0F);  // the body took the rest
    CHECK(l.previewShown);
}

TEST_CASE("material inspector: no degenerate metric can produce a non-finite layout (MR18)") {
    // A NaN reaching SetNextWindowSize or ImGui::Image is an assertion, and std::clamp(NaN, lo, hi)
    // returns NaN on libc++ (3.7.2's standing rule) -- which is why the implementation uses ordered
    // std::min/std::max pairs with NEGATED comparisons and never std::clamp. S20 and S21 are the two
    // seeds this case exists for.
    constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
    constexpr float INF_F = std::numeric_limits<float>::infinity();
    const std::array<float, 4> poisons{NAN_F, INF_F, -INF_F, 0.0F};

    for (const float poison : poisons) {
        CAPTURE(poison);
        std::array<MaterialPanelMetrics, 7> cases{};
        cases.fill(desktopMetrics(700.0F));
        cases.at(0).availHeight = poison;
        cases.at(1).fontSize = poison;
        cases.at(2).frameHeight = poison;
        cases.at(3).textLineHeight = poison;
        cases.at(4).itemSpacingY = poison;
        cases.at(5).separatorHeight = poison;
        cases.at(6) = MaterialPanelMetrics{.availHeight = poison,  // ALL AT ONCE, in DECLARATION order
                                           .fontSize = poison,
                                           .frameHeight = poison,
                                           .textLineHeight = poison,
                                           .itemSpacingY = poison,
                                           .separatorHeight = poison};

        for (std::size_t i = 0; i < cases.size(); ++i) {
            CAPTURE(i);
            const MaterialPanelLayout l = materialPanelLayout(cases.at(i));
            REQUIRE(allFinite(l));
            CHECK(l.headerHeight >= 0.0F);
            CHECK(l.footerHeight >= 0.0F);
            CHECK(l.thumbEdge > 0.0F);
            CHECK(l.previewShown == (l.previewHeight >= 1.0F));
            // UNCONDITIONAL, in both modes and for every poisoned metric: a NaN or an infinity must
            // still produce a PICTURE, not a blank region. The guards floor fontSize at 1, so the
            // floor here is 10 points however degenerate the input.
            CHECK(l.previewShown);
            CHECK(l.previewHeight >= MATERIAL_PREVIEW_MIN_FONT * 1.0F);
            // bodyHeight is MODE-DEPENDENT and an unconditional floor would be wrong the moment the
            // fallback existed: in Scrolling mode NOTHING reserves a body, so the honest value is 0.
            if (l.mode == MaterialPanelMode::FixedRegions) {
                CHECK(l.bodyHeight >= MATERIAL_BODY_FLOOR_POINTS);
            } else {
                CHECK(l.bodyHeight == 0.0F);
            }
        }
    }
}

TEST_CASE("material inspector: thumbEdge is the font multiple, and is positive for every metric (MR19)") {
    CHECK(materialPanelLayout(desktopMetrics(700.0F)).thumbEdge ==
          doctest::Approx(MATERIAL_SLOT_THUMB_FONT_MULTIPLE * 13.0F));
    // A zero edge means a slot row with NO thumbnail and NO error -- invisible in the product until
    // somebody notices the picture is missing (S23).
    CHECK(materialPanelLayout(MaterialPanelMetrics{}).thumbEdge > 0.0F);
    MaterialPanelMetrics poisoned{};
    poisoned.fontSize = std::numeric_limits<float>::quiet_NaN();
    CHECK(materialPanelLayout(poisoned).thumbEdge > 0.0F);
}

TEST_CASE("material inspector: header and footer are positive, and the header contains the preview (MR20)") {
    constexpr float FONT = 13.0F;
    const std::array<MaterialPanelMetrics, 2> bases{desktopMetrics(0.0F), retinaMetrics(0.0F)};
    for (const MaterialPanelMetrics& base : bases) {
        CAPTURE(base.frameHeight);
        for (int step = 0; step <= static_cast<int>(80.0F * FONT); step += 7) {  // stride: this is a
            const auto avail = static_cast<float>(step);                         // shape claim, not a sweep
            CAPTURE(avail);
            MaterialPanelMetrics m = base;
            m.availHeight = avail;
            const MaterialPanelLayout l = materialPanelLayout(m);
            CHECK(l.headerHeight > 0.0F);
            // POSITIVE IN BOTH MODES: the footer always DRAWS, it is only PINNED in one of them. A
            // zero here would mean the footer's own height had been conflated with whether a child
            // reserves it.
            CHECK(l.footerHeight > 0.0F);
            CHECK(l.headerHeight >= l.previewHeight);  // the identity line and the separator are in it
        }
    }
}

TEST_CASE("material inspector: the MEASURED Retina metrics still produce a picture (MR21)") {
    // THESE SIX NUMBERS WERE MEASURED, NOT DERIVED. Instrumentation at the top of onDraw's Ready arm,
    // run under -tc='*I99*' on a 3024x1964 built-in Retina panel at a display scale of 2.0, then
    // reverted: avail.y=98 and 418, font=13, frame=25, text=13, spacing=8, sep=2, with
    // availHeight == windowHeight - 82 at both window sizes. The style is doubled by
    // ScaleAllSizes(SDL_GetWindowDisplayScale(win)) (imgui_layer.cpp:87-89) while the font is NOT --
    // that gap is E.6.1's; this case only makes E.3.4 correct in its presence.
    //
    // BEFORE THE FALLBACK THIS CASE FAILED: the fixed chrome is 103 points against a 98-point content
    // region, so `usable` was negative and previewHeight came out ZERO -- taking I99, I135, I171's ON
    // arm and AC-3 down on every Retina Mac while the three 1x CI lanes stayed green.

    SUBCASE("a 320x180 editor window: the fallback, and a real preview") {
        const MaterialPanelLayout l = materialPanelLayout(retinaMetrics(98.0F));
        CHECK((l.mode == MaterialPanelMode::Scrolling));
        CHECK(l.previewShown);
        // EXACT: the fallback assigns minPreview, and 10 * 13 is exact in binary32.
        CHECK(l.previewHeight == 130.0F);
        CHECK(l.bodyHeight == 0.0F);
        // THE TWO NUMBERS THE CORRECTED FOOTER PRODUCES, pinned exactly and derived independently of
        // the implementation: 8 + 2 + 8 + 25 + 8 + 13 == 64, and 21 + 18 + 64 == 103. Seed S53
        // (footerHeight reverted to two spacings) reddens HERE and in MR14, and nowhere else.
        CHECK(l.footerHeight == 64.0F);
        CHECK(fixedChrome(l) == 103.0F);
    }
    SUBCASE("an 800x500 editor window: the fixed regions, and the fraction governs") {
        const MaterialPanelLayout l = materialPanelLayout(retinaMetrics(418.0F));
        CHECK((l.mode == MaterialPanelMode::FixedRegions));
        CHECK(l.previewShown);
        // STRICTLY above the fallback's floor is the discriminator: it proves the fixed-region arm
        // ran AND that the fraction, not the floor, chose the number. Asserting 418 * 0.38 exactly
        // would be re-deriving the implementation from the same constant it uses.
        CHECK(l.previewHeight > 130.0F);
        CHECK(l.previewHeight < 200.0F);
        CHECK(l.bodyHeight >= MATERIAL_BODY_FLOOR_POINTS);
        CHECK(l.footerHeight == 64.0F);
        CHECK(fixedChrome(l) == 103.0F);
    }
    SUBCASE("the boundary between them, from both sides, at the MEASURED chrome") {
        // 103 + 130 + 4 == 237. One point below is Scrolling, one point above is FixedRegions, and
        // the preview is the SAME at both -- which is the continuity MR16 asserts globally, pinned
        // here at the one boundary a Retina user will actually drag across.
        const MaterialPanelLayout below = materialPanelLayout(retinaMetrics(236.0F));
        const MaterialPanelLayout above = materialPanelLayout(retinaMetrics(238.0F));
        CHECK((below.mode == MaterialPanelMode::Scrolling));
        CHECK((above.mode == MaterialPanelMode::FixedRegions));
        CHECK(above.previewHeight - below.previewHeight < 1.0F);
        CHECK(above.previewHeight >= below.previewHeight);
    }
}

// tests/editor/material_card_test.cpp -- task E.4.5: the material card's PURE model (MB1-MB26). A TU of
// aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED and TIER 0: material_card.hpp needs no generated reflection, no GPU, no ImGui and no disk, so every
// case is PRESENT and PASSING in all three build configurations. NO PREPROCESSOR CONDITIONAL OF ANY KIND
// IN THIS FILE, and NO ENTROPY SOURCE: every "random" byte comes from a fixed-seed generator.
#include <aero/core/math.hpp>
#include <aero/editor/asset_view.hpp>
#include <aero/editor/material_card.hpp>
#include <aero/editor/material_preview_rig.hpp>
#include <aero/reflect/material_format.hpp>
#include <aero/render/environment.hpp>
#include <aero/render/lighting.hpp>
#include <aero/render/tonemap.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>  // MSVC alone needs the complete type to stringify a string_view inside a CHECK
#include <string>
#include <string_view>

using engine::MaterialDocument;
using engine::Vec3;
using engine::Vec4;
using engine::editor::IconColor;
using engine::editor::MATERIAL_CARD_SEPARATOR;
using engine::editor::MATERIAL_DISPLAY_NAME_ELLIPSIS;
using engine::editor::MaterialCard;
using engine::editor::materialCardFor;
using engine::editor::materialCardRowText;
using engine::editor::materialCardSubtitle;
using engine::editor::materialCardTint;
using engine::editor::MaterialDisplayName;
using engine::editor::materialNameMatchesStem;
using engine::editor::materialSwatchColor;
using engine::editor::materialSwatchWantsDarkLabel;
using engine::editor::materialThumbnailLighting;
using engine::editor::materialThumbnailTonemap;
using engine::editor::MAX_MATERIAL_DISPLAY_NAME_BYTES;
using engine::editor::sanitizeMaterialDisplayName;

namespace {

constexpr float QUIET_NAN = std::numeric_limits<float>::quiet_NaN();  // never NAN: that is a <cmath> macro
constexpr float POSITIVE_INFINITY = std::numeric_limits<float>::infinity();

// render/src/primitives.cpp's makeSphere RADIUS -- the sphere a thumbnail draws. Restated here because the
// render layer exports no constant for it; MB23 and MB25 are exactly the cases that would notice it changing.
constexpr float THUMBNAIL_SPHERE_RADIUS = 0.5F;

// The camera produce() builds: the thumbnail's own rig, its fixed azimuth, a square target.
[[nodiscard]] engine::render::CameraView thumbnailCamera() {
    return engine::editor::materialPreviewCamera(engine::editor::MATERIAL_THUMBNAIL_RIG,
                                                 engine::editor::MATERIAL_THUMBNAIL_ORBIT_ANGLE, 1.0F);
}

[[nodiscard]] MaterialDocument documentWithBaseColor(Vec4 color) {
    MaterialDocument document;
    document.baseColorFactor = color;
    return document;
}

// Bytes print as characters in doctest's failure output; an int prints as a number.
[[nodiscard]] int byteOf(std::uint8_t value) { return static_cast<int>(value); }

// TRUE when `text` does not END inside a UTF-8 sequence: the trailing continuation bytes, if any, are
// exactly what the lead byte before them announces. A truncation that stranded the head of a sequence
// fails this; one that stepped back to a boundary passes.
[[nodiscard]] bool endsOnUtf8Boundary(std::string_view text) {
    std::size_t continuation = 0;
    std::size_t i = text.size();
    while (i > 0 && (static_cast<unsigned char>(text[i - 1U]) & 0xC0U) == 0x80U) {
        ++continuation;
        --i;
    }
    if (i == 0) {
        return continuation == 0;
    }
    const auto lead = static_cast<unsigned char>(text[i - 1U]);
    if ((lead & 0x80U) == 0x00U) {
        return continuation == 0;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return continuation == 1;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return continuation == 2;
    }
    return (lead & 0xF8U) == 0xF0U && continuation == 3;
}

[[nodiscard]] std::string withEllipsis(const std::string& prefix) {
    return prefix + std::string(MATERIAL_DISPLAY_NAME_ELLIPSIS);
}

}  // namespace

// ---- sanitising: rules 1, 2, 3 and 5 (MB1-MB10) ------------------------------------------------------

TEST_CASE("material card: a clean name passes through untouched (MB1)") {
    const MaterialDisplayName name = sanitizeMaterialDisplayName("Studio Brass");
    CHECK(name.text == "Studio Brass");
    CHECK_FALSE(name.truncated);
}

TEST_CASE("material card: an empty or all-space name is no name (MB2)") {
    for (const std::string_view input : {std::string_view{}, std::string_view(" "), std::string_view("\t"),
                                         std::string_view("\r\n"), std::string_view(" \t \r\n \x7F ")}) {
        CAPTURE(input);
        const MaterialDisplayName name = sanitizeMaterialDisplayName(input);
        CHECK(name.text.empty());
        CHECK_FALSE(name.truncated);
    }
}

TEST_CASE("material card: control bytes become spaces and collapse (MB3)") {
    CHECK(sanitizeMaterialDisplayName("Studio\nBrass\tv2").text == "Studio Brass v2");
    CHECK(sanitizeMaterialDisplayName("a\x01\x02\x1F"
                                      "b")
              .text == "a b");
    CHECK(sanitizeMaterialDisplayName("a\x7F"
                                      "b")
              .text == "a b");
    CHECK(sanitizeMaterialDisplayName("\x7F").text.empty());
    // A line break reaching ImDrawList::AddText is a REAL line break -- the reason for rule 1. No byte below
    // 0x20 survives, and neither does DEL.
    const MaterialDisplayName hostile = sanitizeMaterialDisplayName("one\ntwo\rthree\x1B[31mred");
    CHECK(hostile.text == "one two three [31mred");
    for (const char c : hostile.text) {
        CHECK(static_cast<unsigned char>(c) >= 0x20U);
        CHECK(static_cast<unsigned char>(c) != 0x7FU);
    }
}

TEST_CASE("material card: runs of spaces collapse and both ends trim (MB4)") {
    CHECK(sanitizeMaterialDisplayName("  Studio   Brass  ").text == "Studio Brass");
    CHECK(sanitizeMaterialDisplayName("\t\tStudio \t \n Brass\r\n").text == "Studio Brass");
}

TEST_CASE("material card: bytes at or above 0x80 pass through untouched (MB5)") {
    // UTF-8 is never validated or re-encoded: "Creme brulee" with its three accents, byte for byte.
    const std::string_view accented =
        "Cr\xC3\xA8me br\xC3\xBBl\xC3\xA9"
        "e";
    CHECK(sanitizeMaterialDisplayName(accented).text == accented);
}

TEST_CASE("material card: the cap's ellipsis is U+2026 in UTF-8, the character elideForCaption appends (MB6)") {
    REQUIRE(MATERIAL_DISPLAY_NAME_ELLIPSIS.size() == 3U);
    CHECK(static_cast<int>(static_cast<unsigned char>(MATERIAL_DISPLAY_NAME_ELLIPSIS[0])) == 0xE2);
    CHECK(static_cast<int>(static_cast<unsigned char>(MATERIAL_DISPLAY_NAME_ELLIPSIS[1])) == 0x80);
    CHECK(static_cast<int>(static_cast<unsigned char>(MATERIAL_DISPLAY_NAME_ELLIPSIS[2])) == 0xA6);
}

TEST_CASE("material card: a name over the cap keeps the cap's bytes and ends in the ellipsis (MB7)") {
    const MaterialDisplayName name = sanitizeMaterialDisplayName(std::string(200, 'a'));
    CHECK(name.truncated);
    CHECK(name.text == withEllipsis(std::string(MAX_MATERIAL_DISPLAY_NAME_BYTES, 'a')));
    CHECK(name.text.size() == MAX_MATERIAL_DISPLAY_NAME_BYTES + MATERIAL_DISPLAY_NAME_ELLIPSIS.size());
}

TEST_CASE("material card: the cap is exact -- 96 bytes stay whole, 97 are cut (MB8)") {
    const std::string atCap(MAX_MATERIAL_DISPLAY_NAME_BYTES, 'a');
    const MaterialDisplayName whole = sanitizeMaterialDisplayName(atCap);
    CHECK_FALSE(whole.truncated);
    CHECK(whole.text == atCap);
    // Surrounding spaces are not content: trimmed before they could count against the cap.
    const MaterialDisplayName padded = sanitizeMaterialDisplayName("   " + atCap + "   ");
    CHECK_FALSE(padded.truncated);
    CHECK(padded.text == atCap);
    const MaterialDisplayName over = sanitizeMaterialDisplayName(atCap + "b");
    CHECK(over.truncated);
    CHECK(over.text == withEllipsis(atCap));
}

TEST_CASE("material card: the cut steps back to a UTF-8 boundary, never slicing a sequence (MB9)") {
    SUBCASE("a two-byte sequence straddling the cap") {
        const MaterialDisplayName name = sanitizeMaterialDisplayName(std::string(95, 'a') + "\xC3\xA9");
        REQUIRE(name.truncated);
        CHECK(name.text == withEllipsis(std::string(95, 'a')));
    }
    SUBCASE("a four-byte sequence straddling the cap") {
        const MaterialDisplayName name = sanitizeMaterialDisplayName(std::string(94, 'a') + "\xF0\x9F\x98\x80" + "zz");
        REQUIRE(name.truncated);
        CHECK(name.text == withEllipsis(std::string(94, 'a')));
    }
    SUBCASE("a run of three-byte characters keeps only whole ones") {
        std::string input;
        for (int i = 0; i < 40; ++i) {
            input += "\xE2\x82\xAC";  // U+20AC, three bytes; the cap (96) falls on a character boundary
        }
        const MaterialDisplayName name = sanitizeMaterialDisplayName(input);
        REQUIRE(name.truncated);
        REQUIRE(name.text.size() > MATERIAL_DISPLAY_NAME_ELLIPSIS.size());
        const std::string_view prefix(name.text.data(), name.text.size() - MATERIAL_DISPLAY_NAME_ELLIPSIS.size());
        CHECK(prefix.size() % 3U == 0U);
        CHECK(prefix.size() <= MAX_MATERIAL_DISPLAY_NAME_BYTES);
        CHECK(endsOnUtf8Boundary(prefix));
    }
}

TEST_CASE("material card: a space left at the cut is dropped before the ellipsis (MB10)") {
    const MaterialDisplayName name = sanitizeMaterialDisplayName(std::string(95, 'a') + " bbbb");
    REQUIRE(name.truncated);
    CHECK(name.text == withEllipsis(std::string(95, 'a')));
}

// ---- the swatch (MB11-MB15) ---------------------------------------------------------------------------

TEST_CASE("material card: the swatch is the base colour through the thumbnail's own tone curve (MB11)") {
    // ORACLES COMPUTED INDEPENDENTLY, in float32, from tonemap.hpp's published constants: the ACES fit
    // x(2.51x + 0.03) / (x(2.43x + 0.59) + 0.14), saturate, the sRGB OETF, times 255, rounded. Every value
    // sits at least 0.1 of a byte from a rounding edge, so no libm or FMA difference can move one. NOT
    // recomputed here through tonemapAndEncode: a test deriving both sides from one source asserts nothing.
    struct Oracle {
        float linear;
        int byte;
    };
    constexpr std::array<Oracle, 6> ORACLES{
        {{0.0F, 0}, {0.18F, 141}, {0.5F, 206}, {1.0F, 232}, {2.0F, 245}, {8.0F, 255}}};
    for (const Oracle& oracle : ORACLES) {
        CAPTURE(oracle.linear);
        const std::optional<IconColor> swatch =
            materialSwatchColor(documentWithBaseColor(Vec4{oracle.linear, oracle.linear, oracle.linear, 1.0F}));
        REQUIRE(swatch.has_value());
        CHECK(byteOf(swatch->r) == oracle.byte);
        CHECK(byteOf(swatch->g) == oracle.byte);
        CHECK(byteOf(swatch->b) == oracle.byte);
        CHECK(byteOf(swatch->a) == 255);
    }
    // Per channel, independently -- a swapped or shared channel would pass every grey above.
    const std::optional<IconColor> mixed = materialSwatchColor(documentWithBaseColor(Vec4{0.5F, 1.0F, 2.0F, 1.0F}));
    REQUIRE(mixed.has_value());
    CHECK(byteOf(mixed->r) == 206);
    CHECK(byteOf(mixed->g) == 232);
    CHECK(byteOf(mixed->b) == 245);
}

TEST_CASE("material card: the swatch ignores alpha and is always opaque (MB12)") {
    const std::optional<IconColor> opaque = materialSwatchColor(documentWithBaseColor(Vec4{0.5F, 0.18F, 1.0F, 1.0F}));
    REQUIRE(opaque.has_value());
    for (const float alpha : {0.0F, 0.3F, 1.0F}) {
        CAPTURE(alpha);
        const std::optional<IconColor> swatch =
            materialSwatchColor(documentWithBaseColor(Vec4{0.5F, 0.18F, 1.0F, alpha}));
        REQUIRE(swatch.has_value());
        CHECK(byteOf(swatch->a) == 255);
        CHECK(byteOf(swatch->r) == byteOf(opaque->r));
        CHECK(byteOf(swatch->g) == byteOf(opaque->g));
        CHECK(byteOf(swatch->b) == byteOf(opaque->b));
    }
}

TEST_CASE("material card: a negative channel encodes as zero and never wraps (MB13)") {
    const std::optional<IconColor> swatch = materialSwatchColor(documentWithBaseColor(Vec4{-1.0F, 0.5F, -0.0F, 1.0F}));
    REQUIRE(swatch.has_value());
    CHECK(byteOf(swatch->r) == 0);
    CHECK(byteOf(swatch->g) == 206);
    CHECK(byteOf(swatch->b) == 0);
}

TEST_CASE("material card: a non-finite base colour is REFUSED, never clamped (MB14)") {
    SUBCASE("NaN in x") {
        CHECK_FALSE(materialSwatchColor(documentWithBaseColor(Vec4{QUIET_NAN, 0.5F, 0.5F, 1.0F})).has_value());
    }
    SUBCASE("NaN in y") {
        CHECK_FALSE(materialSwatchColor(documentWithBaseColor(Vec4{0.5F, QUIET_NAN, 0.5F, 1.0F})).has_value());
    }
    SUBCASE("NaN in z") {
        CHECK_FALSE(materialSwatchColor(documentWithBaseColor(Vec4{0.5F, 0.5F, QUIET_NAN, 1.0F})).has_value());
    }
    SUBCASE("+inf") {
        CHECK_FALSE(materialSwatchColor(documentWithBaseColor(Vec4{POSITIVE_INFINITY, 0.5F, 0.5F, 1.0F})).has_value());
    }
    SUBCASE("-inf") {
        CHECK_FALSE(materialSwatchColor(documentWithBaseColor(Vec4{0.5F, -POSITIVE_INFINITY, 0.5F, 1.0F})).has_value());
    }
    // The card carries the refusal as hasSwatch == false, so the host keeps the kind colour -- and the name
    // is independent of the colour.
    MaterialDocument document = documentWithBaseColor(Vec4{0.5F, QUIET_NAN, 0.5F, 1.0F});
    document.name = "Broken";
    const MaterialCard card = materialCardFor(document);
    CHECK_FALSE(card.hasSwatch);
    CHECK_FALSE(materialCardTint(&card).has_value());
    CHECK(card.displayName == "Broken");
}

TEST_CASE("material card: a NaN in alpha alone does not refuse -- alpha is never read (MB15)") {
    const MaterialDocument nanAlpha = documentWithBaseColor(Vec4{0.5F, 0.5F, 0.5F, QUIET_NAN});
    const std::optional<IconColor> swatch = materialSwatchColor(nanAlpha);
    REQUIRE(swatch.has_value());
    CHECK(byteOf(swatch->r) == 206);
    CHECK(byteOf(swatch->a) == 255);
    // And a finite card's tint IS its swatch.
    const MaterialCard card = materialCardFor(documentWithBaseColor(Vec4{0.5F, 0.5F, 0.5F, 1.0F}));
    REQUIRE(card.hasSwatch);
    const std::optional<IconColor> tint = materialCardTint(&card);
    REQUIRE(tint.has_value());
    CHECK(byteOf(tint->g) == 206);
    CHECK_FALSE(materialCardTint(nullptr).has_value());
}

// ---- rule 4, at draw time (MB16-MB18) ------------------------------------------------------------------

TEST_CASE("material card: the stem comparison is exact up to ASCII case (MB16)") {
    CHECK(materialNameMatchesStem("brass", "brass.aeromat"));
    CHECK(materialNameMatchesStem("BRASS", "brass.aeromat"));
    CHECK(materialNameMatchesStem("Brass", "BRASS.AEROMAT"));
    CHECK(materialNameMatchesStem("a.b", "a.b.aeromat"));  // the stem ends at the LAST dot
    CHECK(materialNameMatchesStem("noext", "noext"));      // no extension: the name is its own stem
    CHECK(materialNameMatchesStem("x.", "x."));            // a trailing dot is not an extension
    // THE ANTI-VACUITY HALF: near-misses are NOT equal.
    CHECK_FALSE(materialNameMatchesStem("brass2", "brass.aeromat"));
    CHECK_FALSE(materialNameMatchesStem("bras", "brass.aeromat"));
    CHECK_FALSE(materialNameMatchesStem("brass ", "brass.aeromat"));  // raw: sanitising is not its job
    CHECK_FALSE(materialNameMatchesStem("brass.aeromat", "brass.aeromat"));
    CHECK_FALSE(materialNameMatchesStem("a", "a.b.aeromat"));
    CHECK_FALSE(materialNameMatchesStem("", "brass.aeromat"));
    // Case folding is ASCII-only: a non-ASCII byte compares exactly ("Ete" with an acute E vs a lowercase e).
    CHECK_FALSE(materialNameMatchesStem("\xC3\x89t\xC3\xA9", "\xC3\xA9t\xC3\xA9.aeromat"));
    CHECK(materialNameMatchesStem("\xC3\xA9t\xC3\xA9", "\xC3\xA9t\xC3\xA9.aeromat"));
}

TEST_CASE("material card: the subtitle is decided against the CURRENT file name (MB17)") {
    MaterialDocument document;
    document.name = "Brass";
    const MaterialCard card = materialCardFor(document);
    // ONE card, two leaves: a rename keeps the ThumbnailKey, so one cached card must answer both correctly.
    CHECK(materialCardSubtitle(&card, "brass.aeromat").empty());    // de-duplicated against its own stem
    CHECK(materialCardSubtitle(&card, "gold.aeromat") == "Brass");  // ...and shown once the file is renamed
    // Nothing is ever DERIVED from a file name: no card, or a card with no name, is no subtitle.
    CHECK(materialCardSubtitle(nullptr, "brass.aeromat").empty());
    const MaterialCard unnamed = materialCardFor(MaterialDocument{});
    CHECK(unnamed.displayName.empty());
    CHECK(materialCardSubtitle(&unnamed, "brass.aeromat").empty());
    CHECK(materialCardSubtitle(&unnamed, "gold.aeromat").empty());
}

TEST_CASE("material card: a TRUNCATED name is never suppressed (MB18)") {
    MaterialCard card;
    card.displayName = "abc";
    card.displayNameTruncated = true;
    // A leaf whose stem equals the stored text byte for byte: a truncated name is still shown...
    CHECK(materialCardSubtitle(&card, "abc.aeromat") == "abc");
    card.displayNameTruncated = false;
    CHECK(materialCardSubtitle(&card, "abc.aeromat").empty());  // ...and suppressed once it is not truncated
    // And the flag is what materialCardFor sets for a capped name.
    MaterialDocument document;
    document.name = std::string(200, 'z');
    CHECK(materialCardFor(document).displayNameTruncated);
}

// ---- the row sentence and the label contrast (MB19-MB21) -----------------------------------------------

TEST_CASE("material card: the row text is the file name VERBATIM without a subtitle (MB19)") {
    CHECK(materialCardRowText("brass.aeromat", "") == "brass.aeromat");
    CHECK(materialCardRowText("readme##v2.aeromat", "") == "readme##v2.aeromat");
    std::string expected = "brass.aeromat";
    expected += MATERIAL_CARD_SEPARATOR;  // read from the constant, never restated here
    expected += "Studio Brass";
    CHECK(materialCardRowText("brass.aeromat", "Studio Brass") == expected);
}

TEST_CASE("material card: the dark-label threshold falls between grey 127 and grey 128, exactly (MB20)") {
    CHECK_FALSE(materialSwatchWantsDarkLabel(IconColor{.r = 127U, .g = 127U, .b = 127U, .a = 255U}));
    CHECK(materialSwatchWantsDarkLabel(IconColor{.r = 128U, .g = 128U, .b = 128U, .a = 255U}));
    CHECK_FALSE(materialSwatchWantsDarkLabel(IconColor{.r = 0U, .g = 0U, .b = 0U, .a = 255U}));
    CHECK(materialSwatchWantsDarkLabel(IconColor{.r = 232U, .g = 232U, .b = 232U, .a = 255U}));  // white's swatch
    // Alpha takes no part in it.
    CHECK(materialSwatchWantsDarkLabel(IconColor{.r = 128U, .g = 128U, .b = 128U, .a = 0U}));
}

TEST_CASE("material card: the label rule weighs green heaviest and blue lightest (MB21)") {
    CHECK(materialSwatchWantsDarkLabel(IconColor{.r = 0U, .g = 232U, .b = 0U, .a = 255U}));
    CHECK_FALSE(materialSwatchWantsDarkLabel(IconColor{.r = 232U, .g = 0U, .b = 0U, .a = 255U}));
    CHECK_FALSE(materialSwatchWantsDarkLabel(IconColor{.r = 0U, .g = 0U, .b = 232U, .a = 255U}));
    // Green alone crosses between 178 and 179: 7152 * 179 = 1 280 208 >= 1 280 000 > 7152 * 178 = 1 273 056.
    CHECK(materialSwatchWantsDarkLabel(IconColor{.r = 0U, .g = 179U, .b = 0U, .a = 255U}));
    CHECK_FALSE(materialSwatchWantsDarkLabel(IconColor{.r = 0U, .g = 178U, .b = 0U, .a = 255U}));
}

// ---- the fixed rig (MB22-MB23, MB25-MB26) -------------------------------------------------------------------

TEST_CASE("material card: the studio rig is a real rig, and a fixed one (MB22)") {
    const engine::render::TonemapParams tonemap = materialThumbnailTonemap();
    CHECK((engine::render::sanitizeTonemapParams(tonemap) == tonemap));  // ALREADY sanitized
    const engine::editor::MaterialPreviewLighting lighting = materialThumbnailLighting();
    CHECK(lighting.hasSun);
    CHECK(std::isfinite(lighting.sun.intensity));
    CHECK(lighting.sun.intensity > 0.0F);
    CHECK(engine::length(lighting.sun.direction) == doctest::Approx(1.0F).epsilon(1e-5));
    CHECK(lighting.sun.direction.y < 0.0F);  // the key light shines DOWN onto the sphere
    CHECK((lighting.environment.backgroundMode == engine::render::BackgroundMode::Sky));
    for (const Vec3 colour :
         {lighting.environment.skyColor, lighting.environment.horizonColor, lighting.environment.groundColor}) {
        CHECK(std::isfinite(colour.x));
        CHECK(std::isfinite(colour.y));
        CHECK(std::isfinite(colour.z));
        CHECK(colour.x >= 0.0F);
        CHECK(colour.y >= 0.0F);
        CHECK(colour.z >= 0.0F);
    }
    CHECK_FALSE((lighting.environment.skyColor == lighting.environment.groundColor));  // a silhouette exists
    // THE SAME VALUE TWICE -- the property the key-completeness argument rests on, asserted, not assumed.
    const engine::editor::MaterialPreviewLighting again = materialThumbnailLighting();
    CHECK((again.environment == lighting.environment));
    CHECK((again.sun.direction == lighting.sun.direction));
    CHECK((again.sun.color == lighting.sun.color));
    CHECK(again.sun.intensity == lighting.sun.intensity);
    CHECK(again.hasSun == lighting.hasSun);
}

TEST_CASE("material card: the fixed orbit frames the sphere from outside and above (MB23)") {
    // The code-review round: THE THUMBNAIL'S OWN RIG, which produce() now uses -- no longer the preview's.
    const engine::render::CameraView camera = thumbnailCamera();
    for (const engine::Mat4& matrix : {camera.view, camera.proj}) {
        for (const Vec4& column : matrix.columns) {
            CHECK(std::isfinite(column.x));
            CHECK(std::isfinite(column.y));
            CHECK(std::isfinite(column.z));
            CHECK(std::isfinite(column.w));
        }
    }
    const float distance = engine::length(camera.eyePosition);
    CHECK(distance > THUMBNAIL_SPHERE_RADIUS);  // outside the sphere it frames (radius 0.5, not 1)
    CHECK(camera.eyePosition.y > 0.0F);         // above its equator
    // The near plane never cuts the sphere, and the far plane never drops its back: the whole sphere lies
    // inside the depth range, with the near plane well short of its nearest point.
    const engine::editor::MaterialPreviewRig& rig = engine::editor::MATERIAL_THUMBNAIL_RIG;
    CHECK(rig.nearPlane < distance - THUMBNAIL_SPHERE_RADIUS);
    CHECK(rig.farPlane > distance + THUMBNAIL_SPHERE_RADIUS);
}

TEST_CASE("material card: the thumbnail's sphere fills about 80% of it, under a visible horizon (MB25)") {
    // The owner's framing decision, pinned through the camera the thumbnail ACTUALLY builds: the silhouette's
    // tangent point is projected through its own view and projection, never recomputed from the rig's fields.
    const engine::render::CameraView camera = thumbnailCamera();
    const Vec3 eye = camera.eyePosition;
    const float distance = engine::length(eye);
    const Vec3 forward = engine::normalize(Vec3{} - eye);  // at the sphere's centre, the origin
    const Vec3 right = engine::normalize(engine::cross(forward, Vec3{0.0F, 1.0F, 0.0F}));
    const Vec3 up = engine::cross(right, forward);
    const auto project = [&camera](Vec3 point) {
        const Vec4 clip = camera.proj * (camera.view * Vec4{point.x, point.y, point.z, 1.0F});
        REQUIRE(clip.w > 0.0F);  // in front of the eye
        return Vec3{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
    };
    // THE SILHOUETTE: from the eye, the tangent ray leaves the view axis at asin(r / d) and touches the sphere
    // d * cos of that away. One tangent point to the RIGHT and one ABOVE; each must lie ON the sphere. NDC spans
    // 2 across the frame, so the silhouette's NDC radius IS the diameter's fraction of the frame.
    const float angularRadius = std::asin(THUMBNAIL_SPHERE_RADIUS / distance);
    const auto tangentPoint = [&](Vec3 across) {
        const Vec3 direction = (forward * std::cos(angularRadius)) + (across * std::sin(angularRadius));
        const Vec3 point = eye + (direction * (distance * std::cos(angularRadius)));
        CHECK(engine::length(point) == doctest::Approx(THUMBNAIL_SPHERE_RADIUS).epsilon(1e-4));
        return point;
    };
    const Vec3 atRight = project(tangentPoint(right));
    CAPTURE(atRight.x);
    CHECK(atRight.x >= 0.75F);
    CHECK(atRight.x <= 0.85F);
    CHECK(std::abs(atRight.y) < 1e-4F);  // centred vertically
    const Vec3 atTop = project(tangentPoint(up));
    CAPTURE(atTop.y);
    CHECK(atTop.y >= 0.75F);
    CHECK(atTop.y <= 0.85F);
    CHECK(std::abs(atTop.x) < 1e-4F);  // centred horizontally
    // THE HORIZON: a direction at ZERO elevation along the view's own azimuth projects INSIDE the frame and ABOVE
    // its centre -- so the sky band stays in the picture. Narrowing the field of view at the preview's 21.8-degree
    // pitch would put this point above the top edge, and the whole frame below the horizon.
    const Vec3 level = engine::normalize(Vec3{forward.x, 0.0F, forward.z});
    const Vec3 horizon = project(eye + (level * 10.0F));
    CAPTURE(horizon.y);
    CHECK(std::abs(horizon.x) < 1e-4F);  // straight ahead: the azimuth is the view's
    CHECK(horizon.y > 0.0F);             // above the centre...
    CHECK(horizon.y < 1.0F);             // ...and inside the frame
}

TEST_CASE("material card: the key light is D-A's recipe for the thumbnail's own eye (MB26)") {
    // "Over the viewer's left shoulder": toward the light is normalise(0.7 * toEye + 0.8 * up + 0.5 * left) for
    // the eye the thumbnail camera actually has, and the light travels the opposite way. Re-derived when the
    // framing changed; a rig retuned without re-deriving the light reddens here.
    const Vec3 eye = thumbnailCamera().eyePosition;
    const Vec3 toEye = engine::normalize(eye);
    const Vec3 left{-std::sin(engine::editor::MATERIAL_THUMBNAIL_ORBIT_ANGLE), 0.0F,
                    std::cos(engine::editor::MATERIAL_THUMBNAIL_ORBIT_ANGLE)};
    const Vec3 towardLight = engine::normalize((toEye * 0.7F) + Vec3{0.0F, 0.8F, 0.0F} + (left * 0.5F));
    const Vec3 travel = materialThumbnailLighting().sun.direction;
    // The source spells the direction to four decimals, so the tolerance is the literal's, not a float's.
    CHECK(travel.x == doctest::Approx(-towardLight.x).epsilon(1e-3));
    CHECK(travel.y == doctest::Approx(-towardLight.y).epsilon(1e-3));
    CHECK(travel.z == doctest::Approx(-towardLight.z).epsilon(1e-3));
    CHECK(engine::dot(travel * -1.0F, left) > 0.0F);   // from the LEFT
    CHECK(engine::dot(travel * -1.0F, toEye) > 0.0F);  // from the viewer's side, so the facing hemisphere is lit
}

// ---- totality (MB24) --------------------------------------------------------------------------------------

TEST_CASE("material card: hostile names are bounded, one line, and never a format (MB24)") {
    std::uint32_t state = 0x2545F491U;  // a fixed-seed LCG: no entropy source anywhere
    std::string noise;
    noise.reserve(4096U);
    for (std::size_t i = 0; i < 4096U; ++i) {
        state = (state * 1664525U) + 1013904223U;
        noise.push_back(static_cast<char>(state >> 24U));
    }
    const MaterialDisplayName name = sanitizeMaterialDisplayName(noise);
    CHECK(name.text.size() <= MAX_MATERIAL_DISPLAY_NAME_BYTES + MATERIAL_DISPLAY_NAME_ELLIPSIS.size());
    for (const char c : name.text) {
        CHECK(static_cast<unsigned char>(c) >= 0x20U);
        CHECK(static_cast<unsigned char>(c) != 0x7FU);
    }
    // D12's VALUE half: a name that looks like a format string is carried verbatim, byte for byte, through
    // every function a host composes with. The DRAW half -- that no host hands it to a format function --
    // is I242(e)'s source pin and validation row 6.
    for (const std::string_view format :
         {std::string_view("%s"), std::string_view("%n%n%n"), std::string_view("100%s"), std::string_view("a##b")}) {
        CAPTURE(format);
        CHECK(sanitizeMaterialDisplayName(format).text == format);
        MaterialDocument document;
        document.name = std::string(format);
        const MaterialCard card = materialCardFor(document);
        CHECK(materialCardSubtitle(&card, "other.aeromat") == format);
        std::string expected = "other.aeromat";
        expected += MATERIAL_CARD_SEPARATOR;
        expected += format;
        CHECK(materialCardRowText("other.aeromat", format) == expected);
    }
}

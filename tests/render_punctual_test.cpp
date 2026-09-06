// tests/render_punctual_test.cpp — task E.2.2: the punctual falloff's HLSL pin (tier 0) and the
// point / spot PIXELS (tier 1). Split at the ONE sanctioned `#if AERO_SHADER_TOOLS_ENABLED` this
// tree allows in a test file: LP1 reads shaders/scene.frag.hlsl as TEXT and needs no device at all,
// LP2 onwards load cooked shaders from build/<preset>/shaders and read texels back.
//
// EVERY HELPER BELOW IS A FILE-LOCAL COPY, never an include of another test's header (E.2.1's own
// decision for Half4): a test header shared between two tier-1 suites is a second place for a
// tolerance to drift.
//
// EVERY CLEAR COLOUR HERE IS {0.9, 0.1, 0.9, 1} AND IS UNEQUAL TO EVERY VALUE COMPARED AGAINST. A
// clear equal to an expected colour makes "the drawn texels are X" true whether or not a single
// fragment was shaded -- which is why every pixel arm also asserts != clear.
#include <aero/render/render.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ostream>  // a CHECK on a std::string_view needs this or MSVC ALONE fails (the 0.4.1 trap)
#include <string>
#include <string_view>

namespace {

// The shader's own source, reached through AERO_SHADERS_SRC_DIR -- defined UNCONDITIONALLY on
// aero_tests, which is what lets LP1 ride both reduced configurations exactly as TM29 and HE16 do.
constexpr std::string_view SCENE_FRAG_PATH = AERO_SHADERS_SRC_DIR "/scene.frag.hlsl";

// COMMENT-STRIPPED, so a token that appears only in a comment cannot satisfy a source-text arm.
[[nodiscard]] std::string strippedSourceAt(std::string_view absolutePath) {
    std::ifstream file{std::string{absolutePath}};
    std::string out;
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t comment = line.find("//");
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        out += line;
        out += '\n';
    }
    return out;
}

[[nodiscard]] std::size_t countOccurrences(const std::string& haystack, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

}  // namespace

TEST_CASE("render punctual: scene.frag.hlsl transcribes light_falloff.hpp, and nothing else (LP1)") {
    const std::string source = strippedSourceAt(SCENE_FRAG_PATH);
    REQUIRE(source.size() > 200);  // the file was actually read, before any needle means anything

    // PRESENT, exactly once each. "Exactly once" rather than "present": a second copy of a
    // transcribed expression is the same defect as a missing one -- a reader who edits the first and
    // not the second leaves the CPU oracle and the GPU disagreeing.
    CHECK(countOccurrences(source, "#define MAX_SPOT_LIGHTS 8") == 1);
    CHECK(countOccurrences(source, "float punctualDistanceAttenuation(float distSq, float range)") == 1);
    CHECK(countOccurrences(source, "saturate(1.0 - f * f)") == 1);
    CHECK(countOccurrences(source, "window * window / max(distSq, 1e-4)") == 1);
    CHECK(countOccurrences(source, "float spotConeAttenuation(float cosAngle, float angleScale, float angleOffset)") ==
          1);
    CHECK(countOccurrences(source, "saturate(cosAngle * angleScale + angleOffset)") == 1);
    // The SQUARE, on its own line: the needle above is present whether or not `t` is squared, so
    // without this one a seed that dropped the square would reach no tier-0 witness at all.
    // Lowercase `t * t` occurs nowhere else in the file -- the tangent is `T`, and dot(T, T) is
    // uppercase.
    CHECK(countOccurrences(source, "return t * t;") == 1);
    CHECK(countOccurrences(source, "for (uint s = 0; s < uSpotCount; ++s)") == 1);
    CHECK(countOccurrences(source, "dot(uSpots[s].direction, -L)") == 1);
    // task E.2.1's HE16 line, re-pinned here because THIS is the file a reader opens when the light
    // section is next edited. Harmless duplication, deliberately kept.
    CHECK(countOccurrences(source, "uAmbientMid + uAmbientHalfDelta * N.y") == 1);

    // ABSENT. The pre-E.2.2 squared linear ramp is gone in BOTH of its spellings.
    CHECK(countOccurrences(source, "atten *= atten") == 0);
    CHECK(countOccurrences(source, "1.0 - dist / max(") == 0);
    // NO TRIGONOMETRY: the cone travels as a resolved {scale, offset} pair, so the shader adds and
    // multiplies and never interprets an angle.
    CHECK(countOccurrences(source, "cos(") == 0);
    CHECK(countOccurrences(source, "acos(") == 0);

    // EXACTLY ONE `lerp(` -- the PRE-EXISTING Fresnel base `f0 = lerp(float3(0.04, ...), ...)`, which
    // is 3.4.1's and legitimate. An "absent" arm here would be RED on the healthy tree; `== 1` is
    // what makes a NEW lerp in either helper redden it, which matters because E.2.1 measured that a
    // lerp form is NOT bit-identical to a scaled delta when the endpoints coincide.
    CHECK(countOccurrences(source, "lerp(") == 1);
}

// ================================================================================================
// Tier 1 — a real Device, no window, cooked shaders from build/<preset>/shaders.
// ================================================================================================

#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/core/log.hpp>
    #include <aero/core/vfs.hpp>
    #include <aero/platform/platform.hpp>
    #include <aero/rhi/rhi.hpp>
    #include <aero/scene/scene.hpp>
    #include <aero/scene_render/scene_renderer.hpp>

    #include "rhi_test_support.hpp"

    #include <algorithm>
    #include <array>
    #include <memory>
    #include <optional>
    #include <span>
    #include <utility>
    #include <vector>

namespace {

using engine::Mat4;
using engine::Vec3;
namespace rd = engine::render;

// One texel as four RAW half bit patterns, never decoded, so every comparison is exact. At namespace
// scope and not inside a case because [class.friend]/6 forbids DEFINING a friend inside a local
// class, and without the operator<< below every assertion would print {?} == {?} on the one run that
// matters.
struct Half4 {
    std::uint16_t r = 0;
    std::uint16_t g = 0;
    std::uint16_t b = 0;
    std::uint16_t a = 0;
    [[nodiscard]] bool operator==(const Half4&) const = default;
};

// HEX, because these are bit patterns rather than numbers. An operator<<, NOT a toString: a toString
// is the ADL trap that hard-errors inside doctest.h.
std::ostream& operator<<(std::ostream& out, const Half4& value) {
    out << "half4(0x" << std::hex << value.r << ", 0x" << value.g << ", 0x" << value.b << ", 0x" << value.a << ")"
        << std::dec;
    return out;
}

// ROUND-TO-NEAREST-EVEN float -> half, render_sky_test.cpp's own. It is what lets an assertion say
// "the drawn texel IS the CPU's encoding" rather than "close to".
[[nodiscard]] std::uint16_t encodeHalf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    const std::uint32_t rawExponent = (bits >> 23U) & 0xFFU;
    std::uint32_t mantissa = bits & 0x007FFFFFU;

    if (rawExponent == 0xFFU) {  // inf / NaN -- a quiet NaN keeps its quiet bit
        return static_cast<std::uint16_t>(sign | 0x7C00U | (mantissa != 0U ? 0x0200U : 0U));
    }
    const std::int32_t exponent = static_cast<std::int32_t>(rawExponent) - 127 + 15;
    if (exponent >= 0x1F) {  // overflows the half range
        return static_cast<std::uint16_t>(sign | 0x7C00U);
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<std::uint16_t>(sign);  // underflows to a signed zero
        }
        mantissa |= 0x00800000U;  // the float's implicit leading bit becomes explicit
        const auto shift = static_cast<std::uint32_t>(14 - exponent);
        const std::uint32_t truncated = mantissa >> shift;
        const std::uint32_t roundBit = 1U << (shift - 1U);
        const bool roundUp =
            (mantissa & roundBit) != 0U && (((mantissa & (roundBit - 1U)) != 0U) || ((truncated & 1U) != 0U));
        return static_cast<std::uint16_t>(sign | (truncated + (roundUp ? 1U : 0U)));
    }
    const std::uint32_t truncated = (static_cast<std::uint32_t>(exponent) << 10U) | (mantissa >> 13U);
    constexpr std::uint32_t ROUND_BIT = 1U << 12U;
    const bool roundUp =
        (mantissa & ROUND_BIT) != 0U && (((mantissa & (ROUND_BIT - 1U)) != 0U) || ((truncated & 1U) != 0U));
    return static_cast<std::uint16_t>(sign | (truncated + (roundUp ? 1U : 0U)));
}

// The INVERSE of encodeHalf, for the ratio arms: two texels of the same face under the same camera
// are compared as a QUOTIENT so the BRDF, the normal, the view vector and the half encoding cancel
// and only the falloff is under test. Subnormals decode too; every value here is a non-negative
// finite colour.
[[nodiscard]] float decodeHalf(std::uint16_t bits) {
    const std::uint32_t sign = (bits >> 15U) & 1U;
    const std::uint32_t exponent = (bits >> 10U) & 0x1FU;
    const std::uint32_t mantissa = bits & 0x3FFU;
    float value = 0.0F;
    if (exponent == 0U) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400U), static_cast<int>(exponent) - 25);
    }
    return sign != 0U ? -value : value;
}

[[nodiscard]] Half4 halfAt(const std::vector<std::byte>& pixels, std::uint32_t textureWidth, std::uint32_t row,
                           std::uint32_t column) {
    const std::size_t base = (((static_cast<std::size_t>(row) * textureWidth) + column) * 8U);
    const auto at = [&pixels](std::size_t index) {
        return static_cast<std::uint16_t>(static_cast<std::uint8_t>(pixels[index]) |
                                          (static_cast<std::uint8_t>(pixels[index + 1U]) << 8U));
    };
    return Half4{at(base), at(base + 2U), at(base + 4U), at(base + 6U)};
}

// The half4 an RGBA16Float clear of `color` produces: the backend writes the clear floats verbatim.
[[nodiscard]] Half4 encodeHalf4(Vec3 rgb, float alpha) {
    return Half4{encodeHalf(rgb.x), encodeHalf(rgb.y), encodeHalf(rgb.z), encodeHalf(alpha)};
}

// UNLIT IS EXACTLY Half4{0, 0, 0, 0x3C00}, and the reasoning belongs here rather than in a comment
// on one case: the ambient term is `0 * diffuse * occlusion` = +0, the directional term is
// `(diffuse + specular) * (colour * 0) * NdotL` = +0 (every factor finite and non-negative, NdotL
// saturated), and a punctual term outside the cone or beyond the range is `x * saturate(negative)` =
// `x * +0` = +0. A sum of +0s is +0, and no -0.0 can arise because no factor is negative. Every case
// below RENDERS the unlit frame anyway and REQUIREs it equals this -- measuring rather than assuming.
constexpr Half4 UNLIT{0, 0, 0, 0x3C00};

// The clear, unequal to UNLIT and to every lit value read below.
constexpr float CLEAR_R = 0.9F;
constexpr float CLEAR_G = 0.1F;
constexpr float CLEAR_B = 0.9F;

// A camera on one axis looking at the origin. `up` must not be parallel to the view direction --
// lookAt's cross product is degenerate there.
[[nodiscard]] rd::CameraView axisCamera(Vec3 eye, Vec3 up) {
    return rd::CameraView{.view = engine::lookAt(eye, Vec3::zero(), up),
                          .proj = engine::perspective(engine::radians(60.0F), 1.0F, 0.1F, 100.0F),
                          .eyePosition = eye};
}

// A white cube (scale 2, so its faces sit at +-1) on the DEFAULT material (an invalid
// MeshInstance::material resolves to DEFAULT_MATERIAL_PARAMS, metallic 0 -- never MaterialParams{},
// whose glTF metallic of 1 renders near-black with nothing to reflect), a Flat ambient of EXACTLY
// ZERO so the texel is the punctual term alone, a REAL directional direction at intensity zero (a
// zero direction is a NaN L inside the BRDF), culling and shadows off. The caller assigns `points`
// or `spots`.
[[nodiscard]] rd::RenderView punctualView(const rd::CameraView& camera, std::span<const rd::MeshInstance> cube) {
    rd::RenderView view;
    view.camera = camera;
    view.instances = cube;
    view.environment = {.ambientMode = rd::AmbientMode::Flat, .ambientColor = Vec3{}, .ambientIntensity = 1.0F};
    view.directional = {.direction = Vec3{0.0F, 0.0F, -1.0F}, .color = Vec3::one(), .intensity = 0.0F};
    view.hasCamera = true;
    view.cullingEnabled = false;
    view.shadowsEnabled = false;
    return view;
}

[[nodiscard]] rd::MeshInstance cubeFor(const rd::CameraView& camera) {
    rd::MeshInstance cube;
    cube.primitive = rd::PrimitiveId::Cube;
    cube.model = engine::scaling(Vec3{2.0F, 2.0F, 2.0F});
    cube.normalMatrix = Mat4::identity();
    cube.mvp = camera.proj * camera.view * cube.model;
    cube.color = Vec3::one();
    return cube;
}

// The cos of the angle between the spot's -Z axis and the light-to-surface direction, at a PIXEL
// CENTRE on the cube's +Z face, for a light on the eye's ray. Computed from the same expression the
// case comments quote, rather than from carried decimals.
[[nodiscard]] float cosAngleAtPixel(float size, float faceDistance, std::uint32_t column, std::uint32_t row) {
    const float halfExtent = faceDistance * std::tan(engine::radians(30.0F));
    const float x = (((static_cast<float>(column) + 0.5F) / size * 2.0F) - 1.0F) * halfExtent;
    const float y = (1.0F - ((static_cast<float>(row) + 0.5F) / size * 2.0F)) * halfExtent;
    return faceDistance / std::sqrt((x * x) + (y * y) + (faceDistance * faceDistance));
}

}  // namespace

    // The tier-1 preamble, written out per case exactly as render_sky_test.cpp does it:
    // AERO_SKIP_OR_FAIL returns from the enclosing function, so it cannot live in a helper.
    #define AERO_PUNCTUAL_TIER1_PREAMBLE()                                                         \
        const engine::platform::Context ctx{{.headless = false}};                                  \
        if (!ctx.valid()) {                                                                        \
            AERO_SKIP_OR_FAIL("no real video driver available");                                   \
        }                                                                                          \
        auto device = engine::rhi::Device::create();                                               \
        if (!device.has_value()) {                                                                 \
            AERO_SKIP_OR_FAIL("no GPU device available");                                          \
        }                                                                                          \
        if (!device->supportsTextureFormat(                                                        \
                engine::rhi::TextureFormat::RGBA16Float,                                           \
                engine::rhi::TextureUsage::Sampler | engine::rhi::TextureUsage::ColorTarget)) {    \
            AERO_SKIP_OR_FAIL("device does not support RGBA16Float as a sampleable color target"); \
        }                                                                                          \
        engine::VirtualFileSystem vfs;                                                             \
        vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR))

TEST_CASE("render punctual: the window discriminates the new falloff from the old ramp (LP2)") {
    AERO_PUNCTUAL_TIER1_PREAMBLE();
    constexpr std::uint32_t SIZE = 64;
    auto target = rd::RenderTarget::create(
        *device, {SIZE, SIZE}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());
    auto forward = rd::ForwardRenderer::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(forward.has_value());

    const engine::rhi::Color clear{CLEAR_R, CLEAR_G, CLEAR_B, 1.0F};
    const rd::CameraView camera = axisCamera(Vec3{0.0F, 0.0F, 5.0F}, Vec3{0.0F, 1.0F, 0.0F});
    const std::array<rd::MeshInstance, 1> instances{cubeFor(camera)};

    const auto renderOnce = [&](std::span<const rd::PointLightData> points) {
        rd::RenderView view = punctualView(camera, instances);
        view.points = points;
        std::optional<rd::Frame> frame = target->beginFrame(clear);
        REQUIRE(frame.has_value());
        forward->draw(*frame, view);
        REQUIRE(target->endFrame(std::move(*frame)));
        std::vector<std::byte> pixels(static_cast<std::size_t>(SIZE) * SIZE * 8U, std::byte{0xAB});
        REQUIRE(device->readbackTexture(target->colorTexture(), 0, pixels));
        return halfAt(pixels, SIZE, SIZE / 2U, SIZE / 2U);
    };

    // MEASURED, not assumed: with no punctual light at all the centre texel is exactly UNLIT.
    REQUIRE(renderOnce({}) == UNLIT);

    // The light sits ON the eye's ray at (0, 0, 5), so L == V at the face centre and the whole BRDF
    // is identical between the two frames -- only `range` differs, which is what makes the QUOTIENT
    // a statement about the window alone. intensity 16 keeps both texels well inside the half's
    // normal range, far from subnormals and from 65504.
    const std::array<rd::PointLightData, 1> near5{
        rd::PointLightData{.position = Vec3{0.0F, 0.0F, 5.0F}, .intensity = 16.0F, .range = 5.0F}};
    const std::array<rd::PointLightData, 1> near8{
        rd::PointLightData{.position = Vec3{0.0F, 0.0F, 5.0F}, .intensity = 16.0F, .range = 8.0F}};
    const Half4 r5 = renderOnce(near5);
    const Half4 r8 = renderOnce(near8);
    CHECK_FALSE(r5 == UNLIT);
    CHECK_FALSE(r8 == UNLIT);
    CHECK_FALSE(r5 == encodeHalf4(Vec3{CLEAR_R, CLEAR_G, CLEAR_B}, 1.0F));
    CHECK_FALSE(r8 == encodeHalf4(Vec3{CLEAR_R, CLEAR_G, CLEAR_B}, 1.0F));

    // THE ORACLE IS THE CPU'S, not a second readback (E.1.2's GR8 rule). d^2 is 16 at the face
    // centre to within 0.017%, which the 3% tolerance swallows.
    const float expected = rd::punctualDistanceAttenuation(16.0F, 5.0F) / rd::punctualDistanceAttenuation(16.0F, 8.0F);
    CHECK(std::fabs(expected - 0.3966F) < 1e-3F);  // the oracle itself, pinned by number
    const std::array<std::pair<std::uint16_t, std::uint16_t>, 3> channels{std::pair{r5.r, r8.r}, std::pair{r5.g, r8.g},
                                                                          std::pair{r5.b, r8.b}};
    for (const auto& [lit5, lit8] : channels) {
        const float ratio = decodeHalf(lit5) / decodeHalf(lit8);
        CAPTURE(ratio);
        CHECK(std::fabs(ratio - expected) < 0.03F * expected);
        // AND NOT the two laws it replaces: the pre-E.2.2 squared linear ramp reads 0.16 here, a
        // window that was not squared reads 0.630, and a window dropped entirely reads 1.0.
        CHECK(std::fabs(ratio - 0.16F) > 0.03F * expected);
        CHECK(std::fabs(ratio - 0.630F) > 0.03F * expected);
        CHECK(std::fabs(ratio - 1.0F) > 0.03F * expected);
    }
}

TEST_CASE("render punctual: the falloff is INVERSE SQUARE, not the old near-linear ramp (LP3)") {
    AERO_PUNCTUAL_TIER1_PREAMBLE();
    constexpr std::uint32_t SIZE = 64;
    auto target = rd::RenderTarget::create(
        *device, {SIZE, SIZE}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());
    auto forward = rd::ForwardRenderer::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(forward.has_value());

    const engine::rhi::Color clear{CLEAR_R, CLEAR_G, CLEAR_B, 1.0F};
    // The eye at z = 6 puts the +Z face (z = 1) FIVE units away, so the two light positions below
    // are 2 and 4 units from it -- a clean factor of two.
    const rd::CameraView camera = axisCamera(Vec3{0.0F, 0.0F, 6.0F}, Vec3{0.0F, 1.0F, 0.0F});
    const std::array<rd::MeshInstance, 1> instances{cubeFor(camera)};

    const auto renderOnce = [&](std::span<const rd::PointLightData> points) {
        rd::RenderView view = punctualView(camera, instances);
        view.points = points;
        std::optional<rd::Frame> frame = target->beginFrame(clear);
        REQUIRE(frame.has_value());
        forward->draw(*frame, view);
        REQUIRE(target->endFrame(std::move(*frame)));
        std::vector<std::byte> pixels(static_cast<std::size_t>(SIZE) * SIZE * 8U, std::byte{0xAB});
        REQUIRE(device->readbackTexture(target->colorTexture(), 0, pixels));
        return halfAt(pixels, SIZE, SIZE / 2U, SIZE / 2U);
    };
    REQUIRE(renderOnce({}) == UNLIT);

    // BOTH lights sit on the eye's ray, so at the face centre L, V, N and H are all +Z in both
    // frames and every BRDF factor cancels in the quotient. range 100 keeps the window at 1 (its
    // deviation is 2*d^4/1e8 <= 5e-6), so what is left is exactly the inverse-square law.
    const std::array<rd::PointLightData, 1> at2{
        rd::PointLightData{.position = Vec3{0.0F, 0.0F, 3.0F}, .intensity = 16.0F, .range = 100.0F}};
    const std::array<rd::PointLightData, 1> at4{
        rd::PointLightData{.position = Vec3{0.0F, 0.0F, 5.0F}, .intensity = 16.0F, .range = 100.0F}};
    const Half4 near = renderOnce(at2);
    const Half4 far = renderOnce(at4);
    CHECK_FALSE(near == UNLIT);
    CHECK_FALSE(far == UNLIT);
    CHECK_FALSE(near == encodeHalf4(Vec3{CLEAR_R, CLEAR_G, CLEAR_B}, 1.0F));

    const float expected =
        rd::punctualDistanceAttenuation(4.0F, 100.0F) / rd::punctualDistanceAttenuation(16.0F, 100.0F);
    CHECK(std::fabs(expected - 4.0F) < 1e-3F);  // the oracle itself: halving the distance quadruples it
    const std::array<std::pair<std::uint16_t, std::uint16_t>, 3> channels{
        std::pair{near.r, far.r}, std::pair{near.g, far.g}, std::pair{near.b, far.b}};
    for (const auto& [lit2, lit4] : channels) {
        const float ratio = decodeHalf(lit2) / decodeHalf(lit4);
        CAPTURE(ratio);
        CHECK(std::fabs(ratio - expected) < 0.03F * expected);
        // AND NOT the old ramp, which at range 100 is very nearly flat: it reads 1.042 here.
        CHECK(std::fabs(ratio - 1.042F) > 0.03F * expected);
    }
}

TEST_CASE("render punctual: beyond range is EXACTLY nothing, for a point and for a spot (LP4)") {
    AERO_PUNCTUAL_TIER1_PREAMBLE();
    constexpr std::uint32_t SIZE = 64;
    auto target = rd::RenderTarget::create(
        *device, {SIZE, SIZE}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());
    auto forward = rd::ForwardRenderer::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(forward.has_value());

    const engine::rhi::Color clear{CLEAR_R, CLEAR_G, CLEAR_B, 1.0F};
    const rd::CameraView camera = axisCamera(Vec3{0.0F, 0.0F, 5.0F}, Vec3{0.0F, 1.0F, 0.0F});
    const std::array<rd::MeshInstance, 1> instances{cubeFor(camera)};

    const auto renderOnce = [&](std::span<const rd::PointLightData> points, std::span<const rd::SpotLightData> spots) {
        rd::RenderView view = punctualView(camera, instances);
        view.points = points;
        view.spots = spots;
        std::optional<rd::Frame> frame = target->beginFrame(clear);
        REQUIRE(frame.has_value());
        forward->draw(*frame, view);
        REQUIRE(target->endFrame(std::move(*frame)));
        std::vector<std::byte> pixels(static_cast<std::size_t>(SIZE) * SIZE * 8U, std::byte{0xAB});
        REQUIRE(device->readbackTexture(target->colorTexture(), 0, pixels));
        return halfAt(pixels, SIZE, SIZE / 2U, SIZE / 2U);
    };
    REQUIRE(renderOnce({}, {}) == UNLIT);

    // The light is 4 units from the face. range 3 puts the face OUTSIDE the window entirely, and the
    // window's exact zero is what makes this a BIT identity rather than "very dark".
    SUBCASE("a point light beyond its range writes the unlit texel bit for bit") {
        const std::array<rd::PointLightData, 1> tooFar{
            rd::PointLightData{.position = Vec3{0.0F, 0.0F, 5.0F}, .intensity = 16.0F, .range = 3.0F}};
        CHECK(renderOnce(tooFar, {}) == UNLIT);
        // ANTI-VACUITY: the same light with range 8 DOES light the face, so the arm above is about
        // the range and not about a light that never reached the shader.
        const std::array<rd::PointLightData, 1> inRange{
            rd::PointLightData{.position = Vec3{0.0F, 0.0F, 5.0F}, .intensity = 16.0F, .range = 8.0F}};
        CHECK_FALSE(renderOnce(inRange, {}) == UNLIT);
    }

    SUBCASE("a spot light beyond its range writes the unlit texel bit for bit") {
        const std::array<rd::SpotLightData, 1> tooFar{rd::SpotLightData{.position = Vec3{0.0F, 0.0F, 5.0F},
                                                                        .direction = Vec3{0.0F, 0.0F, -1.0F},
                                                                        .intensity = 16.0F,
                                                                        .range = 3.0F}};
        CHECK(renderOnce({}, tooFar) == UNLIT);
        const std::array<rd::SpotLightData, 1> inRange{rd::SpotLightData{.position = Vec3{0.0F, 0.0F, 5.0F},
                                                                         .direction = Vec3{0.0F, 0.0F, -1.0F},
                                                                         .intensity = 16.0F,
                                                                         .range = 8.0F}};
        CHECK_FALSE(renderOnce({}, inRange) == UNLIT);
    }
}

TEST_CASE("render punctual: a spot on its axis IS a point light, and its corners are unlit (LP5)") {
    AERO_PUNCTUAL_TIER1_PREAMBLE();
    constexpr std::uint32_t SIZE = 64;
    auto target = rd::RenderTarget::create(
        *device, {SIZE, SIZE}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());
    auto forward = rd::ForwardRenderer::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(forward.has_value());

    const engine::rhi::Color clear{CLEAR_R, CLEAR_G, CLEAR_B, 1.0F};
    const rd::CameraView camera = axisCamera(Vec3{0.0F, 0.0F, 5.0F}, Vec3{0.0F, 1.0F, 0.0F});
    const std::array<rd::MeshInstance, 1> instances{cubeFor(camera)};

    const auto renderOnce = [&](std::span<const rd::PointLightData> points, std::span<const rd::SpotLightData> spots) {
        rd::RenderView view = punctualView(camera, instances);
        view.points = points;
        view.spots = spots;
        std::optional<rd::Frame> frame = target->beginFrame(clear);
        REQUIRE(frame.has_value());
        forward->draw(*frame, view);
        REQUIRE(target->endFrame(std::move(*frame)));
        std::vector<std::byte> pixels(static_cast<std::size_t>(SIZE) * SIZE * 8U, std::byte{0xAB});
        REQUIRE(device->readbackTexture(target->colorTexture(), 0, pixels));
        return pixels;
    };
    const std::vector<std::byte> unlitFrame = renderOnce({}, {});
    REQUIRE(halfAt(unlitFrame, SIZE, 32, 32) == UNLIT);

    // THE GEOMETRY, computed rather than asserted: the +Z face sits at z = 1, FOUR units from the
    // eye; the view's half-extent there is 4 * tan(30 deg) = 2.3094, so the face (+-1) spans NDC
    // +-0.4330 -- pixel columns and rows 18.14 .. 45.86. Pixel (32, 32) has centre (+0.036, -0.036)
    // in world units, 0.73 deg off the axis. The four corner samples are (20, 20), (20, 43),
    // (43, 20) and (43, 43): world (+-0.830, +-0.830), 16.35 deg off the axis. (18, 18) is NOT used
    // -- the face's edge at 18.14 makes it a fill-rule boundary pixel (E.1.2's DG18 rule).
    constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 4> CORNERS{
        std::pair<std::uint32_t, std::uint32_t>{20U, 20U}, std::pair<std::uint32_t, std::uint32_t>{20U, 43U},
        std::pair<std::uint32_t, std::uint32_t>{43U, 20U}, std::pair<std::uint32_t, std::uint32_t>{43U, 43U}};

    const std::array<rd::PointLightData, 1> point{
        rd::PointLightData{.position = Vec3{0.0F, 0.0F, 5.0F}, .intensity = 16.0F, .range = 100.0F}};
    const std::vector<std::byte> pointFrame = renderOnce(point, {});
    const Half4 pointCentre = halfAt(pointFrame, SIZE, 32, 32);
    REQUIRE_FALSE(pointCentre == UNLIT);
    REQUIRE_FALSE(pointCentre == encodeHalf4(Vec3{CLEAR_R, CLEAR_G, CLEAR_B}, 1.0F));

    SUBCASE("a 5/10-degree cone: the axis is the point light BIT FOR BIT, the corners are unlit") {
        const std::array<rd::SpotLightData, 1> narrow{rd::SpotLightData{.position = Vec3{0.0F, 0.0F, 5.0F},
                                                                        .direction = Vec3{0.0F, 0.0F, -1.0F},
                                                                        .intensity = 16.0F,
                                                                        .range = 100.0F,
                                                                        .innerConeRadians = engine::radians(5.0F),
                                                                        .outerConeRadians = engine::radians(10.0F)}};
        const std::vector<std::byte> spotFrame = renderOnce({}, narrow);
        // cos(0.73 deg) = 0.99992 > cos(5 deg), so t saturates to 1 and `atten * 1` IS `atten`: the
        // cone term is the multiplicative identity here, on every backend.
        CHECK(halfAt(spotFrame, SIZE, 32, 32) == pointCentre);
        // 16.35 deg is outside the 10 deg outer cone, so t = 0.9595 * 87.82 - 86.49 = -2.2 -> +0 and
        // the whole punctual term vanishes. BIT identity, because saturate's zero is exact.
        for (const auto& [column, row] : CORNERS) {
            CAPTURE(column);
            CAPTURE(row);
            CHECK(halfAt(spotFrame, SIZE, row, column) == UNLIT);
        }
    }

    SUBCASE("widening the outer cone to 40 degrees lights those same four corners") {
        // THE ANTI-VACUITY OF THE ARM ABOVE: the corners are dark because of the CONE, not because
        // nothing is drawn there. cos(16.35 deg) with a 5/40 cone gives 0.707.
        const std::array<rd::SpotLightData, 1> wide{rd::SpotLightData{.position = Vec3{0.0F, 0.0F, 5.0F},
                                                                      .direction = Vec3{0.0F, 0.0F, -1.0F},
                                                                      .intensity = 16.0F,
                                                                      .range = 100.0F,
                                                                      .innerConeRadians = engine::radians(5.0F),
                                                                      .outerConeRadians = engine::radians(40.0F)}};
        const std::vector<std::byte> spotFrame = renderOnce({}, wide);
        CHECK(halfAt(spotFrame, SIZE, 32, 32) == pointCentre);  // the axis is STILL the point light
        for (const auto& [column, row] : CORNERS) {
            CAPTURE(column);
            CAPTURE(row);
            CHECK_FALSE(halfAt(spotFrame, SIZE, row, column) == UNLIT);
        }
    }
}

TEST_CASE("render punctual: the cone's blend matches the CPU oracle across the falloff band (LP6)") {
    AERO_PUNCTUAL_TIER1_PREAMBLE();
    constexpr std::uint32_t SIZE = 128;
    auto target = rd::RenderTarget::create(
        *device, {SIZE, SIZE}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());
    auto forward = rd::ForwardRenderer::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(forward.has_value());

    const engine::rhi::Color clear{CLEAR_R, CLEAR_G, CLEAR_B, 1.0F};
    const rd::CameraView camera = axisCamera(Vec3{0.0F, 0.0F, 5.0F}, Vec3{0.0F, 1.0F, 0.0F});
    const std::array<rd::MeshInstance, 1> instances{cubeFor(camera)};

    const auto renderOnce = [&](std::span<const rd::PointLightData> points, std::span<const rd::SpotLightData> spots) {
        rd::RenderView view = punctualView(camera, instances);
        view.points = points;
        view.spots = spots;
        std::optional<rd::Frame> frame = target->beginFrame(clear);
        REQUIRE(frame.has_value());
        forward->draw(*frame, view);
        REQUIRE(target->endFrame(std::move(*frame)));
        std::vector<std::byte> pixels(static_cast<std::size_t>(SIZE) * SIZE * 8U, std::byte{0xAB});
        REQUIRE(device->readbackTexture(target->colorTexture(), 0, pixels));
        return pixels;
    };

    const std::array<rd::PointLightData, 1> point{
        rd::PointLightData{.position = Vec3{0.0F, 0.0F, 5.0F}, .intensity = 16.0F, .range = 100.0F}};
    const std::array<rd::SpotLightData, 1> spot{rd::SpotLightData{.position = Vec3{0.0F, 0.0F, 5.0F},
                                                                  .direction = Vec3{0.0F, 0.0F, -1.0F},
                                                                  .intensity = 16.0F,
                                                                  .range = 100.0F,
                                                                  .innerConeRadians = engine::radians(5.0F),
                                                                  .outerConeRadians = engine::radians(20.0F)}};
    const std::vector<std::byte> pointFrame = renderOnce(point, {});
    const std::vector<std::byte> spotFrame = renderOnce({}, spot);

    // At 128x128 the face spans 36.29 .. 91.71, so the centre row is 64 and columns 64..90 walk from
    // 0.37 deg to 13.45 deg off the axis. COLUMN 91 IS EXCLUDED: its centre sits 0.21 px inside the
    // face's edge, a fill-rule boundary (E.1.2's DG18 rule).
    constexpr std::uint32_t ROW = 64;
    constexpr std::uint32_t FIRST_COLUMN = 64;
    constexpr std::uint32_t LAST_COLUMN = 90;
    const rd::SpotCone cone = rd::resolveSpotCone(engine::radians(5.0F), engine::radians(20.0F));

    SUBCASE("three sampled columns match spotConeAttenuation, and an unsquared t does not") {
        // THE POINT FRAME CANCELS distance, NdotL and the whole BRDF: the quotient at one pixel is
        // the cone term alone. The oracle is the CPU's -- never a second readback (E.1.2's GR8).
        constexpr std::array<std::uint32_t, 3> COLUMNS{76U, 82U, 88U};
        constexpr std::array<float, 3> UNSQUARED{0.956F, 0.826F, 0.650F};
        for (std::size_t i = 0; i < COLUMNS.size(); ++i) {
            const std::uint32_t column = COLUMNS[i];
            CAPTURE(column);
            const float cosAngle = cosAngleAtPixel(static_cast<float>(SIZE), 4.0F, column, ROW);
            const float expected = rd::spotConeAttenuation(cosAngle, cone);
            CAPTURE(expected);
            const Half4 spotTexel = halfAt(spotFrame, SIZE, ROW, column);
            const Half4 pointTexel = halfAt(pointFrame, SIZE, ROW, column);
            REQUIRE_FALSE(pointTexel == UNLIT);
            const float ratio = decodeHalf(spotTexel.r) / decodeHalf(pointTexel.r);
            CAPTURE(ratio);
            CHECK(std::fabs(ratio - expected) < 0.03F);
            // ...and NOT the unsquared blend, which reads 0.956 / 0.826 / 0.650 at these three
            // columns -- 4.6 %, 21 % and 54 % away from the squared values.
            CHECK(std::fabs(ratio - UNSQUARED[i]) > 0.03F);
        }
    }

    SUBCASE("the spot's own texels are non-negative and non-increasing along the row") {
        std::uint16_t previous = 0xFFFFU;
        for (std::uint32_t column = FIRST_COLUMN; column <= LAST_COLUMN; ++column) {
            CAPTURE(column);
            const Half4 texel = halfAt(spotFrame, SIZE, ROW, column);
            CHECK((texel.r & 0x8000U) == 0U);  // the sign bit is clear
            CHECK(texel.r <= previous);        // same-sign finite halves order as their bit patterns
            previous = texel.r;
        }
    }

    SUBCASE("at least eight columns' RATIOS lie strictly inside (0.02, 0.98) -- a hard edge has none") {
        // RATIOS, never raw values: along this row the point texel ITSELF decreases (the distance
        // grows, NdotL falls, the GGX lobe narrows), so a "distinct raw values" arm would be green
        // even for a HARD-EDGED cone. The oracle predicts 17 such columns (74..90) for this pair; a
        // hard edge predicts 0.
        std::size_t insideBand = 0;
        for (std::uint32_t column = FIRST_COLUMN; column <= LAST_COLUMN; ++column) {
            const Half4 pointTexel = halfAt(pointFrame, SIZE, ROW, column);
            if (pointTexel == UNLIT) {
                continue;
            }
            const float ratio = decodeHalf(halfAt(spotFrame, SIZE, ROW, column).r) / decodeHalf(pointTexel.r);
            if (ratio > 0.02F && ratio < 0.98F) {
                ++insideBand;
            }
        }
        CAPTURE(insideBand);
        CHECK(insideBand >= 8);
    }
}

#endif  // AERO_SHADER_TOOLS_ENABLED

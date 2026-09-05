// tests/render_sky_test.cpp — task E.2.1: the sky's two packers (tier 0) and the ENVIRONMENT's
// PIXELS (tier 1) -- both halves of it, the background the sky pass draws AND the hemispheric
// ambient the forward pass shades with (SB17, which records no sky pass at all and is here because
// this is the task's only tier-1 file). Split at the ONE sanctioned `#if AERO_SHADER_TOOLS_ENABLED`
// this tree allows in a test file: SB1-SB3 need no device at all, SB4 onwards load cooked shaders
// from build/<preset>/shaders and read texels back.
//
// THIS FILE REACHES THE src-PRIVATE PACKER by a relative include -- the tonemap_pack.hpp precedent --
// which is exactly why HE1's umbrella claim lives in render_environment_test.cpp instead: the
// relative include below pulls <aero/render/environment.hpp> in transitively, and a claim that the
// UMBRELLA carries the vocabulary would be vacuous here.
//
// EVERY CLEAR COLOUR IN THIS FILE IS CHOSEN UNEQUAL TO EVERY SKY COLOUR IT IS COMPARED AGAINST. A
// clear equal to the solid colour makes "the drawn texels are the solid colour" true whether or not
// a single fragment was shaded -- the anti-vacuity trap SB6 states in its own body.
#include <aero/render/render.hpp>

#include "../engine/render/src/sky_pack.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ostream>
#include <string_view>

namespace {

using engine::Mat4;
using engine::Vec3;
using engine::Vec4;
namespace rd = engine::render;

// The float's bit pattern, for the arms that say BIT-exact rather than equal (HE's own helper, the
// same reason: `==` alone cannot tell +0.0 from -0.0, and "the pads are written zero" is a claim
// about bits).
[[nodiscard]] std::uint32_t bitsOf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

}  // namespace

TEST_CASE("render sky: SkyPassConfig's defaults are Invalid formats and the two res:// paths (SB1)") {
    const rd::SkyPassConfig config;
    // BOTH formats default Invalid: colorFormat is REQUIRED and its default must therefore be the
    // value create() refuses, and depthFormat's Invalid means "a depth-free frame", which is legal.
    CHECK((config.colorFormat == engine::rhi::TextureFormat::Invalid));
    CHECK((config.depthFormat == engine::rhi::TextureFormat::Invalid));
    // BY VALUE, so a typo is a RED TEST rather than a runtime load failure inside create() on the one
    // machine that runs the editor. <ostream> is included for exactly this comparison (the MSVC trap).
    CHECK(config.vertexShaderPath == std::string_view{"res://sky.vert"});
    CHECK(config.fragmentShaderPath == std::string_view{"res://sky.frag"});
}

TEST_CASE("render sky: packSkyParams fills three distinct rows and writes every pad zero (SB2)") {
    // The static_asserts in sky_pack.hpp already make a layout drift a COMPILE error; these restate
    // them as assertions so a failure names the number rather than the line.
    CHECK(sizeof(rd::detail::GpuSkyParams) == 48U);
    CHECK(offsetof(rd::detail::GpuSkyParams, skyDelta) == 16U);
    CHECK(offsetof(rd::detail::GpuSkyParams, groundDelta) == 32U);

    // MUTUALLY DISTINCT on every channel, so a packer that wrote one row into two slots, or permuted
    // the three, cannot pass. No channel is -0.0: the delta rule's one stated edge is that
    // -0.0 + 0.0 * w is +0.0, and no case here may seed it.
    const rd::SkyGradient gradient{.horizon = Vec3{0.11F, 0.12F, 0.13F},
                                   .skyDelta = Vec3{0.21F, 0.22F, 0.23F},
                                   .groundDelta = Vec3{0.31F, 0.32F, 0.33F}};
    const rd::detail::GpuSkyParams packed = rd::detail::packSkyParams(gradient);

    CHECK(packed.horizon == gradient.horizon);
    CHECK(packed.skyDelta == gradient.skyDelta);
    CHECK(packed.groundDelta == gradient.groundDelta);
    // BIT-exact zero on all three pads. SDL copies the block verbatim into its uniform ring, so an
    // indeterminate tail float is a value that differs between two runs on the same machine.
    CHECK(bitsOf(packed.pad0) == 0U);
    CHECK(bitsOf(packed.pad1) == 0U);
    CHECK(bitsOf(packed.pad2) == 0U);
}

TEST_CASE("render sky: packSkyCamera inverts proj * view, and refuses a non-finite one (SB3)") {
    const Mat4 view = engine::lookAt(Vec3{3.0F, 4.0F, 5.0F}, Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F});

    SUBCASE("perspective") {
        const Mat4 proj = engine::perspective(engine::radians(55.0F), 16.0F / 9.0F, 0.25F, 400.0F);
        const rd::CameraView camera{.view = view, .proj = proj, .eyePosition = Vec3{3.0F, 4.0F, 5.0F}};
        const std::optional<rd::detail::GpuSkyCamera> packed = rd::detail::packSkyCamera(camera);
        REQUIRE(packed.has_value());

        // (a) ELEMENT FOR ELEMENT against inverse(proj * view). This is what pins the ORDER -- a
        //     packer inverting view * proj, or forgetting the inverse entirely, is finite and wrong.
        const Mat4 expected = engine::inverse(proj * view);
        for (std::size_t column = 0; column < 4; ++column) {
            CAPTURE(column);
            CHECK(packed->invViewProj.columns[column].x == expected.columns[column].x);
            CHECK(packed->invViewProj.columns[column].y == expected.columns[column].y);
            CHECK(packed->invViewProj.columns[column].z == expected.columns[column].z);
            CHECK(packed->invViewProj.columns[column].w == expected.columns[column].w);
        }
        // (b) AND THE PROPERTY, read off neither formula: the product with the forward matrix is the
        //     identity. Without this arm (a) compares two values from one source and asserts only
        //     that the same expression was written twice.
        const Mat4 roundTrip = packed->invViewProj * (proj * view);
        const Mat4 identity = Mat4::identity();  // a NAMED object: binding a reference into
                                                 // Mat4::identity().columns[c] dangles at the
                                                 // semicolon, which ASan reports as a stack
                                                 // use-after-scope (measured, not theorised)
        for (std::size_t column = 0; column < 4; ++column) {
            CAPTURE(column);
            const Vec4& got = roundTrip.columns[column];
            const Vec4& want = identity.columns[column];
            CHECK(std::abs(got.x - want.x) < 1e-4F);
            CHECK(std::abs(got.y - want.y) < 1e-4F);
            CHECK(std::abs(got.z - want.z) < 1e-4F);
            CHECK(std::abs(got.w - want.w) < 1e-4F);
        }
        // ANTI-VACUITY: the matrix under test is not the identity, so (a) and (b) are not both true
        // of a packer that returns whatever it was handed.
        CHECK_FALSE(packed->invViewProj == Mat4::identity());
    }

    SUBCASE("ortho") {
        const Mat4 proj = engine::ortho(-8.0F, 8.0F, -4.5F, 4.5F, 0.1F, 250.0F);
        const rd::CameraView camera{.view = view, .proj = proj, .eyePosition = Vec3{3.0F, 4.0F, 5.0F}};
        const std::optional<rd::detail::GpuSkyCamera> packed = rd::detail::packSkyCamera(camera);
        REQUIRE(packed.has_value());
        const Mat4 expected = engine::inverse(proj * view);
        for (std::size_t column = 0; column < 4; ++column) {
            CAPTURE(column);
            CHECK(packed->invViewProj.columns[column].x == expected.columns[column].x);
            CHECK(packed->invViewProj.columns[column].y == expected.columns[column].y);
            CHECK(packed->invViewProj.columns[column].z == expected.columns[column].z);
            CHECK(packed->invViewProj.columns[column].w == expected.columns[column].w);
        }
        CHECK_FALSE(packed->invViewProj == Mat4::identity());
    }

    SUBCASE("a singular view refuses") {
        // A ZERO-SCALE view: the upper-left 3x3 collapses, the determinant is 0, and engine::inverse
        // yields inf/NaN (scene/camera.hpp:57's own note). ALL SIXTEEN elements are checked by the
        // packer, culling.cpp's Frustum::valid idiom -- a subset check passes a matrix whose only
        // non-finite element is in the translation column.
        const rd::CameraView camera{.view = engine::scaling(Vec3::zero()),
                                    .proj = engine::perspective(engine::radians(60.0F), 1.0F, 0.1F, 100.0F),
                                    .eyePosition = Vec3::zero()};
        CHECK_FALSE(rd::detail::packSkyCamera(camera).has_value());
    }

    SUBCASE("a NaN in the projection refuses") {
        Mat4 proj = engine::perspective(engine::radians(60.0F), 1.0F, 0.1F, 100.0F);
        proj.columns[2].z = std::numeric_limits<float>::quiet_NaN();
        const rd::CameraView camera{.view = Mat4::identity(), .proj = proj, .eyePosition = Vec3::zero()};
        CHECK_FALSE(rd::detail::packSkyCamera(camera).has_value());
    }

    SUBCASE("an identity camera is accepted -- the refusals above are not universal") {
        // ANTI-VACUITY for the two refusals: a packSkyCamera that returned nullopt unconditionally
        // would satisfy both of them.
        const rd::CameraView camera{.view = Mat4::identity(), .proj = Mat4::identity(), .eyePosition = Vec3::zero()};
        CHECK(rd::detail::packSkyCamera(camera).has_value());
    }
}

// ================================================================================================
// TIER 1 -- the pass itself. Gated exactly as render_tonemap_test.cpp's PostProcess blocks are: the
// pass loads two shaders from build/<preset>/shaders, which only exists when the shader toolchain is
// built. Every case above runs in every configuration.
// ================================================================================================

#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/core/log.hpp>
    #include <aero/core/vfs.hpp>
    #include <aero/platform/platform.hpp>
    #include <aero/rhi/rhi.hpp>
    // SB16 alone reaches the bridge: it is the ONE case here that drives the pass through the object
    // that owns it in production rather than through a hand-built RenderView. aero_tests already
    // links aero::scene and aero::scene_render (scene_render_test.cpp rides this same binary), so no
    // link edge moves for it.
    #include <aero/scene/scene.hpp>
    #include <aero/scene_render/scene_renderer.hpp>

    #include "rhi_test_support.hpp"

    #include <algorithm>
    #include <array>
    #include <memory>
    #include <optional>
    #include <span>
    #include <string>
    #include <utility>
    #include <vector>

namespace {

// Rgba's RGBA16Float twin, render_debug_draw_test.cpp:818-840's own -- one texel as four RAW half bit
// patterns, never decoded, so every comparison is exact. It lives at namespace scope and not inside a
// case because [class.friend]/6 forbids DEFINING a friend inside a local class, and without the
// operator<< below every assertion would print {?} == {?} on the one run that matters.
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

// ROUND-TO-NEAREST-EVEN float -> half. It is what lets an assertion say "the drawn texel IS the CPU's
// encoding of the oracle" rather than "close to". A ULP DISTANCE over the raw bit patterns is then
// exactly "N half-ulps" for same-sign finite halves, which is how SB8's tolerance is stated.
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

// "N half-ulps", for SAME-SIGN FINITE halves: two such values differ by N ulps iff their bit patterns,
// read as integers, differ by N. Every value it is called with here is a non-negative colour.
[[nodiscard]] std::uint32_t halfUlpDistance(std::uint16_t a, std::uint16_t b) {
    return a >= b ? static_cast<std::uint32_t>(a - b) : static_cast<std::uint32_t>(b - a);
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

// The CPU mirror of sky.vert.hlsl's unprojection, evaluated at a PIXEL CENTRE. The row -> ndc.y
// mapping is fixed by the READBACK layout (row 0 is the texture's first row, i.e. the top), NOT by
// the shader -- which is what makes SB8's top-vs-bottom arm a genuine upright witness rather than a
// mirror of whatever the vertex stage did.
[[nodiscard]] Vec3 rayAtPixel(const Mat4& invViewProj, std::uint32_t width, std::uint32_t height, std::uint32_t row,
                              std::uint32_t column) {
    const float ndcX = ((static_cast<float>(column) + 0.5F) / static_cast<float>(width) * 2.0F) - 1.0F;
    const float ndcY = 1.0F - ((static_cast<float>(row) + 0.5F) / static_cast<float>(height) * 2.0F);
    const Vec4 nearP = invViewProj * Vec4{ndcX, ndcY, 0.0F, 1.0F};
    const Vec4 farP = invViewProj * Vec4{ndcX, ndcY, 1.0F, 1.0F};
    return Vec3{(farP.x / farP.w) - (nearP.x / nearP.w), (farP.y / farP.w) - (nearP.y / nearP.w),
                (farP.z / farP.w) - (nearP.z / nearP.w)};
}

// A DirectoryBackend that admits exactly one shader's artifacts -- render_tonemap_test.cpp's
// SingleShaderBackend under a distinct name. SB5 needs the arm where ONE of the two loads succeeds:
// with an EMPTY VFS both handles are invalid and destroying an invalid handle is a documented no-op,
// so an empty mount CANNOT see a create() that forgot to release the shader it DID make.
class SingleShaderPrefixBackend final : public engine::FileSystemBackend {
public:
    SingleShaderPrefixBackend(std::string_view rootDirectory, std::string_view allowedPrefix)
        : inner(rootDirectory), prefix(allowedPrefix) {}

    [[nodiscard]] bool exists(std::string_view relPath) const override {
        return admits(relPath) && inner.exists(relPath);
    }
    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view relPath) const override {
        return admits(relPath) ? inner.fileSize(relPath) : std::nullopt;
    }
    [[nodiscard]] std::optional<engine::ByteBuffer> readFile(std::string_view relPath) const override {
        return admits(relPath) ? inner.readFile(relPath) : std::nullopt;
    }

private:
    [[nodiscard]] bool admits(std::string_view relPath) const { return relPath.starts_with(prefix); }

    engine::DirectoryBackend inner;
    std::string prefix;
};

// The RAII log-capture guard, render_tonemap_test.cpp's PP4 own: its destructor detaches, which a
// code-review round required there after the bare form was found to be a latent use-after-free
// (log.hpp:92-101 -- detaching does NOT guarantee the captured state may be destroyed).
struct LogCallbackGuard {
    ~LogCallbackGuard() { engine::setLogCallback({}); }
    LogCallbackGuard() = default;
    LogCallbackGuard(const LogCallbackGuard&) = delete;
    LogCallbackGuard& operator=(const LogCallbackGuard&) = delete;
    LogCallbackGuard(LogCallbackGuard&&) = delete;
    LogCallbackGuard& operator=(LogCallbackGuard&&) = delete;
};

// One frame: clear, record the sky, end, read back. It exists so no case can forget the endFrame,
// and it takes the clear colour explicitly because "the clear is unequal to the sky" is a property
// every case in this file has to choose deliberately.
[[nodiscard]] std::vector<std::byte> skyOnce(engine::rhi::Device& device, engine::render::RenderTarget& target,
                                             engine::render::SkyPass& pass, const engine::render::RenderView& view,
                                             const engine::rhi::Color& clear, std::size_t bytesPerTexel) {
    std::optional<engine::render::Frame> frame = target.beginFrame(clear);
    REQUIRE(frame.has_value());
    pass.draw(*frame, view);
    REQUIRE(target.endFrame(std::move(*frame)));
    const engine::rhi::Extent2D texture = target.textureExtent();
    std::vector<std::byte> pixels(static_cast<std::size_t>(texture.width) * texture.height * bytesPerTexel,
                                  std::byte{0xAB});
    REQUIRE(device.readbackTexture(target.colorTexture(), 0, pixels));
    return pixels;
}

// A view carrying nothing but a camera and an environment: no instances, no lights, culling and
// shadows off. SkyPass reads exactly two fields of it, and this makes that visible.
[[nodiscard]] engine::render::RenderView skyView(const engine::render::CameraView& camera,
                                                 const engine::render::EnvironmentData& environment) {
    engine::render::RenderView view;
    view.camera = camera;
    view.environment = environment;
    view.hasCamera = true;
    view.cullingEnabled = false;
    view.shadowsEnabled = false;
    return view;
}

[[nodiscard]] engine::render::CameraView frontCamera() {
    const Vec3 eye{0.0F, 0.0F, 5.0F};
    return engine::render::CameraView{.view = engine::lookAt(eye, Vec3::zero(), Vec3{0.0F, 1.0F, 0.0F}),
                                      .proj = engine::perspective(engine::radians(60.0F), 1.0F, 0.1F, 100.0F),
                                      .eyePosition = eye};
}

// SB17's camera: on one axis, looking at the origin. `up` must not be parallel to the view direction
// -- lookAt's cross product is degenerate there and yields a non-finite matrix (scene/camera.hpp:57's
// note), which is why the two vertical cameras below are handed a Z up rather than the usual Y.
[[nodiscard]] engine::render::CameraView axisCamera(Vec3 eye, Vec3 up) {
    return engine::render::CameraView{.view = engine::lookAt(eye, Vec3::zero(), up),
                                      .proj = engine::perspective(engine::radians(60.0F), 1.0F, 0.1F, 100.0F),
                                      .eyePosition = eye};
}

// SB17: the centre BLOCK of the drawn face, never a single texel. A 9x9 block at the image centre,
// where the face the camera faces covers about 87% of the image at that camera's distance, so every
// texel read is interior and no fill-rule ambiguity at the silhouette can reach it (E.1.2's DG18
// rule, applied to a face rather than to a line).
//
// THE TOLERANCE IS A PARAMETER BECAUSE THE TWO AMBIENT MODES DIFFER IN KIND. A Flat resolution
// admits EXACTLY ZERO -- `mid + 0 * N.y` is `mid` whatever the interpolated normal did, on every
// backend -- while a hemispheric one rides N.y, which arrives through perspective-correct
// interpolation's reciprocal and can therefore move the last bit from texel to texel.
[[nodiscard]] Half4 centreBlockTexel(const std::vector<std::byte>& pixels, std::uint32_t size,
                                     std::uint32_t toleranceHalfUlps) {
    constexpr std::uint32_t RADIUS = 4;
    const std::uint32_t centre = size / 2U;
    const Half4 first = halfAt(pixels, size, centre - RADIUS, centre - RADIUS);
    std::size_t disagreeing = 0;
    Half4 firstDisagreement{};
    for (std::uint32_t row = centre - RADIUS; row <= centre + RADIUS; ++row) {
        for (std::uint32_t column = centre - RADIUS; column <= centre + RADIUS; ++column) {
            const Half4 texel = halfAt(pixels, size, row, column);
            const bool agrees = halfUlpDistance(texel.r, first.r) <= toleranceHalfUlps &&
                                halfUlpDistance(texel.g, first.g) <= toleranceHalfUlps &&
                                halfUlpDistance(texel.b, first.b) <= toleranceHalfUlps && texel.a == first.a;
            if (!agrees) {
                if (disagreeing == 0) {
                    firstDisagreement = texel;
                }
                ++disagreeing;
            }
        }
    }
    INFO("block first ", first, " first disagreement ", firstDisagreement);
    CHECK(disagreeing == 0);
    return first;
}

}  // namespace

    // The tier-1 preamble, written out per case exactly as render_tonemap_test.cpp does it:
    // AERO_SKIP_OR_FAIL returns from the enclosing function, so it cannot live in a helper.
    #define AERO_SKY_TIER1_PREAMBLE()                                                              \
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

TEST_CASE("render sky: a created pass draws nothing yet and has latched nothing (SB4)") {
    AERO_SKY_TIER1_PREAMBLE();
    auto target = engine::render::RenderTarget::create(
        *device, {64, 64}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());

    auto pass = engine::render::SkyPass::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(pass.has_value());
    CHECK(pass->drawCount() == 0U);
    CHECK_FALSE(pass->hasWarnedDegenerateCamera());
}

TEST_CASE("render sky: create refuses a bad format or a missing shader, and leaks nothing (SB5)") {
    AERO_SKY_TIER1_PREAMBLE();
    auto target = engine::render::RenderTarget::create(
        *device, {64, 64}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());

    SUBCASE("colorFormat Invalid") { CHECK_FALSE(engine::render::SkyPass::create(*device, vfs, {}).has_value()); }
    SUBCASE("colorFormat is a DEPTH format") {
        CHECK_FALSE(engine::render::SkyPass::create(*device, vfs, {.colorFormat = target->depthFormat()}).has_value());
    }
    SUBCASE("neither shader resolves") {
        const engine::VirtualFileSystem emptyVfs;  // nothing mounted at all
        CHECK_FALSE(
            engine::render::SkyPass::create(*device, emptyVfs, {.colorFormat = target->colorFormat()}).has_value());
    }
    SUBCASE("ONLY the vertex shader resolves -- the arm that can see a leak") {
        // With both handles invalid the release is a no-op either way, so the empty-VFS arm above
        // CANNOT witness a create() that forgot to release the shader it DID make. Here `vs` is a real
        // GPU object and `fs` is not. AND THE LEAK IS CAPTURED FROM THE LOG, because nothing else in
        // this tree can see it: ~Device RELEASES a leaked shader (so ASan sees no process leak) and
        // merely WARNs about it.
        std::vector<std::string> warnings;
        const LogCallbackGuard detachOnExit;
        engine::setLogCallback([&warnings](const engine::LogRecord& record) {
            if (record.level >= engine::LogLevel::Warn) {
                warnings.emplace_back(record.message);
            }
        });

        {
            engine::VirtualFileSystem partialVfs;
            partialVfs.mount(std::make_unique<SingleShaderPrefixBackend>(AERO_SHADERS_DIR, "sky.vert"));
            CHECK(partialVfs.exists("res://sky.vert.json"));
            CHECK_FALSE(partialVfs.exists("res://sky.frag.json"));
            CHECK_FALSE(engine::render::SkyPass::create(*device, partialVfs, {.colorFormat = target->colorFormat()})
                            .has_value());
        }
        // The target holds a Device* and must die FIRST; then ~Device is what would report a leak.
        target.reset();
        device.reset();
        engine::setLogCallback({});  // detach BEFORE reading `warnings`; the guard above is the backstop

        const bool leaked = std::any_of(warnings.begin(), warnings.end(), [](const std::string& line) {
            return line.find("leaked shader") != std::string::npos;
        });
        CHECK_FALSE(leaked);
        // ANTI-VACUITY: the capture is live at all. ~Device is silent on a clean teardown, so this
        // asserts the CALLBACK ran rather than that a particular line appeared -- the refused create()
        // above logs its own ERROR through the same sink.
        CHECK_FALSE(warnings.empty());
    }
}

TEST_CASE("render sky: a Solid sky fills the DRAWN rect of a margined target and nothing else (SB6)") {
    AERO_SKY_TIER1_PREAMBLE();
    // THE ENCODER IS CHECKED FIRST. Every pixel assertion in this file compares against
    // encodeHalf(...), so a broken encoder would move every expectation together and read as a shader
    // defect. These five are exact by construction.
    CHECK(encodeHalf(0.0F) == 0x0000U);
    CHECK(encodeHalf(1.0F) == 0x3C00U);
    CHECK(encodeHalf(0.5F) == 0x3800U);
    CHECK(encodeHalf(2.0F) == 0x4000U);
    CHECK(encodeHalf(-1.0F) == 0xBC00U);

    // 48x40 DRAWN inside a 64x64 ALLOCATION. THE MARGIN IS THE POINT (OG7's lesson): with
    // drawExtent == textureExtent a missing viewport is invisible, and every convenient target is
    // quantum = 1. SkyPass sets NO viewport deliberately -- it inherits the frame's, which
    // RenderTarget::beginFrame narrowed to the drawn rect.
    constexpr std::uint32_t DRAW_W = 48;
    constexpr std::uint32_t DRAW_H = 40;
    auto target = engine::render::RenderTarget::create(
        *device, {DRAW_W, DRAW_H},
        {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 64});
    REQUIRE(target.has_value());
    // ASSERTED, not assumed.
    REQUIRE(target->drawExtent().width == DRAW_W);
    REQUIRE(target->drawExtent().height == DRAW_H);
    REQUIRE(target->textureExtent().width == 64U);
    REQUIRE(target->textureExtent().height == 64U);

    auto pass = engine::render::SkyPass::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(pass.has_value());

    // EVERY CHANNEL EXACTLY REPRESENTABLE IN HALF (negative powers of two), so the comparison is on
    // bits with no rounding argument, and DIFFERENT FROM THE CLEAR ON EVERY CHANNEL. THE TRAP THIS
    // AVOIDS: setting the clear equal to the solid colour makes this case blind -- it would pass on a
    // pass that recorded no draw at all.
    const Vec3 solid{0.125F, 0.375F, 0.875F};
    const engine::rhi::Color clear{0.25F, 0.75F, 0.5F, 1.0F};
    const engine::render::RenderView view =
        skyView(frontCamera(), {.backgroundMode = engine::render::BackgroundMode::Solid, .solidColor = solid});

    const std::vector<std::byte> pixels = skyOnce(*device, *target, *pass, view, clear, 8U);
    CHECK(pass->drawCount() == 1U);

    const Half4 expectedSky = encodeHalf4(solid, 1.0F);
    const Half4 expectedClear = encodeHalf4(Vec3{clear.r, clear.g, clear.b}, clear.a);
    REQUIRE_FALSE(expectedSky == expectedClear);

    std::size_t drawnWrong = 0;
    std::size_t marginWrong = 0;
    Half4 firstDrawnWrong{};
    Half4 firstMarginWrong{};
    for (std::uint32_t row = 0; row < 64U; ++row) {
        for (std::uint32_t column = 0; column < 64U; ++column) {
            const Half4 texel = halfAt(pixels, 64U, row, column);
            const bool drawn = row < DRAW_H && column < DRAW_W;
            if (drawn && !(texel == expectedSky)) {
                if (drawnWrong == 0) {
                    firstDrawnWrong = texel;
                }
                ++drawnWrong;
            }
            if (!drawn && !(texel == expectedClear)) {
                if (marginWrong == 0) {
                    firstMarginWrong = texel;
                }
                ++marginWrong;
            }
        }
    }
    INFO("first drawn texel that was not the sky: ", firstDrawnWrong, " expected ", expectedSky);
    CHECK(drawnWrong == 0U);
    INFO("first margin texel that was not the clear: ", firstMarginWrong, " expected ", expectedClear);
    CHECK(marginWrong == 0U);
}

TEST_CASE("render sky: a Solid sky is ONE value across the whole drawn rect (SB7)") {
    AERO_SKY_TIER1_PREAMBLE();
    auto target = engine::render::RenderTarget::create(
        *device, {64, 64}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());
    auto pass = engine::render::SkyPass::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(pass.has_value());

    // SB6 states the invariant POSITIVELY (each texel equals the CPU's encoding); this restates it as
    // a PROPERTY read off the picture alone, so a systematically-wrong-but-uniform encoder cannot
    // satisfy both. The two arms are deliberately independent.
    const Vec3 solid{0.0625F, 0.25F, 0.5F};
    const engine::rhi::Color clear{0.9F, 0.1F, 0.8F, 1.0F};
    const engine::render::RenderView view =
        skyView(frontCamera(), {.backgroundMode = engine::render::BackgroundMode::Solid, .solidColor = solid});
    const std::vector<std::byte> pixels = skyOnce(*device, *target, *pass, view, clear, 8U);

    const Half4 first = halfAt(pixels, 64U, 0U, 0U);
    std::size_t differing = 0;
    for (std::uint32_t row = 0; row < 64U; ++row) {
        for (std::uint32_t column = 0; column < 64U; ++column) {
            if (!(halfAt(pixels, 64U, row, column) == first)) {
                ++differing;
            }
        }
    }
    CHECK(differing == 0U);
    // ANTI-VACUITY: uniformity alone is also true of a frame that only cleared. The clear is chosen
    // unequal to the solid colour on every channel precisely so this arm can say which one it is.
    CHECK(first == encodeHalf4(solid, 1.0F));
    CHECK_FALSE(first == encodeHalf4(Vec3{clear.r, clear.g, clear.b}, clear.a));
}

TEST_CASE("render sky: the gradient matches skyRadiance at nine pixel centres, and is upright (SB8)") {
    AERO_SKY_TIER1_PREAMBLE();
    constexpr std::uint32_t SIZE = 64;
    auto target = engine::render::RenderTarget::create(
        *device, {SIZE, SIZE}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());
    auto pass = engine::render::SkyPass::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(pass.has_value());

    const engine::render::CameraView camera = frontCamera();
    const engine::render::EnvironmentData environment{};  // the default THREE-COLOUR sky
    const engine::render::RenderView view = skyView(camera, environment);
    // The clear is a colour the gradient never produces, so a frame that recorded nothing fails every
    // arm below rather than passing one of them by coincidence.
    const std::vector<std::byte> pixels = skyOnce(*device, *target, *pass, view, {1.0F, 0.0F, 1.0F, 1.0F}, 8U);
    CHECK(pass->drawCount() == 1U);

    const engine::render::SkyGradient gradient = engine::render::resolveSkyGradient(environment);
    const Mat4 invViewProj = engine::inverse(camera.proj * camera.view);
    // TWO half-ulps, and the number is MEASURED rather than chosen wide. Re-run with the constant at
    // 0 and every disagreement across SB8, SB9 and SB12 is exactly ONE half-ulp -- the GPU's pow and
    // normalize differ from the CPU's in the last float32 places, and nothing here differs by more.
    // Two is that worst case plus one ulp of headroom for another backend; do not widen it without
    // measuring again.
    constexpr std::uint32_t TOLERANCE_HALF_ULPS = 2;
    constexpr std::array<std::uint32_t, 3> PROBES{8U, 32U, 55U};

    const auto oracleAt = [&](std::uint32_t row, std::uint32_t column) {
        return engine::render::skyRadiance(gradient, rayAtPixel(invViewProj, SIZE, SIZE, row, column));
    };

    for (const std::uint32_t row : PROBES) {
        for (const std::uint32_t column : PROBES) {
            CAPTURE(row);
            CAPTURE(column);
            const Half4 got = halfAt(pixels, SIZE, row, column);
            const Half4 want = encodeHalf4(oracleAt(row, column), 1.0F);
            INFO("got ", got, " want ", want);
            CHECK(halfUlpDistance(got.r, want.r) <= TOLERANCE_HALF_ULPS);
            CHECK(halfUlpDistance(got.g, want.g) <= TOLERANCE_HALF_ULPS);
            CHECK(halfUlpDistance(got.b, want.b) <= TOLERANCE_HALF_ULPS);
            CHECK(got.a == 0x3C00U);  // alpha 1 exactly; blending is off
        }
    }

    // THE UPRIGHT WITNESS, AND EXACTLY WHAT IT WITNESSES. row -> ndc.y is fixed by the READBACK
    // layout, not by the shader, so the arms above already fail on a picture whose RAY runs the wrong
    // way up -- PROVIDED the two oracles differ by more than the tolerance, which is what this
    // asserts. MEASURED: seeding `float2(ndc.x, -ndc.y)` into the ray alone reddens SB8, SB9 and SB14
    // over 67 assertions. It does NOT witness the sign in sky.vert.hlsl's `ndc` expression, because
    // that sign moves the vertex and its ray together and is unobservable by construction -- also
    // measured, and recorded in the shader's own comment so nobody re-derives it.
    const Half4 topOracle = encodeHalf4(oracleAt(PROBES[0], 32U), 1.0F);
    const Half4 bottomOracle = encodeHalf4(oracleAt(PROBES[2], 32U), 1.0F);
    INFO("top oracle ", topOracle, " bottom oracle ", bottomOracle);
    CHECK(halfUlpDistance(topOracle.b, bottomOracle.b) > 2U * TOLERANCE_HALF_ULPS);
    // ...and the picture really is on the top side of that pair.
    CHECK(halfUlpDistance(halfAt(pixels, SIZE, PROBES[0], 32U).b, topOracle.b) <= TOLERANCE_HALF_ULPS);
}

TEST_CASE("render sky: geometry OVERDRAWS the sky when the forward pass follows it (SB9)") {
    AERO_SKY_TIER1_PREAMBLE();
    constexpr std::uint32_t SIZE = 64;
    auto target = engine::render::RenderTarget::create(
        *device, {SIZE, SIZE}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());
    auto pass = engine::render::SkyPass::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(pass.has_value());
    auto forward = engine::render::ForwardRenderer::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(forward.has_value());

    const engine::render::CameraView camera = frontCamera();
    const engine::render::EnvironmentData environment{};
    engine::render::MeshInstance cube;
    cube.primitive = engine::render::PrimitiveId::Cube;
    cube.model = engine::scaling(Vec3{2.0F, 2.0F, 2.0F});
    cube.normalMatrix = Mat4::identity();
    cube.mvp = camera.proj * camera.view * cube.model;
    cube.color = Vec3{1.0F, 0.0F, 0.0F};
    const std::array<engine::render::MeshInstance, 1> instances{cube};

    engine::render::RenderView view = skyView(camera, environment);
    view.instances = instances;
    view.directional = {.direction = Vec3{0.0F, 0.0F, -1.0F}, .color = Vec3::one(), .intensity = 3.0F};

    std::optional<engine::render::Frame> frame = target->beginFrame({0.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(frame.has_value());
    pass->draw(*frame, view);     // FIRST -- the sky writes no depth
    forward->draw(*frame, view);  // THEN the geometry, into the same open pass
    REQUIRE(target->endFrame(std::move(*frame)));
    std::vector<std::byte> pixels(static_cast<std::size_t>(SIZE) * SIZE * 8U, std::byte{0xAB});
    REQUIRE(device->readbackTexture(target->colorTexture(), 0, pixels));

    const engine::render::SkyGradient gradient = engine::render::resolveSkyGradient(environment);
    const Mat4 invViewProj = engine::inverse(camera.proj * camera.view);
    const auto skyOracleAt = [&](std::uint32_t row, std::uint32_t column) {
        return encodeHalf4(engine::render::skyRadiance(gradient, rayAtPixel(invViewProj, SIZE, SIZE, row, column)),
                           1.0F);
    };

    // THE CENTRE IS THE CUBE. A wrong pass ORDER -- sky recorded after the geometry -- makes this the
    // sky oracle instead, with no error anywhere.
    //
    // AT THE CORNER ARM'S OWN TOLERANCE, AND THAT IS THE WHOLE OF THE ASSERTION. A bare `!=` against
    // the oracle is very nearly unfalsifiable here: the corners below claim GPU/CPU agreement only to
    // within TOLERANCE_HALF_ULPS, so a centre that genuinely BECAME the sky still differs from the CPU
    // oracle in its last bit and a bit comparison reports SUCCESS. Measured with the depth-write seed
    // applied, where the sky really did reject the cube:
    //   CHECK_FALSE( half4(0x37d9, 0x385d, 0x391b, 0x3c00) == half4(0x37da, 0x385e, 0x391b, 0x3c00) )
    // -- one half-ulp apart on r and g, identical on b and a, and GREEN. "Not the sky" therefore has
    // to mean FURTHER THAN THE TOLERANCE on at least one channel, which is what this asserts.
    //
    // AND THIS CASE ORDERS ITS OWN TWO PASSES, IN ITS OWN BODY, a few lines above: no arm of it can
    // ever see a SceneRenderer-side ordering change. That half of the contract is SB16's alone.
    constexpr std::uint32_t TOLERANCE_HALF_ULPS = 2;
    const Half4 centre = halfAt(pixels, SIZE, SIZE / 2U, SIZE / 2U);
    const Half4 skyAtCentre = skyOracleAt(SIZE / 2U, SIZE / 2U);
    const std::uint32_t centreDistance =
        std::max({halfUlpDistance(centre.r, skyAtCentre.r), halfUlpDistance(centre.g, skyAtCentre.g),
                  halfUlpDistance(centre.b, skyAtCentre.b)});
    INFO("centre ", centre, " sky oracle there ", skyAtCentre);
    CHECK(centreDistance > TOLERANCE_HALF_ULPS);

    // ...AND THE FOUR CORNERS ARE STILL THE SKY, which is what says the geometry overdrew rather than
    // the sky failing to draw at all.
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 4> corners{
        std::pair{1U, 1U}, std::pair{1U, SIZE - 2U}, std::pair{SIZE - 2U, 1U}, std::pair{SIZE - 2U, SIZE - 2U}};
    for (const auto& [row, column] : corners) {
        CAPTURE(row);
        CAPTURE(column);
        const Half4 got = halfAt(pixels, SIZE, row, column);
        const Half4 want = skyOracleAt(row, column);
        INFO("got ", got, " want ", want);
        CHECK(halfUlpDistance(got.r, want.r) <= TOLERANCE_HALF_ULPS);
        CHECK(halfUlpDistance(got.g, want.g) <= TOLERANCE_HALF_ULPS);
        CHECK(halfUlpDistance(got.b, want.b) <= TOLERANCE_HALF_ULPS);
    }
}

TEST_CASE("render sky: a view with no camera records nothing, silently (SB10)") {
    AERO_SKY_TIER1_PREAMBLE();
    auto target = engine::render::RenderTarget::create(
        *device, {64, 64}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());
    auto pass = engine::render::SkyPass::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(pass.has_value());

    engine::render::RenderView view = skyView(frontCamera(), {});
    view.hasCamera = false;

    const engine::rhi::Color clear{0.25F, 0.75F, 0.5F, 1.0F};
    const std::vector<std::byte> pixels = skyOnce(*device, *target, *pass, view, clear, 8U);

    const Half4 expectedClear = encodeHalf4(Vec3{clear.r, clear.g, clear.b}, clear.a);
    std::size_t differing = 0;
    for (std::uint32_t row = 0; row < 64U; ++row) {
        for (std::uint32_t column = 0; column < 64U; ++column) {
            if (!(halfAt(pixels, 64U, row, column) == expectedClear)) {
                ++differing;
            }
        }
    }
    CHECK(differing == 0U);
    CHECK(pass->drawCount() == 0U);
    // A MISSING camera is not a DEGENERATE one: nothing latches here.
    CHECK_FALSE(pass->hasWarnedDegenerateCamera());
}

TEST_CASE("render sky: a degenerate camera records nothing and WARNs exactly once (SB11)") {
    AERO_SKY_TIER1_PREAMBLE();
    auto target = engine::render::RenderTarget::create(
        *device, {64, 64}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());
    auto pass = engine::render::SkyPass::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(pass.has_value());

    std::vector<std::string> warnings;
    const LogCallbackGuard detachOnExit;
    engine::setLogCallback([&warnings](const engine::LogRecord& record) {
        if (record.level >= engine::LogLevel::Warn) {
            warnings.emplace_back(record.message);
        }
    });

    const engine::render::CameraView degenerate{.view = engine::scaling(Vec3::zero()),
                                                .proj = engine::perspective(engine::radians(60.0F), 1.0F, 0.1F, 100.0F),
                                                .eyePosition = Vec3::zero()};
    const engine::render::RenderView view = skyView(degenerate, {});
    const engine::rhi::Color clear{0.25F, 0.75F, 0.5F, 1.0F};

    const std::vector<std::byte> first = skyOnce(*device, *target, *pass, view, clear, 8U);
    const std::vector<std::byte> second = skyOnce(*device, *target, *pass, view, clear, 8U);
    engine::setLogCallback({});  // detach BEFORE reading `warnings`

    const Half4 expectedClear = encodeHalf4(Vec3{clear.r, clear.g, clear.b}, clear.a);
    std::size_t differing = 0;
    for (const std::vector<std::byte>* pixels : {&first, &second}) {
        for (std::uint32_t row = 0; row < 64U; ++row) {
            for (std::uint32_t column = 0; column < 64U; ++column) {
                if (!(halfAt(*pixels, 64U, row, column) == expectedClear)) {
                    ++differing;
                }
            }
        }
    }
    CHECK(differing == 0U);
    CHECK(pass->drawCount() == 0U);
    CHECK(pass->hasWarnedDegenerateCamera());
    // LATCHED, once per pass lifetime -- across TWO draw calls, not one.
    const auto degenerateWarns = std::count_if(warnings.begin(), warnings.end(), [](const std::string& line) {
        return line.find("degenerate camera") != std::string::npos;
    });
    CHECK(degenerateWarns == 1);
}

TEST_CASE("render sky: an orthographic camera yields ONE colour, and it is the forward ray's (SB12)") {
    AERO_SKY_TIER1_PREAMBLE();
    constexpr std::uint32_t SIZE = 64;
    auto target = engine::render::RenderTarget::create(
        *device, {SIZE, SIZE}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());
    auto pass = engine::render::SkyPass::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(pass.has_value());

    // Pitched 20 degrees DOWN, so the one colour is neither the horizon nor an endpoint -- a pose at
    // the horizon would make "within tolerance of the forward ray's radiance" true of a much larger
    // family of wrong answers.
    const float pitch = engine::radians(20.0F);
    const Vec3 eye{0.0F, 6.0F, 12.0F};
    const Vec3 forwardDir{0.0F, -std::sin(pitch), -std::cos(pitch)};
    const engine::render::CameraView camera{.view = engine::lookAt(eye, eye + forwardDir, Vec3{0.0F, 1.0F, 0.0F}),
                                            .proj = engine::ortho(-5.0F, 5.0F, -5.0F, 5.0F, 0.1F, 200.0F),
                                            .eyePosition = eye};
    const engine::render::EnvironmentData environment{};
    const engine::render::RenderView view = skyView(camera, environment);
    const std::vector<std::byte> pixels = skyOnce(*device, *target, *pass, view, {1.0F, 0.0F, 1.0F, 1.0F}, 8U);
    CHECK(pass->drawCount() == 1U);

    // ARM 1 -- UNIFORM. Under a parallel projection every ray is the camera's forward direction, so
    // every texel is one value. On its own this is also true of a uniformly WRONG colour, which is
    // why arm 2 exists and why this case has two.
    const Half4 first = halfAt(pixels, SIZE, 0U, 0U);
    std::size_t differing = 0;
    for (std::uint32_t row = 0; row < SIZE; ++row) {
        for (std::uint32_t column = 0; column < SIZE; ++column) {
            if (!(halfAt(pixels, SIZE, row, column) == first)) {
                ++differing;
            }
        }
    }
    CHECK(differing == 0U);

    // ARM 2 -- and it is the CAMERA'S FORWARD RAY's radiance. Not read off the shader's own
    // unprojection: `forwardDir` is the vector this case built the view matrix FROM.
    const Half4 want =
        encodeHalf4(engine::render::skyRadiance(engine::render::resolveSkyGradient(environment), forwardDir), 1.0F);
    INFO("uniform texel ", first, " want ", want);
    constexpr std::uint32_t TOLERANCE_HALF_ULPS = 2;
    CHECK(halfUlpDistance(first.r, want.r) <= TOLERANCE_HALF_ULPS);
    CHECK(halfUlpDistance(first.g, want.g) <= TOLERANCE_HALF_ULPS);
    CHECK(halfUlpDistance(first.b, want.b) <= TOLERANCE_HALF_ULPS);
    CHECK(first.a == 0x3C00U);
}

TEST_CASE("render sky: the drawn alpha is 1 whatever the clear's was, and blending is off (SB13)") {
    AERO_SKY_TIER1_PREAMBLE();
    auto target = engine::render::RenderTarget::create(
        *device, {64, 64}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());
    auto pass = engine::render::SkyPass::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(pass.has_value());

    // A ZERO clear alpha: if blending were on, or the write mask dropped alpha, the drawn texels would
    // keep it. For a fullscreen triangle no half-covered texel exists, so the assertion is on the
    // BYTES rather than on a coverage argument.
    const Vec3 solid{0.125F, 0.375F, 0.875F};
    const engine::render::RenderView view =
        skyView(frontCamera(), {.backgroundMode = engine::render::BackgroundMode::Solid, .solidColor = solid});
    const std::vector<std::byte> pixels = skyOnce(*device, *target, *pass, view, {0.25F, 0.75F, 0.5F, 0.0F}, 8U);

    std::size_t wrongAlpha = 0;
    std::size_t wrongColor = 0;
    const Half4 expected = encodeHalf4(solid, 1.0F);
    for (std::uint32_t row = 0; row < 64U; ++row) {
        for (std::uint32_t column = 0; column < 64U; ++column) {
            const Half4 texel = halfAt(pixels, 64U, row, column);
            if (texel.a != 0x3C00U) {
                ++wrongAlpha;
            }
            if (!(texel == expected)) {
                ++wrongColor;
            }
        }
    }
    CHECK(wrongAlpha == 0U);
    // The colour half matters too: with blending ON against a zero destination alpha the RGB would
    // move as well, and an alpha-only assertion would not see it.
    CHECK(wrongColor == 0U);
}

TEST_CASE("render sky: a depth-free RGBA8Unorm frame draws, and carries no OETF (SB14)") {
    AERO_SKY_TIER1_PREAMBLE();
    constexpr std::uint32_t SIZE = 64;
    // depthFormat Invalid on BOTH the target and the pipeline -- the "a depth-free frame is LEGAL"
    // arm of SkyPassConfig::depthFormat, which is the only arm a caller outside SceneRenderer has.
    auto target = engine::render::RenderTarget::create(
        *device, {SIZE, SIZE}, {.colorFormat = engine::rhi::TextureFormat::RGBA8Unorm, .depth = false, .quantum = 1});
    REQUIRE(target.has_value());
    REQUIRE((target->depthFormat() == engine::rhi::TextureFormat::Invalid));
    auto pass = engine::render::SkyPass::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = engine::rhi::TextureFormat::Invalid});
    REQUIRE(pass.has_value());

    const engine::render::CameraView camera = frontCamera();
    const engine::render::EnvironmentData environment{};
    const std::vector<std::byte> pixels =
        skyOnce(*device, *target, *pass, skyView(camera, environment), {1.0F, 0.0F, 1.0F, 1.0F}, 4U);
    CHECK(pass->drawCount() == 1U);

    // THE BYTES ARE round(saturate(oracle) * 255) AND NOTHING ELSE. There is NO OETF on this path --
    // the sky writes raw linear radiance and 3.6.3's tonemap is what applies a transfer curve, which
    // this frame does not run. An sRGB encode would move 0.18 to 0.46, roughly a hundred levels.
    const engine::render::SkyGradient gradient = engine::render::resolveSkyGradient(environment);
    const Mat4 invViewProj = engine::inverse(camera.proj * camera.view);
    constexpr std::array<std::uint32_t, 3> PROBES{8U, 32U, 55U};
    for (const std::uint32_t row : PROBES) {
        for (const std::uint32_t column : PROBES) {
            CAPTURE(row);
            CAPTURE(column);
            const Vec3 oracle = engine::render::skyRadiance(gradient, rayAtPixel(invViewProj, SIZE, SIZE, row, column));
            const std::size_t base = (((static_cast<std::size_t>(row) * SIZE) + column) * 4U);
            const auto level = [&pixels, base](std::size_t offset) {
                return static_cast<int>(static_cast<std::uint8_t>(pixels[base + offset]));
            };
            const auto expected = [](float value) {
                const float clamped = value < 0.0F ? 0.0F : (value > 1.0F ? 1.0F : value);
                return static_cast<int>(std::lround(clamped * 255.0F));
            };
            INFO("rgb levels ", level(0), " ", level(1), " ", level(2), " oracle ", oracle.x, " ", oracle.y, " ",
                 oracle.z);
            CHECK(std::abs(level(0) - expected(oracle.x)) <= 1);
            CHECK(std::abs(level(1) - expected(oracle.y)) <= 1);
            CHECK(std::abs(level(2) - expected(oracle.z)) <= 1);
            CHECK(level(3) == 255);
        }
    }
}

TEST_CASE("render sky: a moved-from pass is a logged no-op and the move frees exactly once (SB15)") {
    AERO_SKY_TIER1_PREAMBLE();
    auto target = engine::render::RenderTarget::create(
        *device, {64, 64}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());

    std::vector<std::string> warnings;
    const LogCallbackGuard detachOnExit;
    engine::setLogCallback([&warnings](const engine::LogRecord& record) {
        if (record.level >= engine::LogLevel::Warn) {
            warnings.emplace_back(record.message);
        }
    });

    const engine::rhi::Color clear{0.25F, 0.75F, 0.5F, 1.0F};
    const Vec3 solid{0.125F, 0.375F, 0.875F};
    const engine::render::RenderView view =
        skyView(frontCamera(), {.backgroundMode = engine::render::BackgroundMode::Solid, .solidColor = solid});
    {
        auto source = engine::render::SkyPass::create(
            *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
        REQUIRE(source.has_value());
        skyOnce(*device, *target, *source, view, clear, 8U);
        CHECK(source->drawCount() == 1U);

        engine::render::SkyPass destination{std::move(*source)};
        CHECK(destination.drawCount() == 1U);  // the counter TRANSFERS with the pipeline

        // THE MOVED-FROM PASS: a logged no-op, and drawCount() UNMOVED (PostProcess::resolve's
        // asymmetry). NOLINT because reading a moved-from object is exactly what this case is about.
        const std::vector<std::byte> afterMove =
            skyOnce(*device, *target, *source, view, clear, 8U);  // NOLINT(bugprone-use-after-move)
        CHECK(source->drawCount() == 0U);                         // NOLINT(bugprone-use-after-move)
        const Half4 expectedClear = encodeHalf4(Vec3{clear.r, clear.g, clear.b}, clear.a);
        CHECK(halfAt(afterMove, 64U, 32U, 32U) == expectedClear);
        // A SECOND call must not log a SECOND time: the not-renderable latch is what keeps a
        // permanently broken pass from writing a line per frame forever.
        skyOnce(*device, *target, *source, view, clear, 8U);  // NOLINT(bugprone-use-after-move)

        // ...and the DESTINATION still draws.
        const std::vector<std::byte> fromDestination = skyOnce(*device, *target, destination, view, clear, 8U);
        CHECK(destination.drawCount() == 2U);
        CHECK(halfAt(fromDestination, 64U, 32U, 32U) == encodeHalf4(solid, 1.0F));
    }
    // Both passes are gone; the target and the device follow, and ~Device is what would report a
    // double free or a leaked pipeline.
    target.reset();
    device.reset();
    engine::setLogCallback({});

    const auto notRenderable = std::count_if(warnings.begin(), warnings.end(), [](const std::string& line) {
        return line.find("not renderable") != std::string::npos;
    });
    CHECK(notRenderable == 1);
    const bool leaked = std::any_of(warnings.begin(), warnings.end(),
                                    [](const std::string& line) { return line.find("leaked") != std::string::npos; });
    CHECK_FALSE(leaked);
}

TEST_CASE("render sky: the environment reaches the picture end to end through SceneRenderer (SB16)") {
    AERO_SKY_TIER1_PREAMBLE();
    constexpr std::uint32_t SIZE = 64;
    auto target = engine::render::RenderTarget::create(
        *device, {SIZE, SIZE}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());
    auto sceneRenderer =
        engine::scene_render::SceneRenderer::create(*device, vfs, target->colorFormat(), target->depthFormat());
    REQUIRE(sceneRenderer.has_value());

    // A camera looking at the origin, a cube ON the origin, and one directional light. NO Environment
    // -- the ordinary state of every scene authored before this task.
    engine::World world;
    const engine::Entity cameraEntity = world.create();
    REQUIRE(world.add<engine::Transform>(
                cameraEntity, engine::Transform{Vec3{0.0F, 0.0F, 5.0F}, engine::Quat::identity(), Vec3::one()}) !=
            nullptr);
    REQUIRE(world.add<engine::Camera>(cameraEntity, engine::Camera{}) != nullptr);

    const engine::Entity cubeEntity = world.create();
    REQUIRE(world.add<engine::Transform>(cubeEntity, engine::Transform{Vec3::zero(), engine::Quat::identity(),
                                                                       Vec3{2.0F, 2.0F, 2.0F}}) != nullptr);
    REQUIRE(world.add<engine::MeshRenderer>(
                cubeEntity, engine::MeshRenderer{static_cast<std::uint32_t>(engine::render::PrimitiveId::Cube),
                                                 Vec3{1.0F, 0.0F, 0.0F}}) != nullptr);

    const engine::Entity lightEntity = world.create();
    REQUIRE(world.add<engine::Transform>(lightEntity) != nullptr);  // identity -> the -Z world axis
    REQUIRE(world.add<engine::DirectionalLight>(lightEntity, engine::DirectionalLight{Vec3::one(), 3.0F}) != nullptr);

    // A clear UNEQUAL to every colour compared against below, so "the corners are the sky" cannot be
    // satisfied by a frame in which nothing was shaded at all.
    const engine::rhi::Color clear{0.9F, 0.1F, 0.9F, 1.0F};
    const auto renderOnce = [&]() {
        std::optional<engine::render::Frame> frame = target->beginFrame(clear);
        REQUIRE(frame.has_value());
        CHECK((frame->extent() == engine::rhi::Extent2D{SIZE, SIZE}));  // the aspect the oracle assumes
        sceneRenderer->render(world, *frame);
        REQUIRE(target->endFrame(std::move(*frame)));
        std::vector<std::byte> pixels(static_cast<std::size_t>(SIZE) * SIZE * 8U, std::byte{0xAB});
        REQUIRE(device->readbackTexture(target->colorTexture(), 0, pixels));
        return pixels;
    };

    // THE ORACLE'S CAMERA comes from the bridge's own resolver, on the SAME viewport render() uses --
    // the shared input, exactly as SB8/SB9 share theirs. THE ORACLE'S ENVIRONMENT DOES NOT: it is
    // stated here as a DEFAULT-CONSTRUCTED EnvironmentData, which is what makes "a World with no
    // Environment renders the default sky" an assertion rather than a tautology.
    engine::scene_render::RenderViewScratch scratch;
    const engine::render::RenderView resolved = engine::scene_render::buildRenderView(world, scratch, {SIZE, SIZE});
    REQUIRE(resolved.hasCamera);
    CHECK(resolved.environmentCount == 0U);
    const Mat4 invViewProj = engine::inverse(resolved.camera.proj * resolved.camera.view);
    const engine::render::SkyGradient defaultGradient =
        engine::render::resolveSkyGradient(engine::render::EnvironmentData{});

    constexpr std::uint32_t TOLERANCE_HALF_ULPS = 2;
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 4> corners{
        std::pair{1U, 1U}, std::pair{1U, SIZE - 2U}, std::pair{SIZE - 2U, 1U}, std::pair{SIZE - 2U, SIZE - 2U}};

    SUBCASE("no Environment -> the DEFAULT sky in the corners, the cube in the centre") {
        const std::vector<std::byte> pixels = renderOnce();
        for (const auto& [row, column] : corners) {
            CAPTURE(row);
            CAPTURE(column);
            const Half4 got = halfAt(pixels, SIZE, row, column);
            const Half4 want = encodeHalf4(
                engine::render::skyRadiance(defaultGradient, rayAtPixel(invViewProj, SIZE, SIZE, row, column)), 1.0F);
            INFO("got ", got, " want ", want);
            CHECK(halfUlpDistance(got.r, want.r) <= TOLERANCE_HALF_ULPS);
            CHECK(halfUlpDistance(got.g, want.g) <= TOLERANCE_HALF_ULPS);
            CHECK(halfUlpDistance(got.b, want.b) <= TOLERANCE_HALF_ULPS);
            // ...and NOT the clear, which is the anti-vacuity arm for the whole subcase.
            CHECK_FALSE(got == encodeHalf4(Vec3{clear.r, clear.g, clear.b}, clear.a));
        }
        // THE CENTRE IS THE CUBE. A sky recorded AFTER the forward pass makes this the sky oracle
        // instead, with no error anywhere -- SB9's claim, restated where SceneRenderer owns the order,
        // and THIS IS THE ONLY PLACE THAT HALF OF THE CONTRACT IS ASSERTED. SB9 orders its own two
        // passes in its own body and is structurally blind to a bridge-side reordering.
        //
        // AT THE CORNER ARMS' OWN TOLERANCE, for SB9's reason verbatim: those arms claim GPU/CPU
        // agreement only to within TOLERANCE_HALF_ULPS, so a centre that genuinely BECAME the sky is
        // still one half-ulp from the CPU oracle on r and g and a bit comparison reports SUCCESS --
        // measured against a real reordering, not reasoned.
        const Half4 centre = halfAt(pixels, SIZE, SIZE / 2U, SIZE / 2U);
        const Half4 skyAtCentre = encodeHalf4(
            engine::render::skyRadiance(defaultGradient, rayAtPixel(invViewProj, SIZE, SIZE, SIZE / 2U, SIZE / 2U)),
            1.0F);
        const std::uint32_t centreDistance =
            std::max({halfUlpDistance(centre.r, skyAtCentre.r), halfUlpDistance(centre.g, skyAtCentre.g),
                      halfUlpDistance(centre.b, skyAtCentre.b)});
        INFO("centre ", centre, " sky oracle there ", skyAtCentre);
        CHECK(centreDistance > TOLERANCE_HALF_ULPS);
    }

    SUBCASE("a Solid Environment -> the corners are EXACTLY its colour, byte for byte") {
        const Vec3 solid{0.125F, 0.375F, 0.875F};
        const engine::Entity environmentEntity = world.create();
        REQUIRE(world.add<engine::Environment>(
                    environmentEntity, engine::Environment{.backgroundMode = 1U, .solidColor = solid}) != nullptr);

        const std::vector<std::byte> pixels = renderOnce();

        // A frame that ONLY cleared to the same colour: no sky pass, no geometry, nothing recorded.
        // The two must agree BYTE FOR BYTE, which is the whole point of packing a Solid sky as two
        // exactly-zero deltas -- `x + 0 * w` is exact on every IEEE backend.
        std::optional<engine::render::Frame> clearOnly = target->beginFrame({solid.x, solid.y, solid.z, 1.0F});
        REQUIRE(clearOnly.has_value());
        REQUIRE(target->endFrame(std::move(*clearOnly)));
        std::vector<std::byte> cleared(static_cast<std::size_t>(SIZE) * SIZE * 8U, std::byte{0xAB});
        REQUIRE(device->readbackTexture(target->colorTexture(), 0, cleared));

        const Half4 expected = encodeHalf4(solid, 1.0F);
        for (const auto& [row, column] : corners) {
            CAPTURE(row);
            CAPTURE(column);
            const Half4 got = halfAt(pixels, SIZE, row, column);
            INFO("got ", got, " want ", expected);
            CHECK(got == expected);
            CHECK(got == halfAt(cleared, SIZE, row, column));
        }
        // ANTI-VACUITY: the clear used for the sky frame was NOT this colour, so the agreement above
        // is the shader's arithmetic and not a frame nobody drew into.
        CHECK_FALSE(expected == encodeHalf4(Vec3{clear.r, clear.g, clear.b}, clear.a));
        // ...and the centre is still the cube, so "Solid" did not simply overwrite everything. A BIT
        // comparison is legitimate HERE and nowhere else in this case: the corner arms just proved a
        // Solid sky lands on `expected` EXACTLY (`x + 0 * w`, exact on every IEEE backend), so a centre
        // that became the sky would be bit-EQUAL to it rather than a half-ulp away. That is precisely
        // what the two gradient arms cannot say, and why they state a tolerance instead.
        CHECK_FALSE(halfAt(pixels, SIZE, SIZE / 2U, SIZE / 2U) == expected);
    }
}

TEST_CASE("render sky: the hemisphere shades an up-facing face apart from a down-facing one (SB17)") {
    // THE AMBIENT HALF OF THE ENVIRONMENT, AT THE PIXEL TIER. Every other GPU case in this tree that
    // draws geometry resolves ambient FLAT at intensity 1 (DG16 and the whole OG battery, by design),
    // where halfDelta is exactly zero -- and SB9's and SB16's cube shows its +Z-facing quad, whose
    // N.y is 0, so `mid + halfDelta * N.y` is arithmetically insensitive to the delta there. Nothing
    // read a texel whose value depends on the hemisphere until this case, and ambientIrradiance -- a
    // public CPU mirror of scene.frag.hlsl's own term -- had never been compared against one, unlike
    // its twin skyRadiance. It lives in this file rather than in render_environment_test.cpp because
    // this is the task's only tier-1 file and SB9 already draws a lit cube through ForwardRenderer;
    // the sky pass is deliberately NOT recorded here, so nothing but the forward pass can write the
    // texels read below.
    AERO_SKY_TIER1_PREAMBLE();
    constexpr std::uint32_t SIZE = 64;
    auto target = engine::render::RenderTarget::create(
        *device, {SIZE, SIZE}, {.colorFormat = engine::rhi::TextureFormat::RGBA16Float, .depth = true, .quantum = 1});
    REQUIRE(target.has_value());
    auto forward = engine::render::ForwardRenderer::create(
        *device, vfs, {.colorFormat = target->colorFormat(), .depthFormat = target->depthFormat()});
    REQUIRE(forward.has_value());

    // A clear unequal to every colour compared against below, so no arm can be satisfied by a frame
    // in which nothing was shaded at all.
    const engine::rhi::Color clear{0.9F, 0.1F, 0.9F, 1.0F};

    // ONE WHITE CUBE at the origin on the DEFAULT material (an invalid MeshInstance::material
    // resolves to DEFAULT_MATERIAL_PARAMS, metallic 0 -- never MaterialParams{}, whose glTF metallic
    // of 1 renders near-black with no environment to reflect). White instance colour, white base
    // colour, occlusion 1 and emissive 0 make diffuseColor and occlusion EXACTLY one, so the shaded
    // texel IS the ambient term rather than a fraction of it.
    const auto shadeFace = [&](const engine::render::CameraView& camera,
                               const engine::render::EnvironmentData& environment, std::uint32_t tolerance) {
        engine::render::MeshInstance cube;
        cube.primitive = engine::render::PrimitiveId::Cube;
        cube.model = engine::scaling(Vec3{2.0F, 2.0F, 2.0F});
        cube.normalMatrix = Mat4::identity();
        cube.mvp = camera.proj * camera.view * cube.model;
        cube.color = Vec3::one();
        const std::array<engine::render::MeshInstance, 1> instances{cube};

        engine::render::RenderView view = skyView(camera, environment);
        view.instances = instances;
        // A REAL direction at intensity ZERO -- lighting.hpp's "intensity 0 means no directional
        // light". The direction still has to be a unit vector rather than the default zero one:
        // normalize(0,0,0) is a NaN L inside the BRDF and NaN * 0 is NaN, not zero, so a zero
        // direction would poison every texel this case reads with no light in the scene at all.
        view.directional = {.direction = Vec3{0.0F, 0.0F, -1.0F}, .color = Vec3::one(), .intensity = 0.0F};

        std::optional<engine::render::Frame> frame = target->beginFrame(clear);
        REQUIRE(frame.has_value());
        forward->draw(*frame, view);
        REQUIRE(target->endFrame(std::move(*frame)));
        std::vector<std::byte> pixels(static_cast<std::size_t>(SIZE) * SIZE * 8U, std::byte{0xAB});
        REQUIRE(device->readbackTexture(target->colorTexture(), 0, pixels));
        return centreBlockTexel(pixels, SIZE, tolerance);
    };

    // Straight down and straight up: the face at the image centre is then the cube's +Y quad
    // (world normal (0, 1, 0)) and its -Y quad ((0, -1, 0)). Those two are the only faces whose
    // tangent frame leaves N.y untouched by the flat default normal map -- both T and B have a zero
    // y component there -- which is what lets the arms below be exact about N.y rather than about a
    // normal perturbed by a fifth of a percent.
    const engine::render::CameraView fromAbove = axisCamera(Vec3{0.0F, 3.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F});
    const engine::render::CameraView fromBelow = axisCamera(Vec3{0.0F, -3.0F, 0.0F}, Vec3{0.0F, 0.0F, 1.0F});
    constexpr std::uint32_t TOLERANCE_HALF_ULPS = 2;

    const engine::render::EnvironmentData hemisphere{};  // the DEFAULT environment: Hemisphere, I = 0.5
    const engine::render::HemisphereAmbient resolved = engine::render::resolveAmbient(hemisphere);
    // ANTI-VACUITY ON THE FIXTURE: the default environment really does carry a non-zero half-delta on
    // every channel, so "the two texels differ" below is a claim about the shading rather than about
    // two numbers that happen not to match.
    REQUIRE(resolved.halfDelta.x > 0.0F);
    REQUIRE(resolved.halfDelta.y > 0.0F);
    REQUIRE(resolved.halfDelta.z > 0.0F);

    const Half4 up = shadeFace(fromAbove, hemisphere, TOLERANCE_HALF_ULPS);
    const Half4 down = shadeFace(fromBelow, hemisphere, TOLERANCE_HALF_ULPS);

    // (a) EACH texel IS ambientIrradiance's value at that face's normal, to within two half-ulps --
    //     the same bound SB8, SB9 and SB16 state for the sky. This is the arm that ties the CPU
    //     mirror to the picture; without it the case could only say the two differ, and a packer that
    //     scaled both by the same wrong factor would pass.
    const Vec3 upNormal{0.0F, 1.0F, 0.0F};
    const Vec3 downNormal{0.0F, -1.0F, 0.0F};
    const Half4 wantUp = encodeHalf4(engine::render::ambientIrradiance(resolved, upNormal), 1.0F);
    const Half4 wantDown = encodeHalf4(engine::render::ambientIrradiance(resolved, downNormal), 1.0F);
    INFO("up ", up, " want ", wantUp, " -- down ", down, " want ", wantDown);
    CHECK(halfUlpDistance(up.r, wantUp.r) <= TOLERANCE_HALF_ULPS);
    CHECK(halfUlpDistance(up.g, wantUp.g) <= TOLERANCE_HALF_ULPS);
    CHECK(halfUlpDistance(up.b, wantUp.b) <= TOLERANCE_HALF_ULPS);
    CHECK(up.a == encodeHalf(1.0F));
    CHECK(halfUlpDistance(down.r, wantDown.r) <= TOLERANCE_HALF_ULPS);
    CHECK(halfUlpDistance(down.g, wantDown.g) <= TOLERANCE_HALF_ULPS);
    CHECK(halfUlpDistance(down.b, wantDown.b) <= TOLERANCE_HALF_ULPS);
    CHECK(down.a == encodeHalf(1.0F));
    // ...and neither is the clear, which is the anti-vacuity arm for both readings.
    CHECK_FALSE(up == encodeHalf4(Vec3{clear.r, clear.g, clear.b}, clear.a));
    CHECK_FALSE(down == encodeHalf4(Vec3{clear.r, clear.g, clear.b}, clear.a));

    // (b) ...AND THEY DIFFER BY FAR MORE THAN THAT TOLERANCE, on every channel. THIS IS THE PROPERTY
    //     NO ZERO HALF-DELTA CAN PRODUCE: with the delta zero the two faces read the same mid and
    //     every distance below is 0.
    CHECK(halfUlpDistance(up.r, down.r) > TOLERANCE_HALF_ULPS);
    CHECK(halfUlpDistance(up.g, down.g) > TOLERANCE_HALF_ULPS);
    CHECK(halfUlpDistance(up.b, down.b) > TOLERANCE_HALF_ULPS);
    // AND THE DIRECTION OF THE DIFFERENCE, not merely its size: the default sky is brighter than the
    // default ground on every channel, so the up-facing face is the brighter one. Read as unsigned
    // integers, two NON-NEGATIVE finite halves compare in the same order as their values -- so the
    // sign bits are pinned clear first, and a NEGATIVE ground term (which is exactly what writing the
    // mid into the delta slot produces at N.y = -1) cannot masquerade as a large one.
    constexpr std::uint32_t HALF_SIGN_BIT = 0x8000U;
    CHECK((up.r & HALF_SIGN_BIT) == 0U);
    CHECK((up.g & HALF_SIGN_BIT) == 0U);
    CHECK((up.b & HALF_SIGN_BIT) == 0U);
    CHECK((down.r & HALF_SIGN_BIT) == 0U);
    CHECK((down.g & HALF_SIGN_BIT) == 0U);
    CHECK((down.b & HALF_SIGN_BIT) == 0U);
    CHECK(up.r > down.r);
    CHECK(up.g > down.g);
    CHECK(up.b > down.b);

    // (c) THE CONTROL, on the same geometry and the same two cameras: a FLAT ambient carries an
    //     exactly zero half-delta, so the two faces must come back BIT-IDENTICAL and the block itself
    //     admits a zero tolerance. It is what attributes (b)'s difference to the half-delta rather
    //     than to anything else the two views do not share -- and it is HE8's claim at the GPU tier,
    //     where the delta packing was chosen precisely so a Flat resolution is exact on every backend.
    const engine::render::EnvironmentData flat{.ambientMode = engine::render::AmbientMode::Flat,
                                               .ambientColor = Vec3{0.5F, 0.25F, 0.125F},
                                               .ambientIntensity = 0.75F};
    const engine::render::HemisphereAmbient flatResolved = engine::render::resolveAmbient(flat);
    const Half4 flatUp = shadeFace(fromAbove, flat, 0U);
    const Half4 flatDown = shadeFace(fromBelow, flat, 0U);
    INFO("flat up ", flatUp, " flat down ", flatDown);
    CHECK(flatUp == flatDown);
    // ...and it is the resolved mid EXACTLY, not merely a constant: 0.5/0.25/0.125 at I = 0.75 is
    // 0.375/0.1875/0.09375, three dyadic values a half carries without rounding, so this is a bit
    // comparison and needs no tolerance at all. A packer that swapped the mid and the delta writes
    // zero into the mid slot here and fails both of these.
    //
    // THE FIXTURE IS DYADIC ON PURPOSE, AND THAT IS WHAT MAKES A BIT COMPARISON SAFE ACROSS LANES:
    // each expected value sits a full half-ulp from either neighbour (a relative 2.4e-4 here), so it
    // absorbs any sub-ulp difference in the backend's sRGB decode of the default white base colour --
    // the one input this arm cannot compute for itself, and the reason the (a) arms above, whose
    // expectations are NOT representable, state a tolerance instead.
    CHECK(flatUp == encodeHalf4(flatResolved.mid, 1.0F));
    // ANTI-VACUITY: the Flat fixture is not the Hemisphere one, so (c) is not (a) restated.
    CHECK_FALSE(flatUp == up);
    CHECK_FALSE(flatUp == encodeHalf4(Vec3{clear.r, clear.g, clear.b}, clear.a));
}

#endif  // AERO_SHADER_TOOLS_ENABLED

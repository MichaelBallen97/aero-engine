// tests/rhi_upload_test.cpp — task E.1.1: Device::recordBufferUpload and Device::readbackTexture.
//
// TIER 1: a real-video Context and a real Device, no window (rhi_device_test.cpp's gate shape).
// AERO_REQUIRE_GPU=1 (every CI lane) turns each environment skip into a failure.
//
// RU5 IS THE FIRST PIXEL-LEVEL ASSERTION IN THIS TREE. Everything before it proves the plumbing;
// from RU5 on, a case reads bytes a GPU produced and compares them to exact numbers.

#include <aero/platform/platform.hpp>
#include <aero/rhi/rhi.hpp>

#include "rhi_test_support.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
// <ostream> is load-bearing on MSVC for any CHECK on a string_view (see rhi_device_test.cpp's
// comment) — included preventively, before the first such case is written here.
#include <ostream>
#include <span>
#include <utility>
#include <vector>

namespace {

// The tier-1 preamble, written out per case exactly as render_tonemap_test.cpp does it:
// AERO_SKIP_OR_FAIL returns from the enclosing function, so it cannot live in a helper.
#define AERO_RU_PREAMBLE()                                    \
    const engine::platform::Context ctx{{.headless = false}}; \
    if (!ctx.valid()) {                                       \
        AERO_SKIP_OR_FAIL("no real video driver available");  \
    }                                                         \
    auto device = engine::rhi::Device::create();              \
    if (!device.has_value()) {                                \
        AERO_SKIP_OR_FAIL("no GPU device available");         \
    }                                                         \
    (void)0

// A color target cleared by a real render pass, then read back. Returns `byteCount` bytes.
std::vector<std::byte> clearAndReadback(engine::rhi::Device& device, engine::rhi::TextureHandle texture,
                                        const engine::rhi::Color& color, std::size_t byteCount) {
    const engine::rhi::CommandBufferHandle cmd = device.acquireCommandBuffer();
    REQUIRE(cmd.valid());
    const engine::rhi::ColorAttachment attachment{.texture = texture, .clearColor = color};
    const engine::rhi::RenderPassHandle pass =
        device.beginRenderPass(cmd, {.colorAttachments = std::span{&attachment, 1}});
    REQUIRE(pass.valid());
    device.endRenderPass(pass);
    REQUIRE(device.submit(cmd));
    std::vector<std::byte> pixels(byteCount, std::byte{0xAB});
    REQUIRE(device.readbackTexture(texture, 0, pixels));
    return pixels;
}

[[nodiscard]] std::uint8_t byteAt(const std::vector<std::byte>& pixels, std::size_t index) {
    return static_cast<std::uint8_t>(pixels[index]);
}

}  // namespace

TEST_CASE("rhi upload: recordBufferUpload records without waiting and the buffer still submits (RU1)") {
    AERO_RU_PREAMBLE();
    const engine::rhi::BufferHandle buffer =
        device->createBuffer({.usage = engine::rhi::BufferUsage::Vertex, .size = 256});
    REQUIRE(buffer.valid());
    const engine::rhi::CommandBufferHandle cmd = device->acquireCommandBuffer();
    REQUIRE(cmd.valid());

    const std::array<std::uint32_t, 4> payload{1U, 2U, 3U, 4U};
    CHECK(device->recordBufferUpload(cmd, buffer, std::as_bytes(std::span{payload})));

    // THE STRUCTURAL ARM (seed S4's witness): the call did NOT submit and did NOT wait. If it had,
    // `cmd` would be stale and submit() would refuse it. A blocking implementation cannot pass this.
    CHECK(device->submit(cmd));
    // ...and NOW it is stale, which proves the line above consumed a LIVE handle rather than a dead one.
    CHECK_FALSE(device->submit(cmd));

    device->destroyBuffer(buffer);
}

TEST_CASE("rhi upload: recordBufferUpload refuses six ways and leaves the pass usable (RU2)") {
    AERO_RU_PREAMBLE();
    const engine::rhi::BufferHandle buffer =
        device->createBuffer({.usage = engine::rhi::BufferUsage::Vertex, .size = 64});
    REQUIRE(buffer.valid());
    const std::array<std::byte, 64> payload{};

    SUBCASE("a stale command buffer") {
        const engine::rhi::CommandBufferHandle cmd = device->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        REQUIRE(device->submit(cmd));
        CHECK_FALSE(device->recordBufferUpload(cmd, buffer, payload));
    }
    SUBCASE("an invalid command buffer") { CHECK_FALSE(device->recordBufferUpload({}, buffer, payload)); }
    SUBCASE("a destroyed buffer") {
        const engine::rhi::CommandBufferHandle cmd = device->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        const engine::rhi::BufferHandle doomed =
            device->createBuffer({.usage = engine::rhi::BufferUsage::Vertex, .size = 64});
        REQUIRE(doomed.valid());
        device->destroyBuffer(doomed);
        CHECK_FALSE(device->recordBufferUpload(cmd, doomed, payload));
        CHECK(device->submit(cmd));
    }
    SUBCASE("empty data") {
        const engine::rhi::CommandBufferHandle cmd = device->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        CHECK_FALSE(device->recordBufferUpload(cmd, buffer, std::span<const std::byte>{}));
        CHECK(device->submit(cmd));
    }
    SUBCASE("one byte too much") {
        const engine::rhi::CommandBufferHandle cmd = device->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        const std::array<std::byte, 65> tooBig{};
        CHECK_FALSE(device->recordBufferUpload(cmd, buffer, tooBig));
        CHECK(device->submit(cmd));
    }
    SUBCASE("A RENDER PASS OPEN ON cmd -- and the pass is still endable afterwards") {
        // Seed S3's witness. SDL only refuses this under debug_mode, so the refusal must be OURS.
        const engine::rhi::TextureHandle target =
            device->createTexture({.format = engine::rhi::TextureFormat::RGBA8Unorm,
                                   .usage = engine::rhi::TextureUsage::ColorTarget,
                                   .width = 4,
                                   .height = 4});
        REQUIRE(target.valid());
        const engine::rhi::CommandBufferHandle cmd = device->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        const engine::rhi::ColorAttachment attachment{.texture = target};
        const engine::rhi::RenderPassHandle pass =
            device->beginRenderPass(cmd, {.colorAttachments = std::span{&attachment, 1}});
        REQUIRE(pass.valid());

        CHECK_FALSE(device->recordBufferUpload(cmd, buffer, payload));

        // NOTHING WAS DISTURBED: the pass is still open, still endable, and the buffer still submits.
        device->endRenderPass(pass);
        CHECK(device->submit(cmd));
        device->destroyTexture(target);
    }
    SUBCASE("a moved-from Device") {
        const engine::rhi::CommandBufferHandle cmd = device->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        engine::rhi::Device moved = std::move(*device);
        CHECK_FALSE(device->recordBufferUpload(cmd, buffer, payload));  // NOLINT(bugprone-use-after-move)
        CHECK(moved.submit(cmd));
        moved.destroyBuffer(buffer);
        return;  // `device` is moved-from; skip the shared teardown below
    }
    device->destroyBuffer(buffer);
}

TEST_CASE("rhi upload: two uploads on one command buffer, and a whole-buffer replacement (RU3)") {
    AERO_RU_PREAMBLE();
    const engine::rhi::BufferHandle a = device->createBuffer({.usage = engine::rhi::BufferUsage::Vertex, .size = 256});
    const engine::rhi::BufferHandle b = device->createBuffer({.usage = engine::rhi::BufferUsage::Vertex, .size = 256});
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    const engine::rhi::CommandBufferHandle cmd = device->acquireCommandBuffer();
    REQUIRE(cmd.valid());

    const std::array<std::byte, 128> first{};
    const std::array<std::byte, 128> second{};
    const std::array<std::byte, 64> third{};
    // Seed S2's plumbing witness: both must record, on ONE command buffer, with the transfer buffer
    // cycled between them so the first copy's source survives. (The BYTE witness is RU12.)
    CHECK(device->recordBufferUpload(cmd, a, first));
    CHECK(device->recordBufferUpload(cmd, b, second));
    // The whole-buffer replacement: a smaller third write into `a` is legal and does not merge.
    CHECK(device->recordBufferUpload(cmd, a, third));
    CHECK(device->submit(cmd));

    device->destroyBuffer(a);
    device->destroyBuffer(b);
}

TEST_CASE("rhi upload: the Device's transfer buffer grows and is then reused (RU4)") {
    AERO_RU_PREAMBLE();
    const engine::rhi::BufferHandle small =
        device->createBuffer({.usage = engine::rhi::BufferUsage::Vertex, .size = 16});
    const engine::rhi::BufferHandle large =
        device->createBuffer({.usage = engine::rhi::BufferUsage::Vertex, .size = 1U << 20U});
    REQUIRE(small.valid());
    REQUIRE(large.valid());

    // 16 B (creates a 64 KiB floor), then 1 MiB (grows), then 16 B again (reuses, does not shrink).
    // There is no accessor for the transfer buffer's size -- deliberately, it is wrapper machinery --
    // so what is asserted is the EFFECT: all three succeed, in that order, on three command buffers.
    const std::array<std::pair<engine::rhi::BufferHandle, std::size_t>, 3> steps{{
        {small, 16U},
        {large, 1U << 20U},
        {small, 16U},
    }};
    for (const auto& [buffer, bytes] : steps) {
        const std::vector<std::byte> payload(bytes, std::byte{0x5A});
        const engine::rhi::CommandBufferHandle cmd = device->acquireCommandBuffer();
        REQUIRE(cmd.valid());
        CHECK(device->recordBufferUpload(cmd, buffer, payload));
        CHECK(device->submit(cmd));
    }
    CHECK(device->waitIdle());
    device->destroyBuffer(small);
    device->destroyBuffer(large);
}

TEST_CASE("rhi readback: a cleared 4x4 RGBA8Unorm target reads back exactly (RU5)") {
    AERO_RU_PREAMBLE();
    // THE FIRST PIXEL-LEVEL ASSERTION IN THIS TREE. Every number below is exact in fp32 and exact
    // under UNORM's round-to-nearest: 0.2*255 = 51.0, 0.4*255 = 102.0, 0.6*255 = 153.0. No tolerance
    // is stated because none is needed -- if a lane disagrees here, the readback is wrong, not loose.
    const engine::rhi::TextureHandle target = device->createTexture({.format = engine::rhi::TextureFormat::RGBA8Unorm,
                                                                     .usage = engine::rhi::TextureUsage::ColorTarget,
                                                                     .width = 4,
                                                                     .height = 4});
    REQUIRE(target.valid());

    const std::vector<std::byte> pixels =
        clearAndReadback(*device, target, {0.2F, 0.4F, 0.6F, 1.0F}, static_cast<std::size_t>(4U * 4U * 4U));
    REQUIRE(pixels.size() == 64U);
    for (std::size_t texel = 0; texel < 16U; ++texel) {
        const std::size_t base = texel * 4U;
        CHECK(byteAt(pixels, base + 0U) == 51U);
        CHECK(byteAt(pixels, base + 1U) == 102U);
        CHECK(byteAt(pixels, base + 2U) == 153U);
        CHECK(byteAt(pixels, base + 3U) == 255U);
    }
    device->destroyTexture(target);
}

TEST_CASE("rhi readback: six refusals, each leaving `out` untouched (RU6)") {
    AERO_RU_PREAMBLE();
    const engine::rhi::TextureHandle color = device->createTexture({.format = engine::rhi::TextureFormat::RGBA8Unorm,
                                                                    .usage = engine::rhi::TextureUsage::Sampler,
                                                                    .width = 4,
                                                                    .height = 4});
    REQUIRE(color.valid());

    // The sentinel is what makes "untouched" an assertion rather than a hope.
    const auto untouched = [](const std::vector<std::byte>& bytes) {
        for (const std::byte value : bytes) {
            if (value != std::byte{0xAB}) {
                return false;
            }
        }
        return true;
    };

    SUBCASE("a depth format") {
        // D32Float is the format Device::supportsTextureFormat is asked about elsewhere; if this
        // device cannot make one, the refusal under test is unreachable and the subcase says so
        // loudly rather than silently passing on an absent texture.
        const engine::rhi::TextureFormat depthFormat = engine::rhi::TextureFormat::D32Float;
        if (!device->supportsTextureFormat(depthFormat, engine::rhi::TextureUsage::DepthStencilTarget)) {
            MESSAGE("device does not support D32Float as a depth target; the depth-refusal arm is unreachable");
        } else {
            const engine::rhi::TextureHandle depth =
                device->createTexture({.format = depthFormat,
                                       .usage = engine::rhi::TextureUsage::DepthStencilTarget,
                                       .width = 4,
                                       .height = 4});
            REQUIRE(depth.valid());
            std::vector<std::byte> out(64U, std::byte{0xAB});
            CHECK_FALSE(device->readbackTexture(depth, 0, out));
            CHECK(untouched(out));
            device->destroyTexture(depth);
        }
    }
    SUBCASE("a depth format with an EMPTY out -- the one input the blockBytes guard protects") {
        // NOT a duplicate of the subcase above, and the difference is the whole reason it exists.
        // There out.size() is 64 and the SIZE rule refuses first (a depth format's expected size is
        // 0), so the blockBytes guard is never reached and deleting it changes nothing. At
        // out.size() == 0 the size rule AGREES, execution reaches
        // ((mipWidth + pitchBlockW - 1) / pitchBlockW) with texelBlockWidth(D32Float) == 0, and the
        // division by zero aborts. MEASURED: with the guard deleted this subcase SIGABRTs; with it
        // present it returns false cleanly.
        const engine::rhi::TextureFormat depthFormat = engine::rhi::TextureFormat::D32Float;
        if (!device->supportsTextureFormat(depthFormat, engine::rhi::TextureUsage::DepthStencilTarget)) {
            MESSAGE("device does not support D32Float as a depth target; the zero-length arm is unreachable");
        } else {
            const engine::rhi::TextureHandle depth =
                device->createTexture({.format = depthFormat,
                                       .usage = engine::rhi::TextureUsage::DepthStencilTarget,
                                       .width = 4,
                                       .height = 4});
            REQUIRE(depth.valid());
            CHECK_FALSE(device->readbackTexture(depth, 0, std::span<std::byte>{}));
            device->destroyTexture(depth);
        }
    }
    SUBCASE("a mip past mipLevels") {
        std::vector<std::byte> out(64U, std::byte{0xAB});
        CHECK_FALSE(device->readbackTexture(color, 1, out));  // mipLevels defaults to 1
        CHECK(untouched(out));
    }
    SUBCASE("out.size() one byte too small") {
        std::vector<std::byte> out(63U, std::byte{0xAB});
        CHECK_FALSE(device->readbackTexture(color, 0, out));
        CHECK(untouched(out));
    }
    SUBCASE("out.size() one byte too large") {
        std::vector<std::byte> out(65U, std::byte{0xAB});
        CHECK_FALSE(device->readbackTexture(color, 0, out));
        CHECK(untouched(out));
    }
    SUBCASE("a stale handle") {
        const engine::rhi::TextureHandle doomed =
            device->createTexture({.format = engine::rhi::TextureFormat::RGBA8Unorm,
                                   .usage = engine::rhi::TextureUsage::Sampler,
                                   .width = 4,
                                   .height = 4});
        REQUIRE(doomed.valid());
        device->destroyTexture(doomed);
        std::vector<std::byte> out(64U, std::byte{0xAB});
        CHECK_FALSE(device->readbackTexture(doomed, 0, out));
        CHECK(untouched(out));
    }
    SUBCASE("a moved-from Device") {
        std::vector<std::byte> out(64U, std::byte{0xAB});
        engine::rhi::Device moved = std::move(*device);
        CHECK_FALSE(device->readbackTexture(color, 0, out));  // NOLINT(bugprone-use-after-move)
        CHECK(untouched(out));
        moved.destroyTexture(color);
        return;
    }
    device->destroyTexture(color);
}

TEST_CASE("rhi readback: a Sampler-only texture round-trips byte for byte (RU7)") {
    AERO_RU_PREAMBLE();
    // The claim in readbackTexture's own header: usage does NOT gate readability. A texture with no
    // ColorTarget bit is still a legal copy source on all three backends (Vulkan sets
    // VK_IMAGE_USAGE_TRANSFER_SRC_BIT unconditionally; D3D12 and Metal copy from any resource).
    const engine::rhi::TextureHandle texture = device->createTexture({.format = engine::rhi::TextureFormat::RGBA8Unorm,
                                                                      .usage = engine::rhi::TextureUsage::Sampler,
                                                                      .width = 2,
                                                                      .height = 2});
    REQUIRE(texture.valid());
    const std::array<std::uint8_t, 16> pattern{0x01U, 0x02U, 0x03U, 0x04U, 0x11U, 0x12U, 0x13U, 0x14U,
                                               0x21U, 0x22U, 0x23U, 0x24U, 0x31U, 0x32U, 0x33U, 0x34U};
    REQUIRE(device->uploadTexture(texture, 0, std::as_bytes(std::span{pattern})));

    std::vector<std::byte> out(16U, std::byte{0xAB});
    REQUIRE(device->readbackTexture(texture, 0, out));
    for (std::size_t i = 0; i < 16U; ++i) {
        CHECK(byteAt(out, i) == pattern[i]);
    }
    device->destroyTexture(texture);
}

TEST_CASE("rhi readback: mip 1 of a two-level texture reads its own 16 bytes (RU8)") {
    AERO_RU_PREAMBLE();
    const engine::rhi::TextureHandle texture = device->createTexture({.format = engine::rhi::TextureFormat::RGBA8Unorm,
                                                                      .usage = engine::rhi::TextureUsage::Sampler,
                                                                      .width = 4,
                                                                      .height = 4,
                                                                      .mipLevels = 2});
    REQUIRE(texture.valid());
    const std::array<std::uint8_t, 64> level0{};                            // 4x4x4
    const std::array<std::uint8_t, 16> level1{0x9AU, 0x9BU, 0x9CU, 0x9DU};  // 2x2x4; rest zero
    REQUIRE(device->uploadTexture(texture, 0, std::as_bytes(std::span{level0})));
    REQUIRE(device->uploadTexture(texture, 1, std::as_bytes(std::span{level1})));

    // level 1 is 2x2 -> 16 bytes, NOT 64: the size rule is validated against the MIP's extent (E10).
    std::vector<std::byte> tooBig(64U, std::byte{0xAB});
    CHECK_FALSE(device->readbackTexture(texture, 1, tooBig));

    std::vector<std::byte> out(16U, std::byte{0xAB});
    REQUIRE(device->readbackTexture(texture, 1, out));
    CHECK(byteAt(out, 0) == 0x9AU);
    CHECK(byteAt(out, 1) == 0x9BU);
    CHECK(byteAt(out, 2) == 0x9CU);
    CHECK(byteAt(out, 3) == 0x9DU);
    device->destroyTexture(texture);
}

TEST_CASE("rhi readback: an RGBA16Float clear reads back as exact half bit patterns (RU9)") {
    AERO_RU_PREAMBLE();
    // 0.5 -> 0x3800, 1.0 -> 0x3C00, 0.25 -> 0x3400. All three are exactly representable as IEEE-754
    // binary16, so no decode and no tolerance are needed -- the bytes ARE the assertion. Little-endian
    // is the byte order every one of the three lanes runs on.
    const engine::rhi::TextureFormat format = engine::rhi::TextureFormat::RGBA16Float;
    if (!device->supportsTextureFormat(format, engine::rhi::TextureUsage::ColorTarget)) {
        AERO_SKIP_OR_FAIL("device does not support RGBA16Float as a color target");
    }
    const engine::rhi::TextureHandle target = device->createTexture(
        {.format = format, .usage = engine::rhi::TextureUsage::ColorTarget, .width = 2, .height = 2});
    REQUIRE(target.valid());

    const std::vector<std::byte> pixels =
        clearAndReadback(*device, target, {0.5F, 1.0F, 0.25F, 1.0F}, static_cast<std::size_t>(2U * 2U * 8U));
    REQUIRE(pixels.size() == 32U);
    const std::array<std::uint16_t, 4> expected{0x3800U, 0x3C00U, 0x3400U, 0x3C00U};
    for (std::size_t texel = 0; texel < 4U; ++texel) {
        for (std::size_t channel = 0; channel < 4U; ++channel) {
            const std::size_t base = ((texel * 4U) + channel) * 2U;
            const auto got = static_cast<std::uint16_t>(byteAt(pixels, base) | (byteAt(pixels, base + 1U) << 8U));
            CHECK(got == expected[channel]);
        }
    }
    device->destroyTexture(target);
}

TEST_CASE("rhi readback: row 0 in is row 0 out (RU10)") {
    AERO_RU_PREAMBLE();
    // Seed S8's witness (a bottom-up readback). This pins UPLOAD-to-READBACK row order only; that
    // row 0 is the TOP of a RENDERED image is DG5's mirror-row assertion, which is a different claim
    // in a different file and neither substitutes for the other.
    const engine::rhi::TextureHandle texture = device->createTexture({.format = engine::rhi::TextureFormat::RGBA8Unorm,
                                                                      .usage = engine::rhi::TextureUsage::Sampler,
                                                                      .width = 2,
                                                                      .height = 2});
    REQUIRE(texture.valid());
    const std::array<std::uint8_t, 16> pattern{
        255U, 0U,   0U,   255U,  // (0,0) red
        0U,   255U, 0U,   255U,  // (1,0) green
        0U,   0U,   255U, 255U,  // (0,1) blue
        255U, 255U, 255U, 255U,  // (1,1) white
    };
    REQUIRE(device->uploadTexture(texture, 0, std::as_bytes(std::span{pattern})));

    std::vector<std::byte> out(16U, std::byte{0xAB});
    REQUIRE(device->readbackTexture(texture, 0, out));
    for (std::size_t i = 0; i < 16U; ++i) {
        CHECK(byteAt(out, i) == pattern[i]);
    }
    // And the discriminating form, spelled separately so a failure names the row rather than a byte:
    // a flipped readback would put white where red is.
    CHECK(byteAt(out, 0) == 255U);
    CHECK(byteAt(out, 1) == 0U);
    CHECK(byteAt(out, 12) == 255U);
    CHECK(byteAt(out, 13) == 255U);
    device->destroyTexture(texture);
}

// ================================================================================================
// RU11-RU12 -- the two cases that DRAW. Gated on AERO_SHADER_TOOLS_ENABLED for the reason
// render_tonemap_test.cpp's own tier-1 block is: a pipeline needs a cooked shader, and
// AERO_SHADERS_DIR exists only when the shader toolchain built one. RU1-RU10 above run in EVERY
// configuration, which is where the whole upload/readback contract is proved; these two add the
// end-to-end observation that an uploaded buffer is what the rasterizer actually reads.
//
// They draw through the cooked CUBE pair, not the triangle pair: triangle.vert.hlsl is
// vertex-pulling (it takes `uint vertexId : SV_VertexID` and holds its three positions in a
// constant array), so no vertex buffer of any kind reaches it and it cannot witness an upload.
// cube.vert.hlsl is the tree's only cooked vertex shader whose inputs come from a vertex buffer:
// float3 position TEXCOORD0 @0, float2 uv TEXCOORD1 @12, float3 color TEXCOORD2 @20, 32-byte pitch,
// with a float4x4 at b0/space1. cube.frag.hlsl returns texel.rgb * color, so a 1x1 WHITE sampled
// texture makes the written pixel the per-vertex colour exactly.
// ================================================================================================

#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/core/math.hpp>
    #include <aero/core/vfs.hpp>
    #include <aero/rhi/shader_loader.hpp>

    #include <memory>

namespace {

// The cube pair's vertex, spelled in plain floats so the offsets below ARE the layout.
struct RuVertex {
    float px = 0.0F;
    float py = 0.0F;
    float pz = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
};
static_assert(sizeof(RuVertex) == 32);

constexpr std::uint32_t RU_TARGET_SIZE = 64;
constexpr std::size_t RU_TARGET_BYTES = static_cast<std::size_t>(RU_TARGET_SIZE) * RU_TARGET_SIZE * 4U;

// Two triangles covering the NDC rectangle [xLeft, xRight] x [-1, +1], in one flat colour.
std::array<RuVertex, 6> makeBar(float xLeft, float xRight, float red, float green, float blue) {
    const auto corner = [&](float x, float y) {
        return RuVertex{.px = x, .py = y, .pz = 0.0F, .u = 0.0F, .v = 0.0F, .r = red, .g = green, .b = blue};
    };
    return {corner(xLeft, -1.0F), corner(xRight, -1.0F), corner(xRight, 1.0F),
            corner(xLeft, -1.0F), corner(xRight, 1.0F),  corner(xLeft, 1.0F)};
}

// Everything both drawing cases need beyond the Device: the cube pipeline, a 64x64 colour target,
// the 6-vertex buffer the upload under test writes, and the 1x1 WHITE texture + sampler that make
// cube.frag's `texel.rgb * color` equal the per-vertex colour exactly.
struct RuDrawFixture {
    engine::rhi::TextureHandle whiteTexture;
    engine::rhi::SamplerHandle sampler;
    engine::rhi::GraphicsPipelineHandle pipeline;
    engine::rhi::TextureHandle target;
    engine::rhi::BufferHandle vertexBuffer;
};

RuDrawFixture makeDrawFixture(engine::rhi::Device& device) {
    RuDrawFixture fixture{};
    fixture.whiteTexture = device.createTexture({.format = engine::rhi::TextureFormat::RGBA8Unorm,
                                                 .usage = engine::rhi::TextureUsage::Sampler,
                                                 .width = 1,
                                                 .height = 1});
    REQUIRE(fixture.whiteTexture.valid());
    const std::array<std::uint8_t, 4> whiteTexel{255U, 255U, 255U, 255U};
    REQUIRE(device.uploadTexture(fixture.whiteTexture, 0, std::as_bytes(std::span{whiteTexel})));
    fixture.sampler = device.createSampler({});
    REQUIRE(fixture.sampler.valid());

    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));
    const engine::rhi::ShaderHandle vs = loadShader(device, vfs, "res://cube.vert");
    const engine::rhi::ShaderHandle fs = loadShader(device, vfs, "res://cube.frag");
    REQUIRE(vs.valid());
    REQUIRE(fs.valid());
    const engine::rhi::VertexBufferLayout vbLayout{.slot = 0, .pitch = sizeof(RuVertex)};
    const std::array<engine::rhi::VertexAttribute, 3> attrs{{
        {.location = 0, .bufferSlot = 0, .format = engine::rhi::VertexFormat::Float3, .offset = 0},
        {.location = 1, .bufferSlot = 0, .format = engine::rhi::VertexFormat::Float2, .offset = 12},
        {.location = 2, .bufferSlot = 0, .format = engine::rhi::VertexFormat::Float3, .offset = 20},
    }};
    const engine::rhi::ColorTargetDesc colorTarget{.format = engine::rhi::TextureFormat::RGBA8Unorm};
    // No depth and no culling: makeBar's winding is deliberately not part of what is asserted.
    const engine::rhi::GraphicsPipelineDesc pipelineDesc{
        .vertexShader = vs,
        .fragmentShader = fs,
        .vertexBuffers = std::span{&vbLayout, 1},
        .vertexAttributes = attrs,
        .rasterizer = {.cullMode = engine::rhi::CullMode::None},
        .colorTargets = std::span{&colorTarget, 1},
    };
    fixture.pipeline = device.createGraphicsPipeline(pipelineDesc);
    device.destroyShader(vs);  // safe after pipeline creation (device.hpp)
    device.destroyShader(fs);
    REQUIRE(fixture.pipeline.valid());

    fixture.target = device.createTexture({.format = engine::rhi::TextureFormat::RGBA8Unorm,
                                           .usage = engine::rhi::TextureUsage::ColorTarget,
                                           .width = RU_TARGET_SIZE,
                                           .height = RU_TARGET_SIZE});
    REQUIRE(fixture.target.valid());
    fixture.vertexBuffer =
        device.createBuffer({.usage = engine::rhi::BufferUsage::Vertex, .size = sizeof(std::array<RuVertex, 6>)});
    REQUIRE(fixture.vertexBuffer.valid());
    return fixture;
}

void destroyDrawFixture(engine::rhi::Device& device, const RuDrawFixture& fixture) {
    device.destroyBuffer(fixture.vertexBuffer);
    device.destroyTexture(fixture.target);
    device.destroyGraphicsPipeline(fixture.pipeline);
    device.destroySampler(fixture.sampler);
    device.destroyTexture(fixture.whiteTexture);
}

// ONE command buffer carries the upload AND the draw: recordBufferUpload BEFORE the pass opens,
// then bind and draw inside it. Nothing waits until the readback the caller does afterwards.
void uploadAndDrawBar(engine::rhi::Device& device, const RuDrawFixture& fixture, const std::array<RuVertex, 6>& bar) {
    const engine::Mat4 mvp = engine::Mat4::identity();
    const engine::rhi::CommandBufferHandle cmd = device.acquireCommandBuffer();
    REQUIRE(cmd.valid());
    CHECK(device.recordBufferUpload(cmd, fixture.vertexBuffer, std::as_bytes(std::span{bar})));
    const engine::rhi::ColorAttachment attachment{.texture = fixture.target, .clearColor = {0.0F, 0.0F, 0.0F, 1.0F}};
    const engine::rhi::RenderPassHandle pass =
        device.beginRenderPass(cmd, {.colorAttachments = std::span{&attachment, 1}});
    REQUIRE(pass.valid());
    device.pushVertexUniforms(cmd, 0, std::as_bytes(std::span{mvp.data(), 16}));
    device.bindGraphicsPipeline(pass, fixture.pipeline);
    device.bindVertexBuffer(pass, 0, fixture.vertexBuffer);
    const engine::rhi::TextureSamplerBinding binding{.texture = fixture.whiteTexture, .sampler = fixture.sampler};
    device.bindFragmentSamplers(pass, 0, std::span{&binding, 1});
    device.draw(pass, 6);
    device.endRenderPass(pass);
    CHECK(device.submit(cmd));
}

[[nodiscard]] std::uint8_t channelAt(const std::vector<std::byte>& pixels, std::uint32_t x, std::uint32_t y,
                                     std::size_t channel) {
    const std::size_t base = (((static_cast<std::size_t>(y) * RU_TARGET_SIZE) + x) * 4U) + channel;
    return static_cast<std::uint8_t>(pixels[base]);
}

}  // namespace

TEST_CASE("rhi upload: a recordBufferUpload and a draw on ONE command buffer see the same data (RU11)") {
    AERO_RU_PREAMBLE();
    const RuDrawFixture fixture = makeDrawFixture(*device);

    // A red bar covering the LEFT half of a 64x64 target cleared black, uploaded through
    // recordBufferUpload on the SAME command buffer that then draws it -- the shape approach B (the
    // copy pass on the frame's own command buffer) would use, proven once so it cannot rot unnoticed.
    uploadAndDrawBar(*device, fixture, makeBar(-1.0F, 0.0F, 1.0F, 0.0F, 0.0F));

    std::vector<std::byte> pixels(RU_TARGET_BYTES, std::byte{0xAB});
    REQUIRE(device->readbackTexture(fixture.target, 0, pixels));
    // INTERIOR pixels only -- never an edge, never a vertex (the diamond-exit/Bresenham
    // disagreement lives exactly there). Column 8 is deep inside the bar; column 56 deep outside.
    CHECK(channelAt(pixels, 8, 32, 0) == 255U);
    CHECK(channelAt(pixels, 8, 32, 1) == 0U);
    CHECK(channelAt(pixels, 8, 32, 2) == 0U);
    CHECK(channelAt(pixels, 8, 32, 3) == 255U);
    CHECK(channelAt(pixels, 56, 32, 0) == 0U);
    CHECK(channelAt(pixels, 56, 32, 1) == 0U);
    CHECK(channelAt(pixels, 56, 32, 2) == 0U);
    CHECK(channelAt(pixels, 56, 32, 3) == 255U);

    destroyDrawFixture(*device, fixture);
}

TEST_CASE("rhi upload: ten consecutive frames each read THEIR OWN uploaded data (RU12)") {
    AERO_RU_PREAMBLE();
    const RuDrawFixture fixture = makeDrawFixture(*device);

    // SEEDS S1, S2 AND S6's WITNESS. Ten frames back to back into ONE vertex buffer, each frame
    // drawing a four-pixel-wide vertical bar at a DIFFERENT column, each followed by a readback.
    // Frame k must show ONLY columns 4k..4k+3 lit and the previous frame's column dark. A
    // cycle = false on either the transfer buffer or the destination lets frame k read frame k-1's
    // geometry whenever the driver has not yet retired the previous frame; a readback that returns
    // before its fence shows stale bytes.
    //
    // THE HONEST CAVEAT, recorded rather than hidden: a driver that serialises frames makes the
    // seeded (broken) form pass too. Ten frames with no intervening waitIdle is what maximises
    // overlap, and the CONTRACT rests on SDL's documented model (the cycling note in SDL_gpu.h),
    // not on this case discriminating on every driver.
    for (std::uint32_t k = 0; k < 10U; ++k) {
        const auto left = static_cast<float>(4U * k);
        const auto right = static_cast<float>((4U * k) + 4U);
        const auto size = static_cast<float>(RU_TARGET_SIZE);
        uploadAndDrawBar(*device, fixture,
                         makeBar((left / size * 2.0F) - 1.0F, (right / size * 2.0F) - 1.0F, 0.0F, 1.0F, 0.0F));

        std::vector<std::byte> pixels(RU_TARGET_BYTES, std::byte{0xAB});
        REQUIRE(device->readbackTexture(fixture.target, 0, pixels));
        // NO waitIdle between iterations -- that is the whole point.
        CHECK(channelAt(pixels, (4U * k) + 2U, 32, 1) == 255U);
        CHECK(channelAt(pixels, (4U * k) + 2U, 32, 0) == 0U);
        if (k > 0U) {
            CHECK(channelAt(pixels, (4U * (k - 1U)) + 2U, 32, 1) == 0U);
        }
    }

    destroyDrawFixture(*device, fixture);
}

#endif  // AERO_SHADER_TOOLS_ENABLED

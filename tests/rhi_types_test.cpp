// Aero Engine — rhi vocabulary unit tests (task 0.4.1). Black-box over <aero/rhi/rhi.hpp>: handle
// identity/layout, descriptor aggregate-ness + designated-init ergonomics, the D16 convention
// defaults, and the flag-enum operators.
//
// Device is NEVER called here — its members are deliberately declared but NOT defined until 0.4.2;
// odr-using any of them (calling one, taking its address, capturing it) would be a link error by
// design. Group F below uses only <type_traits> queries (is_move_constructible_v, etc.), which do
// NOT odr-use the queried members.
#include <aero/rhi/rhi.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <type_traits>

// docs/04 forbids `using namespace` in HEADERS; this is a test TU (math_test.cpp precedent).
using namespace engine::rhi;

// -------------------------------------------------------------------------------------------
// Group A — handles (AC-2)
// -------------------------------------------------------------------------------------------

TEST_CASE("rhi handles: all eight aliases are engine::Handle<Tag> instantiations") {
    static_assert(std::is_same_v<BufferHandle, engine::Handle<Buffer>>);
    static_assert(std::is_same_v<TextureHandle, engine::Handle<Texture>>);
    static_assert(std::is_same_v<SamplerHandle, engine::Handle<Sampler>>);
    static_assert(std::is_same_v<ShaderHandle, engine::Handle<Shader>>);
    static_assert(std::is_same_v<GraphicsPipelineHandle, engine::Handle<GraphicsPipeline>>);
    static_assert(std::is_same_v<SwapchainHandle, engine::Handle<Swapchain>>);
    static_assert(std::is_same_v<CommandBufferHandle, engine::Handle<CommandBuffer>>);
    static_assert(std::is_same_v<RenderPassHandle, engine::Handle<RenderPass>>);
    CHECK(true);
}

TEST_CASE("rhi handles: mutually non-interchangeable types") {
    static_assert(!std::is_same_v<BufferHandle, TextureHandle>);
    static_assert(!std::is_same_v<CommandBufferHandle, RenderPassHandle>);
    static_assert(!std::is_same_v<TextureHandle, SwapchainHandle>);
    static_assert(!std::is_same_v<ShaderHandle, GraphicsPipelineHandle>);
    static_assert(!std::is_same_v<SamplerHandle, TextureHandle>);
    CHECK(true);
}

TEST_CASE("rhi handles: trivially copyable, 8 bytes") {
    static_assert(std::is_trivially_copyable_v<BufferHandle> && sizeof(BufferHandle) == 8);
    static_assert(std::is_trivially_copyable_v<TextureHandle> && sizeof(TextureHandle) == 8);
    static_assert(std::is_trivially_copyable_v<SamplerHandle> && sizeof(SamplerHandle) == 8);
    static_assert(std::is_trivially_copyable_v<ShaderHandle> && sizeof(ShaderHandle) == 8);
    static_assert(std::is_trivially_copyable_v<GraphicsPipelineHandle> && sizeof(GraphicsPipelineHandle) == 8);
    static_assert(std::is_trivially_copyable_v<SwapchainHandle> && sizeof(SwapchainHandle) == 8);
    static_assert(std::is_trivially_copyable_v<CommandBufferHandle> && sizeof(CommandBufferHandle) == 8);
    static_assert(std::is_trivially_copyable_v<RenderPassHandle> && sizeof(RenderPassHandle) == 8);
    CHECK(true);
}

TEST_CASE("rhi handles: default-constructed handles are invalid") {
    static_assert(!BufferHandle{}.valid());
    static_assert(!TextureHandle{}.valid());
    static_assert(!SamplerHandle{}.valid());
    static_assert(!ShaderHandle{}.valid());
    static_assert(!GraphicsPipelineHandle{}.valid());
    static_assert(!SwapchainHandle{}.valid());
    static_assert(!CommandBufferHandle{}.valid());
    static_assert(!RenderPassHandle{}.valid());
    CHECK(true);
}

TEST_CASE("rhi handles: equality compares index and generation") {
    static_assert(BufferHandle{1, 1} == BufferHandle{1, 1});
    static_assert(BufferHandle{1, 1} != BufferHandle{1, 2});
    static_assert(BufferHandle{1, 1} != BufferHandle{2, 1});
    CHECK(true);
}

// -------------------------------------------------------------------------------------------
// Group B — descriptors/PODs are aggregates and trivially copyable (AC-6)
// -------------------------------------------------------------------------------------------

TEST_CASE("rhi types: shared POD value types are aggregate + trivially copyable") {
    static_assert(std::is_aggregate_v<Extent2D> && std::is_trivially_copyable_v<Extent2D>);
    static_assert(std::is_aggregate_v<Rect> && std::is_trivially_copyable_v<Rect>);
    static_assert(std::is_aggregate_v<Viewport> && std::is_trivially_copyable_v<Viewport>);
    static_assert(std::is_aggregate_v<Color> && std::is_trivially_copyable_v<Color>);
    static_assert(std::is_aggregate_v<TextureSamplerBinding> && std::is_trivially_copyable_v<TextureSamplerBinding>);
    CHECK(true);
}

TEST_CASE("rhi descriptors: every descriptor struct is aggregate + trivially copyable") {
    static_assert(std::is_aggregate_v<DeviceDesc> && std::is_trivially_copyable_v<DeviceDesc>);
    static_assert(std::is_aggregate_v<SwapchainDesc> && std::is_trivially_copyable_v<SwapchainDesc>);
    static_assert(std::is_aggregate_v<BufferDesc> && std::is_trivially_copyable_v<BufferDesc>);
    static_assert(std::is_aggregate_v<TextureDesc> && std::is_trivially_copyable_v<TextureDesc>);
    static_assert(std::is_aggregate_v<SamplerDesc> && std::is_trivially_copyable_v<SamplerDesc>);
    static_assert(std::is_aggregate_v<ShaderDesc> && std::is_trivially_copyable_v<ShaderDesc>);
    static_assert(std::is_aggregate_v<VertexBufferLayout> && std::is_trivially_copyable_v<VertexBufferLayout>);
    static_assert(std::is_aggregate_v<VertexAttribute> && std::is_trivially_copyable_v<VertexAttribute>);
    static_assert(std::is_aggregate_v<RasterizerState> && std::is_trivially_copyable_v<RasterizerState>);
    static_assert(std::is_aggregate_v<StencilOpState> && std::is_trivially_copyable_v<StencilOpState>);
    static_assert(std::is_aggregate_v<DepthStencilState> && std::is_trivially_copyable_v<DepthStencilState>);
    static_assert(std::is_aggregate_v<BlendState> && std::is_trivially_copyable_v<BlendState>);
    static_assert(std::is_aggregate_v<ColorTargetDesc> && std::is_trivially_copyable_v<ColorTargetDesc>);
    static_assert(std::is_aggregate_v<GraphicsPipelineDesc> && std::is_trivially_copyable_v<GraphicsPipelineDesc>);
    static_assert(std::is_aggregate_v<ColorAttachment> && std::is_trivially_copyable_v<ColorAttachment>);
    static_assert(std::is_aggregate_v<DepthStencilAttachment> && std::is_trivially_copyable_v<DepthStencilAttachment>);
    static_assert(std::is_aggregate_v<RenderPassDesc> && std::is_trivially_copyable_v<RenderPassDesc>);
    CHECK(true);
}

TEST_CASE("rhi device: SwapchainTexture is aggregate + trivially copyable") {
    static_assert(std::is_aggregate_v<SwapchainTexture> && std::is_trivially_copyable_v<SwapchainTexture>);
    CHECK(true);
}

// -------------------------------------------------------------------------------------------
// Group C — designated-init smoke (AC-6)
// -------------------------------------------------------------------------------------------

TEST_CASE("rhi descriptors: TextureDesc designated init holds set fields and untouched defaults") {
    const TextureDesc desc{
        .format = TextureFormat::RGBA8Unorm, .usage = TextureUsage::Sampler, .width = 4, .height = 4};
    // Extra parens (the doctest-documented escape hatch): TextureFormat has its own ADL-visible
    // engine::rhi::toString(TextureFormat), which collides with doctest's unqualified stringify
    // lookup during CHECK's expression decomposition and fails to compile undecomposed. Wrapping
    // in a second set of parens evaluates the comparison as a plain bool instead.
    CHECK((desc.format == TextureFormat::RGBA8Unorm));
    CHECK(desc.usage == TextureUsage::Sampler);
    CHECK(desc.width == 4);
    CHECK(desc.height == 4);
    CHECK(desc.mipLevels == 1);
    CHECK(desc.sampleCount == SampleCount::One);
}

TEST_CASE("rhi descriptors: BufferDesc designated init holds combined usage flags") {
    const BufferDesc desc{.usage = BufferUsage::Vertex | BufferUsage::Index, .size = 256};
    CHECK(has(desc.usage, BufferUsage::Vertex));
    CHECK(has(desc.usage, BufferUsage::Index));
    CHECK(desc.size == 256);
}

// -------------------------------------------------------------------------------------------
// Group D — D16 convention defaults (AC-7). No descriptor has operator==; compare field-by-field
// (types.hpp PODs DO have == and may use it).
// -------------------------------------------------------------------------------------------

TEST_CASE("rhi defaults: RasterizerState{} is back-cull/CCW/fill/no-bias/depth-clip-on") {
    const RasterizerState state{};
    CHECK(state.fillMode == FillMode::Fill);
    CHECK(state.cullMode == CullMode::Back);
    CHECK(state.frontFace == FrontFace::CounterClockwise);
    CHECK_FALSE(state.enableDepthBias);
    CHECK(state.depthBiasConstant == 0.0F);
    CHECK(state.depthBiasClamp == 0.0F);
    CHECK(state.depthBiasSlope == 0.0F);
    CHECK(state.enableDepthClip);
}

TEST_CASE("rhi defaults: Viewport{} depth range is [0,1]") {
    const Viewport viewport{};
    CHECK(viewport.x == 0.0F);
    CHECK(viewport.y == 0.0F);
    CHECK(viewport.width == 0.0F);
    CHECK(viewport.height == 0.0F);
    CHECK(viewport.minDepth == 0.0F);
    CHECK(viewport.maxDepth == 1.0F);
}

TEST_CASE("rhi defaults: Color{} is opaque black") {
    const Color color{};
    CHECK(color.r == 0.0F);
    CHECK(color.g == 0.0F);
    CHECK(color.b == 0.0F);
    CHECK(color.a == 1.0F);
}

TEST_CASE("rhi defaults: DepthStencilAttachment{} clears depth to 1.0") {
    const DepthStencilAttachment attachment{};
    CHECK(attachment.clearDepth == 1.0F);
    CHECK(attachment.depthLoadOp == LoadOp::Clear);
    CHECK(attachment.depthStoreOp == StoreOp::DontCare);
    CHECK(attachment.stencilLoadOp == LoadOp::DontCare);
    CHECK(attachment.stencilStoreOp == StoreOp::DontCare);
    CHECK(attachment.clearStencil == 0);
    CHECK_FALSE(attachment.texture.valid());
}

TEST_CASE("rhi defaults: DepthStencilState{} compare is Less, everything disabled") {
    const DepthStencilState state{};
    CHECK(state.compareOp == CompareOp::Less);
    CHECK_FALSE(state.enableDepthTest);
    CHECK_FALSE(state.enableDepthWrite);
    CHECK_FALSE(state.enableStencilTest);
    CHECK(state.frontStencil.failOp == StencilOp::Keep);
    CHECK(state.frontStencil.passOp == StencilOp::Keep);
    CHECK(state.frontStencil.depthFailOp == StencilOp::Keep);
    CHECK(state.frontStencil.compareOp == CompareOp::Always);
    CHECK(state.backStencil.failOp == StencilOp::Keep);
    CHECK(state.backStencil.passOp == StencilOp::Keep);
    CHECK(state.backStencil.depthFailOp == StencilOp::Keep);
    CHECK(state.backStencil.compareOp == CompareOp::Always);
    CHECK(state.stencilCompareMask == 0xFF);
    CHECK(state.stencilWriteMask == 0xFF);
}

TEST_CASE("rhi defaults: StencilOpState{} is all-Keep/Always") {
    const StencilOpState state{};
    CHECK(state.failOp == StencilOp::Keep);
    CHECK(state.passOp == StencilOp::Keep);
    CHECK(state.depthFailOp == StencilOp::Keep);
    CHECK(state.compareOp == CompareOp::Always);
}

TEST_CASE("rhi defaults: GraphicsPipelineDesc{} is triangle-list with no depth target") {
    const GraphicsPipelineDesc desc{};
    CHECK(desc.primitiveType == PrimitiveType::TriangleList);
    CHECK((desc.depthStencilFormat == TextureFormat::Invalid));  // see the extra-parens note above
    CHECK(desc.sampleCount == SampleCount::One);
    CHECK(desc.vertexBuffers.empty());
    CHECK(desc.vertexAttributes.empty());
    CHECK(desc.colorTargets.empty());
    CHECK_FALSE(desc.vertexShader.valid());
    CHECK_FALSE(desc.fragmentShader.valid());
}

TEST_CASE("rhi defaults: SwapchainDesc{} is Vsync") {
    const SwapchainDesc desc{};
    CHECK(desc.presentMode == PresentMode::Vsync);
}

TEST_CASE("rhi defaults: SamplerDesc{} is linear/repeat with an effectively unclamped maxLod") {
    const SamplerDesc desc{};
    CHECK(desc.minFilter == Filter::Linear);
    CHECK(desc.magFilter == Filter::Linear);
    CHECK(desc.mipmapMode == MipmapMode::Linear);
    CHECK(desc.addressU == AddressMode::Repeat);
    CHECK(desc.addressV == AddressMode::Repeat);
    CHECK(desc.addressW == AddressMode::Repeat);
    CHECK(desc.mipLodBias == 0.0F);
    CHECK(desc.minLod == 0.0F);
    CHECK(desc.maxLod >= 1000.0F);
    CHECK_FALSE(desc.enableAnisotropy);
    CHECK(desc.maxAnisotropy == 1.0F);
    CHECK_FALSE(desc.enableCompare);
    CHECK(desc.compareOp == CompareOp::Less);
}

TEST_CASE("rhi defaults: BlendState{} is disabled with One/Zero+Add and mask All") {
    const BlendState state{};
    CHECK_FALSE(state.enableBlend);
    CHECK(state.srcColorFactor == BlendFactor::One);
    CHECK(state.dstColorFactor == BlendFactor::Zero);
    CHECK(state.colorOp == BlendOp::Add);
    CHECK(state.srcAlphaFactor == BlendFactor::One);
    CHECK(state.dstAlphaFactor == BlendFactor::Zero);
    CHECK(state.alphaOp == BlendOp::Add);
    CHECK(state.writeMask == ColorWriteMask::All);
}

TEST_CASE("rhi defaults: ColorAttachment{} clears to Color{} and stores") {
    const ColorAttachment attachment{};
    CHECK(attachment.loadOp == LoadOp::Clear);
    CHECK(attachment.storeOp == StoreOp::Store);
    CHECK(attachment.clearColor == Color{});
    CHECK_FALSE(attachment.texture.valid());
}

TEST_CASE("rhi defaults: ColorTargetDesc{} format defaults to the reject value") {
    const ColorTargetDesc desc{};
    CHECK((desc.format == TextureFormat::Invalid));  // see the extra-parens note above
}

TEST_CASE("rhi defaults: RenderPassDesc{} has no attachments and no depth") {
    const RenderPassDesc desc{};
    CHECK(desc.colorAttachments.empty());
    CHECK_FALSE(desc.depthStencil.has_value());
}

TEST_CASE("rhi defaults: DeviceDesc{} debug layer on, low-power off, Auto driver") {
    const DeviceDesc desc{};
    CHECK(desc.enableDebugLayer);
    CHECK_FALSE(desc.preferLowPower);
    CHECK(desc.driver == DriverPreference::Auto);
}

TEST_CASE("rhi defaults: BufferDesc{}/TextureDesc{} required fields default to reject values") {
    const BufferDesc bufferDesc{};
    CHECK(bufferDesc.usage == BufferUsage::None);
    CHECK(bufferDesc.size == 0);

    const TextureDesc textureDesc{};
    CHECK((textureDesc.format == TextureFormat::Invalid));  // see the extra-parens note above
    CHECK(textureDesc.usage == TextureUsage::None);
    CHECK(textureDesc.width == 0);
    CHECK(textureDesc.height == 0);
}

TEST_CASE("rhi defaults: SwapchainTexture{} is invalid with a zero extent") {
    const SwapchainTexture texture{};
    CHECK_FALSE(texture.texture.valid());
    CHECK(texture.extent == Extent2D{});
}

TEST_CASE("rhi defaults: VertexBufferLayout{}/VertexAttribute{} zero out") {
    const VertexBufferLayout layout{};
    CHECK(layout.inputRate == VertexInputRate::Vertex);
    CHECK(layout.pitch == 0);

    const VertexAttribute attribute{};
    CHECK(attribute.format == VertexFormat::Float4);
    CHECK(attribute.location == 0);
    CHECK(attribute.bufferSlot == 0);
    CHECK(attribute.offset == 0);
}

// -------------------------------------------------------------------------------------------
// Group E — flag enums: constexpr |, &, ~, has(); None == 0 (AC-8)
// -------------------------------------------------------------------------------------------

TEST_CASE("rhi flags: None is zero for all three flag enums") {
    static_assert(static_cast<std::uint8_t>(TextureUsage::None) == 0);
    static_assert(static_cast<std::uint8_t>(BufferUsage::None) == 0);
    static_assert(static_cast<std::uint8_t>(ColorWriteMask::None) == 0);
    CHECK(true);
}

TEST_CASE("rhi flags: operator| combines bits queryable via has()") {
    static_assert(has(TextureUsage::Sampler | TextureUsage::ColorTarget, TextureUsage::Sampler));
    static_assert(has(TextureUsage::Sampler | TextureUsage::ColorTarget, TextureUsage::ColorTarget));
    static_assert(has(BufferUsage::Vertex | BufferUsage::Index, BufferUsage::Vertex));
    static_assert(has(BufferUsage::Vertex | BufferUsage::Index, BufferUsage::Index));
    CHECK(true);
}

TEST_CASE("rhi flags: operator& isolates the shared bit") {
    static_assert(((TextureUsage::Sampler | TextureUsage::ColorTarget) & TextureUsage::Sampler) ==
                  TextureUsage::Sampler);
    CHECK(true);
}

TEST_CASE("rhi flags: operator~ stays inside the defined mask") {
    static_assert(static_cast<std::uint8_t>(~TextureUsage::None) == 0x07U);
    static_assert(static_cast<std::uint8_t>(~BufferUsage::None) == 0x03U);
    static_assert(static_cast<std::uint8_t>(~ColorWriteMask::None) == static_cast<std::uint8_t>(ColorWriteMask::All));
    static_assert((TextureUsage::Sampler & ~TextureUsage::Sampler) == TextureUsage::None);
    CHECK(true);
}

TEST_CASE("rhi flags: has() is false when the bit is absent") {
    static_assert(!has(TextureUsage::Sampler, TextureUsage::ColorTarget));
    CHECK(true);
}

TEST_CASE("rhi flags: ColorWriteMask::All is exactly R|G|B|A") {
    static_assert(ColorWriteMask::All ==
                  (ColorWriteMask::R | ColorWriteMask::G | ColorWriteMask::B | ColorWriteMask::A));
    static_assert(has(ColorWriteMask::All & ~ColorWriteMask::R, ColorWriteMask::G));
    static_assert(has(ColorWriteMask::All & ~ColorWriteMask::R, ColorWriteMask::B));
    static_assert(has(ColorWriteMask::All & ~ColorWriteMask::R, ColorWriteMask::A));
    static_assert(!has(ColorWriteMask::All & ~ColorWriteMask::R, ColorWriteMask::R));
    CHECK(true);
}

// -------------------------------------------------------------------------------------------
// Group F — Device class shape via type traits only (AC-5). No member is ever called, addressed,
// or captured — that would odr-use it and fail to link (Device's members are declared, not
// defined, until task 0.4.2).
// -------------------------------------------------------------------------------------------

TEST_CASE("rhi Device: move-only, no default constructor (traits only, never called)") {
    static_assert(!std::is_copy_constructible_v<Device>);
    static_assert(!std::is_copy_assignable_v<Device>);
    static_assert(std::is_move_constructible_v<Device>);
    static_assert(std::is_move_assignable_v<Device>);
    static_assert(!std::is_default_constructible_v<Device>);
    CHECK(true);
}

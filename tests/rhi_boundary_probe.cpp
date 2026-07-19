// Aero Engine — the RHI SDL_GPU-boundary COMPILE-TIME guard (task 0.4.5; ADR-002 "the sacred
// wrapper").
//
// THIS FILE ASSERTS BY EXISTING. It is not a doctest suite and has no TEST_CASE: the assertion is
// that it COMPILES. Its target (aero_rhi_boundary_probe, tests/CMakeLists.txt) links ONLY aero::rhi,
// which links SDL3 PRIVATE -- and whose PUBLIC deps aero::core/aero::platform propagate no vcpkg
// header -- so vcpkg's shared per-triplet include/ root never reaches this compile line and SDL_GPU
// is genuinely unreachable here. If any public rhi header ever starts including <SDL3/SDL_gpu.h>
// (or any <SDL3/...>), THIS TU fails to compile the moment the leak is written.
//
// aero_tests CANNOT do this: it links doctest AND SDL3 directly and inherits the whole shared vcpkg
// include root, so SDL_GPU would resolve there regardless of what aero_rhi links (risk R12,
// docs/08-risks.md). The grep guard (.github/scripts/check-rhi-boundary.sh) is the enforcement that
// reaches tests/ and every engine/runtime .cpp; THIS probe holds the public-header compile boundary.
//
// KEEP THIS TARGET DEPENDENCY-FREE -- see tests/CMakeLists.txt. Adding doctest, aero::profiling
// (Tracy, a vcpkg package, in Release), SDL3::SDL3, or any vcpkg package restores the shared include
// root and silently reduces this guard to a no-op WHILE CI STAYS GREEN -- the one way it can rot.

#include <aero/rhi/rhi.hpp>  // the umbrella: descriptors + device + format + handles + types

#include <type_traits>

// Handles -- the phantom-tag aliases (identity + invalid-by-default).
static_assert(!engine::rhi::BufferHandle{}.valid());
static_assert(!engine::rhi::TextureHandle{}.valid());
static_assert(!engine::rhi::SwapchainHandle{}.valid());
static_assert(!engine::rhi::CommandBufferHandle{}.valid());
static_assert(!engine::rhi::RenderPassHandle{}.valid());

// Enums & flag operators (types.hpp) -- force the constexpr operator bodies to evaluate.
static_assert(engine::rhi::has(engine::rhi::TextureUsage::Sampler | engine::rhi::TextureUsage::ColorTarget,
                               engine::rhi::TextureUsage::Sampler));
static_assert(!engine::rhi::has(engine::rhi::BufferUsage::Vertex, engine::rhi::BufferUsage::Index));
static_assert((engine::rhi::ColorWriteMask::All & engine::rhi::ColorWriteMask::R) == engine::rhi::ColorWriteMask::R);
static_assert(engine::rhi::MAX_COLOR_ATTACHMENTS == 8U);

// POD value types (types.hpp) -- the pinned defaults (D16) + the trivially-copyable contract.
static_assert(engine::rhi::Extent2D{}.width == 0U);
static_assert(engine::rhi::Viewport{}.maxDepth == 1.0F);
static_assert(engine::rhi::Color{}.a == 1.0F);
static_assert(std::is_trivially_copyable_v<engine::rhi::TextureSamplerBinding>);

// Descriptors (descriptors.hpp) -- aggregates carrying the ADR-005 defaults (D16).
static_assert(std::is_aggregate_v<engine::rhi::DeviceDesc>);
static_assert(engine::rhi::DeviceDesc{}.enableDebugLayer);
static_assert(engine::rhi::SwapchainDesc{}.presentMode == engine::rhi::PresentMode::Vsync);
static_assert(engine::rhi::RasterizerState{}.cullMode == engine::rhi::CullMode::Back);
static_assert(engine::rhi::RasterizerState{}.frontFace == engine::rhi::FrontFace::CounterClockwise);
static_assert(engine::rhi::DepthStencilAttachment{}.clearDepth == 1.0F);
static_assert(std::is_trivially_copyable_v<engine::rhi::GraphicsPipelineDesc>);
static_assert(std::is_trivially_copyable_v<engine::rhi::RenderPassDesc>);

// Device (device.hpp) -- force the class COMPLETE via traits that also assert its move-only contract.
// (Its members are odr-usable at LINK time only; naming a trait needs only the complete type, which
// is exactly what pulls the header's full definition -- and any leaked SDL include -- into this TU.)
static_assert(!std::is_copy_constructible_v<engine::rhi::Device>);
static_assert(std::is_move_constructible_v<engine::rhi::Device>);
static_assert(!std::is_copy_assignable_v<engine::rhi::Device>);
static_assert(std::is_trivially_copyable_v<engine::rhi::SwapchainTexture>);

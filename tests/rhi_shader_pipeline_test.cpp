// Aero Engine — rhi shader-to-pipeline tests, tier 1 (task 0.4.4; AC-3, AC-4, AC-5, AC-6, E12). The
// first REAL bytecode through Device::createShader/createGraphicsPipeline: loads the checked-in
// triangle pair (0.4.3's seed shaders) via the loader under test and proves the pair builds a VALID
// GraphicsPipeline on this host's native backend — Metal (macOS), D3D12/WARP (Windows), or
// Vulkan/lavapipe (Linux). On Windows this pipeline creation IS the E12 verdict: SDL_shadercross's
// DXIL is only fourcc-checked at createShader (F6) — the D3D12 runtime validates/signs it at
// pipeline-state-object creation, which is exactly what this test exercises. No draw (D7) — that
// stays Epic 0.5.x's deliverable.
//
// The whole TU compiles to nothing when AERO_SHADER_TOOLS is OFF (no aero_shaders artifacts exist to
// load, spec E13); when ON, each case still gates on GPU availability via AERO_SKIP_OR_FAIL, mirroring
// rhi_device_test.cpp / rhi_swapchain_test.cpp's tier-1 idiom.
#if AERO_SHADER_TOOLS_ENABLED

    #include <aero/core/vfs.hpp>
    #include <aero/platform/context.hpp>
    #include <aero/rhi/rhi.hpp>
    #include <aero/rhi/shader_loader.hpp>

    #include "rhi_test_support.hpp"

    #include <doctest/doctest.h>

    #include <memory>
    // <ostream> is load-bearing on MSVC (see rhi_device_test.cpp's comment).
    #include <ostream>
    #include <span>

// docs/04 forbids `using namespace` in HEADERS; this is a test TU (rhi_types_test.cpp precedent).
using namespace engine::rhi;

TEST_CASE("rhi shader: load triangle pair → valid pipeline (AC-3, E12)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    engine::VirtualFileSystem vfs;
    // res:// -> build/<preset>/shaders, where aero_add_shaders wrote the triangle pair (AERO_SHADERS_DIR
    // MUST match aero_add_shaders' OUTPUT_DIR — the invariant tests/CMakeLists.txt documents).
    vfs.mount(std::make_unique<engine::DirectoryBackend>(AERO_SHADERS_DIR));

    const ShaderHandle vs = loadShader(*dev, vfs, "res://triangle.vert");
    const ShaderHandle fs = loadShader(*dev, vfs, "res://triangle.frag");
    REQUIRE(vs.valid());  // createShader success on every backend (Windows: DXIL fourcc OK, F6)
    REQUIRE(fs.valid());

    const ColorTargetDesc color{.format = TextureFormat::RGBA8Unorm};  // universal render target (F10)
    const GraphicsPipelineDesc pd{
        .vertexShader = vs,
        .fragmentShader = fs,
        .primitiveType = PrimitiveType::TriangleList,  // vertex-pulling: no vertex buffers/attributes (F11)
        .colorTargets = std::span{&color, 1},          // `color` must outlive this call (borrowed span)
    };
    const GraphicsPipelineHandle pipe = dev->createGraphicsPipeline(pd);
    CHECK(pipe.valid());  // THE deliverable; on Windows/WARP THIS is the unsigned-DXIL verdict (F6/AC-4)
}

TEST_CASE("rhi shader: absent base path → invalid handle, no crash (AC-5)") {
    const engine::platform::Context ctx{{.headless = false}};
    if (!ctx.valid()) {
        AERO_SKIP_OR_FAIL("no real video driver available");
    }
    auto dev = Device::create();
    if (!dev.has_value()) {
        AERO_SKIP_OR_FAIL("no GPU device available");
    }

    const engine::VirtualFileSystem vfs;  // no mounts at all: every base path is absent
    const ShaderHandle handle = loadShader(*dev, vfs, "res://does-not-exist");
    CHECK_FALSE(handle.valid());
}

#endif  // AERO_SHADER_TOOLS_ENABLED

#pragma once
// engine/render/src/debug_draw_pack.hpp — task E.1.1: the two push-uniform blocks, src-private.
// The tonemap_pack.hpp / skinning_pack.hpp shape: one header, inline, no state, and a static_assert
// block pinning the vertex layouts to the LITERALS the pipeline descriptors use -- so a field added
// to either vertex type fails to compile HERE rather than drawing garbage on every backend at once.
//
// PRIVATE to engine/render (src/, never installed, never in the public include tree), reached from
// tests/ by a relative include, exactly as tonemap_pack.hpp and skinning_pack.hpp are.
//
// The CPU mirrors of the TWO cbuffers in the debug pair, under the 0.4.3 binding law
// (vertex t/s -> space0, vertex UBOs -> space1):
//
//   shaders/debug_line.vert.hlsl      : cbuffer DebugView : register(b0, space1)
//        float4x4 uViewProj;                                              // 64 bytes
//   shaders/debug_billboard.vert.hlsl : cbuffer DebugBillboardView : register(b0, space1)
//        float4x4 uViewProj; float2 uViewportPx; float2 _pad0;            // 80 bytes
//
// FIELD ORDER MUST MATCH THE HLSL EXACTLY: a mismatch here neither fails to compile nor fails to
// submit.

#include <aero/core/math.hpp>
#include <aero/render/debug_draw.hpp>
#include <aero/rhi/types.hpp>  // Extent2D

#include <array>
#include <cstddef>
#include <cstring>  // std::memcpy -- MSVC's STL does not supply <cstring> transitively

namespace engine::render {

// std140: a float4x4 is 64 bytes and needs no padding.
inline constexpr std::size_t DEBUG_LINE_VERTEX_UNIFORM_BYTES = 64;
// std140: 64 for the matrix + a float2 at 64 + 8 bytes of explicit padding to the 16-byte boundary.
// The shader declares the padding as `float2 _pad0`, so the two agree by construction.
inline constexpr std::size_t DEBUG_BILLBOARD_VERTEX_UNIFORM_BYTES = 80;

// THE LAYOUT PINS. These literals are what debug_draw.cpp's VertexBufferLayout::pitch and
// VertexAttribute::offset fields spell, and this is the one place the C++ types and those numbers
// meet. DD1 asserts the same facts at runtime; these assert them at compile time, in the file that
// USES them.
static_assert(sizeof(DebugLineVertex) == 16);
static_assert(offsetof(DebugLineVertex, position) == 0);
static_assert(offsetof(DebugLineVertex, rgba) == 12);
static_assert(sizeof(DebugBillboardVertex) == 36);
static_assert(offsetof(DebugBillboardVertex, center) == 0);
static_assert(offsetof(DebugBillboardVertex, corner) == 12);
static_assert(offsetof(DebugBillboardVertex, uv) == 20);
static_assert(offsetof(DebugBillboardVertex, rgba) == 28);
static_assert(offsetof(DebugBillboardVertex, sizePx) == 32);
// What the two memcpys below rest on: 16 contiguous floats, column-major, no padding (mat4.hpp's
// own static_asserts say the same thing one layer down; this one is in the file that COPIES it).
static_assert(sizeof(Mat4) == 64);

// Column-major, no transpose -- the engine's Mat4 upload is a straight memcpy through data()
// (mat4.hpp), and the HLSL declares float4x4 with the matching majorness. BOTH packers ZERO THEIR
// BUFFER FIRST, so no padding byte is ever indeterminate on the wire (DD21 compares the tail).
[[nodiscard]] inline std::array<std::byte, DEBUG_LINE_VERTEX_UNIFORM_BYTES> packDebugLineView(
    const Mat4& viewProj) noexcept {
    std::array<std::byte, DEBUG_LINE_VERTEX_UNIFORM_BYTES> block{};  // value-init: every byte zero
    std::memcpy(block.data(), viewProj.data(), sizeof(Mat4));
    return block;
}

[[nodiscard]] inline std::array<std::byte, DEBUG_BILLBOARD_VERTEX_UNIFORM_BYTES> packDebugBillboardView(
    const Mat4& viewProj, rhi::Extent2D viewportPx) noexcept {
    std::array<std::byte, DEBUG_BILLBOARD_VERTEX_UNIFORM_BYTES> block{};
    std::memcpy(block.data(), viewProj.data(), sizeof(Mat4));
    const std::array<float, 2> extent{static_cast<float>(viewportPx.width), static_cast<float>(viewportPx.height)};
    std::memcpy(block.data() + sizeof(Mat4), extent.data(), sizeof(extent));
    // Bytes 72..79 stay zero: the shader's `float2 _pad0`.
    return block;
}

}  // namespace engine::render

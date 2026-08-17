#pragma once
// engine/render/src/skinning_pack.hpp — task 3.5.1. PRIVATE to engine/render (src/, never installed,
// never in the public include tree) — the material_pack.hpp / primitives.hpp precedent, reached from
// tests/ by a relative include.
//
// It lives beside material_pack.hpp for the reason that header states in full at its own :9-14: A
// FILE-LOCAL PACKER IS UNFALSIFIABLE. A palette row extraction that transposes, drops a column or
// reorders the three rows still records a frame and still draws a lit, moving mesh — merely a wrong
// one — which is exactly the "plausible garbage" class. So it is a named function with a header, and
// its test pins the extraction LITERAL BY LITERAL against an asymmetric matrix rather than against
// the mapping applied to itself.
//
// THE MAPPING: three row-major float4 rows per joint, 48 bytes each, matching
// shaders/scene_skinned.vert.hlsl's `float4 uPaletteRows[255]`.
//   rows[3i + k] = row k of the COLUMN-MAJOR palette[i]
//                = { m.columns[0][k], m.columns[1][k], m.columns[2][k], m.columns[3][k] }
// i.e. the transpose's first three rows. The affine bottom row (0,0,0,1) is dropped and the shader
// reconstructs nothing: a skinning matrix is affine by construction (a product of TRS composites and
// an inverse bind matrix), so its fourth row is never anything else.

#include <aero/core/math.hpp>

#include <cstddef>
#include <span>

namespace engine::render::detail {

// rows.size() must be >= 3 * palette.size(); the caller owns the buffer and ZEROES it first (the
// renderer pushes the FULL 4080-byte block every skinned draw, so unused tail rows must be zero
// rather than whatever the last draw left there). Writes exactly 3 * palette.size() entries and
// never touches the tail. An undersized `rows` is filled as far as it goes rather than overrun.
void packJointPaletteRows(std::span<const Mat4> palette, std::span<Vec4> rows);

}  // namespace engine::render::detail

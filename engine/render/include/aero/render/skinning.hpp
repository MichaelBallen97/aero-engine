#pragma once
// Aero Engine — the runtime skeleton surface (task 3.5.1): pure pose math over the PARSED assets
// type. assets::CookedSkeleton IS the runtime skeleton — there is no mirror struct here, exactly as
// texture_upload.hpp consumes assets::CookedTextureView rather than a copy of it. aero_render already
// links PUBLIC aero::assets (3.4.1's edge), so naming that type here adds no link line, no include
// directory and no dependency of any kind.
//
// PURE and GPU-FREE: no rhi type, no device call, no allocation, no logging, no recursion, no
// per-OS macro. Turning a palette into the bytes a shader reads lives in src-private
// skinning_pack.hpp; binding them lives in ForwardRenderer. This header is the vocabulary only.

#include <aero/assets/cooked_skeleton.hpp>
#include <aero/core/math.hpp>

#include <cstdint>
#include <span>

namespace engine::render {

// THE MEASURED PALETTE BUDGET, not a guessed one. SDL's Vulkan backend binds every push-uniform
// descriptor with a FIXED 4096-byte range: MAX_UBO_SECTION_SIZE is #define'd to 4096 ("// 4 KiB") at
// vcpkg/buildtrees/sdl3/src/ase-3.4.12-441a9855e8.clean/src/gpu/vulkan/SDL_gpu_vulkan.c:71, and every
// bufferInfos[i].range is set to it (:5300, :5419, :8671) — so bytes past 4096 are memcpy'd into the
// uniform ring and NEVER visible to a Vulkan shader, while D3D12 and Metal are bounded instead by the
// 32 KiB block (UNIFORM_BUFFER_SIZE 32768, src/gpu/SDL_sysgpu.h:35) and show the full data. That is a
// silent cross-backend divergence, so the portable per-slot ceiling is 4096 bytes and the engine
// adopts it uniformly. Three row-major float4 rows per affine joint matrix is 48 bytes per joint, and
// floor(4096 / 48) = 85. (The MAX_TEXTURE_DIMENSION_2D justified-constant style: the value and the
// line numbers are recorded HERE because `vcpkg clean` wipes the buildtrees path that proves them.)
//
// Raising it means a storage buffer rather than a push-uniform slot, which is an rhi surface change
// (BufferUsage has Vertex and Index only) and belongs to whoever needs more than 85 joints.
inline constexpr std::uint32_t MAX_SKINNING_JOINTS = 85;
static_assert(3ULL * MAX_SKINNING_JOINTS * 16ULL <= 4096ULL,
              "the packed palette must fit the measured portable push-uniform ceiling");

// The 3.5.2 seam, and the whole reason the format stores bind LOCALS as TRS rather than baked global
// matrices: an animation clip sampler writes these member-wise (a clip may drive rotation alone) and
// never touches a matrix. Field for field the bind-local half of assets::CookedSkeletonJoint.
struct JointPose {
    Vec3 translation{};
    Quat rotation = Quat::identity();
    Vec3 scale = Vec3::one();
};

// Fill `out` with the skeleton's own bind-local TRS, record for record. `out.size()` must equal
// skeleton.joints.size() (debug-asserted; a shorter span is filled as far as it goes rather than
// overrun). This is the starting pose every consumer poses AWAY from — a skeleton drawn straight out
// of bindPose reproduces the authored rest pose exactly.
void bindPose(const assets::CookedSkeleton& skeleton, std::span<JointPose> out);

// The matrix palette: one skinning matrix per palette slot, ready to pack and push.
//
//   global[i] = (parent == INVALID ? TRS(pose[i]) : global[parent] * TRS(pose[i]))
//   out[joint.paletteSlot] = global[i] * joint.inverseBind        // for palette records only
//
// ONE FORWARD PASS, no recursion and no visited set, because parents precede children BY FORMAT
// INVARIANT (docs/09 section 12.4, enforced by the parser as `parent < index`). Hierarchy-only
// records (paletteSlot == INVALID) contribute their transform to descendants and occupy no slot.
//
// pose.size() must equal skeleton.joints.size() and out.size() must equal
// skeleton.paletteJointCount (both debug-asserted; both clamped in release rather than overrun).
// Allocation-free: the retained per-record globals live in a fixed stack scratch bounded by the
// format's own cap, so this is callable every frame.
void computeJointPalette(const assets::CookedSkeleton& skeleton, std::span<const JointPose> pose, std::span<Mat4> out);

}  // namespace engine::render

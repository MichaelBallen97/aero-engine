// engine/render/src/skinning.cpp — task 3.5.1: the pure pose math behind skinning.hpp, plus the
// src-private palette row packer. Nothing here allocates, logs, recurses or touches a GPU; every
// entry point is total under a caller's bad sizes (clamped, with a debug assert naming the contract)
// because a per-frame path must never turn a caller's bookkeeping slip into a read.

#include <aero/render/skinning.hpp>

#include "skinning_pack.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>

namespace engine::render {

namespace {

// The ONE place a JointPose becomes a matrix. T * R * S, right-to-left (Mat4's own convention), via
// the engine's compose — never a hand-rolled quaternion-to-matrix here, because a second spelling of
// that conversion is a second place for a handedness mistake to live.
[[nodiscard]] Mat4 poseMatrix(const JointPose& pose) {
    return compose(Trs{.translation = pose.translation, .rotation = pose.rotation, .scale = pose.scale});
}

}  // namespace

void bindPose(const assets::CookedSkeleton& skeleton, std::span<JointPose> out) {
    assert(out.size() == skeleton.joints.size() && "bindPose: out must be sized to the skeleton's joint count");
    const std::size_t count = std::min(out.size(), skeleton.joints.size());
    for (std::size_t i = 0; i < count; ++i) {
        const assets::CookedSkeletonJoint& joint = skeleton.joints[i];
        out[i] = JointPose{.translation = joint.translation, .rotation = joint.rotation, .scale = joint.scale};
    }
}

void computeJointPalette(const assets::CookedSkeleton& skeleton, std::span<const JointPose> pose, std::span<Mat4> out) {
    assert(pose.size() == skeleton.joints.size() && "computeJointPalette: pose must be sized to the joint count");
    assert(out.size() == skeleton.paletteJointCount && "computeJointPalette: out must be sized to paletteJointCount");

    // Every record's global transform has to survive for its later children, and `out` only holds
    // the palette slots — so the pass needs its own retained buffer. A FIXED STACK ARRAY, never a
    // std::vector local: this runs every frame, and the format's own cap bounds it exactly. 1024
    // records x 64 bytes = 64 KiB, which every platform's default stack (>= 512 KiB on all three)
    // carries comfortably, and it is the smallest allocation-free realization there is.
    std::array<Mat4, assets::MAX_COOKED_SKELETON_JOINTS> globals;
    const std::size_t count = std::min({pose.size(), skeleton.joints.size(), globals.size()});

    for (std::size_t i = 0; i < count; ++i) {
        const assets::CookedSkeletonJoint& joint = skeleton.joints[i];
        const Mat4 local = poseMatrix(pose[i]);
        // `parent < i` is the format invariant the parser enforces, which is what makes this one
        // forward pass correct with no visited set. A hand-built skeleton that violates it is a
        // caller bug, not a file: it is asserted in debug and treated as a root in release, so a
        // forward reference can never become an out-of-range read of `globals`.
        assert((joint.parent == assets::COOKED_SKELETON_INVALID_INDEX || joint.parent < i) &&
               "computeJointPalette: joint records must be parents-before-children");
        const bool hasParent = joint.parent != assets::COOKED_SKELETON_INVALID_INDEX && joint.parent < i;
        globals[i] = hasParent ? globals[joint.parent] * local : local;

        if (joint.paletteSlot != assets::COOKED_SKELETON_INVALID_INDEX && joint.paletteSlot < out.size()) {
            // global THEN inverse bind: the inverse bind takes a vertex from its authored (bind)
            // space into joint space, and the global takes it from joint space to the world. The
            // reverse order composes a matrix that is wrong everywhere the bind pose is not identity.
            out[joint.paletteSlot] = globals[i] * joint.inverseBind;
        }
    }
}

namespace detail {

void packJointPaletteRows(std::span<const Mat4> palette, std::span<Vec4> rows) {
    assert(rows.size() >= 3 * palette.size() && "packJointPaletteRows: rows must hold three per joint");
    const std::size_t count = std::min(palette.size(), rows.size() / 3);
    for (std::size_t i = 0; i < count; ++i) {
        const Mat4& m = palette[i];
        // Row k of a COLUMN-MAJOR matrix gathers component k of all four columns. Written out per
        // row rather than through transpose() so the extraction is visible where it is read, and so
        // nothing computes the dropped fourth row.
        rows[(3 * i) + 0] = Vec4{m.columns[0].x, m.columns[1].x, m.columns[2].x, m.columns[3].x};
        rows[(3 * i) + 1] = Vec4{m.columns[0].y, m.columns[1].y, m.columns[2].y, m.columns[3].y};
        rows[(3 * i) + 2] = Vec4{m.columns[0].z, m.columns[1].z, m.columns[2].z, m.columns[3].z};
    }
}

}  // namespace detail

}  // namespace engine::render

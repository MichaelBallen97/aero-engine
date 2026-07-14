#pragma once
// Aero Engine — TRS composition, view and projection matrices (task 0.2.2, ADR-005).
// Public engine:: surface; ZERO GLM. compose/decompose/lookAt/perspective/ortho are defined in
// engine/core/src/math/glm_backend.cpp.

#include <aero/core/math/mat4.hpp>
#include <aero/core/math/quat.hpp>
#include <aero/core/math/vec3.hpp>

#include <array>

namespace engine {

// E12: named Trs, NOT Transform — engine::Transform is deliberately left free for the scene
// layer's ECS component (Phase 1/2). Squatting the name now would force a rename in a
// reflection-annotated type later, exactly when it is most expensive.
struct Trs {
    Vec3 translation;  // D14: {0, 0, 0}
    Quat rotation;     // D14: identity
    Vec3 scale = Vec3::one();
};

// D8: the translation lives in column 3.
constexpr Mat4 translation(Vec3 t) {
    return Mat4{std::array<Vec4, 4>{Vec4{1.0f, 0.0f, 0.0f, 0.0f}, Vec4{0.0f, 1.0f, 0.0f, 0.0f},
                                    Vec4{0.0f, 0.0f, 1.0f, 0.0f}, Vec4{t.x, t.y, t.z, 1.0f}}};
}

constexpr Mat4 scaling(Vec3 s) {
    return Mat4{std::array<Vec4, 4>{Vec4{s.x, 0.0f, 0.0f, 0.0f}, Vec4{0.0f, s.y, 0.0f, 0.0f},
                                    Vec4{0.0f, 0.0f, s.z, 0.0f}, Vec4{0.0f, 0.0f, 0.0f, 1.0f}}};
}

// T * R * S — right-to-left (D8): scale is applied first, then rotate, then translate.
// The rotation should be a UNIT quaternion; a non-unit one silently adds scale.
Mat4 compose(const Trs& trs);

// D9: TRS-ONLY. Returns false and leaves `out` untouched if a scale axis is zero/near-zero
// (docs/04: explicit status return, never an exception across the public API). Skew and
// perspective are NOT modelled — a matrix carrying them decomposes to nonsense, which is out of
// contract. This is why glm::decompose (experimental GTX, skew/perspective-aware) is unused.
bool decompose(const Mat4& m, Trs& out);

// D3/D4/D5. Right-handed view space (-Z forward, Y-up — glTF's convention), producing SDL_GPU's
// Z in [0,1] clip space (SDL_gpu.h "Coordinate System": "Z values range from [0.0, 1.0] where 0
// is the near plane"). fovY is in RADIANS (D6).
//
// ⚠ Do NOT flip Y for Vulkan. SDL_gpu.h: "If the backend driver differs from this convention
// (e.g. Vulkan, which has an NDC that assumes +Y is down), SDL will automatically convert the
// coordinate system behind the scenes." A proj[1][1] *= -1 here would DOUBLE-flip the image.
Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up);
Mat4 perspective(float fovY, float aspect, float zNear, float zFar);
Mat4 ortho(float left, float right, float bottom, float top, float zNear, float zFar);

}  // namespace engine

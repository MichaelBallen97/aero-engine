// Aero Engine -- the one script-layer source that names the Transform component (task 4.4.2): spawn's identity
// Transform and the transform helpers. BN30 holds every other source to the table.
//
// The helpers (worldPosition, forward, right, up, translate, rotate, lookAt) are methods of engine.Transform's
// prototype, installed by script_api.cpp from TRANSFORM_HELPERS. They read world space through worldMatrix and write
// the entity's own local Transform. Each is total: a degenerate lookAt leaves the rotation as it was, with no error
// and no record, and a core call that asserts on a bad value is reached only past a guard written so a NaN takes the
// safe branch.
//
// Order inside transformHelper (script_api.hpp rule 9): the receiver, then the kind of every argument, then every
// argument read out whole, then the entity's Transform, then the write. A refusal writes nothing.

#include <aero/core/math.hpp>
#include <aero/scene/transform.hpp>
#include <aero/scene/world.hpp>

#include "script_api.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace engine::script {
namespace {

// "engine.<Transform's script name>.<helper>": the name is the table's, never a literal; built only when thrown.
[[nodiscard]] std::string helperWhere(const ScriptApi& api, TransformOp op) {
    const std::span<const internal::ComponentBinding> components = api.table->components();
    std::string where = "engine." + std::string{components[api.transformComponent].scriptName};
    for (const TransformHelper& helper : TRANSFORM_HELPERS) {
        if (helper.op == op) {
            where += "." + std::string{helper.name};
        }
    }
    return where;
}

[[nodiscard]] Vec3 vec3Of(const std::array<float, 4>& values) noexcept { return Vec3{values[0], values[1], values[2]}; }
[[nodiscard]] Quat quatOf(const std::array<float, 4>& values) noexcept {
    return Quat{values[0], values[1], values[2], values[3]};
}
[[nodiscard]] std::array<float, 4> valuesOf(Vec3 v) noexcept { return {v.x, v.y, v.z, 0.0F}; }

// The rotation is left unchanged, with no error and no record, when the target is the entity's own world position
// (within EPSILON), when the direction is parallel to up (or up is zero), when anything is NaN, and when the parent's
// world rotation cannot be recovered. Otherwise -Z faces the target in world space. Every degenerate comparison is
// written `!(x > limit)`, so a NaN takes the no-op branch and never reaches normalize, toQuat or decompose.
void lookAtTotal(const World& world, Entity entity, Transform& transform, Vec3 target, Vec3 up) {
    const Vec3 eye = transformPoint(worldMatrix(world, entity), Vec3{});
    const Vec3 toTarget = target - eye;
    const float distanceSq = lengthSquared(toTarget);
    if (!(distanceSq > EPSILON * EPSILON)) {
        return;
    }
    const Vec3 back = -(toTarget / std::sqrt(distanceSq));  // +Z points away from the target
    const Vec3 side = cross(normalizeOrZero(up), back);
    const float sideSq = lengthSquared(side);
    if (!(sideSq > EPSILON * EPSILON)) {
        return;
    }
    const Vec3 right = side / std::sqrt(sideSq);
    const Vec3 trueUp = cross(back, right);
    const Quat worldRotation = normalizeOrIdentity(toQuat(Mat3{std::array<Vec3, 3>{right, trueUp, back}}));
    Quat parentRotation = Quat::identity();
    if (const Entity parent = world.parent(entity); parent.valid()) {
        Trs parentTrs;
        if (!decompose(worldMatrix(world, parent), parentTrs)) {
            return;
        }
        parentRotation = normalizeOrIdentity(parentTrs.rotation);
    }
    transform.rotation = normalizeOrIdentity(conjugate(parentRotation) * worldRotation);
}

// What the arguments of one call hold once they are read out: the first vector (or quaternion), and lookAt's up.
struct TransformInput {
    std::array<float, 4> first{};
    std::array<float, 4> second{};
};

[[nodiscard]] bool isVectorOf(const ScriptApi& api, JSValueConst value, int dimension) noexcept {
    return JS_GetOpaque(value, dimension == 3 ? api.vec3Class : api.quatClass) != nullptr;
}

// True when `value` is a vector of `dimension`; else a TypeError naming the helper is pending and this is false. The
// text is built only on the refusal: readVectorArgument raises it for a value that is not one.
[[nodiscard]] bool kindIsRight(JSContext* ctx, const ScriptApi& api, TransformOp op, JSValueConst value,
                               int dimension) {
    if (isVectorOf(api, value, dimension)) {
        return true;
    }
    std::array<float, 4> unused{};
    static_cast<void>(readVectorArgument(ctx, api, value, dimension, helperWhere(api, op), unused));
    return false;
}

// Checks the kind of every argument, then reads each out whole. False: a TypeError is pending and nothing has been
// written. A kind that was checked cannot fail a read with the helper's text (only a view of a dead entity fails,
// with its own), so the reads carry no site text and the success path builds no string.
[[nodiscard]] bool readTransformArguments(JSContext* ctx, const ScriptApi& api, TransformOp op, JSValueConst* argv,
                                          TransformInput& input) {
    const std::string noText;
    switch (op) {
        case TransformOp::WorldPosition:
        case TransformOp::Forward:
        case TransformOp::Right:
        case TransformOp::Up:
            return true;
        case TransformOp::Translate:
        case TransformOp::Rotate: {
            const int dimension = op == TransformOp::Translate ? 3 : 4;
            return kindIsRight(ctx, api, op, argv[0], dimension) &&
                   readVectorArgument(ctx, api, argv[0], dimension, noText, input.first);
        }
        case TransformOp::LookAt: {
            const bool hasUp = !JS_IsUndefined(argv[1]);  // an omitted up is world +Y
            if (!kindIsRight(ctx, api, op, argv[0], 3) || (hasUp && !kindIsRight(ctx, api, op, argv[1], 3))) {
                return false;
            }
            input.second = valuesOf(Vec3{0.0F, 1.0F, 0.0F});
            return readVectorArgument(ctx, api, argv[0], 3, noText, input.first) &&
                   (!hasUp || readVectorArgument(ctx, api, argv[1], 3, noText, input.second));
        }
    }
    return true;
}

// A read helper's result, or the mutator's effect (returns JS_UNDEFINED, which the caller replaces with the
// receiver). `transform` is the reference resolveStorage handed back in this same host function, after every argument
// read; no World write happens between it and an assignment (rule 9).
[[nodiscard]] JSValue applyTransformOp(JSContext* ctx, const ScriptApi& api, TransformOp op, const EntityRef& ref,
                                       Transform& transform, const TransformInput& input) {
    const World& world = *api.world;
    switch (op) {
        case TransformOp::WorldPosition:
            return newDetachedVector(ctx, api, 3, valuesOf(transformPoint(worldMatrix(world, ref.entity), Vec3{})));
        case TransformOp::Forward:
            return newDetachedVector(
                ctx, api, 3,
                valuesOf(normalizeOrZero(transformDirection(worldMatrix(world, ref.entity), Vec3{0.0F, 0.0F, -1.0F}))));
        case TransformOp::Right:
            return newDetachedVector(
                ctx, api, 3,
                valuesOf(normalizeOrZero(transformDirection(worldMatrix(world, ref.entity), Vec3{1.0F, 0.0F, 0.0F}))));
        case TransformOp::Up:
            return newDetachedVector(
                ctx, api, 3,
                valuesOf(normalizeOrZero(transformDirection(worldMatrix(world, ref.entity), Vec3{0.0F, 1.0F, 0.0F}))));
        case TransformOp::Translate:
            transform.position = transform.position + normalizeOrIdentity(transform.rotation) * vec3Of(input.first);
            return JS_UNDEFINED;
        case TransformOp::Rotate:
            transform.rotation = normalizeOrIdentity(transform.rotation * quatOf(input.first));
            return JS_UNDEFINED;
        case TransformOp::LookAt:
            lookAtTotal(world, ref.entity, transform, vec3Of(input.first), vec3Of(input.second));
            return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

[[nodiscard]] bool isReadOp(TransformOp op) noexcept {
    return op == TransformOp::WorldPosition || op == TransformOp::Forward || op == TransformOp::Right ||
           op == TransformOp::Up;
}

[[nodiscard]] JSValue foreignTransform(JSContext* ctx, const ScriptApi& api, TransformOp op) {
    const std::span<const internal::ComponentBinding> components = api.table->components();
    return throwTypeError(ctx, helperWhere(api, op) + ": the receiver is not an engine." +
                                   std::string{components[api.transformComponent].scriptName});
}

// The whole of one call, in the contract's order. Not noexcept: it builds strings, so the host function below keeps
// its throwing paths in a callee, as childrenArray does.
[[nodiscard]] JSValue runTransformHelper(JSContext* ctx, ScriptApi& api, JSValueConst thisValue, JSValueConst* argv,
                                         TransformOp op) {
    const auto* const ref =
        static_cast<const EntityRef*>(JS_GetOpaque(thisValue, api.componentClasses[api.transformComponent]));
    if (ref == nullptr) {
        return foreignTransform(ctx, api, op);
    }
    TransformInput input;
    if (!readTransformArguments(ctx, api, op, argv, input)) {
        return JS_EXCEPTION;
    }
    const std::span<const internal::ComponentBinding> components = api.table->components();
    std::string_view helper;
    for (const TransformHelper& entry : TRANSFORM_HELPERS) {
        if (entry.op == op) {
            helper = entry.name;
        }
    }
    const Site site{components[api.transformComponent].scriptName, helper, {}};
    void* const storage = resolveStorage(ctx, api, *ref, api.transformComponent, site);
    if (storage == nullptr) {
        return JS_EXCEPTION;
    }
    const JSValue result = applyTransformOp(ctx, api, op, *ref, *static_cast<Transform*>(storage), input);
    if (isReadOp(op)) {
        return result;  // a new detached vector, or JS_EXCEPTION with an exception pending
    }
    return JS_DupValue(ctx, thisValue);
}

}  // namespace

ComponentTypeId transformTypeId() noexcept { return componentTypeId<Transform>(); }

bool addIdentityTransform(World& world, Entity entity) { return world.add<Transform>(entity, Transform{}) != nullptr; }

JSValue transformHelper(JSContext* ctx, JSValueConst thisValue, int /*argc*/, JSValueConst* argv, int magic) noexcept {
    ScriptApi& api = scriptApiOf(ctx);
    assert(magic >= 0 && static_cast<std::size_t>(magic) < TRANSFORM_HELPERS.size());
    assert(api.transformComponent != NO_COMPONENT);
    return runTransformHelper(ctx, api, thisValue, argv, static_cast<TransformOp>(magic));
}

}  // namespace engine::script
